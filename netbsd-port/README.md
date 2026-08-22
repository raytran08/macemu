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





### The 0x9fc0000 fault: a Fixed value reaches the PC

The System 7.5 image dies at ~18000 traps with `SIGSEGV at 0x9fc0000
[IP=0x9fc0000]`.  Established by test: not the build (the pre-fix binary
does it too), not the disk (a pristine copy does it too), not bfast (it
does it with the module unloaded).

The fault address is not an address.  Read as 16.16 fixed point,
0x09fc0000 is 2556.0 -- and the guest stack at the fault holds
0x09fa0000 (2554.0) two longwords away.  The last traps before the jump
are a868 and a869, `_FixRatio` and `_FixMul`, running in a tight loop at
ROM 0x408270ae-b4.  So a Fixed operand is reaching the program counter.

Ruled out along the way: the A-line vector at guest 0x28 is NOT
corrupted.  Watched across a whole run, it is written once at startup
(0 -> 0x008099b0) and never changes, so the wild jump happens inside the
guest's dispatcher after we vector to it correctly, not because we
vectored somewhere wrong.

Also worth noting for whoever picks this up: the guest executes ROM
through BOTH aliases -- 0x40826xxx and 0x0080xxxx appear in the same
trap ring -- which is the machine-specific double mapping this port
introduced.  A trap dispatch table holding one base while execution runs
at the other is the obvious next thing to check, along with the Fixed
traps' return path.

The register file at the fault settles the mechanism:

    a2  09fc0000   <== equals the PC
    a3  09fa0000   Fixed 2554.0
    d7  012c0000   Fixed 300.0
    d6  0f4a0000   Fixed 3914.0
    a7  005fa1a2

PC == a2, so control transferred THROUGH a2 -- a jmp/jsr (a2) -- while a2
held fixed-point data.  Every register holds a coherent Fixed value; the
arithmetic in flight is sane.  What is wrong is that code expecting a2 to
be a pointer was given control.  So this is a control-flow error upstream
of the crash, not corrupted data at it: the guest's dispatch for the
_FixRatio trap ends up somewhere that treats a2 as a routine address.

Eliminated so far, each by direct test rather than argument:
- the build, the disk image, the bfast module (all fault identically);
- the A-line vector at guest 0x28 (written once at startup, never
  changes across a whole run);
- BasiliskII's EMUL_OP register save/restore.  Verified by reading:
  sigill_handler pushes ascending d0-d7, a0-a7, sr, pc; the trampoline's
  a0@(66) correctly indexes the PC past 64 bytes of registers plus the
  SR word, and the restore is moveml sp@+,d0-d7/a0-a6, skip the stale a7
  slot, rtr.  Consistent, no scrambling.

The Toolbox dispatch table is NOT the problem, and neither is the ROM
alias.  Disassembling the guest's own A-line handler at 0x008099b0 gives
the table addresses directly --

    8099e0:  movel @(0x0e00,%d2:w:4),%sp@(8)   Toolbox: 0x0E00 + trap*4
    809a04:  jsr   @(0x0400,%d2:w:4)@(0)       OS:      0x0400 + trap*4

-- and dumping them at the fault shows the table intact: A868 resolves to
0x0081c312, A869 to 0x0081c490, A9A0 to 0x0081b5A0, and a scan of all
1024 Toolbox entries reports **zero** suspect values.  The handler it
points at is ordinary 68020+ ROM code (mulsl, bfexts), all valid on a
68040 and none of it trapping.

What the trap ring actually shows, read properly, is an interrupt:

    pc=0080a29a op=7129   EMUL_OP -- and 0x7129 is M68K_EMUL_OP_IRQ
    pc=0080a2a8 op=46fc   move #x,sr    interrupt epilogue
    pc=00809b88 op=4e73   rte           return from interrupt

