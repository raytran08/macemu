/*
 *  main_unix.cpp - Startup code for Unix
 *
 *  Basilisk II (C) Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"

#if !EMULATED_68K
// The native 68k handlers read and write the interrupted machine state
// through the ucontext, struct sigcontext no longer being visible to
// userland on NetBSD.
#include <ucontext.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <sstream>

#ifdef USE_SDL
# include "my_sdl.h"
#if !SDL_VERSION_ATLEAST(3, 0, 0)
#define SDL_PLATFORM_MACOS	__MACOSX__
#endif
#endif

#ifndef USE_SDL_VIDEO
# include <X11/Xlib.h>
#endif

#ifdef HAVE_PTHREADS
# include <pthread.h>
#endif

#if REAL_ADDRESSING || DIRECT_ADDRESSING
# include <sys/mman.h>
#endif

#if SDL_PLATFORM_MACOS
# include "utils_macosx.h"
#endif

#if !EMULATED_68K && defined(__NetBSD__)
# include <m68k/sync_icache.h> 
# include <m68k/frame.h>
# include <sys/param.h>
# include <sys/sysctl.h>
struct sigstate {
	int ss_flags;
	struct frame ss_frame;
	struct fpframe ss_fpstate;
};
# define SS_FPSTATE  0x02
# define SS_USERREGS 0x04
#endif

#ifdef ENABLE_GTK
# include <gtk/gtk.h>
# include <gdk/gdk.h>
# if GTK_CHECK_VERSION(3, 14, 0)
#  define ENABLE_GTK3
# endif
# if GTK_CHECK_VERSION(3, 22, 0)
#  define ENABLE_GTK_3_22
#  include "color_scheme.h"
# endif
#endif

#ifdef ENABLE_XF86_DGA
# include <X11/Xutil.h>
# include <X11/extensions/Xxf86dga.h>
#endif

#include <string>
using std::string;

#include "cpu_emulation.h"
#include "sys.h"
#include "rom_patches.h"
#include "xpram.h"
#include "timer.h"
#include "video.h"
#include "emul_op.h"
#include "prefs.h"
#include "prefs_editor.h"
#include "macos_util.h"
#include "adb.h"
#include "user_strings.h"
#include "version.h"
#include "main.h"
#include "vm_alloc.h"
#include "sigsegv.h"
#include "rpc.h"

#if USE_JIT
#ifdef UPDATE_UAE
extern void (*flush_icache)(void); // from compemu_support.cpp
extern bool UseJIT;
#else
extern void flush_icache_range(uint8 *start, uint32 size); // from compemu_support.cpp
#endif
#endif

#ifdef ENABLE_MON
# include "mon.h"
#endif

#define DEBUG 0
#include "debug.h"


// Constants
const char ROM_FILE_NAME[] = "ROM";
#if !EMULATED_68K
const int SIG_STACK_SIZE = SIGSTKSZ;	// Size of signal stack
#endif
const int SCRATCH_MEM_SIZE = 0x10000;	// Size of scratch memory area
const int ROM_MAX_SIZE = 0x100000;


#if !EMULATED_68K
// RAM and ROM pointers
uint32 RAMBaseMac;		// RAM base (Mac address space)
uint8 *RAMBaseHost;		// RAM base (host address space)
uint32 RAMSize;			// Size of RAM
uint32 ROMBaseMac;		// ROM base (Mac address space)
uint8 *ROMBaseHost;		// ROM base (host address space)
uint32 ROMSize;			// Size of ROM
#endif


// CPU and FPU type, addressing mode
int CPUType;
bool CPUIs68060;
int FPUType;
bool TwentyFourBitAddressing;


// Global variables
#ifndef USE_SDL_VIDEO
extern char *x_display_name;						// X11 display name
extern Display *x_display;							// X11 display handle
#ifdef X11_LOCK_TYPE
X11_LOCK_TYPE x_display_lock = X11_LOCK_INIT;		// X11 display lock
#endif
#endif

static uint8 last_xpram[XPRAM_SIZE];				// Buffer for monitoring XPRAM changes

#ifdef HAVE_PTHREADS
#if !EMULATED_68K
static pthread_t emul_thread;						// Handle of MacOS emulation thread (main thread)
#endif
static int use_gui = -1;   							// Override prefs and show gui

static bool xpram_thread_active = false;			// Flag: XPRAM watchdog installed
static volatile bool xpram_thread_cancel = false;	// Flag: Cancel XPRAM thread
static pthread_t xpram_thread;						// XPRAM watchdog

static bool tick_thread_active = false;				// Flag: 60Hz thread installed
static volatile bool tick_thread_cancel = false;	// Flag: Cancel 60Hz thread
static pthread_t tick_thread;						// 60Hz thread
static pthread_attr_t tick_thread_attr;				// 60Hz thread attributes

static pthread_mutex_t intflag_lock = PTHREAD_MUTEX_INITIALIZER;	// Mutex to protect InterruptFlags
#define LOCK_INTFLAGS pthread_mutex_lock(&intflag_lock)
#define UNLOCK_INTFLAGS pthread_mutex_unlock(&intflag_lock)

#else

#define LOCK_INTFLAGS
#define UNLOCK_INTFLAGS

#endif

#if !EMULATED_68K
#define SIG_IRQ SIGUSR1
static struct sigaction sigirq_sa;	// Virtual 68k interrupt signal
static struct sigaction sigill_sa;	// Illegal instruction
static void *sig_stack = NULL;		// Stack for signal handlers
uint16 EmulatedSR;					// Emulated bits of SR (supervisor bit and interrupt mask)
#endif

#if USE_SCRATCHMEM_SUBTERFUGE
uint8 *ScratchMem = NULL;			// Scratch memory for Mac ROM writes
#endif

#if !defined(HAVE_PTHREADS)
static struct sigaction timer_sa;	// sigaction used for timer

#if defined(HAVE_TIMER_CREATE) && defined(_POSIX_REALTIME_SIGNALS)
#define SIG_TIMER SIGRTMIN
static timer_t timer;				// 60Hz timer
#endif
#endif // !HAVE_PTHREADS

#ifdef ENABLE_MON
static struct sigaction sigint_sa;	// sigaction for SIGINT handler
static void sigint_handler(...);
#endif

#if REAL_ADDRESSING
static bool lm_area_mapped = false;	// Flag: Low Memory area mmap()ped
#endif

static rpc_connection_t *gui_connection = NULL;	// RPC connection to the GUI
static const char *gui_connection_path = NULL;	// GUI connection identifier


// Prototypes
static void *xpram_func(void *arg);
static void *tick_func(void *arg);
static void one_tick(...);
#if !EMULATED_68K
static void sigirq_handler(int sig, siginfo_t *sip, void *uap);
static void sigill_handler(int sig, siginfo_t *sip, void *uap);
extern "C" void EmulOpTrampoline(void);
#endif

// vde switch variable
char* vde_sock;

/*
 *  Ersatz functions
 */

extern "C" {

#ifndef HAVE_STRDUP
char *strdup(const char *s)
{
	char *n = (char *)malloc(strlen(s) + 1);
	strcpy(n, s);
	return n;
}
#endif

}


/*
 *  Helpers to map memory that can be accessed from the Mac side
 */

// NOTE: VM_MAP_32BIT is only used when compiling a 64-bit JIT on specific platforms
void *vm_acquire_mac(size_t size)
{
	return vm_acquire(size, VM_MAP_DEFAULT | VM_MAP_32BIT);
}

#if REAL_ADDRESSING
static int vm_acquire_mac_fixed(void *addr, size_t size)
{
	return vm_acquire_fixed(addr, size, VM_MAP_DEFAULT | VM_MAP_32BIT);
}
#endif

/*
 *  Stack-write watch (diagnostic, for the 0x9fc0000 hunt)
 *
 *  Single-stepping cannot see the write that poisons [005fa194]: it lands
 *  in the gap after an EMUL_OP, where T1 has necessarily been stripped.
 *  Page protection does not care about trap state.  The page is written
 *  constantly, so arming is triggered on the exact predecessor value the
 *  kernel watchpoint recorded (0x31300000 -> 0x00010f4a); the very next
 *  write to the page then faults and names its instruction.
 */
#define BW_SLOT		0x005fa194u
#define BW_TRIGGER	0x31300000u
static uintptr	bw_page;		/* protected page, 0 = disarmed */
static bool	bw_fired;

static bool	bw_active;		/* trigger seen: keep re-arming */

static void bw_arm_if_ready(void)
{
	long ps;

	if (bw_fired || bw_page != 0)
		return;
	if (!bw_active) {
		if (ReadMacInt32(BW_SLOT) != BW_TRIGGER)
			return;
		bw_active = true;
	}

	ps = sysconf(_SC_PAGESIZE);
	bw_page = (uintptr)Mac2HostAddr(BW_SLOT) & ~(uintptr)(ps - 1);
	if (mprotect((void *)bw_page, ps, PROT_READ) != 0) {
		fprintf(stderr, "stack watch: mprotect failed: %s\n",
		    strerror(errno));
		bw_page = 0;
		bw_fired = true;
		return;
	}
}

/*
 *  SIGSEGV handler
 */

static sigsegv_return_t sigsegv_handler(sigsegv_info_t *sip)
{
	const uintptr fault_address = (uintptr)sigsegv_get_fault_address(sip);

	/* Stack-write watch: name the writer, then get out of the way. */
	if (bw_page != 0) {
		long ps = sysconf(_SC_PAGESIZE);

		if (fault_address >= bw_page &&
		    fault_address < bw_page + (uintptr)ps) {
			const void *ip = sigsegv_get_fault_instruction_address(sip);

			bool ours = (fault_address >= BW_SLOT &&
			    fault_address < BW_SLOT + 4);

			fprintf(stderr, "STACK WATCH%s: write to %08lx by "
			    "instruction at %p  (slot now %08x)\n",
			    ours ? " *** THE SLOT ***" : "",
			    (unsigned long)fault_address, ip,
			    (unsigned)ReadMacInt32(BW_SLOT));
			mprotect((void *)bw_page, ps,
			    PROT_READ | PROT_WRITE | PROT_EXEC);
			bw_page = 0;
			if (ours)
				bw_fired = true;	/* done: stop re-arming */
			return SIGSEGV_RETURN_SUCCESS;
		}
	}
#if ENABLE_VOSF
	// Handle screen fault
	extern bool Screen_fault_handler(sigsegv_info_t *sip);
	if (Screen_fault_handler(sip))
		return SIGSEGV_RETURN_SUCCESS;
#endif

#ifdef HAVE_SIGSEGV_SKIP_INSTRUCTION
	// Ignore writes to ROM
	if (((uintptr)fault_address - (uintptr)ROMBaseHost) < ROMSize)
		return SIGSEGV_RETURN_SKIP_INSTRUCTION;

	// Ignore all other faults, if requested
	if (PrefsFindBool("ignoresegv"))
		return SIGSEGV_RETURN_SKIP_INSTRUCTION;
#endif

	return SIGSEGV_RETURN_FAILURE;
}

