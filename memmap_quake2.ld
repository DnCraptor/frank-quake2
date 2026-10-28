/*
 * frank-quake2 — Quake II for RP2350
 *
 * Linker script, derived from the pico-sdk 2.2 memmap_default.ld (through
 * frank-castle's memmap_castle.ld):
 *  - in PSRAM (0x11000000): the big engine tables (EXT_RAM_BSS, i.e.
 *    .ext_ram.bss, as on the ESP32), .psram_bss/.psram_data, and all the
 *    .data/.bss of the engine outside the renderer; the renderer's own
 *    state stays in SRAM (with the frame buffers and the z-buffer);
 *  - IRAM_ATTR code (.time_critical) runs from SRAM;
 *  - the firmware must end below the flash asset image (Q2_ASSETS_OFFSET,
 *    passed as __q2_assets_offset);
 *  - 32 KB main stack in its own RAM region;
 *  - TLS is emulated by GCC (__emutls_v.*, see src/rp2350/tls.c): .tdata
 *    and .tbss stay empty.
 */


MEMORY
{
    /* The Pico SDK's FLASH region describes the whole flash chip (or a
     * board-header default).  Here it must cover *only firmware*; remaining
     * capacity belongs to the asset image.  Do not use pico_override_flash_size:
     * that would also reduce the hardware_flash erase/programming limit. */
    FLASH(rx) : ORIGIN = 0x10000000, LENGTH = __q2_assets_offset
    RAM(rwx) : ORIGIN =  0x20000000, LENGTH = 512k - 32k
    STACK_RAM(rwx) : ORIGIN = 0x20078000, LENGTH = 32k
    PSRAM(rwx) : ORIGIN = 0x11000000, LENGTH = 8192k
    SCRATCH_X(rwx) : ORIGIN = 0x20080000, LENGTH = 4k
    SCRATCH_Y(rwx) : ORIGIN = 0x20081000, LENGTH = 4k
}

ENTRY(_entry_point)

