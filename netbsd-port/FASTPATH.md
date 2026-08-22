# bfast: an in-kernel trap fast path for Basilisk II on NetBSD/mac68k

## Why

Measured on the Disk Tools 7.1 desktop: the machine spends 94% of its CPU
in system time delivering emulation traps as POSIX signals, at ~1.3ms per
trap (exception -> signal frame -> handler -> setcontext -> restore).  The
trap mix is 59% SR bookkeeping, 35% A-line, 6% EMUL_OP.  A trap handled at
trap level costs ~15us.  Handling the first two classes in the kernel takes
~94% of traps off the signal path, a ~12x cut in trap load; the EMUL_OP 6%
must keep crossing into userland (the device models live there) and bounds
the gain.

This is the hosted-hypervisor split: Basilisk II is the vmx (device
models, EMUL_OP), the module is the vmmon (fast privileged transitions).
No binary translation is needed -- the 68020+ traps on every sensitive
instruction -- and no world switch: the guest is an ordinary process.

## Where

This module lives in the Basilisk II fork, not the OS repo: it exists
solely to make this emulator fast.  It is coupled to NetBSD 10.1 kernel
internals and builds against the kernel source tree in the build
container, like the rest of the port.

## Hook: VBR swap, no kernel patching

NetBSD/mac68k runs with the CPU's VBR pointing at `vectab`
(`movc %d0,%vbr`, locore.s:259; table in mac68k/vectors.s).  At load the
module allocates a 256-entry table, copies `vectab`, replaces entries 8
(privilege violation) and 10 (A-line) with its own stubs, and `movec`s
the VBR to the copy.  Unload is the reverse `movec`.  The running
kernel's own table is never written; unload is safe at any moment because
the stubs chain to the original handlers for everything they do not
claim.

Vector 4 (illegal instruction) is NOT hooked: EMUL_OP `0x71xx` decodes as
an illegal moveq form and arrives there, so the userland escape path is
untouched by construction.

## Fast-path gate (in the stub, in order)

1. S bit set in the pushed frame's SR -> trap came from supervisor mode:
   chain to the original vector.
2. `_C_LABEL(curpcb)` != the registered pcb -> not our process: chain.
   (One compare; the pcb address is stable for the life of the lwp.)
3. Fetch the opcode at the frame PC with `moves` under `PCB_ONFAULT`
   protection (the `suline` pattern, locore.s:918).  Fault -> chain.
4. Opcode outside the handled set -> chain.

Chaining means: undo nothing, jump to the saved original vectab entry.
The signal path therefore remains complete and correct; the module is
purely an accelerator and Basilisk runs unmodified without it.

## Handled set and semantics (mirror main_unix.cpp exactly)

Virtual SR: Basilisk registers the user addresses of `EmulatedSR` and
`InterruptFlags`.  The stub reads/writes them with `moves` in process
context.  The synthetic SR seen by the guest is
`(frame ccr & 0x1f) | EmulatedSR`, same as userland's GET_SR; only
condition-code bits ever enter the real frame SR (the exit(22) lesson --
NetBSD rejects PSL_MBZ|PSL_IPL|PSL_S, and an RTE here feeds the frame
that the kernel later restores).

- `ori/andi/eori #imm,sr` -- update EmulatedSR + frame ccr, pc += 4.
- `move ea,sr` for dn/(sp)+/#imm (46c0-7, 46df, 46fc) -- likewise.
- `move sr,ea` for dn/-(sp) (40c0-7, 40e7) -- write synthetic SR out.
- `rte` -- pop SR/PC from guest a7; frame format nibble must be 0,
  anything else chains to the signal path.
- `stop #imm` -- load SR as above; leave pc past it; do not sleep.
- `cpusha` -- real `cpusha bc`; cheaper in kernel than the userland
  FlushCodeCache round trip.
- A-line -- push the 6-byte frame userland pushes today
  ({SR, PC, $0028} downward on guest a7), set frame pc from guest
  address 0x28 (guest vector table is at VA 0; the guest runs with
  vm.user_va0_disable=0).  The guest's own dispatcher runs entirely in
  user mode; its terminating `rte` lands back in this stub.