/*
 *  Dump state when everything went wrong after a SEGV
 */

static void sigsegv_dump_state(sigsegv_info_t *sip)
{
	const sigsegv_address_t fault_address = sigsegv_get_fault_address(sip);
	const sigsegv_address_t fault_instruction = sigsegv_get_fault_instruction_address(sip);
	fprintf(stderr, "Caught SIGSEGV at address %p", fault_address);
	if (fault_instruction != SIGSEGV_INVALID_ADDRESS)
		fprintf(stderr, " [IP=%p]", fault_instruction);
	fprintf(stderr, "\n");
#if !EMULATED_68K && defined(__NetBSD__) && defined(__m68k__)
	{
		extern void bf_dump_ring(void);
		bf_dump_ring();
	}
#endif
#if EMULATED_68K
	uaecptr nextpc;
#ifdef UPDATE_UAE
	extern void m68k_dumpstate(FILE *, uaecptr *nextpc);
	m68k_dumpstate(stderr, &nextpc);
#else
	extern void m68k_dumpstate(uaecptr *nextpc);
	m68k_dumpstate(&nextpc);
#endif
#endif
#if USE_JIT && JIT_DEBUG
	extern void compiler_dumpstate(void);
	compiler_dumpstate();
#endif
	VideoQuitFullScreen();
#ifdef ENABLE_MON
	const char *arg[4] = {"mon", "-m", "-r", NULL};
	mon(3, arg);
#endif
	QuitEmulator();
}


/*
 *  Update virtual clock and trigger interrupts if necessary
 */

#ifdef USE_CPU_EMUL_SERVICES
static uint64 n_check_ticks = 0;
static uint64 emulated_ticks_start = 0;
static uint64 emulated_ticks_count = 0;
static int64 emulated_ticks_current = 0;
static int32 emulated_ticks_quantum = 1000;
int32 emulated_ticks = emulated_ticks_quantum;

void cpu_do_check_ticks(void)
{
#if DEBUG
	n_check_ticks++;
#endif

	uint64 now;
	static uint64 next = 0;
	if (next == 0)
		next = emulated_ticks_start = GetTicks_usec();

	// Update total instructions count
	if (emulated_ticks <= 0) {
		emulated_ticks_current += (emulated_ticks_quantum - emulated_ticks);
		// XXX: can you really have a machine fast enough to overflow
		// a 63-bit m68k instruction counter within 16 ms?
		if (emulated_ticks_current < 0) {
			printf("WARNING: Overflowed 63-bit m68k instruction counter in less than 16 ms!\n");
			goto recalibrate_quantum;
		}
	}

	// Check for interrupt opportunity
	now = GetTicks_usec();
	if (next < now) {
		one_tick();
		do {
			next += 16625;
		} while (next < now);
		emulated_ticks_count++;

		// Recalibrate 1000 Hz quantum every 10 ticks
		static uint64 last = 0;
		if (last == 0)
			last = now;
		else if (now - last > 166250) {
		  recalibrate_quantum:
			emulated_ticks_quantum = ((uint64)emulated_ticks_current * 1000) / (now - last);
			emulated_ticks_current = 0;
			last = now;
		}
	}

	// Update countdown
	if (emulated_ticks <= 0)
		emulated_ticks += emulated_ticks_quantum;
}
#else
uint16 emulated_ticks;
void cpu_do_check_ticks(void)
{
	static int delay = -1;
	if (delay < 0)
		delay = PrefsFindInt32("delay");
	if (delay)
		usleep(delay);
}
#endif


/*
 *  Main program
 */

static void usage(const char *prg_name)
{
	printf(
		"Usage: %s [OPTION...]\n"
		"\nUnix options:\n"
		"  --config FILE\n    read/write configuration from/to FILE\n"
		"  --display STRING\n    X display to use\n"
		"  --break ADDRESS\n    set ROM breakpoint in hexadecimal\n"
		"  --loadbreak FILE\n    load breakpoint from FILE\n"
		"  --rominfo\n    dump ROM information\n"
		"  --switch SWITCH_PATH\n    vde_switch address\n", prg_name
	);
	LoadPrefs(NULL); // read the prefs file so PrefsPrintUsage() will print the correct default values
	PrefsPrintUsage();
	printf("\nBuild Date: %s\n", __DATE__);
	exit(0);
}

#ifdef ENABLE_GTK
GtkWindow *win;

static void gui_startup (void)
{
#ifdef ENABLE_GTK_3_22
	color_scheme_set(APP_PREFERS_LIGHT);
#endif
	if (use_gui && !PrefsEditor())
		QuitEmulator();
#ifdef ENABLE_GTK_3_22
	else
		color_scheme_disconnect();
#endif
}

#ifdef ENABLE_GTK3
static void gui_activate (GtkApplication *app)
{
	g_assert (GTK_IS_APPLICATION (app));
	win = gtk_application_get_active_window (app);
	/* Ask the window manager/compositor to present the window. */
	if (win != NULL)
		gtk_window_present (win);
}
#endif
#endif




/* TEMPORARY */
static void report_fatal_signal(int sig)
{
	char buf[64];
	int n = snprintf(buf, sizeof buf, "FATAL SIGNAL %d\n", sig);

	(void)write(2, buf, n);
	_exit(128 + sig);
}

int main(int argc, char **argv)
{
#ifdef ENABLE_GTK3
	GtkApplication *app = NULL;
	int ret;
#endif
	const char *vmdir = NULL;
	char str[256];

	// Initialize variables
	RAMBaseHost = NULL;
	ROMBaseHost = NULL;
	srand(time(NULL));
	tzset();

	// Print some info
	printf(GetString(STR_ABOUT_TEXT1), VERSION_MAJOR, VERSION_MINOR);
	printf(" %s\n", GetString(STR_ABOUT_TEXT2));

	// Parse command line arguments
	for (int i=1; i<argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
#ifndef USE_SDL_VIDEO
		} else if (strcmp(argv[i], "--display") == 0) {
			i++; // don't remove the argument, gtk_init() needs it too
			if (i < argc)
				x_display_name = strdup(argv[i]);
#endif
		} else if (strcmp(argv[i], "--gui-connection") == 0) {
			argv[i++] = NULL;
			if (i < argc) {
				gui_connection_path = argv[i];
				argv[i] = NULL;
			}
		} else if (strcmp(argv[i], "--break") == 0) {
			argv[i++] = NULL;
			if (i < argc) {
				std::stringstream ss;
				ss << std::hex << argv[i];
				ss >> ROMBreakpoint;
				argv[i] = NULL;
			}
#ifdef ENABLE_MON
		} else if (strcmp(argv[i], "--loadbreak") == 0) {
			argv[i++] = NULL;
			if (i < argc)
				mon_load_break_point(argv[i]);
#endif
		} else if (strcmp(argv[i], "--config") == 0) {
			argv[i++] = NULL;
			if (i < argc) {
				extern string UserPrefsPath; // from prefs_unix.cpp
				UserPrefsPath = argv[i];
				argv[i] = NULL;
			}
		} else if (strcmp(argv[i], "--rominfo") == 0) {
			argv[i] = NULL;
			PrintROMInfo = true;
		} else if (strcmp(argv[i], "--switch") == 0) {
			argv[i] = NULL;
			if (argv[++i] == NULL) {
				printf("switch address not defined\n");
				usage(argv[0]);
			}
			vde_sock = argv[i];
			argv[i] = NULL;
		} else if (strcmp(argv[i], "--nogui") == 0) {
			// We intercept the --nogui commandline so that the settings
			// window can change the setting from the prefs file
			argv[i++] = NULL;
			if (i < argc) {
				if (strcmp(argv[i], "true") == 0) {
					use_gui = false;
					argv[i] = NULL;
				}
				else if (strcmp(argv[i], "false") == 0) {
					use_gui = true;
					argv[i] = NULL;
				}
			} else {
				use_gui = false;
			}
		} else if (strcmp(argv[i], "--gui") == 0 || strcmp(argv[i], "--settings") == 0) {
			// Alternative commands to enter the GUI
			use_gui = true;
			argv[i] = NULL;
		}
		
#if defined(__APPLE__) && defined(__MACH__)
		// Mac OS X likes to pass in various options of its own, when launching an app.
		// Attempt to ignore these.
		if (argv[i]) {
			const char * mac_psn_prefix = "-psn_";
			if (strcmp(argv[i], "-NSDocumentRevisionsDebugMode") == 0) {
				argv[i] = NULL;
			} else if (strncmp(mac_psn_prefix, argv[i], strlen(mac_psn_prefix)) == 0) {
				argv[i] = NULL;
			}
		}
#endif
	}

	// Remove processed arguments
	for (int i=1; i<argc; i++) {
		int k;
		for (k=i; k<argc; k++)
			if (argv[k] != NULL)
				break;
		if (k > i) {
			k -= i;
			for (int j=i+k; j<argc; j++)
				argv[j-k] = argv[j];
			argc -= k;
		}
	}

	// Connect to the external GUI
	if (gui_connection_path) {
		if ((gui_connection = rpc_init_client(gui_connection_path)) == NULL) {
			fprintf(stderr, "Failed to initialize RPC client connection to the GUI\n");
			return 1;
		}
	}

	// Read preferences
	PrefsInit(vmdir, argc, argv);
	// Only use nogui preference if not passed as command line argument
	if (use_gui == -1)
		use_gui = !PrefsFindBool("nogui");

	// Any command line arguments left?
	for (int i=1; i<argc; i++) {
		if (argv[i][0] == '-') {
			fprintf(stderr, "Unrecognized option '%s'\n", argv[i]);
			usage(argv[0]);
		}
	}

#ifndef USE_SDL_VIDEO
	// Open display
	/*
	 * Must precede every other Xlib call.  Xlib keeps unprotected global
	 * state, and this emulator is both multi-threaded (redraw and tick
	 * threads) and, in the native 68k build, interrupted by SIGALRM and
	 * SIG_IRQ continuously.  A request interrupted part-written leaves a
	 * partial request on the socket, and a server waiting for the rest
	 * of one stops answering every client -- which is the failure seen
	 * on this port.
	 */
	if (!XInitThreads())
		fprintf(stderr, "warning: XInitThreads() failed\n");

	x_display = XOpenDisplay(x_display_name);
	if (x_display == NULL) {
		char str[256];
		sprintf(str, GetString(STR_NO_XSERVER_ERR), XDisplayName(x_display_name));
		ErrorAlert(str);
		QuitEmulator();
	}

#if defined(ENABLE_XF86_DGA) && !defined(ENABLE_MON)
	// Fork out, so we can return from fullscreen mode when things get ugly
	XF86DGAForkApp(DefaultScreen(x_display));
