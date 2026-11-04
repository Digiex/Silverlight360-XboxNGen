# Where the native image compiler runs

**Decision: the compiler runs on the Xbox 360 side, inside xcoreclr's own NGen pipeline, under xenia-edge (or an RGH/JTAG console). The PC orchestrates the build and wraps the output.**

Status of the evidence each point rests on: *observed* means read from the shipped binaries in this repository; *inferred* means concluded from observations.

## What a native image is

- A `.ni.dll` is an XEX2 DLL module: title|DLL module flags, uncompressed basic blocks, not encrypted, a fixed load address, the app's title ID, no imports (observed: all Plex `.ni.dll` files, `research\xex.py -l`).
- Inside is a big-endian PowerPC PE whose COR20 header points at a `CORCOMPILE_HEADER` of 0xBC bytes. Its data directories are little-endian; other fields big-endian (observed: `PlexResources.ni.dll`, `mscorlib.ni.dll`).
- Most of the image is not code. `.data` starts with the frozen `Module` (0x470 bytes in PlexResources) and holds the persisted `MethodTable`, `EEClass`, `MethodDesc` chunks, dictionaries and fixup cells that xcoreclr's type loader would otherwise build at run time; `.rdata` has a copy of the IL metadata, the import sections, per-method GC info and the 16-byte per-method records; `.text` has delay-load and helper thunks, then the compiled methods, each preceded by a 12-byte block (self-relative GC info pointer, handler `0x84BBBE90` in xcoreclr, handler data) (observed).

The data structures are the exact in-memory layout of this one CLR build (4.0.338.9242). Writing them requires the runtime's own type loader logic.

## What xcoreclr already contains

- The retail `xcoreclr.dll` contains the whole NGen pipeline: the zapper (`Zapper`, `ZapImage`, `ZapInfo`, the PE writer) and the runtime side (`CEECompileInfo`, `CompilationDomain`, `DataImage`, `Module::Save`). Strings unique to both are present: "Compiling input file %s", "Linking preloaded input file %s", "Unable to load Jit Compiler: %s", "Compilation Domain", "ZAP: WARNING: Type '%S' is too deeply nested, and will not be added to native image" (observed).
- What it lacks is the JIT. The zapper loads it with `XexLoadImage` from `xclrjit.dll` (or the `NGen_JitName` setting; `D:xclrjit.dll` is also referenced) and looks up `sxsJitStartup` and `getJit` by name with `XexGetProcedureAddress` (observed: decompiled `Zapper::InitEE`, CorCompileGetRuntimeDllName(NGEN_COMPILER_INFO)). No `xclrjit.dll` exists in any app, firmware or SDK file here (observed: file and content search).
- Microsoft compiled on the console: the runtime writes `D:\CompiledMethods.prof.txt`, `D:\MissingMethods.prof.txt` and refers to `D:\ExplicitInstantiations.ni.dll` (observed strings). A PC cross-compiler would not use `D:` paths (inferred).
- Desktop Silverlight 5's `coreclr.dll` (same CoreCLR generation, installed on this machine, public PDB from Microsoft's symbol server) exports the zapper as ordinal 1, `NGenWorker@12`, and names the 153-method `ICorJitInfo` vtable `ZapInfo` implements (observed: PDB public symbols).

## The two options

| | Compiler on the PC | Compiler on the Xbox side (chosen) |
|---|---|---|
| Image layout (`Module`, `MethodTable`, `EEClass`, `MethodDesc`, fixups, import sections, dependencies, version info) | Re-implement xcoreclr's type loader and `Save` code bit-exactly, from disassembly | Done by xcoreclr itself, exactly as Microsoft's images were made |
| Code generation | IL to PPC compiler | IL to PPC compiler, the same work |
| JIT-to-runtime interface (tokens, call targets, field offsets, helpers, relocations) | Re-implement the answers the runtime would give | Answered by xcoreclr's `ZapInfo`/`CEEInfo` |
| Validation | Every structure is a new risk | Only the JIT output (code, GC info, unwind, EH) is ours |
| What runs where | One PC tool | PC driver + xenia-edge (headless) running a small host title; a console with unsigned code works too |
| Build speed | Fast | Bounded by xenia; acceptable for a build step |

The PC-only option would mean recreating most of a CLR build from disassembly, with every layout detail a silent-corruption risk. The Xbox-side option reduces our part to a JIT, which is needed either way, plus a host and a PE-to-XEX wrapper.

## Consequences

- **`xclrjit.dll`** (our JIT, a PPC XEX DLL built with xsdk's GCC) exports `sxsJitStartup` and `getJit` by name (XEX header `0x00E10402`, a PE export directory, as `agcore.dll` and `npctrl.dll` carry).
- **`ngenhost.xex`** (a small xsdk title) loads `xcoreclr.dll` and calls its zapper entry point for each assembly.
- **The PC driver** stages the app's assemblies, the runtime and our two modules in a folder, runs xenia-edge on it, collects the PE files the zapper writes and wraps them with `pe2nixex.py` into `.ni.dll` XEX modules at free load addresses.
- **Partial images are fine.** A method the JIT declines stays IL; the runtime interprets it, as it does for every app method today.
- **Unsigned.** The XEX modules are not RSA-signed. xenia-edge doesn't check; a retail console without patched hypervisor would reject them.
