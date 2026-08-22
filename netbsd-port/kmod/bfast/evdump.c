/* evdump - print bfast's event log (passes through watched pcs). */
#include <sys/sysctl.h>
#include <stdio.h>
struct e { unsigned pc, sp, fp, a0, aln, alpc, f[4], fsp[4]; };
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
		printf("%4d pc=%08x sp=%08x depth=%u\n", k, p->pc, p->sp, p->aln);
		{ int i; for (i = 0; i < 4 && p->f[i]; i++)
			printf("            frame[-%d] from %08x at sp %08x\n",
			    i, p->f[i], p->fsp[i]); }
	}
	return 0;
}
