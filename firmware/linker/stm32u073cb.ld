/* STM32U073CBT6: 128KB Flash, 40KB SRAM
 *
 * Flash is partitioned for OTA (see firmware/include/flash_ota.h):
 *   0x08000000 .. 0x0800FFFF  pages  0-31  64KB  Application (this image)
 *   0x08010000 .. 0x0801EFFF  pages 32-62  62KB  OTA staging bank
 *   0x0801F800 .. 0x0801FFFF  page  63      2KB  Device config
 * The application region below is therefore clamped to 64K so a build that
 * outgrows it fails at link time instead of silently overwriting the OTA bank. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
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

    /* .ramfunc — code that must execute from RAM, copied by Reset_Handler.
     * The OTA applier (ota_apply.c) lives here because it erases and rewrites
     * the application flash it would otherwise be executing from. */
    _siramfunc = LOADADDR(.ramfunc);

    .ramfunc :
    {
        . = ALIGN(4);
        _sramfunc = .;
        *(.ramfunc)
        *(.ramfunc*)
        . = ALIGN(4);
        _eramfunc = .;
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