So a 60Hz interrupt is delivered in the middle of the Fixed-math
sequence, and after it returns the code proceeds with a2/a3 holding
fixed-point values where it expects pointers.  That fits everything: the
timing dependence, the ~18000-trap delay, and the absence of the fault on
the idle Disk Tools workload.  It is also a bug class this port has hit
before -- see commit 6df9880d, "Fix the 60Hz interrupt corrupting
non-guest context".

That interrupt lead did not survive contact either.  With a2/a3 recorded
in the ring and interrupt DELIVERIES marked, a run shows **no interrupt
at all** in the final 24 events: the 0x7129 entries are the guest
executing Basilisk's IRQ opcode from inside the patched ROM, not
sigirq_handler injecting a frame.  And a2/a3 are *sane pointers*
(0x00003024, 0x005faefc) at every trap including the last one; they only
become Fixed values in the stretch between the final trap and the fault.

Nor is it a jmp through a2.  Scanning the ROM for jmp/jsr (a2) forms
around _FixRatio, the dispatcher, and the Fixed-math caller finds none,
so PC == a2 is a correlation -- the dispatcher restores a2 and the
return address from neighbouring stack slots -- not a jump.

The whole dispatch chain has now been verified correct by disassembly:

    8099b0:  movel %a2,%sp@-        sp' = sp-4
    8099b2:  movel %d2,%sp@-        sp' = sp-8
    8099b4:  moveal %sp@(10),%a2    sp-8+10 = sp+2 = the PC field
    8099b8:  movew %a2@+,%d2        read the trap word
    8099c6:  movel @(0x1e00,%d2:w:4),%sp@(8)   -> writes over sp+0
    8099ce:  movel %a2,%sp@(12)                -> writes over sp+4
    8099d2:  movel %sp@+,%d2
    8099d4:  moveal %sp@+,%a2
    8099d6:  rts

The dispatcher REWRITES the exception frame in place into
[handler][return = trapPC+2] and reaches the handler with rts, discarding
the stacked SR (harmless: real hardware is in supervisor mode either way,
and Basilisk tracks the guest SR separately in EmulatedSR).  Every offset
matches the 8-byte format-0 frame sigill_handler pushes.  A868 resolves
through 0x1E00 + (0xA868-0xAC00 sign-extended)*4 = 0xFA0, which is the
entry we dumped and found sane.

So: frame layout correct, vector correct, table correct, handler correct,
registers sane until the last trap, no interrupt in the window.  The
corruption happens inside the window between the final A-line trap and
the fault, with nothing in the emulator's own trap path implicated.

SOLVED DOWN TO THE FUNCTION, by kernel single-step tracing (bfast hooks
vector 9; T1 is set in the frame our stub RTEs with, so setcontext never
sees it; the userland handlers strip T1 defensively at entry).  A
7580-instruction trace of the fatal window shows, in order:

1. entry 7531: `moveml %sp@+,%d3-%d7/%a2-%a4` at 40826f6a -- a function
   EPILOGUE restoring saved registers from 005fa18a -- loads
   a2=09fc0000, a3=09fa0000.  The save slots fall at: d7@005fa19a,
   a2@005fa19e, a3@005fa1a2.  Those are exactly the addresses the crash
   later reads: the fatal rts pops 005fa19e, and the fault-time stack
   dump shows 09fa0000 at 005fa1a2.
2. `unlk %fp` then moves sp DOWNWARD (005fa1aa -> 005fa194), impossible
   for a balanced frame: the frame linkage is corrupt too.
3. the common tail at 408265f0 (reads low-mem 0x99A/0xB2A/0xB10, sets
   0x15E) ends `moveal %sp@+,%a0; addqw #4,%sp; jmp %a0@` -- pops
   00010f4a (a RAM hook, still sane), jumps to it; the two-instruction
   RAM thunk at 00010f4a ends rts, which pops the overwritten a2 save
   slot -- 09fc0000 -- into the PC.  Fetch faults; that is the crash.

