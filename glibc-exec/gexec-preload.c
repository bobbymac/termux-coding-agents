// gexec-preload.so: LD_PRELOAD shim for glibc programs started by gexec.
//
// Inside a glibc process on Termux (Google Play build, W^X policy) the kernel
// refuses execve() on anything stored in the app's data directory. This shim
// intercepts the glibc entry points that start programs and rewrites them:
//
//   bionic ELF in /data  ->  /system/bin/linker64 <program> <args...>
//   glibc ELF in /data   ->  /system/bin/linker64 <gexec> -a <argv0> <program> <args...>
//   #! script            ->  interpreter (with /bin, /usr/bin mapped into $PREFIX/bin), then the above
//   anything else        ->  unchanged
//
// Children always get bionic's LD_PRELOAD (libtermux-exec.so) back, so Termux
// programs launched from here can launch further programs normally.
// readlink("/proc/self/exe") reports $GEXEC_SELF instead of /system/bin/linker64.
// getifaddrs() uses RTM_GETADDR only, since Android 11+ denies apps RTM_GETLINK.
//
// execve() may run in a vfork() child, so the rewrite path never allocates.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PREFIX "/data/data/com.termux/files/usr"
#define LINKER "/system/bin/linker64"
#define GEXEC_DEFAULT PREFIX "/bin/gexec"
#define MAX_DEPTH 4

extern char **environ;

enum kind { K_PASS, K_BIONIC, K_GLIBC, K_SCRIPT };

struct plan {
	const char *path;
	char **argv;     // rewritten argv (caller-provided storage)
	char **envp;     // rewritten envp (caller-provided storage)
	char resolved[PATH_MAX];
	char interp[MAX_DEPTH][PATH_MAX];
	char iarg[MAX_DEPTH][256];
	char ldpre[PATH_MAX + 16];
};

static size_t count(char *const *v) {
	size_t n = 0;
	if (v) while (v[n]) n++;
	return n;
}

static int under_data(const char *p) {
	return strncmp(p, "/data/", 6) == 0;
}

// Classify an executable file and, for scripts, fill interp/iarg.
static enum kind classify(const char *path, char *interp, char *iarg) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return K_PASS;
	unsigned char buf[512];
	ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
	enum kind k = K_PASS;
	if (n >= 2 && buf[0] == '#' && buf[1] == '!') {
		buf[n] = 0;
		char *s = (char *)buf + 2;
		char *nl = strchr(s, '\n');
		if (nl) *nl = 0;
		while (*s == ' ' || *s == '\t') s++;
		char *e = s;
		while (*e && *e != ' ' && *e != '\t' && *e != '\r') e++;
		char saved = *e;
		*e = 0;
		const char *name = s;
		// termux-exec behaviour: /bin/x, /usr/bin/x, /usr/local/bin/x -> $PREFIX/bin/x
		const char *base = NULL;
		if (!strncmp(name, "/bin/", 5)) base = name + 5;
		else if (!strncmp(name, "/usr/bin/", 9)) base = name + 9;
		else if (!strncmp(name, "/usr/local/bin/", 15)) base = name + 15;
		if (base && access(name, F_OK) != 0) {
			strcpy(interp, PREFIX "/bin/");
			strncat(interp, base, PATH_MAX - sizeof(PREFIX "/bin/"));
		} else {
			strncpy(interp, name, PATH_MAX - 1);
			interp[PATH_MAX - 1] = 0;
		}
		iarg[0] = 0;
		if (saved) {
			char *a = e + 1;
			while (*a == ' ' || *a == '\t') a++;
			char *ae = a + strlen(a);
			while (ae > a && (ae[-1] == ' ' || ae[-1] == '\t' || ae[-1] == '\r')) *--ae = 0;
			strncpy(iarg, a, 255);
			iarg[255] = 0;
		}
		k = K_SCRIPT;
	} else if (n >= (ssize_t)sizeof(Elf64_Ehdr) && !memcmp(buf, ELFMAG, SELFMAG) &&
	           buf[EI_CLASS] == ELFCLASS64) {
		Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
		Elf64_Phdr ph[64];
		int num = eh->e_phnum < 64 ? eh->e_phnum : 64;
		k = K_BIONIC; // default for /data ELFs: let linker64 try
		if (pread(fd, ph, num * sizeof(Elf64_Phdr), eh->e_phoff) == (ssize_t)(num * sizeof(Elf64_Phdr))) {
			for (int i = 0; i < num; i++) {
				if (ph[i].p_type != PT_INTERP) continue;
				char ip[256] = {0};
				size_t len = ph[i].p_filesz < sizeof ip - 1 ? ph[i].p_filesz : sizeof ip - 1;
				if (pread(fd, ip, len, ph[i].p_offset) > 0 && strstr(ip, "ld-linux")) k = K_GLIBC;
			}
		}
	}
	close(fd);
	return k;
}

