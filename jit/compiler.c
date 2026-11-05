// The IL compiler: a baseline (non-optimizing) compiler from IL to Xenon PowerPC code.
//
// Every argument, local and evaluation stack entry has a slot in the method's frame, and each IL instruction loads
// its operands from slots into scratch registers, computes and stores its result. That keeps the compiler simple and
// its GC reporting exact: object references live only in slots set aside for them, which the prolog zeroes and the
// GC info reports as tracked and live at every call (the only GC safe points). Registers hold references only within
// one IL instruction, never across a call.
//
// The frame follows Microsoft's compiled managed code, so xcoreclr's unwinder reads it (docs/RESEARCH.md):
//
//   prolog   mflr r12; stw r12,-8(r1); stwu r1,-F(r1)            (prolog length 3)
//   epilog   addi r1,r1,F; lwz r12,-8(r1); mtlr r12; blr
//   r1+0x00  back chain, linkage and the 0x50-byte outgoing argument area
//   r1+0x50  slots: non-GC stack entries, non-GC locals and arguments, a scratch slot, then the GC slots
//   r1+F-8   the saved link register
//
// A method with exception handlers addresses its frame through r31 instead, so its handlers, compiled as funclets
// after the main body, reach the same slots (Microsoft's layout, PlexFoundation.ni.dll 0x94ADF4B0):
//
//   prolog   mflr r12; stw r12,-8(r1); li r0,0; stw r0,4(r1); std r31,-0x10(r1); addi r31,r1,-F;
//            stwu r1,-F(r1); addi r12,r1,F; stw r12,F+4(r31)     (prolog length 9; the last two store the PSPSym,
//                                                                   the caller's SP, at caller SP + 4)
//   epilog   addi r1,r31,F; lwz r12,-8(r1); mtlr r12; ld r31,-0x10(r1); blr
//   funclet  std r31,-8(r1); lwz r3,4(r3); stw r3,4(r1); addi r31,r3,-F; mflr r12; stw r12,-0x10(r1);
//            stwu r1,-0x60(r1)   ...   lwz r1,0(r1); ld r31,-8(r1); lwz r12,-0x10(r1); mtlr r12; blr
//
// A funclet gets the frame it was called for in r3 (its +4 holds the PSPSym) and, for a catch, the exception in r4;
// a catch funclet returns the address to resume at in r3. The runtime calls handlers when an exception passes; on
// the normal path the main body calls a finally funclet itself, from a thunk placed after the try's code (so outside
// the try's range), as a leave out of the try needs.
//
// Arguments arrive in r3-r10 (this first), int64 in one register; results return in r3 or f1. A method or construct
// this compiler doesn't handle is declined with CORJIT_BADCODE, which leaves it IL for the runtime's interpreter.
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include <xsdk/kernel.h>

#include "compiler.h"
#include "gcinfo.h"
#include "ppc.h"

// ---------------------------------------------------------------------------------------------------------------
// The runtime interface used here (slots in jitee.h, names from Silverlight 4's ZapInfo vtables).

enum {
	MI_getMethodAttribs = 2,
	MI_getMethodSig = 5,
	MI_getEHinfo = 11,
	MI_getMethodClass = 12,
	MI_getMethodVTableOffset = 14,
	MOD_findClass_ = 0,
	MOD_findMethod_ = 2,
	MOD_resolveToken = 3,
	MOD_findSig = 5,
	CI_asCorInfoType = 0,
	CI_getClassName = 1,
	CI_getClassAttribs = 4,
	CI_getClassModuleIdForStatics = 6,
	CI_getSDArrayForClass = 8,
	CI_getClassSize = 9,
	CI_getClassGClayout = 11,  // (class, bytes): a byte per word, 1 for a reference, 2 for an interior pointer; returns the count
	CI_getSharedStaticBaseHelper = 21,
	CI_initClass = 28,
	CI_classMustBeLoadedBeforeCodeIsRun = 29, // (class, sig or 0): the zapper adds a fixup loading the type first
	CI_getArrayRank = 39,
	ARG_getArgNext = 0,
	ARG_getArgType = 1,
	DYN_getHelperFtn = 4,
	DYN_getFunctionEntryPoint = 7,
	DYN_getFunctionFixedEntryPointInfo = 8,
	DYN_embedClassHandle = 12,
	DYN_embedMethodHandle = 13,
	DYN_embedFieldHandle = 14,
	DYN_embedGenericHandle = 15,
	DYN_getAddressOfPInvokeFixup = 18,  // (method, &indirection) -> the cell holding the native target
	DYN_getCallInfo = 23,
	DYN_getClassDomainID = 26,
	DYN_constructStringLiteral = 30,
};

typedef CORINFO_CLASS_HANDLE (*FindClassFn)(void *, CORINFO_MODULE_HANDLE, uint32_t, CORINFO_METHOD_HANDLE, uint32_t);
// CORINFO_RESOLVED_TOKEN (CLR 4). xcoreclr's ModuleInfo is Silverlight 4's with resolveToken (0x84D07640 in the
// runtime) inserted at slot 3: 0 findClass, 1 findField, 2 findMethod, 3 resolveToken, 4 getMemberParent, 5 findSig,
// 6 findCallSiteSig.
typedef struct {
	CORINFO_METHOD_HANDLE context;  // a method handle (flag bit 0 clear) or class handle | 1
	CORINFO_MODULE_HANDLE scope;
	uint32_t token;
	uint32_t kind;                  // CorInfoTokenKind
	CORINFO_CLASS_HANDLE cls;       // out: the exact type, for a member the type that declares it
	CORINFO_METHOD_HANDLE method;
	CORINFO_FIELD_HANDLE field;
	const uint8_t *type_spec;
	uint32_t type_spec_size;
	const uint8_t *method_spec;
	uint32_t method_spec_size;
} ResolvedToken;

// ICorDynamicInfo::embedGenericHandle's result (CORINFO_GENERICHANDLE_RESULT): how code gets the handle a token
// names, exactly, from shared generic code: a constant, or a lookup at run time through the generic context (a
// chain of loads from this object's MethodTable or the hidden instantiation argument, and a helper that fills the
// dictionary slot the first time).
typedef struct {
	uint8_t needs_runtime_lookup;
	uint8_t pad[3];
	uint32_t runtime_lookup_kind;  // 0 this object's type, 1 the class instantiation argument, 2 the method's
	union {
		struct {
			void *signature;
			uint32_t helper;
			uint16_t indirections;
			uint8_t test_for_null;
			uint8_t test_for_fixup;
			uint32_t offsets[4];
		} runtime;
		struct {
			uint32_t access;
			void *addr;
		} constant;
	};
	void *compile_time_handle;
	uint32_t handle_type;
} GenericHandleResult;
// xcoreclr's embedGenericHandle (0x84D09E98, ICorDynamicInfo slot 15) takes the token the CLR 2.0 way: the scope
// (a module, or a class | 1), the token, the context (a method handle, or a class | 1), the token's kind, then
// the result (44 bytes).
typedef void (*EmbedGenericHandleFn)(void *, CORINFO_MODULE_HANDLE, uint32_t, void *, uint32_t, GenericHandleResult *);
typedef void (*ResolveTokenFn)(void *, ResolvedToken *);
// findSig(scope, token, context, &sig, out): the runtime's (0x84D08460) has a fifth, optional out parameter that
// it writes when not null.
typedef void (*FindSigFn)(void *, CORINFO_MODULE_HANDLE, uint32_t, CORINFO_METHOD_HANDLE, CORINFO_SIG_INFO *, void *);
typedef CORINFO_CLASS_HANDLE (*GetSDArrayForClassFn)(void *, CORINFO_CLASS_HANDLE);
typedef uint32_t (*GetArrayRankFn)(void *, CORINFO_CLASS_HANDLE);
typedef void (*ClassMustBeLoadedFn)(void *, CORINFO_CLASS_HANDLE, CORINFO_SIG_INFO *);
typedef CORINFO_CLASS_HANDLE (*GetFieldClassFn)(void *, CORINFO_FIELD_HANDLE);
typedef uint32_t (*GetClassSizeFn)(void *, CORINFO_CLASS_HANDLE);
typedef uint32_t (*GetClassGClayoutFn)(void *, CORINFO_CLASS_HANDLE, uint8_t *);
typedef uint32_t (*GetSharedStaticBaseHelperFn)(void *, CORINFO_FIELD_HANDLE, int32_t);
typedef uint32_t (*GetClassModuleIdForStaticsFn)(void *, CORINFO_CLASS_HANDLE, void **, void **);
typedef uint32_t (*GetClassDomainIDFn)(void *, CORINFO_CLASS_HANDLE, void **);

// CorInfoTokenKind for findClass: what the token is used for. xcoreclr's findClass (0x84D06430) loads an
// uninstantiated generic type (typeof(IDictionary<,>)) only for the Ldtoken kind and throws a TypeLoadException
// otherwise, and refuses a byref or void type for the casting, newarr, box, constrained and ldvirtftn kinds; an
// exception thrown inside a callback can't unwind through this DLL and ends the zapper in DbgBreakPoint.
enum {
	TOKENKIND_CLASS = 0x001,
	TOKENKIND_METHOD = 0x002,
	TOKENKIND_LDTOKEN = 0x017,
	TOKENKIND_CASTING = 0x021,
	TOKENKIND_NEWARR = 0x041,
	TOKENKIND_BOX = 0x081,
	TOKENKIND_CONSTRAINED = 0x107,
	TOKENKIND_NEWOBJ = 0x202,  // a constructor token: the owner class is what gets embedded
};

typedef uint32_t (*GetMethodAttribsFn)(void *, CORINFO_METHOD_HANDLE);
typedef void (*GetMethodSigFn)(void *, CORINFO_METHOD_HANDLE, CORINFO_SIG_INFO *, CORINFO_CLASS_HANDLE);
typedef CORINFO_CLASS_HANDLE (*GetMethodClassFn)(void *, CORINFO_METHOD_HANDLE);
typedef void (*GetMethodVTableOffsetFn)(void *, CORINFO_METHOD_HANDLE, uint32_t *, uint32_t *);
typedef const char *(*GetMethodNameFn)(void *, CORINFO_METHOD_HANDLE, const char **);
typedef CORINFO_METHOD_HANDLE (*FindMethodFn)(void *, CORINFO_MODULE_HANDLE, uint32_t, CORINFO_METHOD_HANDLE, int32_t);
typedef CORINFO_FIELD_HANDLE (*FindFieldFn)(void *, CORINFO_MODULE_HANDLE, uint32_t, CORINFO_METHOD_HANDLE);
typedef uint32_t (*AsCorInfoTypeFn)(void *, CORINFO_CLASS_HANDLE);
typedef const char *(*GetClassNameFn)(void *, CORINFO_CLASS_HANDLE);
typedef uint32_t (*GetClassAttribsFn)(void *, CORINFO_CLASS_HANDLE);
typedef uint32_t (*InitClassFn)(void *, CORINFO_FIELD_HANDLE, CORINFO_METHOD_HANDLE, CORINFO_METHOD_HANDLE, int32_t);
typedef CORINFO_ARG_LIST_HANDLE (*GetArgNextFn)(void *, CORINFO_ARG_LIST_HANDLE);
typedef uint32_t (*GetArgTypeFn)(void *, CORINFO_SIG_INFO *, CORINFO_ARG_LIST_HANDLE, CORINFO_CLASS_HANDLE *);
typedef uint32_t (*GetFieldOffsetFn)(void *, CORINFO_FIELD_HANDLE);
typedef uint32_t (*GetFieldTypeFn)(void *, CORINFO_FIELD_HANDLE, CORINFO_CLASS_HANDLE *, CORINFO_CLASS_HANDLE);
typedef uint32_t (*GetFieldAttribsFn)(void *, CORINFO_FIELD_HANDLE, uint32_t);
typedef void *(*GetHelperFtnFn)(void *, uint32_t, void **);
typedef void (*GetFunctionEntryPointFn)(void *, CORINFO_METHOD_HANDLE, uint32_t, void *, uint32_t);
// CORINFO_LOOKUP: the lookup kind (needsRuntimeLookup, kind), then for a constant lookup the access type and address;
// room for the runtime lookup form that shares the union.
typedef struct {
	uint32_t needs_runtime_lookup;
	uint32_t runtime_lookup_kind;
	uint32_t access;
	void *addr;
	uint32_t runtime_lookup[16];
} Lookup;
_Static_assert(__builtin_offsetof(Lookup, access) == __builtin_offsetof(GenericHandleResult, constant), "lookup layout");
typedef void (*GetFunctionFixedEntryPointInfoFn)(void *, CORINFO_MODULE_HANDLE, uint32_t, CORINFO_METHOD_HANDLE, Lookup *,
	void *);
typedef CORINFO_CLASS_HANDLE (*EmbedClassHandleFn)(void *, CORINFO_CLASS_HANDLE, void **);
typedef CORINFO_METHOD_HANDLE (*EmbedMethodHandleFn)(void *, CORINFO_METHOD_HANDLE, void **);
typedef CORINFO_FIELD_HANDLE (*EmbedFieldHandleFn)(void *, CORINFO_FIELD_HANDLE, void **);
typedef void *(*GetAddressOfPInvokeFixupFn)(void *, CORINFO_METHOD_HANDLE, void **);

// ICorDynamicInfo::getCallInfo (0x84D10830; the zapper's wrapper 0x84E2FDD0 adds the KINDONLY flag for the
// runtime and turns the result's handles into fixups): (scope, token, context, constrained token, caller,
// flags, result). The 0xE0-byte CORINFO_CALL_INFO: the method to call at 0, its class at 4, the exact owner at
// 8, the token's method at 0x44, thisTransform at 0xA8, kind at 0xAC (0 call, 1 code pointer, 3 vtable, 4
// ldvirtftn), nullInstanceCheck at 0xB0, contextHandle at 0xB4, exactContextNeedsRuntimeLookup at 0xB8 and the
// stub lookup at 0xBC (a CORINFO_LOOKUP: with kind 1 it yields the code pointer of the exact instantiation).
typedef struct {
	CORINFO_METHOD_HANDLE method;
	CORINFO_CLASS_HANDLE cls;
	CORINFO_CLASS_HANDLE owner;
	uint32_t words[39];
	uint32_t this_transform;
	uint32_t kind;
	uint32_t null_instance_check;
	void *context_handle;
	uint32_t exact_context_needs_runtime_lookup;
	GenericHandleResult stub_lookup;  // only its lookup part (36 bytes) lies inside the 0xE0
} CallInfo;
enum { CALLINFO_ALLOWINSTPARAM = 1, CALLINFO_CALLVIRT = 2, CALLINFO_KINDONLY = 4 };
typedef void (*GetCallInfoFn)(void *, CORINFO_MODULE_HANDLE, uint32_t, void *, uint32_t, CORINFO_METHOD_HANDLE,
	uint32_t, CallInfo *);
typedef uint32_t (*ConstructStringLiteralFn)(void *, CORINFO_MODULE_HANDLE, uint32_t, void **);
typedef void (*AllocMemFn)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void **, void **, void **);
typedef void (*ReserveUnwindInfoFn)(void *, int32_t, int32_t, uint32_t);
typedef void (*AllocUnwindInfoFn)(void *, uint8_t *, uint8_t *, uint32_t, uint32_t, uint32_t, uint8_t *, uint32_t);
typedef void *(*AllocGCInfoFn)(void *, uint32_t);
typedef void (*RecordRelocationFn)(void *, void *, void *, uint32_t, uint32_t, int32_t);
typedef void *(*GetPersonalityRoutineFn)(void *);
typedef void (*SetEHcountFn)(void *, uint32_t);

// CORINFO_EH_CLAUSE: offsets and lengths in IL from getEHinfo; given to setEHinfo with native start and end offsets
// (the zapper copies the six words as they are; Microsoft's images hold end offsets there).
typedef struct {
	uint32_t flags;
	uint32_t try_offset, try_length;
	uint32_t handler_offset, handler_length;
	uint32_t class_token;
} EHClause;
typedef void (*GetEHinfoFn)(void *, CORINFO_METHOD_HANDLE, uint32_t, EHClause *);
typedef void (*SetEHinfoFn)(void *, uint32_t, const EHClause *);

// CORINFO_EH_CLAUSE_FLAGS. A duplicated clause covers a funclet that lies (in IL) inside another clause's try.
enum { EH_CATCH = 0, EH_FILTER = 1, EH_FINALLY = 2, EH_FAULT = 4, EH_DUPLICATED = 8 };

// allocUnwindInfo's CorJitFuncKind.
enum { FUNC_ROOT = 0, FUNC_HANDLER = 1 };

// CorInfoFlag bits: fields and methods, and classes (getClassAttribs, observed: IDisposable 0x00200400,
// IDictionary`2 0x00220400, Int32 0x20010010, String 0x20040010).
enum {
	FLG_STATIC = 0x00000008,
	FLG_FINAL = 0x00000010,
	FLG_SYNCHRONIZED = 0x00000020,
	FLG_VIRTUAL = 0x00000040,
	FLG_PINVOKE = 0x00080000,     // CORINFO_FLG_PINVOKE
	FLG_FCALL = 0x00200000,       // observed with 0x00400000 on every internal call (getMethodAttribs)
	FLG_ABSTRACT = 0x00000400,
	FLG_SECURITYCHECK = 0x00000400,       // CORINFO_FLG_SECURITYCHECK (no method of mscorlib has it; kept for form)
	FLG_SHAREDINST = 0x00020000,          // CORINFO_FLG_SHAREDINST: code shared between instantiations (List`1::Add
	                                      // 0402000A; its exact instantiations and String::Join(string, string[])
	                                      // 04000019)
	FLG_DONT_INLINE_CALLER = 0x20000000,  // PermissionSet.Assert/Demand, CodeAccessPermission.Assert/Demand (attribs
	                                      // 0x34100051): their callers need a security object in their frame
	FLG_VALUECLASS = 0x00010000,
	FLG_INTERFACE = 0x00200000,
	FLG_DELEGATE = 0x00400000,
	FLG_CONTAINS_GC_PTR = 0x01000000,
};

// InfoAccessType.
enum { IAT_VALUE = 0, IAT_PVALUE = 1, IAT_PPVALUE = 2 };

// CorInfoHelpFunc numbers, read from xcoreclr's helper table (0x84AAE758) by what each entry does. Up to 45 it has
// Silverlight 4's layout (its dynamic-helper markers line up); from 46 it has two more entries (46 and 47 are both
// JIT_Box), so 48 is JIT_Unbox (returns obj + 4 when the type matches), 52 JIT_Stelem_Ref, 53 JIT_Ldelema_Ref
// (element address (index + 3) * 4: reference array data at +12), 54 IL_Throw and 55 IL_Rethrow (both set up a
// helper frame; only 54 takes an argument), 57 range check failure and 58 overflow. 72-308
// are the compiler's register save and restore routines, which ZapInfo::getHelperFtn (0x84E2F578) treats apart. The
// write barriers are 320 (0x84F07930, the target of Microsoft's compiled field setters) and 321 (0x84F078D0, which
// also checks that the destination is in the GC heap); both keep the ephemeral range in r14 and r15, which managed
// code therefore leaves alone.
enum {
	HELP_DBL2INT_OVF = 13,      // (f1) -> r3; out of range (or NaN) raises OverflowException (0x84CC4F00: both
	HELP_DBL2LNG_OVF = 15,      // limits compared, then fctiwz / fctidz). 4-10 are the 64-bit multiply and divide
	HELP_DBL2UINT_OVF = 17,     // helpers, 11 ULNG2DBL, 18 DBL2ULNG, 20 the float remainder.
	HELP_DBL2ULNG_OVF = 19,
	HELP_DBLREM = 21,           // (f1, f2) -> f1: fmod, NaN for a zero divisor (0x84CC5090)
	HELP_NEW = 22,
	HELP_NEW_MDARR = 27,        // JIT_NewMDArr(array type handle, constructor MethodDesc, the dimensions...):
	                            // Microsoft's SZArrayHelper_HACK (mscorlib.ni.dll 0x309514) builds its T[2,2]
	                            // with it; 28 is its sibling
	HELP_NEWARR_1 = 29,         // JIT_NewArr1(array class, length)
	HELP_INITCLASS = 34,        // JIT_InitClass(class)
	HELP_ISINSTANCEOFANY = 39,  // (class, object) -> object or null
	HELP_CHKCASTANY = 43,       // (class, object) -> object, or throws
	HELP_BOX = 46,              // (class, pointer to the value) -> object
	HELP_UNBOX = 48,            // (class, object) -> pointer to the value
	HELP_UNBOX_NULLABLE = 49,   // (destination, Nullable<T> class, object or null) fills the Nullable<T>
	HELP_STELEM_REF = 52,       // (array, index, object)
	HELP_LDELEMA_REF = 53,      // (array, index, element class) -> the element's address; the array's element
	                            // class (at array + 8) must be that class exactly (0x84CCBC58)
	HELP_THROW = 54,            // IL_Throw(object): a null object throws NullReferenceException
	HELP_RETHROW = 55,          // IL_Rethrow()
	HELP_RNGCHKFAIL = 57,
	HELP_OVERFLOW = 58,
	HELP_ASSIGN_REF = 320,
	HELP_CHECKED_ASSIGN_REF = 321,
	HELP_ASSIGN_REF_R5 = 325,   // 320 with the value in r5 .. r12: stw rN, 0(r3)
	HELP_ASSIGN_REF_R17 = 333,  // and in r17 .. r31 (333 .. 347)
	HELP_GETGENERICS_GCSTATIC_BASE = 362,     // (exact MethodTable) -> a generic instantiation's GC statics
	HELP_GETGENERICS_NONGCSTATIC_BASE = 363,  // (exact MethodTable) -> its non-GC statics (0x84CC7F80, 0x84CC83D0)
	HELP_GETSHARED_GCSTATIC_BASE = 364,     // (module ID, class ID) -> the class's GC statics; dynamic helpers
	HELP_GETSHARED_NONGCSTATIC_BASE = 365,  // (module ID, class ID) -> its DomainLocalModule
	HELP_GETSHARED_GCSTATIC_BASE_NOCTOR = 366,     // the same for a class whose constructor needn't run (observed
	HELP_GETSHARED_GCTHREADSTATIC_BASE = 373,     // thread statics ([ThreadStatic] fields): the same (module ID, class
	HELP_GETSHARED_NONGCTHREADSTATIC_BASE = 374,  // ID) arguments, the thread's block instead of the domain's
	                                              // (0x84CC8DD0, 0x84CC8CC0 from the helper table at 0x84AAE758),
	HELP_GETSHARED_GCTHREADSTATIC_BASE_NOCTOR = 375,     // for a class with a class constructor (Silverdash360's
	HELP_GETSHARED_NONGCTHREADSTATIC_BASE_NOCTOR = 376,  // Log) and without one (Thread): the same routines
	HELP_GETSHARED_NONGCSTATIC_BASE_NOCTOR = 367,  // for classes without one); the same arguments
	HELP_GETSHARED_GCSTATIC_BASE_DYNAMICCLASS = 368,     // (module ID, dynamic class ID) -> a generic instantiation's
	HELP_GETSHARED_NONGCSTATIC_BASE_DYNAMICCLASS = 369,  // statics (0x84CC7B90, 0x84CC7360); getClassDomainID gives
	                                                     // the dynamic ID
	HELP_GETGENERICS_GCTHREADSTATIC_BASE = 371,     // (exact MethodTable) -> its thread statics
	HELP_GETGENERICS_NONGCTHREADSTATIC_BASE = 372,
	HELP_GETSHARED_GCTHREADSTATIC_BASE_DYNAMICCLASS = 377,     // (module ID, dynamic class ID)
	HELP_GETSHARED_NONGCTHREADSTATIC_BASE_DYNAMICCLASS = 378,
	HELP_GET_RUNTIME_TYPE = 400, // JIT_GetRuntimeType(class handle) -> the RuntimeType object (0x84CCF178)
	HELP_FIELDDESC_TO_STUBRUNTIMEFIELD = 402, // (field handle) -> the RuntimeFieldHandle's stub object (0x84CCE450;
	                                          // 401 is the method one, 0x84CCEAA8): CLR 4's numbering around 400
	HELP_VIRTUAL_FUNC_PTR = 403, // (object, class, method) -> code address; Microsoft's interface calls use it
};

// Relocation kinds xcoreclr's ZapInfo::recordRelocation (0x84E30428) and image writer (0x84E29880) take.
enum {
	RELOC_PTR32 = 3,      // a 32-bit word
	RELOC_BRANCH24 = 6,   // b/bl displacement; the writer adds a jump stub when out of range
	RELOC_HA16 = 0x10,    // lis immediate: (target + 0x8000) >> 16
	RELOC_LO16 = 0x11,    // the paired instruction's 16-bit immediate
};

// ---------------------------------------------------------------------------------------------------------------
// Compiler state.

enum Kind { K_NONE, K_I4, K_I8, K_R8, K_REF, K_BYREF, K_STRUCT };

// The words of a value type holding references, a bit per word, for its first REF_WORDS words (1 KB). The runtime
// orders a struct's fields by size, so references needn't come first: NGenBench's 328-byte Huge has its string
// after forty doubles, at word 80. A value type with references beyond those words is declined.
enum { REF_WORDS = 256 };
typedef struct {
	uint64_t w[REF_WORDS / 64];
} RefMask;

static const RefMask NO_REFS;

static inline bool mask_bit(const RefMask *m, uint32_t i)
{
	return i < REF_WORDS && (m->w[i / 64] >> (i % 64) & 1);
}

static inline void mask_set(RefMask *m, uint32_t i)
{
	m->w[i / 64] |= (uint64_t)1 << (i % 64);
}

static inline bool mask_any(const RefMask *m)
{
	for (uint32_t i = 0; i < REF_WORDS / 64; i++)
		if (m->w[i])
			return true;
	return false;
}

static inline void mask_or(RefMask *to, const RefMask *from)
{
	for (uint32_t i = 0; i < REF_WORDS / 64; i++)
		to->w[i] |= from->w[i];
}

// The mask of word 0 alone: a TypedReference's interior pointer.
static inline RefMask first_word(void)
{
	RefMask m = NO_REFS;
	mask_set(&m, 0);
	return m;
}

// A value type stack entry holding references at a safe point.
typedef struct {
	uint32_t entry;
	RefMask refs, irefs;
} StackRefs;

typedef struct {
	uint8_t kind;    // Kind
	uint8_t type;    // CorInfoType of the variable
	int16_t offset;  // from the frame register
	uint16_t size;   // a value type's size
	CORINFO_CLASS_HANDLE cls;  // and its class
	RefMask refs;                 // the words holding references (bit i: word i), for a value type
	RefMask irefs;                // and those holding interior pointers (TypedReference's first)
	int8_t reg;                   // the register the variable lives in (0: its frame slot): a leaf method's
	                              // (leaf_registers) or a callee-saved one (choose_saved_registers)
	int8_t freg;                  // a floating-point variable's callee-saved register, f31 down to f14 (0: none)
} Var;

typedef struct {
	uint32_t at;       // code word index
	void *target;      // or, for a local relocation, the target's code word index or constant's index
	uint16_t type;
	uint8_t local;     // the target is in this method's own code (RELOC_CODE) or read-only data (RELOC_CONSTANT)
} Reloc;

enum { RELOC_CODE = 1, RELOC_CONSTANT = 2 };

// What a branch goes to: an IL offset, a leave thunk or a clause's funclet.
enum { TO_IL, TO_THUNK, TO_FUNCLET };

typedef struct {
	uint32_t at;       // code word index of the branch
	uint32_t target;   // IL offset, thunk or clause index
	uint8_t to;
} Fixup;

typedef struct {
	uint32_t flags;                        // EH_CATCH, EH_FINALLY or EH_FAULT
	uint32_t try_start, try_end;           // IL
	uint32_t handler_start, handler_end;
	uint32_t class_token;
	int32_t native_try_start, native_try_end;  // code word indices
	int32_t funclet_start, funclet_end;
} Clause;

// A leave out of a finally's try to `target`: calls the finally, then goes on to the target or to the next
// enclosing finally's thunk. Placed right after the try's code.
typedef struct {
	uint32_t clause;
	uint32_t target;  // IL offset
	int32_t at;       // code word index, once placed
} Thunk;

enum {
	// Sized for the longest methods of the Vimeo package (BouncyCastle's RipeMD320Digest.ProcessBlock: 8,405 bytes
	// of IL; Cast5Engine.SetKey: over 1,024 calls; System.Xml's XsdBuilder..cctor: over 2,048 relocations). The
	// tables live in static storage that is not cleared per method (see Ctx), which must end below the runtime's
	// code cache module at 0x8D000000 (xclrjit.ld checks).
	MAX_CODE = 0x10000,
	MAX_RELOCS = 0x4000,
	MAX_FIXUPS = 0x4000,
	MAX_SAFE_POINTS = 0x2000,
	MAX_IL = 0x10000,
	MAX_TARGETS = 0x2000,  // distinct branch targets
	MAX_STACK_REFS = 0x2000, // value type stack entries holding references, summed over the safe points
	MAX_SCALAR_REFS = 0x10000, // reference and interior pointer entries, summed over the safe points
	MAX_VARS = 512,
	MAX_STACK = 128,       // FrameworkCallbacks.Initialize passes 114 delegates to one native call
	TARGET_ENTRIES = 8,  // evaluation stack entries remembered at a branch target
	MAX_SLOTS = 1024,
	MAX_CLAUSES = 32,
	MAX_THUNKS = 64,
	MAX_CONSTANTS = 128,   // floating-point constants per method, in its read-only data (load_fp_constant)
	STRUCT_SLOT = 136,     // the largest value type handled (System.Windows's Matrix3D is 0x88 bytes), and each
	                       // stack entry's room for one
	LARGE_STRUCT = 0x1000, // the largest value type handled at all (a stack entry's room grows to it: c->struct_slot)
	ARG_SLOTS = 8,         // r3-r10
	STACK_ARG_SLOTS = 8,   // arguments beyond them, in 8-byte slots from the caller's SP + 0x50 (NGenBench's stack
	                       // probe: the 9th int argument is read at +0x54, the 10th at +0x5C): the outgoing area
	                       // every frame reserves by default; a method whose call needs more is compiled again
	                       // with the area its calls need (c->outgoing), up to MAX_STACK_ARG_SLOTS
	MAX_STACK_ARG_SLOTS = 120,
	LINKAGE_BYTES = 0x50,
	OUTGOING_BYTES = LINKAGE_BYTES + 8 * STACK_ARG_SLOTS,
	FUNCLET_FRAME = 0x60,
	EH_PROLOG_WORDS = 9,
	FUNCLET_PROLOG_WORDS = 7,
	MAX_LOOPS = 64,        // backward branches weighed (choose_saved_registers)
	SAVED_GPRS = 14,       // r17-r30 for variables: r14-r16 are xcoreclr's, r31 the frame register of a framed method
	SAVED_FPRS = 18,       // f14-f31 for floating-point variables
};

typedef struct {
	ICorJitInfo *info;
	CORINFO_METHOD_INFO *method;
	const uint8_t *il;
	uint32_t il_size;

	uint32_t n;
	uint32_t relocs_count;
	uint32_t fixups_count;
	uint32_t safe_points_count;

	// Exception handling: the clauses, the leave thunks, where the main body ends, and which clause's funclet is
	// being compiled (+1; 0 in the main body). fp is the frame register: r1, or r31 with exception handlers.
	Clause clauses[MAX_CLAUSES];
	uint32_t clauses_count;
	Thunk thunks[MAX_THUNKS];
	uint32_t thunks_count;
	uint32_t main_end;
	uint32_t funclet;
	int fp;
	bool eh;
	bool framed;    // r31 is the frame register (exception handlers, or localloc moving the stack pointer)
	bool localloc;  // the IL has a localloc
	bool security;  // the IL calls a method that needs its caller to have a security object
	uint8_t fence;  // a volatile. prefix: counts down to the lwsync after the access
	bool readonly;  // a readonly. prefix: the ldelema that follows needs no exact-type check


	uint32_t args_count;
	uint32_t locals_count;
	uint8_t stack[MAX_STACK];            // kinds
	CORINFO_CLASS_HANDLE stack_cls[MAX_STACK];  // a value type entry's class and size; for a primitive entry, the
	                                            // value type the runtime reported as it (primitive_value_type), or 0
	uint16_t stack_size[MAX_STACK];
	RefMask stack_refs[MAX_STACK];   // a value type entry's words holding references
	RefMask stack_irefs[MAX_STACK];  // and interior pointers
	RefMask temp_irefs;
	RefMask temp_refs;    // a value type being constructed in the slot above the stack's top (newobj): its references
	uint32_t temp_index;
	uint32_t depth;
	uint32_t max_stack;
	bool cached[MAX_STACK];  // the entry's value is in its cache register, not its slot (cache_register)
	int8_t alias[MAX_STACK]; // or, cached, in this variable's callee-saved register (load_var): no copy made
	int8_t fuse_reg;         // the next instruction stores the result to this register's variable: computed there
	uint8_t fuse_kind;       // (result_register, store_push), of this kind
	uint32_t insn_depth;     // the evaluation stack's depth when the instruction being compiled began
	bool stale[MAX_STACK];   // the entry's slot was never written (an argument a call took from its register)
	bool caching;            // the instruction being compiled may leave its result in a register
	bool throwing;           // a call that doesn't return: entries left in registers are dead there

	int32_t stack_ng;     // non-GC stack slots: 8 bytes each
	int32_t stack_ref;    // object reference stack slots: 4 bytes each
	int32_t stack_byref;  // interior pointer stack slots: 4 bytes each
	int32_t stack_struct; // value type stack slots: struct_slot bytes each
	bool long_branches;   // conditional branches to IL targets are made of an opposite test and a b (retry)
	bool keep_secret;     // the prolog keeps r11, an IL stub's secret parameter (retry when GetStubContext is met)
	bool calli;           // the IL has a calli (scan_targets)
	bool managed_calli;   // a delegate stub, whose calli calls managed code (IL_STUB_ShuffleThunk, IL_STUB_MulticastInvoke)
	bool leaf;            // compiled without a frame (see xngen_compile); anything that needs one fails it
	// Variables in callee-saved registers (choose_saved_registers): each variable's uses, loops weighing more,
	// and whether its address is taken (then it stays in the frame); the loops are the backward branches' ranges.
	uint16_t uses[2][MAX_VARS];     // [0] arguments, [1] locals
	uint8_t taken[2][MAX_VARS];
	struct { uint32_t start, end; } loops[MAX_LOOPS];
	uint32_t loops_count;
	uint32_t saved_count; // r30 down to r(31 - saved_count) hold variables, saved by the prolog
	uint32_t saved_fcount; // and f31 down to f(32 - saved_fcount), saved below them
	uint32_t epilog_at;   // the main body's first epilog's code word index, plus one (0: none yet)
	uint32_t deepest;     // the deepest the evaluation stack went
	uint32_t leaf_depth;  // a leaf compile: the deepest the framed compile's stack went (the IL's max stack is
	                      // often just the C# compiler's default of 8)
	int32_t struct_slot;  // a value type stack entry's room: STRUCT_SLOT, or the largest value type the method uses
	uint32_t need_struct_slot; // a value type larger than struct_slot was met: compile again with this room
	int32_t retbuf;       // the caller's return buffer (an interior pointer slot), or 0
	int32_t inst_arg;     // shared generic code's hidden instantiation argument (PARAMTYPE), or 0
	bool has_inst_arg;
	int32_t inst_temp;    // 4 bytes: a callee's instantiation argument while the other arguments are loaded
	int32_t secret;       // a runtime IL stub's secret parameter: r11 at entry (the precode that led here)
	int32_t scratch;      // 8-byte scratch slot
	int32_t newobj_temp;  // GC slot holding the object newobj allocated
	int32_t secobj;       // GC slot: the frame's security object (0: none), for methods with FLG_SECURITYCHECK
	int32_t gc_start, gc_end, interior_start, interior_end;
	int32_t frame;
	int32_t outgoing;         // the outgoing argument area's bytes (LINKAGE_BYTES + 8 per stack argument slot)
	uint32_t need_stack_slots; // stack argument slots a call needed beyond the area: compile again with them

	uint32_t return_type;
	CORINFO_CLASS_HANDLE return_cls;
	uint32_t return_size;  // a value type result's size
	const char *failure;
	CORINFO_CLASS_HANDLE constrained;  // a `constrained.` prefix's type, for the callvirt that follows
	uint32_t fail_pc;      // the IL offset being compiled when the method was declined, plus one, for the log
	uint64_t constants[MAX_CONSTANTS]; // the floating-point constants' bits, for the read-only data
	uint32_t constants_count;
	bool dump;             // XNGEN_DUMP_STUBS: this method's call sites are logged

	// The tables. Everything above is cleared for each method (and each retry); these are not, being large: each is
	// filled before it is read (counts above say how much is valid; compile_body sets labels and targets).
	uint32_t code[MAX_CODE];
	Reloc relocs[MAX_RELOCS];
	Fixup fixups[MAX_FIXUPS];
	uint32_t safe_points[MAX_SAFE_POINTS];
	uint8_t safe_point_live[MAX_SAFE_POINTS];  // whether the frame's slots are reported there (not in funclets)
	// Per safe point, the value type entries whose words hold references there: stack_refs_at[s] .. [s + 1] index
	// stack_ref_entries. Those words of the entries' slots are live at that safe point and no other.
	uint32_t stack_refs_at[MAX_SAFE_POINTS + 1];
	StackRefs stack_ref_entries[MAX_STACK_REFS];
	uint32_t stack_ref_count;
	// Likewise the reference and interior pointer entries in their slots at each safe point (entry * 2 + interior):
	// the evaluation stack's scalar GC slots are reported live only there, so the prolog needn't clear them.
	uint32_t scalar_refs_at[MAX_SAFE_POINTS + 1];
	uint8_t scalar_ref_entries[MAX_SCALAR_REFS];
	uint32_t scalar_ref_count;
	int32_t labels[MAX_IL];              // code word index of each IL offset, or -1
	uint16_t branch_target[MAX_IL];       // a branch target's index + 1 (0: not a target)
	uint8_t instruction_start[MAX_IL];    // an instruction begins here (scan_targets)
	uint32_t targets_count;
	uint8_t target_depth[MAX_TARGETS];    // the stack depth recorded at a branch target, +1 (0: none yet)
	uint8_t target_kinds[MAX_TARGETS][TARGET_ENTRIES];  // first entries' kinds at a target (checked when known)
	CORINFO_CLASS_HANDLE target_cls[MAX_TARGETS][TARGET_ENTRIES];
	uint16_t target_size[MAX_TARGETS][TARGET_ENTRIES];
	Var args[MAX_VARS];
	Var locals[MAX_VARS];
} Ctx;

// The part of Ctx cleared for each method.
#define CTX_CLEARED __builtin_offsetof(Ctx, code)

static uint32_t compiled, declined;
static Ctx ctx_storage;

// Declines with a reason: the last one set. Keeping the first instead (`if (!c->failure)`) names the root cause better,
// but that build makes the zapper crash on PlexNet (always at PlexContainer::.ctor, inside getClassAttribs, with the
// same sequence of runtime calls as this one): open question in docs/RESEARCH.md.
#define FAIL(c, why) do { (c)->failure = (why); return false; } while (0)
// For a site that fails because a callee did: keeps the callee's reason.
#define FAIL_UNLESS_SET(c, why) do { if (!(c)->failure) (c)->failure = (why); return false; } while (0)

// A decline naming the type: "local type 0x8 Foo" (the CorInfoType and, for a class or value type, its name).
static const char *fail_type(Ctx *c, const char *what, uint32_t type, CORINFO_CLASS_HANDLE cls);

static const char FAIL_NOT_LEAF[] = "not a leaf";
#ifdef XNGEN_LEAF_STATS // diagnosis: why call-free methods keep their frame, one log line each (xngen_compile)
#define LEAF_WHY(why) (why)
#else
#define LEAF_WHY(why) FAIL_NOT_LEAF
#endif

static void emit(Ctx *c, uint32_t word)
{
	// A leaf method has no frame: any load, store or address relative to r1 means it needs one.
	if (c->leaf) {
		const uint32_t op = word >> 26, ra = word >> 16 & 31;
		if (ra == 1 && (op == 14 || (op >= 32 && op <= 55) || op == 58 || op == 62))
			c->failure = LEAF_WHY("memory at r1");
	}
	if (c->n < MAX_CODE)
		c->code[c->n] = word;
	c->n++;
}

static void reloc(Ctx *c, uint32_t at, void *target, uint16_t type)
{
	if (c->relocs_count < MAX_RELOCS)
		c->relocs[c->relocs_count++] = (Reloc){at, target, type, 0};
	else
		c->failure = "too many relocations";
}

static uint32_t il_u8(Ctx *c, uint32_t at) { return c->il[at]; }
static uint32_t il_u16(Ctx *c, uint32_t at) { return c->il[at] | c->il[at + 1] << 8; }
static uint32_t il_u32(Ctx *c, uint32_t at)
{
	return c->il[at] | c->il[at + 1] << 8 | c->il[at + 2] << 16 | (uint32_t)c->il[at + 3] << 24;
}

// ---------------------------------------------------------------------------------------------------------------
// Types.

static const char *fail_type(Ctx *c, const char *what, uint32_t type, CORINFO_CLASS_HANDLE cls)
{
	static char text[160];
	const char *name = cls ? JI_CALL(c->info, VB_CLASS, CI_getClassName, GetClassNameFn, cls) : 0;
	size_t n = 0;
	for (; what[n] && n < 40; n++)
		text[n] = what[n];
	text[n++] = ' ';
	text[n++] = '0'; text[n++] = 'x';
	for (int shift = 28; shift >= 0; shift -= 4)
		if (type >> shift & 15 || shift == 0 || text[n - 1] != 'x')
			text[n++] = "0123456789abcdef"[type >> shift & 15];
	if (name) {
		text[n++] = ' ';
		for (size_t i = 0; name[i] && n < sizeof text - 1; i++)
			text[n++] = name[i];
	}
	text[n] = 0;
	return text;
}

static enum Kind kind_of(uint32_t type)
{
	switch (type) {
	case CORINFO_TYPE_BOOL:
	case CORINFO_TYPE_CHAR:
	case CORINFO_TYPE_BYTE:
	case CORINFO_TYPE_UBYTE:
	case CORINFO_TYPE_SHORT:
	case CORINFO_TYPE_USHORT:
	case CORINFO_TYPE_INT:
	case CORINFO_TYPE_UINT:
	case CORINFO_TYPE_NATIVEINT:
	case CORINFO_TYPE_NATIVEUINT:
	case CORINFO_TYPE_PTR:
		return K_I4;
	case CORINFO_TYPE_LONG:
	case CORINFO_TYPE_ULONG:
		return K_I8;
	case CORINFO_TYPE_FLOAT:
	case CORINFO_TYPE_DOUBLE:
		return K_R8;
	case CORINFO_TYPE_STRING:
	case CORINFO_TYPE_CLASS:
		return K_REF;
	case CORINFO_TYPE_BYREF:
		return K_BYREF;
	default:
		return K_NONE;
	}
}

// Loads a variable of the given type from d(base) into r (or fr for floating point), widened to its stack kind.
static void load_typed_to(Ctx *c, uint32_t type, int r, int fr, int d, int base)
{
	switch (type) {
	case CORINFO_TYPE_BOOL:
	case CORINFO_TYPE_UBYTE:
		emit(c, ppc_lbz(r, d, base));
		break;
	case CORINFO_TYPE_BYTE:
		emit(c, ppc_lbz(r, d, base));
		emit(c, ppc_extsb(r, r));
		break;
	case CORINFO_TYPE_CHAR:
	case CORINFO_TYPE_USHORT:
		emit(c, ppc_lhz(r, d, base));
		break;
	case CORINFO_TYPE_SHORT:
		emit(c, ppc_lha(r, d, base));
		break;
	case CORINFO_TYPE_LONG:
	case CORINFO_TYPE_ULONG:
		emit(c, ppc_ld(r, d, base));
		break;
	case CORINFO_TYPE_FLOAT:
		emit(c, ppc_lfs(fr, d, base));
		break;
	case CORINFO_TYPE_DOUBLE:
		emit(c, ppc_lfd(fr, d, base));
		break;
	default:
		emit(c, ppc_lwz(r, d, base));
		break;
	}
}

static void load_typed(Ctx *c, uint32_t type, int r, int d, int base)
{
	load_typed_to(c, type, r, 0, d, base);
}

// Stores r (or fr) to a variable of the given type at d(base), narrowing as the type needs (stfs rounds a double
// to single precision itself).
static void store_typed_from(Ctx *c, uint32_t type, int r, int fr, int d, int base)
{
	switch (type) {
	case CORINFO_TYPE_BOOL:
	case CORINFO_TYPE_BYTE:
	case CORINFO_TYPE_UBYTE:
		emit(c, ppc_stb(r, d, base));
		break;
	case CORINFO_TYPE_CHAR:
	case CORINFO_TYPE_SHORT:
	case CORINFO_TYPE_USHORT:
		emit(c, ppc_sth(r, d, base));
		break;
	case CORINFO_TYPE_LONG:
	case CORINFO_TYPE_ULONG:
		emit(c, ppc_std(r, d, base));
		break;
	case CORINFO_TYPE_FLOAT:
		emit(c, ppc_stfs(fr, d, base));
		break;
	case CORINFO_TYPE_DOUBLE:
		emit(c, ppc_stfd(fr, d, base));
		break;
	default:
		emit(c, ppc_stw(r, d, base));
		break;
	}
}

static void store_typed(Ctx *c, uint32_t type, int r, int d, int base)
{
	store_typed_from(c, type, r, 0, d, base);
}

// ---------------------------------------------------------------------------------------------------------------
// The evaluation stack: entry i of kind k lives in the slot for (i, k).

static int32_t slot_of(Ctx *c, uint32_t index, enum Kind kind)
{
	switch (kind) {
	case K_STRUCT:
		return c->stack_struct + c->struct_slot * (int32_t)index;
	case K_REF:
		return c->stack_ref + 4 * (int32_t)index;
	case K_BYREF:
		return c->stack_byref + 4 * (int32_t)index;
	default:
		return c->stack_ng + 8 * (int32_t)index;
	}
}

static bool push(Ctx *c, enum Kind kind);
static bool call_helper(Ctx *c, uint32_t helper);
static bool write_barrier(Ctx *c, bool in_heap, int value);

// A value type's GC layout: a byte per word, 1 a reference, 2 an interior pointer (getClassGClayout).
static const uint8_t *struct_layout(Ctx *c, CORINFO_CLASS_HANDLE cls)
{
	static uint8_t kinds[LARGE_STRUCT / 4];
	memset(kinds, 0, sizeof kinds);
	if (JI_CALL(c->info, VB_CLASS, CI_getClassAttribs, GetClassAttribsFn, cls) & FLG_CONTAINS_GC_PTR)
		JI_CALL(c->info, VB_CLASS, CI_getClassGClayout, GetClassGClayoutFn, cls, kinds);
	return kinds;
}

// A value type this compiler handles: at most LARGE_STRUCT bytes, its references (if any) in its first REF_WORDS
// words, holding no interior pointers. References inside are fine: struct_refs says which words hold them, and those
// words are reported to the GC wherever a copy lives in the frame. Each evaluation stack entry has room for
// STRUCT_SLOT bytes; a larger value type makes the method compile again with the room it needs (c->struct_slot). A
// size that isn't a whole number of words (Char, Boolean, Int16 and Nullable<T> of them as structs: boxed, or the
// this of a constrained call; a struct of three shorts) is copied to the byte: the copies below write no more than
// the size into memory that isn't a frame slot, where the next field may begin.
static const char FAIL_STRUCT_SLOT[] = "value type larger than a stack entry";

// Whether a stack entry has room for a value type of `size` bytes; if not, the method is compiled again with room
// for it (at least STRUCT_SLOT bytes; a method that uses no value types on its stack reserves none).
static bool struct_room(Ctx *c, uint32_t size)
{
	if (size <= (uint32_t)c->struct_slot)
		return true;
	uint32_t room = (size + 7) & ~7u;
	if (room < STRUCT_SLOT)
		room = STRUCT_SLOT;
	if (room > c->need_struct_slot)
		c->need_struct_slot = room;
	FAIL(c, FAIL_STRUCT_SLOT);
}

static bool value_type(Ctx *c, CORINFO_CLASS_HANDLE cls, uint32_t *size)
{
	if (!cls)
		FAIL(c, "value type");
	*size = JI_CALL(c->info, VB_CLASS, CI_getClassSize, GetClassSizeFn, cls);
	if (!*size || *size > LARGE_STRUCT)
		FAIL(c, fail_type(c, "value type size", *size, cls));
	if (!struct_room(c, *size))
		return false;
	const uint32_t attribs = JI_CALL(c->info, VB_CLASS, CI_getClassAttribs, GetClassAttribsFn, cls);
	if (attribs & FLG_CONTAINS_GC_PTR) {
#ifdef XNGEN_NO_STRUCT_REFS
		FAIL(c, "value type with references (experiment)");
#endif
		const uint8_t *kinds = struct_layout(c, cls);
		for (uint32_t i = 0; i < *size / 4; i++) {
			if (kinds[i] == 2)
				FAIL(c, fail_type(c, "value type with interior pointers", CORINFO_TYPE_VALUECLASS, cls));
			if (kinds[i] && i >= REF_WORDS)
				FAIL(c, fail_type(c, "value type with references past its first words", CORINFO_TYPE_VALUECLASS, cls));
		}
	}
	return true;
}

// The words of a value type (one value_type accepted) holding object references (kind 1) or interior pointers
// (kind 2), as a bit mask.
static RefMask struct_words(Ctx *c, CORINFO_CLASS_HANDLE cls, uint32_t size, uint8_t kind)
{
	if (!cls || !(JI_CALL(c->info, VB_CLASS, CI_getClassAttribs, GetClassAttribsFn, cls) & FLG_CONTAINS_GC_PTR))
		return NO_REFS;
	const uint8_t *kinds = struct_layout(c, cls);
	RefMask refs = NO_REFS;
	for (uint32_t i = 0; i < size / 4 && i < REF_WORDS; i++)
		if (kinds[i] == kind)
			mask_set(&refs, i);
	return refs;
}

static RefMask struct_refs(Ctx *c, CORINFO_CLASS_HANDLE cls, uint32_t size)
{
	return struct_words(c, cls, size, 1);
}

// The words of a value type holding interior pointers (getClassGClayout kind 2), reported as such.
static RefMask struct_irefs(Ctx *c, CORINFO_CLASS_HANDLE cls, uint32_t size)
{
	return struct_words(c, cls, size, 2);
}

static bool push(Ctx *c, enum Kind kind)
{
	if (c->depth >= c->max_stack || kind == K_NONE)
		FAIL(c, "stack");
	c->cached[c->depth] = false;
	c->alias[c->depth] = 0;
	c->stale[c->depth] = false;
	c->stack_cls[c->depth] = 0;
	c->stack[c->depth++] = kind;
	if (c->depth > c->deepest)
		c->deepest = c->depth;
	return true;
}

// Evaluation stack entries in registers. Within straight-line code an entry's value can stay in a register instead
// of its frame slot: entry i of an integer kind in r3 + i, of floating point in f1 + i, for the first few entries.
// These are the argument registers a call takes its arguments from when they begin at the stack's bottom, and the
// ones a call's result and a return value come back in, so neither needs a move then. A leaf method's entries are
// in r5 + i and f2 + i instead: its variables have r3 and r4 and the registers above its entries (leaf_registers).
// Only the instructions simple_instruction accepts leave entries there (they call nothing and use only r0, r11,
// r12, f0 and f12 besides, and stfld's write barrier r3 and r4, which it frees first); every other instruction,
// every branch and every join writes them back first (flush), so the frame slots are exact wherever the GC, a
// handler or another path looks at them.
enum { CACHE_GPRS = 8, CACHE_FPRS = 10, LEAF_CACHE_GPRS = 6 };

static int cache_register(const Ctx *c, uint32_t index, enum Kind kind)
{
	if (kind == K_STRUCT) // a leaf's 4- or 8-byte value types are in its integer registers (small_struct)
		return c->leaf && index < LEAF_CACHE_GPRS ? 5 + (int)index : -1;
	if (kind == K_NONE)
		return -1;
	const int base = kind == K_R8 ? (c->leaf ? 2 : 1) : (c->leaf ? 5 : 3);
	const uint32_t count = kind == K_R8 ? CACHE_FPRS : c->leaf ? LEAF_CACHE_GPRS : CACHE_GPRS;
	return index < count ? base + (int)index : -1;
}

// Whether an entry below `upto` names register reg (an alias of its variable): f-register reg if fp, else r-register.
static bool names_register(const Ctx *c, int reg, uint32_t upto, bool fp)
{
	for (uint32_t i = 0; i < upto; i++)
		if (c->cached[i] && c->alias[i] == reg && (c->stack[i] == K_R8) == fp)
			return true;
	return false;
}

// The register holding cached entry `index`: the variable's it was loaded from, or its own cache register.
static int entry_register(const Ctx *c, uint32_t index)
{
	return c->alias[index] ? c->alias[index] : cache_register(c, index, (enum Kind)c->stack[index]);
}

static void store_entry_slot(Ctx *c, uint32_t index, int r, int fr)
{
	const enum Kind kind = (enum Kind)c->stack[index];
	const int32_t at = slot_of(c, index, kind);
	emit(c, kind == K_I8 ? ppc_std(r, at, c->fp) : kind == K_R8 ? ppc_stfd(fr, at, c->fp) : ppc_stw(r, at, c->fp));
}

static void flush(Ctx *c)
{
	if (c->leaf) // its entries live in their registers throughout
		return;
	for (uint32_t i = 0; i < c->depth; i++)
		if (c->cached[i]) {
			const int reg = entry_register(c, i);
			store_entry_slot(c, i, reg, reg);
			c->cached[i] = false;
			c->alias[i] = 0;
		}
}

// After the stack was set to a branch target's: everything is in the slots.
static void uncache(Ctx *c)
{
	memset(c->cached, 0, sizeof c->cached);
	memset(c->alias, 0, sizeof c->alias);
}

// A value type a leaf method keeps in a register, its bytes read as a big-endian integer (as lwz and ld load it,
// and as it is passed and returned): one of 4 or 8 bytes.
static bool small_struct(uint32_t size)
{
	return size == 4 || size == 8;
}

// Pushes a value type entry; its slot is filled by the caller (in a leaf, its register).
static bool push_struct(Ctx *c, CORINFO_CLASS_HANDLE cls, uint32_t size)
{
	if (c->leaf && !small_struct(size))
		FAIL(c, LEAF_WHY("value type size"));
	if (!struct_room(c, size) || !push(c, K_STRUCT))
		return false;
	c->stack_cls[c->depth - 1] = cls;
	c->stack_size[c->depth - 1] = (uint16_t)size;
	c->stack_refs[c->depth - 1] = struct_refs(c, cls, size);
	c->stack_irefs[c->depth - 1] = struct_irefs(c, cls, size);
	return true;
}

// Pushes a TypedReference (CORINFO_TYPE_REFANY): 8 bytes, the interior pointer to the value then its type handle.
static bool push_refany(Ctx *c)
{
	if (!struct_room(c, 8) || !push(c, K_STRUCT))
		return false;
	c->stack_cls[c->depth - 1] = 0;
	c->stack_size[c->depth - 1] = 8;
	c->stack_refs[c->depth - 1] = NO_REFS;
	c->stack_irefs[c->depth - 1] = first_word();
	return true;
}

// The frame offset of entry `from_top`'s slot.
static int32_t entry_slot(Ctx *c, uint32_t from_top)
{
	const uint32_t index = c->depth - 1 - from_top;
	return slot_of(c, index, (enum Kind)c->stack[index]);
}

// Copies the bytes past the last whole word of a value type: a halfword, a byte, or both.
static void copy_tail(Ctx *c, int dst, int32_t dst_off, int src, int32_t src_off, uint32_t size)
{
	int32_t i = (int32_t)(size & ~3u);
	if (size & 2) {
		emit(c, ppc_lhz(0, src_off + i, src));
		emit(c, ppc_sth(0, dst_off + i, dst));
		i += 2;
	}
	if (size & 1) {
		emit(c, ppc_lbz(0, src_off + i, src));
		emit(c, ppc_stb(0, dst_off + i, dst));
	}
}

// Copies a value type a word at a time (fields may be only 4-byte aligned), then its tail bytes.
static void copy_words(Ctx *c, int dst, int32_t dst_off, int src, int32_t src_off, uint32_t size)
{
	for (uint32_t i = 0; i + 4 <= size; i += 4) {
		emit(c, ppc_lwz(0, src_off + (int32_t)i, src));
		emit(c, ppc_stw(0, dst_off + (int32_t)i, dst));
	}
	copy_tail(c, dst, dst_off, src, src_off, size);
}

// Copies a value type whose destination may be in the GC heap (an object field, a boxed static, a return buffer):
// its reference words go through the checked write barrier, which takes a frame address as well. The addresses
// are kept in the scratch slot across the helper calls.
static bool copy_struct(Ctx *c, int dst, int32_t dst_off, int src, int32_t src_off, uint32_t size, RefMask refs)
{
	if (!mask_any(&refs)) {
		copy_words(c, dst, dst_off, src, src_off, size);
		return true;
	}
	emit(c, ppc_addi(12, dst, dst_off));
	emit(c, ppc_stw(12, c->scratch, c->fp));
	emit(c, ppc_addi(12, src, src_off));
	emit(c, ppc_stw(12, c->scratch + 4, c->fp));
	for (uint32_t i = 0; i + 4 <= size; i += 4) {
		emit(c, ppc_lwz(11, c->scratch, c->fp));
		emit(c, ppc_lwz(12, c->scratch + 4, c->fp));
		if (mask_bit(&refs, i / 4)) {
			emit(c, ppc_addi(3, 11, (int32_t)i));
			emit(c, ppc_lwz(4, (int32_t)i, 12));
			if (!write_barrier(c, false, 4))
				return false;
		} else {
			emit(c, ppc_lwz(0, (int32_t)i, 12));
			emit(c, ppc_stw(0, (int32_t)i, 11));
		}
	}
	if (size & 3) {
		emit(c, ppc_lwz(11, c->scratch, c->fp));
		emit(c, ppc_lwz(12, c->scratch + 4, c->fp));
		copy_tail(c, 11, 0, 12, 0, size);
	}
	return true;
}

static void zero_words(Ctx *c, int base, int32_t off, uint32_t size)
{
	emit(c, ppc_li(0, 0));
	uint32_t i = 0;
	for (; i + 4 <= size; i += 4)
		emit(c, ppc_stw(0, off + (int32_t)i, base));
	if (size & 2) {
		emit(c, ppc_sth(0, off + (int32_t)i, base));
		i += 2;
	}
	if (size & 1)
		emit(c, ppc_stb(0, off + (int32_t)i, base));
}

// Loads entry `from_top` (0: the top) into r / f-register fr; returns its kind. A value type entry gives its slot's
// address.
static enum Kind load_entry(Ctx *c, uint32_t from_top, int r, int fr)
{
	const uint32_t index = c->depth - 1 - from_top;
	const enum Kind kind = c->stack[index];
	const int32_t at = slot_of(c, index, kind);
	if (kind == K_STRUCT && c->leaf) { // a leaf's value type is in a register: it has no address
		c->failure = LEAF_WHY("value type address");
		return kind;
	}
	if (c->cached[index]) {
		const int reg = entry_register(c, index);
		if (kind == K_R8 && fr != reg)
			emit(c, ppc_fmr(fr, reg));
		else if (kind != K_R8 && r != reg)
			emit(c, ppc_mr(r, reg));
		return kind;
	}
	switch (kind) {
	case K_STRUCT:
		emit(c, ppc_addi(r, c->fp, at));
		break;
	case K_I8:
		emit(c, ppc_ld(r, at, c->fp));
		break;
	case K_R8:
		emit(c, ppc_lfd(fr, at, c->fp));
		break;
	default:
		emit(c, ppc_lwz(r, at, c->fp));
		break;
	}
	return kind;
}

// Stores r / fr as a new entry of the given kind at the top.
static bool store_push(Ctx *c, enum Kind kind, int r, int fr)
{
	if (!push(c, kind))
		return false;
	const uint32_t index = c->depth - 1;
	const int reg = c->caching ? cache_register(c, index, kind) : -1;
	if (reg < 0) {
		store_entry_slot(c, index, r, fr);
		return true;
	}
	// The next instruction stores it to a callee-saved register's variable: it goes there now, the entry naming the
	// register, and the store finds it in place (result_register may have computed it there already).
	const int value = kind == K_R8 ? fr : r;
	if (c->fuse_reg && kind == c->fuse_kind &&
		(value == c->fuse_reg || !names_register(c, c->fuse_reg, index, kind == K_R8))) {
		if (value != c->fuse_reg)
			emit(c, kind == K_R8 ? ppc_fmr(c->fuse_reg, fr) : ppc_mr(c->fuse_reg, r));
		c->cached[index] = true;
		c->alias[index] = c->fuse_reg;
		return true;
	}
	if (kind == K_R8 && fr != reg)
		emit(c, ppc_fmr(reg, fr));
	else if (kind != K_R8 && r != reg)
		emit(c, ppc_mr(reg, r));
	c->cached[index] = true;
	return true;
}

// The register holding integer entry `from_top`: its cache register, or r after loading it there.
static int operand(Ctx *c, uint32_t from_top, int r)
{
	const uint32_t index = c->depth - 1 - from_top;
	if (c->cached[index])
		return entry_register(c, index);
	load_entry(c, from_top, r, 0);
	return r;
}

// Where a new integer entry of `kind` is best computed: its cache register when the result may stay there,
// else r (store_push then writes it to the slot).
static int result_register(Ctx *c, enum Kind kind, int r)
{
	// Straight into the variable the next instruction stores it to, unless an entry (the operands among them, which
	// the instruction may still read after writing its result) names that register.
	if (kind != K_R8 && c->caching && c->fuse_reg && kind == c->fuse_kind && cache_register(c, c->depth, kind) >= 0 &&
		!names_register(c, c->fuse_reg, c->insn_depth, false))
		return c->fuse_reg;
	const int reg = c->caching ? cache_register(c, c->depth, kind) : -1;
	return reg >= 0 && kind != K_R8 ? reg : r;
}

// The f-register holding floating-point entry `from_top`: its cache register (or its variable's), or fr after
// loading it there.
static int foperand(Ctx *c, uint32_t from_top, int fr)
{
	const uint32_t index = c->depth - 1 - from_top;
	if (c->cached[index])
		return entry_register(c, index);
	load_entry(c, from_top, 0, fr);
	return fr;
}

// Where a new floating-point entry is best computed, as result_register: the variable's register the next
// instruction stores it to, its cache register, or fr.
static int fresult_register(Ctx *c, int fr)
{
	if (c->caching && c->fuse_reg && c->fuse_kind == K_R8 && cache_register(c, c->depth, K_R8) >= 0 &&
		!names_register(c, c->fuse_reg, c->insn_depth, true))
		return c->fuse_reg;
	const int reg = c->caching ? cache_register(c, c->depth, K_R8) : -1;
	return reg >= 0 ? reg : fr;
}

static bool pop(Ctx *c, uint32_t count)
{
	if (c->depth < count)
		FAIL(c, "stack underflow");
	c->depth -= count;
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Addresses and calls through the runtime.

static void call_lookup_at(Ctx *c, uint32_t access, void *addr, bool safe);

// Loads the 32-bit word at a runtime-provided cell (a fixup or handle cell in the image) into r.
static void load_cell(Ctx *c, int r, void *cell)
{
	reloc(c, c->n, cell, RELOC_HA16);
	emit(c, ppc_lis(r, 0));
	reloc(c, c->n, cell, RELOC_LO16);
	emit(c, ppc_lwz(r, 0, r));
}

// Materializes an address the runtime gave (a handle or node in the image) in r.
static void load_address(Ctx *c, int r, void *target)
{
	reloc(c, c->n, target, RELOC_HA16);
	emit(c, ppc_lis(r, 0));
	reloc(c, c->n, target, RELOC_LO16);
	emit(c, ppc_addi(r, r, 0));
}

// Materializes the address of code word `word` of this method in r (a catch funclet's resume address).
static void load_local_address(Ctx *c, int r, uint32_t word)
{
	reloc(c, c->n, (void *)(uintptr_t)word, RELOC_HA16);
	c->relocs[c->relocs_count - 1].local = RELOC_CODE;
	emit(c, ppc_lis(r, 0));
	reloc(c, c->n, (void *)(uintptr_t)word, RELOC_LO16);
	c->relocs[c->relocs_count - 1].local = RELOC_CODE;
	emit(c, ppc_addi(r, r, 0));
}

// Loads a floating-point constant into fr from the method's read-only data (8-byte aligned there, publish), as
// Microsoft's code does (lis, lfd): two instructions and no frame slot, so a leaf method can take one. r11 is
// the address's high half.
static bool load_fp_constant(Ctx *c, int fr, uint64_t bits)
{
	uint32_t i = 0;
	while (i < c->constants_count && c->constants[i] != bits)
		i++;
	if (i == c->constants_count) {
		if (i == MAX_CONSTANTS)
			FAIL(c, "too many floating-point constants");
		c->constants[c->constants_count++] = bits;
	}
	reloc(c, c->n, (void *)(uintptr_t)i, RELOC_HA16);
	c->relocs[c->relocs_count - 1].local = RELOC_CONSTANT;
	emit(c, ppc_lis(11, 0));
	reloc(c, c->n, (void *)(uintptr_t)i, RELOC_LO16);
	c->relocs[c->relocs_count - 1].local = RELOC_CONSTANT;
	emit(c, ppc_lfd(fr, 0, 11));
	return true;
}

// Builds a 64-bit constant in r: li, lis and ori, or for one beyond 32 bits its high word shifted up and the low
// word ored in.
static void load_constant64(Ctx *c, int r, uint64_t value)
{
	const int64_t v = (int64_t)value;
	if (v >= -32768 && v <= 32767) {
		emit(c, ppc_li(r, (int32_t)v));
		return;
	}
	const uint32_t high = (uint32_t)(value >> 32), low = (uint32_t)value;
	if (v >= INT32_MIN && v <= INT32_MAX) {
		emit(c, ppc_lis(r, (int32_t)low >> 16));
		if (low & 0xFFFF)
			emit(c, ppc_ori(r, r, low & 0xFFFF));
		return;
	}
	emit(c, ppc_lis(r, (int32_t)high >> 16));
	if (high & 0xFFFF)
		emit(c, ppc_ori(r, r, high & 0xFFFF));
	emit(c, ppc_sldi32(r, r));
	if (low >> 16)
		emit(c, ppc_oris(r, r, low >> 16));
	if (low & 0xFFFF)
		emit(c, ppc_ori(r, r, low & 0xFFFF));
}

// Records a GC safe point at code word `word`. While a funclet runs, the runtime reports its parent's frame (the
// slots Microsoft's funclets use are dead at their own offsets), so a funclet's safe points report nothing.
static void safe_point_at(Ctx *c, uint32_t word, bool live)
{
	if (c->safe_points_count >= MAX_SAFE_POINTS) {
		c->failure = "too many calls";
		return;
	}
	c->stack_refs_at[c->safe_points_count] = c->stack_ref_count;
	c->scalar_refs_at[c->safe_points_count] = c->scalar_ref_count;
	// The GC sees only the frame and the variables' callee-saved registers: an entry still in its cache register here
	// would be missed (simple_instruction let an instruction through that calls). Declined rather than reported wrong.
	for (uint32_t i = 0; live && !c->throwing && i < c->depth; i++)
		if (c->cached[i] && !c->alias[i]) {
			c->failure = "register-held stack entry at a call";
			return;
		}
	// The references on the evaluation stack: those whose slots hold them (at a throw, an entry still in its
	// register is dead, its slot unwritten).
	for (uint32_t i = 0; live && i < c->depth; i++) {
		if ((c->stack[i] != K_REF && c->stack[i] != K_BYREF) || c->cached[i] || c->stale[i])
			continue;
		if (c->scalar_ref_count >= MAX_SCALAR_REFS) {
			c->failure = "too many references at calls";
			return;
		}
		c->scalar_ref_entries[c->scalar_ref_count++] = (uint8_t)(i * 2 + (c->stack[i] == K_BYREF));
	}
	if (live) {
		for (uint32_t i = 0; i <= c->depth; i++) {
			RefMask refs = NO_REFS, irefs = NO_REFS;
			if (i < c->depth && c->stack[i] == K_STRUCT)
				refs = c->stack_refs[i], irefs = c->stack_irefs[i];
			if (i == c->temp_index)
				mask_or(&refs, &c->temp_refs), mask_or(&irefs, &c->temp_irefs);
			if (!mask_any(&refs) && !mask_any(&irefs))
				continue;
			if (c->stack_ref_count >= MAX_STACK_REFS) {
				c->failure = "too many value types holding references at calls";
				return;
			}
			c->stack_ref_entries[c->stack_ref_count++] = (StackRefs){i, refs, irefs};
		}
	}
	c->safe_point_live[c->safe_points_count] = live;
	c->safe_points[c->safe_points_count++] = word * 4;
	c->stack_refs_at[c->safe_points_count] = c->stack_ref_count;
	c->scalar_refs_at[c->safe_points_count] = c->scalar_ref_count;
}

static void safe_point(Ctx *c)
{
#ifdef XNGEN_DUMP_STUBS
	if (c->dump)
		DbgPrint("xclrjit:   call returns at code %X, IL %u\n", c->n * 4, c->fail_pc ? c->fail_pc - 1 : 0);
#endif
	safe_point_at(c, c->n, c->funclet == 0);
}

// Calls a target given as an InfoAccessType and address: direct (bl, relocated), or through a cell.
static void call_lookup(Ctx *c, uint32_t access, void *addr)
{
	call_lookup_at(c, access, addr, true);
}

// A call; safe: a GC safe point at its return address (not for a write barrier, which never collects).
static void call_lookup_at(Ctx *c, uint32_t access, void *addr, bool safe)
{
	if (c->leaf) // a call needs the link register saved: a frame
		c->failure = FAIL_NOT_LEAF;
	if (access == IAT_VALUE) {
		reloc(c, c->n, addr, RELOC_BRANCH24);
		emit(c, ppc_bl(0));
	} else {
		load_cell(c, 12, addr);
		if (access == IAT_PPVALUE)
			emit(c, ppc_lwz(12, 0, 12));
		emit(c, ppc_mtctr(12));
		emit(c, PPC_BCTRL);
	}
	if (safe)
		safe_point(c);
}

// Calls a method by its entry point. A direct call (IAT_VALUE) makes the zapper bind the call to the callee's code in
// this image, and for a method of a generic instantiation it then compiles that code in a later pass; if the JIT
// declines it there, the image has nothing to bind to and the zapper fails the whole image. Asking for IAT_PVALUE
// takes the zapper's other path, a cell the runtime fills in.
static bool generic_context(const Ctx *c);
static CORINFO_CLASS_HANDLE resolve_class_any(Ctx *c, uint32_t token, uint32_t kind);
static bool load_class_of_token(Ctx *c, int r, uint32_t token, uint32_t kind, CORINFO_CLASS_HANDLE *cls_out);
static bool load_owner_of_ctor(Ctx *c, int r, uint32_t token, CORINFO_CLASS_HANDLE cls);
static bool runtime_lookup(Ctx *c, int r, const GenericHandleResult *g);
static const char *method_name(ICorJitInfo *info, CORINFO_METHOD_HANDLE ftn, const char **class_name);
static uint32_t memberref_parent(const Ctx *c, uint32_t token);

static bool call_method(Ctx *c, CORINFO_METHOD_HANDLE method, bool inst_supplied)
{
	CORINFO_SIG_INFO sig;
	memset(&sig, 0, sizeof sig);
	JI_CALL(c->info, VB_METHOD, MI_getMethodSig, GetMethodSigFn, method, &sig, 0);
	// An FCall (the runtime's own code: String.get_Length, FastAllocateString, Monitor.Enter; attribs 0x46000xx
	// against an IL method's 0x40000xx) has no native code in any image: a direct call to one in the same module
	// makes the zapper wait forever for it (compiling mscorlib hung after String::InternalSubString); Microsoft's
	// code reaches them through a cell.
	const uint32_t attribs = JI_CALL(c->info, VB_METHOD, MI_getMethodAttribs, GetMethodAttribsFn, method);
	// From shared generic code the token names a canonical method (List<__Canon>.Add): an instance method finds
	// its instantiation through this, but a generic method or a static method or constructor of a generic class
	// needs the caller's instantiation handed over through a runtime lookup, which this compiler doesn't make.
	if (generic_context(c) && !inst_supplied &&
		(sig.sigInst.methInstCount || (sig.sigInst.classInstCount && !(sig_call_conv(&sig) & CORINFO_CALLCONV_HASTHIS))))
		FAIL(c, fail_type(c, "generic callee from a generic context", sig_call_conv(&sig), 0));
	// An instance method of a generic class called from shared code is the canonical body itself (its exact
	// instantiation comes through this): the zapper gives no cell for it (getFunctionEntryPoint throws), so it is
	// called directly, as the CLR does.
	const bool shared_instance = generic_context(c) && sig.sigInst.classInstCount &&
		(sig_call_conv(&sig) & CORINFO_CALLCONV_HASTHIS) && !inst_supplied;
	const uint32_t requested = !shared_instance &&
		(sig.sigInst.classInstCount || sig.sigInst.methInstCount || (attribs & FLG_FCALL)) ? IAT_PVALUE : IAT_VALUE;
	struct { uint32_t access; void *addr; } lookup = {0, 0};
	JI_CALL(c->info, VB_DYNAMIC, DYN_getFunctionEntryPoint, GetFunctionEntryPointFn, method, requested, &lookup, 0);
	if (!lookup.addr)
		FAIL(c, "entry point");
	call_lookup(c, lookup.access, lookup.addr);
	return true;
}

// ZapInfo::getHelperFtn (0x84E2F578) answers a helper thunk node for the image, which embeds the runtime's
// address of the helper (the thunk's lis/addi carry a base relocation like every absolute word of the image, which
// is why pe2nixex.ps1 moves only words that point into the image). With the high bit set on the helper number it
// would give an import cell instead; Microsoft's code calls the thunks.
static bool call_helper_at(Ctx *c, uint32_t helper, bool safe)
{
	void *indirection = 0;
	void *direct = JI_CALL(c->info, VB_DYNAMIC, DYN_getHelperFtn, GetHelperFtnFn, helper, &indirection);
	if (indirection)
		call_lookup_at(c, IAT_PVALUE, indirection, safe);
	else if (direct)
		call_lookup_at(c, IAT_VALUE, direct, safe);
	else
		FAIL(c, "helper");
	return true;
}

static bool call_helper(Ctx *c, uint32_t helper)
{
	return call_helper_at(c, helper, true);
}

// Stores reference `value` (a register) at the address in r3 through a write barrier. The barriers use only r0, r11,
// r12, cr0 and the link register (docs/RESEARCH.md, "Variables in callee-saved registers"), so entries held in
// other registers stay there, and since they never collect there is no safe point. The unchecked barrier (the
// destination is in the heap: an object's field) has a variant for each register the value may be in; the checked
// one (any destination) takes it in r4.
static bool write_barrier(Ctx *c, bool in_heap, int value)
{
	uint32_t helper = in_heap ? HELP_ASSIGN_REF : HELP_CHECKED_ASSIGN_REF;
	if (in_heap && value >= 5 && value <= 12)
		helper = HELP_ASSIGN_REF_R5 + (uint32_t)(value - 5);
	else if (in_heap && value >= 17 && value <= 31)
		helper = HELP_ASSIGN_REF_R17 + (uint32_t)(value - 17);
	else if (value != 4)
		emit(c, ppc_mr(4, value));
	return call_helper_at(c, helper, false);
}

// Calls a helper that doesn't return (a throw). A trap follows, so the return address the runtime walks the stack
// from stays inside this method and inside the try the call is in.
static bool call_throw_helper(Ctx *c, uint32_t helper)
{
	c->throwing = true;
	const bool called = call_helper(c, helper);
	c->throwing = false;
	if (!called)
		return false;
	emit(c, PPC_TRAP);
	return true;
}

// Throws NullReferenceException (IL_Throw of null) when r is null. Methods with exception handlers check references
// before using them rather than taking the hardware fault: their GC info has no interruptible ranges, so a frame
// stopped at a faulting instruction would report none of its slots while its handlers run.
static bool null_check(Ctx *c, int r)
{
	if (!c->eh)
		return true;
	emit(c, ppc_cmpwi(6, r, 0));
	const uint32_t skip = c->n;
	emit(c, ppc_bc(BO_FALSE, 6 * 4 + CR_EQ, 0));
	emit(c, ppc_li(3, 0));
	if (!call_throw_helper(c, HELP_THROW))
		return false;
	c->code[skip] = ppc_bc(BO_FALSE, 6 * 4 + CR_EQ, (int32_t)(c->n - skip) * 4);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Frame layout, prolog and epilog.

// The value type a primitive CorInfoType stands for, or 0: the runtime reports a value type whose one field is a
// primitive as that primitive (a struct of one long is a CORINFO_TYPE_LONG argument, local and result), with the
// struct as the class handle, so that only the class tells its value from a native int pointer (ldfld). Only the
// integer and floating-point types stand for one: a pointer's or a byref's class is what it points to (a value type
// too, maybe), and a reference type's handle can be one getClassAttribs throws on (the zapper breaks then).
static CORINFO_CLASS_HANDLE primitive_value_type(Ctx *c, uint32_t type, CORINFO_CLASS_HANDLE cls)
{
	if (!cls || type < CORINFO_TYPE_BOOL || type > CORINFO_TYPE_DOUBLE)
		return 0;
	return (JI_CALL(c->info, VB_CLASS, CI_getClassAttribs, GetClassAttribsFn, cls) & FLG_VALUECLASS) ? cls : 0;
}

// A variable of CorInfoType t (cls for a value type).
// A value type too large for a stack entry (over STRUCT_SLOT bytes) can still be a local, holding no references:
// it lives in the frame and is only ever addressed (the runtime's P/Invoke stubs keep a 0x210-byte
// NDirectMethodFrameStandalone this way); loading or storing it whole declines.
static bool large_local(Ctx *c, CORINFO_CLASS_HANDLE cls, uint32_t *size)
{
	if (!cls)
		return false;
	*size = JI_CALL(c->info, VB_CLASS, CI_getClassSize, GetClassSizeFn, cls);
	if (*size <= (uint32_t)(c->struct_slot > STRUCT_SLOT ? c->struct_slot : STRUCT_SLOT) || *size > LARGE_STRUCT ||
		*size % 4)
		return false;
	return !(JI_CALL(c->info, VB_CLASS, CI_getClassAttribs, GetClassAttribsFn, cls) & FLG_CONTAINS_GC_PTR);
}

static bool make_var(Ctx *c, Var *v, uint32_t t, CORINFO_CLASS_HANDLE cls, bool local)
{
	if (t == CORINFO_TYPE_VALUECLASS) {
		uint32_t size;
		if (local && large_local(c, cls, &size)) {
			*v = (Var){K_STRUCT, (uint8_t)t, 0, (uint16_t)size, cls, NO_REFS, NO_REFS, 0, 0};
			return true;
		}
		if (!value_type(c, cls, &size))
			return false;
		*v = (Var){K_STRUCT, (uint8_t)t, 0, (uint16_t)size, cls, struct_refs(c, cls, size), struct_irefs(c, cls, size), 0, 0};
		return true;
	}
	if (t == CORINFO_TYPE_REFANY) {
		// TypedReference: an interior pointer and a type handle; the runtime's layout says IntPtr, the JIT
		// reports the first word as an interior pointer.
		*v = (Var){K_STRUCT, (uint8_t)t, 0, 8, cls, NO_REFS, first_word(), 0, 0};
		return true;
	}
	const enum Kind k = kind_of(t);
	if (k == K_NONE)
		return false;
	*v = (Var){k, (uint8_t)t, 0, 0, primitive_value_type(c, t, cls), NO_REFS, NO_REFS, 0, 0};
	return true;
}

// Frame bytes for a non-GC variable.
static int32_t var_bytes(const Var *v)
{
	return v->kind == K_STRUCT ? (int32_t)((v->size + 7) & ~7u) : 8;
}

// Argument registers a value takes: one, or a value type's 8-byte chunks.
static uint32_t struct_slots(uint32_t size)
{
	return size <= 4 ? 1 : (size + 7) / 8;
}

static uint32_t var_slots(const Var *v)
{
	return v->kind == K_STRUCT ? struct_slots(v->size) : 1;
}

// A signature's result: 0, or a value type's size (more than 8 bytes comes back through a buffer the caller passes
// first, in r3; 8 or less in r3 like an integer).
static bool return_info(Ctx *c, const CORINFO_SIG_INFO *sig, uint32_t *size)
{
	const uint32_t t = sig_ret_type(sig);
	*size = 0;
	if (t == CORINFO_TYPE_VALUECLASS)
		return value_type(c, sig->retTypeClass, size);
	if (t == CORINFO_TYPE_REFANY) { // a TypedReference: 8 bytes, in r3 like any 8-byte value type
		*size = 8;
		return true;
	}
	if (t != CORINFO_TYPE_VOID && kind_of(t) == K_NONE)
		FAIL(c, fail_type(c, "result type", t, sig->retTypeClass));
	return true;
}

// A leaf method's variables in registers: r3 and r4 (the first two arguments arrive there), then the cache
// registers its evaluation stack doesn't reach; floating-point arguments f1 and f13 (f1 the first one's), then
// the f-registers above its entries' (f2 + i). Each argument in a register; no floating-point locals (clearing
// one needs memory); value types of 4 or 8 bytes only (small_struct), in an integer register.
static bool leaf_registers(Ctx *c)
{
	if (c->framed || c->has_inst_arg || c->secobj || c->keep_secret)
		FAIL(c, LEAF_WHY("frame needed"));
	if (c->return_size && !small_struct(c->return_size))
		FAIL(c, LEAF_WHY("value type result"));
	if (c->leaf_depth > LEAF_CACHE_GPRS)
		FAIL(c, LEAF_WHY("deep stack"));
	if (JI_CALL(c->info, VB_METHOD, MI_getMethodAttribs, GetMethodAttribsFn, c->method->ftn) & FLG_SHAREDINST)
		FAIL(c, LEAF_WHY("shared generic code"));
	int pool[8], free = 0;
	pool[free++] = 3;
	pool[free++] = 4;
	for (int r = 5 + (int)c->leaf_depth; r <= 10; r++)
		pool[free++] = r;
	int fpool[16], ffree = 0;
	fpool[ffree++] = 1;
	fpool[ffree++] = 13;
	for (int f = 2 + (int)c->leaf_depth; f <= 11; f++)
		fpool[ffree++] = f;
	int next = 0, fnext = 0;
	for (uint32_t pass = 0; pass < 2; pass++) {
		Var *vars = pass ? c->locals : c->args;
		const uint32_t count = pass ? c->locals_count : c->args_count;
		for (uint32_t i = 0; i < count; i++) {
			const uint8_t k = vars[i].kind;
			if (k == K_STRUCT && !small_struct(vars[i].size))
				FAIL(c, LEAF_WHY(pass ? "value type local" : "value type argument"));
			if (k == K_R8) {
				if (pass)
					FAIL(c, LEAF_WHY("floating-point local"));
				if (fnext >= ffree || fnext >= 13)
					FAIL(c, LEAF_WHY("too many variables"));
				vars[i].freg = (int8_t)fpool[fnext++];
				continue;
			}
			if (next >= free || (!pass && i >= ARG_SLOTS))
				FAIL(c, LEAF_WHY("too many variables"));
			vars[i].reg = (int8_t)pool[next++];
		}
	}
	c->frame = 0;
	return true;
}

// A method with a frame keeps its most used integer and reference variables in r30 down to r17, which calls
// preserve, as Microsoft's code does: the prolog saves them below the link register's word (rN at caller SP
// - 8 * (33 - N), r31's place left free) before the stwu, where xcoreclr's virtual unwinder (0x84F08FA0) undoes each
// std of the prolog, and the GC info reports the references among them as register slots. Left in the frame: a
// variable whose address is taken, a value type or floating-point one, an argument passed on the stack, and the
// this of shared generic code (the runtime reads its generics context from the slot), and in a method with handlers
// those a handler uses (RyuJIT likewise keeps a variable live into a handler on the stack): the others are restored
// when a catch resumes the method, the runtime rebuilding its registers by unwinding the frames above it. Not for
// a leaf, whose variables have the volatile registers.
// A method with a calli (C# never makes one: the runtime's IL stubs do) saves all of r17-r30 and keeps no variable
// in them, unless it is one of the delegate stubs, whose calli calls managed code: a P/Invoke stub reaches its
// native target that way, and while the native call runs, the GC walks the managed frames from the stub's, whatever
// state the runtime's transition frame recorded, finding its callers' variables only in a frame that saved them
// (RyuJIT saves every callee-saved register in a method with a P/Invoke frame for the same reason), unless the
// runtime's transition frame records them itself: NGenBench's PInvokeUnderGC passes without these saves too, so
// they may be needless, but they cost only the stubs' prologs (docs/RESEARCH.md, open questions). Telling the stubs
// by their ldftn of a P/Invoke method misses some (DebugLog's DbgPrint stub).
static void choose_saved_registers(Ctx *c)
{
	c->saved_count = c->saved_fcount = 0;
	if (c->leaf)
		return;
#ifndef XNGEN_NO_SAVE_ALL // experiment: P/Invoke stubs without the saves (NGenBench still passes)
	if (c->calli && !c->managed_calli) {
		c->saved_count = SAVED_GPRS;
		return;
	}
#endif
	const bool shared = (JI_CALL(c->info, VB_METHOD, MI_getMethodAttribs, GetMethodAttribsFn, c->method->ftn) &
		FLG_SHAREDINST) != 0;
	const uint32_t inst_at = (sig_call_conv(&c->method->args) & CORINFO_CALLCONV_HASTHIS) ? 1 : 0;
	Var *chosen[SAVED_GPRS], *fchosen[SAVED_FPRS];
	uint32_t weight[SAVED_GPRS], fweight[SAVED_FPRS], count = 0, fcount = 0;
	uint32_t slot = c->return_size > 8 ? 1 : 0;
	for (uint32_t pass = 0; pass < 2; pass++) {
		Var *vars = pass ? c->locals : c->args;
		const uint32_t n = pass ? c->locals_count : c->args_count;
		for (uint32_t i = 0; i < n; i++) {
			bool fits = true;
			if (!pass) {
				if (c->has_inst_arg && i == inst_at)
					slot++;
				fits = slot < ARG_SLOTS && !(shared && i == 0 && inst_at);
				slot += var_slots(&vars[i]);
			}
			const uint8_t k = vars[i].kind;
			const uint32_t w = i < MAX_VARS ? c->uses[pass][i] : 0;
			// Saving and restoring costs two instructions; a variable used once or twice gains nothing.
			if (!fits || i >= MAX_VARS || c->taken[pass][i] || w < 3)
				continue;
			if (k == K_R8) {
				uint32_t at = fcount < SAVED_FPRS ? fcount++ : SAVED_FPRS;
				if (at == SAVED_FPRS) {
					if (w <= fweight[SAVED_FPRS - 1])
						continue;
					at = SAVED_FPRS - 1;
				}
				for (; at > 0 && fweight[at - 1] < w; at--)
					fchosen[at] = fchosen[at - 1], fweight[at] = fweight[at - 1];
				fchosen[at] = &vars[i], fweight[at] = w;
				continue;
			}
			if (k != K_I4 && k != K_I8 && k != K_REF && k != K_BYREF)
				continue;
			// Kept sorted by weight, heaviest first; the lightest falls off when all registers are taken.
			uint32_t at = count < SAVED_GPRS ? count++ : SAVED_GPRS;
			if (at == SAVED_GPRS) {
				if (w <= weight[SAVED_GPRS - 1])
					continue;
				at = SAVED_GPRS - 1;
			}
			for (; at > 0 && weight[at - 1] < w; at--)
				chosen[at] = chosen[at - 1], weight[at] = weight[at - 1];
			chosen[at] = &vars[i], weight[at] = w;
		}
	}
	for (uint32_t i = 0; i < count; i++)
		chosen[i]->reg = (int8_t)(30 - (int)i);
	c->saved_count = count;
	for (uint32_t i = 0; i < fcount; i++)
		fchosen[i]->freg = (int8_t)(31 - (int)i);
	c->saved_fcount = fcount;
}

// Where the prolog saves callee-saved register r, from the caller's SP.
static int32_t saved_offset(int r)
{
	return -8 * (33 - r);
}

// And floating-point register f, below the general registers' saves (as Microsoft's frames keep them: the unwinder
// undoes each stfd of the prolog like a std).
static int32_t saved_foffset(const Ctx *c, int f)
{
	return -8 * (2 + (int32_t)c->saved_count) - 8 * (32 - f);
}

static bool layout(Ctx *c)
{
	CORINFO_METHOD_INFO *m = c->method;
	ICorJitInfo *info = c->info;
	const bool has_this = (sig_call_conv(&m->args) & CORINFO_CALLCONV_HASTHIS) != 0;
	if ((sig_call_conv(&m->args) & CORINFO_CALLCONV_MASK) != CORINFO_CALLCONV_DEFAULT)
		FAIL(c, "calling convention");
	// Shared generic code takes its instantiation as a hidden argument (PARAMTYPE): the MethodDesc, in the register
	// after this (first for a static method: Microsoft's Interlocked.CompareExchange<__Canon> in mscorlib.ni.dll,
	// 0x17E220, moves r4-r6 down to r3-r5 for the object overload). It is kept in the frame, but no runtime lookup
	// through it is made: any token that depends on the type parameters, and any callee that needs an
	// instantiation of its own, declines the method (generic_context). A method of a generic class finds its
	// instantiation through this, and an exact instantiation of a generic method (GENERIC without PARAMTYPE,
	// Pick<int>) resolves its tokens to exact types: both compile. The runtime's delegate stubs for a generic
	// delegate type are of the first kind, and must compile (an image whose stubs stay IL fails with
	// BadImageFormatException).
	if (sig_call_conv(&m->args) & CORINFO_CALLCONV_EXPLICITTHIS)
		FAIL(c, "explicit this");
	c->has_inst_arg = (sig_call_conv(&m->args) & CORINFO_CALLCONV_PARAMTYPE) != 0;

	c->args_count = 0;
	if (has_this) {
		CORINFO_CLASS_HANDLE cls = JI_CALL(info, VB_METHOD, MI_getMethodClass, GetMethodClassFn, m->ftn);
		const uint32_t t = JI_CALL(info, VB_CLASS, CI_asCorInfoType, AsCorInfoTypeFn, cls);
		// A value type's method (a struct's, a primitive's: asCorInfoType gives the primitive type itself)
		// takes this by reference.
		const bool byref = t != CORINFO_TYPE_CLASS && t != CORINFO_TYPE_STRING;
		c->args[c->args_count++] = (Var){byref ? K_BYREF : K_REF,
			byref ? CORINFO_TYPE_BYREF : CORINFO_TYPE_CLASS, 0, 0, 0, NO_REFS, NO_REFS, 0, 0};
	}
	CORINFO_ARG_LIST_HANDLE arg = m->args.args;
	for (uint32_t i = 0; i < sig_num_args(&m->args); i++) {
		CORINFO_CLASS_HANDLE cls = 0;
		const uint32_t t = JI_CALL(info, VB_ARG, ARG_getArgType, GetArgTypeFn, &m->args, arg, &cls) & 0x3F;
		if (c->args_count >= MAX_VARS || !make_var(c, &c->args[c->args_count++], t, cls, false))
			FAIL_UNLESS_SET(c, fail_type(c, "argument type", t, cls));
		arg = JI_CALL(info, VB_ARG, ARG_getArgNext, GetArgNextFn, arg);
	}
	c->locals_count = 0;
	arg = m->locals.args;
	for (uint32_t i = 0; i < sig_num_args(&m->locals); i++) {
		CORINFO_CLASS_HANDLE cls = 0;
		const uint32_t t = JI_CALL(info, VB_ARG, ARG_getArgType, GetArgTypeFn, &m->locals, arg, &cls) & 0x3F;
		if (c->locals_count >= MAX_VARS || !make_var(c, &c->locals[c->locals_count++], t, cls, true))
			FAIL_UNLESS_SET(c, fail_type(c, "local type", t, cls));
		arg = JI_CALL(info, VB_ARG, ARG_getArgNext, GetArgNextFn, arg);
	}
	c->return_type = sig_ret_type(&m->args);
	if (!return_info(c, &m->args, &c->return_size))
		return false;
	c->return_cls = m->args.retTypeClass;
	// The arguments arrive in r3-r10 (the return buffer, if any, first, then this, then the instantiation argument:
	// Microsoft's KeyValuePair<__Canon, 16-byte struct>::get_Value, mscorlib.ni.dll 0x472FDC, copies from this in
	// r4 to the buffer in r3, so the instantiation argument it never uses comes after both): more would be on the
	// stack.
	uint32_t slots = (c->return_size > 8 ? 1 : 0) + (c->has_inst_arg ? 1 : 0);
	for (uint32_t i = 0; i < c->args_count; i++)
		slots += var_slots(&c->args[i]);
	if (slots > ARG_SLOTS + MAX_STACK_ARG_SLOTS)
		FAIL(c, "arguments on the stack");

	c->max_stack = m->maxStack + 2;
	if (c->max_stack > MAX_STACK)
		FAIL(c, "max stack");

	choose_saved_registers(c);
	int32_t at = c->outgoing;
	c->stack_ng = at;
	at += 8 * (int32_t)c->max_stack;
	c->stack_struct = at;
	at += c->struct_slot * (int32_t)c->max_stack;
	for (uint32_t i = 0; i < c->args_count; i++)
		if (c->args[i].kind != K_REF && c->args[i].kind != K_BYREF && !c->args[i].reg && !c->args[i].freg)
			c->args[i].offset = (int16_t)at, at += var_bytes(&c->args[i]);
	for (uint32_t i = 0; i < c->locals_count; i++)
		if (c->locals[i].kind != K_REF && c->locals[i].kind != K_BYREF && !c->locals[i].reg && !c->locals[i].freg)
			c->locals[i].offset = (int16_t)at, at += var_bytes(&c->locals[i]);
	c->scratch = at;
	at += 8;
	c->inst_arg = 0;
	if (c->has_inst_arg)
		c->inst_arg = at, at += 8;
	c->inst_temp = at;
	at += 8;
	c->secret = at;
	at += 8;
	// GC slots: interior pointers first, then object references, as the GC info encoding wants.
	c->gc_start = c->interior_start = at;
	c->stack_byref = at;
	at += 4 * (int32_t)c->max_stack;
	for (uint32_t i = 0; i < c->args_count; i++)
		if (c->args[i].kind == K_BYREF && !c->args[i].reg)
			c->args[i].offset = (int16_t)at, at += 4;
	for (uint32_t i = 0; i < c->locals_count; i++)
		if (c->locals[i].kind == K_BYREF && !c->locals[i].reg)
			c->locals[i].offset = (int16_t)at, at += 4;
	c->retbuf = 0;
	if (c->return_size > 8)
		c->retbuf = at, at += 4;
	c->interior_end = at;
	c->stack_ref = at;
	at += 4 * (int32_t)c->max_stack;
	for (uint32_t i = 0; i < c->args_count; i++)
		if (c->args[i].kind == K_REF && !c->args[i].reg)
			c->args[i].offset = (int16_t)at, at += 4;
	for (uint32_t i = 0; i < c->locals_count; i++)
		if (c->locals[i].kind == K_REF && !c->locals[i].reg)
			c->locals[i].offset = (int16_t)at, at += 4;
	c->newobj_temp = at;
	at += 4;
	c->secobj = 0;
	if (c->security || (JI_CALL(c->info, VB_METHOD, MI_getMethodAttribs, GetMethodAttribsFn, c->method->ftn) & FLG_SECURITYCHECK))
		c->secobj = at, at += 4;
	c->gc_end = at;
	// The saved link register, and with exception handlers r31 below it; with variables in callee-saved registers,
	// r31's place and theirs below it (saved_offset).
	c->frame = (at + (c->saved_count || c->saved_fcount ? 8 * (2 + (int32_t)(c->saved_count + c->saved_fcount)) :
		c->framed ? 16 : 8) + 15) & ~15;
	if (c->frame > 0x7FF0 || (c->gc_end - c->gc_start) / 4 > MAX_SLOTS)
		FAIL(c,"frame too large");
	return !c->leaf || leaf_registers(c);
}

// A reference or native int variable into r: from its register or its slot.
static void load_var_word(Ctx *c, int r, const Var *v)
{
	if (v->reg)
		emit(c, ppc_mr(r, v->reg));
	else
		emit(c, ppc_lwz(r, v->offset, c->fp));
}

static void narrow_into(Ctx *c, uint32_t type, int to, int from)
{
	switch (type) {
	case CORINFO_TYPE_BYTE: emit(c, ppc_extsb(to, from)); break;
	case CORINFO_TYPE_SHORT: emit(c, ppc_extsh(to, from)); break;
	case CORINFO_TYPE_BOOL:
	case CORINFO_TYPE_UBYTE: emit(c, ppc_clrlwi(to, from, 24)); break;
	case CORINFO_TYPE_CHAR:
	case CORINFO_TYPE_USHORT: emit(c, ppc_clrlwi(to, from, 16)); break;
	default:
		if (to != from)
			emit(c, ppc_mr(to, from));
		break;
	}
}

static void parallel_move(Ctx *c, bool fp, int *src, int *dst, int n);

// A leaf method's prolog: each argument from the register it arrives in (r3 + i, a floating-point one the next of
// f1-f13) to its own, all at once (parallel_move), then narrowed; the locals cleared.
static void leaf_prolog(Ctx *c)
{
	int src[2][ARG_SLOTS + 16], dst[2][ARG_SLOTS + 16], moves[2] = {0, 0}, fargs = 0;
	for (uint32_t i = 0; i < c->args_count; i++) {
		const Var *v = &c->args[i];
		const int fp = v->kind == K_R8;
		src[fp][moves[fp]] = fp ? 1 + fargs++ : 3 + (int)i;
		dst[fp][moves[fp]++] = fp ? v->freg : v->reg;
	}
	for (int fp = 0; fp < 2; fp++)
		parallel_move(c, fp, src[fp], dst[fp], moves[fp]);
	for (uint32_t i = 0; i < c->args_count; i++)
		if (c->args[i].kind != K_R8)
			narrow_into(c, c->args[i].type, c->args[i].reg, c->args[i].reg);
	for (uint32_t i = 0; i < c->locals_count; i++)
		emit(c, ppc_li(c->locals[i].reg, 0));
}

static void prolog(Ctx *c)
{
	if (c->leaf) {
		leaf_prolog(c);
		return;
	}
	emit(c, ppc_mflr(12));
	emit(c, ppc_stw(12, -8, 1));
	if (c->framed) {
		emit(c, ppc_li(0, 0));
		emit(c, ppc_stw(0, 4, 1));
		for (uint32_t i = c->saved_count; i > 0; i--)
			emit(c, ppc_std(31 - (int)i, saved_offset(31 - (int)i), 1));
		for (uint32_t i = 0; i < c->saved_fcount; i++)
			emit(c, ppc_stfd(31 - (int)i, saved_foffset(c, 31 - (int)i), 1));
		emit(c, ppc_std(31, -16, 1));
		emit(c, ppc_addi(31, 1, -c->frame));
		emit(c, ppc_stwu(1, -c->frame, 1));
		emit(c, ppc_addi(12, 1, c->frame));
		emit(c, ppc_stw(12, c->frame + 4, 31));
	} else {
		// Before the stwu, as Microsoft's prologs save r30 and r31: the unwinder reads them back from the caller's SP.
		for (uint32_t i = 0; i < c->saved_count; i++)
			emit(c, ppc_std(30 - (int)i, saved_offset(30 - (int)i), 1));
		for (uint32_t i = 0; i < c->saved_fcount; i++)
			emit(c, ppc_stfd(31 - (int)i, saved_foffset(c, 31 - (int)i), 1));
		emit(c, ppc_stwu(1, -c->frame, 1));
	}
	// A precode sets r11 to its own address before jumping to the code (word 3 of it is the MethodDesc), and the
	// runtime's IL stubs read that as their secret parameter (StubHelpers.GetStubContext). Kept only by methods
	// that read it: one that meets GetStubContext without it is compiled again (which methods are stubs isn't
	// told apart reliably beforehand: their scope's tag bit isn't always set).
	if (c->keep_secret)
		emit(c, ppc_stw(11, c->secret, c->fp));
	// Zero every GC slot so the GC never sees garbage, and the non-GC locals (IL's localsinit).
	// The evaluation stack's slots are reported only where an entry holds a reference (scalar_slot), so they start
	// as they are; with handlers they are always reported, so cleared like the rest.
	emit(c, ppc_li(0, 0));
	for (int32_t at = c->gc_start; at < c->gc_end; at += 4) {
		const int32_t stack_bytes = 4 * (int32_t)c->max_stack;
		if (!c->eh && ((at >= c->stack_byref && at < c->stack_byref + stack_bytes) ||
			(at >= c->stack_ref && at < c->stack_ref + stack_bytes)))
			continue;
		// An argument's slot is written by the homing below, before anything can look at it.
		bool argument = false;
		for (uint32_t i = 0; i < c->args_count && !argument; i++)
			argument = c->args[i].kind != K_STRUCT && c->args[i].offset == at;
		if (argument)
			continue;
		emit(c, ppc_stw(0, at, c->fp));
	}
	int zero_freg = 0;
	for (uint32_t i = 0; i < c->locals_count; i++) {
		const Var *v = &c->locals[i];
		if (v->freg) {
			if (!zero_freg) {
				emit(c, ppc_std(0, c->scratch, c->fp));
				emit(c, ppc_lfd(v->freg, c->scratch, c->fp));
				zero_freg = v->freg;
			} else {
				emit(c, ppc_fmr(v->freg, zero_freg));
			}
		} else if (v->reg) {
			emit(c, ppc_li(v->reg, 0));
		} else if (v->kind == K_STRUCT) {
			for (int32_t at = 0; at < var_bytes(v); at += 8)
				emit(c, ppc_std(0, v->offset + at, c->fp));
		} else if (v->kind != K_REF && v->kind != K_BYREF) {
			emit(c, ppc_std(0, v->offset, c->fp));
		}
	}
	// Home the arguments: each takes the next of r3-r10 (a value type its 8-byte chunks, the last one left-justified;
	// one of 4 bytes like an integer), a floating-point one the next of f1-f13 as well, and a return buffer comes
	// first (NGenBench's calling-convention probes). The receiver widens narrow arguments (the console's convention).
	int slot = 0, fpr = 1;
	if (c->retbuf)
		emit(c, ppc_stw(3, c->retbuf, c->fp)), slot = 1;
	const uint32_t inst_at = (sig_call_conv(&c->method->args) & CORINFO_CALLCONV_HASTHIS) ? 1 : 0;
	for (uint32_t i = 0; i <= c->args_count; i++) {
		// The hidden instantiation argument takes the register after this (the first for a static method).
		if (c->has_inst_arg && i == inst_at)
			emit(c, ppc_stw(3 + slot, c->inst_arg, c->fp)), slot++;
		if (i == c->args_count)
			break;
		const Var *v = &c->args[i];
		const int r = 3 + slot;
		if (slot >= ARG_SLOTS) {
			// On the stack: the slot's 8 bytes hold an 8-byte value or a value type's chunk, an int in the low word.
			const int32_t in = c->frame + LINKAGE_BYTES + 8 * (slot - ARG_SLOTS);
			if (v->kind == K_STRUCT) {
				for (uint32_t at = 0; at < v->size; at += 8) {
					if (v->size <= 4) {
						emit(c, ppc_lwz(0, in + 4, c->fp));
						emit(c, ppc_stw(0, v->offset, c->fp));
					} else {
						emit(c, ppc_ld(0, in + (int32_t)at, c->fp));
						emit(c, ppc_std(0, v->offset + (int32_t)at, c->fp));
					}
				}
				slot += (int)var_slots(v);
			} else if (v->kind == K_R8) {
				// Still in f1-f13 (a caller needn't store it in the slot); only the fourteenth and on are read there.
				const int from = fpr <= 13 ? fpr : 0;
				if (!from)
					emit(c, ppc_lfd(0, in, c->fp));
				else
					fpr++;
				emit(c, v->type == CORINFO_TYPE_FLOAT ? ppc_stfs(from, v->offset, c->fp) : ppc_stfd(from, v->offset, c->fp));
				slot++;
			} else if (v->kind == K_I8) {
				emit(c, ppc_ld(0, in, c->fp));
				emit(c, ppc_std(0, v->offset, c->fp));
				slot++;
			} else {
				emit(c, ppc_lwz(0, in + 4, c->fp));
				store_typed(c, v->type, 0, v->offset, c->fp);
				slot++;
			}
			continue;
		}
		if (v->kind == K_R8) {
			if (v->freg)
				emit(c, ppc_fmr(v->freg, fpr));
			else
				emit(c, v->type == CORINFO_TYPE_FLOAT ? ppc_stfs(fpr, v->offset, c->fp) : ppc_stfd(fpr, v->offset, c->fp));
			fpr++, slot++;
			continue;
		}
		if (v->kind == K_STRUCT) {
			for (uint32_t at = 0; at < v->size; at += 8) {
				const int chunk = r + (int)(at / 8);
				if (slot + (int)(at / 8) >= ARG_SLOTS) {
					// A value type that starts in the registers and goes on into the caller's slots (a 136-byte
					// one from r3: NGenBench's BigStruct).
					const int32_t in = c->frame + LINKAGE_BYTES + 8 * (slot + (int)(at / 8) - ARG_SLOTS);
					if (v->size - at >= 8) {
						emit(c, ppc_ld(0, in, c->fp));
						emit(c, ppc_std(0, v->offset + (int32_t)at, c->fp));
					} else {
						emit(c, ppc_lwz(0, in, c->fp));
						emit(c, ppc_stw(0, v->offset + (int32_t)at, c->fp));
					}
				} else if (v->size <= 4) {
					emit(c, ppc_stw(chunk, v->offset, c->fp));
				} else if (v->size - at >= 8) {
					emit(c, ppc_std(chunk, v->offset + (int32_t)at, c->fp));
				} else {
					emit(c, ppc_rotldi32(0, chunk));
					emit(c, ppc_stw(0, v->offset + (int32_t)at, c->fp));
				}
			}
			slot += (int)var_slots(v);
			continue;
		}
		slot++;
		if (v->reg) {
			narrow_into(c, v->type, v->reg, r);
			continue;
		}
		switch (v->type) {
		case CORINFO_TYPE_BYTE:
			emit(c, ppc_extsb(r, r));
			break;
		case CORINFO_TYPE_SHORT:
			emit(c, ppc_extsh(r, r));
			break;
		default:
			break;
		}
		store_typed(c, v->type, r, v->offset, c->fp);
	}
}

static void epilog(Ctx *c)
{
	if (c->leaf) {
		emit(c, PPC_BLR);
		return;
	}
	// The main body's later returns branch to its first epilog (with saved registers it is 4 + saved_count words).
#ifdef XNGEN_NO_SHARED_EPILOG // experiment: every return its own epilog
	c->epilog_at = 0;
#endif
	if (c->epilog_at && !c->funclet) {
		emit(c, ppc_b(((int32_t)c->epilog_at - 1 - (int32_t)c->n) * 4));
		return;
	}
	if (!c->funclet)
		c->epilog_at = c->n + 1;
	emit(c, ppc_addi(1, c->fp, c->frame));
	emit(c, ppc_lwz(12, -8, 1));
	emit(c, ppc_mtlr(12));
	if (c->framed)
		emit(c, ppc_ld(31, -16, 1));
	for (uint32_t i = 0; i < c->saved_count; i++)
		emit(c, ppc_ld(30 - (int)i, saved_offset(30 - (int)i), 1));
	for (uint32_t i = 0; i < c->saved_fcount; i++)
		emit(c, ppc_lfd(31 - (int)i, saved_foffset(c, 31 - (int)i), 1));
	emit(c, PPC_BLR);
}

// A funclet's entry: r3 is the frame it runs for, whose +4 holds the parent's caller SP (the PSPSym), which the
// funclet keeps at its own caller SP + 4 for funclets it calls.
static void funclet_prolog(Ctx *c)
{
	emit(c, ppc_std(31, -8, 1));
	emit(c, ppc_lwz(3, 4, 3));
	emit(c, ppc_stw(3, 4, 1));
	emit(c, ppc_addi(31, 3, -c->frame));
	emit(c, ppc_mflr(12));
	emit(c, ppc_stw(12, -16, 1));
	emit(c, ppc_stwu(1, -FUNCLET_FRAME, 1));
}

static void funclet_epilog(Ctx *c)
{
	emit(c, ppc_lwz(1, 0, 1));
	emit(c, ppc_ld(31, -8, 1));
	emit(c, ppc_lwz(12, -16, 1));
	emit(c, ppc_mtlr(12));
	emit(c, PPC_BLR);
}

// ---------------------------------------------------------------------------------------------------------------
// Branches.

static bool note_target(Ctx *c, uint32_t il)
{
	if (il >= c->il_size || !c->branch_target[il])
		FAIL(c, "branch target");
	const uint32_t t = c->branch_target[il] - 1u;
	if (c->target_depth[t] == 0) {
		c->target_depth[t] = (uint8_t)(c->depth + 1);
		for (uint32_t i = 0; i < c->depth && i < TARGET_ENTRIES; i++) {
			c->target_kinds[t][i] = c->stack[i];
			c->target_cls[t][i] = c->stack_cls[i];
			c->target_size[t][i] = c->stack_size[i];
		}
	} else if (c->target_depth[t] != c->depth + 1) {
		FAIL(c, "inconsistent stack at a branch target");
	}
	return true;
}

static const char FAIL_LONG_BRANCH[] = "conditional branch out of range";

static bool branch_to(Ctx *c, uint32_t il, int bo, int bi)
{
	flush(c); // stores only: the condition register is untouched
	if (!note_target(c, il))
		return false;
	if (c->fixups_count >= MAX_FIXUPS)
		FAIL(c, "branches");
	// In a method whose conditional branches don't all reach (more than 32 KB of code), each is the opposite test
	// over an unconditional branch.
	if (bo >= 0 && c->long_branches) {
		emit(c, ppc_bc(bo ^ 8, bi, 8));
		bo = -1;
	}
	c->fixups[c->fixups_count++] = (Fixup){c->n, il, TO_IL};
	emit(c, bo < 0 ? ppc_b(0) : ppc_bc(bo, bi, 0));
	return true;
}

// Compares the top two entries, setting cr6; returns false on a mix it doesn't handle. For floating point, sets
// `fp` so callers pick the unordered-aware forms.
static bool compare_top(Ctx *c, bool is_unsigned, bool *fp)
{
	const enum Kind b = c->stack[c->depth - 1], a = c->stack[c->depth - 2];
	*fp = false;
	if (a == K_STRUCT || b == K_STRUCT)
		FAIL(c, "comparison of value types");
	if (a == K_R8 && b == K_R8) {
		const int fa = foperand(c, 1, 0), fb = foperand(c, 0, 12);
		emit(c, ppc_fcmpu(6, fa, fb));
		*fp = true;
	} else if (a == K_I8 && b == K_I8) {
		const int ra = operand(c, 1, 11), rb = operand(c, 0, 12);
		emit(c, is_unsigned ? ppc_cmpld(6, ra, rb) : ppc_cmpd(6, ra, rb));
	} else if (a != K_R8 && b != K_R8 && a != K_I8 && b != K_I8) {
		const int ra = operand(c, 1, 11), rb = operand(c, 0, 12);
		const bool pointers = a == K_REF || a == K_BYREF || b == K_REF || b == K_BYREF;
		emit(c, is_unsigned || pointers ? ppc_cmplw(6, ra, rb) : ppc_cmpw(6, ra, rb));
	} else {
		FAIL(c, "comparison of mixed types");
	}
	return pop(c, 2);
}

// The cr6 test for a comparison: which bit, and whether to branch on it set. `op` follows the IL order of
// beq..blt.un: 0 eq, 1 ge, 2 gt, 3 le, 4 lt, 5 ne.un, 6 ge.un, 7 gt.un, 8 le.un, 9 lt.un. The .un forms are
// unsigned for integers and true when unordered for floating point.
static void condition(Ctx *c, int op, bool fp, int *bo, int *bi)
{
	const int base = 6 * 4;
	switch (op) {
	case 0: *bo = BO_TRUE; *bi = base + CR_EQ; break;
	case 5: *bo = BO_FALSE; *bi = base + CR_EQ; break;
	case 2: *bo = BO_TRUE; *bi = base + CR_GT; break;
	case 4: *bo = BO_TRUE; *bi = base + CR_LT; break;
	case 1: // ge: not lt for integers; for floats gt or eq
		if (fp) {
			emit(c, ppc_cror(base + CR_EQ, base + CR_GT, base + CR_EQ));
			*bo = BO_TRUE; *bi = base + CR_EQ;
		} else {
			*bo = BO_FALSE; *bi = base + CR_LT;
		}
		break;
	case 3: // le
		if (fp) {
			emit(c, ppc_cror(base + CR_EQ, base + CR_LT, base + CR_EQ));
			*bo = BO_TRUE; *bi = base + CR_EQ;
		} else {
			*bo = BO_FALSE; *bi = base + CR_GT;
		}
		break;
	case 6: *bo = BO_FALSE; *bi = base + CR_LT; break;   // ge.un: not lt (true when unordered)
	case 8: *bo = BO_FALSE; *bi = base + CR_GT; break;   // le.un: not gt
	case 7: // gt.un: not le
		if (fp) {
			emit(c, ppc_cror(base + CR_EQ, base + CR_LT, base + CR_EQ));
			*bo = BO_FALSE; *bi = base + CR_EQ;
		} else {
			*bo = BO_TRUE; *bi = base + CR_GT;
		}
		break;
	default: // 9 lt.un: not ge
		if (fp) {
			emit(c, ppc_cror(base + CR_EQ, base + CR_GT, base + CR_EQ));
			*bo = BO_FALSE; *bi = base + CR_EQ;
		} else {
			*bo = BO_TRUE; *bi = base + CR_LT;
		}
		break;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Instructions.

static bool op_binary(Ctx *c, uint8_t op)
{
	const enum Kind b = c->stack[c->depth - 1], a = c->stack[c->depth - 2];
	if (a == K_STRUCT || b == K_STRUCT)
		FAIL(c, "arithmetic on a value type");
	if (a == K_R8 && b == K_R8) {
		if (op == 0x5D) { // rem: the runtime's helper (C's fmod)
			load_entry(c, 1, 0, 1);
			load_entry(c, 0, 0, 2);
			if (!call_helper(c, HELP_DBLREM))
				return false;
			return pop(c, 2) && store_push(c, K_R8, 0, 1);
		}
		// From the registers holding the operands into the one the result stays in (fresult_register).
		const int fa = foperand(c, 1, 0), fb = foperand(c, 0, 12);
		if (!pop(c, 2))
			return false;
		const int fd = fresult_register(c, 0);
		switch (op) {
		case 0x58: emit(c, ppc_fadd(fd, fa, fb)); break;
		case 0x59: emit(c, ppc_fsub(fd, fa, fb)); break;
		case 0x5A: emit(c, ppc_fmul(fd, fa, fb)); break;
		case 0x5B: emit(c, ppc_fdiv(fd, fa, fb)); break;
		default: FAIL(c, "floating point operation");
		}
		return store_push(c, K_R8, 0, fd);
	}
	const bool wide = a == K_I8 || b == K_I8;
	const bool shift = op == 0x62 || op == 0x63 || op == 0x64;
	if (shift ? (b != K_I4) : (wide && (a != K_I8 || b != K_I8)))
		FAIL(c, "operand types");
	if (a == K_R8 || b == K_R8)
		FAIL(c, "operand types");
	enum Kind result = wide ? K_I8 : K_I4;
	if (!wide) {
		// Pointer arithmetic: byref +/- int stays a byref; an object reference can't take part.
		if (a == K_REF || b == K_REF)
			FAIL(c, "arithmetic on an object reference");
		if (a == K_BYREF || b == K_BYREF) {
			if (op != 0x58 && op != 0x59)
				FAIL(c, "byref arithmetic");
			result = (a == K_BYREF && b == K_BYREF) ? K_I4 : K_BYREF;
		}
	}
	if (shift)
		result = a;
	// The plain operations straight from the operands' registers into the result's.
	if ((op >= 0x58 && op <= 0x5A) || (op >= 0x5F && op <= 0x64)) {
		const int ra = operand(c, 1, 11), rb = operand(c, 0, 12);
		if (!pop(c, 2))
			return false;
		const int d = result_register(c, result, 11);
		switch (op) {
		case 0x58: emit(c, ppc_add(d, ra, rb)); break;
		case 0x59: emit(c, ppc_subf(d, rb, ra)); break;
		case 0x5A: emit(c, wide ? ppc_mulld(d, ra, rb) : ppc_mullw(d, ra, rb)); break;
		case 0x5F: emit(c, ppc_and(d, ra, rb)); break;
		case 0x60: emit(c, ppc_or(d, ra, rb)); break;
		case 0x61: emit(c, ppc_xor(d, ra, rb)); break;
		case 0x62: emit(c, wide ? ppc_sld(d, ra, rb) : ppc_slw(d, ra, rb)); break;
		case 0x63: emit(c, wide ? ppc_srad(d, ra, rb) : ppc_sraw(d, ra, rb)); break;
		default: emit(c, wide ? ppc_srd(d, ra, rb) : ppc_srw(d, ra, rb)); break;
		}
		return store_push(c, result, d, 0);
	}
	load_entry(c, 1, 11, 0);
	load_entry(c, 0, 12, 0);
	switch (op) {
	case 0x58: emit(c, ppc_add(11, 11, 12)); break;
	case 0x59: emit(c, ppc_subf(11, 12, 11)); break;
	case 0x5A: emit(c, wide ? ppc_mulld(11, 11, 12) : ppc_mullw(11, 11, 12)); break;
	case 0x5F: emit(c, ppc_and(11, 11, 12)); break;
	case 0x60: emit(c, ppc_or(11, 11, 12)); break;
	case 0x61: emit(c, ppc_xor(11, 11, 12)); break;
	case 0x62: emit(c, wide ? ppc_sld(11, 11, 12) : ppc_slw(11, 11, 12)); break;
	case 0x63: emit(c, wide ? ppc_srad(11, 11, 12) : ppc_sraw(11, 11, 12)); break;
	case 0x64: emit(c, wide ? ppc_srd(11, 11, 12) : ppc_srw(11, 11, 12)); break;
	case 0xD6: case 0xD7: case 0xD8: case 0xD9: case 0xDA: case 0xDB: { // add/mul/sub .ovf and .ovf.un
		if (result != K_I4 && result != K_I8)
			FAIL(c, "checked arithmetic on a pointer");
		const bool un = op & 1;
		int bo, bi;
		if (!wide) {
			// The exact result of the 64-bit operation on the widened operands must fit 32 bits again.
			emit(c, un ? ppc_clrldi32(11, 11) : ppc_extsw(11, 11));
			emit(c, un ? ppc_clrldi32(12, 12) : ppc_extsw(12, 12));
			emit(c, op <= 0xD7 ? ppc_add(11, 11, 12) : op <= 0xD9 ? ppc_mulld(11, 11, 12) : ppc_subf(11, 12, 11));
			emit(c, un ? ppc_clrldi32(12, 11) : ppc_extsw(12, 11));
			emit(c, ppc_cmpd(6, 12, 11));
			bo = BO_TRUE, bi = 6 * 4 + CR_EQ;
		} else if (op == 0xD6) { // signed: the result's sign differs from both operands'
			emit(c, ppc_add(10, 11, 12));
			emit(c, ppc_xor(11, 11, 10));
			emit(c, ppc_xor(12, 12, 10));
			emit(c, ppc_and(11, 11, 12));
			emit(c, ppc_cmpdi(6, 11, 0));
			emit(c, ppc_mr(11, 10));
			bo = BO_FALSE, bi = 6 * 4 + CR_LT;
		} else if (op == 0xD7) { // unsigned: the sum wrapped below an operand
			emit(c, ppc_add(10, 11, 12));
			emit(c, ppc_cmpld(6, 10, 11));
			emit(c, ppc_mr(11, 10));
			bo = BO_FALSE, bi = 6 * 4 + CR_LT;
		} else if (op == 0xDA) { // signed: the operands differ in sign and the result's differs from the first's
			emit(c, ppc_subf(10, 12, 11));
			emit(c, ppc_xor(12, 11, 12));
			emit(c, ppc_xor(11, 11, 10));
			emit(c, ppc_and(11, 11, 12));
			emit(c, ppc_cmpdi(6, 11, 0));
			emit(c, ppc_mr(11, 10));
			bo = BO_FALSE, bi = 6 * 4 + CR_LT;
		} else if (op == 0xDB) { // unsigned: the subtrahend is the larger
			emit(c, ppc_cmpld(6, 11, 12));
			emit(c, ppc_subf(11, 12, 11));
			bo = BO_FALSE, bi = 6 * 4 + CR_LT;
		} else if (op == 0xD8) { // signed: the high word must be the low word's sign extension
			emit(c, ppc_mulhd(9, 11, 12));
			emit(c, ppc_mulld(11, 11, 12));
			emit(c, ppc_li(0, 63));
			emit(c, ppc_srad(12, 11, 0));
			emit(c, ppc_cmpd(6, 9, 12));
			bo = BO_TRUE, bi = 6 * 4 + CR_EQ;
		} else { // unsigned: the high word must be zero
			emit(c, ppc_mulhdu(9, 11, 12));
			emit(c, ppc_mulld(11, 11, 12));
			emit(c, ppc_cmpdi(6, 9, 0));
			bo = BO_TRUE, bi = 6 * 4 + CR_EQ;
		}
		const uint32_t fits = c->n;
		emit(c, 0);
		if (!call_throw_helper(c, HELP_OVERFLOW))
			return false;
		c->code[fits] = ppc_bc(bo, bi, (int32_t)(c->n - fits) * 4);
		break;
	}
	case 0x5B: // div
	case 0x5C: // div.un
	case 0x5D: // rem
	case 0x5E: { // rem.un
		const bool is_unsigned = op == 0x5C || op == 0x5E;
		// A zero divisor (and int.MinValue / -1) throws. There's no divide-by-zero helper in this build's table,
		// so both go to the overflow helper: an ArithmeticException either way.
		emit(c, wide ? ppc_cmpd(6, 12, 0) : ppc_cmpwi(6, 12, 0));
		if (wide) {
			// cmpd against r0 needs r0 = 0.
			c->n--;
			emit(c, ppc_li(0, 0));
			emit(c, ppc_cmpd(6, 12, 0));
		}
		const uint32_t skip = c->n;
		emit(c, ppc_bc(BO_FALSE, 6 * 4 + CR_EQ, 0));
		if (!call_throw_helper(c, HELP_OVERFLOW))
			return false;
		c->code[skip] = ppc_bc(BO_FALSE, 6 * 4 + CR_EQ, (int32_t)(c->n - skip) * 4);
		if (!is_unsigned) {
			// -1 divisor: divw/divd give an undefined result for MinValue; negate instead (and rem is 0). The
			// MinValue case wraps rather than throwing.
			emit(c, wide ? ppc_li(0, -1) : ppc_cmpwi(6, 12, -1));
			if (wide)
				emit(c, ppc_cmpd(6, 12, 0));
			const uint32_t not_minus_one = c->n;
			emit(c, ppc_bc(BO_FALSE, 6 * 4 + CR_EQ, 0));
			emit(c, op == 0x5B ? ppc_neg(11, 11) : ppc_li(11, 0));
			const uint32_t done = c->n;
			emit(c, ppc_b(0));
			c->code[not_minus_one] = ppc_bc(BO_FALSE, 6 * 4 + CR_EQ, (int32_t)(c->n - not_minus_one) * 4);
			if (op == 0x5B) {
				emit(c, wide ? ppc_divd(11, 11, 12) : ppc_divw(11, 11, 12));
			} else {
				emit(c, wide ? ppc_divd(10, 11, 12) : ppc_divw(10, 11, 12));
				emit(c, wide ? ppc_mulld(10, 10, 12) : ppc_mullw(10, 10, 12));
				emit(c, ppc_subf(11, 10, 11));
			}
			c->code[done] = ppc_b((int32_t)(c->n - done) * 4);
		} else if (op == 0x5C) {
			emit(c, wide ? ppc_divdu(11, 11, 12) : ppc_divwu(11, 11, 12));
		} else {
			emit(c, wide ? ppc_divdu(10, 11, 12) : ppc_divwu(10, 11, 12));
			emit(c, wide ? ppc_mulld(10, 10, 12) : ppc_mullw(10, 10, 12));
			emit(c, ppc_subf(11, 10, 11));
		}
		break;
	}
	default:
		FAIL(c, "operation");
	}
	return pop(c, 2) && store_push(c, result, 11, 0);
}

// conv.ovf.*: the target CorInfoType, and whether the source is read as unsigned.
static uint32_t convert_ovf_type(uint8_t op, bool *un)
{
	static const uint8_t targets[10] = {CORINFO_TYPE_BYTE, CORINFO_TYPE_SHORT, CORINFO_TYPE_INT, CORINFO_TYPE_LONG,
		CORINFO_TYPE_UBYTE, CORINFO_TYPE_USHORT, CORINFO_TYPE_UINT, CORINFO_TYPE_ULONG, CORINFO_TYPE_NATIVEINT,
		CORINFO_TYPE_NATIVEUINT};
	static const uint8_t signed_targets[8] = {CORINFO_TYPE_BYTE, CORINFO_TYPE_UBYTE, CORINFO_TYPE_SHORT,
		CORINFO_TYPE_USHORT, CORINFO_TYPE_INT, CORINFO_TYPE_UINT, CORINFO_TYPE_LONG, CORINFO_TYPE_ULONG};
	*un = op >= 0x82 && op <= 0x8B;
	if (*un)
		return targets[op - 0x82];
	if (op >= 0xB3 && op <= 0xBA)
		return signed_targets[op - 0xB3];
	return op == 0xD4 ? CORINFO_TYPE_NATIVEINT : CORINFO_TYPE_NATIVEUINT;
}

// conv.*: the target CorInfoType.
static bool op_convert(Ctx *c, uint32_t to)
{
	if (c->stack[c->depth - 1] == K_STRUCT)
		FAIL(c, "conversion of a value type");
	const enum Kind from = (enum Kind)c->stack[c->depth - 1];
	// A floating-point operand from the register holding it, an integer one in r11.
	const int fa = from == K_R8 ? foperand(c, 0, 0) : 0;
	if (from != K_R8)
		load_entry(c, 0, 11, 0);
	if (!pop(c, 1))
		return false;
	const enum Kind target = kind_of(to);
	// conv.i/conv.u of a reference is how `fixed (char* p = s)` takes a string's address (the object is pinned
	// through a pinned local, which the GC info reports); the result is an unmanaged pointer, untracked from here.
	if (from == K_REF && target == K_R8)
		FAIL(c, "conversion of an object reference to floating point");
	if (target == K_R8) {
		if (from == K_R8) {
			const int fd = fresult_register(c, 0);
			if (to == CORINFO_TYPE_FLOAT)
				emit(c, ppc_frsp(fd, fa));
			else if (fd != fa)
				emit(c, ppc_fmr(fd, fa));
			return store_push(c, K_R8, 0, fd);
		} else {
			// Integer to floating point through memory: fcfid takes a 64-bit integer.
			if (from != K_I8)
				emit(c, to == CORINFO_TYPE_ULONG ? ppc_clrldi32(11, 11) : ppc_extsw(11, 11));
			emit(c, ppc_std(11, c->scratch, c->fp));
			emit(c, ppc_lfd(0, c->scratch, c->fp));
			emit(c, ppc_fcfid(0, 0));
			if (to == CORINFO_TYPE_FLOAT)
				emit(c, ppc_frsp(0, 0));
		}
		return store_push(c, K_R8, 0, 0);
	}
	if (from == K_R8) {
		emit(c, target == K_I8 ? ppc_fctidz(0, fa) : ppc_fctiwz(0, fa));
		emit(c, ppc_stfd(0, c->scratch, c->fp));
		if (target == K_I8)
			emit(c, ppc_ld(11, c->scratch, c->fp));
		else
			emit(c, ppc_lwz(11, c->scratch + 4, c->fp));
	}
	switch (to) {
	case CORINFO_TYPE_BYTE: emit(c, ppc_extsb(11, 11)); break;
	case CORINFO_TYPE_UBYTE: emit(c, ppc_clrlwi(11, 11, 24)); break;
	case CORINFO_TYPE_SHORT: emit(c, ppc_extsh(11, 11)); break;
	case CORINFO_TYPE_USHORT: emit(c, ppc_clrlwi(11, 11, 16)); break;
	case CORINFO_TYPE_LONG:
		if (from != K_I8 && from != K_R8)
			emit(c, ppc_extsw(11, 11));
		break;
	case CORINFO_TYPE_ULONG:
		if (from != K_I8 && from != K_R8)
			emit(c, ppc_clrldi32(11, 11));
		break;
	default:
		break;
	}
	return store_push(c, target == K_NONE ? K_I4 : target, 11, 0);
}

// conv.ovf.*: the value, widened to 64 bits as the source's signedness says, must survive the narrowing to the
// target unchanged (or keep its sign where the signedness changes between 64-bit types).
static bool op_convert_ovf(Ctx *c, uint32_t to, bool un)
{
	const enum Kind from = c->stack[c->depth - 1];
	if (from == K_R8) {
		// From floating point through the runtime's checked conversions (f1 to r3, raising OverflowException):
		// to long, ulong, int or uint; a smaller target narrows the int further through the integer path below.
		const bool wide = to == CORINFO_TYPE_LONG || to == CORINFO_TYPE_ULONG;
		const bool to_unsigned = to == CORINFO_TYPE_ULONG || to == CORINFO_TYPE_UINT || to == CORINFO_TYPE_NATIVEUINT;
		load_entry(c, 0, 0, 1);
		if (!pop(c, 1))
			return false;
		if (!call_helper(c, wide ? (to_unsigned ? HELP_DBL2ULNG_OVF : HELP_DBL2LNG_OVF)
			: (to_unsigned ? HELP_DBL2UINT_OVF : HELP_DBL2INT_OVF)))
			return false;
		if (!store_push(c, wide ? K_I8 : K_I4, 3, 0))
			return false;
		const bool done = wide || to_unsigned || to == CORINFO_TYPE_INT || to == CORINFO_TYPE_NATIVEINT;
		return done || op_convert_ovf(c, to, false);
	}
	if (from != K_I4 && from != K_I8)
		FAIL(c, "checked conversion of a non-integer");
	load_entry(c, 0, 11, 0);
	if (!pop(c, 1))
		return false;
	if (from == K_I4)
		emit(c, un ? ppc_clrldi32(11, 11) : ppc_extsw(11, 11));
	bool check = true;
	int bo = BO_TRUE, bi = 6 * 4 + CR_EQ;
	switch (to) {
	case CORINFO_TYPE_BYTE: emit(c, ppc_extsb(12, 11)); break;
	case CORINFO_TYPE_UBYTE: emit(c, ppc_clrlwi(12, 11, 24)); break;
	case CORINFO_TYPE_SHORT: emit(c, ppc_extsh(12, 11)); break;
	case CORINFO_TYPE_USHORT: emit(c, ppc_clrlwi(12, 11, 16)); break;
	case CORINFO_TYPE_INT: case CORINFO_TYPE_NATIVEINT: emit(c, ppc_extsw(12, 11)); break;
	case CORINFO_TYPE_UINT: case CORINFO_TYPE_NATIVEUINT: emit(c, ppc_clrldi32(12, 11)); break;
	case CORINFO_TYPE_LONG: case CORINFO_TYPE_ULONG:
		// Only a change of signedness can fail: a negative 64-bit value has no unsigned reading and vice versa.
		check = (to == CORINFO_TYPE_LONG) == un;
		emit(c, ppc_cmpdi(6, 11, 0));
		bo = BO_FALSE, bi = 6 * 4 + CR_LT;
		break;
	default:
		FAIL(c, "checked conversion target");
	}
	if (check) {
		if (to != CORINFO_TYPE_LONG && to != CORINFO_TYPE_ULONG)
			emit(c, ppc_cmpd(6, 12, 11));
		const uint32_t fits = c->n;
		emit(c, 0);
		if (!call_throw_helper(c, HELP_OVERFLOW))
			return false;
		c->code[fits] = ppc_bc(bo, bi, (int32_t)(c->n - fits) * 4);
	}
	const enum Kind target = kind_of(to);
	return store_push(c, target == K_NONE ? K_I4 : target, 11, 0);
}

// A primitive field of a value type held in register r (small_struct: its bytes as a big-endian integer of `size`
// bytes) into d, widened as load_typed would: rotated down to the low bits and the rest cleared, then sign-extended
// as its type needs. Floating-point fields would need memory to reach an f-register.
static bool extract_field(Ctx *c, uint32_t type, int d, int r, uint32_t size, uint32_t offset)
{
	uint32_t n;
	switch (type) {
	case CORINFO_TYPE_BOOL: case CORINFO_TYPE_BYTE: case CORINFO_TYPE_UBYTE:
		n = 1;
		break;
	case CORINFO_TYPE_CHAR: case CORINFO_TYPE_SHORT: case CORINFO_TYPE_USHORT:
		n = 2;
		break;
	case CORINFO_TYPE_LONG: case CORINFO_TYPE_ULONG:
		n = 8;
		break;
	case CORINFO_TYPE_INT: case CORINFO_TYPE_UINT: case CORINFO_TYPE_NATIVEINT: case CORINFO_TYPE_NATIVEUINT:
	case CORINFO_TYPE_PTR: case CORINFO_TYPE_CLASS: case CORINFO_TYPE_STRING: case CORINFO_TYPE_BYREF:
		n = 4;
		break;
	default:
		FAIL(c, LEAF_WHY("value type field type"));
	}
	if (offset + n > size)
		FAIL(c, "value type field offset");
	const int shift = 8 * (int)(size - offset - n);
	emit(c, ppc_rldicl(d, r, (64 - shift) & 63, 64 - 8 * (int)n));
	if (type == CORINFO_TYPE_BYTE)
		emit(c, ppc_extsb(d, d));
	else if (type == CORINFO_TYPE_SHORT)
		emit(c, ppc_extsh(d, d));
	return true;
}

static bool op_field(Ctx *c, uint8_t op, uint32_t token)
{
	CORINFO_FIELD_HANDLE field = JI_CALL(c->info, VB_MODULE, MOD_findField, FindFieldFn, c->method->scope, token,
		c->method->ftn);
	if (!field)
		FAIL(c, "field");
	const uint32_t attribs = JI_CALL(c->info, VB_FIELD, FI_getFieldAttribs, GetFieldAttribsFn, field, 0);
	if (attribs & FLG_STATIC)
		FAIL(c, "static field");
	CORINFO_CLASS_HANDLE cls = 0;
	const uint32_t type = JI_CALL(c->info, VB_FIELD, FI_getFieldType, GetFieldTypeFn, field, &cls, 0);
	const uint32_t offset = JI_CALL(c->info, VB_FIELD, FI_getFieldOffset, GetFieldOffsetFn, field);
	if (offset > 0x7FF0)
		FAIL(c, "field offset");
	uint32_t size = 0;
	if (type == CORINFO_TYPE_VALUECLASS && op != 0x7C && !value_type(c, cls, &size))
		return false;
	if (op == 0x7C) { // ldflda
		if (c->stack[c->depth - 1] == K_STRUCT)
			FAIL(c, "ldflda of a value on the stack");
		const enum Kind base = load_entry(c, 0, 11, 0);
		if (base != K_REF && base != K_BYREF && base != K_I4)
			FAIL(c, "ldflda base");
		emit(c, ppc_addi(11, 11, (int)offset));
		return pop(c, 1) && store_push(c, base == K_I4 ? K_I4 : K_BYREF, 11, 0);
	}
	const enum Kind kind = size ? K_STRUCT : kind_of(type);
	if (kind == K_NONE)
		FAIL(c, "field type");
	if (op == 0x7B) { // ldfld: from an object or a pointer, or from a value type on the stack
		const enum Kind base = c->stack[c->depth - 1];
		int src = 11;
		int32_t src_off = (int32_t)offset;
		if (base == K_STRUCT && c->leaf) { // a leaf's value type is in a register: the field from its bits
			if (kind == K_STRUCT)
				FAIL(c, LEAF_WHY("value type field of a value type"));
			const int from = entry_register(c, c->depth - 1);
			const uint32_t from_size = c->stack_size[c->depth - 1];
			if (!pop(c, 1))
				return false;
			const int d = result_register(c, kind, 11);
			return extract_field(c, type, d, from, from_size, offset) && store_push(c, kind, d, 0);
		}
		// A value type the runtime reported as its one primitive field (primitive_value_type): the field is the
		// value itself, where it is. A long or a double can't be a pointer; an int is the value only by its class.
		if (base == K_I8 || base == K_R8 || (base == K_I4 && c->stack_cls[c->depth - 1])) {
			if (offset || kind != base)
				FAIL(c, fail_type(c, "field of a value type held as a primitive", type, cls));
			c->stack_cls[c->depth - 1] = primitive_value_type(c, type, cls);
			return true;
		}
		if (base == K_STRUCT) {
			src = c->fp;
			src_off += entry_slot(c, 0);
		} else {
			if (base != K_REF && base != K_BYREF && base != K_I4)
				FAIL(c, fail_type(c, "ldfld base of kind", base, 0));
			src = operand(c, 0, 11);
			if (base == K_REF && !null_check(c, src))
				return false;
		}
		if (kind == K_STRUCT) {
			// A value type field of a value type on the stack comes from its own slot, at or above the result's.
			if (!pop(c, 1) || !push_struct(c, cls, size))
				return false;
			if (c->leaf) { // into the entry's register (push_struct took only 4 or 8 bytes)
				if (size == 8 && (src_off & 3)) // ld's offset is a multiple of 4 (a struct of shorts may not be)
					FAIL(c, LEAF_WHY("value type field alignment"));
				const int d = entry_register(c, c->depth - 1);
				emit(c, size == 8 ? ppc_ld(d, src_off, src) : ppc_lwz(d, src_off, src));
				return true;
			}
			copy_words(c, c->fp, entry_slot(c, 0), src, src_off, size);
			return true;
		}
		if (!pop(c, 1))
			return false;
		const int d = result_register(c, kind, 11), fd = kind == K_R8 ? fresult_register(c, 0) : 0;
		load_typed_to(c, type, d, fd, src_off, src);
		if (!store_push(c, kind, d, fd))
			return false;
		c->stack_cls[c->depth - 1] = primitive_value_type(c, type, cls);
		return true;
	}
	// stfld: [object, value]
	const enum Kind base = c->stack[c->depth - 2];
	if (base != K_REF && base != K_BYREF && base != K_I4)
		FAIL(c, "stfld base");
	if ((kind == K_STRUCT) != (c->stack[c->depth - 1] == K_STRUCT))
		FAIL(c, "stfld value");
	// The write barrier takes the destination in r3 and the value in r4 or a register of its own: entries below
	// the operands still in r3 and r4 (entries 0 and 1) are written back first.
	for (uint32_t i = 0; kind == K_REF && !c->leaf && i < 2 && i + 2 < c->depth; i++)
		if (c->cached[i] && !c->alias[i]) {
			store_entry_slot(c, i, entry_register(c, i), 0);
			c->cached[i] = false;
		}
	// The object and the value from the registers holding them, if they are (operand).
	const int obj = operand(c, 1, 11);
	if (base == K_REF && !null_check(c, obj))
		return false;
	if (kind == K_STRUCT)
		return copy_struct(c, obj, (int32_t)offset, c->fp, entry_slot(c, 0), size, c->stack_refs[c->depth - 1]) &&
			pop(c, 2);
	if (kind == K_REF) {
		const int value = operand(c, 0, 4);
		emit(c, ppc_lwz(0, 0, obj)); // null check in this method rather than in the helper
		emit(c, ppc_addi(3, obj, (int)offset));
		if (!pop(c, 2))
			return false;
		return write_barrier(c, base == K_REF, value);
	}
	if (kind == K_R8)
		store_typed_from(c, type, 0, foperand(c, 0, 0), (int)offset, obj);
	else
		store_typed(c, type, operand(c, 0, 12), (int)offset, obj);
	return pop(c, 2);
}

static bool op_load_indirect(Ctx *c, uint32_t type);
static bool op_store_indirect(Ctx *c, uint32_t type);
static bool generic_context(const Ctx *c);
static CORINFO_CLASS_HANDLE resolve_class(Ctx *c, uint32_t token);
static CORINFO_CLASS_HANDLE resolve_class_as(Ctx *c, uint32_t token, uint32_t kind);
static uint32_t class_type(Ctx *c, CORINFO_CLASS_HANDLE cls);

// The CorInfoType ldobj, stobj and initobj treat a class token's type as: a value type, a reference, or a primitive.
static uint32_t object_type(Ctx *c, CORINFO_CLASS_HANDLE cls)
{
	const uint32_t t = class_type(c, cls);
	return t == CORINFO_TYPE_STRING ? CORINFO_TYPE_CLASS : t;
}

static bool op_ldobj(Ctx *c, uint32_t token)
{
	CORINFO_CLASS_HANDLE cls = resolve_class(c, token);
	if (!cls)
		FAIL_UNLESS_SET(c, "ldobj class");
	const uint32_t t = object_type(c, cls);
	if (t != CORINFO_TYPE_VALUECLASS && t != CORINFO_TYPE_REFANY)
		return op_load_indirect(c, t);
	uint32_t size = 8;
	if (t == CORINFO_TYPE_VALUECLASS && !value_type(c, cls, &size))
		return false;
	const enum Kind address = load_entry(c, 0, 11, 0);
	if (address != K_BYREF && address != K_I4)
		FAIL(c, "ldobj address");
	if (!pop(c, 1) || !(t == CORINFO_TYPE_REFANY ? push_refany(c) : push_struct(c, cls, size)))
		return false;
	copy_words(c, c->fp, entry_slot(c, 0), 11, 0, size);
	return true;
}

static bool op_stobj(Ctx *c, uint32_t token)
{
	CORINFO_CLASS_HANDLE cls = resolve_class(c, token);
	if (!cls)
		FAIL_UNLESS_SET(c, "stobj class");
	const uint32_t t = object_type(c, cls);
	if (t != CORINFO_TYPE_VALUECLASS && t != CORINFO_TYPE_REFANY)
		return op_store_indirect(c, t);
	if (c->stack[c->depth - 1] != K_STRUCT)
		FAIL(c, "stobj value");
	const enum Kind address = load_entry(c, 1, 11, 0);
	if (address != K_BYREF && address != K_I4)
		FAIL(c, "stobj address");
	return copy_struct(c, 11, 0, c->fp, entry_slot(c, 0), c->stack_size[c->depth - 1], c->stack_refs[c->depth - 1]) &&
		pop(c, 2);
}

// cpblk (destination, source, size) and initblk (address, value, size) as byte loops: the marshaling stubs' copies
// and zeroing, whose sizes are small.
static bool op_block(Ctx *c, bool copy)
{
	load_entry(c, 2, 11, 0);
	load_entry(c, 1, 12, 0);
	load_entry(c, 0, 10, 0);
	if (!pop(c, 3))
		return false;
	emit(c, ppc_cmpwi(6, 10, 0));
	const uint32_t skip = c->n;
	emit(c, 0);
	emit(c, ppc_mtctr(10));
	const uint32_t loop = c->n;
	if (copy) {
		emit(c, ppc_lbz(0, 0, 12));
		emit(c, ppc_addi(12, 12, 1));
		emit(c, ppc_stb(0, 0, 11));
	} else {
		emit(c, ppc_stb(12, 0, 11));
	}
	emit(c, ppc_addi(11, 11, 1));
	emit(c, ppc_bdnz(-(int32_t)(c->n - loop) * 4));
	c->code[skip] = ppc_bc(BO_TRUE, 6 * 4 + CR_EQ, (int32_t)(c->n - skip) * 4);
	return true;
}

static bool op_initobj(Ctx *c, uint32_t token)
{
	CORINFO_CLASS_HANDLE cls = resolve_class(c, token);
	if (!cls)
		FAIL_UNLESS_SET(c, "initobj class");
	const enum Kind address = load_entry(c, 0, 11, 0);
	if (address != K_BYREF && address != K_I4)
		FAIL(c, "initobj address");
	const uint32_t t = object_type(c, cls);
	uint32_t size = 4;
	if (t == CORINFO_TYPE_VALUECLASS) {
		if (!large_local(c, cls, &size) && !value_type(c, cls, &size))
			return false;
	} else if (kind_of(t) == K_I8 || t == CORINFO_TYPE_DOUBLE || t == CORINFO_TYPE_REFANY) {
		size = 8;
	} else if (t == CORINFO_TYPE_BOOL || t == CORINFO_TYPE_BYTE || t == CORINFO_TYPE_UBYTE) {
		emit(c, ppc_li(0, 0));
		emit(c, ppc_stb(0, 0, 11));
		return pop(c, 1);
	} else if (t == CORINFO_TYPE_CHAR || t == CORINFO_TYPE_SHORT || t == CORINFO_TYPE_USHORT) {
		emit(c, ppc_li(0, 0));
		emit(c, ppc_sth(0, 0, 11));
		return pop(c, 1);
	} else if (kind_of(t) != K_REF && t != CORINFO_TYPE_INT && t != CORINFO_TYPE_UINT && t != CORINFO_TYPE_FLOAT &&
		t != CORINFO_TYPE_NATIVEINT && t != CORINFO_TYPE_NATIVEUINT && t != CORINFO_TYPE_VAR) {
		FAIL(c, fail_type(c, "initobj type", t, cls));
	}
	zero_words(c, 11, 0, size);
	return pop(c, 1);
}

static bool op_load_indirect(Ctx *c, uint32_t type)
{
	const enum Kind address = (enum Kind)c->stack[c->depth - 1];
	if (address != K_BYREF && address != K_I4)
		FAIL(c, "ldind address");
	const int a = operand(c, 0, 11);
	if (!pop(c, 1))
		return false;
	const enum Kind kind = kind_of(type);
	const int d = result_register(c, kind, 11), fd = kind == K_R8 ? fresult_register(c, 0) : 0;
	load_typed_to(c, type, d, fd, 0, a);
	return store_push(c, kind, d, fd);
}

static bool op_store_indirect(Ctx *c, uint32_t type)
{
	if (c->stack[c->depth - 1] == K_STRUCT)
		FAIL(c, "stind of a value type");
	const enum Kind address = (enum Kind)c->stack[c->depth - 2];
	if (address != K_BYREF && address != K_I4)
		FAIL(c, "stind address");
	if (kind_of(type) == K_REF) {
		load_entry(c, 1, 3, 0);
		load_entry(c, 0, 4, 0);
		if (!pop(c, 2))
			return false;
		return write_barrier(c, false, 4);
	}
	const int a = operand(c, 1, 11);
	if (kind_of(type) == K_R8)
		store_typed_from(c, type, 0, foperand(c, 0, 0), 0, a);
	else
		store_typed(c, type, operand(c, 0, 12), 0, a);
	return pop(c, 2);
}

// Loads the pending call's arguments (count entries, the first is `this` if any) as the prolog takes them: from
// argument register first_slot (r3 + first_slot) on, a floating-point one in the next of f1-f13 as well, a value
// type in its 8-byte chunks.
static const char FAIL_OUTGOING[] = "outgoing area";

// A parallel move of n registers (f-registers if fp): each register is written only once no pending move still
// reads it; a cycle goes through r0 (f0). src and dst are clobbered.
static void parallel_move(Ctx *c, bool fp, int *src, int *dst, int n)
{
	while (n > 0) {
		int pick = -1;
		for (int m = 0; m < n && pick < 0; m++) {
			bool read = false;
			for (int o = 0; o < n; o++)
				if (o != m && src[o] == dst[m])
					read = true;
			if (!read)
				pick = m;
		}
		if (pick < 0) {
			emit(c, fp ? ppc_fmr(0, src[0]) : ppc_mr(0, src[0]));
			src[0] = 0;
			continue;
		}
		if (src[pick] != dst[pick])
			emit(c, fp ? ppc_fmr(dst[pick], src[pick]) : ppc_mr(dst[pick], src[pick]));
		src[pick] = src[n - 1];
		dst[pick] = dst[n - 1];
		n--;
	}
}

static bool load_arguments(Ctx *c, uint32_t count, int first_slot)
{
	int slot = first_slot, fpr = 1;
	// The slots this call takes beyond r3-r10 must fit the frame's outgoing area; when they don't, the method is
	// compiled again with an area that large (a funclet's frame is fixed, so a call there keeps to the registers).
	uint32_t needed = (uint32_t)first_slot;
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t index = c->depth - count + i;
		needed += c->stack[index] == K_STRUCT ? struct_slots(c->stack_size[index]) : 1;
	}
	if (needed > ARG_SLOTS) {
		const uint32_t have = (uint32_t)(c->outgoing - LINKAGE_BYTES) / 8;
		if (needed - ARG_SLOTS > MAX_STACK_ARG_SLOTS)
			FAIL(c, "arguments on the stack");
		if (c->funclet)
			FAIL(c, "stack arguments in a handler");
		if (needed - ARG_SLOTS > have) {
			if (needed - ARG_SLOTS > c->need_stack_slots)
				c->need_stack_slots = needed - ARG_SLOTS;
			FAIL(c, FAIL_OUTGOING);
		}
	}
	// Arguments still in their cache registers (a plain call after simple instructions) go to their argument
	// registers, or their outgoing slots, from there; the entries under them are written back, as they live on
	// across the call, except those naming a variable's callee-saved register, which the call preserves (and the
	// GC info reports, updating it).
	bool moved[MAX_STACK];
	memset(moved, 0, sizeof moved);
	for (uint32_t i = 0; i < c->depth - count; i++)
		if (c->cached[i] && !c->alias[i]) {
			const int reg = entry_register(c, i);
			store_entry_slot(c, i, reg, reg);
			c->cached[i] = false;
			c->alias[i] = 0;
		}
	{
		int src[2][MAX_STACK], dst[2][MAX_STACK], moves[2] = {0, 0};
		int s = first_slot, f = 1;
		for (uint32_t i = 0; i < count; i++) {
			const uint32_t index = c->depth - count + i;
			const enum Kind k = c->stack[index];
			const int width = k == K_STRUCT ? (int)struct_slots(c->stack_size[index]) : 1;
			if (c->cached[index]) {
				const int reg = entry_register(c, index);
				const int fp = k == K_R8;
				if (s >= ARG_SLOTS) {
					const int32_t out = LINKAGE_BYTES + 8 * (s - ARG_SLOTS);
					emit(c, fp ? ppc_stfd(reg, out, 1) : k == K_I8 ? ppc_std(reg, out, 1) : ppc_stw(reg, out + 4, 1));
				}
				if (fp ? f <= 13 : s < ARG_SLOTS) {
					src[fp][moves[fp]] = reg;
					dst[fp][moves[fp]++] = fp ? f : 3 + s;
				}
				moved[i] = true;
				c->cached[index] = false; // the entry is popped after the call; its slot isn't reported meanwhile
				c->alias[index] = 0;
				c->stale[index] = true;
			}
			if (k == K_R8)
				f++;
			s += width;
		}
		for (int fp = 0; fp < 2; fp++)
			parallel_move(c, fp, src[fp], dst[fp], moves[fp]);
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t index = c->depth - count + i;
		const enum Kind k = c->stack[index];
		const int r = 3 + slot;
		const int32_t at = slot_of(c, index, k);
		if (moved[i]) {
			if (k == K_R8)
				fpr++;
			slot += k == K_STRUCT ? (int)struct_slots(c->stack_size[index]) : 1;
			continue;
		}
		if (k == K_R8) {
			// In f1-f13 whatever the slot; with a slot beyond r10 (the ninth argument on) the value goes into the
			// outgoing area as well, which a callee may read instead (the ABI allows either).
			if (slot >= ARG_SLOTS + MAX_STACK_ARG_SLOTS)
				FAIL(c, "arguments on the stack");
			// The fourteenth double on (Matrix3D's constructor) lives in its slot alone.
			const int freg = fpr <= 13 ? fpr : 0;
			emit(c, ppc_lfd(freg, at, c->fp));
			if (slot >= ARG_SLOTS)
				emit(c, ppc_stfd(freg, LINKAGE_BYTES + 8 * (slot - ARG_SLOTS), 1));
			fpr++, slot++;
			continue;
		}
		if (k == K_STRUCT) {
			const uint32_t size = c->stack_size[index];
			if (slot + (int)struct_slots(size) > ARG_SLOTS + MAX_STACK_ARG_SLOTS)
				FAIL(c, "arguments on the stack");
			for (uint32_t off = 0; off < size; off += 8) {
				const int chunk = r + (int)(off / 8);
				const int32_t out = LINKAGE_BYTES + 8 * (slot + (int)(off / 8) - ARG_SLOTS);
				if (slot + (int)(off / 8) >= ARG_SLOTS) {
					if (size <= 4) {
						emit(c, ppc_lwz(0, at, c->fp));
						emit(c, ppc_stw(0, out + 4, 1));
					} else if (size - off >= 8) {
						emit(c, ppc_ld(0, at + (int32_t)off, c->fp));
						emit(c, ppc_std(0, out, 1));
					} else {
						emit(c, ppc_lwz(0, at + (int32_t)off, c->fp));
						emit(c, ppc_stw(0, out, 1));
					}
				} else if (size <= 4) {
					emit(c, ppc_lwz(chunk, at, c->fp));
				} else if (size - off >= 8) {
					emit(c, ppc_ld(chunk, at + (int32_t)off, c->fp));
				} else {
					emit(c, ppc_lwz(chunk, at + (int32_t)off, c->fp));
					emit(c, ppc_sldi32(chunk, chunk));
				}
			}
			slot += (int)struct_slots(size);
			continue;
		}
		if (slot >= ARG_SLOTS + MAX_STACK_ARG_SLOTS)
			FAIL(c, "arguments on the stack");
		if (slot >= ARG_SLOTS) {
			// The outgoing area is at the stack pointer, not the frame register: after a localloc r1 is below r31
			// (PopulateEvents passed its list through a localloc'd block, and List.Add got stack words).
			const int32_t out = LINKAGE_BYTES + 8 * (slot - ARG_SLOTS);
			load_entry(c, count - 1 - i, 0, 0);
			emit(c, k == K_I8 ? ppc_std(0, out, 1) : ppc_stw(0, out + 4, 1));
			slot++;
			continue;
		}
		load_entry(c, count - 1 - i, r, 0);
		slot++;
	}
	return true;
}

// For a call returning a value type through a buffer: the buffer is the result's own stack slot (the first
// argument's index), passed in r3.
static void pass_return_buffer(Ctx *c, uint32_t count)
{
	emit(c, ppc_addi(3, c->fp, slot_of(c, c->depth - count, K_STRUCT)));
}

static bool push_result(Ctx *c, const CORINFO_SIG_INFO *sig)
{
	const uint32_t t = sig_ret_type(sig);
	if (t == CORINFO_TYPE_VOID)
		return true;
	if (t == CORINFO_TYPE_REFANY) {
		if (!push_refany(c))
			return false;
		emit(c, ppc_std(3, entry_slot(c, 0), c->fp));
		return true;
	}
	if (t == CORINFO_TYPE_VALUECLASS) {
		uint32_t size;
		if (!value_type(c, sig->retTypeClass, &size) || !push_struct(c, sig->retTypeClass, size))
			return false;
		if (size <= 8)
			emit(c, size == 4 ? ppc_stw(3, entry_slot(c, 0), c->fp) : ppc_std(3, entry_slot(c, 0), c->fp));
		return true;
	}
	const enum Kind k = kind_of(t);
	if (k == K_NONE)
		FAIL(c, "result type");
	switch (t) {
	case CORINFO_TYPE_BOOL:
	case CORINFO_TYPE_UBYTE: emit(c, ppc_clrlwi(3, 3, 24)); break;
	case CORINFO_TYPE_BYTE: emit(c, ppc_extsb(3, 3)); break;
	case CORINFO_TYPE_CHAR:
	case CORINFO_TYPE_USHORT: emit(c, ppc_clrlwi(3, 3, 16)); break;
	case CORINFO_TYPE_SHORT: emit(c, ppc_extsh(3, 3)); break;
	default: break;
	}
	if (!store_push(c, k, 3, 1))
		return false;
	c->stack_cls[c->depth - 1] = primitive_value_type(c, t, sig->retTypeClass);
	return true;
}

static bool check_signature(Ctx *c, const CORINFO_SIG_INFO *sig)
{
	const uint32_t conv = sig_call_conv(sig);
	// A method of an instantiated generic class or a generic method instantiation is fine to call (its code finds
	// the instantiation through this, or an instantiating stub supplies it); one that takes the instantiation as a
	// hidden argument of its own isn't handled.
	if ((conv & CORINFO_CALLCONV_MASK) != CORINFO_CALLCONV_DEFAULT ||
		(conv & (CORINFO_CALLCONV_PARAMTYPE | CORINFO_CALLCONV_EXPLICITTHIS)))
		FAIL(c, "callee signature");
	uint32_t size;
	return return_info(c, sig, &size);
}

// Whether a call's result comes back through a buffer.
static bool returns_buffer(Ctx *c, const CORINFO_SIG_INFO *sig)
{
	uint32_t size;
	return return_info(c, sig, &size) && size > 8;
}

static bool load_class_handle(Ctx *c, int r, CORINFO_CLASS_HANDLE cls);

// Runs the class constructor of `cls` if it hasn't run: JIT_InitClass(class). A method whose class needs it calls
// this in its prolog, and a static field access for the field's class, so callers of a method needn't.
static bool init_class(Ctx *c, CORINFO_CLASS_HANDLE cls)
{
	return load_class_handle(c, 3, cls) && call_helper(c, HELP_INITCLASS);
}

// The exact type that declares a method token's method (resolveToken's class). getMethodClass gives the method's own
// class, which for an instantiation over reference types is the shared canonical one (IEnumerable<__Canon>,
// EventHandler<__Canon>): wrong for anything that checks the type, from interface dispatch to an allocation.
static CORINFO_CLASS_HANDLE exact_owner(Ctx *c, uint32_t token)
{
	ResolvedToken resolved;
	memset(&resolved, 0, sizeof resolved);
	resolved.context = c->method->ftn;
	resolved.scope = c->method->scope;
	resolved.token = token;
	resolved.kind = TOKENKIND_METHOD;
	JI_CALL(c->info, VB_MODULE, MOD_resolveToken, ResolveTokenFn, &resolved);
	return resolved.cls; // canonical in shared generic code: load_owner_of_ctor gets the exact one
}

// JIT_VirtualFunctionPointer(object in r3, type, method) finds the implementation through a cache and leaves its
// address in r3. The type and method must be the exact ones: with the canonical IEqualityComparer<__Canon> of
// shared code the helper doesn't find them in the object's interface map and raises (the first exception of
// NGenBench on our mscorlib, from Dictionary<string, object>.Insert in AppDomain.Setup, whose throwable couldn't
// even be built). Microsoft's shared Dictionary<__Canon, __Canon>.Insert (mscorlib.ni.dll 0x1DC9C4) reads both from
// this's generic dictionary (slots 0x38 and 0x3C, filled by helper 395 when null) before the resolver: the runtime
// lookups embedGenericHandle describes. The object is entry `from_top` of the stack, loaded after the lookups,
// which call helpers.
static bool virtual_function_pointer(Ctx *c, CORINFO_METHOD_HANDLE callee, uint32_t token, uint32_t from_top)
{
	// The method token's owner, exact: embedGenericHandle embeds what a token names, so in shared code the
	// MemberRef's parent (a TypeSpec, IEqualityComparer<TKey>) is read from the metadata and looked up as a class,
	// as load_owner_of_ctor does. (Asking it with TOKENKIND_NEWOBJ gave the method's own lookup twice, and the
	// mscorlib compile hung.)
	CORINFO_CLASS_HANDLE cls = exact_owner(c, token);
	if (!cls)
		FAIL(c, "method's type");
	const uint32_t parent = generic_context(c) ? memberref_parent(c, token) : 0;
	if ((parent >> 24) == 0x1B) {
		CORINFO_CLASS_HANDLE exact;
		if (!load_class_of_token(c, 11, parent, TOKENKIND_CLASS, &exact))
			return false;
	} else if (!load_class_handle(c, 11, cls)) {
		return false;
	}
	emit(c, ppc_stw(11, c->inst_temp, c->fp));
	if (generic_context(c)) {
		GenericHandleResult g;
		memset(&g, 0, sizeof g);
		JI_CALL(c->info, VB_DYNAMIC, DYN_embedGenericHandle, EmbedGenericHandleFn, c->method->scope, token,
			c->method->ftn, TOKENKIND_METHOD, &g);
		if (g.needs_runtime_lookup) {
			if (!runtime_lookup(c, 11, &g))
				return false;
		} else if (g.constant.access == IAT_VALUE && g.constant.addr) {
			load_address(c, 11, g.constant.addr);
		} else if (g.constant.access == IAT_PVALUE && g.constant.addr) {
			load_cell(c, 11, g.constant.addr);
		} else {
			FAIL(c, fail_type(c, "interface method handle access", g.constant.access, cls));
		}
	} else {
		void *indirection = 0;
		CORINFO_METHOD_HANDLE handle = JI_CALL(c->info, VB_DYNAMIC, DYN_embedMethodHandle, EmbedMethodHandleFn,
			callee, &indirection);
		if (handle)
			load_address(c, 11, handle);
		else if (indirection)
			load_cell(c, 11, indirection);
		else
			FAIL(c, "method handle");
	}
	emit(c, ppc_stw(11, c->scratch + 4, c->fp)); // not a GC slot (a MethodDesc); the lookups are done with scratch
	load_entry(c, from_top, 3, 0);
	if (!null_check(c, 3))
		return false;
	emit(c, ppc_lwz(0, 0, 3)); // the helper reads the method table: fault here instead, in managed code
	emit(c, ppc_lwz(4, c->inst_temp, c->fp));
	emit(c, ppc_lwz(5, c->scratch + 4, c->fp));
	return call_helper(c, HELP_VIRTUAL_FUNC_PTR);
}

// An interface call, as Microsoft's compiled code makes it (PlexFoundation.ni.dll 0x94ADFAE4, IDisposable.Dispose):
// the implementation's address from JIT_VirtualFunctionPointer, then a call there, the target waiting in the scratch
// slot while the arguments load.
static bool op_interface_call(Ctx *c, CORINFO_METHOD_HANDLE callee, uint32_t token, uint32_t count, bool buffer)
{
	if (!virtual_function_pointer(c, callee, token, count - 1))
		return false;
	emit(c, ppc_stw(3, c->scratch, c->fp));
	if (!load_arguments(c, count, buffer ? 1 : 0))
		return false;
	if (buffer)
		pass_return_buffer(c, count);
	emit(c, ppc_lwz(12, c->scratch, c->fp));
	emit(c, ppc_mtctr(12));
	emit(c, PPC_BCTRL);
	safe_point(c);
	return true;
}

// A call to a generic method instantiation (a MethodSpec token, Get<string>) resolves to the instantiation's
// MethodDesc: over value types its own code, over reference types an instantiating stub that supplies the hidden
// instantiation argument to the shared code. Either is called like any method, through a cell (call_method), so the
// zapper doesn't compile the instantiation into this image on our account. The instantiation's own body, when the
// zapper asks for it, is shared code that check_signature declines.
// ldtoken of a type, as typeof(T) compiles to: `ldtoken T; call Type::GetTypeFromHandle(RuntimeTypeHandle)`. The
// RuntimeTypeHandle is a 4-byte value type whose one field is the type's RuntimeType object, which helper 400
// (JIT_GetRuntimeType: the MethodTable's exposed class object, made on first use) gives for the embedded type
// handle; its class comes from the following call's signature, so only that pattern is handled.
// ldtoken of a type (typeof(T): Type.GetTypeFromHandle(RuntimeTypeHandle)) or of a field (an array initializer:
// RuntimeHelpers.InitializeArray(array, RuntimeFieldHandle)). Both handle structs hold an object the runtime makes
// from the handle (the RuntimeType, the field's stub), so the token's handle goes through a helper; the struct's
// class is read from the call that follows: its first argument for a type, its last for a field.
// The runtime handle struct (RuntimeTypeHandle, RuntimeFieldHandle: 4 bytes, the runtime object) that the static
// call at IL `at` takes as its first argument (its last with `last`): the class of what ldtoken and refanytype push,
// which no other call names.
static bool handle_argument_class(Ctx *c, uint32_t at, bool last, CORINFO_CLASS_HANDLE *handle_class)
{
	if (at + 5 > c->il_size || c->il[at] != 0x28)
		FAIL(c, "handle not passed to a call");
	CORINFO_METHOD_HANDLE callee = JI_CALL(c->info, VB_MODULE, MOD_findMethod_, FindMethodFn, c->method->scope,
		il_u32(c, at + 1), c->method->ftn, 0);
	if (!callee)
		FAIL(c, "handle callee");
	CORINFO_SIG_INFO sig;
	memset(&sig, 0, sizeof sig);
	JI_CALL(c->info, VB_METHOD, MI_getMethodSig, GetMethodSigFn, callee, &sig, 0);
	*handle_class = 0;
	if (sig_num_args(&sig) < 1 || (sig_call_conv(&sig) & CORINFO_CALLCONV_HASTHIS))
		FAIL(c, "handle call");
	CORINFO_ARG_LIST_HANDLE arg = sig.args;
	if (last)
		for (uint32_t i = 1; i < sig_num_args(&sig); i++)
			arg = JI_CALL(c->info, VB_ARG, ARG_getArgNext, GetArgNextFn, arg);
	const uint32_t t = JI_CALL(c->info, VB_ARG, ARG_getArgType, GetArgTypeFn, &sig, arg, handle_class) & 0x3F;
	uint32_t size;
	if (t != CORINFO_TYPE_VALUECLASS)
		FAIL(c, "handle type");
	if (!value_type(c, *handle_class, &size)) // keeps its reason: a retry for stack room
		return false;
	if (size != 4)
		FAIL(c, "handle type");
	return true;
}

// refanytype: a TypedReference's type as a RuntimeTypeHandle, whose one field is the RuntimeType object (helper
// 400 for the type handle in the TypedReference's second word; a null type stays null). TypedReference.GetHashCode
// passes it to Type.GetTypeFromHandle.
static bool op_refanytype(Ctx *c, uint32_t next)
{
	if (!c->depth || c->stack[c->depth - 1] != K_STRUCT || c->stack_size[c->depth - 1] != 8)
		FAIL(c, "refanytype of a non-TypedReference");
	CORINFO_CLASS_HANDLE handle_class;
	if (!handle_argument_class(c, next, false, &handle_class))
		return false;
	emit(c, ppc_lwz(3, entry_slot(c, 0) + 4, c->fp));
	if (!pop(c, 1))
		return false;
	emit(c, ppc_cmpwi(6, 3, 0));
	const uint32_t skip = c->n;
	emit(c, 0);
	if (!call_helper(c, HELP_GET_RUNTIME_TYPE))
		return false;
	c->code[skip] = ppc_bc(BO_TRUE, 6 * 4 + CR_EQ, (int32_t)(c->n - skip) * 4);
	if (!push_struct(c, handle_class, 4))
		return false;
	emit(c, ppc_stw(3, entry_slot(c, 0), c->fp));
	return true;
}

static bool op_ldtoken(Ctx *c, uint32_t at, uint32_t token)
{
	const uint32_t type = token & 0xFF000000u;
	const bool field_token = type == 0x04000000u || type == 0x0A000000u;
	if (!field_token && type != 0x02000000u && type != 0x01000000u && type != 0x1B000000u)
		FAIL(c, "ldtoken of a method");
	if (at + 10 > c->il_size || c->il[at + 5] != 0x28)
		FAIL(c, "ldtoken not followed by a call");
	CORINFO_CLASS_HANDLE cls = 0;
	CORINFO_FIELD_HANDLE field = 0;
	if (field_token) {
		if (generic_context(c))
			FAIL(c, "ldtoken of a field from a generic context");
		field = JI_CALL(c->info, VB_MODULE, MOD_findField, FindFieldFn, c->method->scope, token, c->method->ftn);
		if (!field)
			FAIL(c, "ldtoken of a method"); // a MemberRef to a method
	} else {
		cls = resolve_class_any(c, token, TOKENKIND_LDTOKEN);
		if (!cls)
			FAIL_UNLESS_SET(c, "ldtoken class");
	}
	CORINFO_CLASS_HANDLE handle_class;
	if (!handle_argument_class(c, at + 5, field_token, &handle_class))
		return false;
	const uint32_t size = 4;
	if (field_token) {
		void *indirection = 0;
		CORINFO_FIELD_HANDLE handle = JI_CALL(c->info, VB_DYNAMIC, DYN_embedFieldHandle, EmbedFieldHandleFn, field,
			&indirection);
		if (handle)
			load_address(c, 3, handle);
		else if (indirection)
			load_cell(c, 3, indirection);
		else
			FAIL(c, "field handle");
		if (!call_helper(c, HELP_FIELDDESC_TO_STUBRUNTIMEFIELD))
			return false;
	} else if (!load_class_of_token(c, 3, token, TOKENKIND_LDTOKEN, &cls) || !call_helper(c, HELP_GET_RUNTIME_TYPE)) {
		return false;
	}
	if (!push_struct(c, handle_class, size))
		return false;
	emit(c, ppc_stw(3, entry_slot(c, 0), c->fp));
	return true;
}

// Type.GetTypeFromHandle(RuntimeTypeHandle): the handle's one field is the RuntimeType object itself, so the
// result is that word, as Microsoft's compiler treats it (its typeof(X) is JIT_GetRuntimeType and a store, no
// call). Calling the method instead (an internal call) ended in PseudoCustomAttribute's class constructor failing
// with no fault and no managed throw: the runtime's cast of what came back to RuntimeType threw, and building that
// exception's message walked back into the class being initialized.
static bool op_type_from_handle(Ctx *c)
{
	if (c->depth < 1 || c->stack[c->depth - 1] != K_STRUCT || c->stack_size[c->depth - 1] != 4)
		FAIL(c, "GetTypeFromHandle argument");
	emit(c, ppc_lwz(3, entry_slot(c, 0), c->fp));
	return pop(c, 1) && store_push(c, K_REF, 3, 0);
}

// Microsoft.Xbox.FPU: internal calls with no implementation in the runtime, each one PowerPC floating-point
// instruction the compiler must emit itself (the runtime's ECall lookup faults on them). fsel(a, b, c) is
// a >= 0 ? b : c; fpmin/fpmax are a subtraction and a select; the conversions leave the integer bits in the
// double they return (fcfid takes them back); frnd is frin. The single-precision ones round their result.
static bool op_fpu(Ctx *c, const char *name)
{
	static const struct { const char *name; uint8_t args; bool single; } ops[] = {
		{"fsqrt", 1, false}, {"fsqrts", 1, true}, {"frsqrte", 1, false}, {"fres", 1, true}, {"fabs", 1, false},
		{"fsel", 3, false}, {"fself", 3, true}, {"fpmin", 2, false}, {"fpmax", 2, false}, {"fctiwz", 1, false},
		{"fctidz", 1, false}, {"fctiw", 1, false}, {"fctid", 1, false}, {"fcfid", 1, false}, {"frnd", 1, false},
	};
	const uint32_t count = sizeof ops / sizeof ops[0];
	uint32_t i = 0;
	while (i < count && strcmp(ops[i].name, name))
		i++;
	if (i == count)
		FAIL(c, "FPU intrinsic");
	const uint32_t args = ops[i].args;
	if (c->depth < args)
		FAIL(c, "stack underflow");
	bool single = ops[i].single;
	for (uint32_t a = 0; a < args; a++) {
		if (c->stack[c->depth - 1 - a] != K_R8)
			FAIL(c, "FPU operand");
		load_entry(c, a, 0, (int)(args - a)); // the first argument in f1
	}
	if (!strcmp(name, "fsqrt")) emit(c, ppc_fsqrt(0, 1));
	else if (!strcmp(name, "fsqrts")) emit(c, ppc_fsqrts(0, 1));
	else if (!strcmp(name, "frsqrte")) emit(c, ppc_frsqrte(0, 1));
	else if (!strcmp(name, "fres")) emit(c, ppc_fres(0, 1));
	else if (!strcmp(name, "fabs")) emit(c, ppc_fabs(0, 1));
	else if (!strcmp(name, "fsel") || !strcmp(name, "fself")) emit(c, ppc_fsel(0, 1, 3, 2));
	else if (!strcmp(name, "fpmin")) {
		emit(c, ppc_fsub(0, 1, 2));
		emit(c, ppc_fsel(0, 0, 1, 2));
	} else if (!strcmp(name, "fpmax")) {
		emit(c, ppc_fsub(0, 1, 2));
		emit(c, ppc_fsel(0, 0, 2, 1));
	}
	else if (!strcmp(name, "fctiwz")) emit(c, ppc_fctiwz(0, 1));
	else if (!strcmp(name, "fctidz")) emit(c, ppc_fctidz(0, 1));
	else if (!strcmp(name, "fctiw")) emit(c, ppc_fctiw(0, 1));
	else if (!strcmp(name, "fctid")) emit(c, ppc_fctid(0, 1));
	else if (!strcmp(name, "fcfid")) emit(c, ppc_fcfid(0, 1));
	else emit(c, ppc_frin(0, 1));
	if (single)
		emit(c, ppc_frsp(0, 0));
	return pop(c, args) && store_push(c, K_R8, 0, 0);
}

// An array accessor taking the array's exact type after this (PARAMTYPE): this in r3, the type in r4, the
// arguments from r5.
static bool op_array_inst_call(Ctx *c, CORINFO_METHOD_HANDLE callee, uint32_t token, const CORINFO_SIG_INFO *sig)
{
	const uint32_t count = sig_num_args(sig) + 1;
	if (c->depth < count)
		FAIL(c, "call arguments");
	if (generic_context(c))
		FAIL(c, "array accessor from a generic context");
	if (returns_buffer(c, sig))
		FAIL(c, "array accessor returning a value type");
	CORINFO_CLASS_HANDLE array = exact_owner(c, token);
	if (!array || !load_class_handle(c, 11, array))
		FAIL_UNLESS_SET(c, "array accessor's type");
	emit(c, ppc_stw(11, c->inst_temp, c->fp));
	if (!load_arguments(c, count, 1))
		return false;
	emit(c, ppc_mr(3, 4));
	if (!null_check(c, 3))
		return false;
	emit(c, ppc_lwz(4, c->inst_temp, c->fp));
	return call_method(c, callee, true) && pop(c, count) && push_result(c, sig);
}

static const char FAIL_SECRET[] = "stub context not kept";

static bool op_call(Ctx *c, uint8_t op, uint32_t token)
{
	ICorJitInfo *info = c->info;
	CORINFO_METHOD_HANDLE callee = JI_CALL(info, VB_MODULE, MOD_findMethod_, FindMethodFn, c->method->scope, token,
		c->method->ftn, 0);
	if (!callee)
		FAIL(c, "callee");
	{
		// StubHelpers.GetStubContext is an intrinsic: the stub's secret parameter, never a call (its body loops).
		const char *cn = 0;
		const char *mn = method_name(info, callee, &cn);
		if (mn && cn && !strcmp(mn, "GetStubContext") && !strcmp(cn, "StubHelpers")) {
			if (!c->keep_secret)
				FAIL(c, FAIL_SECRET);
			emit(c, ppc_lwz(11, c->secret, c->fp));
			return store_push(c, K_I4, 11, 0);
		}
		// Only a plain call takes register-held arguments (load_arguments); every other path writes them back first.
		if (mn && cn && !strcmp(cn, "FPU")) {
			flush(c);
			return op_fpu(c, mn);
		}
		if (mn && cn && !strcmp(cn, "Type") && !strcmp(mn, "GetTypeFromHandle")) {
			flush(c);
			return op_type_from_handle(c);
		}
	}
	CORINFO_SIG_INFO sig;
	memset(&sig, 0, sizeof sig);
	JI_CALL(info, VB_METHOD, MI_getMethodSig, GetMethodSigFn, callee, &sig, 0);
	CORINFO_CLASS_HANDLE cls = JI_CALL(info, VB_METHOD, MI_getMethodClass, GetMethodClassFn, callee);
	// A multi-dimensional array's Address (long[,]::Address, BouncyCastle's RijndaelEngine.GenerateWorkingKey; what
	// C# makes of `ref a[i, j]`) takes the array's exact type as a hidden argument, after this.
	const bool array_inst = (sig_call_conv(&sig) & CORINFO_CALLCONV_PARAMTYPE) &&
		(sig_call_conv(&sig) & CORINFO_CALLCONV_HASTHIS) &&
		JI_CALL(info, VB_CLASS, CI_getArrayRank, GetArrayRankFn, cls) != 0;
	if (array_inst) {
		CORINFO_SIG_INFO plain = sig;
		plain.packed &= ~((uint32_t)CORINFO_CALLCONV_PARAMTYPE << 8);
		if (!check_signature(c, &plain))
			return false;
		flush(c);
		return op_array_inst_call(c, callee, token, &sig);
	}
	if (!check_signature(c, &sig))
		return false;
	const uint32_t attribs = JI_CALL(info, VB_METHOD, MI_getMethodAttribs, GetMethodAttribsFn, callee);
	const uint32_t class_attribs = JI_CALL(info, VB_CLASS, CI_getClassAttribs, GetClassAttribsFn, cls);
	const uint32_t class_type = JI_CALL(info, VB_CLASS, CI_asCorInfoType, AsCorInfoTypeFn, cls);
	const bool has_this = (sig_call_conv(&sig) & CORINFO_CALLCONV_HASTHIS) != 0;
	const uint32_t count = sig_num_args(&sig) + (has_this ? 1 : 0);
	if (c->depth < count)
		FAIL(c, "call arguments");
	(void)class_type;
	// A `constrained. T callvirt M` has a pointer to a T as this (ECMA-335 III.2.1): a reference type's is
	// dereferenced; a value type that itself declares M gets the pointer and a direct call; any other value type
	// is boxed and M called on the box (M is then Object's or an interface's, which the box answers; a method T
	// implements but doesn't declare, say a generic instantiation's, is called on the box too, so a change it
	// makes to the value is lost: not the case in the code seen so far, where such calls are direct).
	bool direct = false;
	if (c->constrained) {
		flush(c);
		CORINFO_CLASS_HANDLE constrained = c->constrained;
		c->constrained = 0;
		if (op != 0x6F || !has_this || c->stack[c->depth - count] != K_BYREF)
			FAIL(c, "constrained call");
		const uint32_t ct = JI_CALL(info, VB_CLASS, CI_asCorInfoType, AsCorInfoTypeFn, constrained);
		const uint32_t index = c->depth - count;
		if (ct == CORINFO_TYPE_CLASS || ct == CORINFO_TYPE_STRING) { // an enum reports its underlying type: a value
			load_entry(c, count - 1, 11, 0);
			emit(c, ppc_lwz(11, 0, 11));
			emit(c, ppc_stw(11, slot_of(c, index, K_REF), c->fp));
			c->stack[index] = K_REF;
		} else if (constrained == cls) {
			direct = true;
		} else {
			uint32_t size;
			if (!value_type(c, constrained, &size))
				return false;
			load_entry(c, count - 1, 4, 0);
			if (!load_class_handle(c, 3, constrained) || !call_helper(c, HELP_BOX))
				return false;
			emit(c, ppc_stw(3, slot_of(c, index, K_REF), c->fp));
			c->stack[index] = K_REF;
		}
	}
	const bool buffer = returns_buffer(c, &sig);
	if (class_attribs & FLG_INTERFACE) {
		flush(c);
		return op == 0x6F && has_this && op_interface_call(c, callee, token, count, buffer) && pop(c, count) &&
			push_result(c, &sig);
	}
	// A generic virtual method from exact code (Shape<string>.Describe<double>): no vtable slot holds an
	// instantiation, so the implementation comes from JIT_VirtualFunctionPointer like an interface method's.
	if (op == 0x6F && has_this && !direct && sig.sigInst.methInstCount && (attribs & FLG_VIRTUAL) && !generic_context(c)) {
		flush(c);
		return op_interface_call(c, callee, token, count, buffer) && pop(c, count) && push_result(c, &sig);
	}

	// A generic method, or a static method of a generic class, called from shared code (Pick<T> from Last<T>):
	// the exact callee depends on the caller's instantiation. getCallInfo without ALLOWINSTPARAM answers either a
	// method to call as any other (an instantiating stub, when the instantiation is known here) or a code pointer
	// to look up at run time through the caller's context (kind 1), the entry of the exact instantiation, which
	// takes the arguments as the IL passes them.
	bool inst_callee = false;
	if (generic_context(c) && (sig.sigInst.methInstCount || (sig.sigInst.classInstCount && !has_this)) &&
		!(sig_call_conv(&sig) & CORINFO_CALLCONV_PARAMTYPE)) {
		flush(c);
		CallInfo ci;
		memset(&ci, 0, sizeof ci);
		JI_CALL(info, VB_DYNAMIC, DYN_getCallInfo, GetCallInfoFn, c->method->scope, token, c->method->ftn, 0,
			c->method->ftn, op == 0x6F ? CALLINFO_CALLVIRT : 0, &ci);
		if (ci.kind == 1) {
			if (ci.stub_lookup.needs_runtime_lookup) {
				if (!runtime_lookup(c, 11, &ci.stub_lookup))
					return false;
			} else if (ci.stub_lookup.constant.access == IAT_VALUE && ci.stub_lookup.constant.addr) {
				load_address(c, 11, ci.stub_lookup.constant.addr);
			} else if (ci.stub_lookup.constant.access == IAT_PVALUE && ci.stub_lookup.constant.addr) {
				load_cell(c, 11, ci.stub_lookup.constant.addr);
			} else {
				FAIL(c, fail_type(c, "generic callee code pointer access", ci.stub_lookup.constant.access, cls));
			}
			emit(c, ppc_stw(11, c->inst_temp, c->fp));
			if (!load_arguments(c, count, buffer ? 1 : 0))
				return false;
			if (buffer)
				pass_return_buffer(c, count);
			emit(c, ppc_lwz(12, c->inst_temp, c->fp));
			emit(c, ppc_mtctr(12));
			emit(c, PPC_BCTRL);
			safe_point(c);
			return pop(c, count) && push_result(c, &sig);
		}
		if (ci.kind == 3 && has_this) {
			// A generic virtual method (Iterator<T>.Select<TResult>, CORINFO_VIRTUALCALL_LDVIRTFTN): the
			// implementation for this object and the exact method instantiation from JIT_VirtualFunctionPointer, as
			// an interface call; the token's MethodDesc is the instantiation's (through the generic context's
			// dictionary from shared code), and the class only names the slot.
			return op_interface_call(c, callee, token, count, buffer) && pop(c, count) && push_result(c, &sig);
		}
		if (ci.kind != 0 || !ci.method)
			FAIL(c, fail_type(c, "generic callee call kind", ci.kind, cls));
		if (!load_arguments(c, count, buffer ? 1 : 0))
			return false;
		if (buffer)
			pass_return_buffer(c, count);
		if (!call_method(c, ci.method, true))
			return false;
		return pop(c, count) && push_result(c, &sig);
	}
	if (generic_context(c) && (sig_call_conv(&sig) & CORINFO_CALLCONV_PARAMTYPE)) {
		flush(c);
		if (has_this || buffer)
			FAIL(c, "generic instance method from a generic context");
		GenericHandleResult g;
		memset(&g, 0, sizeof g);
		JI_CALL(info, VB_DYNAMIC, DYN_embedGenericHandle, EmbedGenericHandleFn, c->method->scope, token, c->method->ftn,
			TOKENKIND_METHOD, &g);
		if (g.needs_runtime_lookup) {
			if (!runtime_lookup(c, 11, &g))
				return false;
		} else if (g.constant.access == IAT_VALUE && g.constant.addr) {
			load_address(c, 11, g.constant.addr);
		} else if (g.constant.access == IAT_PVALUE && g.constant.addr) {
			load_cell(c, 11, g.constant.addr);
		} else {
			FAIL(c, fail_type(c, "generic method handle access", g.constant.access, cls));
		}
		emit(c, ppc_stw(11, c->inst_temp, c->fp));
		inst_callee = true;
	}
	if (!load_arguments(c, count, (buffer ? 1 : 0) + (inst_callee ? 1 : 0)))
		return false;
	if (buffer)
		pass_return_buffer(c, count);
	if (inst_callee)
		emit(c, ppc_lwz(3, c->inst_temp, c->fp));
	const int this_reg = buffer ? 4 : 3;
	const bool is_virtual = op == 0x6F && (attribs & FLG_VIRTUAL) && !(attribs & FLG_FINAL) && !direct;
	if (op == 0x6F && has_this && !null_check(c, this_reg))
		return false;
	if (op == 0x6F && has_this && !is_virtual)
		emit(c, ppc_lwz(0, 0, this_reg)); // callvirt on a non-virtual method still checks for null
	if (is_virtual) {
		uint32_t of_indirection = 0, after_indirection = 0;
		JI_CALL(info, VB_METHOD, MI_getMethodVTableOffset, GetMethodVTableOffsetFn, callee, &of_indirection,
			&after_indirection);
		if (of_indirection > 0x7FF0 || after_indirection > 0x7FF0)
			FAIL(c, "vtable offset");
		// The method table's vtable chunk, then the slot: as Microsoft's compiled code does.
		emit(c, ppc_lwz(12, 0, this_reg));
		emit(c, ppc_lwz(12, (int)of_indirection, 12));
		emit(c, ppc_lwz(12, (int)after_indirection, 12));
		emit(c, ppc_mtctr(12));
		emit(c, PPC_BCTRL);
		safe_point(c);
	} else {
		if (!call_method(c, callee, inst_callee))
			return false;
	}
	return pop(c, count) && push_result(c, &sig);
}

// calli: the code pointer on top, the arguments below it. The runtime's delegate stubs (IL_STUB_ShuffleThunk for a
// static method's delegate, IL_STUB_MulticastInvoke) are calli and little else, and an image whose stubs stay IL
// fails when the runtime needs them.
static bool op_calli(Ctx *c, uint32_t token)
{
	CORINFO_SIG_INFO sig;
	memset(&sig, 0, sizeof sig);
	JI_CALL(c->info, VB_MODULE, MOD_findSig, FindSigFn, c->method->scope, token, c->method->ftn, &sig, 0);
	if (!check_signature(c, &sig))
		return false;
	const uint32_t count = sig_num_args(&sig) + ((sig_call_conv(&sig) & CORINFO_CALLCONV_HASTHIS) ? 1 : 0);
	if (c->depth < count + 1 || c->stack[c->depth - 1] != K_I4)
		FAIL(c, "calli operands");
	const bool buffer = returns_buffer(c, &sig);
	load_entry(c, 0, 12, 0);
	emit(c, ppc_mtctr(12));
	if (!pop(c, 1) || !load_arguments(c, count, buffer ? 1 : 0))
		return false;
	if (buffer)
		pass_return_buffer(c, count);
	emit(c, PPC_BCTRL);
	safe_point(c);
	return pop(c, count) && push_result(c, &sig);
}

// newobj of a value type: the constructor runs on a zeroed slot above the arguments (this is its address), which
// then moves down to where the result goes.
static bool newobj_value_type(Ctx *c, CORINFO_METHOD_HANDLE ctor, CORINFO_CLASS_HANDLE cls, uint32_t count)
{
	uint32_t size;
	if (!value_type(c, cls, &size))
		return false;
	if (c->depth < count || c->depth >= c->max_stack)
		FAIL(c, "constructor arguments");
	const int32_t temp = slot_of(c, c->depth, K_STRUCT);
	zero_words(c, c->fp, temp, size);
	if (!load_arguments(c, count, 1))
		return false;
	emit(c, ppc_addi(3, c->fp, temp));
	// The constructor may store references into the temp and then let the GC run: report them meanwhile.
	c->temp_refs = struct_refs(c, cls, size);
	c->temp_irefs = struct_irefs(c, cls, size);
	c->temp_index = c->depth;
	const bool called = call_method(c, ctor, false);
	c->temp_refs = c->temp_irefs = NO_REFS;
	if (!called)
		return false;
	if (!pop(c, count) || !push_struct(c, cls, size))
		return false;
	if (entry_slot(c, 0) != temp)
		copy_words(c, c->fp, entry_slot(c, 0), c->fp, temp, size);
	return true;
}

// newobj of a value type the runtime reports as a primitive (IntPtr(int), the enum-like wrappers): constructed in
// a temporary slot like any value type, then its bytes are the primitive pushed.
static bool newobj_primitive(Ctx *c, CORINFO_METHOD_HANDLE ctor, CORINFO_CLASS_HANDLE cls, uint32_t type,
	uint32_t count)
{
	const enum Kind kind = kind_of(type);
	const uint32_t size = JI_CALL(c->info, VB_CLASS, CI_getClassSize, GetClassSizeFn, cls);
	if ((kind != K_I4 && kind != K_I8) || (size != 4 && size != 8) || c->depth < count || c->depth >= c->max_stack)
		FAIL(c, fail_type(c, "newobj of a primitive", type, cls));
	const int32_t temp = slot_of(c, c->depth, K_STRUCT);
	zero_words(c, c->fp, temp, 8);
	if (!load_arguments(c, count, 1))
		return false;
	emit(c, ppc_addi(3, c->fp, temp));
	if (!call_method(c, ctor, false) || !pop(c, count))
		return false;
	emit(c, size == 8 ? ppc_ld(11, temp, c->fp) : ppc_lwz(11, temp, c->fp));
	return store_push(c, kind, 11, 0);
}

static bool op_newobj(Ctx *c, uint32_t token)
{
	ICorJitInfo *info = c->info;
	CORINFO_METHOD_HANDLE ctor = JI_CALL(info, VB_MODULE, MOD_findMethod_, FindMethodFn, c->method->scope, token,
		c->method->ftn, 0);
	if (!ctor)
		FAIL(c, "constructor");
	CORINFO_SIG_INFO sig;
	memset(&sig, 0, sizeof sig);
	JI_CALL(info, VB_METHOD, MI_getMethodSig, GetMethodSigFn, ctor, &sig, 0);
	if (!check_signature(c, &sig))
		return false;
	// The object gets the exact type: an EventHandler<__Canon> fails the cast when an event adds it.
	CORINFO_CLASS_HANDLE cls = exact_owner(c, token);
	if (!cls)
		FAIL_UNLESS_SET(c, "constructor's type");
	if (JI_CALL(info, VB_CLASS, CI_asCorInfoType, AsCorInfoTypeFn, cls) == CORINFO_TYPE_VALUECLASS)
		return newobj_value_type(c, ctor, cls, sig_num_args(&sig));
	const uint32_t owner_type = JI_CALL(info, VB_CLASS, CI_asCorInfoType, AsCorInfoTypeFn, cls);
	if (owner_type != CORINFO_TYPE_CLASS) {
		if (JI_CALL(info, VB_CLASS, CI_getClassAttribs, GetClassAttribsFn, cls) & FLG_VALUECLASS)
			return newobj_primitive(c, ctor, cls, owner_type, sig_num_args(&sig));
		FAIL(c, "newobj of a primitive");
	}
	const char *name = JI_CALL(info, VB_CLASS, CI_getClassName, GetClassNameFn, cls);
	if (JI_CALL(info, VB_CLASS, CI_getArrayRank, GetArrayRankFn, cls) != 0) { // an array class has no name here
		// A multi-dimensional array (new T[2, 2]: the array type's constructor taking a length per dimension, or a
		// lower bound and a length each) from JIT_NewMDArr: the exact array class, the argument count, then the
		// arguments as the helper's varargs (r5 on, the rest in the outgoing area's slots).
		const uint32_t dims = sig_num_args(&sig);
		if (c->depth < dims)
			FAIL(c, "newobj arguments");
		for (uint32_t i = 0; i < dims; i++)
			if (c->stack[c->depth - dims + i] != K_I4)
				FAIL(c, "array dimension");
		// Microsoft's calls (mscorlib.ni.dll 0x3095E0, 0x3096B0) pass the array type's handle (a TypeDesc pointer,
		// tagged) in r3 and the constructor's MethodDesc in r4, no count: the helper reads the dimensions the
		// constructor's signature says. Both handles first (their lookups may call helpers), kept in slots across
		// the argument loads.
		if (!load_owner_of_ctor(c, 3, token, cls))
			return false;
		emit(c, ppc_stw(3, c->inst_temp, c->fp));
		if (generic_context(c)) {
			GenericHandleResult g;
			memset(&g, 0, sizeof g);
			JI_CALL(info, VB_DYNAMIC, DYN_embedGenericHandle, EmbedGenericHandleFn, c->method->scope, token,
				c->method->ftn, TOKENKIND_METHOD, &g);
			if (g.needs_runtime_lookup) {
				if (!runtime_lookup(c, 4, &g))
					return false;
			} else if (g.constant.access == IAT_VALUE && g.constant.addr) {
				load_address(c, 4, g.constant.addr);
			} else if (g.constant.access == IAT_PVALUE && g.constant.addr) {
				load_cell(c, 4, g.constant.addr);
			} else {
				FAIL(c, "array constructor handle");
			}
		} else {
			void *indirection = 0;
			CORINFO_METHOD_HANDLE handle = JI_CALL(info, VB_DYNAMIC, DYN_embedMethodHandle, EmbedMethodHandleFn, ctor,
				&indirection);
			if (handle)
				load_address(c, 4, handle);
			else if (indirection)
				load_cell(c, 4, indirection);
			else
				FAIL(c, "array constructor handle");
		}
		emit(c, ppc_stw(4, c->scratch, c->fp));
		if (!load_arguments(c, dims, 2))
			return false;
		emit(c, ppc_lwz(3, c->inst_temp, c->fp));
		emit(c, ppc_lwz(4, c->scratch, c->fp));
		if (!call_helper(c, HELP_NEW_MDARR))
			return false;
		return pop(c, dims) && store_push(c, K_REF, 3, 0);
	}
	if (!name)
		FAIL(c, "newobj of an array");
	const uint32_t class_attribs = JI_CALL(info, VB_CLASS, CI_getClassAttribs, GetClassAttribsFn, cls);
	const uint32_t count = sig_num_args(&sig);
	if (!strcmp(name, "String")) { // getClassName gives the bare name (as "FPU" above)
		// A string's size depends on its constructor's arguments, so there is no JIT_New: the constructor is
		// called with a null this (CLR 2.0's importer does that for variable-sized objects; the FCall behind it
		// ignores the this reference and makes the string) and returns the string.
		if (c->depth < count)
			FAIL(c, "constructor arguments");
		if (!load_arguments(c, count, 1))
			return false;
		emit(c, ppc_li(3, 0));
		if (!call_method(c, ctor, false) || !pop(c, count))
			return false;
		return store_push(c, K_REF, 3, 0);
	}
	// A delegate's constructor (object, native int) is the runtime's DelegateConstruct, which finds the method from
	// the code pointer ldftn or ldvirtftn left.
	if ((class_attribs & FLG_DELEGATE) && (count != 2 || c->stack[c->depth - 1] != K_I4))
		FAIL(c, "delegate constructor");
	if (c->depth < count)
		FAIL(c, "constructor arguments");

	// r3 = the class handle (exact, through a lookup in shared code), then JIT_New(handle).
	if (!load_owner_of_ctor(c, 3, token, cls) || !call_helper(c, HELP_NEW))
		return false;
	emit(c, ppc_stw(3, c->newobj_temp, c->fp));

	// The constructor, with the new object as this.
	if (!load_arguments(c, count, 1))
		return false;
	emit(c, ppc_lwz(3, c->newobj_temp, c->fp));
	if (!call_method(c, ctor, false))
		return false;
	if (!pop(c, count))
		return false;
	emit(c, ppc_lwz(11, c->newobj_temp, c->fp));
	emit(c, ppc_li(0, 0));
	emit(c, ppc_stw(0, c->newobj_temp, c->fp));
	return store_push(c, K_REF, 11, 0);
}

// ldftn: the method's fixed entry point (a cell the image's fixups fill), for a delegate constructor.
static bool op_ldftn(Ctx *c, uint32_t token)
{
	// A P/Invoke method's pointer is its native target: the runtime's P/Invoke IL stubs do `ldftn target; calli`
	// after BeginStandalone, and the method's own entry point is the stub itself (calling it there looped). The
	// target sits in a fixup cell (bound lazily through a thunk), given by getAddressOfPInvokeFixup directly or
	// through a cell of its own.
	CORINFO_METHOD_HANDLE method = JI_CALL(c->info, VB_MODULE, MOD_findMethod_, FindMethodFn, c->method->scope, token,
		c->method->ftn, 0);
	if (method && (JI_CALL(c->info, VB_METHOD, MI_getMethodAttribs, GetMethodAttribsFn, method) & FLG_PINVOKE)) {
		void *indirection = 0;
		void *cell = JI_CALL(c->info, VB_DYNAMIC, DYN_getAddressOfPInvokeFixup, GetAddressOfPInvokeFixupFn, method,
			&indirection);
		if (cell)
			load_address(c, 11, cell);
		else if (indirection)
			load_cell(c, 11, indirection);
		else
			FAIL(c, "P/Invoke target");
		emit(c, ppc_lwz(11, 0, 11));
		return store_push(c, K_I4, 11, 0);
	}
	Lookup lookup;
	uint32_t fixup[8];
	memset(&lookup, 0, sizeof lookup);
	memset(fixup, 0, sizeof fixup);
	JI_CALL(c->info, VB_DYNAMIC, DYN_getFunctionFixedEntryPointInfo, GetFunctionFixedEntryPointInfoFn, c->method->scope,
		token, c->method->ftn, &lookup, fixup);
	if (lookup.needs_runtime_lookup) {
		// From shared generic code the entry point of an instantiation over T comes from the generic context's
		// dictionary (a method-entry slot), through the same walk as a handle; the CORINFO_LOOKUP here is laid out
		// as the lookup part of embedGenericHandle's result.
		if (!runtime_lookup(c, 11, (const GenericHandleResult *)&lookup))
			return false;
		return store_push(c, K_I4, 11, 0);
	}
	if (!lookup.addr) {
		const uint32_t *w = (const uint32_t *)&lookup;
		DbgPrint("xclrjit:   ldftn lookup %08X %08X %08X %08X %08X %08X fixup %08X %08X %08X %08X\n", w[0], w[1], w[2],
			w[3], w[4], w[5], fixup[0], fixup[1], fixup[2], fixup[3]);
		FAIL(c, "ldftn lookup");
	}
	if (lookup.access == IAT_VALUE)
		load_address(c, 11, lookup.addr);
	else if (lookup.access == IAT_PVALUE)
		load_cell(c, 11, lookup.addr);
	else
		FAIL(c, "ldftn access");
	return store_push(c, K_I4, 11, 0);
}

// ldvirtftn: JIT_VirtualFunctionPointer(object, class, method), as for an interface call.
static bool op_ldvirtftn(Ctx *c, uint32_t token)
{
	if (!c->depth || c->stack[c->depth - 1] != K_REF)
		FAIL(c, "ldvirtftn object");
	CORINFO_METHOD_HANDLE method = JI_CALL(c->info, VB_MODULE, MOD_findMethod_, FindMethodFn, c->method->scope, token,
		c->method->ftn, 0);
	if (!method)
		FAIL(c, "ldvirtftn method");
	return virtual_function_pointer(c, method, token, 0) && pop(c, 1) && store_push(c, K_I4, 3, 0);
}

static bool op_ldstr(Ctx *c, uint32_t token)
{
	void *value = 0;
	const uint32_t access = JI_CALL(c->info, VB_DYNAMIC, DYN_constructStringLiteral, ConstructStringLiteralFn,
		c->method->scope, token, &value);
	if (!value)
		FAIL(c, "string literal");
	// The zapper answers IAT_PPVALUE with an import cell that holds the string's handle once the method's fixups
	// are resolved; the string is the handle's target. Microsoft's code loads it the same way:
	// lis rX, cell@ha; lwz rY, cell@l(rX); lwz r3, 0(rY) (PlexResources.ni.dll, 0x94B359CC).
	// mscorlib's own literals (compiling mscorlib) come back as IAT_PVALUE: the cell holds the string itself.
	if (access == IAT_PPVALUE) {
		load_cell(c, 11, value);
		emit(c, ppc_lwz(11, 0, 11));
	} else if (access == IAT_PVALUE) {
		load_cell(c, 11, value);
	} else if (access == IAT_VALUE) {
		load_address(c, 11, value);
	} else {
		FAIL(c, fail_type(c, "string literal access", access, 0));
	}
	return store_push(c, K_REF, 11, 0);
}

// ---------------------------------------------------------------------------------------------------------------
// Types by token: boxing, casts, arrays, statics.

// A method of a generic type or a generic method: its code may be shared between instantiations, so a type that
// depends on the type parameters (T, List<T>, KeyValuePair<TKey,TValue>[]) can't be a fixed handle: it needs a
// runtime lookup through the generic context, which this compiler doesn't do. Resolving one also loads its canonical
// form (an array of __Canon, say), and the zapper fails the whole image (E_OUTOFMEMORY) building that array's
// accessor stubs.
static bool generic_context(const Ctx *c)
{
	// A method of an instantiated class may be the shared (__Canon) instantiation, which there's no telling apart
	// here; an exact generic method instantiation has no hidden argument and resolves exactly.
	return c->method->args.sigInst.classInstCount || (sig_call_conv(&c->method->args) & CORINFO_CALLCONV_PARAMTYPE);
}

// In shared generic code a TypeSpec that mentions a type parameter resolves to the canonical type (__Canon for
// a reference parameter): the right layout for every instantiation sharing the code, so this serves the uses
// that only need the layout (locals, fields, array elements, copies). A use that embeds the handle (box, cast,
// newarr, ldtoken, newobj) goes through load_class_of_token for the exact one.
static CORINFO_CLASS_HANDLE resolve_class_as(Ctx *c, uint32_t token, uint32_t kind)
{
	return JI_CALL(c->info, VB_MODULE, MOD_findClass_, FindClassFn, c->method->scope, token, c->method->ftn, kind);
}

static CORINFO_CLASS_HANDLE resolve_class(Ctx *c, uint32_t token)
{
	return resolve_class_as(c, token, TOKENKIND_CLASS);
}

static bool load_class_handle(Ctx *c, int r, CORINFO_CLASS_HANDLE cls)
{
	void *indirection = 0;
	CORINFO_CLASS_HANDLE handle = JI_CALL(c->info, VB_DYNAMIC, DYN_embedClassHandle, EmbedClassHandleFn, cls,
		&indirection);
	if (handle) {
		// A type of this image, by its address: it must be loaded (restored) before the code runs, which the zapper
		// makes a fixup of the method. Without it an object of the type could be allocated before its vtable's
		// cells for inherited methods of other modules were bound (Silverdash360's DebugException: its Message
		// jumped through an empty cell to address 0).
		JI_CALL(c->info, VB_CLASS, CI_classMustBeLoadedBeforeCodeIsRun, ClassMustBeLoadedFn, cls, 0);
		load_address(c, r, handle);
	} else if (indirection)
		load_cell(c, r, indirection);
	else
		FAIL(c, "class handle");
	return true;
}

// A handle read at run time through the generic context, as the runtime's lookup description says: the context
// (this object's MethodTable, or the hidden class or method instantiation argument) in r11, a chain of loads, and
// if the slot can be empty a test and a helper call (context, signature) that fills it. The context is kept in
// the scratch slot across the loads.
static bool runtime_lookup(Ctx *c, int r, const GenericHandleResult *g)
{
	if (g->runtime.indirections > 4 && g->runtime.indirections != 0xFFFF) { // 0xFFFF: CORINFO_USEHELPER, below
		const uint32_t *w = (const uint32_t *)g;
		DbgPrint("xclrjit:   lookup words %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\n", w[0], w[1], w[2],
			w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10]);
		FAIL(c, "runtime lookup indirections");
	}
	switch (g->runtime_lookup_kind) {
	case 0: // this object's type
		if (!c->args_count || c->args[0].kind != K_REF)
			FAIL(c, "runtime lookup through this");
		load_var_word(c, 11, &c->args[0]);
		emit(c, ppc_lwz(11, 0, 11));
		break;
	case 1: case 2:
		if (!c->has_inst_arg)
			FAIL(c, "runtime lookup without an instantiation argument");
		emit(c, ppc_lwz(11, c->inst_arg, c->fp));
		break;
	default:
		FAIL(c, fail_type(c, "runtime lookup kind", g->runtime_lookup_kind, 0));
	}
	emit(c, ppc_stw(11, c->scratch, c->fp));
	if (g->runtime.indirections == 0xFFFF) {
		// CORINFO_USEHELPER: no dictionary to walk, the helper (context, signature) answers every time.
		emit(c, ppc_mr(3, 11));
		load_address(c, 4, g->runtime.signature);
		if (!call_helper(c, g->runtime.helper))
			return false;
		emit(c, ppc_mr(r, 3));
		return true;
	}
	for (uint32_t i = 0; i < g->runtime.indirections; i++) {
		if (g->runtime.offsets[i] > 0x7FF0)
			FAIL(c, "runtime lookup offset");
		emit(c, ppc_lwz(11, (int)g->runtime.offsets[i], 11));
	}
	if (g->runtime.test_for_fixup) {
		// A native image's dictionary slot may hold a tagged pointer to the handle's fixup cell instead of the
		// handle: if (slot & 1) slot = *(slot - 1) (CLR 4's impRuntimeLookupToTree).
		emit(c, ppc_rlwinm(0, 11, 0, 31, 31));
		emit(c, ppc_cmpwi(6, 0, 0));
		const uint32_t skip_fixup = c->n;
		emit(c, ppc_bc(BO_TRUE, 6 * 4 + CR_EQ, 0));
		emit(c, ppc_lwz(11, -1, 11));
		c->code[skip_fixup] = ppc_bc(BO_TRUE, 6 * 4 + CR_EQ, (int32_t)(c->n - skip_fixup) * 4);
	}
	if (g->runtime.test_for_null) {
		emit(c, ppc_cmpwi(6, 11, 0));
		const uint32_t skip = c->n;
		emit(c, ppc_bc(BO_FALSE, 6 * 4 + CR_EQ, 0));
		emit(c, ppc_lwz(3, c->scratch, c->fp));
		load_address(c, 4, g->runtime.signature);
		if (!call_helper(c, g->runtime.helper))
			return false;
		emit(c, ppc_mr(11, 3));
		c->code[skip] = ppc_bc(BO_FALSE, 6 * 4 + CR_EQ, (int32_t)(c->n - skip) * 4);
	}
	emit(c, ppc_mr(r, 11));
	return true;
}

// The class a token names, as the runtime resolves it here: in shared generic code the canonical one, which has
// the exact type's layout (the code is only shared among instantiations of the same layout).
static CORINFO_CLASS_HANDLE resolve_class_any(Ctx *c, uint32_t token, uint32_t kind)
{
	return JI_CALL(c->info, VB_MODULE, MOD_findClass_, FindClassFn, c->method->scope, token, c->method->ftn, kind);
}

// The exact class a token names, in r: through a runtime lookup in shared generic code when the token depends on
// the type parameters (a TypeSpec). cls_out gets the class as resolved here (canonical in shared code). Clobbers
// r3-r12 when it calls the lookup helper: load it before the other argument registers.
static bool load_class_of_token(Ctx *c, int r, uint32_t token, uint32_t kind, CORINFO_CLASS_HANDLE *cls_out)
{
	ResolvedToken resolved;
	memset(&resolved, 0, sizeof resolved);
	resolved.context = c->method->ftn;
	resolved.scope = c->method->scope;
	resolved.token = token;
	resolved.kind = kind;
	JI_CALL(c->info, VB_MODULE, MOD_resolveToken, ResolveTokenFn, &resolved);
	if (!resolved.cls)
		FAIL(c, "token class");
	*cls_out = resolved.cls;
	if (!generic_context(c) || !resolved.type_spec)
		return load_class_handle(c, r, resolved.cls);
	GenericHandleResult g;
	memset(&g, 0, sizeof g);
	JI_CALL(c->info, VB_DYNAMIC, DYN_embedGenericHandle, EmbedGenericHandleFn, c->method->scope, token, c->method->ftn,
		kind, &g);
	if (g.needs_runtime_lookup)
		return runtime_lookup(c, r, &g);
	if (g.constant.access == IAT_VALUE && g.constant.addr)
		load_address(c, r, g.constant.addr);
	else if (g.constant.access == IAT_PVALUE && g.constant.addr)
		load_cell(c, r, g.constant.addr);
	else
		FAIL(c, fail_type(c, "generic handle access", g.constant.access, resolved.cls));
	return true;
}


// ---------------------------------------------------------------------------------------------------------------
// A little metadata: the parent of a MemberRef, which no ICorJitInfo call gives as a token. The method's IL lies
// in its module's PE image (the CLR maps an assembly whole), so the image is found by walking down to the page
// that starts with "MZ"; the CLI header's metadata and the #~ stream's tables follow ECMA-335 II.24. Both image
// layouts are tried: sections at their virtual addresses, and the file as is.

static uint32_t le16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

typedef struct {
	const uint8_t *base;
	bool flat;  // sections at their file offsets
	uint32_t section_count;
	const uint8_t *sections;
} PeImage;

static const uint8_t *pe_rva(const PeImage *pe, uint32_t rva)
{
	if (!pe->flat)
		return pe->base + rva;
	for (uint32_t i = 0; i < pe->section_count; i++) {
		const uint8_t *sec = pe->sections + 40 * i;
		const uint32_t va = le32(sec + 12), raw_size = le32(sec + 16), raw = le32(sec + 20);
		if (rva >= va && rva < va + raw_size)
			return pe->base + raw + (rva - va);
	}
	return 0;
}

// The metadata root of the module whose image holds `inside`, or 0.
static const uint8_t *metadata_of(const void *inside, PeImage *pe)
{
	const uint8_t *base = (const uint8_t *)((uintptr_t)inside & ~(uintptr_t)0xFFFF);
	for (uint32_t i = 0; i < 1024; i++, base -= 0x10000) {
		if (base[0] != 'M' || base[1] != 'Z')
			continue;
		const uint32_t e_lfanew = le32(base + 0x3C);
		if (e_lfanew > 0x1000 || le32(base + e_lfanew) != 0x4550)
			continue;
		const uint8_t *file = base + e_lfanew + 4;
		const uint32_t opt_size = le16(file + 16);
		const uint8_t *opt = file + 20;
		const uint32_t magic = le16(opt);
		const uint32_t dirs = magic == 0x20B ? 112 : 96;
		pe->base = base;
		pe->section_count = le16(file + 2);
		pe->sections = opt + opt_size;
		const uint32_t cli_rva = le32(opt + dirs + 14 * 8);
		if (!cli_rva)
			return 0;
		for (int flat = 0; flat < 2; flat++) {
			pe->flat = flat != 0;
			const uint8_t *cli = pe_rva(pe, cli_rva);
			if (!cli)
				continue;
			const uint8_t *meta = pe_rva(pe, le32(cli + 8));
			if (meta && le32(meta) == 0x424A5342) // BSJB
				return meta;
		}
		return 0;
	}
	return 0;
}

// The MemberRefParent of MemberRef `token`, as a token (TypeDef, TypeRef, ModuleRef, MethodDef or TypeSpec), or 0.
static uint32_t memberref_parent(const Ctx *c, uint32_t token)
{
	PeImage pe;
	const uint8_t *meta = metadata_of(c->il, &pe);
	if (!meta || (token >> 24) != 0x0A)
		return 0;
	const uint32_t version_length = le32(meta + 12);
	const uint8_t *streams = meta + 16 + ((version_length + 3) & ~3u) + 4;
	const uint32_t stream_count = le16(meta + 16 + ((version_length + 3) & ~3u) + 2);
	const uint8_t *tables = 0;
	for (uint32_t i = 0; i < stream_count; i++) {
		const uint32_t offset = le32(streams);
		const char *name = (const char *)streams + 8;
		if ((name[0] == '#' && name[1] == '~' && !name[2]) || (name[0] == '#' && name[1] == '-' && !name[2]))
			tables = meta + offset;
		uint32_t len = 0;
		while (name[len])
			len++;
		streams += 8 + ((len + 1 + 3) & ~3u);
	}
	if (!tables)
		return 0;
	const uint32_t heaps = tables[6];
	const uint32_t s = heaps & 1 ? 4 : 2, g = heaps & 2 ? 4 : 2, b = heaps & 4 ? 4 : 2;
	const uint32_t valid_lo = le32(tables + 8), valid_hi = le32(tables + 12);
	uint32_t rows[64];
	const uint8_t *p = tables + 24;
	for (uint32_t i = 0; i < 64; i++) {
		const bool present = i < 32 ? (valid_lo >> i) & 1 : (valid_hi >> (i - 32)) & 1;
		rows[i] = present ? le32(p) : 0;
		if (present)
			p += 4;
	}
#define IDX(t) (rows[t] >= 0x10000 ? 4 : 2)
#define CODED2(a, b2, c2, d) (rows[a] >= 0x4000 || rows[b2] >= 0x4000 || rows[c2] >= 0x4000 || rows[d] >= 0x4000 ? 4 : 2)
#define CODED3(a, b2, c2, d, e) \
	(rows[a] >= 0x2000 || rows[b2] >= 0x2000 || rows[c2] >= 0x2000 || rows[d] >= 0x2000 || rows[e] >= 0x2000 ? 4 : 2)
	const uint32_t typedef_or_ref = CODED2(2, 1, 0x1B, 0x1B);
	const uint32_t parent = CODED3(2, 1, 0x1A, 6, 0x1B);
	const uint32_t sizes[11] = {
		2 + s + 3 * g,                                           // Module
		CODED2(0, 0x1A, 0x23, 1) + 2 * s,                        // TypeRef
		4 + 2 * s + typedef_or_ref + IDX(4) + IDX(6),            // TypeDef
		IDX(4),                                                  // FieldPtr
		2 + s + b,                                               // Field
		IDX(6),                                                  // MethodPtr
		4 + 2 + 2 + s + b + IDX(8),                              // MethodDef
		IDX(8),                                                  // ParamPtr
		2 + 2 + s,                                               // Param
		IDX(2) + typedef_or_ref,                                 // InterfaceImpl
		parent + s + b,                                          // MemberRef
	};
#undef IDX
#undef CODED2
#undef CODED3
	for (uint32_t i = 0; i < 10; i++)
		p += rows[i] * sizes[i];
	const uint32_t row = token & 0xFFFFFF;
	if (!row || row > rows[10])
		return 0;
	const uint8_t *r = p + (row - 1) * sizes[10];
	const uint32_t coded = parent == 4 ? le32(r) : le16(r);
	static const uint32_t parent_tables[5] = {0x02, 0x01, 0x1A, 0x06, 0x1B};
	if ((coded & 7) > 4)
		return 0;
	return parent_tables[coded & 7] << 24 | coded >> 3;
}

// The exact class a constructor token's owner is, in r (newobj of a generic class from shared code).
static bool load_owner_of_ctor(Ctx *c, int r, uint32_t token, CORINFO_CLASS_HANDLE cls)
{
	ResolvedToken resolved;
	memset(&resolved, 0, sizeof resolved);
	resolved.context = c->method->ftn;
	resolved.scope = c->method->scope;
	resolved.token = token;
	resolved.kind = TOKENKIND_METHOD;
	JI_CALL(c->info, VB_MODULE, MOD_resolveToken, ResolveTokenFn, &resolved);
	if (!generic_context(c) || !resolved.type_spec)
		return load_class_handle(c, r, cls);
	// The exact owner: embedGenericHandle embeds whatever the token names (handleType by token type, no parent
	// embedding), and the constructor's own MethodDesc would give the canonical class (reference instantiations
	// share the canonical type's MethodDescs), so the constructor's MemberRef is read in the module's metadata for
	// its parent, a TypeSpec, and that is looked up as a class.
	const uint32_t parent = memberref_parent(c, token);
	if ((parent >> 24) != 0x1B)
		FAIL(c, fail_type(c, "newobj of a generic class from a generic context: parent", parent, cls));
	CORINFO_CLASS_HANDLE exact;
	return load_class_of_token(c, r, parent, TOKENKIND_CLASS, &exact);
}

// The CorInfoType a class token stands for: a primitive for the primitive value types and enums, CLASS for
// reference types, VALUECLASS for other structs.
static uint32_t class_type(Ctx *c, CORINFO_CLASS_HANDLE cls)
{
	return JI_CALL(c->info, VB_CLASS, CI_asCorInfoType, AsCorInfoTypeFn, cls);
}

static bool op_box(Ctx *c, uint32_t token)
{
	CORINFO_CLASS_HANDLE cls = resolve_class_any(c, token, TOKENKIND_BOX);
	if (!cls)
		FAIL_UNLESS_SET(c, "box class");
	const uint32_t t = class_type(c, cls);
	// Boxing a reference type does nothing. In shared generic code a type parameter resolves to its canonical
	// class (__Canon): only reference instantiations share the code, so this holds for every one of them.
	if (t == CORINFO_TYPE_CLASS || t == CORINFO_TYPE_STRING) {
		if (c->stack[c->depth - 1] != K_REF)
			FAIL(c, "box of a non-reference");
		return true;
	}
	if (t == CORINFO_TYPE_VALUECLASS) {
		// JIT_Box(class, &value) with the value's own slot. Nullable<T> boxes differently.
		uint32_t size;
		const char *name = JI_CALL(c->info, VB_CLASS, CI_getClassName, GetClassNameFn, cls);
		if (!value_type(c, cls, &size))
			return false;
		if (c->stack[c->depth - 1] != K_STRUCT || !name || !strncmp(name, "System.Nullable", 15))
			FAIL(c, "box of a value type");
		if (!load_class_of_token(c, 3, token, TOKENKIND_BOX, &cls))
			return false;
		load_entry(c, 0, 4, 0);
		if (!pop(c, 1) || !call_helper(c, HELP_BOX))
			return false;
		return store_push(c, K_REF, 3, 0);
	}
	const enum Kind k = kind_of(t);
	if (k == K_NONE || k == K_REF || k == K_BYREF || c->stack[c->depth - 1] != k)
		FAIL(c, "box type");
	// The class first (a lookup uses the scratch slot), then the value in its own size in the scratch slot, then
	// JIT_Box(class, &scratch).
	if (!load_class_of_token(c, 3, token, TOKENKIND_BOX, &cls))
		return false;
	load_entry(c, 0, 11, 0);
	if (t == CORINFO_TYPE_FLOAT)
		emit(c, ppc_frsp(0, 0));
	store_typed(c, t, 11, c->scratch, c->fp);
	if (!pop(c, 1))
		return false;
	emit(c, ppc_addi(4, c->fp, c->scratch));
	if (!call_helper(c, HELP_BOX))
		return false;
	return store_push(c, K_REF, 3, 0);
}

// castclass, isinst, and unbox.any of a reference type. A null object, or one whose MethodTable is the class itself
// (the type handle of a class is its MethodTable; an array's or a type parameter's never equals one), passes without
// the helper, which decides the rest.
static bool op_cast(Ctx *c, uint32_t token, uint32_t kind, uint32_t helper)
{
	CORINFO_CLASS_HANDLE cls;
	if (c->stack[c->depth - 1] != K_REF)
		FAIL(c, "cast of a non-reference");
	if (!load_class_of_token(c, 3, token, kind, &cls))
		return false;
	load_entry(c, 0, 4, 0);
	if (!pop(c, 1))
		return false;
	emit(c, ppc_cmpwi(6, 4, 0));
	const uint32_t if_null = c->n;
	emit(c, ppc_bc(BO_TRUE, 6 * 4 + CR_EQ, 0));
	emit(c, ppc_lwz(0, 0, 4));
	emit(c, ppc_cmpw(6, 0, 3));
	const uint32_t if_exact = c->n;
	emit(c, ppc_bc(BO_TRUE, 6 * 4 + CR_EQ, 0));
	if (!call_helper(c, helper))
		return false;
	emit(c, ppc_mr(4, 3));
	c->code[if_null] = ppc_bc(BO_TRUE, 6 * 4 + CR_EQ, (int32_t)(c->n - if_null) * 4);
	c->code[if_exact] = ppc_bc(BO_TRUE, 6 * 4 + CR_EQ, (int32_t)(c->n - if_exact) * 4);
	return store_push(c, K_REF, 4, 0);
}

// unbox: JIT_Unbox(class, object) gives the address of the value in the box.
static bool op_unbox(Ctx *c, uint32_t token)
{
	CORINFO_CLASS_HANDLE cls = resolve_class_any(c, token, TOKENKIND_CLASS);
	if (!cls || c->stack[c->depth - 1] != K_REF)
		FAIL(c, "unbox");
	const char *name = JI_CALL(c->info, VB_CLASS, CI_getClassName, GetClassNameFn, cls);
	if (!name || !strcmp(name, "Nullable`1")) // the bare name; a boxed T unboxes to Nullable<T> through its own helper
		FAIL(c, "unbox of Nullable");
	if (!load_class_of_token(c, 3, token, TOKENKIND_CLASS, &cls))
		return false;
	load_entry(c, 0, 4, 0);
	if (!pop(c, 1) || !call_helper(c, HELP_UNBOX))
		return false;
	return store_push(c, K_BYREF, 3, 0);
}

static bool op_unbox_any(Ctx *c, uint32_t token)
{
	CORINFO_CLASS_HANDLE cls = resolve_class_any(c, token, TOKENKIND_CLASS);
	if (!cls)
		FAIL_UNLESS_SET(c, "unbox class");
	const uint32_t t = class_type(c, cls);
	if (t == CORINFO_TYPE_CLASS || t == CORINFO_TYPE_STRING)
		return op_cast(c, token, TOKENKIND_CASTING, HELP_CHKCASTANY);
	if (t == CORINFO_TYPE_VALUECLASS) {
		uint32_t size;
		const char *name = JI_CALL(c->info, VB_CLASS, CI_getClassName, GetClassNameFn, cls);
		if (!value_type(c, cls, &size))
			return false;
		if (c->stack[c->depth - 1] != K_REF)
			FAIL(c, "unbox of a value type");
		if (name && !strcmp(name, "Nullable`1")) {
			// A boxed T, or null, becomes a Nullable<T>: JIT_Unbox_Nullable(destination, Nullable<T>'s MethodTable,
			// object), helper 49 (0x84CCC988: Nullable::UnBox, then the cast's failure), which fills the slot.
			if (!load_class_of_token(c, 11, token, TOKENKIND_CLASS, &cls))
				return false;
			emit(c, ppc_stw(11, c->scratch + 4, c->fp));
			load_entry(c, 0, 5, 0);
			if (!pop(c, 1) || !push_struct(c, cls, size))
				return false;
			emit(c, ppc_addi(3, c->fp, entry_slot(c, 0)));
			emit(c, ppc_lwz(4, c->scratch + 4, c->fp));
			return call_helper(c, HELP_UNBOX_NULLABLE);
		}
		if (!load_class_of_token(c, 3, token, TOKENKIND_CLASS, &cls))
			return false;
		load_entry(c, 0, 4, 0);
		if (!pop(c, 1) || !call_helper(c, HELP_UNBOX) || !push_struct(c, cls, size))
			return false;
		copy_words(c, c->fp, entry_slot(c, 0), 3, 0, size);
		return true;
	}
	const enum Kind k = kind_of(t);
	if (k == K_NONE || k == K_REF || k == K_BYREF || c->stack[c->depth - 1] != K_REF)
		FAIL(c, "unbox type");
	if (!load_class_of_token(c, 3, token, TOKENKIND_CLASS, &cls))
		return false;
	load_entry(c, 0, 4, 0);
	if (!pop(c, 1) || !call_helper(c, HELP_UNBOX))
		return false;
	load_typed(c, t, 11, 0, 3);
	return store_push(c, k, 11, 0);
}

// Arrays: the length at +4; elements of up to 4 bytes from +8, object references from +12 (an element type word
// sits at +8), as Microsoft's compiled code addresses them (mscorlib.ni.dll: addi 8 before lhzx/lwzx, addi 12
// before the reference loads). Value types holding references start at +16 (Microsoft's Dictionary.Insert,
// mscorlib.ni.dll 0x1DCAB0, `addi r4, r8, 0x10` for its Entry<,> array against `addi r6, r31, 8` for the int
// array; with +8 our code agreed with itself, and Array.Copy and the GC, which read the element size and start
// from the array's MethodTable, saw every struct array's first element eight bytes off: NGenBench's
// ArrayCopyProbe, and Dictionary.Resize losing its first entry). 8-byte primitives are at +8 (ArrayClearProbe;
// read at +16, Enum's ulong[] of values from the runtime gave every name one off), and so are value types without
// references (struct_element_address).
enum { ARRAY_LENGTH = 4, ARRAY_DATA = 8, REF_ARRAY_DATA = 12, WIDE_ARRAY_DATA = 16 };

static int element_shift(uint32_t type)
{
	switch (type) {
	case CORINFO_TYPE_BOOL:
	case CORINFO_TYPE_BYTE:
	case CORINFO_TYPE_UBYTE:
		return 0;
	case CORINFO_TYPE_CHAR:
	case CORINFO_TYPE_SHORT:
	case CORINFO_TYPE_USHORT:
		return 1;
	case CORINFO_TYPE_INT:
	case CORINFO_TYPE_UINT:
	case CORINFO_TYPE_NATIVEINT:
	case CORINFO_TYPE_NATIVEUINT:
	case CORINFO_TYPE_FLOAT:
	case CORINFO_TYPE_CLASS:
	case CORINFO_TYPE_STRING:
		return 2;
	case CORINFO_TYPE_LONG:
	case CORINFO_TYPE_ULONG:
	case CORINFO_TYPE_DOUBLE:
		return 3;
	default:
		return -1;
	}
}

// Leaves the element's address less its data offset in r11, after a bounds check. The array is entry `array_at`
// from the top and the index the one above it.
static bool element_address(Ctx *c, uint32_t array_at, uint32_t type)
{
	const int shift = element_shift(type);
	if (shift < 0)
		FAIL(c, fail_type(c, "array element type", type, 0));
	if (c->stack[c->depth - 1 - array_at] != K_REF || c->stack[c->depth - array_at] != K_I4)
		FAIL(c, "array operands");
	// The array and the index from the registers holding them, if they are (operand).
	const int array = operand(c, array_at, 11), index = operand(c, array_at - 1, 12);
	if (!null_check(c, array))
		return false;
	emit(c, ppc_lwz(0, ARRAY_LENGTH, array)); // also the null check
	emit(c, ppc_cmplw(6, index, 0));
	const uint32_t in_range = c->n;
	emit(c, ppc_bc(BO_TRUE, 6 * 4 + CR_LT, 0));
	if (!call_throw_helper(c, HELP_RNGCHKFAIL))
		return false;
	c->code[in_range] = ppc_bc(BO_TRUE, 6 * 4 + CR_LT, (int32_t)(c->n - in_range) * 4);
	if (shift)
		emit(c, ppc_rlwinm(12, index, shift, 0, 31 - shift));
	emit(c, ppc_add(11, array, shift ? 12 : index));
	return true;
}

static int32_t data_offset(uint32_t type)
{
	const enum Kind k = kind_of(type);
	// 8-byte primitives are at +8 like the smaller ones (the array object is 8-byte aligned): Array.Clear on a
	// long[] and a double[] cleared the word before an element read at +16, on a struct array the element itself.
	return k == K_REF ? REF_ARRAY_DATA : ARRAY_DATA;
}

static bool op_ldelem(Ctx *c, uint32_t type)
{
	if (!element_address(c, 1, type))
		return false;
	if (!pop(c, 2))
		return false;
	const enum Kind kind = kind_of(type);
	const int d = result_register(c, kind, 11), fd = kind == K_R8 ? fresult_register(c, 0) : 0;
	load_typed_to(c, type, d, fd, data_offset(type), 11);
	return store_push(c, kind, d, fd);
}

// Arrays of value types: elements of `size` bytes from WIDE_ARRAY_DATA on when they are 16 bytes or more, from
// ARRAY_DATA like the primitives when smaller (Array.Clear from the runtime, NGenBench's ArrayOffsets: 4-, 8- and
// 12-byte value types, generic or not, with longs or not, are cleared at +8; 16-, 20- and 24-byte ones, of ints,
// doubles, longs or references, at +16). XcpImports.MethodPack's GCHandle[] written at +16 had run four bytes
// past the object, into the next one's MethodTable word, which then overwrote a handle. Leaves the element's
// address in r11.
static bool struct_element_address(Ctx *c, uint32_t array_at, uint32_t size, bool refs)
{
	if (c->stack[c->depth - 1 - array_at] != K_REF || c->stack[c->depth - array_at] != K_I4)
		FAIL(c, "array operands");
	load_entry(c, array_at, 11, 0);
	load_entry(c, array_at - 1, 12, 0);
	if (!null_check(c, 11))
		return false;
	emit(c, ppc_lwz(0, ARRAY_LENGTH, 11));
	emit(c, ppc_cmplw(6, 12, 0));
	const uint32_t in_range = c->n;
	emit(c, ppc_bc(BO_TRUE, 6 * 4 + CR_LT, 0));
	if (!call_throw_helper(c, HELP_RNGCHKFAIL))
		return false;
	c->code[in_range] = ppc_bc(BO_TRUE, 6 * 4 + CR_LT, (int32_t)(c->n - in_range) * 4);
	emit(c, ppc_mulli(12, 12, (int32_t)size));
	emit(c, ppc_add(11, 11, 12));
	(void)refs; // a 12-byte value type holding references is at +8 like the rest (ArrayOffsets' S12R)
	emit(c, ppc_addi(11, 11, size >= 16 ? WIDE_ARRAY_DATA : ARRAY_DATA));
	return true;
}

static bool struct_has_refs(Ctx *c, CORINFO_CLASS_HANDLE cls)
{
	return (JI_CALL(c->info, VB_CLASS, CI_getClassAttribs, GetClassAttribsFn, cls) & FLG_CONTAINS_GC_PTR) != 0;
}

static bool op_ldelem_struct(Ctx *c, CORINFO_CLASS_HANDLE cls)
{
	uint32_t size;
	if (!value_type(c, cls, &size) || !struct_element_address(c, 1, size, struct_has_refs(c, cls)) || !pop(c, 2) ||
		!push_struct(c, cls, size))
		return false;
	copy_words(c, c->fp, entry_slot(c, 0), 11, 0, size);
	return true;
}

static bool op_stelem_struct(Ctx *c, CORINFO_CLASS_HANDLE cls)
{
	uint32_t size;
	if (!value_type(c, cls, &size))
		return false;
	if (c->stack[c->depth - 1] != K_STRUCT)
		FAIL(c, "stelem value");
	return struct_element_address(c, 2, size, struct_has_refs(c, cls)) &&
		copy_struct(c, 11, 0, c->fp, entry_slot(c, 0), size, c->stack_refs[c->depth - 1]) && pop(c, 3);
}

static bool op_ldelema_struct(Ctx *c, CORINFO_CLASS_HANDLE cls)
{
	uint32_t size;
	return value_type(c, cls, &size) && struct_element_address(c, 1, size, struct_has_refs(c, cls)) && pop(c, 2) &&
		store_push(c, K_BYREF, 11, 0);
}

static bool op_ldelema(Ctx *c, uint32_t type, CORINFO_CLASS_HANDLE cls)
{
	const bool readonly = c->readonly;
	c->readonly = false;
	if (kind_of(type) == K_REF && !readonly) {
		// The array's element type must be the class exactly (a string[] passed as an object[] may not hand out
		// an object& to its elements): JIT_Ldelema_Ref checks and throws ArrayTypeMismatchException.
		if (c->stack[c->depth - 2] != K_REF || c->stack[c->depth - 1] != K_I4)
			FAIL(c, "array operands");
		load_entry(c, 1, 3, 0);
		load_entry(c, 0, 4, 0);
		if (!load_class_handle(c, 5, cls) || !pop(c, 2) || !call_helper(c, HELP_LDELEMA_REF))
			return false;
		return store_push(c, K_BYREF, 3, 0);
	}
	if (!element_address(c, 1, type))
		return false;
	emit(c, ppc_addi(11, 11, data_offset(type)));
	return pop(c, 2) && store_push(c, K_BYREF, 11, 0);
}

static bool op_stelem(Ctx *c, uint32_t type)
{
	if (kind_of(type) == K_REF) {
		// JIT_Stelem_Ref(array, index, object) checks the bounds and the element type and does the write barrier.
		// Inline first: the bounds, then a null stored as is (no barrier), or an object whose MethodTable is the
		// array's element class (at array + 8, as JIT_Ldelema_Ref reads it) stored through the barrier for r5; any
		// other object goes to the helper, which decides.
		if (c->stack[c->depth - 3] != K_REF || c->stack[c->depth - 2] != K_I4 || c->stack[c->depth - 1] != K_REF)
			FAIL(c, "stelem.ref operands");
		load_entry(c, 2, 3, 0);
		load_entry(c, 1, 4, 0);
		load_entry(c, 0, 5, 0);
		if (!pop(c, 3) || !null_check(c, 3))
			return false;
		emit(c, ppc_lwz(0, ARRAY_LENGTH, 3)); // also the null check
		emit(c, ppc_cmplw(6, 4, 0));
		const uint32_t in_range = c->n;
		emit(c, ppc_bc(BO_TRUE, 6 * 4 + CR_LT, 0));
		if (!call_throw_helper(c, HELP_RNGCHKFAIL))
			return false;
		c->code[in_range] = ppc_bc(BO_TRUE, 6 * 4 + CR_LT, (int32_t)(c->n - in_range) * 4);
		emit(c, ppc_rlwinm(12, 4, 2, 0, 29));
		emit(c, ppc_add(12, 3, 12));
		emit(c, ppc_cmpwi(6, 5, 0));
		const uint32_t if_null = c->n;
		emit(c, ppc_bc(BO_TRUE, 6 * 4 + CR_EQ, 0));
		emit(c, ppc_lwz(0, 0, 5));
		emit(c, ppc_lwz(11, 8, 3));
		emit(c, ppc_cmpw(6, 0, 11));
		const uint32_t if_other = c->n;
		emit(c, ppc_bc(BO_FALSE, 6 * 4 + CR_EQ, 0));
		emit(c, ppc_addi(3, 12, REF_ARRAY_DATA));
		if (!write_barrier(c, true, 5))
			return false;
		const uint32_t done_exact = c->n;
		emit(c, ppc_b(0));
		c->code[if_other] = ppc_bc(BO_FALSE, 6 * 4 + CR_EQ, (int32_t)(c->n - if_other) * 4);
		if (!call_helper(c, HELP_STELEM_REF))
			return false;
		const uint32_t done_other = c->n;
		emit(c, ppc_b(0));
		c->code[if_null] = ppc_bc(BO_TRUE, 6 * 4 + CR_EQ, (int32_t)(c->n - if_null) * 4);
		emit(c, ppc_stw(5, REF_ARRAY_DATA, 12));
		c->code[done_exact] = ppc_b((int32_t)(c->n - done_exact) * 4);
		c->code[done_other] = ppc_b((int32_t)(c->n - done_other) * 4);
		return true;
	}
	if (c->stack[c->depth - 1] != kind_of(type))
		FAIL(c, "stelem value");
	if (!element_address(c, 2, type))
		return false;
	if (kind_of(type) == K_R8)
		store_typed_from(c, type, 0, foperand(c, 0, 0), data_offset(type), 11);
	else
		store_typed(c, type, operand(c, 0, 12), data_offset(type), 11);
	return pop(c, 3);
}

static bool op_newarr(Ctx *c, uint32_t token)
{
	CORINFO_CLASS_HANDLE element = resolve_class_any(c, token, TOKENKIND_NEWARR);
	if (!element || c->stack[c->depth - 1] != K_I4)
		FAIL(c, "array element class");
	if (generic_context(c) && (token & 0xFF000000u) == 0x1B000000u) {
		// The exact array type through a lookup: resolveToken with the Newarr kind names the array type itself
		// (checked by its rank), as CLR 4's does.
		CORINFO_CLASS_HANDLE array;
		if (!load_class_of_token(c, 3, token, TOKENKIND_NEWARR, &array))
			return false;
		if (JI_CALL(c->info, VB_CLASS, CI_getArrayRank, GetArrayRankFn, array) != 1)
			FAIL(c, "array type of a type from a generic context");
	} else {
		CORINFO_CLASS_HANDLE array = JI_CALL(c->info, VB_CLASS, CI_getSDArrayForClass, GetSDArrayForClassFn, element);
		if (!array || !load_class_handle(c, 3, array))
			FAIL_UNLESS_SET(c, "array class");
	}
	load_entry(c, 0, 4, 0);
	if (!pop(c, 1) || !call_helper(c, HELP_NEWARR_1))
		return false;
	return store_push(c, K_REF, 3, 0);
}

// Loads a 32-bit constant into r.
static void load_constant(Ctx *c, int r, uint32_t value)
{
	if ((int32_t)value >= -32768 && (int32_t)value <= 32767) {
		emit(c, ppc_li(r, (int32_t)value));
	} else {
		emit(c, ppc_lis(r, (int32_t)value >> 16));
		emit(c, ppc_ori(r, r, value & 0xFFFF));
	}
}

// Loads a value the runtime gives either directly or through a cell.
static bool load_value_or_cell(Ctx *c, int r, uint32_t value, void *cell)
{
	if (cell)
		load_cell(c, r, cell);
	else
		load_constant(c, r, value);
	return true;
}

// The exact MethodTable of the class a static field token belongs to, in r, from shared code: a FieldDef names
// the method's own class, whose exact instantiation is this object's type or the hidden argument (a generic method
// of the class gets a MethodDesc instead: declined); a MemberRef's parent is a TypeSpec, looked up at run time.
static bool load_static_owner(Ctx *c, int r, uint32_t token)
{
	if ((token >> 24) == 0x04) {
		if (c->has_inst_arg) {
			if (c->method->args.sigInst.methInstCount)
				FAIL(c, "generic class statics from a generic method");
			emit(c, ppc_lwz(r, c->inst_arg, c->fp));
			return true;
		}
		if (!c->args_count || c->args[0].kind != K_REF)
			FAIL(c, "generic class statics without a context");
		load_var_word(c, r, &c->args[0]);
		emit(c, ppc_lwz(r, 0, r));
		return true;
	}
	const uint32_t parent = memberref_parent(c, token);
	if ((parent >> 24) != 0x1B)
		FAIL(c, fail_type(c, "generic class statics: field parent", parent, 0));
	CORINFO_CLASS_HANDLE exact;
	return load_class_of_token(c, r, parent, TOKENKIND_CLASS, &exact);
}

// ldsfld, ldsflda, stsfld. Statics live in the domain's per-module blocks (DomainLocalModule), found at run time as
// Microsoft's code does: JIT_GetSharedGCStaticBase / JIT_GetSharedNonGCStaticBase(module ID, class ID) (helpers 364
// and 365, getSharedStaticBaseHelper's answer), which also run the class constructor the first time, then the
// field's offset (for non-GC statics it counts from the DomainLocalModule, header included). The module ID comes
// through a cell (getClassModuleIdForStatics), the class ID as a constant (getClassDomainID).
static bool op_static_field(Ctx *c, uint8_t op, uint32_t token)
{
	CORINFO_FIELD_HANDLE field = JI_CALL(c->info, VB_MODULE, MOD_findField, FindFieldFn, c->method->scope, token,
		c->method->ftn);
	if (!field)
		FAIL(c, "field");
	const uint32_t attribs = JI_CALL(c->info, VB_FIELD, FI_getFieldAttribs, GetFieldAttribsFn, field, 0);
	if (!(attribs & FLG_STATIC))
		FAIL(c, "instance field used as static");
	CORINFO_CLASS_HANDLE cls = 0;
	const uint32_t type = JI_CALL(c->info, VB_FIELD, FI_getFieldType, GetFieldTypeFn, field, &cls, 0);
	const enum Kind kind = kind_of(type);
	// A value type's static (TimeSpan, a user struct) is a boxed object in the GC statics: the slot holds the box,
	// whose data starts after its method table pointer. Primitives live inline.
	uint32_t struct_size = 0;
	const bool boxed = type == CORINFO_TYPE_VALUECLASS; // observed attribs 0x8C0009 for TimeSpan and a user struct
	if (boxed) {
		if (!value_type(c, cls, &struct_size))
			return false;
	} else if (kind == K_NONE || kind == K_BYREF || kind == K_STRUCT) {
		FAIL(c, fail_type(c, "static field type", type, cls));
	}
	if (op == 0x80 && c->stack[c->depth - 1] != (boxed ? K_STRUCT : kind))
		FAIL(c, "stsfld value");
	const uint32_t offset = JI_CALL(c->info, VB_FIELD, FI_getFieldOffset, GetFieldOffsetFn, field);
	if (offset > 0x7FF0)
		FAIL(c, "static field offset");

	CORINFO_CLASS_HANDLE owner = JI_CALL(c->info, VB_FIELD, FI_getFieldClass, GetFieldClassFn, field);
	uint32_t helper = JI_CALL(c->info, VB_CLASS, CI_getSharedStaticBaseHelper, GetSharedStaticBaseHelperFn, field,
		0);
	const bool dynamic = helper == HELP_GETSHARED_GCSTATIC_BASE_DYNAMICCLASS ||
		helper == HELP_GETSHARED_NONGCSTATIC_BASE_DYNAMICCLASS ||
		helper == HELP_GETSHARED_GCTHREADSTATIC_BASE_DYNAMICCLASS ||
		helper == HELP_GETSHARED_NONGCTHREADSTATIC_BASE_DYNAMICCLASS;
	if (helper != HELP_GETSHARED_GCSTATIC_BASE && helper != HELP_GETSHARED_NONGCSTATIC_BASE &&
		helper != HELP_GETSHARED_GCSTATIC_BASE_NOCTOR && helper != HELP_GETSHARED_NONGCSTATIC_BASE_NOCTOR &&
		helper != HELP_GETSHARED_GCTHREADSTATIC_BASE && helper != HELP_GETSHARED_NONGCTHREADSTATIC_BASE &&
		helper != HELP_GETSHARED_GCTHREADSTATIC_BASE_NOCTOR && helper != HELP_GETSHARED_NONGCTHREADSTATIC_BASE_NOCTOR &&
		!dynamic)
		FAIL(c, fail_type(c, "static base helper", helper, owner));
	if (dynamic && generic_context(c)) {
		// A generic class's statics from shared code: the exact class is only known at run time, so the helpers
		// that take the MethodTable (362/363, 371/372) are called with the owner found through the context.
		if (!load_static_owner(c, 3, token))
			return false;
		helper = helper == HELP_GETSHARED_GCSTATIC_BASE_DYNAMICCLASS ? HELP_GETGENERICS_GCSTATIC_BASE
			: helper == HELP_GETSHARED_NONGCSTATIC_BASE_DYNAMICCLASS ? HELP_GETGENERICS_NONGCSTATIC_BASE
			: helper == HELP_GETSHARED_GCTHREADSTATIC_BASE_DYNAMICCLASS ? HELP_GETGENERICS_GCTHREADSTATIC_BASE
			: HELP_GETGENERICS_NONGCTHREADSTATIC_BASE;
		if (!call_helper(c, helper))
			return false;
	} else {
		void *module = 0, *module_cell = 0, *class_cell = 0;
		const uint32_t module_id = JI_CALL(c->info, VB_CLASS, CI_getClassModuleIdForStatics,
			GetClassModuleIdForStaticsFn, owner, &module, &module_cell);
		const uint32_t class_id = JI_CALL(c->info, VB_DYNAMIC, DYN_getClassDomainID, GetClassDomainIDFn, owner,
			&class_cell);
		if (!load_value_or_cell(c, 3, module_id, module_cell) || !load_value_or_cell(c, 4, class_id, class_cell) ||
			!call_helper(c, helper))
			return false;
	}
	emit(c, ppc_addi(11, 3, (int)offset));
	if (boxed) {
		emit(c, ppc_lwz(11, 0, 11));
		emit(c, ppc_addi(11, 11, 4));
		if (op == 0x7F) // ldsflda
			return store_push(c, K_BYREF, 11, 0);
		if (op == 0x7E) { // ldsfld
			if (!push_struct(c, cls, struct_size))
				return false;
			copy_words(c, c->fp, entry_slot(c, 0), 11, 0, struct_size);
			return true;
		}
		return copy_struct(c, 11, 0, c->fp, entry_slot(c, 0), struct_size, c->stack_refs[c->depth - 1]) && pop(c, 1);
	}
	if (op == 0x7F) // ldsflda
		return store_push(c, K_BYREF, 11, 0);
	if (op == 0x7E) { // ldsfld
		load_typed(c, type, 11, 0, 11);
		return store_push(c, kind, 11, 0);
	}
	// stsfld
	if (kind == K_REF) {
		emit(c, ppc_mr(3, 11));
		load_entry(c, 0, 4, 0);
		return pop(c, 1) && write_barrier(c, false, 4);
	}
	if (kind == K_R8)
		store_typed_from(c, type, 0, foperand(c, 0, 0), 0, 11);
	else
		store_typed(c, type, operand(c, 0, 12), 0, 11);
	return pop(c, 1);
}

static bool op_switch(Ctx *c, uint32_t at, uint32_t *pc)
{
	const uint32_t count = il_u32(c, at + 1);
	const uint32_t end = at + 5 + 4 * count;
	*pc = end;
	if (count > 1024 || end > c->il_size)
		FAIL(c, "switch");
	if (c->stack[c->depth - 1] != K_I4)
		FAIL(c, "switch value");
	load_entry(c, 0, 11, 0);
	if (!pop(c, 1))
		return false;
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t target = end + il_u32(c, at + 5 + 4 * i);
		if (i < 0x8000) {
			emit(c, ppc_cmpwi(6, 11, (int)i));
		} else {
			emit(c, ppc_lis(12, (int32_t)i >> 16));
			emit(c, ppc_ori(12, 12, i & 0xFFFF));
			emit(c, ppc_cmpw(6, 11, 12));
		}
		if (!branch_to(c, target, BO_TRUE, 6 * 4 + CR_EQ))
			return false;
	}
	return true;
}

static bool load_var_value(Ctx *c, const Var *v);

// Pushes a variable; a primitive one standing for a value type keeps the class (primitive_value_type).
static bool load_var(Ctx *c, const Var *v)
{
	if (!load_var_value(c, v))
		return false;
	if (v->kind != K_STRUCT)
		c->stack_cls[c->depth - 1] = v->cls;
	return true;
}

static bool load_var_value(Ctx *c, const Var *v)
{
	if (v->kind == K_STRUCT) {
		if (!struct_room(c, v->size))
			return false;
		if (!push_struct(c, v->cls, v->size))
			return false;
		// The variable's own masks: a TypedReference's interior pointer isn't in its class's layout.
		c->stack_refs[c->depth - 1] = v->refs;
		c->stack_irefs[c->depth - 1] = v->irefs;
		if (c->leaf && v->reg) // register to register (small_struct)
			emit(c, ppc_mr(entry_register(c, c->depth - 1), v->reg));
		else
			copy_words(c, c->fp, entry_slot(c, 0), c->fp, v->offset, v->size);
		return true;
	}
	// A callee-saved register variable's value stays where it is: the entry names its register (an f-register for
	// floating point) until the variable is stored to (store_var) or the entry written back.
	if ((v->reg || v->freg) && !c->leaf && c->caching && cache_register(c, c->depth, (enum Kind)v->kind) >= 0) {
		if (!push(c, (enum Kind)v->kind))
			return false;
		c->cached[c->depth - 1] = true;
		c->alias[c->depth - 1] = v->reg ? v->reg : v->freg;
		return true;
	}
	if (v->freg) // into the entry's slot straight from the variable's register
		return store_push(c, K_R8, 11, v->freg);
	if (v->kind == K_R8) {
		const int fd = fresult_register(c, 0);
		load_typed_to(c, v->type, 11, fd, v->offset, c->fp);
		return store_push(c, K_R8, 11, fd);
	}
	const int r = result_register(c, (enum Kind)v->kind, 11);
	if (v->reg) {
		if (r != v->reg)
			emit(c, ppc_mr(r, v->reg));
	} else {
		load_typed(c, v->type, r, v->offset, c->fp);
	}
	return store_push(c, (enum Kind)v->kind, r, 0);
}

static bool store_var(Ctx *c, const Var *v)
{
	if (v->kind == K_STRUCT || c->stack[c->depth - 1] == K_STRUCT) {
		if (c->stack[c->depth - 1] != v->kind || c->stack_size[c->depth - 1] != v->size)
			FAIL(c, "store of a different value type");
		if (c->leaf && v->reg)
			emit(c, ppc_mr(v->reg, entry_register(c, c->depth - 1)));
		else
			copy_words(c, c->fp, v->offset, c->fp, entry_slot(c, 0), v->size);
		return pop(c, 1);
	}
	const enum Kind k = (enum Kind)c->stack[c->depth - 1];
	// A native int and a byref may be stored into each other's variables; nothing else changes kind, so a GC slot
	// never takes an integer.
	if (k != v->kind && !(k == K_I4 && v->kind == K_BYREF) && !(k == K_BYREF && v->kind == K_I4))
		FAIL(c, "store of a different kind");
	// Entries below still naming the variable's register get its old value in their own first.
	const int own = v->reg ? v->reg : v->freg;
	for (uint32_t i = 0; own && i + 1 < c->depth; i++)
		if (c->cached[i] && c->alias[i] == own && (c->stack[i] == K_R8) == (v->freg != 0)) {
			const int to = cache_register(c, i, (enum Kind)c->stack[i]);
			emit(c, v->freg ? ppc_fmr(to, own) : ppc_mr(to, own));
			c->alias[i] = 0;
		}
	if (k == K_R8 && v->freg) {
		load_entry(c, 0, 11, v->freg);
		if (v->type == CORINFO_TYPE_FLOAT)
			emit(c, ppc_frsp(v->freg, v->freg));
		return pop(c, 1);
	}
	if (k == K_R8) {
		store_typed_from(c, v->type, 0, foperand(c, 0, 0), v->offset, c->fp);
		return pop(c, 1);
	}
	const int r = operand(c, 0, 11);
	if (v->reg) {
		narrow_into(c, v->type, v->reg, r);
		return pop(c, 1);
	}
	store_typed(c, v->type, r, v->offset, c->fp);
	return pop(c, 1);
}

// localloc: the stack pointer moves down by the size rounded to 16 bytes, the back chain moves with it and the
// outgoing argument area stays at the bottom, so the block lies above it; as Microsoft's compiler does, every page
// of a large block is touched first (the guard page), and the block is zeroed (localsinit, as every local here).
static bool op_localloc(Ctx *c)
{
	if (c->depth < 1)
		FAIL(c, "stack underflow");
	if (c->funclet)
		FAIL(c, "localloc in a handler");
	load_entry(c, 0, 3, 0);
	if (!pop(c, 1))
		return false;
	emit(c, ppc_addi(10, 3, 15));
	emit(c, ppc_rlwinm(10, 10, 0, 0, 27));
	emit(c, ppc_neg(12, 10));
	emit(c, ppc_addi(0, 10, 0xFFF));
	emit(c, ppc_srawi(0, 0, 12));
	emit(c, ppc_cmpwi(6, 0, 0));
	const uint32_t no_pages = c->n;
	emit(c, 0);
	emit(c, ppc_mr(11, 1));
	emit(c, ppc_mtctr(0));
	emit(c, ppc_lwzu(0, -0x1000, 11));
	emit(c, ppc_bdnz(-4));
	c->code[no_pages] = ppc_bc(BO_FALSE, 6 * 4 + CR_GT, (int32_t)(c->n - no_pages) * 4);
	emit(c, ppc_lwz(9, 0, 1));
	emit(c, ppc_stwux(9, 1, 12));
	emit(c, ppc_addi(11, 1, c->outgoing));
	emit(c, ppc_rlwinm(0, 10, 30, 2, 31));
	emit(c, ppc_cmpwi(6, 0, 0));
	const uint32_t empty = c->n;
	emit(c, 0);
	emit(c, ppc_mtctr(0));
	emit(c, ppc_mr(9, 11));
	emit(c, ppc_li(0, 0));
	emit(c, ppc_stw(0, 0, 9));
	emit(c, ppc_addi(9, 9, 4));
	emit(c, ppc_bdnz(-8));
	c->code[empty] = ppc_bc(BO_TRUE, 6 * 4 + CR_EQ, (int32_t)(c->n - empty) * 4);
	return store_push(c, K_I4, 11, 0);
}

static bool address_of_var(Ctx *c, const Var *v)
{
	emit(c, ppc_addi(11, c->fp, v->offset));
	return store_push(c, K_BYREF, 11, 0);
}

// ldarga/ldloca, with *pc after it. In a leaf, the address of a value type in a register followed by ldfld (how C#
// reads a field of a value type argument or local: TimeSpan's operators) is the field straight from the register,
// both instructions at once; any other address of such a variable needs memory, which fails the leaf.
static bool var_address(Ctx *c, const Var *v, uint32_t *pc)
{
	const uint32_t next = *pc;
	if (!c->leaf || v->kind != K_STRUCT || !v->reg || next + 5 > c->il_size || c->il[next] != 0x7B ||
		c->branch_target[next])
		return address_of_var(c, v);
	CORINFO_FIELD_HANDLE field = JI_CALL(c->info, VB_MODULE, MOD_findField, FindFieldFn, c->method->scope,
		il_u32(c, next + 1), c->method->ftn);
	if (!field)
		FAIL(c, "field");
	if (JI_CALL(c->info, VB_FIELD, FI_getFieldAttribs, GetFieldAttribsFn, field, 0) & FLG_STATIC)
		FAIL(c, "static field");
	CORINFO_CLASS_HANDLE cls = 0;
	const uint32_t type = JI_CALL(c->info, VB_FIELD, FI_getFieldType, GetFieldTypeFn, field, &cls, 0);
	const uint32_t offset = JI_CALL(c->info, VB_FIELD, FI_getFieldOffset, GetFieldOffsetFn, field);
	const enum Kind kind = kind_of(type);
	if (kind == K_NONE || kind == K_R8)
		FAIL(c, LEAF_WHY("value type field type"));
	const int d = result_register(c, kind, 11);
	if (!extract_field(c, type, d, v->reg, v->size, offset))
		return false;
	*pc = next + 5;
	return store_push(c, kind, d, 0);
}

static bool constant_i4(Ctx *c, int32_t value)
{
	const int r = result_register(c, K_I4, 11);
	if (value >= -32768 && value <= 32767) {
		emit(c, ppc_li(r, value));
	} else {
		emit(c, ppc_lis(r, value >> 16));
		emit(c, ppc_ori(r, r, value & 0xFFFF));
	}
	return store_push(c, K_I4, r, 0);
}

static bool op_return(Ctx *c)
{
	if (c->return_type != CORINFO_TYPE_VOID) {
		if (c->depth != 1)
			FAIL(c, "stack at return");
		if (c->return_size) {
			if (c->stack[0] != K_STRUCT || c->stack_size[0] != c->return_size)
				FAIL(c, "value type at return");
			if (c->leaf) { // in its register, as it is returned (leaf_registers took only 4 or 8 bytes)
				emit(c, ppc_mr(3, entry_register(c, 0)));
			} else if (c->return_size <= 8) {
				emit(c, c->return_size == 4 ? ppc_lwz(3, entry_slot(c, 0), c->fp) : ppc_ld(3, entry_slot(c, 0), c->fp));
			} else {
				emit(c, ppc_lwz(11, c->retbuf, c->fp));
				if (!copy_struct(c, 11, 0, c->fp, entry_slot(c, 0), c->return_size, c->stack_refs[c->depth - 1]))
					return false;
			}
			if (!pop(c, 1))
				return false;
			epilog(c);
			return true;
		}
		if (c->stack[0] == K_R8) {
			const int fa = foperand(c, 0, 1);
			if (c->return_type == CORINFO_TYPE_FLOAT)
				emit(c, ppc_frsp(1, fa));
			else if (fa != 1)
				emit(c, ppc_fmr(1, fa));
		} else {
			load_entry(c, 0, 3, 1);
		}
		if (!pop(c, 1))
			return false;
	} else if (c->depth) {
		FAIL(c, "stack at return");
	}
	epilog(c);
	return true;
}

// The IL type of ldind/stind/ldelem forms by their opcode's suffix.
static uint32_t indirect_type(uint8_t op)
{
	switch (op) {
	case 0x46: return CORINFO_TYPE_BYTE;      // ldind.i1
	case 0x47: return CORINFO_TYPE_UBYTE;     // ldind.u1
	case 0x48: return CORINFO_TYPE_SHORT;     // ldind.i2
	case 0x49: return CORINFO_TYPE_USHORT;    // ldind.u2
	case 0x4A: return CORINFO_TYPE_INT;       // ldind.i4
	case 0x4B: return CORINFO_TYPE_UINT;      // ldind.u4
	case 0x4C: return CORINFO_TYPE_LONG;      // ldind.i8
	case 0x4D: return CORINFO_TYPE_NATIVEINT; // ldind.i
	case 0x4E: return CORINFO_TYPE_FLOAT;     // ldind.r4
	case 0x4F: return CORINFO_TYPE_DOUBLE;    // ldind.r8
	case 0x50: return CORINFO_TYPE_CLASS;     // ldind.ref
	case 0x51: return CORINFO_TYPE_CLASS;     // stind.ref
	case 0x52: return CORINFO_TYPE_BYTE;      // stind.i1
	case 0x53: return CORINFO_TYPE_SHORT;     // stind.i2
	case 0x54: return CORINFO_TYPE_INT;       // stind.i4
	case 0x55: return CORINFO_TYPE_LONG;      // stind.i8
	case 0x56: return CORINFO_TYPE_FLOAT;     // stind.r4
	case 0x57: return CORINFO_TYPE_DOUBLE;    // stind.r8
	case 0xDF: return CORINFO_TYPE_NATIVEINT; // stind.i
	default: return CORINFO_TYPE_UNDEF;
	}
}

static uint32_t convert_type(uint8_t op)
{
	switch (op) {
	case 0x67: return CORINFO_TYPE_BYTE;      // conv.i1
	case 0x68: return CORINFO_TYPE_SHORT;     // conv.i2
	case 0x69: return CORINFO_TYPE_INT;       // conv.i4
	case 0x6A: return CORINFO_TYPE_LONG;      // conv.i8
	case 0x6B: return CORINFO_TYPE_FLOAT;     // conv.r4
	case 0x6C: return CORINFO_TYPE_DOUBLE;    // conv.r8
	case 0x6D: return CORINFO_TYPE_UINT;      // conv.u4
	case 0x6E: return CORINFO_TYPE_ULONG;     // conv.u8
	case 0x76: return CORINFO_TYPE_DOUBLE;    // conv.r.un
	case 0xD1: return CORINFO_TYPE_USHORT;    // conv.u2
	case 0xD2: return CORINFO_TYPE_UBYTE;     // conv.u1
	case 0xD3: return CORINFO_TYPE_NATIVEINT; // conv.i
	case 0xE0: return CORINFO_TYPE_NATIVEUINT;// conv.u
	default: return CORINFO_TYPE_UNDEF;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Exception handling.

static bool in_range(uint32_t at, uint32_t start, uint32_t end)
{
	return at >= start && at < end;
}

// The innermost finally whose try a leave from `from` to `target` exits, among the clauses after `after` (-1: all).
// Clauses come inner first (ECMA-335 II.19), so each one found encloses the one before.
static int32_t exited_finally(Ctx *c, uint32_t from, uint32_t target, int32_t after)
{
	for (uint32_t i = (uint32_t)(after + 1); i < c->clauses_count; i++) {
		const Clause *k = &c->clauses[i];
		if (k->flags == EH_FINALLY && in_range(from, k->try_start, k->try_end) &&
			!in_range(target, k->try_start, k->try_end))
			return (int32_t)i;
	}
	return -1;
}

// The thunk that runs clause `clause`'s finally on the way to `target`, requested if new.
static int32_t thunk_for(Ctx *c, uint32_t clause, uint32_t target)
{
	for (uint32_t i = 0; i < c->thunks_count; i++)
		if (c->thunks[i].clause == clause && c->thunks[i].target == target)
			return (int32_t)i;
	if (c->thunks_count >= MAX_THUNKS) {
		c->failure = "leave thunks";
		return -1;
	}
	c->thunks[c->thunks_count] = (Thunk){clause, target, -1};
	return (int32_t)c->thunks_count++;
}

static bool branch_fixup(Ctx *c, uint32_t target, uint8_t to, uint32_t word)
{
	if (c->fixups_count >= MAX_FIXUPS)
		FAIL(c, "branches");
	c->fixups[c->fixups_count++] = (Fixup){c->n, target, to};
	emit(c, word);
	return true;
}

static bool catch_return(Ctx *c, uint32_t target, int32_t finally);

// Places clause `clause`'s thunks, right after its try's code: each calls the finally funclet with the frame, then
// goes on to the next finally the leave exits, or to the target.
static bool place_thunks(Ctx *c, uint32_t clause)
{
	for (uint32_t i = 0; i < c->thunks_count; i++) {
		Thunk *t = &c->thunks[i];
		if (t->clause != clause)
			continue;
		t->at = (int32_t)c->n;
		emit(c, ppc_addi(3, 31, c->frame));
		if (!branch_fixup(c, clause, TO_FUNCLET, ppc_bl(0)))
			return false;
		safe_point(c);
		const int32_t next = exited_finally(c, c->clauses[clause].try_start, t->target, (int32_t)clause);
		// A try inside a catch handler, left for a place outside the handler (HLSPlayback.DataStreamResponseReceived:
		// a finally inside the catch, another around the whole method): its finally ran here in the catch funclet;
		// the catch then returns, resuming where its parent goes on, the next finally's thunk or the target.
		const Clause *h = c->funclet ? &c->clauses[c->funclet - 1] : 0;
		if (h && !in_range(t->target, h->handler_start, h->handler_end) &&
			(next < 0 || !in_range(c->clauses[next].try_start, h->handler_start, h->handler_end))) {
			if (h->flags != EH_CATCH)
				FAIL(c, "leave out of a finally");
			c->depth = 0;
			if (!catch_return(c, t->target, next))
				return false;
			continue;
		}
		if (next < 0) {
			c->depth = 0;
			if (!branch_to(c, t->target, -1, 0))
				return false;
			continue;
		}
		const int32_t n = thunk_for(c, (uint32_t)next, t->target);
		if (n < 0 || !branch_fixup(c, (uint32_t)n, TO_THUNK, ppc_b(0)))
			return false;
	}
	return true;
}

// Returns from a catch funclet to where its parent resumes: the thunk of `finally` (the first finally still to run
// on the way, -1 for none), or the target. Both are in the parent's code, compiled before the funclet.
static bool catch_return(Ctx *c, uint32_t target, int32_t finally)
{
	int32_t word;
	if (finally >= 0) {
		const int32_t t = thunk_for(c, (uint32_t)finally, target);
		if (t < 0)
			return false;
		word = c->thunks[t].at;
	} else {
		if (!note_target(c, target))
			return false;
		word = c->labels[target];
	}
	if (word < 0)
		FAIL(c, "leave target");
	// The parent's frame is reported at the resume address too (all its slots are valid there).
	safe_point_at(c, (uint32_t)word, true);
	load_local_address(c, 3, (uint32_t)word);
	funclet_epilog(c);
	return true;
}

// leave, leave.s: empties the evaluation stack and exits trys on the way to the target, running their finallys.
static bool op_leave(Ctx *c, uint32_t at, uint32_t target)
{
	c->depth = 0;
	const int32_t exited = exited_finally(c, at, target, -1);
	if (!c->funclet) {
		if (exited < 0)
			return branch_to(c, target, -1, 0);
		const int32_t t = thunk_for(c, (uint32_t)exited, target);
		return t >= 0 && branch_fixup(c, (uint32_t)t, TO_THUNK, ppc_b(0));
	}
	const Clause *h = &c->clauses[c->funclet - 1];
	if (in_range(target, h->handler_start, h->handler_end)) {
		// Within the handler: a leave out of a try nested in it still runs the finallys it exits.
		if (exited < 0)
			return branch_to(c, target, -1, 0);
		const int32_t t = thunk_for(c, (uint32_t)exited, target);
		return t >= 0 && branch_fixup(c, (uint32_t)t, TO_THUNK, ppc_b(0));
	}
	if (h->flags != EH_CATCH)
		FAIL(c, "leave out of a finally");
	// A finally inside the handler runs first, from this funclet: its thunk returns from the catch afterwards.
	if (exited >= 0 && in_range(c->clauses[exited].try_start, h->handler_start, h->handler_end)) {
		const int32_t t = thunk_for(c, (uint32_t)exited, target);
		return t >= 0 && branch_fixup(c, (uint32_t)t, TO_THUNK, ppc_b(0));
	}
	return catch_return(c, target, exited);
}

static bool op_endfinally(Ctx *c)
{
	if (!c->funclet || c->clauses[c->funclet - 1].flags == EH_CATCH)
		FAIL(c, "endfinally outside a finally");
	funclet_epilog(c);
	return true;
}

static bool op_throw(Ctx *c)
{
	if (!c->depth || c->stack[c->depth - 1] != K_REF)
		FAIL(c, "throw of a non-reference");
	load_entry(c, 0, 3, 0);
	return pop(c, 1) && call_throw_helper(c, HELP_THROW);
}

// Loads the method's exception clauses. Handled: catch, finally and fault clauses whose trys lie in the main body
// (none inside a handler); filters aren't.
static bool load_clauses(Ctx *c)
{
	c->clauses_count = c->method->EHcount;
	if (c->clauses_count > MAX_CLAUSES)
		FAIL(c, "too many exception clauses");
	for (uint32_t i = 0; i < c->clauses_count; i++) {
		EHClause e;
		memset(&e, 0, sizeof e);
		JI_CALL(c->info, VB_METHOD, MI_getEHinfo, GetEHinfoFn, c->method->ftn, i, &e);
		if (e.flags != EH_CATCH && e.flags != EH_FINALLY && e.flags != EH_FAULT)
			FAIL(c, "exception filter");
		Clause *k = &c->clauses[i];
		*k = (Clause){e.flags, e.try_offset, e.try_offset + e.try_length, e.handler_offset,
			e.handler_offset + e.handler_length, e.class_token, -1, -1, -1, -1};
		if (!e.try_length || !e.handler_length || k->try_end > c->il_size || k->handler_end > c->il_size)
			FAIL(c, "exception clause");
	}
	return true;
}

static int32_t handler_at(Ctx *c, uint32_t pc)
{
	for (uint32_t i = 0; i < c->clauses_count; i++)
		if (c->clauses[i].handler_start == pc)
			return (int32_t)i;
	return -1;
}

// Compiles the instruction at *pc, advancing it. Returns false to decline the method.
static bool compile_instruction(Ctx *c, uint32_t *pc, bool *falls_through)
{
	const uint32_t at = *pc;
	const uint8_t op = (uint8_t)il_u8(c, at);
	*falls_through = true;
	*pc = at + 1;
	switch (op) {
	case 0x00: return true; // nop
	case 0x02: case 0x03: case 0x04: case 0x05: // ldarg.0-3
		if ((uint32_t)(op - 0x02) >= c->args_count) FAIL(c, "ldarg");
		return load_var(c, &c->args[op - 0x02]);
	case 0x06: case 0x07: case 0x08: case 0x09: // ldloc.0-3
		if ((uint32_t)(op - 0x06) >= c->locals_count) FAIL(c, "ldloc");
		return load_var(c, &c->locals[op - 0x06]);
	case 0x0A: case 0x0B: case 0x0C: case 0x0D: // stloc.0-3
		if ((uint32_t)(op - 0x0A) >= c->locals_count) FAIL(c, "stloc");
		return store_var(c, &c->locals[op - 0x0A]);
	case 0x0E: case 0x0F: case 0x10: { // ldarg.s, ldarga.s, starg.s
		const uint32_t i = il_u8(c, at + 1);
		*pc = at + 2;
		if (i >= c->args_count) FAIL(c, "argument index");
		return op == 0x0E ? load_var(c, &c->args[i]) : op == 0x0F ? var_address(c, &c->args[i], pc) : store_var(c, &c->args[i]);
	}
	case 0x11: case 0x12: case 0x13: { // ldloc.s, ldloca.s, stloc.s
		const uint32_t i = il_u8(c, at + 1);
		*pc = at + 2;
		if (i >= c->locals_count) FAIL(c, "local index");
		return op == 0x11 ? load_var(c, &c->locals[i]) : op == 0x12 ? var_address(c, &c->locals[i], pc) : store_var(c, &c->locals[i]);
	}
	case 0x14: // ldnull
		emit(c, ppc_li(11, 0));
		return store_push(c, K_REF, 11, 0);
	case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
		return constant_i4(c, (int32_t)op - 0x16); // ldc.i4.m1 - ldc.i4.8
	case 0x1F: // ldc.i4.s
		*pc = at + 2;
		return constant_i4(c, (int8_t)il_u8(c, at + 1));
	case 0x20: // ldc.i4
		*pc = at + 5;
		return constant_i4(c, (int32_t)il_u32(c, at + 1));
	case 0x21: { // ldc.i8
		*pc = at + 9;
		const int r = result_register(c, K_I8, 11);
		load_constant64(c, r, (uint64_t)il_u32(c, at + 5) << 32 | il_u32(c, at + 1));
		return store_push(c, K_I8, r, 0);
	}
	case 0x22: case 0x23: { // ldc.r4 (widened to double), ldc.r8
		uint64_t bits;
		if (op == 0x22) {
			*pc = at + 5;
			union { uint32_t u; float f; } single = {il_u32(c, at + 1)};
			union { double d; uint64_t u; } widened = {(double)single.f};
			bits = widened.u;
		} else {
			*pc = at + 9;
			bits = (uint64_t)il_u32(c, at + 5) << 32 | il_u32(c, at + 1);
		}
		const int fd = fresult_register(c, 0);
		return load_fp_constant(c, fd, bits) && store_push(c, K_R8, 0, fd);
	}
	case 0x25: { // dup
		if (!c->depth) FAIL(c, "dup");
		if (c->stack[c->depth - 1] == K_STRUCT) {
			const int32_t from = entry_slot(c, 0);
			if (!push_struct(c, c->stack_cls[c->depth - 1], c->stack_size[c->depth - 1])) return false;
			copy_words(c, c->fp, entry_slot(c, 0), c->fp, from, c->stack_size[c->depth - 1]);
			return true;
		}
		// A copy of an entry naming its variable's register names it too.
		const uint32_t top = c->depth - 1;
		const enum Kind k = (enum Kind)c->stack[top];
		if (c->cached[top] && c->alias[top] && c->caching && cache_register(c, c->depth, k) >= 0) {
			const int8_t reg = c->alias[top];
			if (!push(c, k))
				return false;
			c->cached[c->depth - 1] = true;
			c->alias[c->depth - 1] = reg;
			return true;
		}
		if (k == K_R8)
			return store_push(c, k, 11, foperand(c, 0, 0));
		return store_push(c, k, operand(c, 0, 11), 0);
	}
	case 0x70: // cpobj: ldobj from the source address on top, stobj to the destination under it
		*pc = at + 5;
		return op_ldobj(c, il_u32(c, at + 1)) && op_stobj(c, il_u32(c, at + 1));
	case 0x71: // ldobj
		*pc = at + 5;
		return op_ldobj(c, il_u32(c, at + 1));
	case 0x81: // stobj
		*pc = at + 5;
		return op_stobj(c, il_u32(c, at + 1));
	case 0x79: // unbox
		*pc = at + 5;
		return op_unbox(c, il_u32(c, at + 1));
	case 0x26: // pop
		return pop(c, 1);
	case 0x29: // calli
		*pc = at + 5;
		return op_calli(c, il_u32(c, at + 1));
	case 0x28: // call
	case 0x6F: // callvirt
		*pc = at + 5;
		return op_call(c, op, il_u32(c, at + 1));
	case 0xD0: // ldtoken
		*pc = at + 5;
		return op_ldtoken(c, at, il_u32(c, at + 1));
	case 0x2A: // ret
		*falls_through = false;
		return op_return(c);
	case 0x2B: case 0x38: { // br.s, br
		const bool short_form = op == 0x2B;
		const int32_t d = short_form ? (int8_t)il_u8(c, at + 1) : (int32_t)il_u32(c, at + 1);
		*pc = at + (short_form ? 2 : 5);
		*falls_through = false;
		return branch_to(c, *pc + d, -1, 0);
	}
	case 0x2C: case 0x2D: case 0x39: case 0x3A: { // brfalse(.s), brtrue(.s)
		const bool short_form = op < 0x39;
		const int32_t d = short_form ? (int8_t)il_u8(c, at + 1) : (int32_t)il_u32(c, at + 1);
		*pc = at + (short_form ? 2 : 5);
		const enum Kind k = load_entry(c, 0, 11, 0);
		if (k == K_R8 || k == K_STRUCT) FAIL(c, "brtrue on floating point or a value type");
		if (k == K_I8) {
			emit(c, ppc_li(0, 0));
			emit(c, ppc_cmpd(6, 11, 0));
		} else {
			emit(c, ppc_cmpwi(6, 11, 0));
		}
		if (!pop(c, 1)) return false;
		const bool on_true = op == 0x2D || op == 0x3A;
		return branch_to(c, *pc + d, on_true ? BO_FALSE : BO_TRUE, 6 * 4 + CR_EQ);
	}
	case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
	case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: {
		const bool short_form = op < 0x38;
		const int kind = op - (short_form ? 0x2E : 0x3B);
		const int32_t d = short_form ? (int8_t)il_u8(c, at + 1) : (int32_t)il_u32(c, at + 1);
		*pc = at + (short_form ? 2 : 5);
		bool fp;
		if (!compare_top(c, kind >= 5, &fp)) return false;
		int bo, bi;
		condition(c, kind, fp, &bo, &bi);
		return branch_to(c, *pc + d, bo, bi);
	}
	case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
	case 0x50:
		return op_load_indirect(c, indirect_type(op));
	case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0xDF:
		return op_store_indirect(c, indirect_type(op));
	case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61:
	case 0xD6: case 0xD7: case 0xD8: case 0xD9: case 0xDA: case 0xDB:
	case 0x62: case 0x63: case 0x64:
		if (c->depth < 2) FAIL(c, "binary operands");
		return op_binary(c, op);
	case 0x65: { // neg
		if (c->stack[c->depth - 1] == K_R8) {
			const int fa = foperand(c, 0, 0);
			if (!pop(c, 1))
				return false;
			const int fd = fresult_register(c, 0);
			emit(c, ppc_fneg(fd, fa));
			return store_push(c, K_R8, 0, fd);
		}
		const enum Kind k = load_entry(c, 0, 11, 0);
		if (k == K_R8) emit(c, ppc_fneg(0, 0));
		else if (k == K_I4 || k == K_I8) emit(c, ppc_neg(11, 11));
		else FAIL(c, "neg");
		return pop(c, 1) && store_push(c, k, 11, 0);
	}
	case 0x66: { // not
		const enum Kind k = load_entry(c, 0, 11, 0);
		if (k != K_I4 && k != K_I8) FAIL(c, "not");
		emit(c, ppc_nor(11, 11, 11));
		return pop(c, 1) && store_push(c, k, 11, 0);
	}
	case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x76:
	case 0xD1: case 0xD2: case 0xD3: case 0xE0:
		return op_convert(c, convert_type(op));
	case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B:
	case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: case 0xB8: case 0xB9: case 0xBA: case 0xD4: case 0xD5: {
		bool un;
		const uint32_t to = convert_ovf_type(op, &un);
		return op_convert_ovf(c, to, un);
	}
	case 0x72: // ldstr
		*pc = at + 5;
		return op_ldstr(c, il_u32(c, at + 1));
	case 0x73: // newobj
		*pc = at + 5;
		return op_newobj(c, il_u32(c, at + 1));
	case 0x7B: case 0x7C: case 0x7D: // ldfld, ldflda, stfld
		*pc = at + 5;
		return op_field(c, op, il_u32(c, at + 1));
	case 0x7E: case 0x7F: case 0x80: // ldsfld, ldsflda, stsfld
		*pc = at + 5;
		return op_static_field(c, op, il_u32(c, at + 1));
	case 0x45: // switch
		return op_switch(c, at, pc);
	case 0x7A: // throw
		*falls_through = false;
		return op_throw(c);
	case 0xDC: // endfinally
		*falls_through = false;
		return op_endfinally(c);
	case 0xDD: // leave
		*pc = at + 5;
		*falls_through = false;
		return op_leave(c, at, *pc + il_u32(c, at + 1));
	case 0xDE: // leave.s
		*pc = at + 2;
		*falls_through = false;
		return op_leave(c, at, *pc + (int8_t)il_u8(c, at + 1));
	case 0xC6: { // mkrefany T: a TypedReference {pointer, T's exact handle} from a pointer
		*pc = at + 5;
		if (c->stack[c->depth - 1] != K_BYREF && c->stack[c->depth - 1] != K_I4)
			FAIL(c, "mkrefany of a non-pointer");
		CORINFO_CLASS_HANDLE cls;
		if (!load_class_of_token(c, 3, il_u32(c, at + 1), TOKENKIND_CLASS, &cls))
			return false;
		emit(c, ppc_stw(3, c->inst_temp, c->fp));
		load_entry(c, 0, 11, 0);
		if (!pop(c, 1) || !push_refany(c))
			return false;
		emit(c, ppc_stw(11, entry_slot(c, 0), c->fp));
		emit(c, ppc_lwz(0, c->inst_temp, c->fp));
		emit(c, ppc_stw(0, entry_slot(c, 0) + 4, c->fp));
		return true;
	}
	case 0x74: case 0x75: { // castclass, isinst
		*pc = at + 5;
		return op_cast(c, il_u32(c, at + 1), TOKENKIND_CASTING, op == 0x74 ? HELP_CHKCASTANY : HELP_ISINSTANCEOFANY);
	}
	case 0x8C: // box
		*pc = at + 5;
		return op_box(c, il_u32(c, at + 1));
	case 0xA5: // unbox.any
		*pc = at + 5;
		return op_unbox_any(c, il_u32(c, at + 1));
	case 0x8D: // newarr
		*pc = at + 5;
		return op_newarr(c, il_u32(c, at + 1));
	case 0x8E: // ldlen
		if (c->stack[c->depth - 1] != K_REF) FAIL(c, "ldlen");
		load_entry(c, 0, 11, 0);
		if (!null_check(c, 11)) return false;
		emit(c, ppc_lwz(11, ARRAY_LENGTH, 11));
		return pop(c, 1) && store_push(c, K_I4, 11, 0);
	case 0x8F: case 0xA3: case 0xA4: { // ldelema, ldelem, stelem with a type token
		*pc = at + 5;
		CORINFO_CLASS_HANDLE cls = resolve_class(c, il_u32(c, at + 1));
		if (!cls) FAIL_UNLESS_SET(c, "element class");
		const uint32_t t = class_type(c, cls);
		if (t == CORINFO_TYPE_VALUECLASS)
			return op == 0x8F ? op_ldelema_struct(c, cls) : op == 0xA3 ? op_ldelem_struct(c, cls) : op_stelem_struct(c, cls);
		return op == 0x8F ? op_ldelema(c, t, cls) : op == 0xA3 ? op_ldelem(c, t) : op_stelem(c, t);
	}
	case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: case 0x98: case 0x99:
	case 0x9A: { // ldelem.i1 ... ldelem.ref
		static const uint8_t types[] = {CORINFO_TYPE_BYTE, CORINFO_TYPE_UBYTE, CORINFO_TYPE_SHORT, CORINFO_TYPE_USHORT,
			CORINFO_TYPE_INT, CORINFO_TYPE_UINT, CORINFO_TYPE_LONG, CORINFO_TYPE_NATIVEINT, CORINFO_TYPE_FLOAT,
			CORINFO_TYPE_DOUBLE, CORINFO_TYPE_CLASS};
		return op_ldelem(c, types[op - 0x90]);
	}
	case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F: case 0xA0: case 0xA1: case 0xA2: { // stelem.i ... .ref
		static const uint8_t types[] = {CORINFO_TYPE_NATIVEINT, CORINFO_TYPE_BYTE, CORINFO_TYPE_SHORT, CORINFO_TYPE_INT,
			CORINFO_TYPE_LONG, CORINFO_TYPE_FLOAT, CORINFO_TYPE_DOUBLE, CORINFO_TYPE_CLASS};
		return op_stelem(c, types[op - 0x9B]);
	}
	case 0xFE: {
		const uint8_t op2 = (uint8_t)il_u8(c, at + 1);
		*pc = at + 2;
		switch (op2) {
		case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: { // ceq, cgt, cgt.un, clt, clt.un
			static const int kinds[] = {0, 0, 2, 7, 4, 9}; // -, ceq, cgt, cgt.un, clt, clt.un
			bool fp;
			if (!compare_top(c, op2 == 0x03 || op2 == 0x05, &fp)) return false;
			int bo, bi;
			condition(c, kinds[op2], fp, &bo, &bi);
			// r11 = 1; skip the clear when the condition holds.
			emit(c, ppc_li(11, 1));
			emit(c, ppc_bc(bo, bi, 8));
			emit(c, ppc_li(11, 0));
			return store_push(c, K_I4, 11, 0);
		}
		case 0x06: // ldftn
			*pc = at + 6;
			return op_ldftn(c, il_u32(c, at + 2));
		case 0x07: // ldvirtftn
			*pc = at + 6;
			return op_ldvirtftn(c, il_u32(c, at + 2));
		case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: { // ldarg, ldarga, starg, ldloc, ldloca, stloc
			const uint32_t i = il_u16(c, at + 2);
			*pc = at + 4;
			const bool is_arg = op2 <= 0x0B;
			if (i >= (is_arg ? c->args_count : c->locals_count)) FAIL(c, "variable index");
			const Var *v = is_arg ? &c->args[i] : &c->locals[i];
			const int form = (op2 - 0x09) % 3;
			return form == 0 ? load_var(c, v) : form == 1 ? var_address(c, v, pc) : store_var(c, v);
		}
		case 0x0F: // localloc
			return op_localloc(c);
		case 0x13: // volatile.: the access that follows is fenced on both sides (acquire and release)
			emit(c, PPC_LWSYNC);
			c->fence = 2;
			return true;
		case 0x19: // no. <checks>: checks the compiler may leave out; it leaves none out
			*pc = at + 3;
			return true;
		case 0x12: // unaligned. <alignment>: PowerPC loads and stores take any alignment; nothing to change
			*pc = at + 3;
			return true;
		case 0x17: // cpblk
		case 0x18: // initblk
			return op_block(c, op2 == 0x17);
		case 0x1C: { // sizeof <type>
			*pc = at + 6;
			CORINFO_CLASS_HANDLE cls = resolve_class_any(c, il_u32(c, at + 2), TOKENKIND_CLASS);
			if (!cls)
				FAIL_UNLESS_SET(c, "sizeof class");
			const uint32_t t = class_type(c, cls);
			// A type parameter of shared code is a reference type (only those share), 4 bytes like any reference.
			const uint32_t size = (t == CORINFO_TYPE_CLASS || t == CORINFO_TYPE_STRING || t == CORINFO_TYPE_VAR) ? 4
				: JI_CALL(c->info, VB_CLASS, CI_getClassSize, GetClassSizeFn, cls);
			return constant_i4(c, (int32_t)size);
		}
		case 0x1D: // refanytype
			return op_refanytype(c, *pc);
		case 0x1E: // readonly.: the ldelema that follows may skip its type check
			c->readonly = true;
			return true;
		case 0x15: // initobj
			*pc = at + 6;
			return op_initobj(c, il_u32(c, at + 2));
		case 0x16: { // constrained. <type>: for the callvirt that follows
			*pc = at + 6;
			CORINFO_CLASS_HANDLE cls = resolve_class_as(c, il_u32(c, at + 2), TOKENKIND_CONSTRAINED);
			if (!cls)
				FAIL_UNLESS_SET(c, "constrained type");
			if (*pc + 5 > c->il_size || c->il[*pc] != 0x6F)
				FAIL(c, "constrained prefix");
			c->constrained = cls;
			return true;
		}
		case 0x1A: // rethrow
			*falls_through = false;
			if (!c->funclet || c->clauses[c->funclet - 1].flags != EH_CATCH) FAIL(c, "rethrow outside a catch");
			return call_throw_helper(c, HELP_RETHROW);
		default:
			FAIL(c, fail_type(c, "two-byte opcode", il_u8(c, at + 1), 0));
		}
	}
	default:
		FAIL(c, fail_type(c, "opcode", op, 0));
	}
}

static bool mark_target(Ctx *c, uint32_t il)
{
	if (c->branch_target[il])
		return true;
	if (c->targets_count >= MAX_TARGETS)
		FAIL(c, "too many branch targets");
	c->target_depth[c->targets_count] = 0;
	c->branch_target[il] = (uint16_t)++c->targets_count;
	return true;
}

// Marks branch targets ahead of compilation, so the compiler knows where control flow joins.
static bool scan_targets(Ctx *c)
{
	uint32_t pc = 0;
	while (pc < c->il_size) {
		const uint8_t op = c->il[pc];
		uint32_t size = 1;
		int32_t target = -1;
		if (op == 0x2B || (op >= 0x2C && op <= 0x37)) {
			size = 2;
			target = (int32_t)(pc + 2) + (int8_t)c->il[pc + 1];
		} else if (op == 0x38 || (op >= 0x39 && op <= 0x44)) {
			size = 5;
			target = (int32_t)(pc + 5) + (int32_t)il_u32(c, pc + 1);
		} else if (op == 0x45) {
			const uint32_t count = il_u32(c, pc + 1);
			size = 5 + 4 * count;
			if (count > 1024 || pc + size > c->il_size)
				FAIL(c, "switch");
			for (uint32_t i = 0; i < count; i++) {
				const uint32_t t = pc + size + il_u32(c, pc + 5 + 4 * i);
				if (t >= c->il_size) FAIL(c, "switch target");
				if (!mark_target(c, t))
					return false;
			}
		} else if (op == 0x0E || op == 0x0F || op == 0x10 || op == 0x11 || op == 0x12 || op == 0x13 || op == 0x1F) {
			size = 2;
		} else if (op == 0x20 || op == 0x22 || op == 0x28 || op == 0x29 || op == 0x6F || op == 0x70 || op == 0x71 ||
			op == 0x72 || op == 0x73 || op == 0x74 || op == 0x75 || op == 0x79 || (op >= 0x7B && op <= 0x81) ||
			op == 0x8C || op == 0x8D || op == 0x8F || op == 0xA3 || op == 0xA4 || op == 0xA5 || op == 0xC2 ||
			op == 0xC6 || op == 0xD0 || op == 0xDD) {
			size = 5;
			if (op == 0xDD)
				target = (int32_t)(pc + 5) + (int32_t)il_u32(c, pc + 1);
			// A call to PermissionSet.Assert and its kin: the frame gets a security object (SecurityRuntime.Assert
			// threw ExecutionEngineException, "MissingSecurityDescriptor", for our AppDomain.CreateAppDomainManager
			// without one). The IL stubs' scope can't be searched this way.
			if ((op == 0x28 || op == 0x6F) && !c->security && !((uintptr_t)c->method->scope & 1) && pc + 5 <= c->il_size) {
				CORINFO_METHOD_HANDLE callee = JI_CALL(c->info, VB_MODULE, MOD_findMethod_, FindMethodFn,
					c->method->scope, il_u32(c, pc + 1), c->method->ftn, 0);
				if (callee && (JI_CALL(c->info, VB_METHOD, MI_getMethodAttribs, GetMethodAttribsFn, callee) &
					FLG_DONT_INLINE_CALLER))
					c->security = true;
			}
			if (op == 0x29)
				c->calli = true;
		} else if (op == 0x21 || op == 0x23) {
			size = 9;
		} else if (op == 0xDE) {
			size = 2;
			target = (int32_t)(pc + 2) + (int8_t)c->il[pc + 1];
		} else if (op == 0xFE) {
			const uint8_t op2 = c->il[pc + 1];
			if (op2 == 0x0F)
				c->localloc = true;
			size = (op2 >= 0x09 && op2 <= 0x0E) ? 4 : (op2 == 0x06 || op2 == 0x07 || op2 == 0x15 || op2 == 0x16 ||
				op2 == 0x1C) ? 6 : (op2 == 0x12 || op2 == 0x19) ? 3 : 2;
		}
		c->instruction_start[pc] = 1;
		if (target >= 0) {
			if ((uint32_t)target >= c->il_size || (uint32_t)target >= MAX_IL) FAIL(c, "branch target");
			if (!mark_target(c, (uint32_t)target))
				return false;
			if ((uint32_t)target <= pc && c->loops_count < MAX_LOOPS) {
				c->loops[c->loops_count].start = (uint32_t)target;
				c->loops[c->loops_count++].end = pc;
			}
			// A leave's thunks are placed with the main body, before catch funclets that return to them compile.
			// Every finally the leave exits gets its thunk now: one in the main body must be placed with it even
			// when only a funclet's leave (out of a try inside a catch) leads there.
			int32_t exited = (op == 0xDD || op == 0xDE) ? exited_finally(c, pc, (uint32_t)target, -1) : -1;
			for (; exited >= 0; exited = exited_finally(c, c->clauses[exited].try_start, (uint32_t)target, exited))
				if (thunk_for(c, (uint32_t)exited, (uint32_t)target) < 0)
					return false;
		}
		pc += size;
	}
	return true;
}

// Counts each variable's loads and stores for choose_saved_registers, eight times over inside each loop that holds
// them (capped), and marks those whose address is taken or that a handler uses (its funclet reaches the parent's
// variables through r31, in their frame slots).
static void count_uses(Ctx *c)
{
	for (uint32_t pc = 0; pc < c->il_size; pc++) {
		if (!c->instruction_start[pc])
			continue;
		const uint8_t op = c->il[pc];
		uint32_t kind = 2, index = 0;
		bool address = false;
		if (op >= 0x02 && op <= 0x05)
			kind = 0, index = op - 0x02u;
		else if (op >= 0x06 && op <= 0x09)
			kind = 1, index = op - 0x06u;
		else if (op >= 0x0A && op <= 0x0D)
			kind = 1, index = op - 0x0Au;
		else if (op >= 0x0E && op <= 0x13 && pc + 1 < c->il_size)
			kind = op <= 0x10 ? 0 : 1, index = c->il[pc + 1], address = op == 0x0F || op == 0x12;
		else if (op == 0xFE && pc + 3 < c->il_size && c->il[pc + 1] >= 0x09 && c->il[pc + 1] <= 0x0E)
			kind = c->il[pc + 1] <= 0x0B ? 0 : 1, index = il_u16(c, pc + 2),
				address = c->il[pc + 1] == 0x0A || c->il[pc + 1] == 0x0D;
		if (kind == 2 || index >= MAX_VARS)
			continue;
		bool in_handler = false;
		for (uint32_t i = 0; i < c->clauses_count && !in_handler; i++)
			in_handler = in_range(pc, c->clauses[i].handler_start, c->clauses[i].handler_end);
		if (address || in_handler)
			c->taken[kind][index] = 1;
		uint32_t weight = 1;
		for (uint32_t i = 0; i < c->loops_count && weight < 512; i++)
			if (pc >= c->loops[i].start && pc <= c->loops[i].end)
				weight *= 8;
		const uint32_t uses = c->uses[kind][index] + weight;
		c->uses[kind][index] = (uint16_t)(uses > 0xFFFF ? 0xFFFF : uses);
	}
}

// Compiles IL [pc, end): the main body (skipping handlers, and placing each try's leave thunks after its code), or
// a handler as a funclet.
// Whether clause i's try belongs to the code being compiled: the main body's are the ones in no handler, a
// funclet's the ones inside its handler.
static bool clause_in_scope(const Ctx *c, uint32_t i)
{
	const Clause *k = &c->clauses[i];
	if (c->funclet) {
		const Clause *h = &c->clauses[c->funclet - 1];
		return in_range(k->try_start, h->handler_start, h->handler_end);
	}
	for (uint32_t j = 0; j < c->clauses_count; j++)
		if (in_range(k->try_start, c->clauses[j].handler_start, c->clauses[j].handler_end))
			return false;
	return true;
}

// Whether the instruction at pc may leave entries in registers (see cache_register): it makes no call (and no
// safe point) and uses no register but r0, r11, r12, f0 and f12 besides the entries' own and r3/f1 at a return.
// Field accesses, ldlen and the primitive ldelem and stelem forms (whose range check calls only a throw helper, at
// which entries in registers are dead) qualify only without exception handlers (with them a null check calls a
// helper), a stfld of a primitive or a reference (whose write barrier keeps every register but r0, r11 and r12:
// write_barrier) but not of a value type, a ret only of a result in registers.
static bool simple_instruction(Ctx *c, uint32_t pc)
{
	const uint8_t op = c->il[pc];
	if (op <= 0x23 || op == 0x25 || op == 0x26 || (op >= 0x2B && op <= 0x44) || (op >= 0x46 && op <= 0x50) ||
		(op >= 0x52 && op <= 0x5A) || (op >= 0x5F && op <= 0x6E) || op == 0x76 || op == 0x7C ||
		(op >= 0xD1 && op <= 0xD3) || op == 0xDF || op == 0xE0)
		return op != 0x01 && op != 0x24;
	if (op == 0xFE)
		return pc + 1 < c->il_size && ((c->il[pc + 1] >= 0x01 && c->il[pc + 1] <= 0x05) ||
			(c->il[pc + 1] >= 0x09 && c->il[pc + 1] <= 0x0E));
	if (op == 0x2A)
		return c->return_size <= 8;
	if (c->eh)
		return false;
	if (op == 0x7B || op == 0x8E || (op >= 0x90 && op <= 0xA1))
		return true;
	if (op == 0x7D && pc + 5 <= c->il_size) {
		CORINFO_FIELD_HANDLE field = JI_CALL(c->info, VB_MODULE, MOD_findField, FindFieldFn, c->method->scope,
			il_u32(c, pc + 1), c->method->ftn);
		if (!field)
			return false;
		CORINFO_CLASS_HANDLE cls = 0;
		const uint32_t type = JI_CALL(c->info, VB_FIELD, FI_getFieldType, GetFieldTypeFn, field, &cls, 0);
		const enum Kind kind = kind_of(type);
		return kind != K_NONE;
	}
	return false;
}

// The variable the instruction at pc stores to (stloc, starg), or 0.
static const Var *stored_var(const Ctx *c, uint32_t pc)
{
	const uint8_t op = c->il[pc];
	uint32_t index;
	bool arg = false;
	if (op >= 0x0A && op <= 0x0D)
		index = op - 0x0Au;
	else if ((op == 0x13 || op == 0x10) && pc + 1 < c->il_size)
		index = c->il[pc + 1], arg = op == 0x10;
	else if (op == 0xFE && pc + 3 < c->il_size && (c->il[pc + 1] == 0x0B || c->il[pc + 1] == 0x0E))
		index = (uint32_t)c->il[pc + 2] | (uint32_t)c->il[pc + 3] << 8, arg = c->il[pc + 1] == 0x0B;
	else
		return 0;
	if (index >= (arg ? c->args_count : c->locals_count))
		return 0;
	return arg ? &c->args[index] : &c->locals[index];
}

static bool compile_range(Ctx *c, uint32_t pc, uint32_t end, bool reachable)
{
	while (pc < end) {
		if (c->clauses_count) {
			for (uint32_t i = 0; i < c->clauses_count; i++) {
				Clause *k = &c->clauses[i];
				if (!clause_in_scope(c, i))
					continue;
				if (k->try_start == pc && k->native_try_start < 0)
					k->native_try_start = (int32_t)c->n;
				if (k->try_end == pc && k->native_try_end < 0) {
					if (reachable)
						FAIL(c, "falls out of a try");
					k->native_try_end = (int32_t)c->n;
				}
			}
			for (uint32_t i = 0; i < c->clauses_count; i++)
				if (clause_in_scope(c, i) && c->clauses[i].try_end == pc && !place_thunks(c, i))
					return false;
			const int32_t h = handler_at(c, pc);
			if (h >= 0 && clause_in_scope(c, (uint32_t)h)) {
				if (reachable)
					FAIL(c, "falls into a handler");
				pc = c->clauses[h].handler_end;
				continue;
			}
		}
		if (c->branch_target[pc]) {
			const uint32_t t = c->branch_target[pc] - 1u;
			if (reachable) {
				flush(c);
				if (!note_target(c, pc))
					return false;
			} else if (c->target_depth[t]) {
				uncache(c);
				if (c->leaf) // its entries are in their registers on every path
					memset(c->cached, 1, sizeof c->cached);
				c->depth = c->target_depth[t] - 1u;
				for (uint32_t i = 0; i < c->depth && i < TARGET_ENTRIES; i++) {
					c->stack[i] = c->target_kinds[t][i];
					c->stack_cls[i] = c->target_cls[t][i];
					c->stack_size[i] = c->target_size[t][i];
					// The masks follow from the class (a TypedReference has none: its interior pointer).
					const bool refany = c->stack[i] == K_STRUCT && !c->stack_cls[i] && c->stack_size[i] == 8;
					c->stack_refs[i] = c->stack[i] == K_STRUCT ? struct_refs(c, c->stack_cls[i], c->stack_size[i]) : NO_REFS;
					c->stack_irefs[i] = refany ? first_word() :
						c->stack[i] == K_STRUCT ? struct_irefs(c, c->stack_cls[i], c->stack_size[i]) : NO_REFS;
				}
				if (c->depth > TARGET_ENTRIES)
					FAIL(c, "deep stack at a backward-only target");
			} else {
				c->depth = 0; // only reached by a later backward branch, with an empty stack (ECMA-335 III.1.7.5)
			}
		} else if (!reachable) {
			c->depth = 0;
		}
		c->labels[pc] = (int32_t)c->n;
		// A result stays in a register only when the next instruction can take it from there: one that would write
		// it back at once (a call, a join) gets it in its slot directly.
		// A call takes its arguments from their registers (load_arguments; op_call writes them back itself on its
		// other paths), so it counts as a consumer of register-held entries too, and its result may stay in one.
		const bool simple = simple_instruction(c, pc), call = c->il[pc] == 0x28 || c->il[pc] == 0x6F;
		if (!simple && !call)
			flush(c);
		uint32_t next = pc + 1;
		while (next < end && !c->instruction_start[next])
			next++;
		c->caching = (simple || call) && next < end && !c->branch_target[next] &&
			(simple_instruction(c, next) || c->il[next] == 0x28 || c->il[next] == 0x6F);
		const Var *stored = c->caching && simple && !c->leaf ? stored_var(c, next) : 0;
		c->fuse_reg = !stored ? 0 : stored->reg ? stored->reg : stored->freg;
#ifdef XNGEN_NO_FUSE // experiment: results go to their cache registers and the store copies them
		c->fuse_reg = 0;
#endif
		c->fuse_kind = stored ? stored->kind : 0;
		c->insn_depth = c->depth;
		if (c->leaf) {
			// Everything stays in registers; an instruction that may use another register as a temporary (or
			// call, or need memory) isn't for a leaf. ldstr and switch use r11 and r12 only; the prefixes none.
			const uint8_t op = c->il[pc], op2 = op == 0xFE && pc + 1 < c->il_size ? c->il[pc + 1] : 0;
			if (!simple && op != 0x72 && op != 0x45 && !(op == 0xFE && (op2 == 0x12 || op2 == 0x13 || op2 == 0x19 ||
				op2 == 0x1E)))
				FAIL(c, FAIL_NOT_LEAF);
			c->caching = true;
		}
		bool falls_through;
		const uint32_t at = pc;
		if (!compile_instruction(c, &pc, &falls_through) || c->failure) {
			c->fail_pc = at + 1;
			DbgPrint("xclrjit:   declined at IL %u (end %u): opcode 0x%02x, stack depth %u\n", at, end,
				at < c->il_size ? c->il[at] : 0xFF, c->depth);
			return false;
		}
		c->caching = false;
		c->fuse_reg = 0;
		if (c->fence && --c->fence == 0)
			emit(c, PPC_LWSYNC);
		reachable = falls_through;
		if (c->n >= MAX_CODE)
			FAIL(c, "code too large");
	}
	if (reachable)
		FAIL(c, c->funclet ? "falls out of a handler" : "falls off the end");
	return true;
}

static bool compile_body(Ctx *c)
{
	for (uint32_t i = 0; i < c->il_size; i++)
		c->labels[i] = -1;
	memset(c->branch_target, 0, c->il_size * sizeof c->branch_target[0]);
	memset(c->instruction_start, 0, c->il_size);
	c->targets_count = 0;
	c->stack_ref_count = 0;
	c->scalar_ref_count = 0;
	c->eh = c->method->EHcount != 0;
	c->localloc = false;
	c->calli = false;
	c->fence = 0;
	c->readonly = false;
	c->fp = c->eh ? 31 : 1;
	if ((c->eh && !load_clauses(c)) || !scan_targets(c))
		return false;
	count_uses(c);
	// localloc moves the stack pointer, so such a method addresses its frame from r31 as one with handlers does
	// (Microsoft's compiler lays both out the same way, and the unwinder restores r1 from r31).
	c->framed = c->eh || c->localloc;
	c->fp = c->framed ? 31 : 1;
	if (!layout(c))
		return false;
	prolog(c);
	// A static method or constructor of a class with a class constructor the runtime wants checked: run it first.
	if (JI_CALL(c->info, VB_CLASS, CI_initClass, InitClassFn, 0, c->method->ftn, c->method->ftn, 0) > 1 &&
		!init_class(c, JI_CALL(c->info, VB_METHOD, MI_getMethodClass, GetMethodClassFn, c->method->ftn)))
		return false;
	if (!compile_range(c, 0, c->il_size, true))
		return false;
	c->main_end = c->n;

	// The handlers, each a funclet after the main body. The exception handler (the personality routine) and its
	// data precede every function whose unwind word has the exception flag; the zapper writes the main body's.
	// Outer handlers first (clauses come inner first): a try inside a handler is compiled with that handler's
	// funclet, whose code the inner handler's funclet then refers to (a catch's resume address, a finally's thunk).
	void *personality = c->eh ? JI_CALL(c->info, 0, JI_getPersonalityRoutine, GetPersonalityRoutineFn) : 0;
	for (uint32_t n = c->clauses_count; n > 0; n--) {
		const uint32_t i = n - 1;
		Clause *k = &c->clauses[i];
		emit(c, (uint32_t)(uintptr_t)personality);
		emit(c, 0);
		k->funclet_start = (int32_t)c->n;
		c->funclet = i + 1;
		funclet_prolog(c);
		c->depth = 0;
		uncache(c);
		if (k->flags == EH_CATCH) {
			// The exception, in r4, starts the handler's evaluation stack.
			if (!push(c, K_REF))
				return false;
			emit(c, ppc_stw(4, slot_of(c, 0, K_REF), 31));
		}
		if (!compile_range(c, k->handler_start, k->handler_end, true))
			return false;
		k->funclet_end = (int32_t)c->n;
	}
	c->funclet = 0;
	for (uint32_t i = 0; i < c->thunks_count; i++)
		if (c->thunks[i].at < 0)
			FAIL(c, "leave thunk");

	for (uint32_t i = 0; i < c->fixups_count; i++) {
		const Fixup *f = &c->fixups[i];
		const int32_t to = f->to == TO_IL ? c->labels[f->target] :
			f->to == TO_THUNK ? c->thunks[f->target].at : c->clauses[f->target].funclet_start;
		if (to < 0)
			FAIL(c, "branch into an instruction");
#ifdef XNGEN_DUMP_STUBS
		if (c->dump && f->to != TO_IL)
			DbgPrint("xclrjit:   fixup at word %u to %u (kind %u) -> word %d\n", f->at, f->target, f->to, to);
#endif
		const int32_t d = (to - (int32_t)f->at) * 4;
		const uint32_t word = c->code[f->at];
		if ((word >> 26) == 18) {
			c->code[f->at] = ppc_b(d) | (word & 1);
		} else {
			if (d < -32768 || d > 32767)
				FAIL(c, FAIL_LONG_BRANCH);
			c->code[f->at] = (word & 0xFFFF0003u) | ((uint32_t)d & 0xFFFC);
		}
	}
	return true;
}

// Reports the clauses with native offsets: each clause's try and funclet, then a duplicated clause for each
// funclet that lies in IL inside another clause's try, so an exception leaving that funclet finds the outer handler.
static void report_eh(Ctx *c)
{
	uint32_t count = c->clauses_count;
	for (uint32_t i = 0; i < c->clauses_count; i++)
		for (uint32_t j = 0; j < c->clauses_count; j++)
			if (i != j && in_range(c->clauses[i].handler_start, c->clauses[j].try_start, c->clauses[j].try_end))
				count++;
	JI_CALL(c->info, 0, JI_setEHcount, SetEHcountFn, count);
	uint32_t index = 0;
	for (uint32_t i = 0; i < c->clauses_count; i++) {
		const Clause *k = &c->clauses[i];
		const EHClause e = {k->flags, (uint32_t)k->native_try_start * 4, (uint32_t)k->native_try_end * 4,
			(uint32_t)k->funclet_start * 4, (uint32_t)k->funclet_end * 4, k->class_token};
		JI_CALL(c->info, 0, JI_setEHinfo, SetEHinfoFn, index++, &e);
	}
	for (uint32_t i = 0; i < c->clauses_count; i++) {
		const Clause *inner = &c->clauses[i];
		for (uint32_t j = 0; j < c->clauses_count; j++) {
			const Clause *k = &c->clauses[j];
			if (i == j || !in_range(inner->handler_start, k->try_start, k->try_end))
				continue;
			const EHClause e = {k->flags | EH_DUPLICATED, (uint32_t)inner->funclet_start * 4,
				(uint32_t)inner->funclet_end * 4, (uint32_t)k->funclet_start * 4, (uint32_t)k->funclet_end * 4,
				k->class_token};
			JI_CALL(c->info, 0, JI_setEHinfo, SetEHinfoFn, index++, &e);
		}
	}
}

// A GC slot's liveness code for publish: SCALAR_SLOT | (entry * 2 + interior) for an evaluation stack entry's
// slot (reported where the entry holds a reference), SCALAR_NEVER for one that never does (left out), 0 for the
// variables and temporaries, reported wherever the frame is.
enum { SCALAR_SLOT = 0x80000000u, SCALAR_NEVER = 0xFFFFFFFFu };

static uint32_t scalar_slot(const Ctx *c, int32_t at, const uint8_t *ever)
{
	// With handlers, a funclet's evaluation stack lives in the same slots, and while it runs the runtime reports
	// the parent's frame at the parent's safe point: every slot is reported, and cleared in the prolog.
	if (c->eh)
		return 0;
	for (uint32_t interior = 0; interior < 2; interior++) {
		const int32_t start = interior ? c->stack_byref : c->stack_ref;
		if (at >= start && at < start + 4 * (int32_t)c->max_stack) {
			const uint32_t code = (uint32_t)(at - start) / 4 * 2 + interior;
			return ever[code] ? SCALAR_SLOT | code : SCALAR_NEVER;
		}
	}
	return 0;
}

// Hands the finished code, unwind words, exception clauses and GC info to the zapper.
static int32_t publish(Ctx *c, uint8_t **entry, uint32_t *code_bytes, uint32_t prolog_words)
{
	ICorJitInfo *info = c->info;
	const uint32_t bytes = c->n * 4;
	const uint32_t main_words = c->eh ? c->main_end : c->n;
	JI_CALL(info, 0, JI_reserveUnwindInfo, ReserveUnwindInfoFn, 0, 0, 4);
	void *hot = 0, *cold = 0, *ro = 0;
	// The floating-point constants' read-only data, 4 bytes more than they take so that they can start 8-byte aligned
	// whatever the block's alignment.
	const uint32_t ro_bytes = c->constants_count ? c->constants_count * 8 + 4 : 0;
	JI_CALL(info, 0, JI_allocMem, AllocMemFn, bytes, 0, ro_bytes, c->clauses_count, 0, &hot, &cold, &ro);
	if (!hot || (ro_bytes && !ro)) {
		c->failure = "allocMem gave no memory";
		return CORJIT_OUTOFMEM;
	}
	memcpy(hot, c->code, bytes);
	uint8_t *constants = (uint8_t *)(((uintptr_t)ro + 7) & ~(uintptr_t)7);
	if (ro_bytes)
		memcpy(constants, c->constants, c->constants_count * 8);
	for (uint32_t i = 0; i < c->relocs_count; i++) {
		const Reloc *r = &c->relocs[i];
		void *target = r->local == RELOC_CODE ? (uint8_t *)hot + (uintptr_t)r->target * 4 :
			r->local == RELOC_CONSTANT ? constants + (uintptr_t)r->target * 8 : r->target;
#ifdef XNGEN_DUMP_STUBS
		if (c->dump)
			DbgPrint("xclrjit:   reloc at word %u type %x local %u target %08X\n", r->at, r->type, r->local,
				(uint32_t)(uintptr_t)r->target);
#endif
		JI_CALL(info, 0, JI_recordRelocation, RecordRelocationFn, (uint8_t *)hot + r->at * 4, target, r->type, 0, 0);
	}

	// The second word of each function's RUNTIME_FUNCTION: the exception and 32-bit flags, its length and its
	// prolog's, in instructions.
	uint32_t unwind = 0xC0000000u | main_words << 8 | prolog_words;
	JI_CALL(info, 0, JI_allocUnwindInfo, AllocUnwindInfoFn, hot, 0, 0, main_words * 4, 4, (uint8_t *)&unwind,
		FUNC_ROOT);
	for (uint32_t i = 0; i < c->clauses_count; i++) {
		const Clause *k = &c->clauses[i];
		const uint32_t words = (uint32_t)(k->funclet_end - k->funclet_start);
		uint32_t funclet_unwind = 0xC0000000u | words << 8 | FUNCLET_PROLOG_WORDS;
		JI_CALL(info, 0, JI_reserveUnwindInfo, ReserveUnwindInfoFn, 1, 0, 4);
		JI_CALL(info, 0, JI_allocUnwindInfo, AllocUnwindInfoFn, hot, 0, (uint32_t)k->funclet_start * 4,
			(uint32_t)k->funclet_end * 4, 4, (uint8_t *)&funclet_unwind, FUNC_HANDLER);
	}
	if (c->eh)
		report_eh(c);

	// Safe points in order, merging duplicates (a catch's resume address can also be a call's return address):
	// order[] holds the recorded safe points sorted by offset, and group[g] .. group[g + 1] those at the g-th offset.
	static uint32_t order[MAX_SAFE_POINTS], group[MAX_SAFE_POINTS + 1], offsets[MAX_SAFE_POINTS];
	for (uint32_t i = 0; i < c->safe_points_count; i++) {
		uint32_t j = i;
		for (; j > 0 && c->safe_points[order[j - 1]] > c->safe_points[i]; j--)
			order[j] = order[j - 1];
		order[j] = i;
	}
	uint32_t safe_points = 0;
	for (uint32_t i = 0; i < c->safe_points_count; i++)
		if (!i || c->safe_points[order[i]] != c->safe_points[order[i - 1]]) {
			group[safe_points] = i;
			offsets[safe_points++] = c->safe_points[order[i]];
		}
	group[safe_points] = c->safe_points_count;

	// With exception handlers, slots are relative to r31 (the frame in the main body and its funclets alike), as in
	// Microsoft's images; otherwise to the caller's stack pointer.
	// The slots: interior pointers first, then references in ascending order (the encoding's delta form): the
	// reference words of value types on the evaluation stack (those live at some safe point) and in variables, and
	// the reference region. A stack word is live at the safe points where its entry holds references; the rest
	// wherever the frame is reported.
	static GcSlot slots[MAX_SLOTS];
	static uint32_t stack_bit[MAX_SLOTS];  // a stack word's (entry * 2 + interior) * REF_WORDS + word + 1; 0 for the others
	uint32_t slot_count = 0;
	const int32_t base = c->framed ? 0 : c->frame;
	// Variables in callee-saved registers (choose_saved_registers): interior pointers, then references, each by
	// register; live wherever the frame is (the prolog clears them).
	GcSlot reg_slots[SAVED_GPRS];
	uint32_t reg_count = 0;
	for (uint32_t kind = 0; kind < 2 && !c->leaf; kind++)
		for (int r = 31 - (int)c->saved_count; r <= 30; r++)
			for (uint32_t pass = 0; pass < 2; pass++) {
				const Var *vars = pass ? c->locals : c->args;
				const uint32_t count = pass ? c->locals_count : c->args_count;
				for (uint32_t i = 0; i < count; i++)
					if (vars[i].reg == r && vars[i].kind == (kind ? K_REF : K_BYREF))
						reg_slots[reg_count++] = (GcSlot){r, kind ? 0 : GC_SLOT_INTERIOR};
			}
	RefMask ever[2][MAX_STACK];
	memset(ever, 0, sizeof ever);
	for (uint32_t i = 0; i < c->stack_ref_count; i++) {
		mask_or(&ever[0][c->stack_ref_entries[i].entry], &c->stack_ref_entries[i].irefs);
		mask_or(&ever[1][c->stack_ref_entries[i].entry], &c->stack_ref_entries[i].refs);
	}
	uint8_t scalar_ever[2 * MAX_STACK];
	memset(scalar_ever, 0, sizeof scalar_ever);
	for (uint32_t i = 0; i < c->scalar_ref_count; i++)
		scalar_ever[c->scalar_ref_entries[i]] = 1;
	// Two passes in the encoding's order: interior pointers, then references; within each the value type words
	// on the evaluation stack (those live at some safe point), in variables, then the region.
	uint32_t interior_count = 0;
	for (uint32_t kind = 0; kind < 2; kind++) {
		const uint32_t flag = kind ? 0 : GC_SLOT_INTERIOR;
		for (uint32_t i = 0; i < c->max_stack; i++)
			for (uint32_t w = 0; w < REF_WORDS; w++)
				if (mask_bit(&ever[kind][i], w)) {
					if (slot_count >= MAX_SLOTS) {
						c->failure = "too many GC slots";
						return CORJIT_BADCODE;
					}
					slots[slot_count] = (GcSlot){c->stack_struct + c->struct_slot * (int32_t)i + 4 * (int32_t)w - base, flag};
					stack_bit[slot_count++] = i * 2 * REF_WORDS + (kind ? 0 : REF_WORDS) + w + 1;
				}
		for (uint32_t pass = 0; pass < 2; pass++) {
			const Var *vars = pass ? c->locals : c->args;
			const uint32_t count = pass ? c->locals_count : c->args_count;
			for (uint32_t i = 0; i < count; i++)
				for (uint32_t w = 0; vars[i].kind == K_STRUCT && w < vars[i].size / 4 && w < REF_WORDS; w++)
					if (mask_bit(kind ? &vars[i].refs : &vars[i].irefs, w)) {
						if (slot_count >= MAX_SLOTS) {
							c->failure = "too many GC slots";
							return CORJIT_BADCODE;
						}
						slots[slot_count] = (GcSlot){vars[i].offset + 4 * (int32_t)w - base, flag};
						stack_bit[slot_count++] = 0;
					}
		}
		if (kind == 0) {
			for (int32_t at = c->interior_start; at < c->interior_end; at += 4) {
				const uint32_t code = scalar_slot(c, at, scalar_ever);
				if (code == SCALAR_NEVER)
					continue;
				if (slot_count >= MAX_SLOTS) {
					c->failure = "too many GC slots";
					return CORJIT_BADCODE;
				}
				slots[slot_count] = (GcSlot){at - base, GC_SLOT_INTERIOR}, stack_bit[slot_count++] = code;
			}
			interior_count = slot_count;
		}
	}
	for (int32_t at = c->interior_end; at < c->gc_end; at += 4) {
		const uint32_t code = scalar_slot(c, at, scalar_ever);
		if (code == SCALAR_NEVER)
			continue;
		if (slot_count >= MAX_SLOTS) {
			c->failure = "too many GC slots";
			return CORJIT_BADCODE;
		}
		slots[slot_count] = (GcSlot){at - base, 0};
		stack_bit[slot_count++] = code;
	}
	for (uint32_t i = interior_count + 1; i < slot_count; i++)
		for (uint32_t j = i; j > interior_count && slots[j - 1].offset > slots[j].offset; j--) {
			const GcSlot t = slots[j]; slots[j] = slots[j - 1]; slots[j - 1] = t;
			const uint32_t u = stack_bit[j]; stack_bit[j] = stack_bit[j - 1]; stack_bit[j - 1] = u;
		}
	static uint8_t live_bits[MAX_SAFE_POINTS][MAX_SLOTS / 8];
	RefMask now[2][MAX_STACK];
	memset(now, 0, sizeof now);
	uint8_t scalar_now[2 * MAX_STACK];
	memset(scalar_now, 0, sizeof scalar_now);
	if (reg_count + slot_count > MAX_SLOTS) {
		c->failure = "too many GC slots";
		return CORJIT_BADCODE;
	}
	for (uint32_t s = 0; s < safe_points; s++) {
		memset(live_bits[s], 0, (reg_count + slot_count + 7) / 8);
		bool frame_live = false;
		for (uint32_t g = group[s]; g < group[s + 1]; g++) {
			const uint32_t sp = order[g];
			frame_live |= c->safe_point_live[sp] != 0;
			for (uint32_t e = c->stack_refs_at[sp]; e < c->stack_refs_at[sp + 1]; e++) {
				mask_or(&now[0][c->stack_ref_entries[e].entry], &c->stack_ref_entries[e].irefs);
				mask_or(&now[1][c->stack_ref_entries[e].entry], &c->stack_ref_entries[e].refs);
			}
			for (uint32_t e = c->scalar_refs_at[sp]; e < c->scalar_refs_at[sp + 1]; e++)
				scalar_now[c->scalar_ref_entries[e]] = 1;
		}
		for (uint32_t i = 0; i < slot_count; i++) {
			bool live;
			if (stack_bit[i] & SCALAR_SLOT) {
				live = scalar_now[stack_bit[i] & ~SCALAR_SLOT] != 0;
			} else if (stack_bit[i]) {
				const uint32_t b = stack_bit[i] - 1;
				live = mask_bit(&now[b % (2 * REF_WORDS) >= REF_WORDS ? 0 : 1][b / (2 * REF_WORDS)], b % REF_WORDS);
			} else {
				live = frame_live;
			}
			if (live)
				live_bits[s][(reg_count + i) / 8] |= (uint8_t)(1u << ((reg_count + i) % 8));
		}
		for (uint32_t i = 0; i < reg_count && frame_live; i++)
			live_bits[s][i / 8] |= (uint8_t)(1u << (i % 8));
		for (uint32_t g = group[s]; g < group[s + 1]; g++) {
			const uint32_t sp = order[g];
			for (uint32_t e = c->stack_refs_at[sp]; e < c->stack_refs_at[sp + 1]; e++)
				now[0][c->stack_ref_entries[e].entry] = now[1][c->stack_ref_entries[e].entry] = NO_REFS;
			for (uint32_t e = c->scalar_refs_at[sp]; e < c->scalar_refs_at[sp + 1]; e++)
				scalar_now[c->scalar_ref_entries[e]] = 0;
		}
	}
	// Shared generic code declares where its instantiation context lives, as Microsoft's images do (header flags
	// 0x10 from a MethodTable argument, 0x20 from a MethodDesc argument, 0x30 from `this`; the slot's offset from
	// the caller's SP, like the PSPSym's): the runtime's stack walks read it for the exact instantiation (a catch
	// clause naming T, GetCurrentMethod, the security and resource stack marks).
	uint32_t generics_kind = 0;
	int32_t generics_slot = 0;
	if (prolog_words && (JI_CALL(info, VB_METHOD, MI_getMethodAttribs, GetMethodAttribsFn, c->method->ftn) & FLG_SHAREDINST)) {
		if (c->has_inst_arg) {
			generics_kind = c->method->args.sigInst.methInstCount ? GC_GENERICS_FROM_METHODDESC : GC_GENERICS_FROM_METHODTABLE;
			generics_slot = c->inst_arg - c->frame;
		} else if ((sig_call_conv(&c->method->args) & CORINFO_CALLCONV_HASTHIS) && c->args_count && c->args[0].kind == K_REF) {
			generics_kind = GC_GENERICS_FROM_THIS;
			generics_slot = c->args[0].offset - c->frame;
		}
	}
	static uint8_t gc[0x20000];
	const uint32_t gc_bytes = prolog_words
		? gcinfo_build(gc, sizeof gc, bytes, (uint32_t)c->outgoing, offsets, &live_bits[0][0], MAX_SLOTS / 8,
			safe_points, reg_slots, reg_count, slots, slot_count, c->framed ? 31 : 0, c->secobj ? c->secobj - c->frame : 0,
			prolog_words * 4, main_words * 4, generics_kind, generics_slot)
		: gcinfo_build(gc, sizeof gc, bytes, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
	if (!gc_bytes) {
		c->failure = "GC info";
		return CORJIT_BADCODE;
	}
	void *gc_block = JI_CALL(info, 0, JI_allocGCInfo, AllocGCInfoFn, gc_bytes);
	memcpy(gc_block, gc, gc_bytes);

	*entry = hot;
	*code_bytes = bytes;
	return CORJIT_OK;
}

static int32_t compile_magic(Ctx *c, uint8_t **entry, uint32_t *code_bytes)
{
	// The test hook test\NGenProbe uses: code its IL can't produce, so seeing 0x5EED proves the native image ran.
	c->n = 0;
	emit(c, 0x38605EED); // li r3, 0x5EED
	emit(c, PPC_BLR);
	return publish(c, entry, code_bytes, 0);
}

// Calling-convention probes for NGenBench: XNGenR<n> returns r<n>, XNGenF<n> returns f<n>, so an interpreted caller
// shows where its arguments arrive.
// XNGenS<hex>: returns the word at the caller's stack pointer + 0x<hex>, to find where arguments beyond r3-r10 land.
static int32_t compile_stack_probe(Ctx *c, const char *name, uint8_t **entry, uint32_t *code_bytes)
{
	int offset = 0;
	for (const char *p = name + 6; *p; p++)
		offset = offset * 16 + (*p >= 'A' ? *p - 'A' + 10 : *p - '0');
	c->n = 0;
	emit(c, ppc_lwz(3, offset, 1));
	emit(c, PPC_BLR);
	return publish(c, entry, code_bytes, 0);
}

static int32_t compile_register_probe(Ctx *c, const char *name, uint8_t **entry, uint32_t *code_bytes)
{
	const int r = (name[6] - '0') * (name[7] ? 10 : 1) + (name[7] ? name[7] - '0' : 0);
	c->n = 0;
	emit(c, name[5] == 'R' ? ppc_mr(3, r) : ppc_fmr(1, r));
	emit(c, PPC_BLR);
	return publish(c, entry, code_bytes, 0);
}

// Struct return probes: XNGenW returns an 8-byte struct, XNGenV a 16-byte one (two doubles). Each writes markers to
// the memory r3 points to, if r3 looks like a pointer (a hidden return buffer: 0x77 words, or 2.0 and 2.0), and sets
// r3/r4 to other markers (0xAAA:0xBBB, or 3.0 and 4.0) for a return in registers.
static int32_t compile_return_probe(Ctx *c, bool wide, uint8_t **entry, uint32_t *code_bytes)
{
	const uint32_t sldi32_r3 = 30u << 26 | 3u << 21 | 3u << 16 | 62u << 5 | 1u << 2 | 1u << 1;
	const uint32_t sldi32_r4 = 30u << 26 | 4u << 21 | 4u << 16 | 62u << 5 | 1u << 2 | 1u << 1;
	c->n = 0;
	emit(c, ppc_cmplwi(6, 3, 0x1000));
	const uint32_t skip = c->n;
	emit(c, ppc_bc(BO_TRUE, 6 * 4 + CR_LT, 0));
	if (wide) {
		emit(c, ppc_lis(0, 0x4000));
		emit(c, ppc_stw(0, 0, 3));
		emit(c, ppc_stw(0, 8, 3));
		emit(c, ppc_li(0, 0));
		emit(c, ppc_stw(0, 4, 3));
		emit(c, ppc_stw(0, 12, 3));
	} else {
		emit(c, ppc_li(0, 0x77));
		emit(c, ppc_stw(0, 0, 3));
		emit(c, ppc_stw(0, 4, 3));
	}
	c->code[skip] = ppc_bc(BO_TRUE, 6 * 4 + CR_LT, (int32_t)(c->n - skip) * 4);
	if (wide) {
		emit(c, ppc_lis(3, 0x4008));
		emit(c, sldi32_r3);
		emit(c, ppc_lis(4, 0x4010));
		emit(c, sldi32_r4);
	} else {
		emit(c, ppc_li(3, 0xAAA));
		emit(c, sldi32_r3);
		emit(c, ppc_ori(3, 3, 0xBBB));
	}
	emit(c, PPC_BLR);
	return publish(c, entry, code_bytes, 0);
}

// XNGenI, an instance method returning a 16-byte struct: writes 3.0, 3.0 through r3 if it points at a zero word (a
// fresh return buffer; an object starts with its method table), else 4.0, 4.0 through r4 if that does, showing which
// of this and the return buffer comes first.
static int32_t compile_instance_return_probe(Ctx *c, uint8_t **entry, uint32_t *code_bytes)
{
	c->n = 0;
	for (int r = 3; r <= 4; r++) {
		emit(c, ppc_lwz(11, 0, r));
		emit(c, ppc_cmpwi(6, 11, 0));
		const uint32_t next = c->n;
		emit(c, ppc_bc(BO_FALSE, 6 * 4 + CR_EQ, 0));
		emit(c, ppc_lis(0, r == 3 ? 0x4008 : 0x4010));
		emit(c, ppc_stw(0, 0, r));
		emit(c, ppc_stw(0, 8, r));
		emit(c, ppc_li(0, 0));
		emit(c, ppc_stw(0, 4, r));
		emit(c, ppc_stw(0, 12, r));
		emit(c, PPC_BLR);
		c->code[next] = ppc_bc(BO_FALSE, 6 * 4 + CR_EQ, (int32_t)(c->n - next) * 4);
	}
	emit(c, PPC_BLR);
	return publish(c, entry, code_bytes, 0);
}

static const char *method_name(ICorJitInfo *info, CORINFO_METHOD_HANDLE ftn, const char **class_name)
{
	*class_name = "?";
	return JI_CALL(info, VB_METHOD, MI_getMethodName, GetMethodNameFn, ftn, class_name);
}

static const char *il_offset_text(uint32_t offset)
{
	static char text[24] = " at IL 0x";
	size_t n = 9;
	for (int shift = 28; shift >= 0; shift -= 4)
		if (offset >> shift & 15 || shift == 0 || n > 9)
			text[n++] = "0123456789abcdef"[offset >> shift & 15];
	text[n] = 0;
	return text;
}

#ifdef XNGEN_DECLINE
// Whether `Class::method` is one of the ;-separated names in list.
static bool listed(const char *list, const char *class_name, const char *name)
{
	if (!class_name || !name)
		return false;
	for (const char *p = list; *p;) {
		const char *c = class_name, *q = p;
		while (*c && *q == *c)
			c++, q++;
		if (!*c && q[0] == ':' && q[1] == ':') {
			const char *n = name;
			q += 2;
			while (*n && *q == *n)
				n++, q++;
			if (!*n && (*q == ';' || !*q))
				return true;
		}
		while (*p && *p != ';')
			p++;
		if (*p)
			p++;
	}
	return false;
}
#endif

#define XNGEN_PAD_STR_(x) #x
#define XNGEN_PAD_STR(x) XNGEN_PAD_STR_(x)

int32_t xngen_compile(ICorJitInfo *info, CORINFO_METHOD_INFO *method, uint32_t flags, uint8_t **entry,
	uint32_t *code_bytes)
{
	(void)flags;
	*entry = 0;
	*code_bytes = 0;
	Ctx *c = &ctx_storage;
	memset(c, 0, CTX_CLEARED);
	c->info = info;
	c->method = method;
	c->il = method->ILCode;
	c->il_size = method->ILCodeSize;

	const char *class_name;
	const char *name = method_name(info, method->ftn, &class_name);
	int32_t result;
#ifdef XNGEN_DUMP_STUBS
	{
		// The zapper's ICorDynamicInfo table, once: which function each slot is (to name getCallInfo and its kin).
		static bool dumped;
		if (!dumped) {
			dumped = true;
			void *object;
			void *const *vtable = ji_interface(info, VB_DYNAMIC, &object);
			for (int i = 0; i < 32; i += 4)
				DbgPrint("xclrjit:   dynamic slots %d..%d: %08X %08X %08X %08X\n", i, i + 3,
					(uint32_t)(uintptr_t)vtable[i], (uint32_t)(uintptr_t)vtable[i + 1],
					(uint32_t)(uintptr_t)vtable[i + 2], (uint32_t)(uintptr_t)vtable[i + 3]);
		}
	}
#endif
#ifdef XNGEN_DUMP_STUBS
	// The IL of a runtime stub, with its call targets: what the runtime's P/Invoke stubs do.
	if (name && !strncmp(name, "IL_STUB_PInvoke", 15) && c->il_size <= 512) {
		char hex[1100];
		for (uint32_t i = 0; i < c->il_size; i++) {
			hex[2 * i] = "0123456789ABCDEF"[c->il[i] >> 4];
			hex[2 * i + 1] = "0123456789ABCDEF"[c->il[i] & 15];
		}
		hex[2 * c->il_size] = 0;
		DbgPrint("xclrjit:   stub IL %s (scope %08X)\n", hex, (uint32_t)(uintptr_t)method->scope);
		c->dump = true;
		for (uint32_t i = 0; i + 5 <= c->il_size; i++)
			if ((c->il[i] == 0x28 || c->il[i] == 0x6F) && (c->il[i + 4] == 0x06 || c->il[i + 4] == 0x0A)) {
				const uint32_t tok = il_u32(c, i + 1);
				CORINFO_METHOD_HANDLE m = JI_CALL(info, VB_MODULE, MOD_findMethod_, FindMethodFn, method->scope, tok,
					method->ftn, 0);
				const char *cn = 0;
				const char *mn = m ? method_name(info, m, &cn) : 0;
				DbgPrint("xclrjit:   call at %u token %08X %s::%s\n", i, tok, cn ? cn : "?", mn ? mn : "?");
			}
	}
#endif
	if (name && !strcmp(name, "get_XNGenMagic")) {
		result = compile_magic(c, entry, code_bytes);
	} else if (name && !strcmp(name, "XNGenI")) {
		result = compile_instance_return_probe(c, entry, code_bytes);
	} else if (name && (!strcmp(name, "XNGenW") || !strcmp(name, "XNGenV"))) {
		result = compile_return_probe(c, name[5] == 'V', entry, code_bytes);
	} else if (name && (!strncmp(name, "XNGenR", 6) || !strncmp(name, "XNGenF", 6))) {
		result = compile_register_probe(c, name, entry, code_bytes);
	} else if (name && !strncmp(name, "XNGenS", 6)) {
		result = compile_stack_probe(c, name, entry, code_bytes);
	} else if (c->il_size > MAX_IL) {
		c->failure = "IL too long";
		result = CORJIT_BADCODE;
#ifdef XNGEN_DECLINE
	} else if (listed(XNGEN_DECLINE, class_name, name)) {
		// Diagnosis: methods left to the interpreter by name, to find a miscompiled one
		// (make CFLAGS='... -DXNGEN_DECLINE="\"Class::Method;Other::Method\""').
		c->failure = "listed in XNGEN_DECLINE";
		result = CORJIT_BADCODE;
#endif
	} else {
		uint32_t out_slots = STACK_ARG_SLOTS, struct_slot = 0;
		bool long_branches = false, keep_secret = false, leaf = false, leaf_tried = false;
		const bool managed_calli = name && (!strcmp(name, "IL_STUB_ShuffleThunk") || !strcmp(name, "IL_STUB_MulticastInvoke"));
		uint32_t depth_seen = 0;
		for (;;) {
			c->outgoing = LINKAGE_BYTES + 8 * (int32_t)out_slots;
			c->struct_slot = (int32_t)struct_slot;
			c->long_branches = long_branches;
			c->keep_secret = keep_secret;
			c->managed_calli = managed_calli;
			c->leaf = leaf;
			c->leaf_depth = depth_seen;
			const bool done = compile_body(c);
			// A method that makes no call (not even to a throw helper: a frameless method's return address stays
			// in the link register) is compiled again without a frame, its variables in registers; if anything
			// in it needs memory after all, it is compiled as before.
			if (done && !leaf_tried && !c->safe_points_count && !c->eh && !c->framed) {
				leaf = leaf_tried = true;
				depth_seen = c->deepest;
			} else if (done) {
				result = publish(c, entry, code_bytes, leaf ? 0 :
					(c->framed ? EH_PROLOG_WORDS : 3) + c->saved_count + c->saved_fcount);
				break;
			} else if (leaf) {
#ifdef XNGEN_LEAF_STATS
				DbgPrint("xclrjit:   leaf: %s at IL %u opcode 0x%02x\n", c->failure ? c->failure : "?",
					c->fail_pc ? c->fail_pc - 1 : 0, c->fail_pc ? c->il[c->fail_pc - 1] : 0);
#endif
				leaf = false;
			}
			// A call needs more stack argument slots than the frame reserved, or a value type more room than a stack
			// entry has: once more with the area or room it needs.
			else if (c->failure == FAIL_OUTGOING && c->need_stack_slots > out_slots && c->need_stack_slots <= MAX_STACK_ARG_SLOTS)
				out_slots = c->need_stack_slots;
			else if (c->failure == FAIL_STRUCT_SLOT && c->need_struct_slot > struct_slot)
				struct_slot = c->need_struct_slot;
			else if (c->failure == FAIL_LONG_BRANCH && !long_branches)
				long_branches = true;
			else if (c->failure == FAIL_SECRET && !keep_secret)
				keep_secret = true;
			else {
				result = CORJIT_BADCODE;
				break;
			}
			memset(c, 0, CTX_CLEARED);
			c->info = info;
			c->method = method;
			c->il = method->ILCode;
			c->il_size = method->ILCodeSize;
		}
	}

	// The code address names the body in a guest stack dump: in the zapper it lies in the image under
	// construction, and when the runtime itself JITs a method through us (no native image of its own
	// assembly) it lies in the code heap.
	DbgPrint("xclrjit: %s %s::%s (%u bytes of IL) -> %u bytes at %08X flags %08X attribs %08X%s%s%s\n",
			result == CORJIT_OK ? "compiled" : "declined",
			class_name ? class_name : "?", name ? name : "?", method->ILCodeSize, *code_bytes, (uint32_t)(uintptr_t)*entry, flags,
			JI_CALL(info, VB_METHOD, MI_getMethodAttribs, GetMethodAttribsFn, method->ftn),
			c->failure ? ": " : "", c->failure ? c->failure : "", c->fail_pc ? il_offset_text(c->fail_pc - 1) : "");
	if (result == CORJIT_OK)
		compiled++;
	else
		declined++;
	return result;
}

void xngen_report(void)
{
#ifdef XNGEN_PAD // shifts the code layout, to reproduce layout-sensitive bugs: make CFLAGS='... -DXNGEN_PAD=3'
	__asm__ volatile(".rept " XNGEN_PAD_STR(XNGEN_PAD) "; nop; .endr");
#endif
	DbgPrint("xclrjit: %u methods compiled, %u declined\n", compiled, declined);
}

