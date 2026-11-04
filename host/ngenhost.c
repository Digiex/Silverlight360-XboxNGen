// ngenhost.xex: runs xcoreclr's zapper on assemblies, the way Microsoft's build ran it on a console.
//
// It loads xcoreclr.dll from the package folder and calls its export ordinal 1, NGenWorker (decompiled at 0x84E0A6D0;
// desktop Silverlight 5 coreclr.dll names the same export NGenWorker@12 in its public PDB):
//
//   HRESULT NGenWorker(LPCWSTR assembly, LPCWSTR option_string, DWORD option_value, DWORD flags)
//
// which fills NGenOptions (assembly at +8, flags bit 0 as fInstrument at +0xC, option_value at +0x2C, option_string at
// +0x30), builds a Zapper and calls Zapper::Compile(assembly). The zapper loads the JIT from xclrjit.dll beside it.
//
// game:\ngen.txt lists one assembly path per line (for example D:\MyApp.dll). Results go to game:\ngenhost.log and,
// with dbgprint.txt beside the .xex, to xenia's log.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"

typedef int32_t (*NGenWorkerFn)(const uint16_t *assembly, const uint16_t *option_string, uint32_t option_value,
	uint32_t flags);

enum {
	NGEN_WORKER_ORDINAL = 1,
	MAX_PATH_CHARS = 260,
};

// Diagnostics. Xenia won't let a title write to another module's code, so research/patch_xex.py patches a copy of
// xcoreclr.dll instead: its EEPolicy::HandleFatalError (0x84C586E8 in the 4.0.338.9242 build, which every
// COR_E_EXECUTIONENGINE failure calls) jumps to xngen_fatal_hook, whose address the link map gives. The hook logs who
// called it and the guest stack, then stops the thread.
static void log_stack(uint32_t sp)
{
	// xcoreclr's MSVC frames: the back chain at 0(r1) and the return address 8 bytes below the caller's r1.
	for (int i = 0; i < 32 && sp; i++) {
		const uint32_t top = *(volatile uint32_t *)sp;
		if (top <= sp || top - sp > 0x100000)
			break;
		xsdk_log_write("ngenhost:   frame %2d sp %08X returns to %08X", i, (unsigned)top,
			(unsigned)*(volatile uint32_t *)(top - 8));
		sp = top;
	}
}

void __attribute__((used, noinline)) xngen_fatal_hook(uint32_t hr, uint32_t address, uint32_t message)
{
	uint32_t sp;
	__asm__ volatile("mr %0, 1" : "=r"(sp));
	xsdk_log_write("ngenhost: HandleFatalError(0x%08X, 0x%08X, 0x%08X) called from %08X", (unsigned)hr,
		(unsigned)address, (unsigned)message, (unsigned)(uintptr_t)__builtin_return_address(0));
	log_stack(sp);
	xsdk_log_close();
	for (;;)
		KeDelayExecutionThread(1, 0, &(int64_t){-10000000});
}

static void widen(const char *text, uint16_t *wide, size_t capacity)
{
	size_t i = 0;
	for (; text[i] && i + 1 < capacity; i++)
		wide[i] = (uint8_t)text[i];
	wide[i] = 0;
}

int main(void)
{
	xsdk_log_open();
	xsdk_log_write("ngenhost: start");

	HANDLE clr = 0;
	NTSTATUS status = XexLoadImage("game:\\xcoreclr.dll", XEX_LOAD_IMAGE_DLL, 0, &clr);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("ngenhost: XexLoadImage(xcoreclr.dll) failed: 0x%08X", (unsigned)status);
		return 1;
	}
	void *address = 0;
	status = XexGetProcedureAddress(clr, NGEN_WORKER_ORDINAL, &address);
	if (!NT_SUCCESS(status) || !address) {
		xsdk_log_write("ngenhost: no NGenWorker export: 0x%08X", (unsigned)status);
		return 1;
	}
	NGenWorkerFn ngen_worker = (NGenWorkerFn)address;
	xsdk_log_write("ngenhost: NGenWorker at %p, fatal hook at %p", address, (void *)xngen_fatal_hook);

	FILE *list = fopen("game:\\ngen.txt", "r");
	if (!list) {
		xsdk_log_write("ngenhost: no game:\\ngen.txt");
		return 1;
	}
	char line[MAX_PATH_CHARS];
	static const uint16_t empty[1] = {0};
	int failures = 0;
	while (fgets(line, sizeof line, list)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#')
			continue;
		uint16_t path[MAX_PATH_CHARS];
		widen(line, path, MAX_PATH_CHARS);
		xsdk_log_write("ngenhost: compiling %s", line);
		const int32_t hr = ngen_worker(path, empty, 0, 0);
		xsdk_log_write("ngenhost: %s: hr 0x%08X", line, (unsigned)hr);
		if (hr < 0)
			failures++;
	}
	fclose(list);
	xsdk_log_write("ngenhost: done, %d failed", failures);
	xsdk_log_close();
	return failures != 0;
}
