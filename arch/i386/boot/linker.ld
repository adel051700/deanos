/* The bootloader will look at this image and start execution at the symbol
   designated as the entry point. */
ENTRY(_start)

/* Define program headers to separate executable and writable segments */
PHDRS
{
    rotext PT_LOAD FLAGS(4);    /* 4 = R, readable only */
    rwtext PT_LOAD FLAGS(6);    /* 6 = RW, readable and writable */
}

/* Tell where the various sections of the object files will be put in the final
   kernel image. */
SECTIONS
{
    /* Begin putting sections at 1 MiB. */
    . = 1M;

    /* Mark kernel start as soon as we have a fixed location */
    _kernel_start = .;

    /* Multiboot header MUST be first and within the first 32KB */
    .multiboot ALIGN(4K) : AT(ADDR(.multiboot))
    {
        *(.multiboot)
    } :rotext

    /* Text section follows - executable and read-only */
    .text ALIGN(4K) : AT(ADDR(.text))
    {
        *(.text)
    } :rotext

    /* Read-only data - no execution permission */
    .rodata ALIGN(4K) : AT(ADDR(.rodata))
    {
        *(.rodata)
    } :rotext

    /* Read-write data (initialized) - no execution permission */
    .data ALIGN(4K) : AT(ADDR(.data))
    {
        *(.data)
    } :rwtext

    /* Constructor table - no execution permission */
    .ctors ALIGN(4K) : AT(ADDR(.ctors))
    {
        __ctors_start = .;
        *(.ctors)
        *(.init_array)
        *(.init_array.*)
        __ctors_end = .;
    } :rwtext

    /* Read-write data (uninitialized) and stack - no execution permission */
    .bss ALIGN(4K) : AT(ADDR(.bss))
    {
        *(COMMON)
        *(.bss)
    } :rwtext

    /* Mark kernel end after the last section */
    _kernel_end = .;
    PROVIDE(end = .);
}