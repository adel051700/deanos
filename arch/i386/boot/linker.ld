/* The bootloader will look at this image and start execution at the symbol
   designated as the entry point. */
ENTRY(_start)

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
    }

    /* Text section follows */
    .text ALIGN(4K) : AT(ADDR(.text))
    {
        *(.text)
    }

    /* Read-only data */
    .rodata ALIGN(4K) : AT(ADDR(.rodata))
    {
        *(.rodata)
    }

    /* Read-write data (initialized) */
    .data ALIGN(4K) : AT(ADDR(.data))
    {
        *(.data)
    }

    /* Constructor table */
    .ctors ALIGN(4K) : AT(ADDR(.ctors))
    {
        __ctors_start = .;
        *(.ctors)
        *(.init_array)
        *(.init_array.*)
        __ctors_end = .;
    }

    /* Read-write data (uninitialized) and stack */
    .bss ALIGN(4K) : AT(ADDR(.bss))
    {
        *(COMMON)
        *(.bss)
    }

    /* Mark kernel end after the last section */
    _kernel_end = .;
    PROVIDE(end = .);
}