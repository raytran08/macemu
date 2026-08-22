/*
 * bfast - in-kernel trap fast path for Basilisk II on NetBSD/mac68k.
 *
 * Phase 2: the SR-bookkeeping family is emulated at trap level; A-line
 * still counts and chains (that is P3).
 *
 * Structure: the vector stubs (asm) do only the cheap gate -- trap from
 * user mode, current pcb is the registered pcb -- and then call a C
 * handler with the saved register block.  The C handler either emulates
 * the instruction and returns nonzero, in which case the stub restores
 * registers and executes RTE directly (no signal, no chain), or returns
 * zero WITHOUT HAVING MUTATED ANYTHING, in which case the stub chains to
 * the stock handler and the signal path repeats the instruction from
 * scratch.  Decide-before-mutate is what makes chaining always correct.
 *
 * The semantics are a line-for-line relocation of the privileged-
 * instruction switch in BasiliskII's main_unix.cpp sigill_handler; the
 * virtual SR lives where it always lived, in userland's EmulatedSR,
 * accessed here with ufetch/ustore.  Guest memory access likewise: the
 * u-access functions carry the fault handling that raw dereferences
 * would not.
 *
 * See FASTPATH.md for the design; P0/P1 results at the bottom of it.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/module.h>
#include <sys/kmem.h>
#include <sys/sysctl.h>
#include <sys/lwp.h>
#include <sys/proc.h>

MODULE(MODULE_CLASS_MISC, bfast, NULL);

#define BF_NVEC		256
#define BF_TABBYTES	(BF_NVEC * sizeof(uint32_t))
#define BF_TABALIGN	1024

#define BF_VEC_PRIV	8
#define BF_VEC_ALINE	10
#define BF_VEC_TRACE	9

static void	*bf_alloc;
static size_t	bf_alloclen;
static uint32_t	*bf_table;
static uint32_t	bf_orig_vbr;
static int	bf_swapped;

static struct sysctllog *bf_clog;

/*
 * Shared with the stubs (asm) -- non-static, bf_ prefixed.
 */
struct pcb	*bf_pcb;		/* gate: registered lwp's pcb */
static pid_t	bf_pid;			/* who registered; re-checked in C */
uint32_t	bf_orig_priv;		/* stock vector 8 handler */
uint32_t	bf_orig_aline;		/* stock vector 10 handler */
uint32_t	bf_orig_trace;		/* stock vector 9 handler */
uint32_t	bf_n_fast;		/* emulated at trap level */
uint32_t	bf_n_defer;		/* ours, declined -> signal path */
uint32_t	bf_n_aline;		/* ours, counted, chained (P3 pending) */
uint32_t	bf_n_chain_priv;	/* not ours */
uint32_t	bf_n_chain_aline;	/* not ours */

/*
 * Single-step tracing, for the 0x9fc0000 hunt.  When an A-line trap is
 * reflected from bf_trace_arm_pc, T1 is set in the frame SR our stub
 * RTEs with -- entirely inside the kernel, so setcontext() never sees a
 * PS it would reject.  Every subsequent instruction raises vector 9,
 * logged below, until the guest returns past the armed trap or the
 * window overflows.  The fatal wild jump faults on the FETCH, before
 * any trace exception for it, so the ring's last entry is the
 * instruction that computed the bad PC.
 */
#define BF_TRN		32768		/* entries; 640KB */
struct bf_tr_ent { uint32_t pc, a2, fp, usp; };
static struct bf_tr_ent bf_tr[BF_TRN];
static uint32_t	bf_tr_n;		/* total logged since load */
static uint32_t	bf_tr_window;		/* logged in the current window */
static uint32_t	bf_trace_arm_pc;	/* sysctl: arm on this A-line pc */
static uint32_t	bf_tr_armed_ret;	/* pc that closes the window */
static uint32_t	bf_arm_now;		/* sysctl: arm at the next priv trap */
static uint32_t	bf_watch_addr;		/* derived: what the fatal rts pops */
static int	bf_tracing;		/* sticky: keep T1 across SR writes */
#define BF_MARK_ARM	0xfeedfaceu	/* ring marker: tracing armed here */

