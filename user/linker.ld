    ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);
}

SECTIONS
{
	. = 0x08048000;

	.text : ALIGN(16)
	{
		*(.text*)
		*(.rodata*)
	} :text
}

