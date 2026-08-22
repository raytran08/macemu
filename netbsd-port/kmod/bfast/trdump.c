/* trdump - print the whole bfast trace ring, oldest first. */
#include <sys/sysctl.h>
#include <stdio.h>
#include <stdlib.h>
struct e { unsigned pc, a2, a3, usp; };
int main(void) {
	static struct e r[8192];
	size_t rl = sizeof(r), nl = sizeof(int);
	int n, k;
	if (sysctlbyname("kern.bfast.trace_n", &n, &nl, NULL, 0) ||
	    sysctlbyname("kern.bfast.trace_ring", r, &rl, NULL, 0)) {
		perror("sysctl"); return 1;
	}
	int lo = n > 8192 ? n - 8192 : 0;
	for (k = lo; k < n; k++) {
		struct e *p = &r[k & 8191];
		printf("%6d pc=%08x a2=%08x a3=%08x usp=%08x\n",
		    k, p->pc, p->a2, p->a3, p->usp);
	}
	return 0;
}