/* Registered user addresses of Basilisk's virtual-SR state. */
static uint32_t	bf_uaddr_emulsr;	/* uint16 EmulatedSR */
static uint32_t	bf_uaddr_intflags;	/* uint32 InterruptFlags */

void bf_stub_priv(void);
void bf_stub_aline(void);
int bf_ctrap_priv(uint32_t *r);
int bf_ctrap_aline(uint32_t *r);
int bf_ctrap_trace(uint32_t *r);

/*
 * Layout handed to bf_ctrap_priv: the stub pushes a pad longword, then
 * moveml #0xffff -- so ascending memory holds d0-d7, a0-a6, the useless
 * a7 slot, the pad, and then the hardware exception frame.
 */
#define BF_R_D(r, n)	((r)[(n)])
#define BF_R_A(r, n)	((r)[8 + (n)])		/* a0..a6 only */
struct bf_hwframe {
	uint16_t	sr;
	uint16_t	pc_hi;		/* split to dodge misalignment */
	uint16_t	pc_lo;
	uint16_t	fmtvec;
} __packed;
#define BF_FRAME(r)	((struct bf_hwframe *)((char *)(r) + 17 * 4))

static inline uint32_t
bf_frame_pc(const struct bf_hwframe *f)
{
	return ((uint32_t)f->pc_hi << 16) | f->pc_lo;
}

static inline void
bf_frame_set_pc(struct bf_hwframe *f, uint32_t pc)
{
	f->pc_hi = pc >> 16;
	f->pc_lo = pc & 0xffff;
}

static inline uint32_t
bf_usp_read(void)
{
	uint32_t v;

	__asm volatile("movl %%usp,%0" : "=a"(v));
	return v;
}

static inline void
bf_usp_write(uint32_t v)
{
	__asm volatile("movl %0,%%usp" : : "a"(v));
}

/*
 * The stubs.  Gate in asm exactly as proven in P1; on a gate match,
 * save everything and let C decide.  Chaining pushes the stock handler
 * and returns to it with the frame untouched.
 */
__asm(
"	.text\n"
"	.even\n"
"	.globl	bf_stub_priv\n"
"bf_stub_priv:\n"
"	btst	#5,%sp@\n"		/* from supervisor mode? */
"	jne	2f\n"
"	movl	%d0,%sp@-\n"
"	movl	curpcb,%d0\n"
"	cmpl	bf_pcb,%d0\n"
"	jne	1f\n"
"	movl	%sp@+,%d0\n"
"	clrl	%sp@-\n"		/* pad, mirrors the stock handlers */
"	moveml	#0xffff,%sp@-\n"	/* d0-d7/a0-a7 ascending */
"	movl	%sp,%sp@-\n"		/* arg: register block */
"	jbsr	bf_ctrap_priv\n"
"	addql	#4,%sp\n"
"	tstl	%d0\n"
"	jeq	0f\n"
"	moveml	%sp@+,#0x7fff\n"	/* d0-d7/a0-a6; NOT the a7 slot */
"	addql	#8,%sp\n"		/* drop a7 slot + pad */
"	rte\n"				/* handled: resume the guest */
"0:	moveml	%sp@+,#0x7fff\n"
"	addql	#8,%sp\n"
"	movl	bf_orig_priv,%sp@-\n"
"	rts\n"
"1:	movl	%sp@+,%d0\n"
"	addql	#1,bf_n_chain_priv\n"
"2:	movl	bf_orig_priv,%sp@-\n"
"	rts\n"
"\n"
"	.even\n"
"	.globl	bf_stub_aline\n"
"bf_stub_aline:\n"
"	btst	#5,%sp@\n"
"	jne	2f\n"
"	movl	%d0,%sp@-\n"
"	movl	curpcb,%d0\n"
"	cmpl	bf_pcb,%d0\n"
"	jne	1f\n"
"	movl	%sp@+,%d0\n"
"	clrl	%sp@-\n"
"	moveml	#0xffff,%sp@-\n"
"	movl	%sp,%sp@-\n"
"	jbsr	bf_ctrap_aline\n"
"	addql	#4,%sp\n"
"	tstl	%d0\n"
"	jeq	0f\n"
"	moveml	%sp@+,#0x7fff\n"
"	addql	#8,%sp\n"
"	rte\n"
"0:	moveml	%sp@+,#0x7fff\n"
"	addql	#8,%sp\n"
"	movl	bf_orig_aline,%sp@-\n"
"	rts\n"
"1:	movl	%sp@+,%d0\n"
"	addql	#1,bf_n_chain_aline\n"
"2:	movl	bf_orig_aline,%sp@-\n"
"	rts\n"
"\n"
"	.even\n"
"	.globl	bf_stub_trace\n"
"bf_stub_trace:\n"
"	btst	#5,%sp@\n"
"	jne	2f\n"
"	movl	%d0,%sp@-\n"
"	movl	curpcb,%d0\n"
"	cmpl	bf_pcb,%d0\n"
"	jne	1f\n"
"	movl	%sp@+,%d0\n"
"	clrl	%sp@-\n"
"	moveml	#0xffff,%sp@-\n"
"	movl	%sp,%sp@-\n"
"	jbsr	bf_ctrap_trace\n"
"	addql	#4,%sp\n"
"	tstl	%d0\n"
"	jeq	0f\n"
"	moveml	%sp@+,#0x7fff\n"
"	addql	#8,%sp\n"
"	rte\n"
"0:	moveml	%sp@+,#0x7fff\n"
"	addql	#8,%sp\n"
"	movl	bf_orig_trace,%sp@-\n"
"	rts\n"
"1:	movl	%sp@+,%d0\n"
"2:	movl	bf_orig_trace,%sp@-\n"
"	rts\n"
);

