/* tests/test.ld -- flat 32-bit image at 0x100000.
 *
 * .mbheader must be first (multiboot magic must be within the first 8192
 * bytes at a 4-byte aligned file offset) and immediately followed by .text,
 * so that the entry point is exactly load_address + 32.
 */

ENTRY(_start)

SECTIONS
{
	. = 0x00100000;

	.text : {
		*(.mbheader)
		*(.text)
	}

	. = 0x00101000;
	.bss (NOLOAD) : {
		*(.bss)
		*(COMMON)
	}

	/DISCARD/ : {
		*(.note*)
		*(.comment)
		*(.eh_frame*)
		*(.rel*)
		*(.debug*)
	}
}
