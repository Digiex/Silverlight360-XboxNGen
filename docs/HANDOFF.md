# Handoff: where the work stands and what is left

Updated 2026-11-04 (afternoon), after the register, constant and leaf work. [README.md](../README.md) says what
the project is and how to use it;
[RESEARCH.md](RESEARCH.md) holds every runtime fact the compiler rests on. This file is the state of the work and
the list of what remains, with a prompt at the end for an agent continuing it.

## State

- The pipeline is complete: the retail `xcoreclr.dll`'s zapper loads our JIT (`jit\compiler.c`, a PowerPC XEX DLL
  built in WSL) under xenia-edge through `host\ngenhost.xex`; `tools\NGen-Package.ps1` rebases and wraps the output
  into `.ni.dll` XEX modules the runtime loads.
- **Verified in xenia-edge:** `test\NGenBench` passes all 131 checks running entirely on our own images of
  mscorlib, the fourteen platform assemblies it loads and itself (`test\Test-Mscorlib.ps1`, about 5 minutes).
  The SDK samples `HelloXbox`, `XboxControls`, `PageNavigation` and `FocusNavigation` run and render on our images
  of the whole platform and of themselves (`test\Run-OnOurs.ps1`, which stops on each sample's "<name>: ready"
  line). Each sample saves a screenshot of its own drawing just before that line (the SDK's
  `samples\Common\Screenshot.cs`: the root visual drawn into a `WriteableBitmap`, written to `game:\screenshot.argb`),
  which `Run-App.ps1` turns into the run's PNG (`tools\argb2png.ps1`; xenia runs with
  `--allow_game_relative_writes`). A capture of xenia's window (PrintWindow), the fallback for apps that save none,
  often gives xenia's stale splash frame instead. `test\Compare-Run.ps1` runs a shipped package on Microsoft's
  images and on ours and compares how far each got: Plex reaches its "Please sign in" screen on both. Seven more
  (2026-11-04, RESEARCH.md "Apps of other runtime builds"): Microsoft rebuilt the framework for every app and our
  images bind to the IL they were compiled from, so `Run-OnOurs.ps1` now compiles the whole platform in a package
  whose framework isn't Plex's. With that, Napster's 13 platform and 12 own assemblies compile without a decline
  (the zapper's "stack overflow" was this mismatch), but Napster exits after 5 s with a SecurityException that it
  doesn't get on Microsoft's platform images (open). IGN, Crackle and Netflix are of the 2011-11 runtime build, WSJ
  of 2012-05: our mscorlib compiles on IGN's (`Work
gen\ours-2011`), its System.Windows crashes the zapper. Their
  manifests' `System.Xml.Serialization` and `Microsoft.Xbox.RuntimeHelpers` are no files of their own: the binder
  unifies them with System.Xml and mscorlib. Without a network in xenia none of them would get past its first
  screens anyway.
- **On the console** (2026-11-04, Silverdash360's own image only, Microsoft's platform): with the callee-saved
  registers interp.loop 4.63 -> 4.00 ms, library.search 0.150 -> 0.140, cache.serialize 117.6 -> 112.8, the rest
  within noise; its suites unchanged (41 passed, the one known failure, which fails interpreted too). Its
  benchmarks mostly run Microsoft's platform code, which this doesn't change; our whole image set on the console
  (mscorlib and the platform too, which the app folder can hold) is the next measurement, with the risk that a
  broken mscorlib takes the dashboard and its FTP server down (have a way back first).
- **Feature parity on the Vimeo package** (`research\Compare-Package.ps1`): every method the
  zapper offers in its 34 assemblies compiles, mscorlib included; no declines are left. Microsoft's images hold 42
  bodies more (69,168 against 69,126; System.Windows 28 of them): bodies their zapper produced that ours never asks
  for, not declines (RESEARCH.md, "Bodies the zapper never asks for"; which ones is still open, since nicompare's
  row pairing is unreliable on System.Windows).
- **Code:** our `.text` over the Vimeo package is 1.207 times Microsoft's (2026-11-04 afternoon: 22,406,608
  bytes against 18,570,160; mscorlib 1.203, System.Windows 1.157, BouncyCastle 1.348;
  1.26 before the handler, floating-point, array, register and leaf work of the day, 1.29 with the callee-saved registers
  before the shared epilogs and the stores into variables' registers, 1.28 before them; measure with a Compare-Package run and the `.text` lines of `<Work>\*.compare.txt`). Evaluation stack
  entries stay in registers (r3-r10, f1-f10: the argument registers) within straight-line code and into calls (load_arguments moves them
  into place); methods that call nothing are frameless with their variables in registers; methods with a frame and
  keep their most used integer and reference variables in r30-r17 and floating-point ones in f31-f14, a
  handler's own excepted (RESEARCH.md, "Variables in callee-saved registers"); reference stores go through the
  per-register write barriers without a write-back; casts test the exact class first; array accesses keep the
  stack in registers, and stelem.ref of the element class itself or of null skips its helper. NGenBench's timing loops (`test\Bench-Timing.ps1`, three configurations):
  compiled they take 130-330 ms each; against Microsoft's library (NGenBench interpreted on both) ours runs
  StringBuilder 1.07 and Dictionary 0.96 times as fast (0.86 and 0.84 before). xenia shows little gain from
  registers over frame slots: on the console, a store and reload of the same slot is a load-hit-store stall, so the
  next hardware run (Silverdash, as on 2026-11-03) is the measurement that matters.
- The working copies live in the work folder (README.md, "Configuration"): `ours\` (our `mscorlib.ni.dll`,
  `platform-ours\` with the 14 platform images, `bench-ni-aside\` with Microsoft's images for reference sizing)
  and `apps\<sample>` (packages staged by `Run-OnOurs.ps1`).

## Remaining work, in order

Fix issues before features; commit between steps; verify every JIT change with `Test-Mscorlib.ps1` (full
pipeline) and `Run-OnOurs.ps1` on HelloXbox as the rendering check (NGenBench never presents a frame: its package
has no title id).

1. **Code quality against Microsoft's output** (`research\nicompare.py` pairs methods; `research\nishow.py` prints one
   method's bodies from either image). Measure with NGenBench's timings and the code size totals of a
   Compare-Package run. In order of expected gain:
   - Done (2026-11-04): variables in callee-saved registers r30 down to r17 (`choose_saved_registers`, saved
     with `std` before the `stwu`, reported as register slots), entries that name a variable's register instead
     of copying it (and stay that way across plain calls), reference stores through the per-register write
     barriers without writing cached entries back or a safe point, a MethodTable-equality fast path before
     the cast helpers, one epilog per method that later returns branch to, and results computed straight into the
     callee-saved register of the variable the next instruction stores them to (`fuse_reg`).
   - The fusion looked broken for a while: PageNavigation gave xenia's stale splash in every window capture with
     it. The samples' own screenshots showed every sample drawing the same pixels with and without it; the window
     capture was wrong, and PageNavigation's page arrives over a second after its first frames, so it was the one
     most often caught early.
   - Done later the same day: methods with handlers keep the variables their handlers don't use in saved
     registers too, floating-point variables go in f31-f14, primitive ldelem/stelem are simple instructions and
     stelem.ref has an inline path.
   - Done in the afternoon (RESEARCH.md, "The evaluation stack's registers, constants and leaf methods"):
     floating-point variables aliased and fused like integer ones, FP operations in place; the stack cached in
     the argument registers (r3 + i, f1 + i); FP constants from read-only data and 64-bit ones in registers; leaf
     methods with floating-point arguments and with 4- and 8-byte value types; and a fix found on the way, `ldfld`
     on a value type the runtime reports as its one primitive field. mscorlib.ni.dll 9,572,352 -> 9,306,112 bytes.
   - The other stelem.ref case worth inlining, storing into an object[] (any reference fits).
   - Leaf methods still keep a frame for shared generic code (241 of 730 call-free methods in mscorlib), a
     reference store (125: the write barrier is a call; keeping the link register in a volatile register across
     it would do, if the runtime never walks a frame mid-barrier), and integer-to-float conversions (no
     GPR-to-FPR move without memory). To bisect a fault by assembly, `Test-Mscorlib.ps1 -Jit`
     takes another JIT DLL (one for mscorlib, then `-SkipMscorlib` with another for the platform); by method,
     `XNGEN_DECLINE`; the `XNGEN_NO_FUSE`, `XNGEN_NO_SHARED_EPILOG` and `XNGEN_NO_SAVE_ALL` builds turn the newest
     code generation off.
2. **The apps that don't run on our images yet** (above): Napster's SecurityException on its own platform build
   (Crunchyroll and GameTrailers are of the same runtime build: try them with `Run-OnOurs.ps1` first), and the
   2011-11 runtime's zapper crash on System.Windows (IGN, Crackle, Netflix), whose xcoreclr is unmapped.
3. **Declines no app seen so far needs:** exception filters (filter funclets, funcKind 2: what they receive and
   return is an open question in RESEARCH.md), more than 13 floating-point arguments, `ldtoken` of a method,
   `tail.`, `refanyval`, calls with stack arguments inside a handler. Each needs an NGenBench check written in IL
   or C# that produces it.
4. **The 42 bodies** Microsoft's images have and ours lack: name them (fix nicompare's pairing for System.Windows
   or compare the images' method lists another way), then see whether the zapper can be made to ask for them.
5. **Real apps:** run Vimeo and Plex on all-ours images once a network path exists in xenia-edge, and the other
   extracted app packages through `Run-OnOurs.ps1` (each may expose a new runtime fact).
6. **Emulator:** xenia-edge's GDB stub crashes on connect (`Processor::SuspendAllThreads` dereferences the null
   host thread of a fiber-scheduled guest thread); a working stub would let `research\gdbpeek.py` stop a hung guest.
   The startup scheduler stall (`Run-App.ps1` restarts on it, keeping the stalled run's log as `xenia.stall.log`)
   is still unexplained; it didn't occur in any run of 2026-11-04. Any xenia-edge kernel change
   must be based on a dump of the console's 2.0.17559 kernel, never guessed.

## Facts that cost the most to learn (do not rediscover)

- The JIT and the host are built with `-ffixed-r14 -ffixed-r15 -ffixed-r16`: xcoreclr owns r16 (the Thread) and
  r14/r15 (the write barriers' bounds) and they change across callbacks. A bug that comes and goes with logging
  or code layout is this kind first.
- The JIT's bss must end below 0x8D000000, where xcoreclr loads its code cache module (`xclrjit.ld` asserts it).
- Images bind to the dependency images they were compiled against: mscorlib, platform and app images must be
  compiled as a set (`Test-Mscorlib.ps1` does), and the runtime refuses platform assemblies without images
  (error 2101), so run-time JIT bisection is impossible; bisect by compiling with and without a feature instead.
- Array data offsets: primitives (including `long`/`double`) at +8, references at +12, value types under 16 bytes
  at +8 even with references, value types of 16 bytes and more at +16.
- Short image file names: System.Windows.Controls.Navigation → `SW.Controls.Navigation`,
  Microsoft.Web.Media.SmoothStreaming → `MWM.SmoothStreaming`, Microsoft.Xna.Framework → `MXF` (the scripts'
  alias tables).
- Outgoing stack arguments go through r1, never the frame register (a `localloc` moves r1).
- Helper 27 (`JIT_NewMDArr`) takes the array type handle in r3 and the constructor's MethodDesc in r4, then the
  dimensions; 403 is the virtual-function-pointer helper used for interface and generic virtual calls alike.
- The retail mscorlib's resources are a stub: bracketed exception messages are normal on Microsoft's images too.
- Diagnosing a misbehaving method on our images: trace first executions with
  `--trace_define_start=<decimal> --trace_define_end=<decimal>` (Run-App.ps1 `-ExtraArgs`), name them with
  `research\trace_names.py`, disassemble the suspect with `research\nishow.py`, read its IL with `research\ildump.py`; the
  JIT built with `-DXNGEN_DUMP_STUBS` logs IL stubs' IL, fixups and relocations.
- `Compare-Package.ps1` reuses images already in `<Work>\ours`: delete them (and `summary.txt`) to recompile.
- Run one xenia-edge at a time. Python heredocs mangle backslashes (Windows paths, `\n` in C strings): write
  scripts to files instead.

## Prompt for the next agent

```text
Continue the XboxNGen project (this repository; also checkouts of xenia-edge, xsdk (with its DLL support, from
https://github.com/xenosworks/xsdk/pull/4 until that is merged) and the Silverlight360 SDK,
set up as README.md, "Making native images" and "Testing the JIT", say). Read README.md, docs\HANDOFF.md and docs\RESEARCH.md first.

The goal: native images for Xbox 360 Silverlight apps with at least feature parity with Microsoft's native image
generator (reached for the Vimeo package: no declines), now better code. Our JIT is jit\compiler.c, built in WSL
with
  wsl -e bash -lc "make -C jit XSDK=~/xsdk"
and run by the retail xcoreclr zapper under xenia-edge through tools\NGen-Package.ps1.

Rules: fix issues when found, work on features after fixing issues, commit between steps. Verify every JIT
change in xenia-edge:
test\Test-Mscorlib.ps1 compiles mscorlib, the platform and NGenBench as a set and runs the bench (112 checks
must pass); test\Run-OnOurs.ps1 on the SDK's HelloXbox is the rendering check. Run one xenia-edge at a time.
Base any xenia-edge change on a dump of the console's 2.0.17559 kernel. Ask before installing tools. Add a check to test\NGenBench for every feature you add, and record every new runtime fact in
docs\RESEARCH.md.

Work the list in docs\HANDOFF.md "Remaining work" in order: code quality against Microsoft's output first
(frames and prologs, registers across calls, inlined helpers), measured with NGenBench's timings and a
research\Compare-Package.ps1 run of the Vimeo package (into a fresh -Work folder); then the
remaining declines. Update README.md's status and docs\HANDOFF.md as items close.
```