void bf_stub_trace(void);

/*
 * Virtual SR plumbing, mirroring main_unix.cpp:
 *   GET_SR       (frame ccr) | EmulatedSR
 *   STORE_SR(v)  frame sr = v & 0x1f; EmulatedSR = v & 0xe700;
 *                if mask cleared and interrupts pending -> TriggerInterrupt
 * The trigger case is exactly what we DECLINE: userland must run it.
 */
static inline int
bf_get_sr(const struct bf_hwframe *f, uint16_t *out)
{
	uint16_t emul;

	if (ufetch_16((const uint16_t *)(uintptr_t)bf_uaddr_emulsr, &emul))
		return -1;
	*out = (f->sr & 0x1f) | emul;
	return 0;
}

/* Returns 0 ok, -1 fault, 1 must-defer (interrupt trigger due). */
static inline int
bf_store_sr(struct bf_hwframe *f, uint16_t v)
{
	uint32_t flags;

	if ((v & 0x0700) == 0) {
		if (ufetch_32((const uint32_t *)(uintptr_t)bf_uaddr_intflags,
		    &flags))
			return -1;
		if (flags != 0)
			return 1;
	}
	if (ustore_16((uint16_t *)(uintptr_t)bf_uaddr_emulsr, v & 0xe700))
		return -1;
	f->sr = v & 0x1f;
	return 0;
}

/*
 * The emulation.  Return 1 = handled (registers/frame updated), 0 =
 * decline with nothing mutated.  Every user access can fault; faults
 * always decline.
 */
