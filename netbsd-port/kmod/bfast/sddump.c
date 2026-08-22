/* sddump - per-opcode guest stack deltas measured by bfast. */
#include <sys/sysctl.h>
#include <stdio.h>
int main(void) {
	static unsigned short op[64];
	static int delta[64];
	static unsigned count[64];
	size_t lo = sizeof(op), ld = sizeof(delta), lc = sizeof(count), li = sizeof(int);
	int n, i;
	if (sysctlbyname("kern.bfast.sd_n", &n, &li, NULL, 0) ||
	    sysctlbyname("kern.bfast.sd_op", op, &lo, NULL, 0) ||
	    sysctlbyname("kern.bfast.sd_delta", delta, &ld, NULL, 0) ||
	    sysctlbyname("kern.bfast.sd_count", count, &lc, NULL, 0)) {
		perror("sysctl"); return 1;
	}
	printf("opcode  sp delta   count\n");
	for (i = 0; i < n && i < 64; i++)
		printf("  %04x   %+6d   %8u\n", op[i], delta[i], count[i]);
	return 0;
}
