ENTRY(_start)

SECTIONS
{
    . = 0x4000000000;

    .text ALIGN(4K) : {
        *(.text*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata*)
    }

    .data ALIGN(4K) : {
        *(.data*)
    }

    .tdata ALIGN(8) : {
        KEEP(*(.tdata*))
    }

    .tbss ALIGN(8) : {
        KEEP(*(.tbss*))
    }

    .preinit_array ALIGN(8) : {
        PROVIDE(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE(__preinit_array_end = .);
    }

    .init_array ALIGN(8) : {
        PROVIDE(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE(__init_array_end = .);
    }

    .ctors ALIGN(8) : {
        PROVIDE(__ctors_start = .);
        KEEP(*(SORT_BY_NAME(.ctors.*)))
        KEEP(*(.ctors))
        PROVIDE(__ctors_end = .);
    }

    .fini_array ALIGN(8) : {
        PROVIDE(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE(__fini_array_end = .);
    }

    .dtors ALIGN(8) : {
        PROVIDE(__dtors_start = .);
        KEEP(*(SORT_BY_NAME(.dtors.*)))
        KEEP(*(.dtors))
        PROVIDE(__dtors_end = .);
    }

    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss*)
    }

    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.debug*)
        *(.eh_frame*)
        *(.eh_frame_hdr*)
        *(.gcc_except_table*)
        *(.interp)
    }
}