#endif
#endif

#ifdef USE_SDL
	// Initialize SDL system
	int sdl_flags = 0;
#ifdef USE_SDL_VIDEO
	sdl_flags |= SDL_INIT_VIDEO;
#endif
#ifdef USE_SDL_AUDIO
	sdl_flags |= SDL_INIT_AUDIO;
#endif
	assert(sdl_flags != 0);
	if (SDL_Init(sdl_flags) == -1) {
		char str[256];
		sprintf(str, "Could not initialize SDL: %s.\n", SDL_GetError());
		ErrorAlert(str);
		QuitEmulator();
	}
	atexit(SDL_Quit);

#if SDL_PLATFORM_MACOS && SDL_VERSION_ATLEAST(2,0,0)
	// On Mac OS X hosts, SDL2 will create its own menu bar.  This is mostly OK,
	// except that it will also install keyboard shortcuts, such as Command + Q,
	// which can interfere with keyboard shortcuts in the guest OS.
	//
	// HACK: disable these shortcuts, while leaving all other pieces of SDL2's
	// menu bar in-place.
	disable_SDL2_macosx_menu_bar_keyboard_shortcuts();
#endif
	
#endif

	// Init system routines
	SysInit();

#ifdef ENABLE_GTK3
	if (!gui_connection) {
		// Init GTK
		app = gtk_application_new (GetString(STR_APP_ID), G_APPLICATION_FLAGS_NONE);
		g_set_prgname (GetString(STR_APP_DISPLAY_NAME));
		g_signal_connect (app, "activate", G_CALLBACK (gui_activate), NULL);
		g_signal_connect (app, "startup", G_CALLBACK (gui_startup), NULL);
		g_application_register (G_APPLICATION (app), NULL, NULL);
		ret = g_application_run (G_APPLICATION (app), argc, argv);
	}
#elif defined(ENABLE_GTK)
	if (!gui_connection) {
		// Init GTK
		gtk_set_locale();
		gtk_init(&argc, &argv);
		gui_startup();
	}
#endif

	// Install the handler for SIGSEGV
	if (!sigsegv_install_handler(sigsegv_handler)) {
		sprintf(str, GetString(STR_SIG_INSTALL_ERR), "SIGSEGV", strerror(errno));
		ErrorAlert(str);
		QuitEmulator();
	}
	
	// Register dump state function when we got mad after a segfault
	sigsegv_set_dump_state(sigsegv_dump_state);

	// Read RAM size
	RAMSize = PrefsFindInt32("ramsize");
	if (RAMSize <= 1000) {
		RAMSize *= 1024 * 1024;
	}
	RAMSize &= 0xfff00000;	// Round down to 1MB boundary
	if (RAMSize < 1024*1024) {
		WarningAlert(GetString(STR_SMALL_RAM_WARN));
		RAMSize = 1024*1024;
	}
	if (RAMSize > 1023*1024*1024)						// Cap to 1023MB (APD crashes at 1GB)
		RAMSize = 1023*1024*1024;

#if REAL_ADDRESSING || DIRECT_ADDRESSING
	RAMSize = RAMSize & -getpagesize();					// Round down to page boundary
#endif
	
	// Initialize VM system
	vm_init();

#if REAL_ADDRESSING
	// Flag: RAM and ROM are contigously allocated from address 0
	bool memory_mapped_from_zero = false;

	// Make sure to map RAM & ROM at address 0 only on platforms that
	// supports linker scripts to relocate the Basilisk II executable
	// above 0x70000000
#if HAVE_LINKER_SCRIPT
	const bool can_map_all_memory = true;
#else
	const bool can_map_all_memory = false;
#endif

	// Try to allocate all memory from 0x0000, if it is not known to crash
	if (can_map_all_memory && (vm_acquire_mac_fixed(0, RAMSize + ROM_MAX_SIZE) == 0)) {
		D(bug("Could allocate RAM and ROM from 0x0000\n"));
		memory_mapped_from_zero = true;
	}
	
#ifndef PAGEZERO_HACK
	// Otherwise, just create the Low Memory area (0x0000..0x2000)
	else if (vm_acquire_mac_fixed(0, 0x2000) == 0) {
		D(bug("Could allocate the Low Memory globals\n"));
		lm_area_mapped = true;
	}
	
	// Exit on failure
	else {
		sprintf(str, GetString(STR_LOW_MEM_MMAP_ERR), strerror(errno));
		ErrorAlert(str);
		QuitEmulator();
	}
#endif
#endif /* REAL_ADDRESSING */

	// Create areas for Mac RAM and ROM
#if REAL_ADDRESSING
	if (memory_mapped_from_zero) {
		RAMBaseHost = (uint8 *)0;
		ROMBaseHost = RAMBaseHost + RAMSize;
	}
	else
#endif
	{
		uint8 *ram_rom_area = (uint8 *)vm_acquire_mac(RAMSize + ROM_MAX_SIZE + SCRATCH_MEM_SIZE);
		if (ram_rom_area == VM_MAP_FAILED) {
			ErrorAlert(STR_NO_MEM_ERR);
			QuitEmulator();
		}
		RAMBaseHost = ram_rom_area;
		ROMBaseHost = RAMBaseHost + RAMSize;
		ScratchMem = ROMBaseHost + ROM_MAX_SIZE + SCRATCH_MEM_SIZE / 2;
	}

#if REAL_ADDRESSING && USE_SCRATCHMEM_SUBTERFUGE
	// Allocate scratch memory
	ScratchMem = (uint8 *)vm_acquire_mac(SCRATCH_MEM_SIZE);
	if (ScratchMem == VM_MAP_FAILED) {
		ErrorAlert(STR_NO_MEM_ERR);
		QuitEmulator();
	}
	ScratchMem += SCRATCH_MEM_SIZE/2;	// ScratchMem points to middle of block
#endif

#if DIRECT_ADDRESSING
	// RAMBaseMac shall always be zero
	MEMBaseDiff = (uintptr)RAMBaseHost;
	RAMBaseMac = 0;
	ROMBaseMac = Host2MacAddr(ROMBaseHost);
#endif
#if REAL_ADDRESSING
	RAMBaseMac = Host2MacAddr(RAMBaseHost);
	ROMBaseMac = Host2MacAddr(ROMBaseHost);
#endif

#if SDL_PLATFORM_MACOS
	extern void set_current_directory();
	set_current_directory();
#endif

	// Get rom file path from preferences
	const char *rom_path = PrefsFindString("rom");

	// Load Mac ROM
	int rom_fd = open(rom_path ? rom_path : ROM_FILE_NAME, O_RDONLY);
	if (rom_fd < 0) {
		ErrorAlert(STR_NO_ROM_FILE_ERR);
		QuitEmulator();
	}
	printf("%s", GetString(STR_READING_ROM_FILE));
	ROMSize = lseek(rom_fd, 0, SEEK_END);
	if (ROMSize != 64*1024 && ROMSize != 128*1024 && ROMSize != 256*1024 && ROMSize != 512*1024 && ROMSize != 1024*1024) {
		ErrorAlert(STR_ROM_SIZE_ERR);
		close(rom_fd);
		QuitEmulator();
	}
	lseek(rom_fd, 0, SEEK_SET);
	if (read(rom_fd, ROMBaseHost, ROMSize) != (ssize_t)ROMSize) {
		ErrorAlert(STR_ROM_FILE_READ_ERR);
		close(rom_fd);
		QuitEmulator();
	}

#if !EMULATED_68K
	// Get CPU model
	int mib[2] = {CTL_HW, HW_MODEL};
	char *model;
	size_t model_len;
	sysctl(mib, 2, NULL, &model_len, NULL, 0);
	model = (char *)malloc(model_len);
	sysctl(mib, 2, model, &model_len, NULL, 0);
	D(bug("Model: %s\n", model));

	// Set CPU and FPU type
	CPUIs68060 = false;
	if (strstr(model, "020"))
		CPUType = 2;
	else if (strstr(model, "030"))
		CPUType = 3;
	else if (strstr(model, "040"))
		CPUType = 4;
	else if (strstr(model, "060")) {
		CPUType = 4;
		CPUIs68060 = true;
	} else {
		printf("WARNING: Cannot detect CPU type, assuming 68020\n");
		CPUType = 2;
	}
	FPUType = 1;	// NetBSD has an FPU emulation, so the FPU ought to be available at all times
	TwentyFourBitAddressing = false;
#endif

	// Initialize everything
	/*
	 * Also make the ROM visible where it lives on real hardware.
	 *
	 * This Quadra ROM holds absolute references to 0x40800000, the
	 * physical ROM base on mac68k (ROMBASE in machine/cpu.h), because
	 * that is where it sits on the machine it was written for -- which
	 * is the machine we are running on.  The low-memory ROMBase global
	 * is correct at 0x00800000 and the guest still reached 0x408266b4,
	 * so these are constants baked into the ROM, not a computed base.
	 *
	 * The two addresses must be the SAME memory, not two copies.  MacOS
	 * patches its ROM in place while starting up, and with independent
	 * copies a patch written through one address is invisible from the
	 * other, so code running from 0x40800000 would execute unpatched --
	 * which is what a boot that reaches the welcome screen and then
	 * wanders off looks like.
	 *
	 * So back both with one shared object: a temporary file, unlinked
	 * immediately, mapped MAP_SHARED at each address.
	 */
	{
		char tmpl[] = "/tmp/BasiliskII-rom.XXXXXX";
		int rfd = mkstemp(tmpl);

		if (rfd < 0)
			fprintf(stderr, "warning: no backing file for the "
			    "hardware ROM alias: %s\n", strerror(errno));
		else {
			(void)unlink(tmpl);
			if (ftruncate(rfd, ROMSize) < 0) {
				fprintf(stderr, "warning: ftruncate for ROM "
				    "alias: %s\n", strerror(errno));
				close(rfd);
			} else {
				void *lo, *hi;
				uint8 *keep = (uint8 *)malloc(ROMSize);

				/*
				 * The ROM has already been read into
				 * ROMBaseHost, and mapping over those pages
				 * discards them, so keep a copy across the
				 * remap and write it back afterwards.
				 */
				if (keep != NULL)
					memcpy(keep, ROMBaseHost, ROMSize);

				lo = mmap(ROMBaseHost, ROMSize,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_FIXED, rfd, 0);
				hi = mmap((void *)0x40800000, ROMSize,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_FIXED, rfd, 0);
				if (lo == MAP_FAILED || hi == MAP_FAILED ||
				    keep == NULL)
					fprintf(stderr, "warning: could not "
					    "alias ROM at 0x40800000: %s\n",
					    strerror(errno));
				else
					memcpy(lo, keep, ROMSize);
				free(keep);
				close(rfd);
			}
		}
	}

	if (!InitAll(vmdir))
		QuitEmulator();
	D(bug("Initialization complete\n"));

	D(bug("Mac RAM starts at %p (%08x)\n", RAMBaseHost, RAMBaseMac));
	D(bug("Mac ROM starts at %p (%08x)\n", ROMBaseHost, ROMBaseMac));

