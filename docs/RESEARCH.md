# How xcoreclr makes and uses native images

Reverse-engineering notes for the Xbox 360 Silverlight CLR, `xcoreclr.dll` 4.0.338.9242 (built 2012-07-18), as shipped
with Plex (v0.0.0.7, the package the Silverlight360 SDK's default runtime profile is made from). Addresses are
virtual addresses in that build
(image base 0x84AA0000). *Observed* facts come from the binaries or from runs under xenia-edge; *inferred* ones are
marked.

## Tools and references

| What | How | Used for |
|---|---|---|
| xcoreclr basefile | `research\xex.py -b` of the package's `xcoreclr.dll` | Static analysis. It is a memory image: raw offset = RVA. |
| Ghidra project of xcoreclr | Raw-binary import of the basefile at 0x84AA0000, PowerPC:BE:64:64-32addr | 20,622 functions seeded from `.pdata` (`research\ghidra\PdataFuncs.java`), decompiled into one file (`research\ghidra\DumpAll.java`; "the decompile" below). |
| Silverlight 4 `coreclr.dll` 4.0.60310.0 + public PDB | Microsoft's Silverlight 4 runtime | Closest named relative: same virtual-base JIT interface, zapper, `NGenWorker@12`. Its PDB needs OMAP translation (`research\pdbpub.py` does it). |
| Silverlight 5 `coreclr.dll` x86 and x64 5.1.50918.0 + PDBs | Microsoft's Silverlight 5 runtimes | Zapper and EE code with names; x64 has the 2012 `GcInfoEncoder`/`GcInfoDecoder`. |
| dotnet/coreclr v1.0.0 source | github.com/dotnet/coreclr, tag v1.0.0 | Structure names and algorithms (zap/, vm/, inc/). Its interfaces are newer than xcoreclr's. |

Microsoft's public symbol server serves the Silverlight PDBs (`https://msdl.microsoft.com/download/symbols/coreclr.pdb/<GUID><age>/coreclr.pdb`).

## The native image container

- A `.ni.dll` is an XEX2 module: flags `TITLE | DLL` (9), basic compression (zero runs), no encryption, image flags
  `0x90000000` (4 KB pages, revocation check) for images at or above 0x90000000, the app's execution info (title ID),
  a fixed load address, no import libraries (observed: every Plex `.ni.dll`, `research\xex.py -l`).
- The XEX image ends where the PE's `.reloc` section starts; the relocations are not loaded.
- App images sit at 0x941C0000 and up in Plex, framework images below 0x8B860000 between the platform DLLs.
- XamlHost preloads the app's native images by name at start (`PLOADMOD` resource; observed `XexLoadImage` calls
  before the CLR opens the IL), and the CLR opens `<name>.ni.dll` beside `<name>.dll` with `XexLoadImage`.
- `tools\pe2nixex.py` writes this container. Re-wrapping the shipped `mscorlib`, `PlexFoundation` and `PlexResources`
  images with it gives the same basefiles (only the header fields `fixpe.py` touched differ) and Plex runs with them.

## The PE inside

- Machine 0x1F2 (PowerPC big-endian), sections `.data`, `.text`, `.rdata`, optionally `.bisland`, then `.reloc`.
  Data in sections is big-endian.
- COR20 header: `Flags = 4` (IL library), `ManagedNativeHeader` points at a 0xBC-byte `CORCOMPILE_HEADER`.
- `CORCOMPILE_HEADER` (data directories little-endian, other fields big-endian): 17 data directories, then
  `ImageBase` (+0x88), `Flags`, `PEKind`, `COR20Flags`, `Machine`/`Characteristics` (+0x98), then 4 more directories.
  Known entries: `ModuleImage` (directory 8, the frozen `Module`, 0x470 bytes), `VersionInfo` (directory 5, 0x10C bytes),
  `Dependencies` (6), `ManifestMetaData` (11, a `BSJB` metadata copy), `CodeManagerTable` (9), helper thunks
  (directory 14, 16-byte `lis/lwz/mtctr/bctr` thunks in `.text`).
- `CORCOMPILE_VERSION_INFO` ends with runtime DLL info: xcoreclr's timestamp and checksum (0x50072ADB, 0x004DBA69) and
  the JIT's (Microsoft's `xclrjit.dll`: 0x50072A57, 0x001510DF).
- Compiled methods: `.pdata` (`IMAGE_DIRECTORY_ENTRY_EXCEPTION`) has Xbox `RUNTIME_FUNCTION`s, `{BeginAddress,
  PrologLength:8 | FunctionLength/4:22 | 32bit:1 | ExceptionFlag:1}`. Each method is preceded by 12 bytes: a
  method-relative offset to a 16-byte record of self-relative pointers (MethodDesc, 0, GC info, `.pdata` entry), the
  personality routine 0x84BBBE90 in xcoreclr, and 0. GC info blobs are deduplicated (identical methods share one).
- Code is MSVC-style: `mflr r12; stw r12,-8(r1); std r14..r31` or a call to a `__savegprlr` helper emitted in the image,
  `stwu r1,-N(r1)`; statics and imports through indirection cells (`lis rX,hi; lwz rY,lo(rX)`).

## The NGen pipeline is inside retail xcoreclr

Observed:

- The zapper (`Zapper`, `ZapImage`, `ZapInfo`, the PE writer) and the runtime side (`CEECompileInfo`,
  `CompilationDomain`, `DataImage`, `Module::Save`) are compiled into the retail `xcoreclr.dll`. Only the JIT is
  absent.
- Export ordinal 1 (0x84E0A6D0) is `NGenWorker(LPCWSTR assembly, LPCWSTR s, DWORD v, DWORD flags)`. It fills a 0x34-byte
  `NGenOptions` (`dwSize`, `fDebug`..`fSilent`, the assembly at +8, `flags & 1` as `fInstrument` at +0xC, `v` at +0x2C,
  `s` at +0x30; the rest zero), constructs a `Zapper` (0x284 bytes, constructor 0x84E0AD28) and calls
  `Zapper::Compile(assembly)` (0x84E0B9F0). Desktop Silverlight 4 and 5 export the same function as ordinal 1,
  `NGenWorker@12`.
