/*
 * Link the smoke payload where the machine layer puts a kernel: guest
 * physical 0x80000000, entered at the first byte.
 *
 * .bss is NOLOAD on purpose. The image is objcopy'd to a flat binary, so
 * anything in .bss is not in the file at all - it exists because
 * rv_machine_init() zeroes every byte of guest RAM past the image. That is
 * the same contract a Linux Image relies on for its own .bss.
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
	. = 0x80000000;

	.text : {
		*(.text.init)
		*(.text .text.*)
	}

	.rodata : {
		*(.rodata .rodata.*)
		*(.srodata .srodata.*)
	}

	. = ALIGN(8);
	.data : {
		*(.data .data.*)
		*(.sdata .sdata.*)
	}

	. = ALIGN(4096);
	.bss (NOLOAD) : {
		*(.sbss .sbss.*)
		*(.bss .bss.*)
		*(COMMON)
	}

	_end = .;
}