#if !EMULATED_68K
	// (Virtual) supervisor mode, disable interrupts
	EmulatedSR = 0x2700;

#ifdef HAVE_PTHREADS
	// Get handle of main thread
	emul_thread = pthread_self();
#endif

	// Create and install stack for signal handlers
	sig_stack = malloc(SIG_STACK_SIZE);
	D(bug("Signal stack at %p\n", sig_stack));
	if (sig_stack == NULL) {
		ErrorAlert(STR_NOT_ENOUGH_MEMORY_ERR);
		QuitEmulator();
	}
	stack_t new_stack;
	new_stack.ss_sp = sig_stack;
	new_stack.ss_flags = 0;
	new_stack.ss_size = SIG_STACK_SIZE;
	if (sigaltstack(&new_stack, NULL) < 0) {
		sprintf(str, GetString(STR_SIGALTSTACK_ERR), strerror(errno));
		ErrorAlert(str);
		QuitEmulator();
	}

	// Install SIGILL handler for emulating privileged instructions and
	// executing A-Trap and EMUL_OP opcodes
	sigemptyset(&sigill_sa.sa_mask);	// Block virtual 68k interrupts during SIGILL handling
	sigaddset(&sigill_sa.sa_mask, SIG_IRQ);
	sigaddset(&sigill_sa.sa_mask, SIGALRM);
	sigill_sa.sa_sigaction = sigill_handler;
	sigill_sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
	if (sigaction(SIGILL, &sigill_sa, NULL) < 0) {
		sprintf(str, GetString(STR_SIG_INSTALL_ERR), "SIGILL", strerror(errno));
		ErrorAlert(str);
		QuitEmulator();
	}

	// Install virtual 68k interrupt signal handler
	sigemptyset(&sigirq_sa.sa_mask);
	sigaddset(&sigirq_sa.sa_mask, SIGALRM);
	sigirq_sa.sa_sigaction = sigirq_handler;
	sigirq_sa.sa_flags = SA_ONSTACK | SA_RESTART | SA_SIGINFO;
	if (sigaction(SIG_IRQ, &sigirq_sa, NULL) < 0) {
		sprintf(str, GetString(STR_SIG_INSTALL_ERR), "SIG_IRQ", strerror(errno));
		ErrorAlert(str);
		QuitEmulator();
	}
#endif

#ifdef ENABLE_MON
	// Setup SIGINT handler to enter mon
	/* TEMPORARY: report a fatal signal instead of vanishing */
	{
		struct sigaction q;

		memset(&q, 0, sizeof(q));
		sigemptyset(&q.sa_mask);
		q.sa_handler = report_fatal_signal;
		q.sa_flags = 0;
		sigaction(SIGTERM, &q, NULL);
		sigaction(SIGHUP, &q, NULL);
		sigaction(SIGQUIT, &q, NULL);
		sigaction(SIGABRT, &q, NULL);
		sigaction(SIGPIPE, &q, NULL);
	}

	sigemptyset(&sigint_sa.sa_mask);
	sigint_sa.sa_handler = (void (*)(int))sigint_handler;
	sigint_sa.sa_flags = 0;
	sigaction(SIGINT, &sigint_sa, NULL);
#endif

#ifndef USE_CPU_EMUL_SERVICES
#if defined(HAVE_PTHREADS)

	// POSIX threads available, start 60Hz thread
	Set_pthread_attr(&tick_thread_attr, 0);
	tick_thread_active = (pthread_create(&tick_thread, &tick_thread_attr, tick_func, NULL) == 0);
	if (!tick_thread_active) {
		sprintf(str, GetString(STR_TICK_THREAD_ERR), strerror(errno));
		ErrorAlert(str);
		QuitEmulator();
	}
	D(bug("60Hz thread started\n"));

#elif defined(HAVE_TIMER_CREATE) && defined(_POSIX_REALTIME_SIGNALS)

	// POSIX.4 timers and real-time signals available, start 60Hz timer
	sigemptyset(&timer_sa.sa_mask);
	timer_sa.sa_sigaction = (void (*)(int, siginfo_t *, void *))one_tick;
	timer_sa.sa_flags = SA_SIGINFO | SA_RESTART;
	if (sigaction(SIG_TIMER, &timer_sa, NULL) < 0) {
		sprintf(str, GetString(STR_SIG_INSTALL_ERR), "SIG_TIMER", strerror(errno));
		ErrorAlert(str);
		QuitEmulator();
	}
	struct sigevent timer_event;
	timer_event.sigev_notify = SIGEV_SIGNAL;
	timer_event.sigev_signo = SIG_TIMER;
	if (timer_create(CLOCK_REALTIME, &timer_event, &timer) < 0) {
		sprintf(str, GetString(STR_TIMER_CREATE_ERR), strerror(errno));
		ErrorAlert(str);
		QuitEmulator();
	}
	struct itimerspec req;
	req.it_value.tv_sec = 0;
	req.it_value.tv_nsec = 16625000;
	req.it_interval.tv_sec = 0;
	req.it_interval.tv_nsec = 16625000;
	if (timer_settime(timer, 0, &req, NULL) < 0) {
		sprintf(str, GetString(STR_TIMER_SETTIME_ERR), strerror(errno));
		ErrorAlert(str);
		QuitEmulator();
	}
	D(bug("60Hz timer started\n"));

#else

	// Start 60Hz timer
	sigemptyset(&timer_sa.sa_mask);		// Block virtual 68k interrupts during SIGARLM handling
#if !EMULATED_68K
	sigaddset(&timer_sa.sa_mask, SIG_IRQ);
#endif
	timer_sa.sa_handler = one_tick;
	timer_sa.sa_flags = SA_ONSTACK | SA_RESTART;
	if (sigaction(SIGALRM, &timer_sa, NULL) < 0) {
		sprintf(str, GetString(STR_SIG_INSTALL_ERR), "SIGALRM", strerror(errno));
		ErrorAlert(str);
		QuitEmulator();
	}
	struct itimerval req;
	req.it_interval.tv_sec = req.it_value.tv_sec = 0;
	req.it_interval.tv_usec = req.it_value.tv_usec = 16625;
	setitimer(ITIMER_REAL, &req, NULL);

#endif
#endif

#ifdef USE_PTHREADS_SERVICES
	// Start XPRAM watchdog thread
	memcpy(last_xpram, XPRAM, XPRAM_SIZE);
	xpram_thread_active = (pthread_create(&xpram_thread, NULL, xpram_func, NULL) == 0);
	D(bug("XPRAM thread started\n"));
#endif

#if defined(__NetBSD__) && defined(__m68k__)
	/*
	 * Register this thread with the bfast kernel module, if loaded.
	 * From here on the module may fast-path our privilege-violation
	 * and A-line traps at trap level instead of leaving them to the
	 * signal path.  Must be done from THIS thread: the module gates
	 * on the calling lwp's pcb, and this is the thread about to run
	 * 68k code.  Absence of the module is not an error.
	 */
	{
		extern uint16 EmulatedSR;
		extern uint32 InterruptFlags;
		int one = 1;
		int a1 = (int)(uintptr_t)&EmulatedSR;
		int a2 = (int)(uintptr_t)&InterruptFlags;

		if (sysctlbyname("kern.bfast.uaddr_emulsr", NULL, NULL,
		        &a1, sizeof(a1)) == 0 &&
		    sysctlbyname("kern.bfast.uaddr_intflags", NULL, NULL,
		        &a2, sizeof(a2)) == 0 &&
		    sysctlbyname("kern.bfast.attach", NULL, NULL,
		        &one, sizeof(one)) == 0)
			printf("bfast: kernel fast path attached\n");
	}
#endif

#if defined(__NetBSD__) && defined(__m68k__)
	/*
	 * BII_SHIFT_BOOT=1 holds the Shift key down from before the guest
	 * starts until a few seconds in, which is how a Mac is told to boot
	 * with extensions disabled.  Done through the ADB layer rather than
	 * through X because the emulator grabs the keyboard, and because a
	 * physical hand on the keyboard is not available to a script.
	 * 0x38 is the Mac keycode for Shift.
	 */
	if (getenv("BII_SHIFT_BOOT") != NULL) {
		extern bool bii_shift_boot;
		bii_shift_boot = true;
		ADBKeyDown(0x38);
		fprintf(stderr, "SHIFT held for boot (extensions off)\n");
	}

	/*
	 * TEMPORARY, diagnostic: BII_NO_TICK=1 suppresses the 60Hz tick.
	 * The crash hunt has an unavoidable blind spot -- sigirq_handler
	 * must strip T1 before setcontext sees the context, so the window
	 * between an interrupt and the next privileged instruction cannot
	 * be single-stepped, and the fault falls inside it.  Removing the
	 * tick removes the blind spot: if the fault persists the interrupt
	 * is irrelevant and the trace covers everything; if it disappears
	 * the interrupt is causal.  Either answer is decisive.
	 */
	if (getenv("BII_NO_TICK") != NULL) {
		extern bool tick_inhibit;
		tick_inhibit = true;
		fprintf(stderr, "60Hz tick INHIBITED (diagnostic)\n");
	}
#endif

	// Start 68k and jump to ROM boot routine
	D(bug("Starting emulation...\n"));
	Start680x0();

	QuitEmulator();
	return 0;
}


/*
 *  Quit emulator
 */