// Environment for any child: bionic LD_PRELOAD restored, glibc one dropped.
static void build_env(struct plan *pl, char *const envp[], char **out) {
	size_t j = 0;
	const char *bionic = NULL;
	for (size_t i = 0; envp && envp[i]; i++)
		if (!strncmp(envp[i], "GEXEC_BIONIC_LD_PRELOAD=", 24)) bionic = envp[i] + 24;
	for (size_t i = 0; envp && envp[i]; i++) {
		if (!strncmp(envp[i], "LD_PRELOAD=", 11)) continue;
		out[j++] = envp[i];
	}
	if (bionic && *bionic) {
		strcpy(pl->ldpre, "LD_PRELOAD=");
		strncat(pl->ldpre, bionic, PATH_MAX);
		out[j++] = pl->ldpre;
	}
	out[j] = NULL;
}

// Rewrite (path, argv) into pl. argv storage must hold count(argv) + 4*MAX_DEPTH + 8.
static void make_plan(struct plan *pl, const char *path, char *const argv[], char *const envp[],
                      char **argv_store, char **env_store) {
	size_t argc = count(argv);
	pl->argv = argv_store;
	pl->envp = env_store;
	build_env(pl, envp, env_store);

	// Working argv starts as the caller's.
	for (size_t i = 0; i <= argc; i++) argv_store[i] = argc ? argv[i] : NULL;
	if (!argc) { argv_store[0] = (char *)path; argv_store[1] = NULL; argc = 1; }
	const char *cur = path;

	for (int depth = 0; depth < MAX_DEPTH; depth++) {
		if (!realpath(cur, pl->resolved)) break;
		if (!under_data(pl->resolved)) break;
		enum kind k = classify(pl->resolved, pl->interp[depth], pl->iarg[depth]);
		if (k == K_SCRIPT) {
			// interp [iarg] script args...  (script replaces argv[0])
			int extra = pl->iarg[depth][0] ? 2 : 1;
			memmove(argv_store + extra + 1, argv_store + 1, argc * sizeof(char *));
			argv_store[0] = pl->interp[depth];
			if (extra == 2) argv_store[1] = pl->iarg[depth];
			argv_store[extra] = (char *)cur;
			argc += extra;
			cur = pl->interp[depth];
			continue;
		}
		if (k == K_BIONIC) {
			// linker64 program args...   (the program sees argv[0] = its path)
			memmove(argv_store + 1, argv_store, (argc + 1) * sizeof(char *));
			argv_store[0] = LINKER;
			argv_store[1] = (char *)cur;
			pl->path = LINKER;
			return;
		}
		if (k == K_GLIBC) {
			// linker64 gexec -a argv0 program args[1..]
			const char *gx = getenv("GEXEC_PATH");
			if (!gx || !*gx) gx = GEXEC_DEFAULT;
			char *a0 = argv_store[0];
			memmove(argv_store + 5, argv_store + 1, argc * sizeof(char *));
			argv_store[0] = LINKER;
			argv_store[1] = (char *)gx;
			argv_store[2] = "-a";
			argv_store[3] = a0;
			argv_store[4] = (char *)cur;
			pl->path = LINKER;
			return;
		}
		break;
	}
	pl->path = cur;
}