So the corruption event is: DURING the body of the function whose
epilogue is at 40826f6a, its saved-register area and frame word were
overwritten by consecutive Fixed values (two adjacent longwords, 2554.0
and 2556.0).  That is the signature of Toolbox trap results stored
through a displaced stack pointer -- FixRatio-family traps write their
result to caller-reserved stack space ABOVE the return address, so an
sp off by a constant paints results upward over the caller's frame.

The trace also shows the whole visible window behaving perfectly -- the
poison predates the window's own FixRatio calls, whose frames all sit
safely below the save area.  Three further rounds (a data watchpoint piggybacked on the trace, and a
healthy-vs-fatal iteration comparison) narrow it to this:

- The "clobbered save area" was a red herring: the watchpoint shows the
  write at 005fa19e is an ordinary PUSH by the RAM hook at 00010f3a in a
  SHALLOWER, healthy context.  The values there are stale leftovers, not
  corruption.
- The real defect is DEPTH, not data.  A healthy iteration runs the
  common resume tail (408265f0: pops a resume address, skips 4, jmp)
  at usp=005fa19c and pops 00010f2a, the RAM hook entry.  The fatal
  iteration runs the SAME tail at usp=005fa194 -- exactly 8 bytes,
  one exception frame, deeper -- and pops 00010f4a, which is a stale
  A-line RETURN address (trap at 00010f48, +2), landing in the hook's
  rts thunk, which then returns into loose Fixed data.
- The 60Hz interrupt is implicated by direct observation: in the fatal
  run the userland trap ring records *** INTERRUPT *** delivered at
  0081b6d6 (a table-scan loop) a handful of traps before the death, and
  the crash follows within ~5 traps.  The same delivery is what blinds
  the kernel tracer (sigirq must strip T1 from the interrupted context,
  because T1 is in PSL_MBZ and setcontext would refuse it).

Working hypothesis, one instrument from proof: the common tail at
408265f0 is shared between a normal path and an interrupt-return path
whose stack carries one extra 8-byte frame; the 60Hz delivery -- either
by its placement or by how the guest's IRQ glue unwinds through this
region -- leaves the tail entered at the wrong depth.  Next instrument:
kernel-side interrupt markers in the trace ring (the userland ring has
them; the kernel ring does not yet), plus tracing armed from the IRQ
vector (0x64) rather than from an A-line, so the delivery-to-death
window is captured whole.

Tracer limitation to remember: any 60Hz delivery inside a traced window
kills the trace (the T1 strip), so windows that need to survive an
interrupt must be re-armed from the interrupt path itself.

Instrumentation for this is in main_unix.cpp behind the SIGSEGV dump: a
24-entry ring of (pc, opcode, a7) filled on every signal-path trap, the
guest stack, and the full register file captured in sigsegv.cpp's handler
(sigsegv_info_t carries only addr and pc, which is not enough to say why
control went somewhere impossible).  The hot-path cost is two stores.

### Correction: Applesex.hfv is NOT damaged; System 7.5 hits a real bug

Commit 1a50cab7's message claims the System 7.5 volume is damaged.  That
was wrong, and this note supersedes it.

The reasoning was: both the fixed and the pre-fix binary died on that
image at ~18000 traps with an identical `SIGSEGV at 0x9fc0000
[IP=0x9fc0000]`, so the variable had to be the image rather than the
build.  The first half of that is sound; the conclusion was not.  Two
binaries failing identically is equally consistent with a deterministic
emulator bug that this guest triggers, and that is what it turned out to
be.  A pristine 200MB copy, transferred fresh, fails at the same address,
as does the same copy with the bfast module unloaded.  Three variables
eliminated -- build, disk contents, kernel fast path -- and the fault
does not move.

What is known about it: the faulting address is the instruction pointer
itself (`IP == fault address`), so control transferred to 0x09fc0000 and
faulted on the fetch.  That is a HOST address, well below MAP_BASE
(0x10000000) and about 31MB past the text segment base (0x08000000), so
it is not a guest address that failed to translate -- something jumped
the host CPU through a bad pointer.  The same image demonstrably booted
to the Finder earlier in its history, so this is a state-dependent path
rather than an unconditional one.