Fallback-to-signal cases (correctness lives in userland, keep it there):
- any SR write that LOWERS the interrupt mask while `InterruptFlags` is
  nonzero (userland must run TriggerInterrupt);
- `rte` with a nonzero frame format;
- movec, stop-with-wait semantics if ever needed, anything unrecognised.

Interrupt delivery (SIGURG -> sigirq_handler) is unchanged: it already
works and is only ~60/s.

## Interface

sysctl, following the dafbcons precedent:
- `kern.bfast.attach` (write: pid + the two user addresses, packed
  struct) -- registers the caller; one process at a time; root only.
- `kern.bfast.detach` -- clears it.  A dead process is also harmless
  without detach: its pcb never matches again (gate 2).
- `kern.bfast.stats` -- per-class counters (fast-pathed, chained, by
  reason), so the census can be re-read from the kernel side.

Basilisk side: one probe + attach at driver init, detach in the
destructor, behind a pref (`fastpath true`).  No other emulator change.

## Bring-up phases (each independently deployable and testable)

- P0  Skeleton: load, copy vectab UNPATCHED, movec VBR, sysctl stats,
      unload.  Proves the swap is a no-op.  Risk: near zero.
- P1  Stubs installed but handling nothing: gate + count + chain only.
      Proves the gate logic under full load.  Counters must match the
      userland histogram.
- P2  SR family fast-pathed.  Basilisk's histogram (it now sees only
      signal-path traps) should show the SR class gone; system time
      should drop by roughly its share.
- P3  A-line reflection + rte.  The big one.  Histogram shows only
      EMUL_OP; measure the real speedup.
- P4  Tidy: drop the userland census printfs to a debug flag, write the
      results into this file.

## Risks, stated plainly

A bug in a trap stub panics or wedges the physical machine (fsck on
reboot, minutes).  Bring-up is sequenced so each phase risks little:
P0/P1 change no behaviour, and every stub path ends in either a clean
rte or a chain to the stock handler.  The machine is UP, so no SMP
hazards; trap entry does not raise IPL, and the stubs use only the
kernel stack and may be interrupted safely.

Known-unknowns to watch at P3: guest RTEs of non-zero-format frames
(chained, so correct but slow if common); the trace bit (ignored, same
as userland today); interaction with genuine page faults on guest stack
pushes mid-stub (PCB_ONFAULT covers the access, chain on fault, the
signal path then repeats the work with full fault handling).

## P0 result (2026-08-22)

Proven on the machine.  Two modload/modunload cycles while Basilisk II
ran at full trap load: `kern.bfast.swapped=1` after each load, 100k+
guest traps flowed through the copied table between load and unload
(histogram 700000 -> 800000), the sysctl tree came and went cleanly, and
machine and guest stayed healthy throughout.  One detail worth keeping:
the kernel runs with VBR=0 -- the live table sits at virtual address
zero -- so `vbr_orig` reads 0 and the restore is a movec of 0, which is
correct and was verified by the machine surviving it.

Build note: the module compiler targets the 68020 baseline, so 68040-only
instructions (cpusha) must be emitted as raw opcodes (`.word 0xf4f8`).

## P1 result (2026-08-22)

Gate proven exact.  Over a boundary-aligned 100k-signal-trap window:
kernel priv +54178 vs census other +53509 (1.25%), kernel aline +39744
vs census 39229 (1.3%) -- both within the 2s sampling skew -- and both
chain counters zero across the whole run.  The S-bit and curpcb tests
classify every trap correctly under full load.

## P2 result (2026-08-22)

The SR family is emulated at trap level and the guest boots to the
Finder on it.  Steady state: n_fast 529/s with a ~1% defer rate (the
designed fallbacks: pending-interrupt mask lowering, unhandled opcodes).
The A-line rate ROSE from ~260/s to 551/s -- the CPU freed by the fast
path lets the guest generate work faster -- so total guest throughput is
up ~1.6x while system time only fell 94% -> 86.6%: the A-line signal
path absorbs all slack, exactly as the census predicted.  P3 is the
payoff phase.

