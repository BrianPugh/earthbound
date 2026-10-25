#include "st7789_rp2040.h"
#include "hal/hal.h"
#include "board.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"

int st7789_rp2040_claim_dma(void) {
    int dma_chan = dma_claim_unused_channel(true);

    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_dreq(&cfg, spi_get_dreq(LCD_SPI_INST, true));
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);

    dma_channel_configure(dma_chan, &cfg,
                          &spi_get_hw(LCD_SPI_INST)->dr,
                          NULL, 0, false);

    return dma_chan;
}

void st7789_rp2040_release_dma(int dma_chan) {
    if (dma_chan >= 0)
        dma_channel_unclaim(dma_chan);
}

void st7789_rp2040_begin_frame(const st7789_t *lcd) {
    hal_spi_set_format(8);
    st7789_set_window(lcd);
    hal_spi_set_format(16);
}

void st7789_rp2040_end_frame(void) {
    st7789_end_write();
}

void __not_in_flash_func(st7789_rp2040_set_row)(uint16_t y, uint16_t y_last) {
    static const uint16_t raset = ST7789_CMD_RASET; /* NOP, RASET */
    static const uint16_t ramwr = ST7789_CMD_RAMWR; /* NOP, RAMWR */
    const uint16_t rows[2] = { y, y_last };

    hal_gpio_dc_write(0);
    spi_write16_blocking(LCD_SPI_INST, &raset, 1);
    hal_gpio_dc_write(1);
    spi_write16_blocking(LCD_SPI_INST, rows, 2);
    hal_gpio_dc_write(0);
    spi_write16_blocking(LCD_SPI_INST, &ramwr, 1);
    hal_gpio_dc_write(1);
}

void st7789_rp2040_dma_start(int dma_chan, const uint16_t *buf, uint16_t count) {
    dma_channel_transfer_from_buffer_now(dma_chan, buf, count);
}

void st7789_rp2040_dma_wait(int dma_chan) {
    dma_channel_wait_for_finish_blocking(dma_chan);
}

void st7789_rp2040_dma_finish(int dma_chan) {
    dma_channel_wait_for_finish_blocking(dma_chan);
    while (spi_is_busy(LCD_SPI_INST))
        tight_loop_contents();
}