#define PLAN_STORAGE(argv, envp)                                    \
	size_t _argc = count(argv), _envc = count(envp);                 \
	char *_av[_argc + 4 * MAX_DEPTH + 8];                            \
	char *_ev[_envc + 2];                                            \
	struct plan _pl;

static int (*real_execve)(const char *, char *const[], char *const[]);
static int (*real_posix_spawn)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                               const posix_spawnattr_t *, char *const[], char *const[]);
static int (*real_pidfd_spawn)(int *, const char *, const posix_spawn_file_actions_t *,
                               const posix_spawnattr_t *, char *const[], char *const[]);
static ssize_t (*real_readlink)(const char *, char *, size_t);
static ssize_t (*real_readlinkat)(int, const char *, char *, size_t);

__attribute__((constructor)) static void init(void) {
	real_execve = dlsym(RTLD_NEXT, "execve");
	real_posix_spawn = dlsym(RTLD_NEXT, "posix_spawn");
	real_pidfd_spawn = dlsym(RTLD_NEXT, "pidfd_spawn");
	real_readlink = dlsym(RTLD_NEXT, "readlink");
	real_readlinkat = dlsym(RTLD_NEXT, "readlinkat");
}

int execve(const char *path, char *const argv[], char *const envp[]) {
	PLAN_STORAGE(argv, envp);
	make_plan(&_pl, path, argv, envp, _av, _ev);
	return real_execve(_pl.path, _pl.argv, _pl.envp);
}

// Find FILE on PATH like execvp does; returns 0 and fills out on success.
static int search_path(const char *file, char *out) {
	if (strchr(file, '/')) {
		strncpy(out, file, PATH_MAX - 1);
		out[PATH_MAX - 1] = 0;
		return 0;
	}
	const char *p = getenv("PATH");
	if (!p) p = PREFIX "/bin:/system/bin";
	while (*p) {
		const char *c = strchr(p, ':');
		size_t len = c ? (size_t)(c - p) : strlen(p);
		if (len && len + strlen(file) + 2 < PATH_MAX) {
			memcpy(out, p, len);
			out[len] = '/';
			strcpy(out + len + 1, file);
			struct stat st;
			if (stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0) return 0;
		}
		if (!c) break;
		p = c + 1;
	}
	return ENOENT;
}

int execvp(const char *file, char *const argv[]) {
	char full[PATH_MAX];
	int err = search_path(file, full);
	if (err) { errno = err; return -1; }
	return execve(full, argv, environ);
}

int execvpe(const char *file, char *const argv[], char *const envp[]) {
	char full[PATH_MAX];
	int err = search_path(file, full);
	if (err) { errno = err; return -1; }
	return execve(full, argv, envp);
}

int execv(const char *path, char *const argv[]) { return execve(path, argv, environ); }

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
	char *const *ev = envp ? envp : environ;
	PLAN_STORAGE(argv, ev);
	make_plan(&_pl, path, argv, ev, _av, _ev);
	return real_posix_spawn(pid, _pl.path, fa, attr, _pl.argv, _pl.envp);
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
	char full[PATH_MAX];
	int err = search_path(file, full);
	if (err) return err;
	return posix_spawn(pid, full, fa, attr, argv, envp);
}

int pidfd_spawn(int *pidfd, const char *path, const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
	if (!real_pidfd_spawn) return ENOSYS;
	char *const *ev = envp ? envp : environ;
	PLAN_STORAGE(argv, ev);
	make_plan(&_pl, path, argv, ev, _av, _ev);
	return real_pidfd_spawn(pidfd, _pl.path, fa, attr, _pl.argv, _pl.envp);
}

int pidfd_spawnp(int *pidfd, const char *file, const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
	char full[PATH_MAX];
	int err = search_path(file, full);
	if (err) return err;
	return pidfd_spawn(pidfd, full, fa, attr, argv, envp);
}

static int is_self_exe(const char *p) {
	if (!strcmp(p, "/proc/self/exe")) return 1;
	char mine[32];
	int n = 0;
	pid_t pid = getpid();
	char digits[16];
	do { digits[n++] = '0' + pid % 10; pid /= 10; } while (pid);
	strcpy(mine, "/proc/");
	int k = 6;
	while (n) mine[k++] = digits[--n];
	strcpy(mine + k, "/exe");
	return !strcmp(p, mine);
}

