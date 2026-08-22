/* deskchk - exit 0 if the guest is showing a Mac menu bar (mostly-white row 8) */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <dev/wscons/wsconsio.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
	struct wsdisplayio_fbinfo fbi;
	unsigned char *fb; size_t len; int fd, x, white = 0;
	if ((fd = open("/dev/ttyE0", O_RDWR)) == -1) return 2;
	if (ioctl(fd, WSDISPLAYIO_GET_FBINFO, &fbi) == -1) return 2;
	len = (size_t)fbi.fbi_fboffset + (size_t)fbi.fbi_fbsize;
	fb = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) return 2;
	unsigned char *row = fb + fbi.fbi_fboffset + (size_t)8 * fbi.fbi_stride;
	int hist[256]; int i, best = 0, bestn = 0;
	for (i = 0; i < 256; i++) hist[i] = 0;
	for (x = 0; x < (int)fbi.fbi_width; x++) hist[row[x]]++;
	for (i = 0; i < 256; i++) if (hist[i] > bestn) { bestn = hist[i]; best = i; }
	printf("row8: dominant index %d covers %d/%u\n", best, bestn, fbi.fbi_width);
	(void)white;
	/* menu bar: one index covering most of the row */
	return bestn > 400 ? 0 : 1;
}
