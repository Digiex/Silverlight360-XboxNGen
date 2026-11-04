# XboxNGen

Native images (`.ni.dll`) for Xbox 360 Silverlight apps, made by the console runtime's own zapper. Part of the
[Silverlight360](https://github.com/Digiex/Silverlight360) project:
[github.com/Digiex/Silverlight360-XboxNGen](https://github.com/Digiex/Silverlight360-XboxNGen).

The Xbox 360 Silverlight CLR, `xcoreclr.dll`, has no JIT on a retail console: app code runs through its IL interpreter
unless the assembly has a native image. Microsoft made the platform's and the launch apps' native images with a JIT
that never shipped. This project supplies one.

**How it works** ([docs/DECISION.md](docs/DECISION.md), [docs/RESEARCH.md](docs/RESEARCH.md)): retail `xcoreclr.dll`
still contains the whole NGen pipeline (the zapper, the runtime's image writer) and only lacks the JIT, which it loads
as `xclrjit.dll`. So compilation runs on the Xbox side, under xenia-edge:

| Piece | What it is |
|---|---|
| `jit\xclrjit.dll` | Our JIT, a PowerPC XEX DLL built with xsdk's GCC. Exports `sxsJitStartup` and `getJit` by name. |
| `host\ngenhost.xex` | A small title that loads `xcoreclr.dll` and calls its `NGenWorker` (export 1) for each assembly in `ngen.txt`. |
| `codecache\CLRCodeCachedll_0.xex` | The code cache module xcoreclr generates run-time code through, which a retail console lacks. |

The repository's folders split by who needs them:

- **`tools\`: making native images.** All that using XboxNGen on your own assemblies takes
  ([Making native images](#making-native-images)).
- **`test\`: developing the JIT.** The test apps and the scripts that check a JIT change against them
  ([Testing the JIT](#testing-the-jit)).
- **`research\`: looking inside images.** Disassemblers, image readers and the comparisons with Microsoft's images,
  for working out how the runtime and Microsoft's compiler behave ([Research tools](#research-tools)).

## Status

- **Working end to end:** the zapper loads our JIT, compiles, writes the image; the image loads in an app and its
  code runs (`test\NGenProbe` shows a value only our compiled code produces).
- **The JIT is a baseline compiler** (`jit\compiler.c`): arguments, locals, constants, integer, 64-bit and
  floating-point arithmetic, comparisons, branches and `switch`, conversions, instance fields with write barriers,
  indirect loads and stores, calls (static, instance, virtual through the vtable, through interfaces, into instantiated generic classes),
  `newobj`, strings, `box`/`unbox.any`, `castclass`/`isinst`, single-dimension arrays of 1- to 4-byte elements and
  references, class constructors, `throw`, delegates (`ldftn`, `ldvirtftn`, delegate construction, `calli`, and the
  runtime's delegate stubs), static fields (also in other modules, of value types too), floating-point arguments,
  value types (structs of up to 136 bytes, with or without references: locals, arguments, results, fields, arrays,
  `new`, `box`/`unbox`, `IntPtr`'s constructor), calls of generic method instantiations and exact instantiations'
  bodies (`Pick<int>`), shared generic bodies (`Pick<string>`'s, taking the hidden instantiation argument) with
  their runtime lookups (`new T[]`, casts to T, `typeof(T)`, `new List<T>()`, calls of generic methods from
  shared code through `getCallInfo`), `constrained.` calls, `typeof`, array initializers (`ldtoken` of a field), `fixed`
  (pinned pointers), `[ThreadStatic]` fields, calls with up to 128 argument slots (the outgoing area sized per method, as Microsoft's compiler does). Every
  frame slot holding a reference is reported to the GC, the words of value types at the safe points where they
  hold one.
- **Exception handling:** `try` with `catch`, `finally` and `fault` handlers, nested trys, `leave` through any number
  of finallys, `rethrow`. Handlers are funclets after the method's body, laid out as Microsoft's compiler does
  (r31 frame, PSPSym, funclet `.pdata` entries, duplicated clauses for nested handlers); a leave calls a finally
  funclet from a thunk after the try.
- **Also:** `stackalloc` (`localloc`, in an r31-framed method laid out as Microsoft's compiler does), checked
  arithmetic and conversions (`add/sub/mul.ovf`, `conv.ovf.*`, signed and unsigned, 32- and 64-bit), arrays of
  `long`/`ulong`/`double`, `volatile.` (fenced with `lwsync`), `no.` and `readonly.`, a generic class's static
  fields (from exact and from shared code), `ldelema` of reference types (the exact-type check through the
  runtime's helper), `initobj` of a `TypedReference`, `sizeof`, the `Microsoft.Xbox.FPU` intrinsics (`fsel`,
  `fabs`, `fsqrt`, `fpmin`/`fpmax`, the conversions: internal calls with no implementation in the runtime that
  the compiler turns into the PowerPC instruction), value types smaller than a word or not a whole number of
  words (a boxed `char`, `bool?`, a struct of three shorts: copied to the byte), `newobj` of a string,
  `Type.GetTypeFromHandle` as the intrinsic it is, generic-struct methods with both an instantiation argument and
  a return buffer, `rem` of doubles and checked conversions from doubles (the runtime's helpers), `cpobj`,
  `initblk`, `cpblk` and `unaligned.` (the runtime's marshaling stubs use them), `ldftn` of a generic method's
  instantiation over T from shared code (the entry point from the generic context's dictionary), the generics
  context slot in the GC info of shared generic code (what the runtime's stack walks read for the exact
  instantiation), generic virtual methods (`Iterator<T>.Select<TResult>`: the implementation for the object and
  the exact instantiation from the runtime's virtual-function-pointer helper, like an interface call).
  With these, every method of Vimeo's System.Core (1,594) and Newtonsoft.Json (3,940) that Microsoft's images
  hold compiles. Also floating-point arguments whose slot is on the stack (in their register and in the slot),
  generic callees returning a value type through a buffer from shared code, multi-dimensional arrays (the
  runtime's vararg constructor helper and its accessor methods, `Address` with its hidden type argument),
  value types up to 4 KB (the marshaling stubs' 0x130- to 0xCF8-byte structs), methods of any length (64K words of
  code, thousands of calls and relocations, conditional branches beyond 32 KB), `TypedReference` results and
  `refanytype`, a leave from a try inside a catch out past an enclosing finally.
- **Declined (left to the interpreter):** exception filters, more than 13 floating-point arguments, `ldtoken` of
  a method, `tail.`, `refanyval`, calls with stack arguments inside a handler. None of these occurs in the Vimeo
  package or mscorlib.
- **Code:** a simple compiler, 1.21 times the size of Microsoft's code over the Vimeo package (1.74 before the
  register work). Evaluation stack entries stay in registers (r3-r10, f1-f10, the argument registers) through
  straight-line runs of simple instructions and into calls, which take their arguments from there; a method that
  calls nothing is compiled without a frame, its variables in registers (floating-point arguments and 4- and 8-byte
  value types too); a method with a frame keeps its most used integer and reference
  variables in callee-saved registers (r30 down to r17, saved in the prolog, reported to the GC as register slots)
  and its floating-point ones in f31 down to f14, except those its handlers use, and loads of them cost nothing; reference stores call the write barrier variant for the
  register the value is in, keeping the other entries in theirs; results go straight into the register of the
  variable they are stored to; casts skip the helper when the object's class is the target; array accesses keep
  the stack in registers, and stelem.ref of the element class itself skips its helper; floating-point constants
  come from the method's read-only data. Frames reserve value-type room only when used and clear only the variables' GC slots. Entries still
  go to frame slots at joins and across calls other than plain ones, and methods with handlers keep their
  variables in the frame.
- **Tested in xenia-edge:** `test\NGenBench` passes all 131 checks natively, and does so running entirely on our own
  images of mscorlib, the fourteen platform assemblies it loads and itself (`test\Test-Mscorlib.ps1`,
  `tools\NGen-Platform.ps1`), including GC stress tests in plain code
  and inside a catch and a finally, `using`, `foreach` over iterators, delegates of every kind, statics, and struct and floating-point arguments and results; against the interpreter, `SumTo(3,000,000)` goes from 264 ms to 2-3 ms, `Fib(24)`
  from 619 ms to 1 ms and a Collatz loop from 47 ms to 1-2 ms. Its timing loops of a few hundred ms each
  (StringBuilder, Dictionary, List<T>, an object field loop, Mandelbrot, Math calls; `test\Bench-Timing.ps1` runs
  them on Microsoft's images and on ours) take 130-330 ms each compiled; since NGenBench's own code can only be
  interpreted on Microsoft's images, the library-bound loops compare the libraries: ours ran StringBuilder and
  Dictionary 0.86 and 0.84 times as fast as Microsoft's before the callee-saved registers, and 1.07 and 0.96 times
  after (xenia's run-to-run spread is around 15%, so only runs made back to back compare). The SDK's `HelloXbox`, `XboxControls`,
  `PageNavigation` and `FocusNavigation` run and render on our images of the whole platform and of themselves
  (`test\Run-OnOurs.ps1`, which stops on each sample's "<name>: ready" line; each saves a screenshot of its own
  drawing just before it, which `Run-App.ps1` keeps as the run's PNG), as they do on Microsoft's.
- **Compared with Microsoft's images** of the whole Vimeo package (`research\Compare-Package.ps1`: every one of the
  69,126 methods the zapper offers in the 34 assemblies compiles, no declines; Microsoft's images hold 69,168 bodies,
  the 42 more being bodies their zapper run produced that ours never requests; docs/RESEARCH.md) and of Plex's five
  assemblies, paired method by method: the same exception handling structure and GC header on every pair; the
  differences are code quality (frames and larger code where Microsoft's is frameless and tight, helper calls where
  it inlines). The zapper needs xenia-edge in devkit mode with a code cache module to finish the larger assemblies.
- **On hardware:** Silverdash360's native image runs on a real 2.0.17559 console that runs unsigned code (the images
  are unsigned, so a stock retail console won't load them), its benchmarks 28 to 300 times faster than interpreted
  and its test suites unchanged; the console's hypervisor wants a page descriptor per 64 KB (docs/RESEARCH.md).

## Making native images

What you need:

- [xenia-edge](https://github.com/has207/xenia-edge), built;
- [xsdk](https://github.com/xenosworks/xsdk) with its toolchain in WSL (`~/xsdk-gcc`) and `libxsdk.a` built, to build
  the JIT, the host and the code cache module. They need its DLL support (`tools/elf2xex.py --dll --export-name`),
  which is in [xenosworks/xsdk#4](https://github.com/xenosworks/xsdk/pull/4) until it is merged: check out that
  pull request's branch, `dll-modules` of [jessenic/xsdk](https://github.com/jessenic/xsdk);
- Python 3 (the standard library only);
- the package to compile: a Silverlight360 / Xbox 360 Silverlight app, as the
  [Silverlight360 SDK](https://github.com/Digiex/Silverlight360) builds it (its `default.xex`, `AppManifest.xaml`, the
  runtime and the assemblies).

```powershell
git clone https://github.com/Digiex/Silverlight360-XboxNGen
# Beside it, where Configure.ps1 looks: xsdk with its DLL support (xenosworks/xsdk#4, until it is merged). Build its
# toolchain and libxsdk.a as its own docs say.
git clone -b dll-modules https://github.com/jessenic/xsdk
cd Silverlight360-XboxNGen

# Once: record where xenia-edge and xsdk are, then build the JIT, the host and the code cache module in WSL.
.\tools\Configure.ps1
wsl -e bash -lc 'for d in jit host codecache; do make -C $d || exit 1; done'

# Make native images for a package's assemblies, then run it.
.\tools\NGen-Package.ps1 -Package <package folder>
.\tools\Run-App.ps1 -Xex <package folder>\default.xex
```

`tools\` holds:

| Script | What it does |
|---|---|
| `Configure.ps1` | Records the locations the scripts use (below). |
| `NGen-Package.ps1` | Stages a package with the JIT and the host, runs it in xenia-edge, then rebases and wraps each output. |
| `NGen-Platform.ps1` | Compiles a package's platform assemblies against its mscorlib.ni.dll, one NGen-Package run each, at following load addresses. |
| `Run-App.ps1` | Runs a package in xenia-edge until a DbgPrint line matches, it hangs, or exits; restarts on the emulator's startup scheduler stall. |
| `Run-Xenia.ps1` | Runs a staged package in xenia-edge, headless, until the title exits or a timeout, and prints its DbgPrint lines (`NGen-Package.ps1`'s zapper runs). |
| `pe2nixex.py` | Rebases the zapper's PE (based at 0) to a load address (moving only words that point into the image: precodes hold xcoreclr's fixup thunk addresses under relocations too), sets `CORCOMPILE_HEADER.ImageBase`, wraps it in the XEX module xcoreclr loads. |
| `argb2png.py` | Turns a screenshot an app dumps into a PNG, for `Run-App.ps1`. |

`NGen-Package.ps1` compiles the `AppManifest.xaml` assemblies that have no native image yet (or those given with
`-Assemblies`), placing images from 0x95000000 up (`-BaseAddress`). It runs xenia-edge as a devkit
(`--console_type=0`) with `codecache\CLRCodeCachedll_0.xex` beside the runtime: the zapper has the runtime generate
some code itself (an array type's accessor stubs, for one), and on a retail console the runtime can't
([docs/RESEARCH.md](docs/RESEARCH.md), "Run-time code generation"). Compile an assembly whose dependencies you also
compile in a run of its own, dependents first (`-Assemblies PlexViewModel -BaseAddress 95200000`, then
`PlexNet` at another base): a freshly written image beside a dependency breaks the next compile.

`Run-App.ps1` checks every 5 seconds that xenia is alive, and stops when the app logs a done line (`-DonePattern`),
xenia exits or crashes (it then prints the Windows error record and the log's end), or the log stops growing
(`-IdleSeconds`); it saves a screenshot before stopping. xenia runs `--unattended`: dialogs such as the host's "Error:
2110 The application encountered an error" are logged and dismissed, so the run ends with the app, and Run-App prints
them (state `app error dialog`). Run one xenia-edge at a time: two instances at once can stop one with "Overflowed
stackpoints!", even for an app with no native image of ours.

The images are unsigned: they run in xenia-edge and on a console that runs unsigned code, not on a stock retail one.

### Configuration

`.\tools\Configure.ps1` finds each location (from its parameter, the environment, else beside this repository), asks
for xenia-edge, xsdk and the SDK if it can't find them, and records them for every later run in `config.json`,
`config.mk` and `config.props` at the repository's root (not committed). `-Show` prints the result; run it again with a
parameter (`-Xenia`, `-Xsdk`, `-Work`, `-Silverlight360`, `-Apps`) to change one. An environment variable overrides the
recorded value:

| Variable | What | Needed for | Default |
|---|---|---|---|
| `XENIA_EDGE` | `xenia_edge.exe` | everything | from `PATH` |
| `XSDK` | the xsdk checkout, with `libxsdk.a` built (for make, in WSL) | building | none |
| `XBOXNGEN_WORK` | the work folder: our mscorlib and platform images (`ours\`), staged app packages (`apps\`), timings | `test\`, `research\` | `work\` in this repository |
| `Silverlight360Dir` | the Silverlight360 SDK checkout the test apps build with (an MSBuild property too) | `test\` | `..\Silverlight360\`, beside this repository |
| `XBOXNGEN_APPS` | the folder of extracted app packages (`Vimeo\`, `Plex\`, ...) | `test\Test-Mscorlib.ps1`, `research\` | none |

## Testing the JIT

For changing the JIT. On top of the above:

- the [Silverlight360 SDK](https://github.com/Digiex/Silverlight360) and everything it needs to build an app, with a
  runtime profile made from an extracted Xbox 360 Silverlight app (the SDK's docs say how), to build the test apps;
- for `Test-Mscorlib.ps1`, extracted app packages (the SDK's `tools\extract_god_packages.py` extracts them from their
  Games on Demand archives); give their folder with `.\tools\Configure.ps1 -Apps <folder>`.

```powershell
git clone https://github.com/Digiex/Silverlight360   # beside this repository, where Configure.ps1 looks
.\tools\Configure.ps1 -Apps <folder of extracted packages>

# Compile mscorlib, the platform and NGenBench with the current JIT and run NGenBench's 131 checks on those images.
.\test\Test-Mscorlib.ps1

# One app on our images alone.
.\test\Run-OnOurs.ps1 -Package <Silverlight360>\samples\HelloXbox
```

`test\` holds:

| Piece | What it is |
|---|---|
| `NGenBench\` | The test app: 131 checks of what the JIT compiles (GC stress, handlers, generics, delegates, value types, floating point) and timing loops. |
| `NGenProbe\` | The smallest app: shows a value only our compiled code produces. |
| `Test-Mscorlib.ps1` | Compiles mscorlib (in the Vimeo package), the platform assemblies and NGenBench with the current JIT and runs NGenBench on those images alone (`-SkipMscorlib`, `-SkipPlatform` reuse the images made last time). |
| `Run-OnOurs.ps1` | Runs an app package on our images alone: our mscorlib and platform images in a copy, the app's assemblies compiled against them. |
| `Bench-Timing.ps1` | Runs NGenBench's timing loops on Microsoft's images and on ours. |
| `Compare-Run.ps1` | Runs a package on Microsoft's images and on ours alone and prints how far each got (end state, error dialogs, DbgPrint lines, last frame). |

## Research tools

For looking inside images: ours, Microsoft's and the runtime's. On top of the above:

- Python 3 with `capstone` (the disassemblers) and `pycryptodome` (Microsoft's encrypted images):
  `pip install capstone pycryptodome`;
- extracted app packages, for Microsoft's images to compare with;
- optionally [Ghidra](https://github.com/NationalSecurityAgency/ghidra), for the scripts in `research\ghidra\` (decompiling every function, listing `.pdata` functions).

The main ones:

| Script | What it does |
|---|---|
| `Compare-Package.ps1` | Compiles a whole package with our JIT and compares every method with Microsoft's image of it. |
| `nicompare.py` | Compares one of our images with Microsoft's for the same assembly, method by method (`methodnames.py` names the methods); the Plex comparison is summarized in docs/RESEARCH.md. |
| `nishow.py` | Disassembles one method of a native image with its GC and unwind info. |
| `xex.py` | An XEX's memory image (`-b`: the PE as it lies in memory, for `nicompare.py` and `ppcdis.py`) and its headers (`-l`), for Microsoft's encrypted, LZX-compressed images too (`lzx.py`). |
| `trace_names.py` | Names every function of a first-execution trace (`--trace_define_start/end`) across a package's images; `names_in_package.py` names single addresses. Diffing two apps' traces by method name found the declined method that kept HelloXbox from rendering. |
| `ppcdis.py`, `x86dis.py` | Disassemble a range of a PowerPC memory image, or of an x86 PE with a symbol list. |
| `ildump.py`, `gcinfo.py`, `pe.py` | IL of a method, a method's GC info, a PE's headers and sections. |

The rest are small single-purpose readers (relocations, strings, vtables, PDB publics, XEX hashes), each described at
its top.

## Next

How the compiler works and the runtime facts it rests on are in [docs/RESEARCH.md](docs/RESEARCH.md); the state of
the work and the remaining items, with a prompt for continuing it, are in [docs/HANDOFF.md](docs/HANDOFF.md):

1. Code quality against Microsoft's images: leaf methods for shared generic code and with reference stores,
   inlining `stelem.ref` into an `object[]`, and measuring against Microsoft's code again (sizes, the console).
2. The declines no app seen so far needs: exception filters, more than 13 floating-point arguments, `ldtoken` of a
   method, `tail.`, `refanyval`.

## License

Silverlight360-XboxNGen is licensed under the
[PolyForm Noncommercial License 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0)
with one additional condition; see [LICENSE.md](LICENSE.md). In short:

- Noncommercial use only: personal, hobby, research and other noncommercial projects.
- Any project made with it, including an app shipped with native images its JIT or tools made and work based on
  its code, must include a visible reference to https://github.com/Digiex/Silverlight360-XboxNGen.
- Copies of it must keep the license and its `Required Notice` line.

The license covers this repository only, not the Microsoft and third-party binaries the tools run or read.

## Trademarks

Microsoft, Xbox, Xbox 360, Silverlight and Windows are trademarks or registered trademarks of Microsoft Corporation.
Plex, Vimeo and the other app names mentioned here are trademarks of their respective owners. They are used only to
describe what this project works with. Silverlight360-XboxNGen is an independent project, not affiliated with,
endorsed or sponsored by Microsoft or any of these companies.
