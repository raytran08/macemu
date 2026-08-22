/*
 * bfast - in-kernel trap fast path for Basilisk II on NetBSD/mac68k.
 *
 * Phase 0: prove the hook mechanism and nothing else.
 *
 * The whole module rests on one manoeuvre: the mac68k kernel runs with
 * the CPU's VBR pointing at vectab (locore.s), so exception routing can
 * be changed for the entire machine with a single movc of the VBR to a
 * private copy of the table -- and changed back just as atomically.  The
 * kernel's own table is never written.  Later phases patch entries 8
 * (privilege violation) and 10 (A-line) in the COPY; this phase installs
 * an identical copy, which must be a no-op.
 *
 * If the machine survives modload/modunload cycles and runs normally in
 * between, the hook is sound.  Everything after that is just what the
 * stubs do.
 *
 * See FASTPATH.md for the full design.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/module.h>
#include <sys/kmem.h>
#include <sys/sysctl.h>

MODULE(MODULE_CLASS_MISC, bfast, NULL);

#define BF_NVEC		256
#define BF_TABBYTES	(BF_NVEC * sizeof(uint32_t))
/*
 * The 68040 UM only requires the VBR to be longword aligned, but the
 * table has historically lived on a 1KB boundary (it is 1KB long and
 * sat at 0 on the 68000); align the copy the same way out of caution.
 */
#define BF_TABALIGN	1024

static void	*bf_alloc;		/* underlying allocation */
static size_t	bf_alloclen;
static uint32_t	*bf_table;		/* aligned copy the VBR points at */
static uint32_t	bf_orig_vbr;		/* what we found, restored on unload */
static int	bf_swapped;

static struct sysctllog *bf_clog;

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

		/*
		 * Copy the live table from wherever the VBR points now,
		 * rather than from the vectab symbol: the running CPU's
		 * routing is the truth we must preserve.
		 */
		bf_orig_vbr = bf_vbr_read();
		memcpy(bf_table, (const void *)bf_orig_vbr, BF_TABBYTES);

		/*
		 * Push the copy out of the (copyback) data cache before
		 * the CPU can fetch vectors through it.  Exception vector
		 * fetches are data references on the 68040, so this is
		 * strictly belt-and-braces, but it is one instruction.
		 */
		/*
		 * cpusha %bc.  Raw opcode because the module build targets
		 * the 68020 baseline and the assembler refuses the
		 * mnemonic; this module is 68040-only regardless (the VBR
		 * hook and the target machine both are).
		 */
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
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READONLY, CTLTYPE_INT, "swapped",
			    SYSCTL_DESCR("VBR points at the module's table"),
			    NULL, 0, &bf_swapped, 0,
			    CTL_KERN, node->sysctl_num, CTL_CREATE, CTL_EOL);
			sysctl_createv(&bf_clog, 0, NULL, NULL,
			    CTLFLAG_READONLY, CTLTYPE_INT, "vbr_orig",
			    SYSCTL_DESCR("VBR as found at modload"),
			    NULL, 0, &bf_orig_vbr, 0,
			    CTL_KERN, node->sysctl_num, CTL_CREATE, CTL_EOL);
		}

		printf("bfast: phase 0, VBR %08x -> %08x (copy, unpatched)\n",
		    bf_orig_vbr, (uint32_t)bf_table);
		return 0;

	case MODULE_CMD_FINI:
		if (bf_swapped) {
			s = splhigh();
			bf_vbr_write(bf_orig_vbr);
			bf_swapped = 0;
			splx(s);
		}
		sysctl_teardown(&bf_clog);
		if (bf_alloc != NULL) {
			kmem_free(bf_alloc, bf_alloclen);
			bf_alloc = NULL;
			bf_table = NULL;
		}
		printf("bfast: unloaded, VBR restored to %08x\n", bf_orig_vbr);
		return 0;

	default:
		return ENOTTY;
	}
}
