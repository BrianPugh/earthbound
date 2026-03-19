#include "hal/hal.h"
#include "board.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

uint8_t hal_spi_init(void) {
    spi_init(LCD_SPI_INST, LCD_SPI_BAUD);
    spi_set_format(LCD_SPI_INST, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_LCD_DIN, GPIO_FUNC_SPI);
    gpio_set_function(PIN_LCD_CLK, GPIO_FUNC_SPI);
    return 0;
}

uint8_t hal_spi_deinit(void) {
    spi_deinit(LCD_SPI_INST);
    return 0;
}

uint8_t hal_spi_write(const uint8_t *buf, uint16_t len) {
    spi_write_blocking(LCD_SPI_INST, buf, len);
    return 0;
}

uint8_t hal_spi_set_format(uint8_t bits) {
    /* PL022 spec: SSPCR0 must only be modified when SSP is disabled. */
    hw_clear_bits(&spi_get_hw(LCD_SPI_INST)->cr1, SPI_SSPCR1_SSE_BITS);
    spi_set_format(LCD_SPI_INST, bits, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    hw_set_bits(&spi_get_hw(LCD_SPI_INST)->cr1, SPI_SSPCR1_SSE_BITS);
    return 0;
}

uint8_t hal_gpio_cs_init(void) {
    gpio_init(PIN_LCD_CS);
    gpio_set_dir(PIN_LCD_CS, GPIO_OUT);
    gpio_put(PIN_LCD_CS, 1);
    return 0;
}

uint8_t hal_gpio_cs_write(uint8_t value) {
    gpio_put(PIN_LCD_CS, value);
    return 0;
}

uint8_t hal_gpio_dc_init(void) {
    gpio_init(PIN_LCD_DC);
    gpio_set_dir(PIN_LCD_DC, GPIO_OUT);
    return 0;
}

uint8_t hal_gpio_dc_write(uint8_t value) {
    gpio_put(PIN_LCD_DC, value);
    return 0;
}

uint8_t hal_gpio_reset_init(void) {
    gpio_init(PIN_LCD_RST);
    gpio_set_dir(PIN_LCD_RST, GPIO_OUT);
    gpio_put(PIN_LCD_RST, 1);
    return 0;
}

uint8_t hal_gpio_reset_write(uint8_t value) {
    gpio_put(PIN_LCD_RST, value);
    return 0;
}

void hal_delay_ms(uint32_t ms) {
    sleep_ms(ms);
}

void hal_setup_delay(void) {
    __asm volatile("nop\nnop\nnop\nnop\n");
}
