/*
 * fbshot - capture the wscons framebuffer to a PPM.
 *
 * Written because every observation of this port so far has needed a
 * human at the screen, which makes the loop slow and leaves the most
 * important question -- what is the guest actually drawing? -- answerable
 * only in words.  This grabs the pixels instead.
 *
 * It reads the hardware CLUT through WSDISPLAYIO_GETCMAP and expands the
 * 8-bit indices to RGB, so the capture shows what is really on screen
 * rather than raw index values.  GETCMAP is safe here where a direct
 * RAMDAC readback is not: on this hardware reading the DAC data register
 * walks its R/G/B phase and turns the display red.
 *
 * Safe to run while a DGA guest owns the display: mapping the framebuffer
 * alongside another process was established to work, and this only reads.
 * It never changes the display mode.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <dev/wscons/wsconsio.h>

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	const char *dev = "/dev/ttyE0";
	const char *out = (argc > 1) ? argv[1] : "screen.ppm";
	struct wsdisplayio_fbinfo fbi;
	unsigned char r[256], g[256], b[256];
	struct wsdisplay_cmap cm;
	unsigned char *fb, *base;
	size_t maplen;
	FILE *f;
	int fd;
	unsigned x, y;

	if ((fd = open(dev, O_RDWR)) == -1)
		err(1, "open %s", dev);
	if (ioctl(fd, WSDISPLAYIO_GET_FBINFO, &fbi) == -1)
		err(1, "WSDISPLAYIO_GET_FBINFO");
	if (fbi.fbi_bitsperpixel != 8)
		errx(1, "only 8bpp is handled, got %u", fbi.fbi_bitsperpixel);

	memset(&cm, 0, sizeof(cm));
	cm.index = 0;
	cm.count = 256;
	cm.red = r;
	cm.green = g;
	cm.blue = b;
	if (ioctl(fd, WSDISPLAYIO_GETCMAP, &cm) == -1) {
		/* No palette available: fall back to greyscale so the shape
		 * of what is on screen is still visible. */
		warn("WSDISPLAYIO_GETCMAP, using greyscale");
		for (x = 0; x < 256; x++)
			r[x] = g[x] = b[x] = (unsigned char)x;
	}

	maplen = (size_t)fbi.fbi_fboffset + (size_t)fbi.fbi_fbsize;
	fb = mmap(NULL, maplen, PROT_READ, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED)
		err(1, "mmap %zu bytes", maplen);
	base = fb + fbi.fbi_fboffset;

	if ((f = fopen(out, "wb")) == NULL)
		err(1, "%s", out);
	fprintf(f, "P6\n%u %u\n255\n", fbi.fbi_width, fbi.fbi_height);
	for (y = 0; y < fbi.fbi_height; y++) {
		unsigned char *row = base + (size_t)y * fbi.fbi_stride;

		for (x = 0; x < fbi.fbi_width; x++) {
			unsigned char p = row[x];

			fputc(r[p], f);
			fputc(g[p], f);
			fputc(b[p], f);
		}
	}
	fclose(f);
	(void)munmap(fb, maplen);
	(void)close(fd);
	printf("wrote %s (%ux%u)\n", out, fbi.fbi_width, fbi.fbi_height);
	return 0;
}