int
bf_ctrap_priv(uint32_t *r)
{
	struct bf_hwframe *f = BF_FRAME(r);

	/*
	 * The asm gate compares curpcb against a saved pointer, which is
	 * fast but not an identity: if the emulator dies by SIGKILL while
	 * attached (no detach runs) and the allocator later hands that pcb
	 * to another process, its privilege violations would be given guest
	 * semantics.  Confirm the pid here, where a compare is free next to
	 * the work below.
	 */
	if (__predict_false(curproc->p_pid != bf_pid))
		return 0;
	uint32_t pc = bf_frame_pc(f);
	uint32_t usp = bf_usp_read();
	uint16_t op, ext, sr;
	int st;

	if (ufetch_16((const uint16_t *)pc, &op))
		goto defer;

	switch (op) {

	case 0x40e7:				/* move sr,-(sp) */
		if (bf_get_sr(f, &sr))
			goto defer;
		if (ustore_16((uint16_t *)(usp - 2), sr))
			goto defer;
		bf_usp_write(usp - 2);
		bf_frame_set_pc(f, pc + 2);
		break;

	case 0x46df:				/* move (sp)+,sr */
		if (ufetch_16((const uint16_t *)usp, &sr))
			goto defer;
		if ((st = bf_store_sr(f, sr)) != 0)
			goto defer;
		bf_usp_write(usp + 2);
		bf_frame_set_pc(f, pc + 2);
		break;

	case 0x007c:				/* ori #xxxx,sr */
		if (ufetch_16((const uint16_t *)(pc + 2), &ext))
			goto defer;
		if (bf_get_sr(f, &sr))
			goto defer;
		sr |= ext;
		/* oring in bits cannot lower the mask: no trigger check */
		if (ustore_16((uint16_t *)(uintptr_t)bf_uaddr_emulsr,
		    sr & 0xe700))
			goto defer;
		f->sr = sr & 0x1f;
		bf_frame_set_pc(f, pc + 4);
		break;

	case 0x027c:				/* andi #xxxx,sr */
		if (ufetch_16((const uint16_t *)(pc + 2), &ext))
			goto defer;
		if (bf_get_sr(f, &sr))
			goto defer;
		if ((st = bf_store_sr(f, sr & ext)) != 0)
			goto defer;
		bf_frame_set_pc(f, pc + 4);
		break;

	case 0x46fc:				/* move #xxxx,sr */
		if (ufetch_16((const uint16_t *)(pc + 2), &ext))
			goto defer;
		if ((st = bf_store_sr(f, ext)) != 0)
			goto defer;
		bf_frame_set_pc(f, pc + 4);
		break;

	case 0x46ef: {				/* move (d16,sp),sr */
		int16_t d16;

		if (ufetch_16((const uint16_t *)(pc + 2), &ext))
			goto defer;
		d16 = (int16_t)ext;
		if (ufetch_16((const uint16_t *)(usp + d16), &sr))
			goto defer;
		if ((st = bf_store_sr(f, sr)) != 0)
			goto defer;
		bf_frame_set_pc(f, pc + 4);
		break;
	}

	case 0x46d8: case 0x46d9: {		/* move (an)+,sr, n = 0,1 */
		uint32_t an = BF_R_A(r, op & 7);

		if (ufetch_16((const uint16_t *)an, &sr))
			goto defer;
		if ((st = bf_store_sr(f, sr)) != 0)
			goto defer;
		BF_R_A(r, op & 7) = an + 2;
		bf_frame_set_pc(f, pc + 2);
		break;
	}

	case 0x40f8: {				/* move sr,xxxx.w */
		if (ufetch_16((const uint16_t *)(pc + 2), &ext))
			goto defer;
		if (bf_get_sr(f, &sr))
			goto defer;
		if (ustore_16((uint16_t *)(uintptr_t)ext, sr))
			goto defer;
		bf_frame_set_pc(f, pc + 4);
		break;
	}

	case 0x40d0: case 0x40d1: case 0x40d2: case 0x40d3:
	case 0x40d4: case 0x40d5: case 0x40d6: case 0x40d7: {
						/* move sr,(an) */
		uint32_t an = (op & 7) == 7 ? usp : BF_R_A(r, op & 7);

		if (bf_get_sr(f, &sr))
			goto defer;
		if (ustore_16((uint16_t *)an, sr))
			goto defer;
		bf_frame_set_pc(f, pc + 2);
		break;
	}

	case 0x40c0: case 0x40c1: case 0x40c2: case 0x40c3:
	case 0x40c4: case 0x40c5: case 0x40c6: case 0x40c7:
						/* move sr,dn */
		if (bf_get_sr(f, &sr))
			goto defer;
		BF_R_D(r, op & 7) = (BF_R_D(r, op & 7) & 0xffff0000) | sr;
		bf_frame_set_pc(f, pc + 2);
		break;

	case 0x46c0: case 0x46c1: case 0x46c2: case 0x46c3:
	case 0x46c4: case 0x46c5: case 0x46c6: case 0x46c7:
						/* move dn,sr */
		if ((st = bf_store_sr(f, (uint16_t)BF_R_D(r, op & 7))) != 0)
			goto defer;
		bf_frame_set_pc(f, pc + 2);
		break;

	case 0xf327:				/* fsave -(sp) */
		if (ustore_32((uint32_t *)(usp - 4), 0x41000000))
			goto defer;
		bf_usp_write(usp - 4);
		bf_frame_set_pc(f, pc + 2);
		break;

	case 0xf35f:				/* frestore (sp)+ */
		bf_usp_write(usp + 4);
		bf_frame_set_pc(f, pc + 2);
		break;

	case 0xf478:				/* cpusha dc */
	case 0xf4f8:				/* cpusha dc/ic */
		__asm volatile(".word 0xf4f8");
		bf_frame_set_pc(f, pc + 2);
		break;

	case 0x4e73: {				/* rte */
		/*
		 * Diagnostic for the 0x9fc0000 hunt: when the IRQ glue's
		 * terminating rte (always at 00809b88) pops a PC outside
		 * guest code, log a marker BEFORE emulating it, carrying
		 * the popped pc, the frame address, and the guest sp.
		 * 0x00A00000 covers 8MB RAM + 1MB ROM at 0x800000 +
		 * headroom; the wild value is far above it.
		 */
		static const int frame_adj[16] = {
			0, 0, 4, 4, 8, 0, 0, 52, 50, 12, 24, 84, 16, 0, 0, 0
		};
		uint32_t npc;
		uint16_t nsr, fmt, pc_hi, pc_lo;

		if (ufetch_16((const uint16_t *)usp, &nsr))
			goto defer;
		if (ufetch_16((const uint16_t *)(usp + 2), &pc_hi))
			goto defer;
		if (ufetch_16((const uint16_t *)(usp + 4), &pc_lo))
			goto defer;
		if (ufetch_16((const uint16_t *)(usp + 6), &fmt))
			goto defer;
		npc = ((uint32_t)pc_hi << 16) | pc_lo;
		if (__predict_false(pc == 0x00809b88 && npc >= 0x00a00000)) {
			struct bf_tr_ent *e =
			    &bf_tr[bf_tr_n & (BF_TRN - 1)];

			e->pc = 0xbadc0deu;	/* marker: bad rte frame */
			e->a2 = npc;		/* the popped pc */
			e->fp = nsr | ((uint32_t)fmt << 16); /* sr + format */
			e->usp = usp;		/* where the frame was */
			bf_tr_n++;
		}
		if ((st = bf_store_sr(f, nsr)) != 0)
			goto defer;
		bf_frame_set_pc(f, npc);
		bf_usp_write(usp + 8 + frame_adj[fmt >> 12]);
		break;
	}

	default:
		goto defer;
	}

	if (__predict_false(bf_tracing))
		f->sr |= 0x8000;	/* survive rte / move-to-sr */

	if (__predict_false(bf_arm_now)) {
		struct bf_tr_ent *e = &bf_tr[bf_tr_n & (BF_TRN - 1)];

		bf_arm_now = 0;
		f->sr |= 0x8000;
		bf_tracing = 1;
		bf_tr_window = 0;
		e->pc = BF_MARK_ARM;
		e->a2 = pc;		/* where the arm took effect */
		e->fp = BF_R_A(r, 6);
		e->usp = bf_usp_read();
		bf_tr_n++;
	}
	bf_n_fast++;
	return 1;

defer:
	bf_n_defer++;
	return 0;
}

