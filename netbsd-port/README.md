# Basilisk II on NetBSD/mac68k

Running Basilisk II on a Macintosh Centris 650 (68040/25MHz, 68MB RAM,
512KB VRAM, DAFB video) under NetBSD 10.1 — a 68k Mac emulator on a 68k
Mac. Downstream port work; the host OS patches live in a separate tree.

## Why this is worth attempting at all

Basilisk II can execute guest 68k code **on the host CPU**, with no
interpreter and no JIT. `configure.ac` gates that on `CAN_NATIVE_M68K`,
and the `case "$target_os"` switch sets it for exactly one Unix:

    netbsd*)
      CAN_NATIVE_M68K=yes

The native backend (`src/native_cpu/`, `Unix/asm_support.s`) is present.
So applications inside the emulator run at the host's real speed and the
display is the entire cost. Everything below follows from that.

## The problem with the stock X11 path

`video_x.cpp` is the only video backend in `Unix/`. Its windowed mode
works in two steps per frame: `memcmp` the guest screen against a shadow
copy to find what changed (`update_display_static`), then `XPutImage`
the changed rectangle.

Both steps were measured on the target with `fbbench` (in the host-OS
tree), over a 640x480x8 frame:

| operation | | |
|---|---|---|
| RAM compare | 7.21 MB/s | **40.6 ms/frame** |
| RAM to RAM copy | 11.64 MB/s | 25.2 ms/frame |
| RAM to VRAM (write) | 11.45 MB/s | 25.6 ms/frame |
| VRAM to RAM (read) | 8.13 MB/s | 36.0 ms/frame |
| VRAM clear (memset) | 21.41 MB/s | 13.7 ms/frame |

The useful surprise: **the unaccelerated framebuffer is not the slow
part.** A full-screen VRAM write costs 25.6 ms and a pure clear runs at
21 MB/s, while comparing two buffers in ordinary RAM costs 40.6 ms — two
thirds slower than writing to the "slow" uncached device memory.

That scan runs *every frame even when nothing changes*, so the windowed
path is capped near 24 fps doing nothing at all, and around 11 fps for
full-screen updates (40.6 + 25.2 socket copy + 25.6 blit ≈ 91 ms).

Why `memcmp` trails `memcpy` is **not explained**. NetBSD's m68k `memcmp`
is not a byte loop (it converts to a longword count and uses `cmpml`),
and neither it nor `memcpy` uses `movem`. Left open rather than guessed
at a third time.

## The approach: a wscons DGA backend

`driver_fbdev` (a `driver_dga` subclass) already does the right thing on
Linux — it `mmap`s the framebuffer device straight into `the_buffer`, the
guest's screen. Zero copy: no shadow, no scan, no blit. All of the above
costs vanish.

That path uses **no Linux headers and no `FBIO*` ioctls**. It reads a
text file of `name depth offset`, opens a device, and `mmap`s at that
offset. So `driver_wscons` is a sibling class, not new architecture:

- device `/dev/ttyE0` instead of `/dev/fb`
- `WSDISPLAYIO_GET_FBINFO` instead of the `fbdevices` text file — the
  kernel reports width, height, stride, depth and offset directly, so the
  hand-maintained table disappears entirely
- `WSDISPLAYIO_SMODE` → `MODE_DUMBFB` before the mmap, `MODE_EMUL` after
  (no Linux equivalent, must be added)
- palette through `WSDISPLAYIO_PUTCMAP`
- `suspend`/`resume` become real rather than stubs: they are the
  `SMODE` transitions, which the host's console driver already hooks

`driver_fbdev` is ~150 lines. This should be comparable.

VOSF (`--enable-vosf`, dirty pages via `mprotect` + `SIGSEGV`) becomes
irrelevant if this works: VOSF exists to make the *scan* cheap, and this
removes the scan, the shadow and the blit together.

## Verified on hardware

**Coexistence (the assumption everything rested on).** `driver_dga` keeps
an X connection for input while bypassing it for drawing, so the X server
and the emulator both hold the framebuffer mapped. Tested with
`fbcoexist.c` against a live Xwscons:

- `mmap` of `fboffset + fbsize` (311296 bytes) **succeeds** while X owns
  the display — no `EBUSY`, no refusal
- **no `SMODE` is needed** under X; the mapped mode is inherited, which
  is how `driver_fbdev` behaves