static ssize_t self_link(char *buf, size_t sz) {
	const char *self = getenv("GEXEC_SELF");
	if (!self || !*self) return -2;
	size_t len = strlen(self);
	if (len > sz) len = sz;
	memcpy(buf, self, len);
	return (ssize_t)len;
}

ssize_t readlink(const char *path, char *buf, size_t sz) {
	if (path && is_self_exe(path)) {
		ssize_t r = self_link(buf, sz);
		if (r != -2) return r;
	}
	return real_readlink(path, buf, sz);
}

// ---------------------------------------------------------------------------
// getifaddrs(): Android (11+) denies apps the RTM_GETLINK netlink dump that
// glibc's getifaddrs() starts with, so it fails with EACCES. Bionic only uses
// RTM_GETADDR, which is still allowed; do the same. Each entry is one malloc
// block, released by our freeifaddrs().
#include <ifaddrs.h>
#include <stdio.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

struct ifa_node {
	struct ifaddrs ifa;
	struct sockaddr_storage addr, mask, brd;
	char name[IF_NAMESIZE];
	unsigned index;
	int named; // name came from the kernel, not a placeholder
};

static void prefix_to_mask(struct sockaddr_storage *ss, int family, unsigned plen) {
	memset(ss, 0, sizeof *ss);
	ss->ss_family = family;
	unsigned char *b = family == AF_INET ? (unsigned char *)&((struct sockaddr_in *)ss)->sin_addr
	                                     : (unsigned char *)&((struct sockaddr_in6 *)ss)->sin6_addr;
	unsigned max = family == AF_INET ? 32 : 128;
	if (plen > max) plen = max;
	for (unsigned i = 0; i < plen; i++) b[i / 8] |= (unsigned char)(0x80 >> (i % 8));
}

static void set_addr(struct sockaddr_storage *ss, int family, const void *data, unsigned scope) {
	memset(ss, 0, sizeof *ss);
	ss->ss_family = family;
	if (family == AF_INET) {
		memcpy(&((struct sockaddr_in *)ss)->sin_addr, data, 4);
	} else {
		struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)ss;
		memcpy(&s6->sin6_addr, data, 16);
		if (IN6_IS_ADDR_LINKLOCAL(&s6->sin6_addr)) s6->sin6_scope_id = scope;
	}
}

static unsigned iface_flags(const char *name) {
	unsigned flags = 0;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd >= 0) {
		struct ifreq r;
		memset(&r, 0, sizeof r);
		strncpy(r.ifr_name, name, IF_NAMESIZE - 1);
		if (ioctl(fd, SIOCGIFFLAGS, &r) == 0) flags = (unsigned short)r.ifr_flags;
		close(fd);
	}
	if (!flags) { // ioctl refused: assume an address means the interface is up
		flags = IFF_UP | IFF_RUNNING;
		if (!strcmp(name, "lo")) flags |= IFF_LOOPBACK;
	}
	return flags;
}

void freeifaddrs(struct ifaddrs *ifa) {
	while (ifa) {
		struct ifaddrs *next = ifa->ifa_next;
		free(ifa); // ifa is the first member of its ifa_node
		ifa = next;
	}
}