void QuitEmulator(void)
{
	fprintf(stderr, "QuitEmulator called\n");

#if defined(__NetBSD__) && defined(__m68k__)
	// Detach from the bfast module; harmless if never attached.
	{
		int zero = 0;
		sysctlbyname("kern.bfast.attach", NULL, NULL,
		    &zero, sizeof(zero));
	}
#endif
	D(bug("QuitEmulator\n"));

#if EMULATED_68K
	// Exit 680x0 emulation
	Exit680x0();
#endif

#if defined(USE_CPU_EMUL_SERVICES)
	// Show statistics
	uint64 emulated_ticks_end = GetTicks_usec();
	D(bug("%ld ticks in %ld usec = %f ticks/sec [%ld tick checks]\n",
		  (long)emulated_ticks_count, (long)(emulated_ticks_end - emulated_ticks_start),
		  emulated_ticks_count * 1000000.0 / (emulated_ticks_end - emulated_ticks_start), (long)n_check_ticks));
#elif defined(USE_PTHREADS_SERVICES)
	// Stop 60Hz thread
	if (tick_thread_active) {
		tick_thread_cancel = true;
#ifdef HAVE_PTHREAD_CANCEL
		pthread_cancel(tick_thread);
#endif
		pthread_join(tick_thread, NULL);
	}
#elif defined(HAVE_TIMER_CREATE) && defined(_POSIX_REALTIME_SIGNALS)
	// Stop 60Hz timer
	timer_delete(timer);
#else
	struct itimerval req;
	req.it_interval.tv_sec = req.it_value.tv_sec = 0;
	req.it_interval.tv_usec = req.it_value.tv_usec = 0;
	setitimer(ITIMER_REAL, &req, NULL);
#endif

#ifdef USE_PTHREADS_SERVICES
	// Stop XPRAM watchdog thread
	if (xpram_thread_active) {
		xpram_thread_cancel = true;
#ifdef HAVE_PTHREAD_CANCEL
		pthread_cancel(xpram_thread);
#endif
		pthread_join(xpram_thread, NULL);
	}
#endif

	// Deinitialize everything
	ExitAll();

	// Free ROM/RAM areas
	if (RAMBaseHost != VM_MAP_FAILED) {
		vm_release(RAMBaseHost, RAMSize + ROM_MAX_SIZE + SCRATCH_MEM_SIZE);
		RAMBaseHost = NULL;
		ROMBaseHost = NULL;
	}

#if REAL_ADDRESSING && USE_SCRATCHMEM_SUBTERFUGE
	// Delete scratch memory area
	if (ScratchMem != (uint8 *)VM_MAP_FAILED) {
		vm_release((void *)(ScratchMem - SCRATCH_MEM_SIZE/2), SCRATCH_MEM_SIZE);
		ScratchMem = NULL;
	}
#endif

#if REAL_ADDRESSING
	// Delete Low Memory area
	if (lm_area_mapped)
		vm_release(0, 0x2000);
#endif
	
	// Exit VM wrappers
	vm_exit();

	// Exit system routines
	SysExit();

	// Exit preferences
	PrefsExit();

	// Close X11 server connection
#ifndef USE_SDL_VIDEO
	if (x_display)
		XCloseDisplay(x_display);
#endif

	// Notify GUI we are about to leave
	if (gui_connection) {
		if (rpc_method_invoke(gui_connection, RPC_METHOD_EXIT, RPC_TYPE_INVALID) == RPC_ERROR_NO_ERROR)
			rpc_method_wait_for_reply(gui_connection, RPC_TYPE_INVALID);
	}

	exit(0);
}


/*
 *  Code was patched, flush caches if necessary (i.e. when using a real 680x0
 *  or a dynamically recompiling emulator)
 */

void FlushCodeCache(void *start, uint32 size)
{
#if USE_JIT
    if (UseJIT)
#ifdef UPDATE_UAE
		flush_icache();
#else
		flush_icache_range((uint8 *)start, size);
#endif
#endif
#if !EMULATED_68K && defined(__NetBSD__)
	m68k_sync_icache(start, size);
#endif
}


/*
 *  SIGINT handler, enters mon
 */

#ifdef ENABLE_MON
static void sigint_handler(...)
{
#if EMULATED_68K
	uaecptr nextpc;
#ifdef UPDATE_UAE
	extern void m68k_dumpstate(FILE *, uaecptr *nextpc);
	m68k_dumpstate(stderr, &nextpc);
#else
	extern void m68k_dumpstate(uaecptr *nextpc);
	m68k_dumpstate(&nextpc);
#endif
#endif
	VideoQuitFullScreen();
	const char *arg[4] = {"mon", "-m", "-r", NULL};
	mon(3, arg);
	QuitEmulator();
}
#endif


#ifdef HAVE_PTHREADS
/*
 *  Pthread configuration
 */

void Set_pthread_attr(pthread_attr_t *attr, int priority)
{
	pthread_attr_init(attr);
#if defined(_POSIX_THREAD_PRIORITY_SCHEDULING)
	// Some of these only work for superuser
	if (geteuid() == 0) {
		pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
		pthread_attr_setschedpolicy(attr, SCHED_FIFO);
		struct sched_param fifo_param;
		fifo_param.sched_priority = ((sched_get_priority_min(SCHED_FIFO) + 
					      sched_get_priority_max(SCHED_FIFO)) / 2 +
					     priority);
		pthread_attr_setschedparam(attr, &fifo_param);
	}
	if (pthread_attr_setscope(attr, PTHREAD_SCOPE_SYSTEM) != 0) {
#ifdef PTHREAD_SCOPE_BOUND_NP
	    // If system scope is not available (eg. we're not running
	    // with CAP_SCHED_MGT capability on an SGI box), try bound
	    // scope.  It exposes pthread scheduling to the kernel,
	    // without setting realtime priority.
	    pthread_attr_setscope(attr, PTHREAD_SCOPE_BOUND_NP);
#endif
	}
#endif
}
#endif // HAVE_PTHREADS


/*
 *  Mutexes
 */

#ifdef HAVE_PTHREADS

struct B2_mutex {
	B2_mutex() { 
	    pthread_mutexattr_t attr;
	    pthread_mutexattr_init(&attr);
	    // Initialize the mutex for priority inheritance --
	    // required for accurate timing.
#if defined(HAVE_PTHREAD_MUTEXATTR_SETPROTOCOL) && !defined(__CYGWIN__)
	    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
#endif
#if defined(HAVE_PTHREAD_MUTEXATTR_SETTYPE) && defined(PTHREAD_MUTEX_NORMAL)
	    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
#endif
#ifdef HAVE_PTHREAD_MUTEXATTR_SETPSHARED
	    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE);
#endif
	    pthread_mutex_init(&m, &attr);
	    pthread_mutexattr_destroy(&attr);
	}
	~B2_mutex() { 
	    pthread_mutex_trylock(&m); // Make sure it's locked before
	    pthread_mutex_unlock(&m);  // unlocking it.
	    pthread_mutex_destroy(&m);
	}
	pthread_mutex_t m;
};

B2_mutex *B2_create_mutex(void)
{
	return new B2_mutex;
}

void B2_lock_mutex(B2_mutex *mutex)
{
	pthread_mutex_lock(&mutex->m);
}

void B2_unlock_mutex(B2_mutex *mutex)
{
	pthread_mutex_unlock(&mutex->m);
}

void B2_delete_mutex(B2_mutex *mutex)
{
	delete mutex;
}

#else

struct B2_mutex {
	int dummy;
};

B2_mutex *B2_create_mutex(void)
{
	return new B2_mutex;
}

void B2_lock_mutex(B2_mutex *mutex)
{
}

void B2_unlock_mutex(B2_mutex *mutex)
{
}

void B2_delete_mutex(B2_mutex *mutex)
{
	delete mutex;
}

#endif


/*
 *  Interrupt flags (must be handled atomically!)
 */

uint32 InterruptFlags = 0;

#if EMULATED_68K
void SetInterruptFlag(uint32 flag)
{
	LOCK_INTFLAGS;
	InterruptFlags |= flag;
	UNLOCK_INTFLAGS;
}

void ClearInterruptFlag(uint32 flag)
{
	LOCK_INTFLAGS;
	InterruptFlags &= ~flag;
	UNLOCK_INTFLAGS;
}
#endif

#if !EMULATED_68K
void TriggerInterrupt(void)
{
#if defined(HAVE_PTHREADS)
	pthread_kill(emul_thread, SIG_IRQ);
#else
	raise(SIG_IRQ);
#endif
}

void TriggerNMI(void)
{
	// not yet supported
}
#endif


/*
 *  XPRAM watchdog thread (saves XPRAM every minute)
 */

static void xpram_watchdog(void)
{
	if (memcmp(last_xpram, XPRAM, XPRAM_SIZE)) {
		memcpy(last_xpram, XPRAM, XPRAM_SIZE);
		SaveXPRAM();
	}
}

#ifdef USE_PTHREADS_SERVICES
static void *xpram_func(void *arg)
{
	while (!xpram_thread_cancel) {
		for (int i=0; i<60 && !xpram_thread_cancel; i++)
			Delay_usec(999999);		// Only wait 1 second so we quit promptly when xpram_thread_cancel becomes true
		xpram_watchdog();
	}
	return NULL;
}
#endif


/*
 *  60Hz thread (really 60.15Hz)
 */

static void one_second(void)
{
	// Pseudo Mac 1Hz interrupt, update local time
	WriteMacInt32(0x20c, TimerDateTime());

	SetInterruptFlag(INTFLAG_1HZ);
	TriggerInterrupt();

#ifndef USE_PTHREADS_SERVICES
	static int second_counter = 0;
	if (++second_counter > 60) {
		second_counter = 0;
		xpram_watchdog();
	}
#endif
}

static void one_tick(...)
{
	static int tick_counter = 0;
	if (++tick_counter > 60) {
		tick_counter = 0;
		one_second();
	}

#ifndef USE_PTHREADS_SERVICES
	// Threads not used to trigger interrupts, perform video refresh from here
	VideoRefresh();
#endif

#ifndef HAVE_PTHREADS
	// No threads available, perform networking from here
	SetInterruptFlag(INTFLAG_ETHER);
#endif

	// Trigger 60Hz interrupt
	if (ROMVersion != ROM_VERSION_CLASSIC || HasMacStarted()) {
		SetInterruptFlag(INTFLAG_60HZ);
		TriggerInterrupt();
	}
}

#ifdef USE_PTHREADS_SERVICES
bool tick_inhibit;
bool bii_shift_boot;
static long bii_shift_ticks;
static void *tick_func(void *arg)
{
	uint64 start = GetTicks_usec();
	int64 ticks = 0;
	uint64 next = start;
	while (!tick_thread_cancel) {
		if (!tick_inhibit)
			one_tick();
		/*
		 * Hold Shift for the extensions-off boot.
		 *
		 * Asserting it once before Start680x0 is not enough: the
		 * guest initialises ADB during early boot and the key state
		 * does not survive that.  So re-assert on every tick until
		 * well past the point where MacOS samples the keyboard to
		 * decide whether to load extensions, then release.
		 */
		if (bii_shift_boot) {
			if (++bii_shift_ticks > 60 * 25) {
				bii_shift_boot = false;
				ADBKeyUp(0x38);
				fprintf(stderr, "SHIFT released after %ld "
				    "ticks\n", bii_shift_ticks);
			} else
				ADBKeyDown(0x38);
		}
		next += 16625;
		int64 delay = next - GetTicks_usec();
		if (delay > 0)
			Delay_usec(delay);
		else if (delay < -16625)
			next = GetTicks_usec();
		ticks++;
	}
#if DEBUG
	uint64 end = GetTicks_usec();
	D(bug("%lld ticks in %lld usec = %f ticks/sec\n", ticks, end - start, ticks * 1000000.0 / (end - start)));
#endif
	return NULL;
}
#endif


#if !EMULATED_68K
/*
 *  Virtual 68k interrupt handler
 */

unsigned long bf_t1_stripped;	/* T1 bits scrubbed before setcontext */

