/* Lays CLRCodeCachedll_0.xex out for xsdk's tools/elf2xex.py --dll: code and data on their own 64 KB pages. */
OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(codecache_entry)

SECTIONS
{
	/* Above xclrjit.dll (0x8C800000, under 1 MB) and below the native images from 0x941C0000. The Makefile's --base
	   says the same. */
	. = 0x8D000000 + 0x1000;
	.rdata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
	}

	. = ALIGN(0x10000);
	.text : {
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
