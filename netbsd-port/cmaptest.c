/* Does this X server support AllocAll colormaps?  Basilisk II's DGA path
 * creates two of them (video_x.cpp), and BadMatch there stops it before
 * any of our driver runs.  Isolate the question from the emulator. */
#include <X11/Xlib.h>
#include <stdio.h>

static int bad = 0;
static int on_error(Display *d, XErrorEvent *e)
{
	char buf[128];
	XGetErrorText(d, e->error_code, buf, sizeof buf);
	printf("  X error: %s (opcode %d)\n", buf, e->request_code);
	bad = 1;
	return 0;
}

int main(void)
{
	Display *d = XOpenDisplay(NULL);
	if (!d) { printf("cannot open display\n"); return 1; }
	int scr = DefaultScreen(d);
	Visual *v = DefaultVisual(d, scr);
	printf("depth %d, visual class %d (PseudoColor=3)\n",
	    DefaultDepth(d, scr), v->class);
	XSetErrorHandler(on_error);

	printf("AllocNone: ");
	XCreateColormap(d, RootWindow(d, scr), v, AllocNone);
	XSync(d, False);
	printf(bad ? "FAILED\n" : "ok\n");

	bad = 0;
	printf("AllocAll:  ");
	XCreateColormap(d, RootWindow(d, scr), v, AllocAll);
	XSync(d, False);
	printf(bad ? "FAILED  <- this is what stops Basilisk II\n" : "ok\n");

	XCloseDisplay(d);
	return 0;
}