int getifaddrs(struct ifaddrs **out) {
	*out = NULL;
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0) return -1;
	struct {
		struct nlmsghdr nh;
		struct ifaddrmsg ifm;
	} req;
	memset(&req, 0, sizeof req);
	req.nh.nlmsg_len = sizeof req;
	req.nh.nlmsg_type = RTM_GETADDR;
	req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.nh.nlmsg_seq = 1;
	req.ifm.ifa_family = AF_UNSPEC;
	struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
	if (sendto(fd, &req, sizeof req, 0, (struct sockaddr *)&kernel, sizeof kernel) < 0) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}

	struct ifaddrs *head = NULL, **tail = &head;
	char buf[32768];
	int done = 0, err = 0;
	while (!done && !err) {
		ssize_t n = recv(fd, buf, sizeof buf, 0);
		if (n < 0) {
			if (errno == EINTR) continue;
			err = errno;
			break;
		}
		for (struct nlmsghdr *nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, (size_t)n); nh = NLMSG_NEXT(nh, n)) {
			if (nh->nlmsg_type == NLMSG_DONE) { done = 1; break; }
			if (nh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = NLMSG_DATA(nh);
				err = e->error ? -e->error : EIO;
				break;
			}
			if (nh->nlmsg_type != RTM_NEWADDR) continue;
			struct ifaddrmsg *ifm = NLMSG_DATA(nh);
			if (ifm->ifa_family != AF_INET && ifm->ifa_family != AF_INET6) continue;
			const void *local = NULL, *address = NULL, *bcast = NULL;
			const char *label = NULL;
			int len = IFA_PAYLOAD(nh);
			for (struct rtattr *rta = IFA_RTA(ifm); RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
				switch (rta->rta_type) {
				case IFA_LOCAL: local = RTA_DATA(rta); break;
				case IFA_ADDRESS: address = RTA_DATA(rta); break;
				case IFA_BROADCAST: bcast = RTA_DATA(rta); break;
				case IFA_LABEL: label = RTA_DATA(rta); break;
				}
			}
			const void *a = local ? local : address;
			if (!a) continue;
			struct ifa_node *node = calloc(1, sizeof *node);
			if (!node) { err = ENOMEM; break; }
			node->index = ifm->ifa_index;
			node->named = 1;
			if (label) strncpy(node->name, label, IF_NAMESIZE - 1);
			else if (!if_indextoname(ifm->ifa_index, node->name)) {
				// if_indextoname is refused too; fixed up from IPv4 labels below.
				snprintf(node->name, IF_NAMESIZE, "if%u", ifm->ifa_index);
				node->named = 0;
			}
			set_addr(&node->addr, ifm->ifa_family, a, ifm->ifa_index);
			prefix_to_mask(&node->mask, ifm->ifa_family, ifm->ifa_prefixlen);
			node->ifa.ifa_name = node->name;
			node->ifa.ifa_flags = iface_flags(node->name);
			node->ifa.ifa_addr = (struct sockaddr *)&node->addr;
			node->ifa.ifa_netmask = (struct sockaddr *)&node->mask;
			if (ifm->ifa_family == AF_INET && bcast && (node->ifa.ifa_flags & IFF_BROADCAST)) {
				set_addr(&node->brd, AF_INET, bcast, 0);
				node->ifa.ifa_broadaddr = (struct sockaddr *)&node->brd;
			} else if (local && address && memcmp(local, address, ifm->ifa_family == AF_INET ? 4 : 16)) {
				set_addr(&node->brd, ifm->ifa_family, address, ifm->ifa_index); // point-to-point peer
				node->ifa.ifa_dstaddr = (struct sockaddr *)&node->brd;
			}
			*tail = &node->ifa;
			tail = &node->ifa.ifa_next;
		}
	}
	close(fd);
	if (err) {
		freeifaddrs(head);
		errno = err;
		return -1;
	}
	// IPv6 addresses carry no label: borrow the name of an IPv4 address on the
	// same interface index, then look up the real flags under that name.
	for (struct ifaddrs *p = head; p; p = p->ifa_next) {
		struct ifa_node *n = (struct ifa_node *)p;
		if (n->named) continue;
		for (struct ifaddrs *q = head; q; q = q->ifa_next) {
			struct ifa_node *m = (struct ifa_node *)q;
			if (m->named && m->index == n->index) {
				memcpy(n->name, m->name, IF_NAMESIZE);
				n->named = 1;
				n->ifa.ifa_flags = m->ifa.ifa_flags;
				break;
			}
		}
	}
	*out = head;
	return 0;
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t sz) {
	if (path && path[0] == '/' && is_self_exe(path)) {
		ssize_t r = self_link(buf, sz);
		if (r != -2) return r;
	}
	return real_readlinkat(dirfd, path, buf, sz);
}
