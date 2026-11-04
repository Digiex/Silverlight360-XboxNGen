// The JIT-to-runtime interface of xcoreclr 4.0.338.9242, as our JIT sees it (docs/RESEARCH.md, "The JIT interface").
//
// xcoreclr is compiled by MSVC for the Xbox 360: 32-bit pointers, big-endian, bitfields allocated from the low bit of
// their word. The ICorJitInfo the zapper passes is its ZapInfo object, whose interfaces are virtual bases: word 0 is
// the vtable of ICorJitInfo's own methods, word 1 points at a vbtable, and interface k is the sub-object at
// this + 4 + vbtable[k], called through its own vtable. Slot orders come from the live object and from Silverlight 4
// coreclr.dll's named ZapInfo vtables, the closest relative with symbols.
//
// The xsdk toolchain's -mcall-xsdk makes C calls through these pointers follow the console's convention: this in
// r3, the rest in r4-r10 and f1-f13.
#ifndef XNGEN_JITEE_H
#define XNGEN_JITEE_H

#include <stdint.h>

typedef int32_t HRESULT;
typedef uint16_t WCHAR;
typedef void *CORINFO_MODULE_HANDLE;
typedef void *CORINFO_CLASS_HANDLE;
typedef void *CORINFO_METHOD_HANDLE;
typedef void *CORINFO_FIELD_HANDLE;
typedef void *CORINFO_ARG_LIST_HANDLE;
typedef uint32_t mdToken;

// CorJitResult. xcoreclr's ThrowExceptionForJit (0x84D1DA18) makes 0x80000004 (CORJIT_SKIPPED in later CLRs) a fatal
// error; a JIT declines a method with CORJIT_BADCODE, which the zapper catches and counts.
enum {
	CORJIT_OK = 0,
	CORJIT_BADCODE = (int32_t)0x80000001,
	CORJIT_OUTOFMEM = (int32_t)0x80000002,
};

// CorInfoType.
enum {
	CORINFO_TYPE_UNDEF, CORINFO_TYPE_VOID, CORINFO_TYPE_BOOL, CORINFO_TYPE_CHAR, CORINFO_TYPE_BYTE,
	CORINFO_TYPE_UBYTE, CORINFO_TYPE_SHORT, CORINFO_TYPE_USHORT, CORINFO_TYPE_INT, CORINFO_TYPE_UINT,
	CORINFO_TYPE_LONG, CORINFO_TYPE_ULONG, CORINFO_TYPE_NATIVEINT, CORINFO_TYPE_NATIVEUINT, CORINFO_TYPE_FLOAT,
	CORINFO_TYPE_DOUBLE, CORINFO_TYPE_STRING, CORINFO_TYPE_PTR, CORINFO_TYPE_BYREF, CORINFO_TYPE_VALUECLASS,
	CORINFO_TYPE_CLASS, CORINFO_TYPE_REFANY, CORINFO_TYPE_VAR,
};

typedef struct {
	uint32_t classInstCount;
	CORINFO_CLASS_HANDLE *classInst;
	uint32_t methInstCount;
	CORINFO_CLASS_HANDLE *methInst;
} CORINFO_SIG_INST;

// CORINFO_SIG_INFO, 0x30 bytes. Observed in the CORINFO_METHOD_INFO xcoreclr's zapper passes: a getter shows
// packed 0x00002014 (CLASS, HASTHIS, no arguments), a setter 0x00012001 (VOID, HASTHIS, one argument), and the field
// after retTypeSigClass is the signature's length (pSig to args is the header's length).
typedef struct {
	CORINFO_CLASS_HANDLE retTypeClass;
	uint32_t packed; // retType | callConv << 8 | numArgs << 16
	CORINFO_CLASS_HANDLE retTypeSigClass;
	uint32_t cbSig;
	CORINFO_SIG_INST sigInst;
	CORINFO_ARG_LIST_HANDLE args;
	const uint8_t *pSig;
	CORINFO_MODULE_HANDLE scope;
	mdToken token;
} CORINFO_SIG_INFO;

static inline uint32_t sig_ret_type(const CORINFO_SIG_INFO *sig) { return sig->packed & 0xFF; }
static inline uint32_t sig_call_conv(const CORINFO_SIG_INFO *sig) { return (sig->packed >> 8) & 0xFF; }
static inline uint32_t sig_num_args(const CORINFO_SIG_INFO *sig) { return sig->packed >> 16; }