Consequence for the fast-path results: they were all measured on Disk
Tools 7.1, which boots and runs indefinitely.  They stand on that
workload.  The heavier System 7.5 workload remains unmeasured, and this
bug -- not disk damage -- is what blocks it.

### Trap census: what an in-kernel fast path must cover

Measured on the Disk Tools 7.1 desktop (histogram built into
`sigill_handler`, dumped every 100k traps; the 100k-200k delta excludes
boot).  The machine spends 94% of its CPU in the kernel delivering these
as signals, at ~1.3ms per trap against ~15us for a trap handled at trap
level, so the mix below is the design input for a vmmon-style module:

    SR bookkeeping   59%   007c ori #,sr (11%), 40e7 move sr,-(sp) (8%),
                           46df move (sp)+,sr (8%), 40c0 move sr,d0 (5%),
                           4e73 rte (4%), 46fc move #,sr (4%),
                           46c0 move d0,sr (2%), f4f8 cpusha (2%), tail
    A-line (toolbox) 35%   a822, a02e, a055, a030, a0dd, abf7, long tail
    EMUL_OP (71xx)    6%   must reach userland: the device models live
                           there; this class bounds any speedup

Covering the first two classes in the kernel -- virtual-SR bookkeeping
and exception reflection onto the guest's own A-line vector -- takes ~94%
of traps off the signal path.  At 15us/1300us that cuts the trap load
roughly 12x; what remains is almost entirely EMUL_OP delivery, which is
Amdahl's share and cannot move without moving the device models.

The opcode set the module must handle is small and closed: the SR family
(ori/andi/move to and from SR, rte, stop), cpusha, and blind A-line
reflection.  Everything else -- EMUL_OP, movec, genuinely illegal
opcodes -- falls through to the existing signal path unchanged, which
also keeps the emulator fully functional with the module absent.


### Do not give the DGA window a blank X cursor

The obvious tidy-up -- `XDefineCursor` an empty pixmap cursor on the
wscons window, the way `driver_window` does with its `no_cursor`, and pass
that cursor to `XGrabPointer` -- **breaks mouse clicks**.  The pointer
still moves, so it looks harmless; but button events stop reaching the
guest entirely.  The likely mechanism is that the cursor is rejected,
`XGrabPointer` fails with BadCursor, and the grab is therefore never
established.  Motion continues to look normal because under `-kcursor`
dafbcons draws the pointer from the vertical blank interrupt, in the
kernel, whether or not the emulator has a pointer grab -- which makes a
dead grab remarkably convincing.

It is also unnecessary.  The `hw.dafbcons.cursor` stand-down in the
`driver_wscons` constructor does the job: the sysctl reads 0 for as long
as the guest holds the display, and no cursor is drawn over the guest.

Stray X-shaped marks on the guest desktop are not evidence against it.
They are droppings left on the framebuffer *before* the guest took over,
and they persist only because nothing repaints that region afterwards.
Judge this from a fresh run, not from marks inherited from the last one.

Unrelated latent bug noticed while looking: `XCreatePixmap` leaves the
pixmap contents **undefined**, and `driver_window`'s no_cursor has always
depended on them being zero.  It works because the servers in practice
zero new pixmaps, not because it is correct.


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

**The null-pointer hypothesis is NOT confirmed.**  An early SIGILL
handler was installed to print the faulting PC -- low PC would mean
executing guest memory, a PC near 0x10000000 would mean a real bad
instruction in our own text.  It never printed: the process died with the
default action despite the handler being installed, which usually means
signal delivery failed for want of a usable stack.

Retried with `sigaltstack` + `SA_ONSTACK`, and handlers for SIGBUS and
SIGSEGV as well.  The result was not a captured PC but a *changed failure
mode*: no SIGILL, no core, the process simply blocks at the same point
until killed.  Still no handler output, so no signal is being delivered
at all now.