/*
 * A-line reflection: hand the trap to the guest's own dispatcher without
 * waking the emulator at all.  Byte-for-byte the frame main_unix.cpp
 * pushes -- vector offset $28, the PC of the A-line instruction, the
 * synthetic SR -- and the new PC comes from the guest vector table at
 * user address 0x28 (guest page zero is mapped; vm.user_va0_disable=0).
 * The guest dispatcher's terminating RTE lands in bf_ctrap_priv, which
 * pops this exact frame.  Neither EmulatedSR nor the real frame SR
 * changes: userland's A-line path never touched them either.
 */
int
bf_ctrap_aline(uint32_t *r)
{
	struct bf_hwframe *f = BF_FRAME(r);

	if (__predict_false(curproc->p_pid != bf_pid))
		return 0;
	uint32_t pc = bf_frame_pc(f);
	uint32_t usp = bf_usp_read();
	uint32_t npc;
	uint16_t sr;

	if (bf_get_sr(f, &sr))
		goto defer;
	if (ufetch_32((const uint32_t *)0x28, &npc))
		goto defer;
	if (ustore_16((uint16_t *)(usp - 2), 0x28))
		goto defer;
	if (ustore_32((uint32_t *)(usp - 6), pc))
		goto defer;
	if (ustore_16((uint16_t *)(usp - 8), sr))
		goto defer;
	bf_usp_write(usp - 8);
	bf_frame_set_pc(f, npc);

	if (__predict_false(bf_trace_arm_pc != 0 && pc == bf_trace_arm_pc)) {
		f->sr |= 0x8000;	/* T1: trace every instruction */
		bf_tracing = 1;
		bf_tr_armed_ret = pc + 2;
		bf_tr_window = 0;
	}

	if (__predict_false(bf_tracing))
		f->sr |= 0x8000;

	bf_n_aline++;
	bf_n_fast++;
	return 1;

defer:
	bf_n_defer++;
	return 0;
}

