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
#define BF_VEC_ILL	4

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
uint32_t	bf_orig_ill;		/* stock vector 4 handler */
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

/*
 * Event log: every pass through a handful of interesting pcs, kept in a
 * small array of its own so it cannot be wrapped away by the 2.8M-entry
 * instruction trace.  This is what makes healthy-vs-fatal comparison of
 * the SAME code path possible within one run.
 */
#define BF_EVN		256
struct bf_ev_ent { uint32_t pc, sp, fp, a0, aln, alpc;
			  uint32_t f[4], fsp[4], fsr[4]; };

/*
 * Shadow stack of live A-line frames.
 *
 * The ROM's A-line dispatcher does NOT consume its frame with rte -- it
 * rewrites the frame in place into [handler][return] and leaves via rts
 * (see 8099c6..8099d6).  So frames cannot be matched to returns; instead
 * a frame recorded at guest sp S is live while sp <= S and gone once sp
 * has risen past it.  Pruning on that rule at every trap gives the count
 * of A-line frames currently on the guest stack, which is exactly the
 * quantity that differs by one between a healthy and a fatal pass.
 */
#define BF_ALN		32
static uint32_t	bf_al_sp[BF_ALN];
static uint32_t	bf_al_pc[BF_ALN];
static uint32_t	bf_al_sr[BF_ALN];	/* guest SR seen at each reflection */
static int	bf_al_n;

static void
bf_al_prune(uint32_t sp)
{
	while (bf_al_n > 0 && sp > bf_al_sp[bf_al_n - 1])
		bf_al_n--;
}
static struct bf_ev_ent bf_ev[BF_EVN];
static uint32_t	bf_ev_n;
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
void bf_clog_ill(uint32_t *r);
static void bf_watch_check(uint32_t here);

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
"\n"
"	.even\n"
"	.globl	bf_stub_ill\n"
"bf_stub_ill:\n"
"	btst	#5,%sp@\n"
"	jne	1f\n"
"	movl	%d0,%sp@-\n"
"	movl	curpcb,%d0\n"
"	cmpl	bf_pcb,%d0\n"
"	jne	0f\n"
"	movl	%sp@+,%d0\n"
"	clrl	%sp@-\n"
"	moveml	#0xffff,%sp@-\n"
"	movl	%sp,%sp@-\n"
"	jbsr	bf_clog_ill\n"
"	addql	#4,%sp\n"
"	moveml	%sp@+,#0x7fff\n"
"	addql	#8,%sp\n"
"	movl	bf_orig_ill,%sp@-\n"	/* ALWAYS chain: EMUL_OP is userland's */
"	rts\n"
"0:	movl	%sp@+,%d0\n"
"1:	movl	bf_orig_ill,%sp@-\n"
"	rts\n"
);

