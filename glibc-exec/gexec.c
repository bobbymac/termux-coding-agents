// gexec: run a glibc program on Android when execve() of app-data files is
// forbidden (Termux from Google Play, targetSdk >= 29, "W^X" SELinux policy).
//
// Android lets an app mmap(PROT_EXEC) files in its data directory (that is how
// Termux loads its own libraries) but refuses execve() on them. Termux works
// around that by launching bionic programs through /system/bin/linker64, but
// glibc's dynamic loader cannot be started that way. gexec does the kernel's
// job in userspace instead: it maps glibc's ld.so, builds a fresh process stack
// (argv, envp, auxv) and jumps to ld.so's entry point, asking it to load the
// real program:  ld.so --argv0 <argv0> <program> <args...>
//
// Usage: gexec [-a ARGV0] PROGRAM [ARGS...]
//   GEXEC_LDSO   path to glibc's ld-linux-aarch64.so.1 (default: Termux glibc)
//   GEXEC_GLIBC_LD_PRELOAD  LD_PRELOAD for the glibc program (e.g. gexec-preload.so)
// Exports GEXEC_SELF (the program's real path) and GEXEC_BIONIC_LD_PRELOAD.
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef __aarch64__
#error "gexec currently supports aarch64 only"
#endif

#define DEFAULT_LDSO "/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1"
#define STACK_SIZE (16UL << 20)

extern char **environ;

static void die(const char *what) {
	fprintf(stderr, "gexec: %s: %s\n", what, strerror(errno));
	_exit(127);
}

static void fail(const char *msg) {
	fprintf(stderr, "gexec: %s\n", msg);
	_exit(127);
}

#define PAGE_DOWN(x, p) ((x) & ~((uintptr_t)(p) - 1))
#define PAGE_UP(x, p) (((x) + (p) - 1) & ~((uintptr_t)(p) - 1))

struct loaded {
	uintptr_t base;  // load bias
	uintptr_t entry; // absolute entry point
	uintptr_t phdr;  // absolute address of program headers
	int phnum;
};

static int prot_of(uint32_t flags) {
	return ((flags & PF_R) ? PROT_READ : 0) | ((flags & PF_W) ? PROT_WRITE : 0) |
	       ((flags & PF_X) ? PROT_EXEC : 0);
}

