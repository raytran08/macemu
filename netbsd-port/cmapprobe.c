/*
 * Copyright (c) 2026 Ray Tran
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * cmapprobe - who owns the colour lookup table, and can we borrow it?
 *
 * fbcoexist proved a second process can map and write the framebuffer
 * while X owns the display.  It also proved that is only half the job:
 * it wrote pixel values 0xff and 0x80 expecting grey and white and got
 * blue and dark, because at depth 8 a pixel is an INDEX and X had
 * allocated the palette those indices land in.
 *
 * A DGA guest has its own palette.  Mac OS reprograms the CLUT whenever
 * it feels like it, so driver_wscons must be able to push a palette to
 * the hardware while an X server is running, and ideally put back what it
 * found.  Three parties want those 256 entries here: the X server, the
 * host's panning console driver (which installs an r3g3b2 ramp when it
 * attaches), and the emulator.
 *
 * So: draw a ramp where the pixel value equals the X coordinate, making
 * the palette directly visible as a strip of 256 colours.  Then save the
 * current map, install a known one, pause to be looked at, and put the
 * saved one back.
 *
 * WHAT TO LOOK FOR
 *
 *   strip changes colour on install   -> PUTCMAP reaches the hardware
 *   the X SESSION also changes colour -> the CLUT is shared, as expected;
 *                                        the emulator must save/restore
 *                                        around focus changes
 *   X session unaffected              -> something is keeping per-client
 *                                        colormaps, which would be a
 *                                        surprise worth understanding
 *   everything returns on restore     -> save/restore works, and
 *                                        update_palette() is implementable
 *                                        the straightforward way
 *
 * SAFETY
 *
 * WSDISPLAYIO_GETCMAP is asked for the palette rather than reading the
 * RAMDAC directly.  That matters on this hardware: reading the CLUT back
 * through the DAC data register desynchronises its R/G/B phase and turns
 * the display red.  The ioctl returns genfb's own software copy instead,
 * so it is safe.
 *
 * As with fbcoexist, WSDISPLAYIO_MODE_EMUL is never set: handing the
 * console back while X is drawing is what corrupts the display.
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

#define STRIP_H	64		/* height of the 256-entry ramp */
#define CMAP_N	256

static unsigned char save_r[CMAP_N], save_g[CMAP_N], save_b[CMAP_N];
static unsigned char test_r[CMAP_N], test_g[CMAP_N], test_b[CMAP_N];

static int
cmap_get(int fd, unsigned char *r, unsigned char *g, unsigned char *b)
{
	struct wsdisplay_cmap cm;

	memset(&cm, 0, sizeof(cm));
	cm.index = 0;
	cm.count = CMAP_N;
	cm.red = r;
	cm.green = g;
	cm.blue = b;
	return ioctl(fd, WSDISPLAYIO_GETCMAP, &cm);
}

static int
cmap_put(int fd, unsigned char *r, unsigned char *g, unsigned char *b)
{
	struct wsdisplay_cmap cm;

	memset(&cm, 0, sizeof(cm));
	cm.index = 0;
	cm.count = CMAP_N;
	cm.red = r;
	cm.green = g;
	cm.blue = b;
	return ioctl(fd, WSDISPLAYIO_PUTCMAP, &cm);
}

int
main(int argc, char **argv)
{
	struct wsdisplayio_fbinfo fbi;
	const char *dev = (argc > 1) ? argv[1] : "/dev/ttyE0";
	int hold = (argc > 2) ? atoi(argv[2]) : 8;
	unsigned char *fb, *base;
	size_t maplen;
	int fd, i, x, y, saved = 0;

	if ((fd = open(dev, O_RDWR)) == -1)
		err(1, "open %s", dev);
	if (ioctl(fd, WSDISPLAYIO_GET_FBINFO, &fbi) == -1)
		err(1, "WSDISPLAYIO_GET_FBINFO (is X running?)");

	printf("%ux%u depth %u stride %u fboffset %llu\n",
	    fbi.fbi_width, fbi.fbi_height, fbi.fbi_bitsperpixel,
	    fbi.fbi_stride, (unsigned long long)fbi.fbi_fboffset);

	/* Save first: if this fails we still probe, but we cannot put back. */
	if (cmap_get(fd, save_r, save_g, save_b) == -1) {
		warn("WSDISPLAYIO_GETCMAP -- cannot save, will NOT install");
		printf("  refusing to change a palette we cannot restore\n");
		(void)close(fd);
		return 1;
	}
	saved = 1;
	printf("GETCMAP ok.  A few entries as X left them:\n");
	for (i = 0; i < CMAP_N; i += 51)
		printf("    [%3d] %02x %02x %02x\n",
		    i, save_r[i], save_g[i], save_b[i]);

	maplen = (size_t)fbi.fbi_fboffset + (size_t)fbi.fbi_fbsize;
	fb = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED)
		err(1, "mmap %zu bytes", maplen);
	base = fb + fbi.fbi_fboffset;

	/*
	 * Pixel value == x coordinate, so the strip IS the palette: entry n
	 * is drawn at column n and any change to the map is visible directly
	 * rather than inferred.
	 */
	for (y = 0; y < STRIP_H; y++) {
		unsigned char *row = base + (size_t)y * fbi.fbi_stride;

		for (x = 0; x < CMAP_N && x < (int)fbi.fbi_width; x++)
			row[x] = (unsigned char)x;
	}
	printf("drew a 256-entry index ramp at the top left\n");

	/* A ramp nobody could mistake for an accident: R, G and B each
	 * sweeping at a different rate, so every entry is distinct. */
	for (i = 0; i < CMAP_N; i++) {
		test_r[i] = (unsigned char)i;
		test_g[i] = (unsigned char)(255 - i);
		test_b[i] = (unsigned char)((i * 4) & 0xff);
	}

	printf("installing test palette in 2s -- watch the WHOLE screen\n");
	fflush(stdout);
	sleep(2);

	if (cmap_put(fd, test_r, test_g, test_b) == -1) {
		warn("WSDISPLAYIO_PUTCMAP -- that is the answer: it is refused");
	} else {
		printf("PUTCMAP accepted; holding %d s\n", hold);
		fflush(stdout);
		sleep(hold);
	}

	if (saved) {
		if (cmap_put(fd, save_r, save_g, save_b) == -1)
			warn("restoring the saved palette FAILED");
		else
			printf("saved palette restored\n");
	}

	printf("done -- mode left exactly as found, EMUL never restored\n");
	(void)munmap(fb, maplen);
	(void)close(fd);
	return 0;
}