enum {
	CORINFO_CALLCONV_DEFAULT = 0x0,
	CORINFO_CALLCONV_VARARG = 0x5,
	CORINFO_CALLCONV_MASK = 0x0f,
	CORINFO_CALLCONV_GENERIC = 0x10,
	CORINFO_CALLCONV_HASTHIS = 0x20,
	CORINFO_CALLCONV_EXPLICITTHIS = 0x40,
	CORINFO_CALLCONV_PARAMTYPE = 0x80,
};

// CORINFO_METHOD_INFO, 0x80 bytes: ZapInfo::CompileMethod (0x84E2DAE0) clears 0x80 bytes for it, and
// ZapInfo::getMethodInfo (0x84E31D50) sets regionKind at +0x1C.
typedef struct {
	CORINFO_METHOD_HANDLE ftn;
	CORINFO_MODULE_HANDLE scope;
	const uint8_t *ILCode;
	uint32_t ILCodeSize;
	uint32_t maxStack;
	uint32_t EHcount;
	uint32_t options;
	uint32_t regionKind;
	CORINFO_SIG_INFO args;
	CORINFO_SIG_INFO locals;
} CORINFO_METHOD_INFO;

_Static_assert(sizeof(CORINFO_SIG_INFO) == 0x30, "CORINFO_SIG_INFO");
_Static_assert(sizeof(CORINFO_METHOD_INFO) == 0x80, "CORINFO_METHOD_INFO");

// The interfaces' vbtable indices in ZapInfo.
enum {
	VB_METHOD = 1,
	VB_MODULE = 2,
	VB_CLASS = 3,
	VB_FIELD = 4,
	VB_DEBUG = 5,
	VB_ARG = 6,
	VB_ERROR = 7,
	VB_STATIC = 8,
	VB_DYNAMIC = 9,
};

// ICorJitInfo's own slots, named by what xcoreclr's ZapInfo implementations do.
enum {
	JI_getMemoryManager = 0,
	JI_allocMem = 1,
	JI_reserveUnwindInfo = 2,
	JI_allocUnwindInfo = 3,
	JI_getPersonalityRoutine = 4, // Xbox-specific: returns 0x84BBBE90
	JI_allocGCInfo = 5,
	JI_yieldExecution = 6,
	JI_setEHcount = 7,
	JI_setEHinfo = 8,
	JI_logMsg = 9,
	JI_doAssert = 10,
	JI_reportFatalError = 11,
	JI_allocBBProfileBuffer = 12,
	JI_getBBProfileData = 13,
	JI_recordRelocation = 14,
	JI_getRelocTypeHint = 15,
	JI_getModuleNativeEntryPointRange = 16,
};

// ICorFieldInfo (vbtable 4): seven slots, the same as Silverlight 4's.
enum {
	FI_getFieldName = 0,
	FI_getFieldAttribs = 1,
	FI_getFieldClass = 2,
	FI_getFieldType = 3,
	FI_getFieldHelper = 4,
	FI_getFieldOffset = 5,
	FI_getFieldInfo = 6,
};

// ICorModuleInfo (vbtable 2): thirteen slots, Silverlight 4's twelve and one more; findField is assumed at its
// Silverlight 4 place and checked at run time against getFieldOffset.
enum {
	MOD_findClass = 0,
	MOD_findField = 1,
	MOD_findMethod = 2,
};

// ICorMethodInfo (vbtable 1): getMethodInfo is slot 6 (the zapper calls it there); slot 0 is checked at run time as
// Silverlight 4's getMethodName.
enum {
	MI_getMethodName = 0,
	MI_getMethodInfo = 6,
};

typedef struct ICorJitInfo {
	void *const *vtable;
	const int32_t *vbtable;
} ICorJitInfo;

static inline void *const *ji_interface(ICorJitInfo *info, int index, void **object)
{
	if (!index) {
		*object = info;
		return info->vtable;
	}
	void *sub = (uint8_t *)info + 4 + info->vbtable[index];
	*object = sub;
	return *(void *const **)sub;
}

// Calls slot `slot` of interface `vb` (0 for ICorJitInfo's own) with the given function type.
#define JI_CALL(info, vb, slot, type, ...)                                                        \
	({                                                                                             \
		void *ji_object_;                                                                          \
		void *const *ji_vtable_ = ji_interface((info), (vb), &ji_object_);                         \
		((type)ji_vtable_[(slot)])(ji_object_, ##__VA_ARGS__);                                     \
	})

typedef struct ICorJitCompiler {
	void *const *vtable;
} ICorJitCompiler;

#endif