SECTIONS
{
    .flash_begin : {
        __flash_binary_start = .;
    } > FLASH

    /* The bootrom will enter the image at the point indicated in your
       IMAGE_DEF, which is usually the reset handler of your vector table.

       The debugger will use the ELF entry point, which is the _entry_point
       symbol, and in our case is *different from the bootrom's entry point.*
       This is used to go back through the bootrom on debugger launches only,
       to perform the same initial flash setup that would be performed on a
       cold boot.
    */

    .text : {
        __logical_binary_start = .;
        KEEP (*(.vectors))
        KEEP (*(.binary_info_header))
        __binary_info_header_end = .;
        KEEP (*(.embedded_block))
        __embedded_block_end = .;
        KEEP (*(.reset))
        /* TODO revisit this now memset/memcpy/float in ROM */
        /* bit of a hack right now to exclude all floating point and time critical (e.g. memset, memcpy) code from
         * FLASH ... we will include any thing excluded here in .data below by default */
        *(.init)
        *libgcc.a:cmse_nonsecure_call.o
        /* frank-quake2: the renderer's world (bsp, edges, spans) and model
         * (alias, polyse) code runs from SRAM, out of the XIP cache */
        *(EXCLUDE_FILE(*libgcc.a:_udivmoddi4.o *libgcc.a:_aeabi_ldivmod.o *libc.a:*lib_a-mem*.o *libc.a:*libc_a-mem*.o *libg.a:*libc_a-mem*.o *libm.a: *r_polyse.c.o *r_alias.c.o *r_edge.c.o *r_rast.c.o *r_bsp.c.o) .text*)
        *(.flashtext*)
        *(.fini)
        /* Pull all c'tors into .text */
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)
        /* Followed by destructors */
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)

        . = ALIGN(4);
        /* preinit data */
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(SORT(.preinit_array.*)))
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);

        . = ALIGN(4);
        /* init data */
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);

        . = ALIGN(4);
        /* finit data */
        PROVIDE_HIDDEN (__fini_array_start = .);
        *(SORT(.fini_array.*))
        *(.fini_array)
        PROVIDE_HIDDEN (__fini_array_end = .);

        *(.eh_frame*)
        . = ALIGN(4);
    } > FLASH

    /* Note the boot2 section is optional, and should be discarded if there is
       no reference to it *inside* the binary, as it is not called by the
       bootrom. (The bootrom performs a simple best-effort XIP setup and
       leaves it to the binary to do anything more sophisticated.) However
       there is still a size limit of 256 bytes, to ensure the boot2 can be
       stored in boot RAM.

       Really this is a "XIP setup function" -- the name boot2 is historic and
       refers to its dual-purpose on RP2040, where it also handled vectoring
       from the bootrom into the user image.
    */

    .boot2 : {
        __boot2_start__ = .;
        *(.boot2)
        __boot2_end__ = .;
    } > FLASH

    ASSERT(__boot2_end__ - __boot2_start__ <= 256,
        "ERROR: Pico second stage bootloader must be no more than 256 bytes in size")

    .rodata : {
        *(EXCLUDE_FILE(*libgcc.a:_udivmoddi4.o *libgcc.a:_aeabi_ldivmod.o *libc.a:*lib_a-mem*.o *libm.a:) .rodata*)
        *(.srodata*)
        . = ALIGN(4);
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.flashdata*)))
        . = ALIGN(4);
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH
    __exidx_end = .;

    /* Machine inspectable binary information */
    . = ALIGN(4);
    __binary_info_start = .;
    .binary_info :
    {
        KEEP(*(.binary_info.keep.*))
        *(.binary_info.*)
    } > FLASH
    __binary_info_end = .;
    . = ALIGN(4);

    .ram_vector_table (NOLOAD): {
        *(.ram_vector_table)
    } > RAM

    .uninitialized_data (NOLOAD): {
        . = ALIGN(4);
        *(.uninitialized_data*)
    } > RAM

    /* ============ PSRAM ============ */
    .psram_data : {
        . = ALIGN(4);
        __psram_data_start__ = .;
        /* the engine outside the renderer: game, client, server, common */
        *quake2_core.dir/src/quake2/client/*.o*(.data .data.*)
        *quake2_core.dir/src/quake2/game/*.o*(.data .data.*)
        *quake2_core.dir/src/quake2/server/*.o*(.data .data.*)
        *quake2_core.dir/src/quake2/qcommon/*.o*(.data .data.*)
        *quake2_core.dir/src/quake2/other/*.o*(.data .data.*)
        *quake2_core.dir/src/quake2/net/*.o*(.data .data.*)
        *(.psram_data*)
        . = ALIGN(4);
        __psram_data_end__ = .;
    } > PSRAM AT> FLASH
    __psram_data_load__ = LOADADDR(.psram_data);

    .psram_bss (NOLOAD) : {
        . = ALIGN(4);
        __psram_bss_start__ = .;
        *quake2_core.dir/src/quake2/client/*.o*(.bss .bss.* COMMON)
        *quake2_core.dir/src/quake2/game/*.o*(.bss .bss.* COMMON)
        *quake2_core.dir/src/quake2/server/*.o*(.bss .bss.* COMMON)
        *quake2_core.dir/src/quake2/qcommon/*.o*(.bss .bss.* COMMON)
        *quake2_core.dir/src/quake2/other/*.o*(.bss .bss.* COMMON)
        *quake2_core.dir/src/quake2/net/*.o*(.bss .bss.* COMMON)
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.ext_ram.bss*)))
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.psram_bss*)))
        . = ALIGN(4);
        __psram_bss_end__ = .;
    } > PSRAM

    .psram_heap (NOLOAD) : {
        . = ALIGN(4096);
        __psram_heap_start__ = .;
    } > PSRAM
    /* ============ end PSRAM ============ */

    .data : {
        __data_start__ = .;
        *(vtable)

        *(.time_critical*)

        /* remaining .text and .rodata; i.e. stuff we exclude above because we want it in RAM */
        *(.text*)
        . = ALIGN(4);
        *(.rodata*)
        . = ALIGN(4);

        *(.data*)
        *(.sdata*)

        . = ALIGN(4);
        *(.after_data.*)
        . = ALIGN(4);
        /* preinit data */
        PROVIDE_HIDDEN (__mutex_array_start = .);
        KEEP(*(SORT(.mutex_array.*)))
        KEEP(*(.mutex_array))
        PROVIDE_HIDDEN (__mutex_array_end = .);

        *(.jcr)
        . = ALIGN(4);
    } > RAM AT> FLASH

    .tdata : {
        . = ALIGN(4);
		*(.tdata .tdata.* .gnu.linkonce.td.*)
        /* All data end */
        __tdata_end = .;
    } > RAM AT> FLASH
    PROVIDE(__data_end__ = .);

    /* __etext is (for backwards compatibility) the name of the .data init source pointer (...) */
    __etext = LOADADDR(.data);

    .tbss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        __tls_base = .;
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)

        __tls_end = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __tbss_end = .;

        *(.sram_bss*)
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.bss*)))
        *(COMMON)
        PROVIDE(__global_pointer$ = . + 2K);
        *(.sbss*)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    .heap (NOLOAD):
    {
        __end__ = .;
        end = __end__;
        KEEP(*(.heap*))
    } > RAM
    /* historically on GCC sbrk was growing past __HeapLimit to __StackLimit, however
       to be more compatible, we now set __HeapLimit explicitly to where the end of the heap is */
    __HeapLimit = ORIGIN(RAM) + LENGTH(RAM);

    /* Start and end symbols must be word-aligned */
    .scratch_x : {
        __scratch_x_start__ = .;
        *(.scratch_x.*)
        . = ALIGN(4);
        __scratch_x_end__ = .;
    } > SCRATCH_X AT > FLASH
    __scratch_x_source__ = LOADADDR(.scratch_x);

    .scratch_y : {
        __scratch_y_start__ = .;
        *(.scratch_y.hdmi_tab) /* PIO HDMI: 4 KB aligned, the bank's start */
        *(.scratch_y.*)
        . = ALIGN(4);
        __scratch_y_end__ = .;
    } > SCRATCH_Y AT > FLASH
    __scratch_y_source__ = LOADADDR(.scratch_y);

    /* .stack*_dummy section doesn't contains any symbols. It is only
     * used for linker to calculate size of stack sections, and assign
     * values to stack symbols later
     *
     * stack1 section may be empty/missing if platform_launch_core1 is not used */

    /* by default we put core 0 stack at the end of scratch Y, so that if core 1
     * stack is not used then all of SCRATCH_X is free.
     */
    .stack1_dummy (NOLOAD):
    {
        *(.stack1*)
    } > SCRATCH_X
    .stack_dummy (NOLOAD):
    {
        . = . + 32K;
    } > STACK_RAM

    .flash_end : {
        KEEP(*(.embedded_end_block*))
        PROVIDE(__flash_binary_end = .);
    } > FLASH =0xaa

    /* stack limit is poorly named, but historically is maximum heap ptr */
    __StackLimit = ORIGIN(STACK_RAM);
    __StackOneTop = ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X);
    __StackTop = ORIGIN(STACK_RAM) + LENGTH(STACK_RAM);
    __StackOneBottom = __StackOneTop - SIZEOF(.stack1_dummy);
    __StackBottom = ORIGIN(STACK_RAM);
    PROVIDE(__stack = __StackTop);

    /* picolibc and LLVM */
    PROVIDE (__heap_start = __end__);
    PROVIDE (__heap_end = __HeapLimit);
    PROVIDE( __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)) );
    PROVIDE( __tls_size_align = (__tls_size + __tls_align - 1) & ~(__tls_align - 1));
    PROVIDE( __arm32_tls_tcb_offset = MAX(8, __tls_align) );

    /* llvm-libc */
    PROVIDE (_end = __end__);
    PROVIDE (__llvm_libc_heap_limit = __HeapLimit);

    /* Check if data + heap + stack exceeds RAM limit */
    ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")

    ASSERT(__flash_binary_end <= ORIGIN(FLASH) + __q2_assets_offset,
        "ERROR: the firmware runs into the flash asset image (raise Q2_ASSETS_OFFSET)")

    ASSERT( __binary_info_header_end - __logical_binary_start <= 1024, "Binary info must be in first 1024 bytes of the binary")
    ASSERT( __embedded_block_end - __logical_binary_start <= 4096, "Embedded block must be in first 4096 bytes of the binary")

    /* todo assert on extra code */
}

