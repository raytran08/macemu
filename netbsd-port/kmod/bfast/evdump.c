/* evdump - print bfast's event log (passes through watched pcs). */
#include <sys/sysctl.h>
#include <stdio.h>
struct e { unsigned pc, sp, fp, a0, aln, alpc, f[4], fsp[4], fsr[4]; };
int main(void) {
	static struct e v[256];
	size_t vl = sizeof(v), nl = sizeof(int);
	int n, k, lo;
	if (sysctlbyname("kern.bfast.events_n", &n, &nl, NULL, 0) ||
	    sysctlbyname("kern.bfast.events", v, &vl, NULL, 0)) {
		perror("sysctl"); return 1;
	}
	lo = n > 256 ? n - 256 : 0;
	for (k = lo; k < n; k++) {
		struct e *p = &v[k & 255];
		{
			const char *tag = "";
			if (p->pc == 0x008099b8) tag = "  <- dispatcher entry, a0=trapping pc";
			else if (p->pc == 0x008099d6) tag = "  <- dispatcher rts, a0=HANDLER";
			printf("%4d pc=%08x sp=%08x a0=%08x depth=%u%s\n",
			    k, p->pc, p->sp, p->a0, p->aln, tag);
		}
		{ int i; for (i = 0; i < 4 && p->f[i]; i++)
			printf("            frame[-%d] from %08x at sp %08x  guestSR=%04x%s\n",
			    i, p->f[i], p->fsp[i], p->fsr[i] & 0xffff,
			    (p->fsr[i] & 0x2000) ? " S" : "  (USER!)"); }
	}
	return 0;
}
