// CLRCodeCachedll_0.xex: the executable memory xcoreclr carves its stubs from.
//
// The Xbox kernel gives a title no way to allocate executable memory, so xcoreclr's code heap (its allocator at
// 0x84DFB8E8, ClrVirtualAlloc's replacement) takes it from a module shipped for the purpose: it loads
// CLRCodeCachedll_0.xex, then _1 and _2 as each fills, finds the exports LINKHACKFUNCTIONPROC0 and
// LINKHACKFUNCTIONPROCLAST, reserves the range between them with xboxkrnl's XexReserveCodeBuffer (ordinal 0x36B) and
// hands out pieces of it. Without one, the first stub the runtime needs (an array's accessor stubs, a precode, a
// delegate's shuffle thunk) fails with E_OUTOFMEMORY, and the zapper fails the whole image with it. The platform's
// copy isn't in any package we have, so this is a stand-in: 4 MB between the two symbols in the module's code
// section, which xenia-edge executes from (it refuses to fetch code from a data page: "Code fetch fault" when the
// zapper ran a stub it had written there, compiling mscorlib) and lets the runtime write with
// --writable_code_segments=true (NGen-Package.ps1 passes it).
#include <stdint.h>

// Something read-only, so the image has the read-only page elf2xex expects before the code page.
const char codecache_tag[] __attribute__((used)) = "XboxNGen code cache: 4 MB";

__asm__(
	".text\n"
	".balign 65536\n"
	".globl LINKHACKFUNCTIONPROC0\n"
	"LINKHACKFUNCTIONPROC0:\n"
	".space " "4194304" "\n"
	".globl LINKHACKFUNCTIONPROCLAST\n"
	"LINKHACKFUNCTIONPROCLAST:\n"
	".long 0x4e800020\n"  // blr: the last export marks the end and is never run
	".text\n");

uint32_t codecache_entry(void *module, uint32_t reason, void *reserved)
{
	(void)module;
	(void)reason;
	(void)reserved;
	return 1;
}