/* Recent-trap ring; dumped by sigsegv_dump_state after a wild jump. */
#define BF_RING 24
struct bf_ring_ent { uint32 pc; uint32 a7; uint32 a2; uint32 a3; uint16 op; };
static struct bf_ring_ent bf_ring[BF_RING];
static unsigned bf_ring_i;

/* Not a real opcode: marks an event rather than a trap. */
#define BF_MARK_IRQ	0xffff

static inline void
bf_ring_add(uint32 pc, uint32 a7, uint32 a2, uint32 a3, uint16 op)
{
	struct bf_ring_ent *e = &bf_ring[bf_ring_i & (BF_RING - 1)];

	e->pc = pc; e->a7 = a7; e->a2 = a2; e->a3 = a3; e->op = op;
	bf_ring_i++;
}

void bf_dump_ring(void);
void
bf_dump_ring(void)
{
	unsigned k;

#if defined(__NetBSD__) && defined(__m68k__)
	{
		extern unsigned long bf_segv_regs[18];
		extern int bf_segv_have_regs;
		static const char *nm[18] = {
		    "d0","d1","d2","d3","d4","d5","d6","d7",
		    "a0","a1","a2","a3","a4","a5","a6","a7","pc","ps" };
		int i;

		if (bf_segv_have_regs) {
			fprintf(stderr, "registers at the fault:\n");
			for (i = 0; i < 18; i++) {
				unsigned long v = bf_segv_regs[i];

				fprintf(stderr, "  %-3s %08lx%s\n", nm[i], v,
				    (v == 0x09fc0000UL) ? "   <== the "
				        "fault address" :
				    ((v >> 16) >= 0x0900 && (v >> 16) < 0x0a00)
				        ? "   Fixed ~2500" : "");
			}
		}
	}
#endif

	/*
	 * The kernel single-step ring, if bfast was tracing.  Entries wrap;
	 * the newest is trace_n-1.  The last entry is the instruction that
	 * computed the bad PC: the wild jump's fetch faulted before any
	 * trace exception could log it.
	 */
	{
		extern unsigned long bf_t1_stripped;
		static struct { uint32 pc, a2, fp, usp; } ring[32768];
		size_t rlen = sizeof(ring);
		int n = 0;
		size_t nlen = sizeof(n);

		if (sysctlbyname("kern.bfast.trace_n", &n, &nlen, NULL, 0) == 0 &&
		    n > 0 &&
		    sysctlbyname("kern.bfast.trace_ring", ring, &rlen,
		        NULL, 0) == 0) {
			int show = n < 300 ? n : 300;
			int k;

			fprintf(stderr, "kernel trace ring: %d logged, "
			    "T1 stripped %lu times, last %d:\n",
			    n, bf_t1_stripped, show);
			for (k = n - show; k < n; k++) {
				unsigned j = (unsigned)k & 32767;

				fprintf(stderr, "  pc=%08x a2=%08x fp=%08x "
				    "usp=%08x\n", ring[j].pc, ring[j].a2,
				    ring[j].fp, ring[j].usp);
			}
		}
	}

	/*
	 * The RAM hook around 0x00010f2a.  The watchpoint proved the
	 * instruction at 0x00010f3a writes the value the fatal rts later
	 * pops, and this is RAM, so it cannot be read from the ROM image --
	 * dump it here for disassembly.
	 */
	{
		uint32 a;

		fprintf(stderr, "RAM hook 0x00010f20..0x00010f60:\n");
		for (a = 0x00010f20; a < 0x00010f60; a += 8)
			fprintf(stderr, "  %08x: %04x %04x %04x %04x\n", a,
			    ReadMacInt16(a), ReadMacInt16(a + 2),
			    ReadMacInt16(a + 4), ReadMacInt16(a + 6));

		/*
		 * The A815 handler.  The dispatcher resolves _SCSIDispatch
		 * to 0x0000f100 -- a RAM patch, not Basilisk's ROM
		 * replacement -- which is why selector 4 never reaches our
		 * emulation.  This is the code that services it.
		 */
		fprintf(stderr, "A815 handler 0x0000f100..0x0000f180:\n");
		for (a = 0x0000f100; a < 0x0000f180; a += 8)
			fprintf(stderr, "  %08x: %04x %04x %04x %04x\n", a,
			    ReadMacInt16(a), ReadMacInt16(a + 2),
			    ReadMacInt16(a + 4), ReadMacInt16(a + 6));
	}

	/*
	 * The guest's trap dispatch tables.
	 *
	 * Addresses come from disassembling the ROM's own A-line handler at
	 * 0x008099b0: the Toolbox path is
	 *     movel @(0x0e00,%d2:w:4),%sp@(8)
	 * and the OS path is
	 *     jsr @(0x0400,%d2:w:4)@(0)
	 * so Toolbox entries live at 0x0E00 + trap*4 and OS entries at
	 * 0x0400 + trap*4.  If the entry for the trap we died on holds the
	 * bad value, the table is the fault; if it holds a sane ROM address,
	 * the dispatch was fine and the damage is further in.
	 */
	{
		static const uint16 interesting[] = { 0xa868, 0xa869, 0xa9a0 };
		unsigned t, bad = 0;

		for (t = 0; t < sizeof(interesting)/sizeof(interesting[0]); t++) {
			uint16 tr = interesting[t];
			uint32 idx = tr & 0x03ff;
			uint32 ent = 0x0e00 + idx * 4;

			fprintf(stderr, "toolbox trap %04x -> [%04x] = %08x\n",
			    tr, (unsigned)ent, ReadMacInt32(ent));
		}
		fprintf(stderr, "scanning the toolbox table for non-code "
		    "entries:\n");
		for (t = 0; t < 1024; t++) {
			uint32 v = ReadMacInt32(0x0e00 + t * 4);

			if (v == 0 || v >= 0x10000000 ||
			    (v >= 0x00900000 && v < 0x40800000)) {
				if (bad++ < 12)
					fprintf(stderr, "  [%04x] trap a%03x "
					    "= %08x\n",
					    (unsigned)(0x0e00 + t * 4),
					    (unsigned)(0x800 + t), v);
			}
		}
		fprintf(stderr, "  %u suspect entries of 1024\n", bad);
	}

	/*
	 * Guest stack at the fault.  The wild jump left a return address
	 * behind if it was a jsr/bsr, and the dispatcher's frame is here
	 * too; anything in ROM (0x40800000/0x00800000 alias) or low RAM is
	 * flagged as a plausible code address.
	 */
	if (bf_ring_i != 0) {
		uint32 a7 = bf_ring[(bf_ring_i - 1) & (BF_RING - 1)].a7;
		unsigned w;

		fprintf(stderr, "guest stack at %08x:\n", a7);
		for (w = 0; w < 24; w++) {
			uint32 at = a7 + w * 4;
			uint32 v;

			if (at < 0x1000 || at >= 0x800000)
				break;
			v = ReadMacInt32(at);
			fprintf(stderr, "  %08x: %08x%s\n", at, v,
			    (v >= 0x40800000 && v < 0x40900000) ? "  ROM(alias)" :
			    (v >= 0x00800000 && v < 0x00900000) ? "  ROM" :
			    (v >= 0x00001000 && v < 0x00800000) ? "  RAM" : "");
		}
	}

	fprintf(stderr, "last %d traps before the fault, oldest first:\n",
	    BF_RING);
	for (k = 0; k < BF_RING; k++) {
		unsigned j = (bf_ring_i + k) & (BF_RING - 1);

		if (bf_ring[j].pc == 0)
			continue;
		if (bf_ring[j].op == BF_MARK_IRQ)
			fprintf(stderr, "  pc=%08x  *** INTERRUPT *** "
			    "a7=%08x a2=%08x a3=%08x\n",
			    bf_ring[j].pc, bf_ring[j].a7, bf_ring[j].a2,
			    bf_ring[j].a3);
		else
			fprintf(stderr, "  pc=%08x op=%04x a7=%08x a2=%08x "
			    "a3=%08x%s\n",
			    bf_ring[j].pc, bf_ring[j].op, bf_ring[j].a7,
			    bf_ring[j].a2, bf_ring[j].a3,
			    (bf_ring[j].op & 0xf000) == 0xa000 ? "  A-line" :
			    (bf_ring[j].op & 0xff00) == 0x7100 ? "  EMUL_OP" : "");
	}
}