That the failure changes when fault handlers are merely installed is
itself a clue, and not one that fits the null-jump story cleanly.  A
wild jump should still trap.  Something about signal disposition or
delivery is involved.  Next: find out whether it is blocked in
vm_acquire_mac itself -- ktrace will say, as it did for the X socket --
rather than inferring from the outside again.

The original mechanism, still unproven, was: with RAM mapped from zero, `RAMBaseHost` is literally
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

## Status: Mac OS boots to the desktop, then exits silently

It was never stalled at the welcome screen -- it is **slow**, and every
observation that said otherwise was made by killing it too early.  Two
framebuffer captures, three minutes apart, taken with `fbshot`:

- `doc-startup-screen.png` -- the Mac OS splash with the happy Mac, and
  "Starting up..." with the progress bar about 15% along
- `doc-desktop.png` -- splash gone, desktop pattern filling the screen,
  menu bar drawn across the top

So it loads extensions and reaches the Finder.

**Why it is slow** is structural: in native 68k mode every A-line trap and
every privileged instruction becomes a Unix signal -- SIGILL, kernel
delivery, our handler builds a Mac exception frame, return, ROM
dispatcher.  That is the entire toolbox going through signal delivery, at
roughly 700 traps/sec, so about 1.4ms for what hardware does in
microseconds.  Nothing is wrong; there is simply a very large constant
factor.

**The exit(22) is setcontext() failing.**  ktrace, attached near the end
and dumped, catches it exactly:

    setcontext(0xc941140)  RET  JUSTRETURN          <- thousands of times
    setcontext(0xc941140)  RET  -1 errno 22 Invalid argument
    exit(0x16)                                      <- 0x16 == 22

NetBSD's signal trampoline returns from a handler by calling
`setcontext()`.  It succeeds for hundreds of thousands of traps, then one
call is refused with EINVAL and libc exits with that errno.  That is why
nothing is printed and why `QuitEmulator` is never reached: the exit
happens inside libc, below the emulator.

`setcontext` validates the context it is handed, and our handlers rewrite
`__gregs` -- PC, PS and A7 -- on every trap.  So one of those writes
eventually produces something the kernel will not resume.  RTE is the
obvious suspect, since it takes its return address off the guest stack
(`sc_pc = ReadMacInt32(a7)`).

A first attempt to catch it (rejecting an odd PC or a PS with high bits
set) never fired, but that check was placed only before the `ill:` label
-- the unhandled-opcode path -- so it never ran on the branches that
actually return, and `sigirq_handler` was not instrumented at all.  The
check needs to be on every return path, in both handlers.

Earlier framing, still accurate as far as it goes:  Confirmed not to be
an artefact of how it is launched: a run started with `nohup`, fully
detached, with no watchdog and nothing holding it, ended by itself at
396,000 traps.  Three runs now: 396k, 406k, 416k.

Status 22 is a deliberate `exit(22)`, not a signal -- a killed process
would report 128+signal.  It bypasses `QuitEmulator` (instrumented, never
prints) and a handler installed for SIGTERM/HUP/QUIT/ABRT/PIPE (never
fires).  No literal `exit(22)` appears in the source.  The log's final
line is an ordinary trap sample; nothing is printed on the way out.

The last EMUL_OP seen repeatedly before the end is 0x7130 at pc
0x0009d576 -- `M68K_EMUL_OP_BLOCK_MOVE`, which is common enough during
boot that its presence proves nothing on its own.

Earlier note, now superseded:  No fault, no message, exit after roughly
400,000 traps -- 406000 in one run, 416000 in another, suspiciously
close.  Memory is tight (about 6.7MB free, 31MB active on a 68MB
machine).  Whether that is a resource limit, a guest-side shutdown, or
something reached at a particular point in the boot is unknown.

### fbshot: seeing the screen without a human

`fbshot.c` captures the framebuffer to a PPM, expanding 8-bit indices
through the hardware CLUT read with WSDISPLAYIO_GETCMAP.  It is read-only
and never changes the display mode, so it is safe to run while a DGA
guest owns the display.

