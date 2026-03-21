    ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = 0x08048000;

	.text : ALIGN(16)
	{
		*(.text*)
		*(.rodata*)
	} :text

	.data : ALIGN(16)
	{
		*(.data*)
	} :data

	.bss : ALIGN(16)
	{
		*(.bss*)
		*(COMMON)
	} :data
}