static void sigirq_handler(int sig, siginfo_t *sip, void *uap)
{
	/*
	 * NetBSD stopped exposing struct sigcontext to userland (it is now
	 * guarded by _LIBC || _KERNEL in m68k/signal.h), so the interrupted
	 * 68k state comes from the ucontext instead.  m68k/mcontext.h lays
	 * __gregs out as d0-d7 then a0-a7 then PC and PS, which is exactly
	 * the order M68kRegisters expects, so regs can point straight at it.
	 *
	 * Note a7 and the stack pointer are now the same storage rather than
	 * two fields that had to be kept in step; assigning both is harmless
	 * and left alone to keep this diff readable.
	 */
	ucontext_t *ucp = (ucontext_t *)uap;
	__greg_t *gr = ucp->uc_mcontext.__gregs;
	__greg_t &sc_pc = gr[_REG_PC];
	__greg_t &sc_ps = gr[_REG_PS];
	__greg_t &sc_sp = gr[_REG_A7];
	M68kRegisters *regs = (M68kRegisters *)&gr[_REG_D0];

	/*
	 * If bfast armed single-step tracing, the interrupted context can
	 * carry the T1 bit -- which is in PSL_MBZ, so handing it back to
	 * setcontext() would EINVAL and kill the process (the exit(22)
	 * mechanism).  Strip it; tracing resumes at the next armed window.
	 */
	if (__builtin_expect(sc_ps & 0x8000, 0)) {
		extern unsigned long bf_t1_stripped;
		static int one = 1;

		sc_ps &= ~0x8000;
		bf_t1_stripped++;
		/*
		 * We had to drop T1 (setcontext rejects it), which silently
		 * ends the kernel trace.  Ask bfast to re-arm at the next
		 * fast-pathed privileged op so the window continues.
		 */
		sysctlbyname("kern.bfast.arm_now", NULL, NULL,
		    &one, sizeof(one));
	}

	/*
	 * If bfast armed single-step tracing, the interrupted context can
	 * carry the T1 bit -- which is in PSL_MBZ, so handing it back to
	 * setcontext() would EINVAL and kill the process (the exit(22)
	 * mechanism).  Strip it; tracing resumes at the next armed window.
	 */
	if (__builtin_expect(sc_ps & 0x8000, 0)) {
		extern unsigned long bf_t1_stripped;
		static int one = 1;

		sc_ps &= ~0x8000;
		bf_t1_stripped++;
		/*
		 * We had to drop T1 (setcontext rejects it), which silently
		 * ends the kernel trace.  Ask bfast to re-arm at the next
		 * fast-pathed privileged op so the window continues.
		 */
		sysctlbyname("kern.bfast.arm_now", NULL, NULL,
		    &one, sizeof(one));
	}

	// Interrupts disabled? Then do nothing
	if (EmulatedSR & 0x0700)
		return;

	/*
	 * Only interrupt the GUEST.  This signal arrives on a timer and can
	 * land anywhere -- inside the emulator's own code, inside libc, or
	 * in another thread entirely -- and the EmulatedSR mask above only
	 * covers EmulOp.  Injecting a Mac interrupt frame in those cases
	 * captures a HOST pc as though it were guest code; MacOS later
	 * RTEs to it and jumps into nowhere.
	 *
	 * Observed exactly that: a frame with a plausible SR (0x2010, which
	 * is sc_ps|EmulatedSR from right here) and pc=0x0c51c5a0, an address
	 * on the host heap between our text at 0x08000000 and the mapping
	 * arena at 0x10000000.
	 *
	 * Guest code lives in RAM from 0 and ROM immediately above it, so
	 * anything outside that is not ours to interrupt.  Dropping the tick
	 * is harmless: another arrives in 1/60s.
	 */
	static unsigned long irq_deferred, irq_delivered;

	if ((uint32)sc_pc >= RAMSize + ROM_MAX_SIZE) {
		/*
		 * Not in guest code, so a frame built here would capture a
		 * host pc.  Leave the interrupt PENDING rather than losing
		 * it: InterruptFlags stays set and EmulOpTrampoline
		 * re-triggers on the way back out of native code.
		 */
		irq_deferred++;
		if ((irq_deferred % 200) == 1)
			fprintf(stderr, "irq: deferred=%lu delivered=%lu\n",
			    irq_deferred, irq_delivered);
		return;
	}
	irq_delivered++;
	if ((irq_delivered % 200) == 1)
		fprintf(stderr, "irq: deferred=%lu delivered=%lu\n",
		    irq_deferred, irq_delivered);


	// Set up interrupt frame on stack
	uint32 a7 = regs->a[7];
	a7 -= 2;
	WriteMacInt16(a7, 0x64);
	a7 -= 4;
	WriteMacInt32(a7, sc_pc);
	a7 -= 2;
	WriteMacInt16(a7, sc_ps | EmulatedSR);
	sc_sp = regs->a[7] = a7;

	// Set interrupt level
	EmulatedSR |= 0x2100;

	/* Mark delivery so a2/a3 can be compared across the interrupt. */
	bf_ring_add((uint32)sc_pc, (uint32)sc_sp, (uint32)gr[_REG_A0 + 2],
	    (uint32)gr[_REG_A0 + 3], BF_MARK_IRQ);

	/*
	 * Ask bfast to arm single-step tracing at the next fast-pathed
	 * privileged op -- the IRQ glue's own SR writes, moments from now.
	 * The T1 bit cannot be set here: it is in PSL_MBZ and setcontext
	 * would refuse the context.  Absent the module this simply fails.
	 */
	{
		static int one = 1;
		sysctlbyname("kern.bfast.arm_now", NULL, NULL,
		    &one, sizeof(one));
	}

	// Jump to MacOS interrupt handler on return
	sc_pc = ReadMacInt32(0x64);
	/*
	 * Every path through this handler converges here, so what __gregs
	 * holds now is exactly what setcontext() will be asked to resume.
	 * A context it refuses is not a signal: setcontext() returns EINVAL
	 * inside libc's signal trampoline and libc exits with that errno,
	 * with no message and no core.  That failure mode is invisible
	 * enough to be worth two instructions per trap to catch.
	 */
	{
		uint32 p = (uint32)sc_pc, t = (uint32)sc_ps;

		if ((t & 0xffff7fe0u) != 0)   /* PSL_MBZ|PSL_IPL|PSL_S */
			fprintf(stderr, "%s: SUSPECT CONTEXT pc=%08x ps=%08x "
			    "a7=%08x\n", "sigirq", (unsigned)p, (unsigned)t,
			    (unsigned)sc_sp);
	}

}


/*
 *  SIGILL handler, for emulation of privileged instructions and executing
 *  A-Trap and EMUL_OP opcodes
 */