/*
 * Data watchpoint, piggybacked on the single-step trace: the address a
 * previous run proved gets clobbered (the a2 save slot of the function
 * whose epilogue is at 40826f6a; the run is fully reproducible).  When
 * its value changes between traced instructions, a marker entry is
 * logged carrying the pc of the instruction that JUST COMPLETED -- the
 * writer -- which is the previous entry's pc, since a trace exception
 * reports the NEXT instruction.
 */
#define BF_MARK_WRITE	0xdeadbeefu

int
bf_ctrap_trace(uint32_t *r)
{
	struct bf_hwframe *f = BF_FRAME(r);
	struct bf_tr_ent *e;
	uint32_t pc = bf_frame_pc(f);
	static uint32_t watch_last;
	static int watch_valid;
	uint32_t wv;

	if (__predict_false(curproc->p_pid != bf_pid))
		return 0;

	/*
	 * Trace GUEST code only.  T1 otherwise survives into the emulator's
	 * own text and libc -- one run's entire 32768-entry ring held
	 * nothing but host addresses.  Guest RAM is 8MB from 0 with ROM
	 * just above it, so anything at or past 0x00a00000 is not the guest.
	 */
	if (__predict_false(pc >= 0x00a00000)) {
		f->sr &= 0x7fff;
		bf_tracing = 0;
		return 1;
	}

	/*
	 * Watchpoint on the slot the fatal rts actually reads.  The RAM
	 * hook's rts at 00010f4c pops [usp]; arm the address the first time
	 * we step that instruction, then report every change to it.
	 */
	if (__predict_false(pc == 0x00010f4c && bf_watch_addr == 0))
		bf_watch_addr = bf_usp_read();

	if (bf_watch_addr != 0 &&
	    ufetch_32((const uint32_t *)bf_watch_addr, &wv) == 0) {
		if (watch_valid && wv != watch_last) {
			uint32_t wpc = bf_tr_n ?
			    bf_tr[(bf_tr_n - 1) & (BF_TRN - 1)].pc : 0;

			e = &bf_tr[bf_tr_n & (BF_TRN - 1)];
			e->pc = BF_MARK_WRITE;
			e->a2 = watch_last;	/* old value */
			e->fp = wv;		/* new value */
			e->usp = wpc;		/* the writing instruction */
			bf_tr_n++;
		}
		watch_last = wv;
		watch_valid = 1;
	}

	e = &bf_tr[bf_tr_n & (BF_TRN - 1)];
	e->pc = pc;
	e->a2 = BF_R_A(r, 2);
	e->fp = BF_R_A(r, 6);
	e->usp = bf_usp_read();
	bf_tr_n++;
	bf_tr_window++;

	/*
	 * Do NOT stop at the return: the first traced run showed a
	 * completely clean _FixRatio call whose caller then died in the
	 * very gap where tracing had been switched off.  Keep T1 until
	 * the window overflows the ring; the guest's own SR writes clear
	 * it anyway (bf_store_sr masks the frame SR to the ccr), and the
	 * next armed A-line re-arms.  The ring wraps, so at the fault it
	 * holds the last BF_TRN instructions regardless.
	 */
	if (bf_tr_window > BF_TRN) {
		f->sr &= 0x7fff;
		bf_tracing = 0;
	}

	return 1;
}

