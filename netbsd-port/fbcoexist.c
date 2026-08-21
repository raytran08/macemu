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
 * fbcoexist - can a second process draw into the framebuffer while the X
 * server owns it?
 *
 * This is the one assumption the whole wscons DGA backend rests on, so it
 * gets tested on its own before any of that is written.
 *
 * Basilisk II's DGA drivers (driver_dga and its driver_fbdev subclass) do
 * not stop using X.  They keep an X connection for keyboard, mouse and
 * events, and bypass it only for drawing: the guest's screen is an mmap of
 * the framebuffer device, written directly.  So during normal operation the
 * X server and the emulator BOTH hold the display mapped.  If wscons or
 * kdrive refuses that -- or if one of them fights the other for the mode --
 * the zero-copy design is dead and no amount of driver work rescues it.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO
 *
 * It never sets WSDISPLAYIO_MODE_EMUL.  X has put the display into mapped
 * mode and is drawing on that basis; handing the console back underneath it
 * is precisely the mistake that corrupted the display and wedged a session
 * earlier in this project (a module was unloaded from under a running X).
 * The mode is left exactly as found, so the worst case here is a few
 * scribbled pixels that X will repaint.
 *
 * By default it does not issue WSDISPLAYIO_SMODE at all, because X has
 * already established mapped mode and the interesting question is whether a
 * second mmap simply works.  -m asks for the SMODE too, which is what a
 * standalone (no X) run would need, and is worth testing separately.
 *
 * WHAT TO LOOK FOR
 *
 * A grey frame with a white diagonal is drawn near the top-left, redrawn a
 * few times so it is unmistakably live rather than a leftover.  Then:
 *
 *   pattern appears, X keeps working  -> coexistence is fine, proceed
 *   pattern appears, X stops redrawing or the pointer dies -> they fight
 *   no pattern, mmap failed          -> the error says why
 *   pattern appears in the WRONG PLACE -> the offset or stride differs from
 *                                      what X is using; that is a real
 *                                      finding, not a failure
 *
 * That last case matters: dafbcons narrows the stride to 640 and pans the
 * scanout, and although it parks the pan when anything enters mapped mode,
 * "parked" is an assumption worth seeing rather than trusting.
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

#define BOX_W	200
#define BOX_H	120

int
main(int argc, char **argv)
{
	struct wsdisplayio_fbinfo fbi;
	const char *dev = "/dev/ttyE0";
	unsigned char *fb, *base;
	size_t maplen;
	int fd, mode, setmode = 0, passes = 6, i, x, y, ch;

	while ((ch = getopt(argc, argv, "md:n:")) != -1) {
		switch (ch) {
		case 'm':
			setmode = 1;
			break;
		case 'd':
			dev = optarg;
			break;
		case 'n':
			passes = atoi(optarg);
			break;
		default:
			fprintf(stderr,
			    "usage: fbcoexist [-m] [-d device] [-n passes]\n"
			    "  -m  also issue WSDISPLAYIO_SMODE DUMBFB\n"
			    "      (default: do not -- X has already set the "
			    "mode, and this never restores EMUL)\n");
			return 1;
		}
	}

	if ((fd = open(dev, O_RDWR)) == -1)
		err(1, "open %s", dev);

	if (ioctl(fd, WSDISPLAYIO_GET_FBINFO, &fbi) == -1)
		err(1, "WSDISPLAYIO_GET_FBINFO (is X running?)");

	printf("fbinfo while X holds the display:\n");
	printf("  %ux%u depth %u stride %u fboffset %llu fbsize %llu\n",
	    fbi.fbi_width, fbi.fbi_height, fbi.fbi_bitsperpixel,
	    fbi.fbi_stride, (unsigned long long)fbi.fbi_fboffset,
	    (unsigned long long)fbi.fbi_fbsize);

	if (setmode) {
		mode = WSDISPLAYIO_MODE_DUMBFB;
		if (ioctl(fd, WSDISPLAYIO_SMODE, &mode) == -1)
			warn("WSDISPLAYIO_SMODE DUMBFB (continuing anyway)");
		else
			printf("  SMODE DUMBFB accepted\n");
	} else
		printf("  no SMODE issued (X's mode left alone)\n");

	/*
	 * Map fboffset + fbsize, not fbsize: on this port fbi_fboffset is
	 * 4096 and the visible framebuffer starts there, so a mapping of
	 * fbsize alone runs off the end by exactly that much.  Same
	 * correction the tinyx wsfb.c patch carries.
	 */
	maplen = (size_t)fbi.fbi_fboffset + (size_t)fbi.fbi_fbsize;
	fb = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED)
		err(1, "mmap %zu bytes -- THIS is the answer we were after",
		    maplen);

	printf("  mmap of %zu bytes SUCCEEDED\n", maplen);
	printf("drawing %dx%d test box, %d passes -- watch the screen\n",
	    BOX_W, BOX_H, passes);
	fflush(stdout);

	base = fb + fbi.fbi_fboffset;

	/*
	 * Redraw several times with a moving diagonal so the box cannot be
	 * mistaken for a static leftover, and so X repainting over it is
	 * visible as a fight rather than read as success.
	 */
	for (i = 0; i < passes; i++) {
		for (y = 0; y < BOX_H; y++) {
			unsigned char *row = base + (size_t)y * fbi.fbi_stride;

			for (x = 0; x < BOX_W; x++)
				row[x] = ((x + y + i * 8) % 32 < 4) ? 0xff : 0x80;
		}
		sleep(1);
	}

	printf("done -- mode left exactly as found, EMUL never restored\n");
	(void)munmap(fb, maplen);
	(void)close(fd);
	return 0;
}
