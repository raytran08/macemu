/*
 * wedgebisect - which step in driver_wscons wedges the X server?
 *
 * fbcoexist maps the framebuffer under a live Xwscons, writes to it, and
 * leaves everything healthy.  driver_wscons does that too and the server
 * stops answering ANY client -- xdpyinfo included -- so the emulator's
 * hang in wait_mapped() is a symptom, not the cause.
 *
 * Two guesses have already been wrong (a missing MapNotify; our
 * WSDISPLAYIO_SMODE).  So stop guessing and bisect: the driver does a
 * handful of things fbcoexist does not, and each is added here one at a
 * time.  Run with a stage number; the harness checks whether X still
 * answers afterwards.  The lowest stage that wedges it is the answer.
 *
 *   1  create an override-redirect window, do not map it
 *   2  + set input focus, map it, wait (bounded) for MapNotify
 *   3  + grab the keyboard
 *   4  + grab the pointer
 *   5  + XChangePointerControl, which is what disable_mouse_accel does
 *   6  + mmap the framebuffer and write to it
 *
 * Every stage exits cleanly and releases what it took, so a stage that
 * does NOT wedge leaves the server fit for the next one.  Nothing here
 * uses XMaskEvent: that is the call that blocks forever.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>

#include <dev/wscons/wsconsio.h>

#include <X11/Xlib.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
on_x_error(Display *d, XErrorEvent *e)
{
	char buf[128];

	XGetErrorText(d, e->error_code, buf, sizeof buf);
	printf("    X error: %s (opcode %d) -- continuing\n", buf,
	    e->request_code);
	return 0;
}

static int
wait_for_map(Display *d, Window w, double secs)
{
	int fd = ConnectionNumber(d);
	struct timeval deadline, now, tv;

	gettimeofday(&deadline, NULL);
	deadline.tv_sec += (long)secs;
	for (;;) {
		while (XPending(d)) {
			XEvent e;

			XNextEvent(d, &e);
			if (e.type == MapNotify && e.xmap.event == w)
				return 1;
		}
		gettimeofday(&now, NULL);
		if (timercmp(&now, &deadline, >))
			return 0;
		timersub(&deadline, &now, &tv);

		fd_set r;
		FD_ZERO(&r);
		FD_SET(fd, &r);
		if (select(fd + 1, &r, NULL, NULL, &tv) <= 0)
			return 0;
	}
}

int
main(int argc, char **argv)
{
	int stage = (argc > 1) ? atoi(argv[1]) : 1;
	Display *d;
	Window w;
	XSetWindowAttributes wattr;
	int screen;

	d = XOpenDisplay(NULL);
	if (d == NULL) {
		printf("    stage %d: CANNOT OPEN DISPLAY (already wedged?)\n",
		    stage);
		return 2;
	}
	screen = DefaultScreen(d);
	XSetErrorHandler(on_x_error);

	memset(&wattr, 0, sizeof(wattr));
	wattr.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask |
	    ButtonReleaseMask | PointerMotionMask | StructureNotifyMask;
	wattr.override_redirect = True;
	wattr.background_pixel = BlackPixel(d, screen);

	w = XCreateWindow(d, RootWindow(d, screen), 0, 0,
	    DisplayWidth(d, screen), DisplayHeight(d, screen), 0,
	    DefaultDepth(d, screen), InputOutput, DefaultVisual(d, screen),
	    CWEventMask | CWBackPixel | CWOverrideRedirect, &wattr);
	XSync(d, False);
	printf("    created override-redirect window\n");

	if (stage >= 2) {
		XMapRaised(d, w);
		XSync(d, False);
		printf("    mapped; MapNotify %s\n",
		    wait_for_map(d, w, 5.0) ? "arrived" : "did NOT arrive");
		XSetInputFocus(d, w, RevertToParent, CurrentTime);
		XSync(d, False);
		printf("    set input focus\n");
	}
	if (stage >= 3) {
		XGrabKeyboard(d, w, True, GrabModeAsync, GrabModeAsync,
		    CurrentTime);
		XSync(d, False);
		printf("    grabbed keyboard\n");
	}
	if (stage >= 4) {
		XGrabPointer(d, w, True, PointerMotionMask | ButtonPressMask |
		    ButtonReleaseMask, GrabModeAsync, GrabModeAsync, w, None,
		    CurrentTime);
		XSync(d, False);
		printf("    grabbed pointer\n");
	}
	if (stage >= 5) {
		/* what disable_mouse_accel() does: server-wide pointer state */
		XChangePointerControl(d, True, False, 1, 1, 0);
		XSync(d, False);
		printf("    changed pointer control\n");
	}
	if (stage >= 6) {
		struct wsdisplayio_fbinfo fbi;
		int fd = open("/dev/ttyE0", O_RDWR);

		if (fd < 0 || ioctl(fd, WSDISPLAYIO_GET_FBINFO, &fbi) < 0)
			printf("    framebuffer: cannot open/query\n");
		else {
			size_t len = (size_t)fbi.fbi_fboffset +
			    (size_t)fbi.fbi_fbsize;
			unsigned char *fb = mmap(NULL, len,
			    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

			if (fb == MAP_FAILED)
				printf("    framebuffer: mmap failed\n");
			else {
				memset(fb + fbi.fbi_fboffset, 0x2a,
				    (size_t)fbi.fbi_stride * 64);
				printf("    mmapped and wrote 64 lines\n");
				munmap(fb, len);
			}
			close(fd);
		}
	}

	/* release everything, in the order the driver would */
	if (stage >= 4)
		XUngrabPointer(d, CurrentTime);
	if (stage >= 3)
		XUngrabKeyboard(d, CurrentTime);
	XDestroyWindow(d, w);
	XSync(d, False);
	XCloseDisplay(d);
	printf("    stage %d completed and cleaned up\n", stage);
	return 0;
}
