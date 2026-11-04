/* Lays xclrjit.dll out for xsdk's tools/elf2xex.py --dll, as xsdk's src/xsdk.ld lays out a title: read-only data,
   code and writable data each on their own 64 KB pages. Based on UnRar360's unrar360.ld. */
OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(xclrjit_entry)

SECTIONS
{
	/* Clear of the title (0x82000000), the Silverlight runtime's modules, the last of which ends at 0x8B860000, and
	   the native images from 0x941C0000 (the Plex 0.0.0.7 runtime's XEX headers). The Makefile's --base says the
	   same. */
	. = 0x8C800000 + 0x1000;
	.rdata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
		*(.sdata2 .sdata2.* .sbss2 .sbss2.*)
	}

	. = ALIGN(0x10000);
	.text : {
		*(.text.xsdk_thunk_*)
		*(.text .text.*)
	}

	. = ALIGN(0x10000);
	.data : {
		*(.data .data.* .gnu.linkonce.d.*)
		*(.sdata .sdata.*)
	}
	.bss : {
		*(.sbss .sbss.* .bss .bss.* COMMON)
	}
	/* xcoreclr loads its code cache module, CLRCodeCachedll_0.xex, at 0x8D000000. */
	ASSERT(. <= 0x8D000000, "xclrjit.dll would overlap the code cache module at 0x8D000000")

	.xsdk.imports 0 : {
		KEEP(*(.xsdk.imports))
	}
	.xsdk.exports 0 : {
		KEEP(*(.xsdk.exports))
	}

	/DISCARD/ : {
		*(.comment .note.* .gnu.attributes .eh_frame .preinit_array .init_array .fini_array)
	}
}