Retrieve with:

    ./fbshot screen.ppm && gzip -9 screen.ppm
    # then, from the build host, over rexec:
    od -An -v -tx1 screen.ppm.gz | tr -d ' \n'

and unhex/gunzip at the other end.  Every question about this port until
now needed someone at the screen; this answers them directly.

### Testing note

Kill runs with SIGINT, never SIGKILL.  The driver restores the palette,
the display mode and the kernel cursor in its destructor, and `pkill -9`
skips all of it -- which is why an X session was left black and white
after one run.

## Status: reaches "Welcome to Macintosh", does not get past it

Correction to an earlier claim here: this is NOT a completed boot.  Mac OS
displays the welcome screen and then makes no further progress, and runs
crash intermittently with

    Caught SIGSEGV at address 0x9fc0000 [IP=0x9fc0000]

IP equals the address, so the guest branches there -- the same signature
that last time turned out to be memory corruption rather than a bad
branch.

**Interrupt starvation is ruled out.**  Counting delivery in
`sigirq_handler` gives 601 ticks delivered against 287 deferred, so the
guest gets roughly two thirds of its 60Hz interrupts.  That was the
leading theory and it is wrong.

**Tested and eliminated: the 0x40800000 ROM mapping.**  It is now a
genuine alias -- one temporary file, unlinked, mapped MAP_SHARED at both
0x800000 and 0x40800000, so a patch through either address is visible
from the other.  Both mappings succeed and the fault is unchanged.  Worth
keeping regardless, since two copies able to diverge is a latent bug, but
it is not the cause.

The fault is **deterministic**: 0x9fc0000 on every run, never a different
address.  Random corruption would scatter; something computes exactly
that value each time, which should make it findable.

The earlier reasoning, now disproved, was:  The ROM
now exists twice: at 0x800000 where Basilisk II places it, and at
0x40800000 as a `MAP_ANON | MAP_PRIVATE` copy satisfying the ROM's
baked-in absolute references.  Those are independent pages.  MacOS
patches its ROM in place during startup, so a patch applied to one copy
is invisible in the other -- and code executing from 0x40800000 would run
unpatched.  A boot that reaches the welcome screen and then wanders off
fits that exactly.

If so, the fix is to make the second address share the *same* memory
rather than duplicate it, so both views stay identical.

## Earlier status: IT BOOTS (overstated -- see above)

Mac OS starts on the Centris 650 and reaches "Welcome to Macintosh" --
the ROM finds the disk, loads the System file and hands control to it.
The guest runs natively on the 68040 with `driver_wscons` drawing
straight into the wscons framebuffer.  10317 traps, zero faults, over a
sustained run.

What it took, after the port would build:

1. **`--screen dga` never parsed.**  `video_x.cpp` accepts `dga/...` only
   under the other two backends, so every run silently used the windowed
   driver.  That alone accounted for days of confusing evidence.
2. **The image was linked on top of the guest.**  NetBSD/m68k links at
   0x2000, exactly where guest RAM must start; and then 0x10000000
   collided with `MAP_BASE`, the emulator's own mapping arena, so
   `vm_acquire_mac` mapped scratch memory over our text.  0x08000000 sits
   clear of both.
3. **The 60Hz tick corrupted non-guest context**, injecting Mac interrupt
   frames holding host PCs.
4. **`driver_wscons` never called `set_mac_frame_buffer()`**, so MacOS
   was never told where the screen is and drew over the video driver in
   low RAM.  This was ours, and the single most damaging omission.
5. **MacOS writes past the end of the screen.**  The kernel will not map
   beyond `sc_fbsize`, so the mapping is reserved with anonymous slack
   behind the device pages to absorb it.
6. **The ROM holds absolute references to 0x40800000**, its physical base
   on real hardware.  Unusually for this port, the guest *is* running on
   that hardware, so a second copy of the ROM is simply mapped there.

### Known issue: two cursors

