/*
 * mapnotifytest - does this X server deliver MapNotify for an
 * override-redirect window?
 *
 * Basilisk II's DGA drivers create a fullscreen override-redirect window,
 * XMapRaised it, then sit in wait_mapped():
 *
 *     do { XMaskEvent(x_display, StructureNotifyMask, &e); }
 *     while ((e.type != MapNotify) || (e.xmap.event != w));
 *
 * XMaskEvent blocks forever if the event never comes, and on kdrive it
 * never does -- ktrace shows the process parked in poll() on the X socket
 * with an infinite timeout, no X error raised.  This isolates the question
 * from the emulator: map both kinds of window and see which report back.
 *
 * Everything here is timed out and nothing is grabbed, so it cannot take a
 * session hostage the way the emulator did.
 */

#include <X11/Xlib.h>

#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

/*
 * XMaskEvent is exactly what must not be used here -- it is the call that
 * hangs.  Wait on the connection instead, so "never arrived" is an answer
 * rather than a lockup.
 */
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

static void
try_window(Display *d, int screen, Bool override)
{
	XSetWindowAttributes wattr;
	Window w;
	int got;

	memset(&wattr, 0, sizeof(wattr));
	wattr.event_mask = StructureNotifyMask;
	wattr.override_redirect = override;

	w = XCreateWindow(d, RootWindow(d, screen), 0, 0, 320, 200, 0,
	    CopyFromParent, InputOutput, CopyFromParent,
	    CWEventMask | CWOverrideRedirect, &wattr);
	XMapRaised(d, w);
	XFlush(d);

	got = wait_for_map(d, w, 5.0);
	printf("  override_redirect=%-5s -> MapNotify %s\n",
	    override ? "True" : "False",
	    got ? "ARRIVED" : "NEVER CAME (5s timeout)");

	XDestroyWindow(d, w);
	XFlush(d);
}

int
main(void)
{
	Display *d = XOpenDisplay(NULL);

	if (d == NULL) {
		printf("cannot open display\n");
		return 1;
	}
	printf("MapNotify delivery, StructureNotifyMask selected at create:\n");
	try_window(d, DefaultScreen(d), False);	/* control */
	try_window(d, DefaultScreen(d), True);	/* what DGA uses */
	XCloseDisplay(d);
	return 0;
}
