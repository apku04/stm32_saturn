/* STM32U073CBT6: 128KB Flash, 40KB SRAM */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 40K
}

ENTRY(Reset_Handler)

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        . = ALIGN(4);
    } >FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } >FLASH

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } >RAM AT> FLASH

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } >RAM

    /* .noinit — preserved across soft/IWDG/BOR resets, lost on full power-cycle.
     * Reset_Handler must NOT zero this region (it lies outside _sbss.._ebss). */
    .noinit (NOLOAD) :
    {
        . = ALIGN(4);
        _snoinit = .;
        KEEP(*(.noinit))
        KEEP(*(.noinit*))
        . = ALIGN(4);
        _enoinit = .;
    } >RAM

    end = .;
    _end = .;

    _estack = ORIGIN(RAM) + LENGTH(RAM);
}