`startxws` defaults to `-kcursor`, so `dafbcons` draws the X pointer into
the framebuffer from the vblank interrupt -- and a DGA guest owns that
framebuffer.  The Mac cursor appears briefly and is then repainted over
on any mouse movement.  `KCURSOR=no startxws` avoids it.

The proper fix is for `driver_wscons` to stand the kernel cursor down
while it holds the display, the same save/take-over/restore it already
does for the palette, through the `hw.dafbcons.cursor` sysctl.  Then
`-kcursor` can stay default for ordinary X use, where it is what fixed
pointer tearing.

### Still to do

Pare back the temporary instrumentation in `main_unix.cpp` and
`video_x.cpp` -- extensive, marked TEMPORARY, and it earned its keep.
Revisit the 0x40800000 mapping, which suits this machine rather than
being a general patch.

## Status: driver_wscons proven; guest dies in a truncated driver copy

**driver_wscons coexists with X correctly.**  Tested with the DGA path
actually selected (the earlier wedge was the windowed driver, which we
were unknowingly running): the emulator runs, and `xdpyinfo` answers
throughout -- **X ALIVE**.  The backend maps the framebuffer, grabs
input, drives the palette and does not disturb the server.

The guest boots ROM code natively, opens the video driver and calls its
Control routine successfully, then faults.

**The fault is precisely located.**  The slot ROM declares the video
driver with Open at 0x32, Prime 0x36, Control 0x3a, Status 0x46, Close
0x6c.  The Control EMUL_OP executed at guest 0x5f8a, so the driver base
in RAM is 0x5f8a - 0x3a = 0x5f50.  The fault is at 0x5fbc, which is
0x5f50 + 0x6c -- exactly the **Close** entry point.  It contains 0xff00
fill.

The slot ROM itself is not at fault, twice over: it is placed correctly
(size 1330 at Mac 0x008fface, carrying the "Basilisk" identifier), and
the emitted driver is complete -- counting from Status at 0x46 the code
runs to `Word(0x70e8)` at exactly 0x6c and ends at 0x70, inside the
declared length 0x72.

So the driver is **truncated when MacOS copies it out of the slot ROM
into the system heap**: Control at 0x3a survives and runs, Close at 0x6c
does not.  Establishing why that copy is short is the next task -- it is
a bounded question with the source and destination both known.

## Status: boots deep, then executes unmapped-space fill

Real fix landed: **the 60Hz interrupt was corrupting non-guest context.**
`sigirq_handler` fires on a timer and can land anywhere -- inside the
emulator, inside libc, in another thread -- and the `EmulatedSR & 0x0700`
guard only covers EmulOp.  When it landed outside guest code it built a
Mac interrupt frame containing a HOST pc; MacOS later RTE'd to it and
jumped into nowhere.  Caught red-handed: a frame with SR 0x2010 (exactly
`sc_ps|EmulatedSR` from that handler) and pc 0x0c51c5a0, an address in
the host's shared-library region.  It now declines to interrupt anything
outside RAM+ROM and drops the tick; another arrives in 1/60s.

With that, the emulator no longer crashes on the RTE path and exits
cleanly.

**Remaining fault.**  The guest still walks into a region around
0x5f80-0x5fc0 filled entirely with `0xff00`:

    00005f8c: ff00 ff00 ff00 ff00
    expected at Control(): 7119 0c68 0001 001a 6604 4e75

`0xff00` repeated is the signature of reads from unmapped address space
floating high, not of anonymous guest RAM (which is zero-filled).  The
faulting opcode is `0xff00` (F-line) and the address we chased for hours,
`0xff00ff00`, is simply two of those words read as a pointer.

The code that *should* be there is the video driver's Control routine
from the slot declaration ROM (`slot_rom.cpp`), which emits
`M68K_EMUL_OP_VIDEO_CONTROL` followed by real 68k code and is copied to
`ROMBaseHost + ROMSize - slot_rom_size`.  That placement was verified as
correct.