void bf_stub_ill(void);

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

	bf_watch_check(pc);
	bf_al_prune(bf_usp_read());

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

	/*
	 * A-line reflections are ~35% of all traps, so sampling the watched
	 * slot here roughly triples the coverage of the trap-time watch.
	 * It cannot see a write that happens between two traps, but it
	 * narrows the bracket, which is the point.
	 */
	bf_watch_check(bf_frame_pc(f));
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

	bf_al_prune(usp);
	if (bf_al_n < BF_ALN) {
		bf_al_sp[bf_al_n] = usp - 8;	/* the frame just pushed */
		bf_al_pc[bf_al_n] = pc;		/* the trapping instruction */
		bf_al_sr[bf_al_n] = sr;		/* the SR the guest had */
		bf_al_n++;
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

/*
 * Watchpoint, decoupled from tracing.
 *
 * Called from the privileged-op handler, which runs continuously for the
 * whole boot, so it needs no T1 and covers the stretches single-stepping
 * cannot reach.  Resolution is one privileged op rather than one
 * instruction: the marker names the trap we were servicing when the
 * change was noticed, which brackets the writer.
 */
static void
bf_watch_check(uint32_t here)
{
	static uint32_t last;
	static int valid;
	uint32_t v;

	if (bf_watch_addr == 0)
		return;
	if (ufetch_32((const uint32_t *)bf_watch_addr, &v) != 0)
		return;
	if (valid && v != last) {
		struct bf_tr_ent *e = &bf_tr[bf_tr_n & (BF_TRN - 1)];

		e->pc = 0xdeadbeefu;
		e->a2 = last;		/* old */
		e->fp = v;		/* new */
		e->usp = here;		/* trap pc when noticed */
		bf_tr_n++;
	}
	last = v;
	valid = 1;
}

/*
 * Vector 4 logger.  EMUL_OP (0x71xx) decodes as an illegal instruction
 * and MUST reach userland -- the device models live there -- so this
 * handles nothing and always chains.  It exists only to keep the trace
 * ring continuous across the stretches single-stepping cannot cover:
 * T1 is in PSL_MBZ, so the signal path that services EMUL_OP necessarily
 * strips it.  One ring entry per EMUL_OP, no behaviour change.
 */
void
bf_clog_ill(uint32_t *r)
{
	struct bf_hwframe *f = BF_FRAME(r);
	struct bf_tr_ent *e;

	if (__predict_false(curproc->p_pid != bf_pid))
		return;

	bf_watch_check(bf_frame_pc(f));

	e = &bf_tr[bf_tr_n & (BF_TRN - 1)];
	e->pc = 0xe3110000u;	/* marker: EMUL_OP seen */
	e->a2 = bf_frame_pc(f);
	/*
	 * Record the opcode too.  The pc alone cannot be attributed to a
	 * particular EmulOp without knowing where each patch landed, and
	 * several land at runtime-computed offsets (sony_offset and
	 * friends).  The opcode names it directly.
	 */
	{
		uint16_t op;

		e->fp = (ufetch_16((const uint16_t *)bf_frame_pc(f), &op) == 0)
		    ? (uint32_t)op : 0xffffffffu;
	}
	e->usp = bf_usp_read();
	bf_tr_n++;
}

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
	 * nothing but host addresses.
	 *
	 * Guest code lives in TWO ranges, and forgetting the second cost
	 * three runs: RAM from 0 with ROM above it (below 0x00a00000), and
	 * the ROM's hardware alias at 0x40800000, which this ROM genuinely
	 * executes from.  Gating on the low range alone silently ended every
	 * window at the first `rtd` returning into 0x40826xxx.
	 */
	if (__predict_false(pc >= 0x00a00000 &&
	    (pc < 0x40800000 || pc >= 0x40900000))) {
		f->sr &= 0x7fff;
		bf_tracing = 0;
		return 1;
	}

	/*
	 * Watchpoint on the slot the fatal rts actually reads.  The RAM
	 * hook's rts at 00010f4c pops [usp]; arm the address the first time
	 * we step that instruction, then report every change to it.
	 */
	/*
	 * Watch the slot the resume tail pops.  At 40826620
	 * (`moveal %sp@+,%a0`) the value about to become a0 sits at [usp];
	 * a healthy pass finds 00010f2a there, the fatal one 00010f4a.
	 * Arm on the first pass and report every write afterwards, with
	 * the writing pc -- that is the whole remaining question.
	 */
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

	/*
	 * Per-instruction watch resolution while stepping.  The trap-time
	 * sampling in bf_ctrap_priv covers the whole boot but only sees the
	 * slot when a trap happens; inside a traced window this sees every
	 * single write, which is what is needed to name the writer.
	 */
	if (__predict_false(pc == 0x408265f0 || pc == 0x4082661e ||
	    pc == 0x40826f78 || pc == 0x40826f6a ||
	    pc == 0x408268ca ||
	    pc == 0x00010f28 || pc == 0x00010f48 ||
	    pc == 0x00010f2a || pc == 0x00010f4a ||
	    pc == 0x008099b0 || pc == 0x008099b8 ||
	    pc == 0x008099c6 || pc == 0x008099d6)) {
		struct bf_ev_ent *v = &bf_ev[bf_ev_n & (BF_EVN - 1)];
		uint32_t sp = bf_usp_read();

		v->pc = pc;
		v->sp = sp;
		v->fp = BF_R_A(r, 6);
		/*
		 * At the function's `link` (408268ca) the longword at [sp]
		 * is the return address, which names the caller -- that is
		 * the next question, so capture it here instead of a0.
		 */
		if (pc == 0x408268ca) {
			uint32_t ra;

			v->a0 = (ufetch_32((const uint32_t *)sp, &ra) == 0) ?
			    ra : 0xffffffffu;
		} else if (pc == 0x008099d6) {
			uint32_t h;

			/* rts is about to jump to [sp]: the handler */
			v->a0 = (ufetch_32((const uint32_t *)sp, &h) == 0) ?
			    h : 0xffffffffu;
		} else if (pc == 0x008099b8) {
			uint32_t tw;

			/* a2 holds the trap pc; read the trap word itself */
			v->a0 = (ufetch_16((const uint16_t *)BF_R_A(r, 2),
			    (uint16_t *)&tw) == 0) ? (tw & 0xffff) : 0xffffffffu;
			v->a0 = BF_R_A(r, 2);	/* the trapping pc */
		} else
			v->a0 = BF_R_A(r, 0);
		bf_al_prune(sp);
		v->aln = (uint32_t)bf_al_n;
		v->alpc = bf_al_n > 0 ? bf_al_pc[bf_al_n - 1] : 0;
		{
			int i;

			/* top four live frames, newest first */
			for (i = 0; i < 4; i++) {
				int j = bf_al_n - 1 - i;

				v->f[i]   = (j >= 0) ? bf_al_pc[j] : 0;
				v->fsp[i] = (j >= 0) ? bf_al_sp[j] : 0;
				v->fsr[i] = (j >= 0) ? bf_al_sr[j] : 0;
			}
		}
		bf_ev_n++;
	}

	bf_watch_check(pc);

	/*
	 * The ROM fill loop at 40826e72..40826e88 (`movel %d0,%a0@+` /
	 * `dbf %d1`) is what overwrites the guest stack.  Log its base and
	 * counter, but only once a0 has strayed near the stack -- logging
	 * every iteration would be thousands of entries of nothing.
	 * 0x00500000 is comfortably above any table it should be filling
	 * and below the observed stack around 0x005fa000.
	 */
	if (__predict_false(pc == 0x40826e86)) {
		uint32_t a0 = BF_R_A(r, 0);

		if (a0 >= 0x00500000) {
			struct bf_tr_ent *fe =
			    &bf_tr[bf_tr_n & (BF_TRN - 1)];

			fe->pc = 0xf111u << 16;	/* marker: fill in stack */
			fe->a2 = a0;		/* destination */
			fe->fp = BF_R_D(r, 0);	/* value being stored */
			fe->usp = BF_R_D(r, 1);	/* dbf counter remaining */
			bf_tr_n++;
		}
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
		bf_orig_ill = bf_table[BF_VEC_ILL];
		bf_table[BF_VEC_PRIV] = (uint32_t)bf_stub_priv;
		bf_table[BF_VEC_ALINE] = (uint32_t)bf_stub_aline;
		bf_table[BF_VEC_TRACE] = (uint32_t)bf_stub_trace;
		bf_table[BF_VEC_ILL] = (uint32_t)bf_stub_ill;

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
			    CTLFLAG_READWRITE | CTLFLAG_ANYWRITE,
			    CTLTYPE_INT, "watch_addr",
			    SYSCTL_DESCR("guest address to watch for writes"),
			    NULL, 0, &bf_watch_addr, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READONLY, CTLTYPE_INT, "trace_n",
			    SYSCTL_DESCR("instructions logged since load"),
			    NULL, 0, &bf_tr_n, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READONLY, CTLTYPE_INT, "events_n",
			    SYSCTL_DESCR("event log entries written"),
			    NULL, 0, &bf_ev_n, 0,
			    CTL_KERN, n, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READONLY, CTLTYPE_STRUCT, "events",
			    SYSCTL_DESCR("struct {u32 pc,sp,fp,a0,aln,alpc,f[4],fsp[4],fsr[4]}[256]"),
			    NULL, 0, bf_ev, sizeof(bf_ev),
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