- the X server **survives** a second process writing into its framebuffer
- drawing lands where expected and is visible
- geometry reports stride 640, fboffset 4096 — so the host's panning
  console driver does park its scanout when X enters mapped mode

Consequence for the design: under X the constructor need not issue
`SMODE` at all. It is still needed for a standalone (no X) run.

**The CLUT is contested, and this is the real open question.** The probe
wrote pixel values `0xff` and `0x80` expecting grey and white; they came
out blue and dark, because at depth 8 those values are *indices into
whatever palette is loaded* and X had allocated its own when it started.
Writing pixels into a framebuffer is therefore only half the job — the
guest's colours mean nothing until its palette is in the hardware CLUT,
and while X is running X owns that CLUT.

Upstream anticipates this: `video_x.cpp` keeps `static Colormap cmap[2]`
with the comment that DGA needs two of them, and `driver_base` declares
`update_palette()` as the hook. So the mechanism exists; what is not yet
established is how it behaves on this port, where `WSDISPLAYIO_PUTCMAP`
reaches the hardware through genfb's colormap callback and the host's
console driver has its own opinions about the palette (it installs an
r3g3b2 ramp at attach). Expect the emulator taking the CLUT to recolour
the X session behind it, and vice versa.

This is a fullscreen-DGA design, so the emulator owning the palette
whenever it has focus is defensible — but it needs testing, not assuming.

**Tested with `cmapprobe.c`, and the mechanism works.** Against a live
Xwscons on the target:

- `WSDISPLAYIO_GETCMAP` returns a full 256-entry map — X's own allocated
  colours, `[0] ff ff ff` (white) through `[255] 00 00 00` (black), with
  allocations scattered between: magenta near 51, blue near 102, purple
  near 153, orange near 204
- `WSDISPLAYIO_PUTCMAP` is **accepted while X is running** and reaches
  the hardware
- restoring the saved map afterwards works exactly: a 256-entry index
  ramp drawn into the framebuffer renders with its bands falling on the
  colours `GETCMAP` had reported, which is only possible if the restore
  was byte-for-byte
- the X session survives all of it

So `update_palette()` is implementable the straightforward way: save on
entry, install the guest's map, restore on exit. There is one hardware
colormap serving both parties — our pixels rendering through X's palette
proves it — so the save/restore is mandatory rather than optional.

Note `GETCMAP` is safe here where a direct RAMDAC readback is not: it
returns genfb's software copy rather than reading the DAC data register,
which on this hardware desynchronises the R/G/B phase and turns the
display red.

## Build notes

**SDL is not required, but it is the default** — and `configure.ac` lies
about that. The help text reads `[default=no]` while the default action
sets `yes`:

    AC_ARG_ENABLE(sdl-video, [ ... [default=no]], [WANT_SDL_VIDEO=$enableval], [WANT_SDL_VIDEO=yes])

And `WANT_SDL_VIDEO=yes` sets `WANT_XF86_DGA=no`, `WANT_XF86_VIDMODE=no`
and `WANT_FBDEV_DGA=no` — silently disabling every direct-framebuffer
path. So the build must pass:

    ./configure --disable-sdl-video --disable-sdl-audio

which leaves `WANT_SDL=no` (no SDL dependency at all) and keeps the DGA
paths available.

**Audio.** With SDL audio off, the `netbsd*` case sets only
`CAN_NATIVE_M68K` and `ETHERSRC`, never `AUDIOSRC`, so it falls through to
`audio_dummy.cpp` — a silent emulator. `audio_oss_esd.cpp` exists and the
target has working OSS emulation (`libossaudio`, `/dev/dsp` → `sound0`),
so this is likely a one-line addition to that case. Unverified.

**Cross-compilation.** Built in a Debian container against an m68k sysroot
and toolchain. `configure`'s run-time probes cannot execute when cross
compiling, so the cache needs seeding with answers established separately
on the target — `sigsegv_recovery` in particular, if VOSF is ever wanted.

**XF86 DGA is not an option here.** It needs the server to implement the
XFree86-DGA extension, which kdrive/tinyx does not. `ENABLE_FBDEV_DGA`
needs no X extension, only the device — which is why it is the viable one.

## Status: driver_wscons works; the guest faults at 0x2000