A bootable disk was supplied and transferred (200MB `Applesex.hfv`, HFS,
volume "Macintosh HD", System + MacsBug boot blocks) and does NOT change
the outcome -- so the fault is not "nothing to boot from".

**Next lead**: whether the guest's reads of NuBus slot space are mapped
at all.  The `0xff00` float pattern is what unmapped bus reads look like,
and the video driver lives in slot space before MacOS copies it out.

## Status: Mac OS boots deep into ROM, then a bad exception return

The guest gets a long way.  Instrumenting `sigill_handler` -- where every
privileged instruction and EMUL_OP escape lands -- counts **3453 traps
across 244 distinct PCs** before the failure, and the mix near the end is
real Mac OS startup:

    op=7129  EMUL_OP        op=4e73  RTE
    op=f4f8  CPUSHA         op=a029, a055  A-traps
    pc=00005f8a             <- executing code copied into RAM

**Two earlier claims here were wrong and are retracted.**

*"EMUL_OP dispatch loops without advancing."*  It does not.  The repeated
PC was the ROM walking XPRAM in a tight loop -- `CLKNOMEM` showed `d1`
incrementing (`0x000000b8`, `0x000004b8`, `0x000008b8`), which decodes to
register 0, 1, 2.  Normal behaviour, not a hang.

*"a7 never changes, so the register saves are not taking effect."*  Also
wrong: `EmulOpTrampoline` ends `moveml sp@+,d0-d7/a0-a6` / `addql #4,sp`
/ `rtr`, restoring the stack it was given.  `a7` returning to its former
value is what success looks like.  Tracing confirmed the push does happen
(70 bytes) and the PC is stored at offset 66 where the trampoline expects
it.  The ucontext port of the handler is working.

**The actual failure** is an exception return to a nonsense address:

    RTE to implausible pc=0c51c5a0 (sr=2010 format=0 adj=0
                                    oldA7=0040fb58 newA7=0040fb60)
    Caught SIGSEGV at address 0xff00ff00 [IP=0xff00ff00]

The popped SR (`0x2010`) is plausible; the PC is not.  RTE consumed 8
bytes, correct for a format-0 frame.

Both emulated exception pushes were checked and are correct 8-byte
format-0 frames: the A-line path writes vector word `0x28`, then PC, then
SR; the interrupt path writes `0x64`, PC, SR.  So the frame layout is not
the bug.

**Next**: find who wrote that frame.  Either something corrupts the guest
stack earlier, or an exception is taken on a path that does not build a
frame at all.  Worth checking the interrupt path specifically --
`sigirq_handler` was also ported to ucontext, fires at 60Hz, and pushes
onto whatever stack the guest happens to be using.

## Status: the guest executes; it faults early in ROM startup

Startup now runs to completion and hands control to the 68040:

    mtrace: ScratchMem=0x10008000
    mtrace: reading ROM to 0x800000 size 1048576
    mtrace: ROM read ok; entering InitAll
    wstrace: CONSTRUCTOR COMPLETE
    mtrace: InitAll returned
    mtrace: installing signal handlers
    mtrace: about to Start680x0 (guest begins)
    Caught SIGSEGV at address 0xff00ff00 [IP=0xff00ff00]

IP equals the fault address, so the guest **jumped** there rather than
reading it -- it branched through a pointer holding `0xff00ff00`, a fill
pattern rather than a real address.  `0xff00ff00` appears nowhere in the
source, so it came from memory the ROM read.

The ROM itself is fine: its version word is `0x067c`, which is exactly
`ROM_VERSION_32`, so `CheckROM()` passes and the patches apply.

Entry is `Start680x0` in `asm_support.s`: guest stack at RAMBase+0x8000,
then `jmp (a0)` with `a0 = ROMBaseHost + 0x2a`.  So the ROM runs from
0x80002a and faults later, somewhere in its own startup.

Next: find how far the ROM gets.  `--break ADDRESS` sets a ROM
breakpoint, and the patches in `rom_patches.cpp` are the place to look
for whichever hardware probe is not being intercepted.

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
