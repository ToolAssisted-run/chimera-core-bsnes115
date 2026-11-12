/*
  libco for this core, in place of the one in bsnes's tree (extern/bsnes/libco).

  bsnes gives each chip of the console - the CPU, the sound processor, the
  picture processor, every coprocessor - a stack of its own and switches
  between them with libco. Upstream's libco takes those stacks from malloc,
  and its aarch64 switch does not keep d8-d15, which the AArch64 calling
  convention says a callee must keep. This one:

  - maps each stack with MAP_STACK and a guard below it, so the sandbox
    knows the memory is a stack (a fault on the page the stack pointer is in
    is handled differently from one on ordinary memory);
  - keeps what the System V and AAPCS64 conventions say a callee must keep
    (rsp, rbp, rbx, r12-r15; sp, x19-x30, d8-d15) and never touches x18,
    which the guest toolchain reserves;
  - answers co_derive on a cothread co_create made, which is how bsnes
    starts a chip again at power-on: the same stack, started over.

  co_serializable() is 0: bsnes then never copies a stack into its own
  savestates. Chimera's savestates are the sandbox's whole memory, stacks
  included, and do not go through bsnes's serializer at all.

  The aarch64 switch is the one in ares's libco/aarch64.c (ISC licence,
  Copyright (c) 2004-2025 ares team, Near et al), written as assembly; the
  amd64 one keeps the same registers upstream's amd64.c does.
*/

#include <libco/libco.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* each stack is 16 KiB larger than asked for; the bottom of it is the guard */
#define GUARD_SIZE 0x4000

typedef struct {
	/* read and written by the switch; has to be first */
	uint64_t regs[22];
	/* the lowest address of the mapping (the guard, not usable stack) */
	void *stack;
	uint64_t stack_size;
} cothread_impl;

#if defined(__x86_64__)

/* regs: rsp 0, rbp 8, rbx 16, r12-r15 24-48, the address to resume at 56 */
__asm__(
	".text\n"
	".globl libco_swap\n"
	".type libco_swap,@function\n"
	"libco_swap:\n"
	"	mov %rsp, 0(%rsi)\n"
	"	mov %rbp, 8(%rsi)\n"
	"	mov %rbx, 16(%rsi)\n"
	"	mov %r12, 24(%rsi)\n"
	"	mov %r13, 32(%rsi)\n"
	"	mov %r14, 40(%rsi)\n"
	"	mov %r15, 48(%rsi)\n"
	"	mov 0(%rdi), %rsp\n"
	"	mov 8(%rdi), %rbp\n"
	"	mov 16(%rdi), %rbx\n"
	"	mov 24(%rdi), %r12\n"
	"	mov 32(%rdi), %r13\n"
	"	mov 40(%rdi), %r14\n"
	"	mov 48(%rdi), %r15\n"
	"	ret\n"
	".size libco_swap, .-libco_swap\n");

#elif defined(__aarch64__)

/* regs: sp, x30, x19-x28, x29, d8-d15 (offsets 0, 8, 16-88, 96, 112-168) */
__asm__(
	".text\n"
	".globl libco_swap\n"
	".type libco_swap,%function\n"
	"libco_swap:\n"
	"	mov x16, sp\n"
	"	stp x16, x30, [x1]\n"
	"	ldp x16, x30, [x0]\n"
	"	mov sp, x16\n"
	"	stp x19, x20, [x1, 16]\n"
	"	ldp x19, x20, [x0, 16]\n"
	"	stp x21, x22, [x1, 32]\n"
	"	ldp x21, x22, [x0, 32]\n"
	"	stp x23, x24, [x1, 48]\n"
	"	ldp x23, x24, [x0, 48]\n"
	"	stp x25, x26, [x1, 64]\n"
	"	ldp x25, x26, [x0, 64]\n"
	"	stp x27, x28, [x1, 80]\n"
	"	ldp x27, x28, [x0, 80]\n"
	"	str x29, [x1, 96]\n"
	"	ldr x29, [x0, 96]\n"
	"	stp d8, d9, [x1, 112]\n"
	"	ldp d8, d9, [x0, 112]\n"
	"	stp d10, d11, [x1, 128]\n"
	"	ldp d10, d11, [x0, 128]\n"
	"	stp d12, d13, [x1, 144]\n"
	"	ldp d12, d13, [x0, 144]\n"
	"	stp d14, d15, [x1, 160]\n"
	"	ldp d14, d15, [x0, 160]\n"
	"	br x30\n"
	".size libco_swap, .-libco_swap\n");

#else
#error "this libco knows x86_64 and aarch64"
#endif

void libco_swap(cothread_impl *to, cothread_impl *from);

/* the cothread of the thread that called in, and the one running now */
static cothread_impl co_host;
static cothread_impl *co_running;

/* where a cothread starts: its entry point is kept in a register the switch
   restores (rbx on x86_64, x19 on aarch64) */
static void co_entry(void)
{
	void (*entrypoint)(void) = (void (*)(void))co_running->regs[2];
	entrypoint();
	abort(); /* an entry point never returns */
}

static void co_start_over(cothread_impl *co, void (*entrypoint)(void))
{
	uint64_t top = ((uint64_t)(uintptr_t)co->stack + co->stack_size) & ~(uint64_t)15;
	memset(co->regs, 0, sizeof co->regs);
#if defined(__x86_64__)
	/* the switch's ret pops co_entry; after it the stack is as a call left it */
	uint64_t *sp = (uint64_t *)(uintptr_t)top;
	*--sp = 0;
	*--sp = (uint64_t)(uintptr_t)co_entry;
	co->regs[0] = (uint64_t)(uintptr_t)sp;
#else
	co->regs[0] = top;
	co->regs[1] = (uint64_t)(uintptr_t)co_entry;
	co->regs[12] = top; /* x29 */
#endif
	co->regs[2] = (uint64_t)(uintptr_t)entrypoint;
}

cothread_t co_active(void)
{
	if (!co_running)
		co_running = &co_host;
	return co_running;
}

cothread_t co_create(unsigned int size, void (*entrypoint)(void))
{
	co_active();
	cothread_impl *co = calloc(1, sizeof *co);
	if (!co)
		return 0;
	uint64_t total = (((uint64_t)size + 4095) & ~(uint64_t)4095) + GUARD_SIZE;
	void *stack = mmap(NULL, total, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stack == MAP_FAILED)
	{
		free(co);
		return 0;
	}
	if (mprotect(stack, GUARD_SIZE, PROT_NONE) != 0)
	{
		munmap(stack, total);
		free(co);
		return 0;
	}
	co->stack = stack;
	co->stack_size = total;
	co_start_over(co, entrypoint);
	return co;
}

cothread_t co_derive(void *handle, unsigned int size, void (*entrypoint)(void))
{
	(void)size;
	cothread_impl *co = handle;
	if (!co || !co->stack)
		return 0;
	co_start_over(co, entrypoint);
	return co;
}

void co_delete(cothread_t handle)
{
	cothread_impl *co = handle;
	if (!co)
		return;
	munmap(co->stack, co->stack_size);
	free(co);
}

void co_switch(cothread_t handle)
{
	cothread_impl *from = co_active();
	co_running = handle;
	libco_swap(co_running, from);
}

int co_serializable(void)
{
	return 0;
}