// Map an ET_DYN ELF (glibc's ld.so) at a kernel-chosen base.
static struct loaded load_elf(const char *path) {
	struct loaded out = {0};
	long pg = sysconf(_SC_PAGESIZE);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) die(path);

	Elf64_Ehdr eh;
	if (pread(fd, &eh, sizeof eh, 0) != sizeof eh) die("read ELF header");
	if (memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh.e_machine != EM_AARCH64 || eh.e_type != ET_DYN)
		fail("loader is not an aarch64 ET_DYN ELF");

	size_t phsz = (size_t)eh.e_phnum * sizeof(Elf64_Phdr);
	Elf64_Phdr *ph = malloc(phsz);
	if (!ph || pread(fd, ph, phsz, eh.e_phoff) != (ssize_t)phsz) die("read program headers");

	uintptr_t lo = UINTPTR_MAX, hi = 0;
	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD) continue;
		if (PAGE_DOWN(ph[i].p_vaddr, pg) < lo) lo = PAGE_DOWN(ph[i].p_vaddr, pg);
		if (PAGE_UP(ph[i].p_vaddr + ph[i].p_memsz, pg) > hi) hi = PAGE_UP(ph[i].p_vaddr + ph[i].p_memsz, pg);
	}
	if (lo == UINTPTR_MAX) fail("loader has no PT_LOAD segments");

	// Reserve the whole span, then map each segment over it.
	uint8_t *span = mmap(NULL, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (span == MAP_FAILED) die("reserve address space");
	uintptr_t bias = (uintptr_t)span - lo;

	for (int i = 0; i < eh.e_phnum; i++) {
		Elf64_Phdr *p = &ph[i];
		if (p->p_type != PT_LOAD) continue;
		uintptr_t seg = bias + PAGE_DOWN(p->p_vaddr, pg);
		uintptr_t off = PAGE_DOWN(p->p_offset, pg);
		uintptr_t file_end = bias + p->p_vaddr + p->p_filesz;
		uintptr_t mem_end = bias + p->p_vaddr + p->p_memsz;
		int prot = prot_of(p->p_flags);
		size_t flen = file_end - seg;
		if (flen && mmap((void *)seg, flen, prot | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, off) == MAP_FAILED)
			die("map loader segment");
		// Zero the tail of the last file page and map anonymous pages for .bss.
		uintptr_t file_page_end = PAGE_UP(file_end, pg);
		if (p->p_memsz > p->p_filesz) {
			memset((void *)file_end, 0, file_page_end - file_end);
			if (mem_end > file_page_end &&
			    mmap((void *)file_page_end, mem_end - file_page_end, prot,
			         MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
				die("map loader bss");
		}
		if (mprotect((void *)seg, file_page_end - seg, prot)) die("protect loader segment");
	}

	out.base = bias;
	out.entry = bias + eh.e_entry;
	out.phnum = eh.e_phnum;
	out.phdr = 0;
	for (int i = 0; i < eh.e_phnum; i++)
		if (ph[i].p_type == PT_PHDR) out.phdr = bias + ph[i].p_vaddr;
	if (!out.phdr) out.phdr = (uintptr_t)span + eh.e_phoff; // first PT_LOAD covers the headers
	free(ph);
	close(fd);
	return out;
}

static void reset_process_state(void) {
	// Bionic (linker64/debuggerd) installed signal handlers and an alternate
	// stack that point into code glibc knows nothing about.
	stack_t ss = {.ss_flags = SS_DISABLE};
	sigaltstack(&ss, NULL);
	struct sigaction dfl = {.sa_handler = SIG_DFL};
	for (int s = 1; s < 65; s++) {
		if (s == SIGKILL || s == SIGSTOP) continue;
		struct sigaction cur;
		if (sigaction(s, NULL, &cur) == 0 && cur.sa_handler != SIG_IGN) sigaction(s, &dfl, NULL);
	}
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
}

int main(int argc, char **argv) {
	int ai = 1;
	const char *argv0 = NULL;
	if (ai + 1 < argc && strcmp(argv[ai], "-a") == 0) {
		argv0 = argv[ai + 1];
		ai += 2;
	}
	if (ai >= argc) {
		fprintf(stderr, "usage: gexec [-a ARGV0] PROGRAM [ARGS...]\n");
		return 2;
	}
	const char *prog = argv[ai];
	if (!argv0) argv0 = prog;
	const char *ldso = getenv("GEXEC_LDSO");
	if (!ldso || !*ldso) ldso = DEFAULT_LDSO;

	// Glibc programs must not inherit bionic's LD_PRELOAD (libtermux-exec.so);
	// keep it around so a glibc-side shim can restore it for bionic children.
	const char *pre = getenv("LD_PRELOAD");
	if (pre && *pre && strstr(pre, "libtermux-exec")) {
		setenv("GEXEC_BIONIC_LD_PRELOAD", pre, 1);
		unsetenv("LD_PRELOAD");
	}
	const char *glibc_pre = getenv("GEXEC_GLIBC_LD_PRELOAD");
	if (glibc_pre && *glibc_pre) setenv("LD_PRELOAD", glibc_pre, 1);
	char self[4096];
	setenv("GEXEC_SELF", realpath(prog, self) ? self : prog, 1);

	struct loaded ld = load_elf(ldso);

	// New argv: ld.so --argv0 ARGV0 PROGRAM ARGS...
	int nargs = argc - ai;
	int nnew = 3 + nargs;
	char **nargv = calloc(nnew + 1, sizeof(char *));
	nargv[0] = (char *)ldso;
	nargv[1] = "--argv0";
	nargv[2] = (char *)argv0;
	for (int i = 0; i < nargs; i++) nargv[3 + i] = argv[ai + i];

	int nenv = 0;
	while (environ[nenv]) nenv++;

	// Build the initial process stack the way the kernel does.
	uint8_t *stk = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
	                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stk == MAP_FAILED) die("allocate stack");
	uintptr_t sp = (uintptr_t)stk + STACK_SIZE;

#define PUSH_STR(s) ({ size_t _l = strlen(s) + 1; sp -= _l; memcpy((void *)sp, (s), _l); (char *)sp; })
	char *platform = PUSH_STR("aarch64");
	char *execfn = PUSH_STR(prog);
	char **sargv = calloc(nnew + 1, sizeof(char *));
	for (int i = nnew - 1; i >= 0; i--) sargv[i] = PUSH_STR(nargv[i]);
	char **senv = calloc(nenv + 1, sizeof(char *));
	for (int i = nenv - 1; i >= 0; i--) senv[i] = PUSH_STR(environ[i]);
	sp -= 16;
	uint8_t *rnd = (uint8_t *)sp;
	if (getrandom(rnd, 16, 0) != 16) memset(rnd, 0x5a, 16);
	sp &= ~(uintptr_t)15;

	Elf64_auxv_t aux[] = {
		{AT_PHDR, {ld.phdr}},
		{AT_PHENT, {sizeof(Elf64_Phdr)}},
		{AT_PHNUM, {(uint64_t)ld.phnum}},
		{AT_PAGESZ, {(uint64_t)sysconf(_SC_PAGESIZE)}},
		{AT_BASE, {0}},
		{AT_FLAGS, {0}},
		{AT_ENTRY, {ld.entry}},
		{AT_UID, {getuid()}},
		{AT_EUID, {geteuid()}},
		{AT_GID, {getgid()}},
		{AT_EGID, {getegid()}},
		{AT_SECURE, {0}},
		{AT_RANDOM, {(uintptr_t)rnd}},
		{AT_HWCAP, {getauxval(AT_HWCAP)}},
		{AT_HWCAP2, {getauxval(AT_HWCAP2)}},
		{AT_CLKTCK, {(uint64_t)sysconf(_SC_CLK_TCK)}},
		{AT_PLATFORM, {(uintptr_t)platform}},
		{AT_EXECFN, {(uintptr_t)execfn}},
		{AT_SYSINFO_EHDR, {getauxval(AT_SYSINFO_EHDR)}},
#ifdef AT_MINSIGSTKSZ
		{AT_MINSIGSTKSZ, {getauxval(AT_MINSIGSTKSZ)}},
#endif
		{AT_NULL, {0}},
	};
	size_t words = 1 + (nnew + 1) + (nenv + 1) + 2 * (sizeof aux / sizeof aux[0]);
	sp -= words * sizeof(uint64_t);
	sp &= ~(uintptr_t)15;
	uint64_t *w = (uint64_t *)sp;
	*w++ = (uint64_t)nnew;
	for (int i = 0; i < nnew; i++) *w++ = (uintptr_t)sargv[i];
	*w++ = 0;
	for (int i = 0; i < nenv; i++) *w++ = (uintptr_t)senv[i];
	*w++ = 0;
	memcpy(w, aux, sizeof aux);

	reset_process_state();

	__asm__ volatile(
		"mov sp, %0\n"
		"mov x0, #0\n"
		"mov x29, #0\n"
		"mov x30, #0\n"
		"br %1\n"
		:
		: "r"(sp), "r"(ld.entry)
		: "x0", "memory");
	__builtin_unreachable();
}