Implementation notes that were not in the plan:
- The stub hands a moveml-saved register block to a C handler at trap
  level; C either emulates and the stub RTEs straight back to the guest,
  or declines BEFORE MUTATING ANYTHING and the stub chains.  ufetch/
  ustore carry the user-access fault handling.
- moveml mask asymmetry: save with #0xffff (all-ones dodges the
  predecrement bit-reversal), restore with #0x7fff so the stacked a7
  slot never reaches the real SSP.
- The virtual SR stays in userland (EmulatedSR/InterruptFlags, addresses
  registered via kern.bfast.uaddr_*); the kernel reads InterruptFlags
  locklessly, safe because TriggerInterrupt also raises SIGURG itself.

## P3 result (2026-08-22)

A-line reflection works and delivers the payoff.  The guest boots to the
Finder with both trap classes handled at trap level:

                    signals only   P2 (SR only)   P3 (SR + A-line)
  trap throughput       733/s        ~1100/s         34,718/s
  CPU user/system       6%/94%       12%/87%         25%/75%

47x the trap throughput of the signal path.  The remaining system time
is the fast path doing real work (34,718 x ~20us =~ 70%), not delivery
overhead.  Signal-path traffic is down to EMUL_OP plus a <1% defer rate
(619 defers against ~10^6 fast traps at measurement time); the userland
census, which now sees only signal-delivered traps, had not reached its
first 100k dump when this was recorded -- it used to reach it in under
three minutes.

The guest's Toolbox dispatcher runs entirely via kernel reflection: push
{SR, PC, $0028} on the guest stack, vector through guest 0x28, and the
dispatcher's terminating RTE lands in the vector-8 handler that P2
proved.  The emulator process is not woken at all for either class.

## Current state and how to run it

Working: the module loads, attaches to one emulator process, and handles the
SR family and A-line reflection at trap level.  Disk Tools 7.1 boots to the
Finder and runs indefinitely on it.  Bring-up phases P0-P3 are all proven on
the machine; P4 (tidying the userland census behind a debug flag) is not done,
and is harmless as it stands -- the census now fires only every 100k
SIGNAL-path traps, which with the fast path loaded takes a very long time.

    # build both, in the container
    docker run --rm -v nbwork:/work -v "$PWD":/out debian:12 \
        sh /out/netbsd-port/build-bfast.sh
    docker run --rm -v nbwork:/work -v "$PWD":/out debian:12 \
        sh /out/netbsd-port/build-basilisk.sh

    # on the target, as root
    /sbin/modload ./bfast.kmod
    ./runbii.sh            # writes run2.log / status2.txt
    /sbin/sysctl kern.bfast

`runbii.sh` on the target carries the invocation, which matters: the RAM size
must be **8MB** (`--ramsize 8388608`).  The default is too small for System 7.5
and 16MB relocates the ROM from 0x800000 to 0x1000000, changing the memory map
entirely.  That invocation was reconstructed twice by guesswork at the cost of
a run each time; it is written down now so it need not be guessed again.

Attach is automatic: BasiliskII registers the addresses of EmulatedSR and
InterruptFlags and then its own lwp, immediately before Start680x0, and detaches
in QuitEmulator.  With the module absent every sysctl simply fails and the
emulator runs unchanged on the signal path.

Counters worth watching: `n_fast` (handled at trap level), `n_defer` (ours,
declined to the signal path -- expect well under 1%), `n_chain_priv` /
`n_chain_aline` (traps from other processes or supervisor mode; these should be
exactly zero, and have been across every run).

## Not done

- P4 tidy: the userland opcode census and the trap ring are still compiled in
  unconditionally.  They cost two stores per trap and earn their keep while the
  0x9fc0000 bug is open; they should go behind a flag once it is closed.
- System 7.5 is unvalidated, blocked by that bug (see README.md).  Every
  measurement here is from Disk Tools 7.1, which is a lighter and more
  idle-heavy workload than a full System with extensions; expect a larger
  EMUL_OP share, and therefore a smaller multiple, on real work.
- The module is 68040-only and NetBSD 10.1-specific: it reads curpcb, assumes
  the mac68k VBR arrangement, and emits cpusha as a raw opcode because kmod
  builds target the 68020 baseline.