static inline uint32_t
bf_vbr_read(void)
{
	uint32_t v;

	__asm volatile("movc %%vbr,%0" : "=r"(v));
	return v;
}

static inline void
bf_vbr_write(uint32_t v)
{
	__asm volatile("movc %0,%%vbr" : : "r"(v));
}

/*
 * kern.bfast.attach: write nonzero to register the CALLING lwp; the
 * sysctl runs in the writer's context, so curlwp is exactly the thread
 * that takes the traps.  The uaddr sysctls must be set first; attach
 * refuses without them.  Write zero to detach.
 */
static int
bf_sysctl_attach(SYSCTLFN_ARGS)
{
	struct sysctlnode node;
	int t, error;

	t = (bf_pcb != NULL);
	node = *rnode;
	node.sysctl_data = &t;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;

	if (t) {
		if (bf_uaddr_emulsr == 0 || bf_uaddr_intflags == 0)
			return EINVAL;
		if (bf_pcb != NULL && bf_pcb != lwp_getpcb(curlwp))
			return EBUSY;
		bf_pcb = lwp_getpcb(curlwp);
		bf_pid = curproc->p_pid;
		printf("bfast: attached pid %d (emulsr %08x intflags %08x)\n",
		    curproc->p_pid, bf_uaddr_emulsr, bf_uaddr_intflags);
	} else {
		bf_pcb = NULL;
		bf_pid = 0;
		printf("bfast: detached\n");
	}
	return 0;
}