The backend is exercised and sound.  With DGA actually selected the whole
constructor runs and the X server survives:

    vtrace: about to construct driver (type 1)
    wstrace: constructor entered
    wstrace: XCreateWindow returned
    wstrace: XMapRaised done; entering wait_mapped
    wstrace: wait_mapped returned          <- no hang
    wstrace: grabs done
    wstrace: mmap ok
    wstrace: CONSTRUCTOR COMPLETE
    mtrace: about to Start680x0 (guest begins)
    Caught SIGSEGV at address 0x2000

So: the window, the mode handling, the palette save, the grabs and the
framebuffer mapping all work, `wait_mapped()` returns promptly, and the
emulator reaches the point of starting the guest CPU.

**SOLVED: the guest faulted at 0x2000 because that is where WE were.**
`objdump -p` showed the executable's first LOAD segment at vaddr 0x2000
-- exactly the fault address.  NetBSD/m68k links there by default, so the
emulator's own read-only text sat precisely where the guest's RAM must
begin, and a natively executing guest hit it immediately.

configure has machinery for this (`LINKER_SCRIPT_FLAGS`) with entries for
Linux i386/ppc, FreeBSD i386 and NetBSD i386 -- but none for m68k, so
`HAVE_LINKER_SCRIPT` was undefined and `can_map_all_memory` was false.
A `netbsd*:m68k` case using `-Wl,-Ttext-segment=0x10000000` moves the
image clear; no script file is needed. Verified: the binary now loads at
0x10000000 and `HAVE_LINKER_SCRIPT` is defined.

**Now: SIGILL in startup, narrowed to one call.**  Traces bracket it
exactly:

    mtrace: mapped RAM+ROM from 0x0000
    mtrace: memory areas decided
    mtrace: bases set RAM=0x0 ROM=0x800000
    <dies -- the ScratchMem trace never prints>

So it dies in `vm_acquire_mac(SCRATCH_MEM_SIZE)`, the next call after the
bases are set.

A mechanism worth checking first, because it explains why this appears
only now: with RAM mapped from zero, `RAMBaseHost` is literally
`(uint8 *)0`, so address 0 is valid, mapped, and full of zeros.  Any null
or uninitialised function pointer that would previously have died with a
clean SIGSEGV now *jumps into guest RAM* and executes zeros until it
reaches something illegal.  An early, otherwise-inexplicable SIGILL is
what that looks like.  UNTESTED.

`ScratchMem` was suspected first, on the grounds that the
`memory_mapped_from_zero` branch does not assign it while the `else`
branch does -- but that is wrong: line 767 assigns it unconditionally for
both paths, after the branch.

**Old note kept, since the reasoning still holds:**
SIGILL during startup, right after XOpenDisplay.  The relocated
binary itself is sound -- `--help` runs and exits 0 -- so the image is not
corrupt.  What changed is behaviour: with `HAVE_LINKER_SCRIPT` defined,
`can_map_all_memory` is true and startup takes the
`vm_acquire_mac_fixed(0, RAMSize + ROM_MAX_SIZE)` path for the first
time, mapping the guest's RAM and ROM from address zero.  Everything
before that point is identical to the run that previously reached
`Start680x0`.

Worth noting `sigill_handler` is deliberate in native 68k mode -- it is
how EMUL_OP and A-traps are dispatched -- but it is installed well after
this point, so an early SIGILL is fatal rather than handled.

The old note follows, kept because the reasoning still applies:

**The guest faulted at 0x2000**, immediately above the low memory
area (0x0000-0x2000).  The low globals themselves ARE mapped -- the
"Cannot map Low Memory Globals" error disappears once
`vm.user_va0_disable=0` -- so this is about what lies just above them,
i.e. where the guest's RAM is expected to be in real-addressing mode.

`BII_CROSS_MAP_LOW_AREA` is a red herring: despite its description it
feeds `PAGEZERO_HACK`, the Mach-O `__PAGEZERO` trick, and does nothing on
NetBSD.

## The X server wedge -- SOLVED, and it was never ours

Running the emulator wedges Xwscons: it stops answering every client,
xdpyinfo included.  The emulator's own symptom is a hang in
`wait_mapped()`, blocked in `poll()` on the X socket forever.

**Two diagnoses were published here and both were wrong.**  First, that
kdrive never delivers `MapNotify` to an override-redirect window --
`wait_mapped()` waits on exactly that, and `ktrace` showed the blocked
poll.  Second, that `driver_wscons` issuing `WSDISPLAYIO_SMODE` under a
live server was disturbing it.  The driver now asks `GMODE` first and
leaves an already-mapped display alone, which is correct regardless, and
X still wedged.