- `Zapper::InitEE` loads the JIT named by `CorCompileGetRuntimeDllName(NGEN_COMPILER_INFO)`: `xclrjit.dll` (or the
  `NGen_JitName` setting) with `XexLoadImage`, then `XexGetProcedureAddress(handle, "sxsJitStartup")` and
  `"getJit"` **by name**. A module exports by name through XEX header 0x00E10402, a little-endian PE export directory
  whose `AddressOf*` fields and name entries are relative to the directory and whose function entries are image RVAs
  (as `agcore.dll`'s is). `sxsJitStartup(callbacks, getCLRFunction)` is called before `getJit()`.
- Settings come from `D:\config\machine.config` and `D:\xmrun.xex.config`.
- Run under xenia-edge with `--allow_game_relative_writes`, the zapper loads every dependency and its native image,
  compiles each method through `ICorJitCompiler::compileMethod`, and writes `<name>.ni.dll` and `<name>.ndb` beside the
  IL assembly. It prints "ComputeHash: Not implemented" (no IL hash on the console).
- The written `.ni.dll` is a PE based at **0** with base relocations: `HIGHLOW` (big-endian words), `HIGHADJ` (a `lis`
  immediate plus a following entry with the paired low half) and `LOW` (the paired `lwz`/`addi` immediate). The shipped
  images are the same files rebased to their load address, with `CORCOMPILE_HEADER.ImageBase` (+0x88, which has no
  relocation) set to it. Microsoft's pipeline therefore rebased and wrapped the zapper's output afterwards; `pe2nixex.py
  --base` does both.
- The strings `D:\CompiledMethods.prof.txt`, `D:\MissingMethods.prof.txt` and `D:\ExplicitInstantiations.ni.dll`
  suggest Microsoft ran NGen on a console or devkit, not on a PC (inferred).

### JIT results

`ThrowExceptionForJit` (0x84D1DA18) in this build: success returns; `0x80000002` throws out-of-memory;
**`0x80000004` (`CORJIT_SKIPPED` in later CLRs) calls `EEPolicy::HandleFatalError(COR_E_EXECUTIONENGINE)`**, ending the
process; anything else throws `InvalidProgramException`, which `ZapInfo`'s caller catches ("while compiling method
token 0x%x") and counts ("Warning: %d methods (%d%%) could not be compiled"). A JIT declines a method with
`CORJIT_BADCODE` (0x80000001). A declined method stays IL in the image; the runtime interprets it.

## The JIT interface (ICorJitInfo)

The `ICorJitInfo *` passed to `compileMethod` is the `ZapInfo` object. Unlike later CLRs, its interfaces are **virtual
bases**: word 0 is the vtable of `ICorJitInfo`'s own methods, word 1 a vbtable pointer. A method of interface *k* is
called on the sub-object at `this + 4 + vbtable[k]`, through that sub-object's vtable. Observed in the zapper
(0x84E2DAE0 calls `getMethodInfo` as `vbtable[1]` slot 6 and `ThrowExceptionForJitResult` as `vbtable[7]` slot 4) and
dumped from a live `ZapInfo` by our JIT:

| vbtable index | Interface (by order and size, Silverlight 4 names) | Slots | xcoreclr vtable |
|---|---|---|---|
| — (primary) | ICorJitInfo | 17 | 0x84ACB3A0 |
| 1 | ICorMethodInfo | 28 | 0x84ACB3E8 |
| 2 | ICorModuleInfo | 13 | 0x84ACB458 |
| 3 | ICorClassInfo | 42 | 0x84ACB490 |
| 4 | ICorFieldInfo | 7 | 0x84ACB538 |
| 5 | ICorDebugInfo | 6 | 0x84ACB554 |
| 6 | ICorArgInfo | 3 | 0x84ACB56C |
| 7 | ICorErrorInfo | 6 | 0x84ACB578 |
| 8 | ICorStaticInfo | 1 | 0x84ACB590 |
| 9 | ICorDynamicInfo | 38 | 0x84ACB598 |

The vtable slots are adjustor thunks (`lwz r11,-4(r3); subf r3,r11,r3; b impl`); `research\xvt.py` resolves them.
Silverlight 4's `ZapInfo` has the same shape with 14/26/12/42/7/6/3/6/1/39 slots and still uses the older token API
(`findClass`, `findMethod`, `findField`, `getMemberParent`, not `resolveToken`).

`ICorJitInfo`'s own slots in xcoreclr, named by what each implementation does: 0 `getMemoryManager`, 1 `allocMem`,
2 `reserveUnwindInfo` (does nothing), 3 `allocUnwindInfo`, 4 *Xbox-specific: returns the personality routine
0x84BBBE90*, 5 `allocGCInfo`, 6 `yieldExecution`, 7 `setEHcount`, 8 `setEHinfo`, 9 `logMsg`, 10 `doAssert`,
11 `reportFatalError`, 12 `allocBBProfileBuffer`, 13 `getBBProfileData`, 14 `recordRelocation`, 15 `getRelocTypeHint`
(returns -1), 16 `getModuleNativeEntryPointRange`.

Slots confirmed at run time by calling them from our JIT: `ICorMethodInfo` 0 `getMethodName(ftn, &className)`,
6 `getMethodInfo`; `ICorModuleInfo` 1 `findField(module, token, context)`; `ICorFieldInfo` 1 `getFieldAttribs`
(PRIVATE is 0x2, STATIC 0x8), 3 `getFieldType(field, &class, owner)`, 5 `getFieldOffset` (object-relative: the first
field is at 4); `ICorMethodInfo` 11 `getEHinfo(ftn, index, &clause)`; `ICorClassInfo` 4 `getClassAttribs`, whose
class flags have CoreCLR's values, not the older ones: interface **0x00200000** (IDisposable 0x00200400,
IDictionary\`2 0x00220400), value class 0x10000 (Int32 0x20010010), variable size 0x40000 (String 0x20040010),
abstract 0x400. (An earlier guess of 0x200 for interfaces let interface calls through as vtable calls.) The other
slots are named by Silverlight 4's order and still need checking before use.

### Structures

- `CORINFO_METHOD_INFO`, 0x80 bytes: `ftn, scope, ILCode, ILCodeSize, maxStack, EHcount, options, regionKind,
  args, locals`.
- `CORINFO_SIG_INFO`, 0x30 bytes: `retTypeClass, packed, retTypeSigClass, cbSig, sigInst[4], args, pSig, scope,
  token`, where `packed` is `retType | callConv << 8 | numArgs << 16` (MSVC on Xbox allocates bitfields from the low
  bit). Observed: a getter's `0x00002014` (CLASS, HASTHIS, no arguments), a setter's `0x00012001`.
- `CORINFO_EH_CLAUSE`: six words, as `setEHinfo` (0x84E2E8F0) copies them.

### What a JIT returns

- **Code** through `allocMem(hot, cold, roData, exceptionCount, flags, &hot, &cold, &roData)`.
- **Unwind**: `allocUnwindInfo(hot, cold, start, end, size, block, funcKind)` keeps only the first word of the
  block: the second word of the Xbox `RUNTIME_FUNCTION` (prolog length in instructions, length in instructions << 8,
  0xC0000000 for the 32-bit and exception flags). `reserveUnwindInfo` does nothing.
- **GC info** through `allocGCInfo`, in the format of xcoreclr's `GcInfoDecoder` (0x84EFB348; `research\gcinfo.py`
  decodes headers): a bit stream read from little-endian 32-bit words, low bit first. Header: 7 flag bits (vararg,
  security object, GS cookie, PSPSym, two bits of generics context, stack base register); the code length in
  4-byte units as a variable-length number in 6-bit chunks; if a security object or GS cookie is present, prolog and
  epilog sizes (3-bit chunks, in 2-byte units); the security object, GS cookie, PSPSym and generics context slots
  (signed, 4-byte units); the stack base register (2-bit chunks, plus 31); the outgoing argument area (4-bit
  chunks, 4-byte units; 0x50 in framed methods); the safe point and interruptible range counts (2-bit chunks); then
  the safe points (each `ceil(log2(code length / 2))` bits), the ranges and the slot table. Each variable-length
  chunk is followed by a bit saying another follows. Silverlight 5's x64 `GcInfoEncoder`/`GcInfoDecoder`
  (decompiled the same way) is the same family with AMD64 constants.
  The generics context bits say where shared generic code keeps its instantiation context, and the slot is its
  offset from the caller's SP: 0x10 the MethodTable argument (Dictionary`2::IsCompatibleKey, slot 20: r3's home
  in the caller's area), 0x20 the MethodDesc argument (String::Join<T>, slot 20), 0x30 `this` (Dictionary`2::
  get_Values, slot -32: a frame slot the prologue copies `this` to). The zapper's CORINFO_METHOD_INFO.options
  never carries CLR 4's CORINFO_GENERICS_CTXT_FROM_* bits (always 0x02700010), so the kind comes from the method:
  getMethodAttribs bit 0x00020000 (CORINFO_FLG_SHAREDINST: List`1::Add 0402000A, its exact instantiations
  04000019) says the code is shared, the PARAMTYPE convention with a method instantiation means MethodDesc, without
  one MethodTable, and an instance method's `this` otherwise. Ours point at the instantiation argument's frame
  slot and `this`'s argument slot; the kinds agree with Microsoft's on 7,776 paired mscorlib methods, and we
  declare a context on 82 shared methods where Microsoft doesn't need one.
- **Generic virtual methods** (getCallInfo kind 3, CORINFO_VIRTUALCALL_LDVIRTFTN: `Iterator<T>.Select<TResult>`,
  Newtonsoft's `ReflectionDelegateFactory.CreateGet<T>`) go through JIT_VirtualFunctionPointer (helper 403) like
  interface calls: the object, the method's class (the slot is the same in every instantiation, so the canonical
  one serves) and the exact MethodDesc of the instantiation (embedGenericHandle of the MethodSpec; a runtime lookup
  from shared code). Declining `Enumerable.Select` had cost System.Core 314 of Microsoft's 1,594 bodies: the
  zapper compiles the instantiations a method pulls in only when the method itself compiles. From exact code
  such a call had gone through the vtable like any virtual call, which no slot of an instantiation answers
  (NGenBench's GenericVirtual faulted in the runtime at the first one); it takes the same helper.
- **ldftn needing a runtime lookup** (`Func<T, string>(Show<T>)` in shared code, LINQ's `CombinePredicates`):
  getFunctionFixedEntryPointInfo's CORINFO_LOOKUP has the layout of embedGenericHandle's, and the dictionary walk
  (a method-entry slot) yields the entry point. Compare-Package.ps1 reuses an image already in its work folder,
  so a recompile needs that image deleted (and it refreshes its JIT copy when a newer one has been built).
- **Floating-point arguments on the stack.** A double whose argument slot is beyond r10 (the ninth argument on)
  still travels in f1-f13; the caller stores it in the outgoing slot as well, and a callee of ours reads the
  register (only the fourteenth and later doubles, declined, would live in the slot alone), so either convention
  of the other side works. Value types up to 136 bytes (System.Windows's Matrix3D) are handled like the smaller
  ones; each stack entry's room grew with them.
- The zapper adds the rest: the `.pdata` entry, the 12 bytes before each method (a word at method-12 that is the
  record's offset from method-16, the personality routine, 0) and the 16-byte record of self-relative pointers
  (MethodDesc, 0, GC info, `.pdata` entry); identical GC info blobs are shared.

### Calls, helpers, relocations

- **Relocations** the JIT reports with `recordRelocation(location, target, type, 0, 0)` (ZapInfo 0x84E30428, resolved
  by the image writer at 0x84E29880): **3** a 32-bit word; **6** a `b`/`bl` displacement (the writer adds a jump stub
  when the target is out of range); **0x10** a `lis` immediate, `(target + 0x8000) >> 16`; **0x11** the paired
  instruction's 16-bit immediate. Targets are the zapper's nodes (import cells, helper thunks, other methods).
- **Helpers** (`getHelperFtn`, DynamicInfo slot 4): numbered as in Silverlight 4 up to 45; from 46 there are two
  more entries, so `JIT_Box` 46, `JIT_Unbox` 48, `JIT_Stelem_Ref` 52, `JIT_Ldelema_Ref` 53, range check failure 57,
  overflow 58. 72-308 are the compiler's register save/restore routines (`__savegprlr_N` and others), which the zapper
  binds directly. The write barriers are **320** (`stw r4,0(r3)` plus card marking) and **321** (the same after
  checking the destination is in the heap). They read the ephemeral generation's bounds from **r14 and r15**, which
  managed code must not change. Identified by each table entry's code (`research\xvt.py`, the decompile).
- **Entry points**: `getFunctionEntryPoint(method, IAT_VALUE, &lookup, 0)`; a direct call is `bl` with relocation 6.
- **Virtual calls**: `getMethodVTableOffset(method, &ofIndirection, &afterIndirection)`; Microsoft's code is
  `lwz r12,0(this); lwz r12,ofIndirection(r12); lwz r12,afterIndirection(r12); mtctr r12; bctrl`.
- **Interface calls** don't use virtual stub dispatch: Microsoft's code (PlexFoundation.ni.dll 0x94ADFAE4,
  `IDisposable.Dispose`) calls helper **403**, `JIT_VirtualFunctionPointer(object, interface class, method)`
  (0x84CCF590: a hash cache keyed on both handles and the object's type, then a slow path), and `bctrl`s to the code
  address it returns; the handles come from `embedClassHandle`/`embedMethodHandle` (DynamicInfo slot 13) cells. The
  class must be the exact interface type the call names: `getMethodClass` gives the shared canonical type of an
  instantiation over reference types (`IEnumerable<__Canon>`), for which the helper throws
  EntryPointNotFoundException. The exact type comes from **`resolveToken`** (ModuleInfo slot 3; the runtime's
  0x84D07640): xcoreclr's ModuleInfo has the older `findClass`/`findField`/`findMethod` in slots 0-2 and CLR 4's
  `resolveToken(CORINFO_RESOLVED_TOKEN *)` after them, with the struct as in CLR 4 (context, scope, token, kind in;
  class, method, field, type spec, method spec out). Calling slot 3 as `getMemberParent(scope, token)` kills the
  zapper. The full ModuleInfo order: 0 `findClass`, 1 `findField`, 2 `findMethod`, 3 `resolveToken`,
  4 `getMemberParent`, 5 `findSig` (the runtime's 0x84D08460 takes a fifth, optional out pointer it writes when not
  null), 6 `findCallSiteSig`. The same exact type is what `newobj` must allocate: an object of
  `EventHandler<__Canon>` fails the cast when an event adds it (InvalidCastException).
- **Delegates**: `ldftn` loads `getFunctionFixedEntryPointInfo(scope, token, context, &lookup, &fixup)`'s cell
  (DynamicInfo slot 8; `CORINFO_LOOKUP` is the lookup kind, then access type and address); `ldvirtftn` is helper 403;
  `newobj` of a delegate allocates it and calls its runtime-implemented `.ctor(object, IntPtr)`, which finds the
  method from the code pointer. A module that defines a delegate type gets the runtime's IL stubs for it in its image,
  compiled by the JIT: `IL_STUB_ShuffleThunk` (a static method's delegate: `ldarg.1; ldarg.0; ldfld _methodPtrAux;
  calli; ret`) and `IL_STUB_MulticastInvoke` (a combined delegate), one per instantiation of a generic delegate type.
  Their scope is a dynamic resolver (bit 0 set) and their tokens resolve through it. **A declined stub breaks the
  app**: the runtime can neither run nor interpret it (a crash in the prestub, or BadImageFormatException), so the JIT
  must compile them, `calli` included. Stubs with handlers (`IL_STUB_PInvoke`, `IL_STUB_ReversePInvoke`) compile
  too: an early crash when asking for their clauses was something since fixed, and declining them is worse than
  it looked. A declined reverse P/Invoke stub leaves the image's UMEntryThunk record (the 0x7C-byte records the
  loader walks in FUN_84BD2408, making a marshalling thunk for each with FUN_84D03768) without its MethodDesc,
  and loading the image then faults in the runtime (0x84BB8A9C, under RuntimeType.CreateInstance while the
  host created the application): that is what our first System.Windows.ni.dll did.
- **Reverse P/Invoke thunk records.** Compiling the stubs wasn't enough: the loader faulted the same way,
  because the records (0x7C bytes; MethodDesc at +4, two more pointers at +0x74 and +0x78; the array at native
  header +0xA4 with its size at +0xA8) hold RVAs in the zapper's output and have no relocations, while the
  shipped images hold addresses. pe2nixex.py adds the load address to them.
- **NGenBench on our images alone** (2026-11-03): with mscorlib, the fourteen platform assemblies it loads
  (`tools\NGen-Platform.ps1`; two of them under the file names xcoreclr shortens, see below) and NGenBench
  itself all compiled by us, Microsoft's images moved aside, the app starts and every check passes.
- **Outgoing stack arguments and localloc.** An intermittent death in the XAML parser (an
  IndexOutOfRangeException from List<RuntimeEventInfo>.Add's store, helper 52's bounds check, under
  MemberInfoCache.PopulateEvents) became deterministic with a later mscorlib image and was caught with the
  helper's failure path poisoned: List.Add's `this` was a stack word. PopulateEvents(Filter, RuntimeType,
  Dictionary, List) takes its list as the ninth argument, on the stack, and its caller has done a `localloc`:
  the compiler wrote outgoing stack arguments relative to the frame register (r31), but after a localloc the
  stack pointer is below r31 and the callee reads them at its own SP + frame size. They are written relative to
  r1 now (NGenBench's StackAllocArgs). Whether the stale word at r1 + 0x54 happened to be a live object decided
  the earlier runs.
- **Short image file names.** xcoreclr maps three assembly names to shorter native image file names (a table in
  its .rdata, UTF-16 at file offset 0x1C300 of the retail xcoreclr.dll): System.Windows.Controls.Navigation to
  `SW.Controls.Navigation.ni.dll`, Microsoft.Web.Media.SmoothStreaming to `MWM.SmoothStreaming.ni.dll`,
  Microsoft.Xna.Framework to `MXF.ni.dll` (the XEX's original PE name keeps the full name). The zapper writes
  the short name too; NGen-Package.ps1 reads and writes it. Leaving Microsoft's SW.Controls.Navigation.ni.dll
  (hard-bound to its mscorlib) in a package of our images makes the zapper break (DbgBreakPoint from
  0x84D6BF20 before compiling anything) when it compiles an assembly that loads it, Microsoft.Xbox for one.
- **Our images bind to their dependencies' images too:** a platform image compiled against one mscorlib.ni.dll
  is rejected with another (the same DbgBreakPoint), so mscorlib, the platform and the app are compiled as a
  set (`test\Test-Mscorlib.ps1`).
- **Strings**: `constructStringLiteral` returns `IAT_PPVALUE` and an import cell; the string is `[[cell]]`
  (Microsoft: `lis; lwz rY,cell(rX); lwz r3,0(rY)`). Class handles (`embedClassHandle`) come back as a cell holding
  the handle.
- **Arrays**: length at +4; elements of up to 4 bytes from +8; object references from +12 (+8 holds the element
  type); value types of 16 bytes or more from +16; smaller value types and 8-byte primitives from +8.
- **Statics** are not at a fixed address (`getFieldAddress`'s cell is still 0 when code runs). They live in the
  domain's per-module blocks (DomainLocalModule) and are reached as Microsoft's code does (`lwz r3,cell; li r4,3;
  bl JIT_GetSharedGCStaticBase`): helper **364** (GC statics: returns `[DLM+0x10]`, the class's block of references)
  or **365** (non-GC: returns the DLM), as `getSharedStaticBaseHelper(field, 0)` (ClassInfo slot 21) answers, called
  with the module ID (`getClassModuleIdForStatics(class, &module, &cell)`, ClassInfo slot 6: 0 and a cell) and the
  class ID (`getClassDomainID(class, &cell)`, DynamicInfo slot 26: a constant), then the field's offset
  (`getFieldOffset`: from the block for GC statics, from the DLM, header included, for the others; observed 0 and
  48). Both helpers are dynamic (the table holds 0xA-0xD; the runtime installs the routines, 0x84CC6840 for the GC
  one): when the class's initialized flag (DLM + 0x18 + class ID) is clear they take a slow path that runs the class
  constructor. Statics of other modules (mscorlib's `String.Empty`) work the same way. A store of a reference goes
  through the checked write barrier.
- **Calling convention** (read with NGenBench's probes: methods the JIT compiles to return r<n> or f<n>, called by
  the interpreter, which uses the real convention): each argument takes the next of r3-r10 in order, `this` first,
  int64 in one register; a `double` or `float` goes in the next of f1-f13 and still uses up its GPR (`(double,
  int, double, int)`: f1, r4, f2, r6; Microsoft's `Number.FormatDouble(double, string, NumberFormatInfo)` call
  sites agree: f1, r4, r5). A value type is passed by value as its memory image in 8-byte chunks across consecutive
  GPRs, a final 4-byte chunk left-justified (`Point` in r3-r4, `Rect` in four, a 12-byte struct in r3 and the high
  half of r4), one of 4 bytes like an integer (right-justified). Results: r3, or f1 for floating point; a value
  type of up to 8 bytes in r3 the same way; a larger one through a buffer the caller passes in r3, ahead of `this`
  and the arguments (an instance method then has `this` in r4). Arguments past r10 go on the stack (not handled
  yet: the caller's frame has the 0x40-byte save area for r3-r10 at r1+0x10, so further ones follow at r1+0x50).
  The receiver widens narrow arguments and results.

### GC info for methods with frames

Decoded from Microsoft's methods (`research\gcinfo.py full`): after the header come the safe points, each the call's
return address offset / 2 - 1 in `ceil(log2(code length / 2))` bits; the interruptible ranges (pairs of 4-bit-chunk
numbers); the slot table (register and stack slot counts in 4-bit chunks; a register slot is its number and 2 flag
bits; a stack slot is a 2-bit base, 0 meaning relative to the caller's stack pointer, a signed offset / 4 and 2 flag
bits, 1 interior and 2 pinned, after which slots without flags are delta-encoded); then one live bit per slot at each
safe point. A reference argument Microsoft homes at `0x74(r1)` in a 0x60-byte frame is reported as base 0, offset
+20.

### Variables in callee-saved registers

Read from Microsoft's mscorlib.ni.dll and xcoreclr, then used by our compiler (2026-11-04):

- **Register slots.** Microsoft's methods report references in r17-r31 as register slots: the interior pointer
  slots first, then the references, each in ascending register order, one register possibly in both lists (r29
  as an interior pointer and as a reference at different safe points). After a slot without flags the next is the
  increase in register number minus one in 2-bit chunks, flags 0; otherwise the number in 4-bit chunks and 2 flag
  bits. The live bits count the register slots first, then the stack slots. Every register from r17 to r31 occurs
  (counts over the methods whose body starts with `mflr r12`: r31 119, r30 104, r29 62, down to r17 1).
- **Prologs.** Either `std rN` stores below the link register's word before the `stwu` (`mflr r12; stw r12,-8(r1);
  std r30,-0x18(r1); std r31,-0x10(r1); stwu r1,-0x70(r1); mr r31,r3`, prolog length 6: the argument move counts)
  or `bl __savegprlr_N` between `mflr` and `stwu` (prolog length 3). Each register has a fixed place: rN at the
  caller's SP - 8 * (33 - N).
- **The unwinder** (xcoreclr 0x84F08FA0, classifier 0x84F09830) is the NT PowerPC virtual unwinder: it scans the
  prolog backwards from its end (or from the PC inside it) and undoes each instruction, `std`/`stw` of a GPR
  restoring it from the store's address (and recording that address for the GC's register updates), `stwu r1`
  following the back chain, `mflr` moving r12 back to the link register, `mr`/`addi` reversed; a `bl` into a
  save helper pushes the helper's range and scans that too (classifier case 0xC). So plain `std`s before the
  `stwu` unwind exactly like Microsoft's, as long as the `.pdata` prolog length covers them.
- **The write barriers** use only r0, r11, r12 and cr0 and read r14/r15: every other register survives them, and
  they never collect. Helper 320 (0x84F07930) is `stw r4,0(r3)` with the card marking for a destination known to be
  in the heap, 321 (0x84F078D0) the same after checking that it is; **324** stores r3 through r4, **325-332** store
  r5-r12 through r3 and **333-347** r17-r31 through r3 (0x84F079B0 on, 0x40 bytes apart, each `stw rN,0(r3);
  cmpw rN,r14; bltlr; cmpw rN,r15; bgelr`, then the card byte at `[0x84F37BC8] + (r3 >> 10 ^ 3)` set to 0xFF).
  Microsoft's code calls them rather than inlining them (no `cmpw rX,r14` occurs in its mscorlib.ni.dll); ours calls
  the variant for the register the value is in, keeps the evaluation stack's other entries in their registers
  across it and records no safe point there.
- **P/Invoke.** While a P/Invoke stub's native call runs, the GC walks the managed frames from the stub's. Our
  compiler makes the methods with a `calli` (the runtime's IL stubs: C# makes none), except the delegate stubs
  `IL_STUB_ShuffleThunk` and `IL_STUB_MulticastInvoke` whose `calli` is managed, save all of r17-r30, as RyuJIT
  saves every callee-saved register in a method with a P/Invoke frame. Whether that is needed is open: NGenBench's
  PInvokeUnderGC (references in the caller's saved registers while another thread's allocations collect during a
  1 ms `KeDelayExecutionThread` P/Invoke) passes without the saves too, so the runtime's transition frame may
  record the registers itself. A stub isn't told by an `ldftn` of a P/Invoke method: DebugLog's `DbgPrint` stub
  (236 bytes of IL) has none that resolves as one. The saves cost 28 instructions per stub.
- **Value types of one primitive field come as that primitive.** `getArgType` (arguments and locals) and a
  signature's result report a struct whose one field is a `long` as `CORINFO_TYPE_LONG` (NGenBench's `Span8`),
  with the struct as the class handle; one of an int (`Int4`), of a double (`Dbl8`), of two shorts (`Pair4`) or
  of an int, a byte and a short (`Mixed8`) stays `VALUECLASS`. Passing and returning them is the same either way
  (an 8-byte value type travels in a GPR as its bytes read big-endian), but C# reads a field of a value type
  argument or a call's result with `ldarg` or the call then `ldfld` on the value, and a `ldfld` on a long had been
  declined ("ldfld base"). Our compiler keeps the class on such a stack entry (`stack_cls`, from the variable, the
  result or the field) and reads the field as the value itself; an int is read that way only with the class, so
  a native int pointer is never taken for a value.
  Only the integer and floating-point types carry it: a pointer's or a byref's class is the type pointed to (a
  value type, maybe: mscorlib's FileStreamAsyncResult.get_AsyncWaitHandle reads through a `NativeOverlapped*`),
  and `getClassAttribs` on a reference result's `retTypeClass` threw inside the zapper while it compiled
  NGenBench (`DbgBreakPoint` from 0x84F0A768), which then wrote no image.
- **Microsoft's interpreter** faults on the same reads: with NGenBench's image missing, its LeafStructs check
  threw a NullReferenceException from a load at address 5 inside xcoreclr (0x84C9E84C), 5 being the `Ticks` of a
  `Span8` it was reading.
- **Microsoft's interpreter** passes a value type argument holding references by reference: NGenBench's
  NamedStructs (`Rename(Named n, string name)` stores `n.Name` and returns `n`) changes the caller's `a` too when
  NGenBench is interpreted, on Microsoft's images and on ours alike (5044548 against 5044538; compiled, it is
  right).

### The evaluation stack's registers, constants and leaf methods

Measured and used by our compiler (2026-11-04, afternoon):

- **Cache registers are the argument registers.** Evaluation stack entry i is cached in r3 + i and f1 + i (r5 + i
  and f2 + i before): a call whose arguments start at the stack's bottom, a call's result and a return value need
  no moves then. A leaf method keeps r5 + i and f2 + i, its variables having r3 and r4 and the registers above. The
  only simple instruction touching r3 and r4 is a reference `stfld`, whose write barrier takes the destination in
  r3 and the value in r4 (or its own register: the per-register variants above): it writes the entries still in r3
  and r4 below its operands back first (NGenBench's BarrierBelow fails without that). mscorlib.ni.dll: 9,523,200 ->
  9,310,208 bytes.
- **Floating-point variables in f31-f14 are aliased** like integer ones: an entry names the variable's register
  (`alias` holds an f-register for a floating-point entry, so register names are compared per bank), and results
  are computed straight into it when the next instruction stores there. FP arithmetic, compares and conversions
  read their operands where they are and write the result's register, without f0/f12 copies. `stfs` rounds a
  double to single precision itself: a float store needs no `frsp` (a float variable in a register still does).
  mscorlib.ni.dll: 9,572,352 -> 9,523,200 bytes.
- **Floating-point constants** come from the method's read-only data, as Microsoft's do (`lis r11; lfd`): the
  zapper's `allocMem` takes a `roData` size and gives a block in the image (NGenBench's SavedFloats loads 0.5 and
  0.25 from 0x999B5398 and 0x999B53A0, inside its image), and a HA16/LO16 relocation pair to an address in that
  block is written like one to code. We ask 4 bytes more and start the constants 8-byte aligned whatever the
  block's alignment. 64-bit integer constants are built in a register (`li`; `lis`, `ori`; or the high word
  shifted up with `sldi` and the low word ored in) instead of a frame slot.
- **Why call-free methods kept a frame** (`XNGEN_LEAF_STATS`, mscorlib, before the leaf work): of 730, 241 are
  shared generic code (the runtime reads the generic context from `this`'s slot), 210 need a frame for an
  instantiation argument, a security object or a stub's secret, 125 store a reference (the write barrier is a
  call), 49 return a value type, 36 touch memory at r1 (integer-to-float conversions go through a frame slot,
  no instruction moving a GPR to an FPR; `ldc.r8`/`ldc.i8` before the constants above; `ldarga`), 29 take a value
  type argument, 23 a floating-point argument, 10 have too many variables, 3 a floating-point local.
- **Leaf methods with floating point**: floating-point arguments keep f1 and f13 or move to the f-registers above the
  stack's (f2 + depth .. f11); the leaf prolog moves every argument at once (a parallel move: the old moves, last
  argument first, assumed registers only move up, which an FP argument among integer ones breaks: with
  `(double, int a, int b)`, b would have overwritten a). Floating-point locals keep a frame (clearing one needs
  memory).
- **Leaf methods with value types of 4 or 8 bytes**: held in an integer register as their bytes read big-endian,
  as `lwz`/`ld` load them and as they are passed and returned. A primitive field comes out with `rldicl` (rotated
  down, the rest cleared) and `extsb`/`extsh` for signed bytes and shorts; `ldarga`/`ldloca` followed by `ldfld`
  is the same read from the variable's register, both instructions at once; a value type field of an object is
  loaded with `lwz`/`ld` (`ld`'s offset is a multiple of 4, which a struct of shorts may not be: that one keeps a
  frame). TimeSpan's and DateTime's operators and getters are of this kind. Anything else that wants such a
  value's address keeps the frame.

Our compiler (`choose_saved_registers`) keeps the most used integer, reference and byref variables of a method in
r30 down to r17 and its floating-point ones in f31 down to f14 (saved with `stfd` below the general registers'
saves, which the unwinder undoes as well: classifier case 6 writes the context's FPRs), uses counted from the IL
(eight times over inside each loop), the address-taken ones, value types, stack-passed arguments, the `this` of
shared generic code and any variable a handler uses (its funclet reads the parent's frame through r31) left in the
frame. In a method with handlers the rest stay in registers across a catch: the runtime resumes the method with
the registers it rebuilt by unwinding the frames above it, so they hold what they held at the throwing call.
NGenBench checks it: SavedRegistersGC (references and a byref in registers across collections), UnwindSaved (an
exception through a method that saved and clobbered its caller's registers), SavedAcrossHandlers (the same into a
method with a catch and a finally, a GC in the catch) and SavedFloats (the same with floating-point registers).

stelem.ref's inline path reads the array's element class at array + 8 (as JIT_Ldelema_Ref does) and stores an
object whose MethodTable is that class, or null, without JIT_Stelem_Ref (helper 52), which keeps every other case
(subtypes, covariant arrays, the ArrayTypeMismatchException; NGenBench's StelemRefPaths).

### The image's .pdata

The zapper writes `RUNTIME_FUNCTION.BeginAddress` as an RVA with no relocation; the shipped images hold absolute
addresses. Left as RVAs, xcoreclr's unwinder can't find the compiled methods during stack walks and calls
`DbgBreakPoint` (the Plex break). `pe2nixex.py` adds the load address.

### Exception handling (funclets)

Read from Microsoft's methods with handlers (PlexFoundation.ni.dll 0x94ADF4B0 and its funclet 0x94ADFABC; 943
funclets in mscorlib.ni.dll) and from the zapper:

- **Frame.** A method with handlers addresses its frame through **r31**, set to its SP in the prolog, and keeps the
  **PSPSym** (its caller's SP) at caller SP + 4: `mflr r12; stw r12,-8(r1); li r0,0; stw r0,4(r1); std r30,-0x18(r1);
  std r31,-0x10(r1); addi r31,r1,-F; stwu r1,-F(r1); addi r12,r1,F; stw r12,F+4(r31)`. The epilog is
  `addi r1,r31,F; lwz r12,-8(r1); mtlr r12; ld r30,-0x18(r1); ld r31,-0x10(r1); blr`.
- **Funclets.** Each handler is a separate function after the main body, with its own `.pdata` entry (prolog length
  7) and an 8-byte pre-header in the method's code: the personality routine 0x84BBBE90 (ICorJitInfo slot 4 returns
  it) and 0. The exception flag (bit 31) of a `.pdata` word means "handler and data precede the function"; the zapper
  writes the main body's 12-byte pre-header, the JIT the funclets'. Entry: `std r31,-8(r1); lwz r3,4(r3);
  stw r3,4(r1); addi r31,r3,-F; mflr r12; stw r12,-0x10(r1); stwu r1,-0x60(r1)`; exit: `lwz r1,0(r1); ld r31,-8(r1);
  lwz r12,-0x10(r1); mtlr r12; blr`. r3 is the frame the funclet runs for (its +4 holds the PSPSym, which the
  funclet copies to its own caller SP + 4, so nested funclets work the same way); a catch gets the exception in
  **r4** and returns the address to resume at in **r3** (`lis r3; addi r3` of a label in the main body).
- **Reporting.** `allocUnwindInfo(hot, cold, start, end, 4, &word, funcKind)` once per function: kind 0 for the main
  body (0..its end), 1 for a handler funclet, 2 for a filter, with `0xC0000000 | length << 8 | prolog` words.
  `setEHcount(n)` (0x84E2E860) allocates the table: a header byte 0x41 and a 24-bit size `n * 24 + 4`, then
  `setEHinfo(i, clause)` (0x84E2E8F0) copies six words: flags, try start, **try end**, handler start, **handler end**,
  class token (the IL token as is). Native *end offsets*, not lengths: Microsoft's table for 0x94ADF4B0 holds
  `4, 0x3F4, 0x5B4, 0x60C, 0x674` (a fault clause; its funclet starts at +0x60C, 104 bytes long). A clause with flag
  **8** (duplicated) covers a funclet whose IL lies inside another clause's try: try = the funclet's range, handler =
  the outer clause's funclet (`0x8, 0x184, 0x1F8, 0x200, 0x258` beside the catch whose handler is 0x184-0x1F8).
- **Microsoft's finally.** On the normal path Microsoft's compiler inlines a copy of the finally after the try and
  turns the clause into a fault; the clone gets a duplicated finally clause with an empty try (`0xA, 0x5B4, 0x5B4,
  0x5B4, 0x5EC`). Our compiler calls the finally funclet instead (`addi r3,r31,F; bl funclet`) from a thunk placed
  after the try's code, so the call is outside the try's range and an exception from the finally doesn't run it
  again; NGenBench confirms that both ways work.
- **GC.** Every Microsoft method with handlers is fully interruptible (no safe points, two interruptible ranges: the
  main body after its prolog, and the funclet after its prolog) with header flags **0x48** (PSPSym at caller SP + 4,
  stack base register r31) and stack slots relative to r31 (base 2). Decoding its liveness (decoder 0x84EFB8B0: a
  pointer width in 3-bit chunks, a pointer per 64 two-byte units of interruptible code, then per chunk a
  could-be-live bit per slot, a final state bit per such slot and per slot its transitions, `1` + a 6-bit offset each,
  ended by `0`) shows the parent's stack slots **dead inside the funclet** even where the funclet reads them through
  r31: the runtime reports the parent's frame while a funclet runs, and a funclet reports only its own registers. Our
  funclets' safe points therefore report nothing.
- **Partially interruptible methods with handlers.** The decoder only consults safe points when the frame isn't
  "execution aborted" (flag 2: the frame took a hardware fault); otherwise it looks up the interruptible ranges and
  reports nothing outside them. So our compiler, which emits only safe points, never relies on a hardware fault in a
  method with handlers: it tests references for null before using them and calls `IL_Throw(null)` (helper 54), which
  throws NullReferenceException from a call site. A trap follows every call that doesn't return, so the return
  address the runtime walks from stays inside the method and inside the try (a `bl IL_Throw` as a method's last
  instruction otherwise puts it past the end, and the stack walk fails). Catch resume addresses are added as safe
  points too.
- **Helpers:** 54 `IL_Throw(object)`, 55 `IL_Rethrow()` (both set up a helper method frame).

## Our images run

- `test\NGenProbe`: our JIT compiles its three getters; `NGen-Package.ps1` makes `NGenProbe.ni.dll` at 0x95000000;
  run in xenia-edge, the app shows `XNGenMagic = 24301`, a value only our compiled code returns (its IL returns 7),
  and the other getters return the right values. **xcoreclr loads the image and runs its code.**
- The same JIT compiles `PlexResources.dll`'s field getters to `lwz r3, 4(r3); blr`, byte for byte Microsoft's
  code for the same getter, with the same GC info and `.pdata` word. Plex runs with that image.
- In Plex (not in NGenProbe), xcoreclr's PPC virtual unwinder (0x84F08FA0) calls `DbgBreakPoint` during a stack walk
  with our `PlexResources` image, which xenia turns into a host breakpoint and a crash. Patching that import thunk
  (0x84F2F4B4) to `blr` in a test copy of `xcoreclr.dll` (`research\patch_xex.py`) lets Plex run to its sign-in screen.
  Why it breaks is open.
- `test\NGenBench` runs its exception handling checks natively: finally on return and on fall-through, catches of
  runtime exceptions (range check, null reference) and of thrown ones, an exception through a finally into an outer
  catch, a catch's return through an outer finally (resume address at a thunk, duplicated clause), two finallys on
  one leave, rethrow into an outer catch, and the GC running inside a catch and a finally while only the parent's
  frame holds references.
- Native code changes timing: XboxControls' `Loaded` handler ran before the first layout pass (`ActualWidth` 0), so
  `Focus()` failed and the controller had nothing focused; interpreted, layout usually won. The sample now retries on a
  later dispatcher turn (retrying from `LayoutUpdated` recurses: `Focus` runs layout, which raises `LayoutUpdated`).
  Apps that focus in `Loaded` can show the same thing.
- Two xenia-edge instances at once can stop one with "Overflowed stackpoints!" (seen with XboxControls, also with no
  image of ours); alone it doesn't happen.

## Run-time code generation: the code cache and the devkit gate

The zapper doesn't only call the JIT. For some methods it has the runtime generate code at run time and copies that
into the image: an array type's accessor stubs (`Get`, `Set`, `Address` of a `T[]`, MethodDescs of kind 4 that
`FUN_84C3F8E0` prepares from the pending list at 0x84F340D0), precodes, delegate thunks. The runtime takes
executable memory for them from its code heap (`FUN_84DFB8E8`), and that heap comes from a module shipped for the
purpose:

- It loads **`CLRCodeCachedll_0.xex`**, then `_1` and `_2` as each fills (`XexLoadImage`, names at 0x84AC7534), finds
  the named exports **`LINKHACKFUNCTIONPROC0`** and **`LINKHACKFUNCTIONPROCLAST`**, reserves the range between them
  with xboxkrnl ordinal **0x36B `XexReserveCodeBuffer`**, and carves allocations from it. A failure anywhere returns
  0xC0000001, which `FUN_84DF11C0` turns into `E_OUTOFMEMORY` and the zapper into "Error compiling X: Out Of Memory"
  (hr 0x8007000E) for the whole image. That is what failed for every assembly that has an array of a generic
  instantiation or a declined generic method: the stub's code, not the JIT's.
- It only tries when the runtime is in **mode 1** (`DAT_84F3B038`, set by `FUN_84DF81C8`): `D:\xclrjit.dll` exists
  and `xbdm.xex` ordinal 0xA1 (`DmGetSystemInfo`) succeeds with bit 0x02000000 of its flags clear. No `xbdm.xex`
  (a retail console) means mode 2, where the code heap refuses every request; no JIT file means mode 3.
- In the retail kernel (a dump of 2.0.17559) ordinals 0x36B and 0x36C are one stub
  at 0x8007BB68 returning `STATUS_INVALID_SYSTEM_SERVICE` (0xC000001C), and 2.0.1888 doesn't export them.

So generating code at run time, and with it NGen of anything beyond trivial assemblies, is a **devkit** feature:
Microsoft's images were made with a devkit kernel and xbdm. `NGen-Package.ps1` therefore runs xenia-edge as a
devkit (`--console_type=0`, which loads its xbdm) with our stand-in `codecache\CLRCodeCachedll_0.xex` (4 MB of
writable data between the two exports; xenia executes code from any guest memory), and xenia-edge's
`XexReserveCodeBuffer`/`XexCommitCodeBuffer` succeed only as a devkit, returning the retail kernel's status otherwise.

Two more consequences of the zapper being a live runtime with our JIT in it:

- **The runtime JITs through us at NGen time.** With no `mscorlib.ni.dll` beside it (compiling mscorlib itself), every
  mscorlib method the zapper process executes (`AppDomain::SetupDomain`, `String::Substring`...) goes through the
  prestub to `compileMethod` and into the code heap; the log line's `at <address>` tells the two apart (image buffer
  versus code heap). A decline there is not "leave it as IL": the runtime throws and the image fails with hr
  0x80131500 (`Thread::get_CurrentUICulture`, thread statics helper 0x177). The same holds for a runtime exception
  inside any callback (`findClass` on a token it refuses): with no unwind information for this DLL's frames the
  exception reaches `DbgBreakPoint` (xenia-edge now logs the guest stack there).
- **Precodes are patched code.** A method's temporary entry point is a 0x18-byte stub in the code heap
  (`lis r11; ori/lwz r11; mtctr r11; bctr` plus data; `Precode::GetTarget` 0x84BA0138 decodes it,
  `IsPointingToPrestub` 0x84BA05F8 compares it with the prestub), and `SetTargetInterlocked` (0x84D95A80) rewrites
  it and sweeps the data and instruction caches (`KeSweepDcacheRange`, `KeSweepIcacheRange`, 0x18 bytes). xenia-edge
  dropped the stub's translation on the sweep but kept its indirection-table slot, so a call through a patched
  stub ran the old translation (to the prestub) forever: `DoPrestub` returned the stub as the stable entry, the
  stub jumped back to the prestub, with the main thread spinning in `ThePreStub` → `PreStubWorker` → `DoPrestub`
  and three `KeTlsGetValue` per lap (the mscorlib hang after `String::InternalSubString`). Fixed in xenia-edge
  (c57f0ff3c: the slots of a swept range go back to the resolve thunk).

- **The interpreter can take the zapper process's own code.** `UnsafeJitFunction` (0x84D203A0) calls
  `CallCompileMethodWithSEHWrapper` (0x84D1FA98), which first offers the method to the interpreter (0x84C9F598)
  unless the JIT flags have 0x80 or 0x800000, and only then to the loaded JIT. The interpreter takes it in mode 1
  only if the method is in the `Interpret` list (a `MethodNamesList`: `[class:]method[(args)]` entries, `*` for
  any; `InterpretExclude`, `InterpreterMethHashMin/Max` narrow it), read like every setting through the runtime's
  config callback (0x84C5A5A8) from the `<runtime>` section of `D:\xmrun.xex.config` (`<Interpret enabled="*"/>`:
  `tools\xmrun.xex.config`, which `NGen-Package.ps1` stages). With it, compiling mscorlib no longer depends on
  the JIT compiling everything the zapper runs; the methods the runtime insists on compiling (flag 0x80: `IntPtr`
  and `MetadataToken` constructors, `String.wstrcpy`, `Version..ctor`...) still come through `compileMethod`,
  and a decline there is still an `InvalidProgramException`. Without the setting every method the process runs
  is JIT-compiled (the runtime's own startup path first: `AppDomain::SetupDomain`, `String::Substring`...).
- **Shared generic code's hidden argument** (`CORINFO_CALLCONV_PARAMTYPE`): the instantiation's MethodDesc comes in
  the register after `this`, first for a static method. Microsoft's `Interlocked.CompareExchange<__Canon>`
  (mscorlib.ni.dll 0x17E220) is `mr r10,r4; mr r9,r5; mr r5,r6; mr r4,r9; mr r3,r10; bl CompareExchange(object)`:
  r4-r6 were the user's arguments. The compiler keeps the argument in its frame and compiles such a body when
  nothing in it needs a lookup through it (tokens that mention a type parameter decline it, as do calls of
  generic methods and of static methods or constructors of generic classes; `box !!T` for a T that resolves to
  `__Canon` is a no-op, since only reference instantiations share the code).
- **Runtime lookups** (`ICorDynamicInfo::embedGenericHandle`, slot 15, 0x84D09E98) give shared code the exact
  handle a token names. The call is the CLR 2.0 one: `(scope, token, context, tokenKind, result)` (the scope's
  low bit marks a class used as scope, the context's a class), not CLR 4's resolved-token form; the result is
  `CORINFO_GENERICHANDLE_RESULT` (44 bytes: `needsRuntimeLookup` byte, lookup kind 0 this/1 class/2 method,
  then either `{signature, helper, indirections:16, testForNull:8, testForFixup:8, offsets[4]}` or
  `{accessType, handle}`, then `compileTimeHandle`, `handleType` 1 class/2 method/3 field). The zapper's
  wrapper (0x84E2F380) turns a constant into a fixup cell. The compiler reads the context (`this`'s MethodTable
  or the hidden argument), follows the offsets, applies the fixup test (`if (slot & 1) slot = *(slot - 1)`, which
  the zapper sets) and the null test (the helper `(context, signature)` fills the slot), for `newarr T`
  (resolveToken with the Newarr kind names the array type), `castclass`/`isinst T`, `ldtoken T`, `box`/`unbox`
  of a value type mentioning T, and `unbox.any`.
- Calling a generic method (or a static method of a generic class) from shared code goes through
  `ICorDynamicInfo::getCallInfo` (slot 23; the runtime's 0x84D10830 behind the zapper's wrapper 0x84E2FDD0, which
  adds flag 4 and turns the result's handles into fixups). The CLR 2.0-style signature: `(scope, token, context,
  constrained token, caller method, flags, CORINFO_CALL_INFO*)` with flags 1 ALLOWINSTPARAM, 2 CALLVIRT, 0x20 LDFTN.
  The 0xE0-byte result: the method to call at 0, its class at 4, the exact owner at 8, the token's method at
  0x44, `thisTransform` at 0xA8, `kind` at 0xAC (0 call, 1 code pointer, 3 vtable, 4 ldvirtftn),
  `nullInstanceCheck` 0xB0, `contextHandle` 0xB4, `exactContextNeedsRuntimeLookup` 0xB8 and `stubLookup` at 0xBC
  (the same CORINFO_LOOKUP as `embedGenericHandle`'s). Without ALLOWINSTPARAM a callee that would need the
  hidden argument is answered as kind 1: a runtime lookup through the caller's context whose result is the code
  pointer of the exact instantiation (its instantiating stub), called with the arguments as the IL passes them.
  `findMethod` alone is not enough there: it gives a method whose signature has no PARAMTYPE (0x10, GENERIC
  only), the canonical body, which cannot be called without the instantiation.

Two more things about the zapper's phases, found on the way:

- A direct call (`getFunctionEntryPoint` with `IAT_VALUE`) to a method of a generic instantiation makes the zapper
  compile that instantiation in a later pass; a decline there fails the image (hr 0x8013153A, after the warning "while
  compiling method"), unlike a decline in the main pass. Asking for `IAT_PVALUE` for such callees (`call_method`)
  takes the zapper's other path, a cell the runtime fills, and the instantiation is left alone.
- Once a `.ni.dll` the zapper wrote (a PE based at 0) lies beside an assembly, compiling an assembly that depends on
  it fails with hr 0x80070002: the loader takes it for a native image and can't load it. Compile dependents before
  their dependencies, one zapper run each, or wrap each image before the next run.

## Comparison with Microsoft's images (Plex, all five assemblies)

`research\nicompare.py` pairs the methods of two images of one assembly and tallies the differences;
`research\methodnames.py` lists an assembly's MethodDef rows as `token Type::name`. Facts the pairing rests on:

- Each method's pre-header word (at method-12) leads to a 16-byte record of self-relative pointers; its last word
  points to the method's `.pdata` entry and its first to a MethodDesc, but to the MethodDesc of the method **before** it
  in the zapper's list: a method's own MethodDesc is the next record's. (Checked both ways: with the shift, every body
  whose IL has exception clauses has funclets and none without has any, in both images; without it, 82 of Microsoft's
  876 and 24 of our 698 bodies disagree.)
- `MethodDesc::GetMemberDef` (0x84BC8A70) is `0x06000000 | first halfword | chunk's token range byte << 16`, so the
  halfword is the MethodDef row for assemblies under 65536 methods.
- Neither image lays methods out in compile order (Microsoft's by profile, ours by the zapper's choice), so the log
  can't name them; the row can.

Results (Plex 0.0.0.7, our JIT compiling 292 of 316, 1043 of 1170, 188 of 272, 736 of 862 and 223 of 226 methods):

- **No structural differences on any paired method:** the same funclet counts, the same GC header flags (PSPSym,
  generics context, stack base register), GC info code lengths covering the `.pdata` length, and every body of ours
  that reports no safe points makes no call.
- **Microsoft's images hold more bodies** (316 methods → 318 main bodies plus save/restore routines; 862 → 1001):
  instantiations of generic methods over the types the assembly uses, which we decline, and the register save/restore
  routines its prologs call.
- **Code quality, not errors** (counts for PlexNet, 736 pairs): we always set up a frame and an 80-byte outgoing area
  (286 methods are frameless in Microsoft's), our code is 2 to 4 times larger for most methods (every operand goes
  through the frame), we call helpers where Microsoft's code inlines the check (70 methods: write barriers, null
  checks), we use safe points where Microsoft's code is fully interruptible in some methods with loops (20), and
  Microsoft's prologs call `__savegprlr` so its bodies often have more calls than ours.
