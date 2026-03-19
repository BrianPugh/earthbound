/*
 * LakeSnes symbol prefix header
 *
 * Force-included on all LakeSnes translation units to rename public symbols
 * with an lk_ prefix, avoiding collisions with the port's own PPU/DMA/etc.
 */

#ifndef LK_PREFIX_H
#define LK_PREFIX_H

/* snes.c / snes.h / snes_other.c */
#define snes_init           lk_snes_init
#define snes_free           lk_snes_free
#define snes_reset          lk_snes_reset
#define snes_handleState    lk_snes_handleState
#define snes_runFrame       lk_snes_runFrame
#define snes_runCycles      lk_snes_runCycles
#define snes_syncCycles     lk_snes_syncCycles
#define snes_readBBus       lk_snes_readBBus
#define snes_writeBBus      lk_snes_writeBBus
#define snes_read           lk_snes_read
#define snes_write          lk_snes_write
#define snes_cpuIdle        lk_snes_cpuIdle
#define snes_cpuRead        lk_snes_cpuRead
#define snes_cpuWrite       lk_snes_cpuWrite
#define snes_runCpuCycle    lk_snes_runCpuCycle
#define snes_runSpcCycle    lk_snes_runSpcCycle
#define snes_loadRom        lk_snes_loadRom
#define snes_setButtonState lk_snes_setButtonState
#define snes_setPixelFormat lk_snes_setPixelFormat
#define snes_setPixels      lk_snes_setPixels
#define snes_setSamples     lk_snes_setSamples
#define snes_saveBattery    lk_snes_saveBattery
#define snes_loadBattery    lk_snes_loadBattery
#define snes_saveState      lk_snes_saveState
#define snes_loadState      lk_snes_loadState

/* cpu.c / cpu.h */
#define cpu_init            lk_cpu_init
#define cpu_free            lk_cpu_free
#define cpu_reset           lk_cpu_reset
#define cpu_handleState     lk_cpu_handleState
#define cpu_runOpcode       lk_cpu_runOpcode
#define cpu_nmi             lk_cpu_nmi
#define cpu_setIrq          lk_cpu_setIrq

/* ppu.c / ppu.h */
#define ppu_init            lk_ppu_init
#define ppu_free            lk_ppu_free
#define ppu_reset           lk_ppu_reset
#define ppu_handleState     lk_ppu_handleState
#define ppu_checkOverscan   lk_ppu_checkOverscan
#define ppu_handleVblank    lk_ppu_handleVblank
#define ppu_handleFrameStart lk_ppu_handleFrameStart
#define ppu_runLine         lk_ppu_runLine
#define ppu_read            lk_ppu_read
#define ppu_write           lk_ppu_write
#define ppu_putPixels       lk_ppu_putPixels
#define ppu_setPixelOutputFormat lk_ppu_setPixelOutputFormat

/* apu.c / apu.h */
#define apu_init            lk_apu_init
#define apu_free            lk_apu_free
#define apu_reset           lk_apu_reset
#define apu_handleState     lk_apu_handleState
#define apu_runCycles       lk_apu_runCycles
#define apu_read            lk_apu_read
#define apu_write           lk_apu_write
#define apu_spcRead         lk_apu_spcRead
#define apu_spcWrite        lk_apu_spcWrite
#define apu_spcIdle         lk_apu_spcIdle

/* dma.c / dma.h */
#define dma_init            lk_dma_init
#define dma_free            lk_dma_free
#define dma_reset           lk_dma_reset
#define dma_handleState     lk_dma_handleState
#define dma_read            lk_dma_read
#define dma_write           lk_dma_write
#define dma_handleDma       lk_dma_handleDma
#define dma_startDma        lk_dma_startDma

/* dsp.c / dsp.h */
#define dsp_init            lk_dsp_init
#define dsp_free            lk_dsp_free
#define dsp_reset           lk_dsp_reset
#define dsp_handleState     lk_dsp_handleState
#define dsp_cycle           lk_dsp_cycle
#define dsp_read            lk_dsp_read
#define dsp_write           lk_dsp_write
#define dsp_getSamples      lk_dsp_getSamples

/* spc.c / spc.h */
#define spc_init            lk_spc_init
#define spc_free            lk_spc_free
#define spc_reset           lk_spc_reset
#define spc_handleState     lk_spc_handleState
#define spc_runOpcode       lk_spc_runOpcode

/* cart.c / cart.h */
#define cart_init            lk_cart_init
#define cart_free            lk_cart_free
#define cart_reset           lk_cart_reset
#define cart_handleTypeState lk_cart_handleTypeState
#define cart_handleState     lk_cart_handleState
#define cart_load            lk_cart_load
#define cart_handleBattery   lk_cart_handleBattery
#define cart_read            lk_cart_read
#define cart_write           lk_cart_write

/* input.c / input.h */
#define input_init           lk_input_init
#define input_free           lk_input_free
#define input_reset          lk_input_reset
#define input_handleState    lk_input_handleState
#define input_latch          lk_input_latch
#define input_read           lk_input_read

/* statehandler.c / statehandler.h */
#define sh_init              lk_sh_init
#define sh_free              lk_sh_free
#define sh_handleBools       lk_sh_handleBools
#define sh_handleBytes       lk_sh_handleBytes
#define sh_handleBytesS      lk_sh_handleBytesS
#define sh_handleWords       lk_sh_handleWords
#define sh_handleWordsS      lk_sh_handleWordsS
#define sh_handleInts        lk_sh_handleInts
#define sh_handleIntsS       lk_sh_handleIntsS
#define sh_handleLongLongs   lk_sh_handleLongLongs
#define sh_handleFloats      lk_sh_handleFloats
#define sh_handleDoubles     lk_sh_handleDoubles
#define sh_handleByteArray   lk_sh_handleByteArray
#define sh_handleWordArray   lk_sh_handleWordArray
#define sh_placeInt          lk_sh_placeInt

#endif /* LK_PREFIX_H */