`wedgebisect.c` settles what does NOT cause it.  Against a live server,
each step added one at a time, every stage completes and X keeps
answering:

1. create a fullscreen override-redirect window
2. map it -- **`MapNotify` ARRIVES**, disproving the first diagnosis
3. set input focus
4. grab the keyboard
5. grab the pointer
6. `XChangePointerControl` (what `disable_mouse_accel` does)
7. mmap the framebuffer and write to it

So the wedge is in something the driver does that this does not.  What
remains untested: the `CWColormap` window attribute (the bisect uses the
default visual and no colormap), `set_window_name`, and -- most
interesting -- the emulator's threads and its 60 Hz signal traffic, since
Xlib is not thread-safe without `XInitThreads` and the native 68k build
takes `SIGALRM` and `SIG_IRQ` constantly.

**Instrumenting the driver settled it: the wedge is not ours.**  Traces
were added through the whole `driver_wscons` constructor, on stderr so
nothing could be lost to buffering when the process is killed.  The trace
file came back EMPTY while the server wedged anyway -- the constructor
never executed its first line.

Confirmed from the other direction by running `--screen win/640/480`,
where `driver_wscons` is never constructed at all.  X wedges just the
same.

So the cause is generic to Basilisk II on this machine and has nothing to
do with the wscons backend, the framebuffer mapping, the colormaps or the
display mode.  The remaining suspect is the emulator's interaction with
Xlib under its own signal traffic: the native 68k build fields SIGALRM
and SIG_IRQ continuously, Xlib is not thread-safe without
`XInitThreads()`, and a signal taken part-way through writing a request
leaves a partial request on the socket -- a server waiting for the rest
of one is a plausible way for it to stop answering everybody.

That is a hypothesis and has NOT been tested.  The cheap experiments are
to call `XInitThreads()` early, and to block SIG_IRQ/SIGALRM around Xlib
calls, and see whether either changes the outcome.

Note the first version of the bisect produced a meaningless "all stages
passed": it called `XSetInputFocus` on an unmapped window, which is
`BadMatch`, and Xlib's default error handler exits -- so stages 2 to 6
never ran their steps.  Mapping now precedes focus and the handler is
non-fatal.

## Build status

`driver_wscons` is written and `configure` selects it. The build gets
through the whole emulator -- CPU, video, audio, ethernet, SCSI, the lot --
and stops in one place:

    SDL support ............................ : none
    XFree86 DGA support .................... : no
    fbdev DGA support ...................... : no
    wscons DGA support ..................... : yes
    Enable video on SEGV signals ........... : no
    Running m68k code natively ............. : yes

**Blocker: `CrossPlatform/sigsegv.cpp` has no working NetBSD/m68k case.**

It has one, but it was written when `struct sigcontext` was public API.
On NetBSD 10 that struct is guarded:

    /usr/include/m68k/signal.h:
    #if defined(_LIBC) || defined(_KERNEL)
    struct sigcontext {

so an ordinary program cannot see it, and the block fails on an incomplete
type, on `scp->sc_ap`, and on a `code` argument that no longer exists.

Setting `BII_CROSS_HAVE_SIGCONTEXT_SUBTERFUGE=no` does not help: with no
mechanism selected, neither `SIGSEGV_FAULT_HANDLER_ARGLIST` family gets
defined and the file fails earlier still. sigsegv.cpp is compiled
unconditionally and insists on one or the other, even with VOSF off.

The fix is to port that block to the modern interface — a `siginfo_t` +
`ucontext_t` handler in the extended-signals family rather than the
legacy sigcontext one. NetBSD/m68k has what is needed:
`m68k/mcontext.h` defines `__gregs` with `_REG_PC` (16) and `_REG_A7`
(15), and the i386, x86_64 and powerpc NetBSD cases in the same file
already show the shape (`((ucontext_t *)scp)->uc_mcontext.__gregs`).

Note this is needed *despite* VOSF being disabled, so it is not optional
work that could be skipped by giving up on dirty-page tracking.

## Guest configuration

Run the guest at **640x480x8** to match the host exactly. Any depth or
size mismatch forces per-pixel conversion; matched, it is a straight
mapping. Every cost here is linear in pixels, so a smaller guest screen
is a proportional saving if one is ever needed.

## Files

- `fbcoexist.c` — the coexistence probe described above. Deliberately
  never restores `MODE_EMUL`: handing the console back while X is drawing
  is what corrupts the display.