- **Declines** (the 24, 127, 233, 235 and 3 methods we leave to the interpreter) were, in order of count: generic
  method instantiations (`Get<T>`), shared generic code (methods of `Dictionary`'s nested types, `T`-typed locals),
  locals and arguments of unhandled types, static value-type fields and other static base helpers, `ldftn` of generic
  methods, and a few opcodes. After the features that followed (generic instantiation calls, value types with
  references, boxed statics, `constrained.`, `typeof`, stack arguments) Plex compiles 2954 of 3164 methods, nearly
  all of the rest shared generic code.
- **The whole Vimeo package, 2026-11-03 evening** (`Work
gen\cmp\Vimeo2`): of the 69,168 method bodies in
  Microsoft's 34 images we compile 69,279 (mscorlib 171 more than theirs: the instantiations the zapper asks us
  for), System.Windows 30 short (more than 13 floating-point arguments, 0x130-byte value types, a 2,500-byte
  method), BouncyCastleCrypto 9, Microsoft.Xbox 5, a handful elsewhere, and 24 of the 34 complete.
- **Platform and other apps** (`research\Compare-Package.ps1`): Microsoft's 22 platform images in the Plex package
  (mscorlib, System.Windows, System.Xml, Microsoft.Xbox and the rest) and Vimeo's 9 app assemblies, 30 of them
  compiled and compared the same way (`<Work>\<name>.compare.txt`). The tool checks two errors
  directly: a body of ours without funclets for a method whose IL has exception clauses (`methodnames.py` counts
  them), and a body that calls but reports no safe points. Neither occurs, apart from pairing artifacts on
  overloads and instantiations (the record-to-MethodDesc rule misfires around some stub bodies). Compile rates run
  from 52% (HLSMSS_XBOX, generic-heavy) to 93% (System.Xml). `Newtonsoft.Json` ended in `DbgBreakPoint` at
  `JsonDictionaryContract..ctor`: its `typeof(IDictionary<,>)` is an `ldtoken` of an uninstantiated generic, which
  `findClass` only loads for the Ldtoken token kind (below) and otherwise throws, and a runtime exception inside a
  callback can't unwind through this DLL. With the token kinds it compiles (2585 of 3622 methods). mscorlib needs
  the code cache in a code section (the zapper runs stubs it has written there) and a JIT that compiles everything
  the zapper process itself executes (below).