static int
bfast_modcmd(modcmd_t cmd, void *aux)
{
	const struct sysctlnode *node;
	int s;

	switch (cmd) {
	case MODULE_CMD_INIT:
		bf_alloclen = BF_TABBYTES + BF_TABALIGN;
		bf_alloc = kmem_zalloc(bf_alloclen, KM_SLEEP);
		bf_table = (uint32_t *)roundup2((uintptr_t)bf_alloc,
		    BF_TABALIGN);

		bf_orig_vbr = bf_vbr_read();
		memcpy(bf_table, (const void *)bf_orig_vbr, BF_TABBYTES);
		bf_orig_priv = bf_table[BF_VEC_PRIV];
		bf_orig_aline = bf_table[BF_VEC_ALINE];
		bf_orig_trace = bf_table[BF_VEC_TRACE];
		bf_table[BF_VEC_PRIV] = (uint32_t)bf_stub_priv;
		bf_table[BF_VEC_ALINE] = (uint32_t)bf_stub_aline;
		bf_table[BF_VEC_TRACE] = (uint32_t)bf_stub_trace;

		/* cpusha %bc as raw opcode; kmod builds target 68020. */
		__asm volatile(".word 0xf4f8");

		s = splhigh();
		bf_vbr_write((uint32_t)bf_table);
		bf_swapped = 1;
		splx(s);

		sysctl_createv(&bf_clog, 0, NULL, &node,
		    CTLFLAG_PERMANENT, CTLTYPE_NODE, "bfast",
		    SYSCTL_DESCR("Basilisk II trap fast path"),
		    NULL, 0, NULL, 0,
		    CTL_KERN, CTL_CREATE, CTL_EOL);
		if (node != NULL) {
			const int n = node->sysctl_num;

			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READWRITE | CTLFLAG_ANYWRITE,
			    CTLTYPE_INT, "attach",
			    SYSCTL_DESCR("write 1 to register the calling "
			        "lwp, 0 to detach"),
			    bf_sysctl_attach, 0, NULL, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READWRITE | CTLFLAG_ANYWRITE,
			    CTLTYPE_INT, "uaddr_emulsr",
			    SYSCTL_DESCR("user address of EmulatedSR"),
			    NULL, 0, &bf_uaddr_emulsr, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READWRITE | CTLFLAG_ANYWRITE,
			    CTLTYPE_INT, "uaddr_intflags",
			    SYSCTL_DESCR("user address of InterruptFlags"),
			    NULL, 0, &bf_uaddr_intflags, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READONLY, CTLTYPE_INT, "swapped",
			    SYSCTL_DESCR("VBR points at the module's table"),
			    NULL, 0, &bf_swapped, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READONLY, CTLTYPE_INT, "n_fast",
			    SYSCTL_DESCR("traps emulated at trap level"),
			    NULL, 0, &bf_n_fast, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READONLY, CTLTYPE_INT, "n_defer",
			    SYSCTL_DESCR("ours, declined to the signal path"),
			    NULL, 0, &bf_n_defer, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READONLY, CTLTYPE_INT, "n_aline",
			    SYSCTL_DESCR("A-line traps reflected to the guest"),
			    NULL, 0, &bf_n_aline, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READONLY, CTLTYPE_INT, "n_chain_priv",
			    SYSCTL_DESCR("other-source vector 8 traps"),
			    NULL, 0, &bf_n_chain_priv, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READWRITE | CTLFLAG_ANYWRITE,
			    CTLTYPE_INT, "arm_now",
			    SYSCTL_DESCR("arm tracing at the next "
			        "fast-pathed privileged op"),
			    NULL, 0, &bf_arm_now, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READWRITE | CTLFLAG_ANYWRITE,
			    CTLTYPE_INT, "trace_arm_pc",
			    SYSCTL_DESCR("A-line pc that arms single-step "
			        "tracing (0 = off)"),
			    NULL, 0, &bf_trace_arm_pc, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READONLY, CTLTYPE_INT, "watch_addr",
			    SYSCTL_DESCR("address the fatal rts pops"),
			    NULL, 0, &bf_watch_addr, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READONLY, CTLTYPE_INT, "trace_n",
			    SYSCTL_DESCR("instructions logged since load"),
			    NULL, 0, &bf_tr_n, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READONLY, CTLTYPE_STRUCT, "trace_ring",
			    SYSCTL_DESCR("struct {u32 pc,a2,fp,usp}[32768]"),
			    NULL, 0, bf_tr, sizeof(bf_tr),
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READONLY, CTLTYPE_INT, "n_chain_aline",
			    SYSCTL_DESCR("other-source vector 10 traps"),
			    NULL, 0, &bf_n_chain_aline, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
		}

		printf("bfast: phase 3, VBR %08x -> %08x, SR family "
		    "fast-pathed, A-line reflected\n",
		    bf_orig_vbr, (uint32_t)bf_table);
		return 0;

	case MODULE_CMD_FINI:
		if (bf_swapped) {
			s = splhigh();
			bf_vbr_write(bf_orig_vbr);
			bf_swapped = 0;
			splx(s);
		}
		bf_pcb = NULL;
		bf_pid = 0;
		sysctl_teardown(&bf_clog);
		if (bf_alloc != NULL) {
			kmem_free(bf_alloc, bf_alloclen);
			bf_alloc = NULL;
			bf_table = NULL;
		}
		printf("bfast: unloaded (fast %u defer %u aline %u "
		    "chained %u/%u)\n", bf_n_fast, bf_n_defer, bf_n_aline,
		    bf_n_chain_priv, bf_n_chain_aline);
		return 0;

	default:
		return ENOTTY;
	}
}