static void sigill_handler(int sig, siginfo_t *sip, void *uap)
{
	/*
	 * NetBSD stopped exposing struct sigcontext to userland (it is now
	 * guarded by _LIBC || _KERNEL in m68k/signal.h), so the interrupted
	 * 68k state comes from the ucontext instead.  m68k/mcontext.h lays
	 * __gregs out as d0-d7 then a0-a7 then PC and PS, which is exactly
	 * the order M68kRegisters expects, so regs can point straight at it.
	 *
	 * Note a7 and the stack pointer are now the same storage rather than
	 * two fields that had to be kept in step; assigning both is harmless
	 * and left alone to keep this diff readable.
	 */
	ucontext_t *ucp = (ucontext_t *)uap;
	__greg_t *gr = ucp->uc_mcontext.__gregs;
	__greg_t &sc_pc = gr[_REG_PC];
	__greg_t &sc_ps = gr[_REG_PS];
	__greg_t &sc_sp = gr[_REG_A7];
	M68kRegisters *regs = (M68kRegisters *)&gr[_REG_D0];

	uint16 *pc = (uint16 *)sc_pc;
	uint16 opcode = *pc;

	/*
	 * Last-N trap ring, for post-mortem after a wild jump.  No I/O on
	 * the hot path: two stores and a mask.
	 */
	bf_ring_add((uint32)sc_pc, (uint32)sc_sp, (uint32)gr[_REG_A0 + 2],
	    (uint32)gr[_REG_A0 + 3], opcode);

	/*
	 * Opcode histogram.
	 *
	 * Design input for an in-kernel fast path: which operations must it
	 * cover, and how much of the load is EMUL_OP -- the class that has
	 * to keep crossing into userland and so bounds any speedup.  One
	 * counter per opcode word (256KB, nothing on this machine), dumped
	 * every 100000 traps so no manner of exit can lose it.
	 */
	{
		static uint32 op_hist[65536];
		static unsigned long n;

		op_hist[opcode]++;
		if ((++n % 100000) == 0) {
			uint32 top[16]; int ti, tn = 0, i2;
			unsigned long aline = 0, emulop = 0, other = 0;
			unsigned op;

			for (op = 0; op < 65536; op++) {
				uint32 c = op_hist[op];
				if (c == 0)
					continue;
				if ((op & 0xf000) == 0xa000)
					aline += c;
				else if ((op & 0xff00) == 0x7100)
					emulop += c;
				else
					other += c;
				for (ti = 0; ti < tn; ti++)
					if (c > op_hist[top[ti]])
						break;
				if (ti < 16) {
					for (i2 = (tn < 16 ? tn : 15);
					    i2 > ti; i2--)
						top[i2] = top[i2 - 1];
					top[ti] = op;
					if (tn < 16)
						tn++;
				}
			}
			fprintf(stderr, "hist %lu: a-line %lu (%lu%%)  "
			    "emulop %lu (%lu%%)  other %lu (%lu%%)\n",
			    n, aline, aline * 100 / n,
			    emulop, emulop * 100 / n,
			    other, other * 100 / n);
			for (ti = 0; ti < tn; ti++)
				fprintf(stderr, "  op %04x  %9u  %2u%%\n",
				    top[ti], op_hist[top[ti]],
				    (unsigned)((uint64)op_hist[top[ti]] *
				        100 / n));
		}
	}



#define INC_PC(n) sc_pc += (n)

/*
 * Bits of a 68k SR that may be placed in the *real* host SR.
 *
 * Only the condition codes, bits 0-4, are user state.  Bits 5-7 of the low
 * byte are unused-must-be-zero, and NetBSD enforces that: cpu_mcontext_validate()
 * in sys/arch/m68k/m68k/sig_machdep.c rejects any context whose PS has a bit
 * in PSL_MBZ|PSL_IPL|PSL_S (0xffff7fe0, whose low byte is 0xe0).
 *
 * This matters because a rejected context is not a signal -- setcontext()
 * returns EINVAL inside libc's signal trampoline and libc exits with that
 * errno.  Masking with 0xff instead of 0x1f let a guest SR carrying any of
 * bits 5-7 through to the kernel, which killed the emulator with a silent
 * exit(22) after several hundred thousand traps.  The supervisor, trace and
 * interrupt bits are unaffected: they are kept in software in EmulatedSR.
 */
#define SR_HOST_MASK 0x1f

#define GET_SR (sc_ps | EmulatedSR)

#define STORE_SR(v) \
	sc_ps = (v) & SR_HOST_MASK; \
	EmulatedSR = (v) & 0xe700; \
	if (((v) & 0x0700) == 0 && InterruptFlags) \
		TriggerInterrupt();

//printf("opcode %04x at %p, sr %04x, emul_sr %04x\n", opcode, pc, sc_ps, EmulatedSR);

	if ((opcode & 0xf000) == 0xa000) {

		// A-Line instruction, set up A-Line trap frame on stack
		uint32 a7 = regs->a[7];
		a7 -= 2;
		WriteMacInt16(a7, 0x28);
		a7 -= 4;
		WriteMacInt32(a7, (uint32)pc);
		a7 -= 2;
		WriteMacInt16(a7, GET_SR);
		sc_sp = regs->a[7] = a7;

		/*
		 * Jump to MacOS A-Line handler on return.
		 *
		 * TEMPORARY: watch the vector itself.  A wild jump to an
		 * unmapped address whose IP equals the fault address, taken
		 * immediately after an A-line trap, is what corruption of
		 * this longword would look like -- so report the moment it
		 * changes, and refuse to jump somewhere obviously invalid.
		 */
		{
			static uint32 last_vec;
			uint32 vec = ReadMacInt32(0x28);

			if (vec != last_vec) {
				fprintf(stderr, "A-line vector @0x28: %08x -> "
				    "%08x (at pc=%08x op=%04x)\n",
				    last_vec, vec, (unsigned)pc,
				    (unsigned)opcode);
				last_vec = vec;
			}
			if (vec == 0 || vec >= 0x10000000) {
				fprintf(stderr, "A-line vector is insane "
				    "(%08x); refusing the jump\n", vec);
				QuitEmulator();
			}
		}
		sc_pc = ReadMacInt32(0x28);

	} else if ((opcode & 0xff00) == 0x7100) {

		bw_arm_if_ready();

		// Extended opcode, push registers on user stack
		uint32 a7 = regs->a[7];
		a7 -= 4;
		WriteMacInt32(a7, (uint32)pc);
		a7 -= 2;
		WriteMacInt16(a7, sc_ps);
		for (int i=7; i>=0; i--) {
			a7 -= 4;
			WriteMacInt32(a7, regs->a[i]);
		}
		for (int i=7; i>=0; i--) {
			a7 -= 4;
			WriteMacInt32(a7, regs->d[i]);
		}
		sc_sp = regs->a[7] = a7;

		// Jump to EmulOp trampoline code on return
		sc_pc = (uint32)EmulOpTrampoline;
		
	} else switch (opcode) {	// Emulate privileged instructions

		case 0x40e7:	// move sr,-(sp)
			regs->a[7] -= 2;
			WriteMacInt16(regs->a[7], GET_SR);
			sc_sp = regs->a[7];
			INC_PC(2);
			break;

		case 0x46df: {	// move (sp)+,sr
			uint16 sr = ReadMacInt16(regs->a[7]);
			STORE_SR(sr);
			regs->a[7] += 2;
			sc_sp = regs->a[7];
			INC_PC(2);
			break;
		}

		case 0x007c: {	// ori #xxxx,sr
			uint16 sr = GET_SR | pc[1];
			sc_ps = sr & SR_HOST_MASK;	// oring bits into the sr can't enable interrupts, so we don't need to call STORE_SR
			EmulatedSR = sr & 0xe700;
			INC_PC(4);
			break;
		}

		case 0x027c: {	// andi #xxxx,sr
			uint16 sr = GET_SR & pc[1];
			STORE_SR(sr);
			INC_PC(4);
			break;
		}

		case 0x46fc:	// move #xxxx,sr
			STORE_SR(pc[1]);
			INC_PC(4);
			break;

		case 0x46ef: {	// move (xxxx,sp),sr
			uint16 sr = ReadMacInt16(regs->a[7] + (int32)(int16)pc[1]);
			STORE_SR(sr);
			INC_PC(4);
			break;
		}

		case 0x46d8:	// move (a0)+,sr
		case 0x46d9: {	// move (a1)+,sr
			uint16 sr = ReadMacInt16(regs->a[opcode & 7]);
			STORE_SR(sr);
			regs->a[opcode & 7] += 2;
			INC_PC(2);
			break;
		}

		case 0x40f8:	// move sr,xxxx.w
			WriteMacInt16(pc[1], GET_SR);
			INC_PC(4);
			break;

		case 0x40d0:	// move sr,(a0)
		case 0x40d1:	// move sr,(a1)
		case 0x40d2:	// move sr,(a2)
		case 0x40d3:	// move sr,(a3)
		case 0x40d4:	// move sr,(a4)
		case 0x40d5:	// move sr,(a5)
		case 0x40d6:	// move sr,(a6)
		case 0x40d7:	// move sr,(sp)
			WriteMacInt16(regs->a[opcode & 7], GET_SR);
			INC_PC(2);
			break;

		case 0x40c0:	// move sr,d0
		case 0x40c1:	// move sr,d1
		case 0x40c2:	// move sr,d2
		case 0x40c3:	// move sr,d3
		case 0x40c4:	// move sr,d4
		case 0x40c5:	// move sr,d5
		case 0x40c6:	// move sr,d6
		case 0x40c7:	// move sr,d7
			regs->d[opcode & 7] = GET_SR;
			INC_PC(2);
			break;

		case 0x46c0:	// move d0,sr
		case 0x46c1:	// move d1,sr
		case 0x46c2:	// move d2,sr
		case 0x46c3:	// move d3,sr
		case 0x46c4:	// move d4,sr
		case 0x46c5:	// move d5,sr
		case 0x46c6:	// move d6,sr
		case 0x46c7: {	// move d7,sr
			uint16 sr = regs->d[opcode & 7];
			STORE_SR(sr);
			INC_PC(2);
			break;
		}

		case 0xf327:	// fsave -(sp)
			regs->a[7] -= 4;
			WriteMacInt32(regs->a[7], 0x41000000);	// Idle frame
			sc_sp = regs->a[7];
			INC_PC(2);
			break;

		case 0xf35f:	// frestore (sp)+
			regs->a[7] += 4;
			sc_sp = regs->a[7];
			INC_PC(2);
			break;

		case 0x4e73: {	// rte
			uint32 a7 = regs->a[7];
			uint16 sr = ReadMacInt16(a7);
			a7 += 2;
			sc_ps = sr & SR_HOST_MASK;
			EmulatedSR = sr & 0xe700;
			sc_pc = ReadMacInt32(a7);
			a7 += 4;
			uint16 format = ReadMacInt16(a7) >> 12;
			a7 += 2;
			static const int frame_adj[16] = {
				0, 0, 4, 4, 8, 0, 0, 52, 50, 12, 24, 84, 16, 0, 0, 0
			};
			sc_sp = regs->a[7] = a7 + frame_adj[format];
			break;
		}

		case 0x4e7a:	// movec cr,x
			switch (pc[1]) {
				case 0x0002:	// movec cacr,d0
					regs->d[0] = 0x3111;
					break;
				case 0x1002:	// movec cacr,d1
					regs->d[1] = 0x3111;
					break;
				case 0x0003:	// movec tc,d0
				case 0x0004:	// movec itt0,d0
				case 0x0005:	// movec itt1,d0
				case 0x0006:	// movec dtt0,d0
				case 0x0007:	// movec dtt1,d0
				case 0x0806:	// movec urp,d0
				case 0x0807:	// movec srp,d0
					regs->d[0] = 0;
					break;
				case 0x1000:	// movec sfc,d1
				case 0x1001:	// movec dfc,d1
				case 0x1003:	// movec tc,d1
				case 0x1801:	// movec vbr,d1
					regs->d[1] = 0;
					break;
				case 0x8801:	// movec vbr,a0
					regs->a[0] = 0;
					break;
				case 0x9801:	// movec vbr,a1
					regs->a[1] = 0;
					break;
				default:
					goto ill;
			}
			INC_PC(4);
			break;

		case 0x4e7b:	// movec x,cr
			switch (pc[1]) {
				case 0x1000:	// movec d1,sfc
				case 0x1001:	// movec d1,dfc
				case 0x0801:	// movec d0,vbr
				case 0x1801:	// movec d1,vbr
					break;
				case 0x0002:	// movec d0,cacr
				case 0x1002:	// movec d1,cacr
					FlushCodeCache(NULL, 0);
					break;
				default:
					goto ill;
			}
			INC_PC(4);
			break;

		case 0xf478:	// cpusha dc
		case 0xf4f8:	// cpusha dc/ic
			FlushCodeCache(NULL, 0);
			INC_PC(2);
			break;

		default:

ill:		printf("SIGILL num %d, code %d\n", sig, sip ? sip->si_code : 0);
			printf(" context %p:\n", (void *)ucp);
			printf("  uc_flags %08x\n", (unsigned)ucp->uc_flags);
			printf("  sp %08x\n", sc_sp);
			printf("  fp %08x\n", gr[_REG_A6]);
			printf("  pc %08x\n", sc_pc);
			printf("   opcode %04x\n", opcode);
			printf("  sr %08x\n", sc_ps);
									for (int i=0; i<8; i++)
				printf("  d%d %08x\n", i, gr[_REG_D0 + i]);
			for (int i=0; i<8; i++)
				printf("  a%d %08x\n", i, gr[_REG_A0 + i]);

			VideoQuitFullScreen();
#ifdef ENABLE_MON
			const char *arg[4] = {"mon", "-m", "-r", NULL};
			mon(3, arg);
#endif
			QuitEmulator();
			break;
	}
	/*
	 * Every path through this handler converges here, so what __gregs
	 * holds now is exactly what setcontext() will be asked to resume.
	 * A context it refuses is not a signal: setcontext() returns EINVAL
	 * inside libc's signal trampoline and libc exits with that errno,
	 * with no message and no core.  That failure mode is invisible
	 * enough to be worth two instructions per trap to catch.
	 */
	{
		uint32 p = (uint32)sc_pc, t = (uint32)sc_ps;

		if ((t & 0xffff7fe0u) != 0)   /* PSL_MBZ|PSL_IPL|PSL_S */
			fprintf(stderr, "%s: SUSPECT CONTEXT pc=%08x ps=%08x "
			    "a7=%08x\n", "sigill", (unsigned)p, (unsigned)t,
			    (unsigned)sc_sp);
	}

}
#endif


/*
 *  Display alert
 */

#ifdef ENABLE_GTK
static GCallback dl_destroyed(GtkWidget *dialog)
{
	gtk_widget_destroy(dialog);
	gtk_main_quit();
	return NULL;
}

void display_alert(int title_id, int prefix_id, int button_id, const char *text)
{
	GtkWidget *dialog = gtk_message_dialog_new(NULL,
	                                           GTK_DIALOG_MODAL,
	                                           GTK_MESSAGE_WARNING,
	                                           GTK_BUTTONS_NONE,
	                                           GetString(title_id), NULL);
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", text);
	gtk_dialog_add_button(GTK_DIALOG(dialog), GetString(button_id), GTK_RESPONSE_CLOSE);
	g_signal_connect(dialog, "response", G_CALLBACK(dl_destroyed), NULL);
	gtk_widget_show(dialog);

	gtk_main();
}
#endif


/*
 *  Display error alert
 */

void ErrorAlert(const char *text)
{
	if (gui_connection) {
		if (rpc_method_invoke(gui_connection, RPC_METHOD_ERROR_ALERT, RPC_TYPE_STRING, text, RPC_TYPE_INVALID) == RPC_ERROR_NO_ERROR &&
			rpc_method_wait_for_reply(gui_connection, RPC_TYPE_INVALID) == RPC_ERROR_NO_ERROR)
			return;
	}
#ifdef ENABLE_GTK
#ifndef USE_SDL_VIDEO
	if (x_display == NULL) {
		printf(GetString(STR_SHELL_ERROR_PREFIX), text);
		return;
	}
#endif
	VideoQuitFullScreen();
	display_alert(STR_ERROR_ALERT_TITLE, STR_GUI_ERROR_PREFIX, STR_QUIT_BUTTON, text);
#else
	printf(GetString(STR_SHELL_ERROR_PREFIX), text);
#endif
}


/*
 *  Display warning alert
 */

void WarningAlert(const char *text)
{
	if (gui_connection) {
		if (rpc_method_invoke(gui_connection, RPC_METHOD_WARNING_ALERT, RPC_TYPE_STRING, text, RPC_TYPE_INVALID) == RPC_ERROR_NO_ERROR &&
			rpc_method_wait_for_reply(gui_connection, RPC_TYPE_INVALID) == RPC_ERROR_NO_ERROR)
			return;
	}
#ifdef ENABLE_GTK
#ifndef USE_SDL_VIDEO
	if (x_display == NULL) {
		printf(GetString(STR_SHELL_WARNING_PREFIX), text);
		return;
	}
#endif
	display_alert(STR_WARNING_ALERT_TITLE, STR_GUI_WARNING_PREFIX, STR_OK_BUTTON, text);
#else
	printf(GetString(STR_SHELL_WARNING_PREFIX), text);
#endif
}


/*
 *  Display choice alert
 */

bool ChoiceAlert(const char *text, const char *pos, const char *neg)
{
	printf(GetString(STR_SHELL_WARNING_PREFIX), text);
	return false;	//!!
}