## Value types, statics and generics: the runtime facts behind the later features

- `ICorClassInfo` slot **11 is `getClassGClayout(class, bytes)`**: a byte per word (1 a reference, 2 an interior
  pointer), returning the count; slot 10 is the alignment, 12 the instance field count. The CLR lays a struct's
  reference fields out first: `struct { string Name; int Id; object Tag; }` is `01 01 00`.
- A **value type's static field is a boxed object in the GC statics**: the slot the helper's base plus the field
  offset reaches holds the box, whose data starts 4 bytes in (observed field attribs 0x8C0009 for `TimeSpan` and a
  user struct; primitives stay inline).
- `getSharedStaticBaseHelper` also answers **366 and 367** for classes with no static constructor: the same
  (module ID, class ID) helpers that needn't run one (the `NoCtor` variants, in CoreCLR's order right after
  364/365), and **368/369** (0x84CC7B90, 0x84CC7360) for a generic instantiation's statics: the same (module ID,
  class ID) arguments, the class ID being the *dynamic* one `getClassDomainID` answers for such classes (the
  helper indexes the module's dynamic entries with it). A `[ThreadStatic]` field gets **373** (GC, 0x84CC8DD0)
  or **374** (non-GC, 0x84CC8CC0) in a class with a class constructor (Silverdash360's `Log`), **375/376** in one
  without (`Thread::get_CurrentUICulture`): the table holds the same two routines for both pairs, taking the same
  arguments as 364/365, the thread's block instead of the domain's (NGenBench's ThreadStaticCctor); 377/378
  (0x84CC9008, 0x84CC8EF0) are their dynamic-class variants. The table follows CoreCLR's order around them: **362/363** (0x84CC7F80, 0x84CC83D0)
  are `JIT_GetGenericsGCStaticBase`/`NonGCStaticBase`, taking the exact MethodTable, and **371/372** their thread
  static kin: what shared code calls, with the MethodTable from its context (`this` object's type, the hidden
  argument, or a MemberRef's parent TypeSpec looked up at run time), since the dynamic ID of an instantiation
  only known at run time cannot be a constant. 364-367 hold small numbers (0xA-0xD) in the table: they are
  dynamic helpers the zapper resolves through `getHelperFtn`.
- **Helper 53** (0x84CCBC58) is `JIT_Ldelema_Ref(array, index, element class)`: a null array throws (code 0x28),
  an index past the length too (0x17), an array whose element class (at array + 8) is not the class exactly
  throws ArrayTypeMismatchException (5); the element's address is array + 12 + index * 4. The helper numbers
  from 52 on follow CLR 2.0's corinfo.h: 52 ARRADDR_ST, 53 LDELEMA_REF, 54 THROW, 55 RETHROW, 56 USER_BREAKPOINT,
  57 RNGCHKFAIL, 58 OVERFLOW.
- `conv.i`/`conv.u` of an object reference is `fixed (char* p = s)`: the result is an untracked pointer, the object
  stays alive through the pinned local (`String::InternalSubString`, `StringBuilder..ctor`).
- `newobj` of a value type the runtime reports as a primitive (`IntPtr(int)`: `asCorInfoType` NATIVEINT, class
  attribs with VALUECLASS) constructs in a temporary slot and pushes its bytes as the primitive.
- The runtime's **P/Invoke IL stubs** keep a 0x210-byte `NDirectMethodFrameStandalone` as a local that they only
  take the address of: a value type over STRUCT_SLOT with no references can be such a local (frame-resident,
  `ldloca`/`initobj` only; loading or storing it whole declines).
- The **interpreter passes a value type argument by reference**: NGenBench's `NamedStructs` run interpreted
  (no image) returns 5044548, where `Rename(a, name)` renamed the caller's `a` as well; compiled, and by hand,
  it is 5044538. An app that works interpreted can therefore behave differently (correctly) once compiled.
- A **TypedReference** local comes as `CORINFO_TYPE_REFANY` (0x15): 8 bytes, the pointer then the type handle; its
  GC layout says `IntPtr`, so the first word is reported as an interior pointer by the compiler's own rule.
  `mkrefany T` fills one from a pointer and T's exact handle (`JitHelpers.GetObjectHandleOnStack<T>`:
  `ldarg.0; mkrefany !!T; stloc.0; ldloca.0; call TypedReference::GetPointerOnStack; newobj
  ObjectHandleOnStack(IntPtr)`). Interior words of value types (`getClassGClayout` kind 2) are reported too.
- A **generic method instantiation** (`Pick<int>`, `Pick<string>`) resolves through `findMethod` to its own
  MethodDesc: over value types the instantiation's code, over reference types an instantiating stub that supplies
  the hidden argument to the shared code. Its `getMethodSig` carries `CORINFO_CALLCONV_GENERIC` and the
  instantiation count but no `PARAMTYPE`, so it is called like any method (through a cell). The shared body itself
  has `PARAMTYPE` and takes the hidden argument described under "Run-time code generation" (compiled when it
  needs no lookup; NGenBench's `SharedArgs<T>`); `Pick<int>`'s body compiles like any method, its tokens
  resolving exactly.
- `asCorInfoType` of an **enum is its underlying primitive** (INT for `enum Color`), not VALUECLASS: anything taking
  "value type" from it must treat the primitives as value types too (`constrained.` did not, and dereferenced the
  enum's value as an object).
- `constrained. T callvirt M` is compiled from the ECMA rules alone: a reference T's pointer is dereferenced, a value
  T that declares M is called directly with the pointer, any other value T (an enum's `ToString`, a struct's Object
  methods) is boxed and M called on the box. `ICorDynamicInfo::getCallInfo`, which would resolve T's own
  implementation of an interface method, is **slot 23** (Silverlight 4's ZapInfo table at file offset 0x18A070 of
  its coreclr.dll, whose slots 4, 12, 26 and 30 match xcoreclr's); used for generic callees from shared code
  (above), not for `constrained.` yet.
- `ldtoken T` for `typeof` needs the RuntimeType object (`RuntimeTypeHandle`'s one field is that object, so the
  struct is reported to the GC), which helper **400** returns for a type handle (`JIT_GetRuntimeType`, 0x84CCF178:
  the MethodTable's writeable data +4 holds the exposed object or a handle-table index; 0x84CCF240 makes it).
  Storing the type handle itself crashes. The helper table is at 0x84AAE758 (403 entries of 4 bytes; helper 403
  there is 0x84CCF590, `JIT_VirtualFunctionPointer`); 393-396 are the runtime-handle lookups, 397 a framed variant
  of 400. `ldtoken` of a field (an array initializer's `RuntimeHelpers.InitializeArray(array, ldtoken field)`)
  is the field handle (`embedFieldHandle`, ICorDynamicInfo slot 14) through helper **402** (0x84CCE450,
  `FIELDDESC_TO_STUBRUNTIMEFIELD`: `RuntimeFieldHandle` holds a stub object); 401 (0x84CCEAA8) is the method one.
  CLR 4's numbering from 397: TYPEHANDLE_TO_RUNTIMETYPEHANDLE, METHODDESC_TO_RUNTIMEMETHODHANDLE,
  FIELDDESC_TO_RUNTIMEFIELDHANDLE, TYPEHANDLE_TO_RUNTIMETYPE, METHODDESC_TO_STUBRUNTIMEMETHOD,
  FIELDDESC_TO_STUBRUNTIMEFIELD, VIRTUAL_FUNC_PTR.
- **Stack arguments**: those beyond r3-r10 go in 8-byte slots from the **caller's SP + 0x50** (NGenBench's
  `XNGenS<hex>` probes, read from compiled code and through the interpreter's thunk alike: the 9th int argument is
  at +0x54, the 10th at +0x5C: an int in the slot's low word, an 8-byte value or a value type's chunk in the whole
  slot). The compiler reserves 8 such slots in every frame and sizes the area per method when a call needs more
  (`c->outgoing`, see "HelloXbox showed no frame"); floating-point arguments whose slot is on the stack go in
  their register and the slot, the fourteenth and later doubles (slot only) are still declined.
- The compiler reports a value type's reference words **with liveness**: a variable's always, an evaluation stack
  slot's at the safe points where the entry holds that struct (`safe_point_stack_refs`), and the slot being filled
  by a `newobj` constructor during that call. Copies into the heap (a field, a boxed static, an array element, a
  return buffer) go word by word through the checked write barrier.

## The JIT DLL's contract with xcoreclr

- **r16 is the current Thread.** xcoreclr keeps `Thread*` in r16 and its callbacks read it without reloading
  (`unaff_r16` throughout the decompile: stack probes, frames, `GetThread`). Microsoft's JIT never touches it;
  ours is built with `-ffixed-r16` (jit and host Makefiles). Before that, whatever GCC had left in r16 at a callback
  made the runtime fault inside `findClass`/`findMethod`/`getClassAttribs` depending on register allocation, which
  is the "layout-sensitive" crash seen with the FAIL-first build and with any larger change to the compiler.
- **r14 and r15 are the runtime's as well** (the ephemeral generation's bounds the write barriers read): the JIT
  and the host are built with `-ffixed-r14 -ffixed-r15` too. A value GCC kept in r14/r15 across a callback came
  back changed: in a build whose instruction loop made one more callback per instruction (`findField`), the
  finally fixup of the DebugLog P/Invoke stub read back as an IL branch to offset 0, so its leave thunk jumped to
  the stub's start and NGenBench printed its first check forever; a build with more logging put the value
  elsewhere and was right. Same symptom class as r16: correctness that depends on register allocation.
- **Runtime IL stubs.** A precode sets r11 to its own address (its word 3 is the MethodDesc) before jumping, so the
  stub's secret parameter is r11 at entry; `StubHelpers.GetStubContext` is an intrinsic that returns it (the
  FCall body loops). P/Invoke stubs do `ldftn target; calli` after `BeginStandalone`: the target's pointer is the
  native entry, read from the cell `getAddressOfPInvokeFixup` (ICorDynamicInfo slot 18) names, not the
  method's own entry (the stub itself). A 0x210-byte `NDirectMethodFrameStandalone` local is only addressed.
- **Nested handlers.** A try inside a handler compiles with that handler's funclet (the clause bookkeeping is
  scoped to the clauses whose try lies in the code being compiled); funclets are compiled outer first, so an
  inner catch's resume address and an inner finally's thunk exist when referred to. A leave to a target inside
  the enclosing handler still runs the finallys it exits.
- **newobj of a generic class from shared code** cannot be built from the exact constructor: reference
  instantiations share the canonical type's MethodDescs, so the constructor's MethodTable is `List<__Canon>`'s
  (the object's dictionary is then wrong: `List<T>..ctor`'s own lookup faulted in helper 395). No ICorJitInfo
  call gives the parent as a token, so the compiler reads the constructor's MemberRef row in the module's own
  metadata (the IL lies in the mapped PE: walk down to the "MZ" page, CLI header, `#~` tables) for its parent
  TypeSpec, and looks that up as a class (NGenBench's `SharedNew<T>`).

- **localloc.** Microsoft's `Utf8String::ToString` (mscorlib.ni.dll 0x2887A8) shows the frame: the prolog of a
  method with handlers (`addi r31, r1, -N; stwu r1, -N(r1)`, r31 the frame register, the epilog `addi r1, r31, N`),
  then per `localloc` the size rounded to 16 (`neg; rldicr ..,0,0x3b`), every page of a large block touched
  (`lwzu r0, -0x1000(r11)` in a `bdnz` loop), the back chain moved with `lwz r10, 0(r1); stwux r10, r1, r12`, the
  block at r1 + 0x50 (above the outgoing area), and a memset when the method has localsinit. The unwinder
  reads r1 back from r31, so the prolog's `stwu` size no longer has to match. Our compiler does the same with
  its outgoing area of 0x90 and zeroes every block. The methods that need it at ngen time (`Utf8String::ToString`,
  `RuntimeParameterInfo::GetParameters`) run in the zapper process with JIT flag 0x80, which the interpreter
  does not take: a decline there is an InvalidProgramException and the interpreter-recursion hang.
- **Checked arithmetic.** `add/sub/mul.ovf` and `conv.ovf.*` are done in 64-bit arithmetic on operands widened
  as their signedness says, followed by a fit test (sign extension or zero extension of the low word equals the
  value; for 64-bit sums the sign rule, for products `mulhd`/`mulhdu` against the low word's sign); a failure
  calls the overflow helper (58). `volatile.` emits `lwsync` before and after the access it prefixes.
  `conv.ovf.*` **from a double** goes through the runtime's conversion helpers, which take f1 and answer in r3:
  13 to int (0x84CC4F00), 15 to long (0x84CC4F78), 17 to uint, 19 to ulong (each compares against both limits
  from the runtime's constant table, then `fctiwz`/`fctidz`; out of range or NaN raises OverflowException through
  0x84C7F5F8 with code 0x2C); a smaller target narrows the int through the integer checks. The table's helpers
  4-10 are the 64-bit multiply and divide routines (`JIT_LMul`, `_LMulOvf`, `_ULMulOvf`, `_LDiv`, `_LMod`,
  `_ULDiv`, `_ULMod`), 11 `ULng2Dbl`, 18 `Dbl2ULng`, 20 and 21 the float and double remainders (`fmod`, f1 and
  f2 to f1, NaN for a zero divisor; 0x84CC5090), which `rem` of doubles calls. 1-3 and 23 are empty; 12, 14, 16,
  25 and 26 are dynamic-helper markers.
  `unaligned.` changes nothing (PowerPC loads and stores take any alignment); `cpobj` is `ldobj` then `stobj`;
  `initblk` and `cpblk` are byte loops on the count register (the marshaling stubs' zeroing and copies).

- **Microsoft.Xbox.FPU.** mscorlib declares `fsqrt`, `fsqrts`, `frsqrte`, `fres`, `fabs`, `fsel`, `fself`, `fpmin`,
  `fpmax` (double and float overloads), `fctiwz`, `fctidz`, `fctiw`, `fctid`, `fcfid` and `frnd` as internal calls
  (MethodDesc classification 1, FCall) that have no entry in the runtime's ECall table: `ECall::GetFCallImpl`
  (0x84C72040) dereferences the null its lookup (0x84C71FB0) returns, a fault at 0 in the middle of resolving a
  method-entry fixup (the first image we ran, the stack `0x84CDB248 0x84D20FD0 0x84BE41A0...`, r29 = the
  MethodDesc, whose first halfword is the MethodDef row: `FPU::fsel`). Microsoft's compiler expands them inline;
  ours does the same (`op_fpu`): `fsel(a, b, c)` is `fsel f0, a, c, b` (a >= 0 ? b : c), `fpmin`/`fpmax` a
  subtraction and a select, the conversions leave the integer bits in the double they return (`fcfid` takes them
  back), `frnd` is `frin`, and the single-precision ones round with `frsp`.
- **Running our mscorlib image:** `test\Test-Mscorlib.ps1` compiles NGenBench (at 0x96000000) and runs it on
  Microsoft's mscorlib.ni.dll, compiles mscorlib in the Vimeo package (NGen-Package writes the wrapped image back
  into the package folder, so the script moves it to the work folder's `ours\` and restores Microsoft's), then runs
  NGenBench with ours beside it. xenia-edge now logs the integer registers with a guest data fault.
- **Relocations of runtime addresses.** Our mscorlib image stopped at "No guest code at 19F06D18 for a call":
  2869 precodes (the stubs the zapper writes for a method with no code in the image, FCalls and declined methods:
  `lis/addi r11` of the MethodDesc, `lis/addi r12` of the target, `mtctr`, `bctr`) jumped to 0x19F06D18 and 2000
  more to 0x19F27BF0, while Microsoft's mscorlib image (2899 and 2000 of the same stubs) jumps to 0x84F06D18
  (`PrecodeFixupThunk`: a transition frame, `PreStubWorker` 0x84B9F178, jump to its result) and 0x84F27BF0, two
  of the six delay-load helpers listed at 0x84AAEDC0 (0x84BBBE90, 0x84F07518, 0x84F07700, 0x84F06D18,
  0x84F27BF0, 0x84CA0768). Nothing is at 0x19F06D18 in the zapper's process either (a probe faulted): it is
  0x84F06D18 + 0x95000000, our load address. The zapper writes the image based at 0 and lists every absolute
  word in .reloc, xcoreclr's addresses included, so pe2nixex.py's rebase moved them; it now moves only words
  whose value lies inside the image. (The dynamic static-base helpers 364-367 never had process-local
  addresses: the earlier reading of 0x19F06D18 as one was this same shift.) The table's form: HIGHLOW on a
  word; HIGHADJ on a `lis` (or an `oris`) with the next entry holding the low half *big-endian*, as the
  instruction stores it (997 of NGenBench's 1000 pair with their instruction that way, the other three read
  the same both ways); LOW on the paired `addi`/`lwz`/`ld`, after its HIGHADJ in table order, with padding
  between them at a page boundary and two of each for `lis r11; lis r12; ld r11; ld r12`, so the LOWs take
  the HIGHADJ decisions first in, first out. Reading the low half little-endian made the moved addresses
  come out right only while the sign bit happened not to matter.
- **The XEX holds the image with basic compression:** runs of zero pages are left out of the file, so reading
  the wrapped .ni.dll at section raw offsets goes wrong past the first zero run (mscorlib's function table
  looked unsorted and garbage-filled); `research\xex.py -b` rebuilds the memory layout from the block list.
  A MethodDesc's first halfword is the whole MethodDef row, not CLR 4's 12-bit remainder (nicompare.py), and
  a body's record (16 bytes before it: MethodDesc, 0, GC info, RUNTIME_FUNCTION, all relative) names the
  MethodDesc of the method before it; the next record's is the body's (checked against the code sizes in
  xclrjit's log: 14648 of 15333 names agree, the rest are instantiations sharing a name).
- **Type.GetTypeFromHandle is an intrinsic.** With the precodes right, NGenBench on our mscorlib faulted in
  PseudoCustomAttribute.GetCustomAttributes reading the null `s_pca` while a TypeInitializationException was
  being built, then overflowed the stack in xcoreclr's dispatcher (the exception's message lookup walks
  ResourceManager -> GetCustomAttributes -> the same class, whose constructor is the one failing). Finding
  the class: `patch_xex.py` plants `lis r0,0x19F0; lwz r0,0x6D18(r0)` (an unmapped address) inside the
  constructor's body (the entry's first words never faulted: whatever enters it skips them), and xenia-edge's
  fault log now dumps the memory behind r3-r10: r4 was the string "System.Reflection.PseudoCustomAttribute"
  and r5 a bare System.Exception with every field null, the runtime's preallocated base exception, used when
  the real throwable can't be made. No fault and no IL_Throw (a copy of xcoreclr.dll with helper 54 jumping
  to DbgBreakPoint, which logs lr and the stack, showed none) came before it, so the failure was a C++ throw.
  The class constructor fills `RuntimeType[]` with `typeof(X)`: `ldtoken X; call Type::GetTypeFromHandle`.
  Microsoft's code is JIT_GetRuntimeType (helper 400) and a store; ours called the method (an internal call
  taking the 4-byte RuntimeTypeHandle struct) and cast what came back to RuntimeType with helper 39, and that
  cast threw. The handle's one field is the RuntimeType object, so the call is now the word itself. That was
  not the failure, though (the constructor still failed with it): poisons inside the constructor showed it
  entered, past the array and the `new Dictionary<RuntimeType, RuntimeType>(9)`, and never back from its
  first `set_Item`, with no exception constructor of ours (74 of them poisoned at once) running before the
  TypeInitializationException's.
- **Interface calls from shared generic code.** A copy of xcoreclr.dll with the raise wrapper 0x84F0A720
  (`mflr; ...; bl 0x84F09F00`, the dispatcher; its return addresses 0x84F0A708/0x84F0A758 are the recursion
  in every stack-overflow trace) jumping to DbgBreakPoint showed the first raise of the run: from
  `JIT_VirtualFunctionPointer` (0x84CCF590) called by our Dictionary<__Canon, __Canon>.Insert under
  AppDomain.SetDataHelper in AppDomain.Setup, long before PseudoCustomAttribute. Our interface call passed
  the canonical IEqualityComparer<__Canon> and its method (exact_owner of the token); the resolver can't find
  those in the comparer's interface map and raises, and the runtime couldn't build that throwable either (the
  preallocated base Exception became the inner exception of the later TypeInitializationException).
  Microsoft's shared Insert (0x1DC9C4) reads the exact interface MethodTable and MethodDesc from this's generic
  dictionary (`[MT+0x20]` slots 0x38 and 0x3C, helper 395 `JIT_GenericHandleClass(MT, signature)` when a slot
  is still null) and only then calls the resolver with (object, exact interface, exact method). Ours now
  looks both up before loading the object: the interface through the MemberRef's parent TypeSpec read from
  the metadata (as load_owner_of_ctor does; embedGenericHandle embeds what a token names, and asking it
  with TOKENKIND_NEWOBJ gave the method's lookup twice and hung the mscorlib compile), the method through
  embedGenericHandle with TOKENKIND_METHOD. NGenBench's GenericIface check calls IBox<T>.Get from
  Caller<T> shared over string and object.
- **First-execution trace.** xenia-edge's `--trace_define_start=<dec>` / `--trace_define_end=<dec>` log
  `Defined guest function X` whenever a function in that range is translated, i.e. first called (the thunks
  of .bisland show up too); with the image's function table that is a trace of which methods ran, in order,
  something a native-image run otherwise never tells (Run-App.ps1 -ExtraArgs, from PowerShell so the array
  survives). The lines `Invalid instruction 95179130 00821968` after a method's final `bl IL_Throw; trap`
  are the translator reading on past the trap into the next method's record: noise.
- **Security objects.** With the interface calls right the trace showed Environment.GetResourceString entered
  from PermissionTokenFactory.BuiltInGetToken's finally, with no exception dispatched (the dispatcher's entry
  0x84F09F00 hooked); a planted fault in GetResourceString gave the caller, SecurityRuntime.Assert, and the
  key "ExecutionEngine_MissingSecurityDescriptor": the frame of AppDomain.CreateAppDomainManager, which calls
  PermissionSet.Assert, had no security object. Microsoft's GC info for it has header flag 2 with the
  object's offset from the caller's SP (-416) and a valid range (3-bit varints: start = (a + 1) * 2, end =
  (words * 2 - b) * 2, bytes; GcInfoDecoder 0x84EFB348); the slot is zeroed in the prolog. No method's
  attributes carry CORINFO_FLG_SECURITYCHECK (0x400); PermissionSet.Assert/Demand and
  CodeAccessPermission.Assert/Demand have 0x34100051, and a method that calls one with bit 0x20000000
  (CORINFO_FLG_DONT_INLINE_CALLER, the [DynamicSecurityMethod] ones) gets the slot: the IL pre-scan looks up
  each call's callee. With it, startup reaches the host's assembly loads.
- **Hard-bound dependents.** The runtime then rejected Microsoft's System.Core.ni.dll ("Rejecting native image
  %s because native image dependency %s does not contain all the types saved in", 0x84AB5CC0; it hard-binds
  to Microsoft's mscorlib.ni) and broke into the debugger (DbgBreakPoint from 0x84D6BF20, a configurable
  break on rejection), and without native images for the platform assemblies the retail runtime looks for
  `\xclrjit.dll` (it has no JIT of its own: Microsoft's run never looks) and the host shows error 2101. The
  platform assemblies an app uses have to be compiled against our mscorlib image as a set, one NGen-Package
  run each (`tools\NGen-Platform.ps1`): the zapper finds the raw, unwrapped image of an assembly compiled
  earlier in the same run in its stage and xenia can't load it ("XNA executables are not yet implemented"),
  and the dependents fail with 0x80070002.
- **Array layout.** Value-type elements start at +16, not +8: Microsoft's Dictionary.Insert
  (0x1DCAB0) forms its Entry<,> array's data pointer with `addi r4, r8, 0x10` and its int array's with
  `addi r6, r31, 8`. Our +8 for every non-reference element was consistent with itself, so NGenBench's own
  struct arrays passed, but Array.Copy (0x84ECEC00: the start from the MethodTable's base size at +4, the
  element size from its flags) and the GC saw each struct array's first element eight bytes off. Found with the
  platform compile running on our mscorlib: the zapper process faulted in Dictionary.FindEntry under
  RuntimeResourceSet.GetObject (its cache is a Dictionary<string, ResourceLocator>), and NGenBench's
  DictStructRef then lost its first key after every Resize; ArrayCopyProbe pinned it to the copy of struct
  arrays (the first entry's first eight bytes unwritten, later entries intact, int arrays fine).
  8-byte primitives are not: generalizing the +16 to long[] and double[] made Enum.ToString give every name one
  off (the runtime's InternalGetValues builds the ulong[] at +8), and Array.Clear(array, 0, 1) from the runtime
  clears the word before an element read at +16 on a long[] and a double[], the element itself on a struct
  array (NGenBench's ArrayClearProbe). Nor are small value types: ArrayOffsets fills every field of three
  elements and lets the runtime clear element 0, and the zeroed words put 4-, 8- and 12-byte value types
  (generic or not, holding a long, holding references) at +8, and 16-, 20- and 24-byte ones (ints, doubles,
  longs, references) at +16: the data offset depends on the element size alone. (Written at +16, a 12-byte
  element array's last element lies past the object the runtime sized, and reads back garbage.) That cost
  HelloXbox: XcpImports.MethodPack keeps the GCHandles it allocates for a native call in a
  GCHandle[], which our code wrote at +16, four bytes past the object the runtime had sized for +8, into the
  next object's MethodTable word; a later allocation put a MethodTable there, GCHandle.Free passed it to the
  runtime as the handle, and the store into the read-only image faulted (an AccessViolationException the app
  didn't survive, under Control.GetTemplateChild).
- **Exception messages on the console** are the bracketed fallbacks (`[Arg_InvalidOperationException]`): the
  retail mscorlib's `mscorlib.resources` is a 1,074-byte stub and the texts live in a `mscorlib.debug.resources`
  assembly the packages don't carry (the runtime probes for it on every lookup), so a message of that form is
  not a sign of a failed lookup of ours (NGenBench's ResourceProbe).
- **HelloXbox showed no frame on our images** while its logic ran (the traced run reached the WebClient
  requests), and NGenBench shows none even on Microsoft's platform (its package has no title: nothing to compare
  there). Naming the whole first-execution trace of HelloXbox on Microsoft's images and on ours
  (`trace_names.py`) and diffing the sets of methods left ten run by Microsoft's alone, among them
  `FrameworkCallbacks.Initialize`: 114 `ldsfld` of the callback delegates and one native call taking them all
  (IL max stack 115), declined for its stack depth and so interpreted, and interpreted it evidently doesn't
  register what rendering needs (the other callbacks reach managed code through the image's thunk records).
  Its call wants 106 stack argument slots, so the outgoing area is sized per method now, as Microsoft's: a
  frame reserves 8 slots, and a call that needs more fails the compile with the count, which compiles the
  method again with that area (`c->outgoing`; the GC info's outgoing size follows it). A funclet's frame is
  fixed, so a call with stack arguments inside a handler declines.
- **Multi-dimensional arrays** (`new T[2, 2]`: `newobj` of the array type's constructor) come from helper 27,
  a vararg helper (it spills r5-r10 to its caller's slots) that takes the array type's handle (a tagged TypeDesc
  pointer: 0x839913F2 in Microsoft's image) in r3, the constructor's MethodDesc in r4 (whose signature says how
  many dimensions follow; passing a count there made the bench exit at startup, since the runtime's warm-up
  runs SZArrayHelper_HACK) and the dimensions from r5 on like a call's arguments. Microsoft's SZArrayHelper_HACK
  (mscorlib.ni.dll 0x3095E0 and 0x3096B0) calls it so; 28 is its sibling. The array class has no name from
  getClassName, so the rank is checked first. The element accessors (`Get`, `Set`,
  `Address`) are the runtime's array methods, called like any method. That was the last decline with
  instantiations in mscorlib (23 of SZArrayHelper_HACK, the runtime's warm-up of common instantiations).
- **Instantiation argument with a return buffer:** r3 the buffer, r4 this, r5 the instantiation argument
  (Microsoft's KeyValuePair<__Canon, 16-byte struct>::get_Value at 0x472FDC copies from r4 to r3 and never
  touches r5), the order the prolog already used; the 11 methods declined for it compile.
- **The JIT's own memory.** xcoreclr loads its code cache module (`CLRCodeCachedll_0.xex`) at **0x8D000000**,
  so xclrjit.dll (based at 0x8C800000) must end below it: a build whose tables grew the bss to 0x8D075000 let
  the runtime fault reading guest 0x00400000 right after `sxsJitStartup`, before the first method. `xclrjit.ld`
  asserts the bound. Clearing the whole compiler context (1.4 MB) for each method was most of the compile time:
  the full Test-Mscorlib pipeline (mscorlib, 14 platform assemblies, NGenBench, the run) went from about 4
  minutes to 1 once only the small part is cleared.
- **Value-type field order.** The runtime orders an auto-layout struct's fields by size, so references come first
  only among fields of their size: NGenBench's `Huge` (a string and forty doubles, 328 bytes) has the string at
  word 80, after the doubles. The compiler's reference masks cover a value type's first 256 words (1 KB).
- **Value types over 136 bytes** come from the runtime's marshaling stubs: Microsoft.Xbox's P/Invoke stub for a
  `MediaState` (0x130 bytes, by reference) copies the argument into a local with `cpobj` and passes the local's
  address to `ValueClassMarshaler.ConvertToNative`; Microsoft.Advertising's stubs have 0x90-, 0x4C0-, 0x80C- and
  0xCF8-byte ones, mscorlib's TimeZoneInformation one 0xAC. The compiler sizes a stack entry's room per method
  (136 bytes, or the largest value type the method uses, up to 4 KB; a retry like the outgoing area's).
- **refanytype** (`FE 1D`) has no operand (the scan had taken it for 6 bytes). It yields a RuntimeTypeHandle, whose
  one field is the RuntimeType: helper 400 on the TypedReference's second word, as `ldtoken` does.
  `TypedReference.GetHashCode` is `ldarg.0; ldobj TypedReference; refanytype; call GetTypeFromHandle`. A
  TypedReference result is assumed to come back in r3 like any 8-byte value type (`ArgIterator.GetNextArg`;
  not exercised: C# can't declare one).
- **A leave from a try inside a catch to outside the catch** (HLSPlayback.DataStreamResponseReceived: a
  try/finally in the catch, the whole method in another try/finally, `leave` to the final `ret`): the inner
  finally runs from the catch funclet (its thunk lies in the funclet's code), and the catch then returns with the
  resume address of the next finally's thunk in the main body. Resuming at the inner thunk instead would run
  main-frame code inside the funclet's range, where the unwinder takes it for the funclet. Every thunk on a
  leave's way is requested before compiling, so the main body's ones are placed with it (NGenBench's
  LeaveCatchFinally).
- **Multi-dimensional array Address** (`long[,]::Address(int, int)`, what C# makes of `ref a[i, j]`;
  RijndaelEngine.GenerateWorkingKey, the Vimeo package's last decline) has PARAMTYPE in its signature: the array's
  exact type handle goes in the register after `this`, the indices after it (NGenBench's MdArrayAddress, a
  `long[,]` and a `string[,]`). Microsoft's code inlines the element access instead.
- **Bodies the zapper never asks for.** With that, every method the zapper offers in the Vimeo package compiles,
  and our images hold 69,126 bodies to Microsoft's 69,168: System.Windows 28 short with all 9,260 offered methods
  compiled, a few elsewhere. They are bodies Microsoft's zapper run produced and ours never requested
  (instantiations reached through its JIT's direct calls, which ours makes through cells; inferred), not declines.
- **Types must be loaded before code that embeds them runs.** For a type of the image being compiled,
  `embedClassHandle` answers its MethodTable's address, and the JIT has to call
  `classMustBeLoadedBeforeCodeIsRun(class, 0)` (ICorClassInfo **slot 29**, 0x84E30F18; Silverlight 4's order) so
  the zapper adds a fixup that loads and restores the type before the method first runs. Without it our code
  allocated objects of types never restored: a type's vtable slots for methods inherited from another module
  point to import thunks (`lis r12; lwz r12, cell; mtctr; bctr`) whose cells are 0 in the image (Microsoft's
  images have as many: VimeoXbox 433 in both) until the restore binds them. Silverdash360's `DebugException`,
  allocated by our `newobj`, had `Message` jump to address 0 inside a catch handler; the runtime's vectored handler
  then faulted on the frame (NGenBench's CatchFirstMessage). Leaving either the allocating method or the catching
  one to the interpreter hid it: the interpreter's allocation restores the type.
- **A funclet's evaluation stack lives in its parent's frame slots**, and while it runs the runtime reports the
  parent's frame at the parent's safe point: in methods with handlers every evaluation-stack slot is reported
  wherever the frame is (and cleared in the prolog), not only where the parent's own stack holds a reference.
- **On a real console (2.0.17559, a homebrew-capable one) our images loaded nowhere until their page
  descriptors were split at 64 KB.** The kernel's `XexLoadImage` (0x8007D7C0) refuses with
  STATUS_ACCESS_DENIED unless the flags have 0x8 or 0x80, and its module-list lookup (0x80079768) refuses an
  already loaded module whose type bit (flag 0x1) differs from the request: the CLR loads `.ni.dll` images with
  flags 0x9. The loader (0x8007C4F0) passes every page descriptor to the hypervisor, `HvxLoadImageData`
  (syscall 0x14; the syscall table is at HV 0x15FD0, the handlers behind the pointer at HV 0x80, this one at HV
  0x2A120), which accepts a descriptor only if its run is at most 64 KB, doesn't cross a 64 KB boundary, has
  kind bit 1 or 2 and not 4, and then hashes the run and the next descriptor against the chain. `pe2nixex.py`
  wrote one descriptor per section run, so the console answered STATUS_IMAGE_CHECKSUM_MISMATCH (C0000221) and the
  CLR silently fell back to IL; xenia checks none of this. Microsoft's images have a descriptor per 64 KB
  (mscorlib: 131 for 0x830000 bytes). `research\xexhashes.py` checks a chain. Silverdash360's own native image
  then ran on the console: its benchmarks 28 to 300 times faster than interpreted, its suites unchanged.
  Neither the execution info (title ID 0) nor the host's PLOADMOD preload list mattered.

## Apps of other runtime builds

- **Our images bind to Plex's runtime build.** The shipped apps carry five CLR builds (the SDK's
  docs\runtime-inventory.md). IGN, Crackle and Netflix ship the one of 2011-11 and Wall Street Journal the one of
  2012-05, with an mscorlib.dll and framework IL of their own. In a copy of IGN given our images, the zapper of
  IGN's own xcoreclr (NGenWorker at 0x84E0A1C0, Plex's at 0x84E0A6D0) failed every assembly with hr 0x80131F06,
  GalaSoft.MvvmLight (which needs only the platform) as well; with Plex's runtime and framework files swapped in,
  the same assembly compiled, and so did all 26 of IGN's, without a decline. The 0x80131F06 was not the two
  missing assemblies below: with EPIX's copies of them in the package it stayed the same.
- **Microsoft rebuilt the framework for every app.** IGN's, Crackle's and Netflix's System.Windows.dll,
  Microsoft.Xbox.dll and System.Net.dll have one file version (3.7.60901) and size each and a different MVID in
  every app; Napster's, GameTrailers' and Plex's are three builds (4.1.20917, 4.1.21107, 4.1.31216). Only
  mscorlib is shared within a CLR build (MVID a18a0811 for 2011-11, 28d1815f for 2012-07, which Napster,
  Crunchyroll, GameTrailers and Plex have). A native image dependency is checked by MVID, so our platform images
  (of Plex's IL) serve only packages with Plex's framework files: `test\Run-OnOurs.ps1` compiles the whole
  platform in any other package against the set's mscorlib, and refuses a package whose mscorlib isn't the set's.
- **Our mscorlib compiles on the 2011-11 runtime** (IGN's xcoreclr, NGenWorker at 0x84E0A1C0): 15,745 methods,
  no decline, an image the size of ours on Plex's. Its platform doesn't: System, System.Core,
  System.Xml, System.Net, RuntimeHost, Interactivity and Navigation compile, but System.Windows crashes xenia
  before the JIT is asked for a method, the zapper faulting on guest loads at 0x84BD8584 from 0x13010014 (r28 =
  0x13010000, AssemblyRef 0x23000003 in r26 and r30; stack 84BF3CD8 84C04444 84BDB244 84BDE42C 84C38904 84E17138
  ...). The 2011 xcoreclr is a different build from the one mapped here, so its addresses are unmapped.
- **The zapper's "stack overflow" on Napster, Crunchyroll and GameTrailers was the same mismatch.** Compiling
  their assemblies against our platform images (of Plex's IL) ended in xcoreclr's stack-overflow path
  (FUN_84d6bed0: HRESULT 0x800703E9, exception 0xE053534F, then the `DbgBreakPoint` at 0x84D6BF20 that a rejected
  dependency image also reaches), after "ComputeHash: Not implemented" (FUN_84d75858, a stub that zeroes the
  hash) from the dependency check FUN_84bfd268. Their framework IL is another build than Plex's (4.1.20917 for
  Napster); with the platform compiled in Napster's package, all 13 platform assemblies and all 12 of Napster's
  own compile, without a decline.
- **Napster on our images throws a SecurityException at startup (open).** It exits after 5 s with hr 0x8013150A,
  as IGN does on Plex's platform: on our platform images with its own assemblies interpreted too, but not on
  Microsoft's platform images with them interpreted (it runs on). The two runs part after Microsoft.Xbox.Storage
  and its string resources load: Microsoft's goes on to open `xnaa:\Settings.dat` through
  `Microsoft.Xna.Framework.Storage` (called by Rhapsody.Utility and IdentityMine.Xbox.Presentation), ours looks
  up mscorlib's exception messages instead. It isn't that storage path alone: the SDK's PlatformCalls sample
  (StorageDevice, BeginOpenContainer, a file round trip, the cache drive, the playback bar) passes on our images
  of Plex's platform and of Napster's (its runtime, framework and host laid over the sample, the platform
  compiled in the package). No method of Napster's platform build has declarative security of its own
  (Microsoft.Xbox.Storage: RequestMinimum on the assembly only). Removing a platform image, to have it compiled at
  run time, shows the runtime's error dialog instead, with xclrjit.dll never loaded, so the images can't be
  bisected that way.
- **But a 2011 app doesn't run on Plex's platform.** IGN with Plex's runtime and framework exits after 5 s with
  hr 0x8013150A (COR_E_SECURITY, the host's `[XAPI RETURN VALUE] -2146233078`): on our images, on our platform
  with its own assemblies interpreted, and on Microsoft's platform images (Plex's) alike, with its own host or
  the SDK's, and with the two missing parts taken out of its manifest. It loads IGN.Xbox.Common, IGN.Xbox.Data,
  GalaSoft.MvvmLight, Newtonsoft.Json and Ninject, then mscorlib looks for its exception message resources. The
  `Microsoft.Xbox` types' security attributes are the same in both mscorlibs. On its own runtime with
  Microsoft's images it draws its first screen.
- **System.Xml.Serialization and Microsoft.Xbox.RuntimeHelpers are unified with System.Xml and mscorlib.** 45 apps'
  manifests list both and the apps' code references them (IGN.Xbox and IGN.Xbox.Common: RuntimeHelpers; Crackle's
  Cypress.Core.XBox: XmlSerializer), yet no package has either file except EPIX, whose copies are reference
  assemblies (RuntimeHelpers' `ALU`, `FPU` and `Cache` methods are InternalCall, every other body 2 bytes; the same
  in Serialization). Every mscorlib defines `Microsoft.Xbox.RuntimeHelpers`, `FPU`, `ALU` and `Cache`, and every
  System.Xml the `System.Xml.Serialization` types. Microsoft's IGN.Xbox.ni.dll records it: its CORCOMPILE
  dependencies (0xDC bytes each: AssemblyRef token, the AssemblyDef token it bound to, the MVID, timestamp and
  IL size, the native image signature last) have AssemblyRef 0x2300001A Microsoft.Xbox.RuntimeHelpers bound to
  def 0x23000002 mscorlib (its MVID), and 0x2300002A System.Xml.Serialization to def 0x23000007 System.Xml. The
  names are in no runtime binary as text, so the binder's table is keyed otherwise. It isn't an extraction loss:
  a raw scan of IGN's GoD directory gives the same 119 files `extract_god_packages.py` extracts.

## Diagnostics

- **Screenshots.** A capture of xenia-edge's D3D12 window (PrintWindow) often gives xenia's splash in a stale,
  smaller frame long after the app has drawn, so it can't tell a rendering fault (it once made a correct code
  change look broken). The samples draw their root visual into a `WriteableBitmap` (what Silverdash360's
  `ui.screenshot` does) and write its pixels to `game:\screenshot.argb` with `NtCreateFile`/`NtWriteFile`
  (xboxkrnl 0xD2/0xFF, `NtClose` 0xCF); xenia writes the app's folder only with `--allow_game_relative_writes`. The
  bitmap shows what Silverlight has laid out, so an app must wait for its content: PageNavigation's first page
  (an `XboxApplicationFrame` navigation, then its entrance transition) was still missing sixty frames after it
  arrived, and drawn two seconds after. xenia's own F12 screenshot needs a title id, which the samples lack.
- xenia's "Invalid instruction" lines come from translating whole blocks: a method's last `trap` is followed by
  the next method's 12-byte pre-header, which xenia decodes and reports, though nothing executes it
  (`--log_lr_at_instruction` there never fires).
- To see the registers at an instruction of an image of ours, plant `lwz r0, 0(r0)` there in a copy
  (`patch_xex.py <in> <out> <address>=80000000`): xenia logs every register and the memory behind r3-r10 at the
  fault. The JIT built with `XNGEN_DECLINE` (`make EXTRA_CFLAGS='-include decline.h'`, the header defining it as
  `"Class::Method;..."`) leaves the listed methods to the interpreter, to find a miscompiled one.

- xenia-edge's GDB stub (`--debug --gdbport=N`, `research\gdbpeek.py` speaks the protocol) crashes the emulator on
  connect: `Processor::SuspendAllThreads` dereferences `thread->thread()`, which is null for guest threads under
  xenia-edge's fiber scheduler. A first-execution trace plus `research\nishow.py` (one method's bodies from an image)
  found the looping stub instead.
- `research\patch_xex.py --jump <address>=<target>` patches a test copy of `xcoreclr.dll` to jump into the host; the
  host's `xngen_fatal_hook` logs `HandleFatalError`'s caller and the guest stack (xenia won't let a title write to
  another module's code at run time).
- `jit\compiler.c` logs each method's class, name, result and code address to xenia's log.
- `findClass`'s token kind (`CorInfoTokenKind`, the last argument) is checked by xcoreclr (0x84D06430): Class 0x01,
  Method 0x02, Field 0x04, Ldtoken 0x17, Casting 0x21, Newarr 0x41, Box 0x81, Constrained 0x107, Ldvirtftn 0x200.
  Only Ldtoken may load an uninstantiated generic type; Casting, Newarr, Box, Constrained and Ldvirtftn throw for a
  byref or void type. Anything thrown inside a callback is fatal for the zapper (no unwind data for our frames).
- xenia-edge (`tools\Run-Xenia.ps1 -IdleSeconds`, default 20) stops a run whose log stops growing; xenia's guest
  scheduler logs the running thread's return-address chain every ~10 s while no frame is presented, and
  `DbgBreakPoint` logs the caller's chain before stopping the process. Map the addresses with the largest
  `//==== <address>` at or below each in the decompile (`research\ghidra\DumpAll.java`), and our DLL's with
  `powerpc-xsdk-elf-addr2line -f -e jit\build\xclrjit.elf`.

## Open questions

- **NGenBench hung twice at PInvokeUnderGC** (2026-11-04, the constant pool build; again later that day, the build
  with xsdk's elf2xex, which passed on the rerun): after BarrierRegisters the log
  went quiet with the churn thread started, and the run stopped idle; the same images passed in the next seven runs
  and the earlier and later builds in theirs. A GC race with the P/Invoke stub's frame, or xenia's scheduling, is
  open: the check runs 2000 one-millisecond `KeDelayExecutionThread` calls while another thread allocates.


- The remaining slot names where xcoreclr differs from Silverlight 4 (`ICorMethodInfo` has two more slots,
  `ICorModuleInfo` one more, `ICorDynamicInfo` one fewer) and the `CORINFO_FIELD_INFO` layout.
- Filter funclets (funcKind 2): what they receive and return.
- Whether the parent's frame is reported at the catch's resume address or at the throwing call while a catch runs
  (our compiler makes both safe points).
- The managed calling convention details beyond `this`/arguments in r3-r10: hidden arguments (generic context,
  return buffers).
- What the unwinder's `DbgBreakPoint` objects to with the `PlexResources` image.
- A build of the JIT whose `FAIL` keeps the first reason instead of the last (one instruction's difference, in
  `jit\compiler.c`) makes the zapper crash on PlexNet, always compiling `PlexContainer::.ctor`: `getClassAttribs`
  on a class handle it had just answered for ends in a hash lookup returning -1, a fault at 0x79B and
  `DbgBreakPoint`. A trace of every runtime call the JIT makes, with its first argument, is identical in the two
  builds up to that call; only our frames' stack addresses differ (by 0x20). It is not the code cache (it happens
  with the code cache working) and not uninitialized locals (`-ftrivial-auto-var-init=pattern` changes nothing).
