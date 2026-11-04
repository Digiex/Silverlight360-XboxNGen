// xclrjit.dll: the JIT xcoreclr's zapper loads to compile methods into native images.
//
// xcoreclr loads this module with XexLoadImage and finds two exports by name: sxsJitStartup, which it calls with its
// runtime callbacks, and getJit, which returns the ICorJitCompiler it calls compileMethod on for every method it
// precompiles (decompiled Zapper::InitEE, docs/RESEARCH.md). A method we don't compile is left as IL; the runtime
// interprets it when it runs.
#include <stdint.h>
#include <xsdk/kernel.h>

#include "jitee.h"
#include "compiler.h"

// The compiler keeps its state in one static block: one compilation at a time, should the runtime ever call from a
// second thread (its finalizer thread exists in the zapper process).
static RTL_CRITICAL_SECTION compile_lock;

static int32_t __attribute__((used)) compile_method(ICorJitCompiler *self, ICorJitInfo *info, CORINFO_METHOD_INFO *method,
	uint32_t flags, uint8_t **entry, uint32_t *code_bytes)
{
	(void)self;
	RtlEnterCriticalSection(&compile_lock);
	const int32_t result = xngen_compile(info, method, flags, entry, code_bytes);
	RtlLeaveCriticalSection(&compile_lock);
	return result;
}

static void clear_cache(ICorJitCompiler *self)
{
	(void)self;
}

static int32_t is_cache_cleanup_required(ICorJitCompiler *self)
{
	(void)self;
	return 0;
}

static void process_shutdown_work(ICorJitCompiler *self, void *static_info)
{
	(void)self;
	(void)static_info;
	xngen_report();
}

// Any slot past the ones Silverlight 5's CILJit has, in case this build calls one: it does nothing and returns 0.
static int32_t unknown_slot(ICorJitCompiler *self)
{
	(void)self;
	DbgPrint("xclrjit: an unknown ICorJitCompiler slot was called\n");
	return 0;
}

static void *const jit_vtable[] = {
	(void *)compile_method,
	(void *)clear_cache,
	(void *)is_cache_cleanup_required,
	(void *)process_shutdown_work,
	(void *)unknown_slot,
	(void *)unknown_slot,
	(void *)unknown_slot,
	(void *)unknown_slot,
};

static ICorJitCompiler jit = {jit_vtable};

// Exported by name. xcoreclr passes a structure whose first word is its module handle and a callback; we keep
// neither yet.
void sxsJitStartup(void *callbacks, void *get_function)
{
	(void)callbacks;
	(void)get_function;
	DbgPrint("xclrjit: sxsJitStartup\n");
}

ICorJitCompiler *getJit(void)
{
	return &jit;
}

// The module's entry, which the loader calls with DLL_PROCESS_ATTACH and the rest.
uint32_t xclrjit_entry(void *module, uint32_t reason, void *reserved)
{
	(void)module;
	(void)reserved;
	if (reason == 1) {
		RtlInitializeCriticalSection(&compile_lock);
		DbgPrint("xclrjit: loaded\n");
	}
	return 1;
}
