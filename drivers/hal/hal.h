#ifndef DRIVERS_HAL_H
#define DRIVERS_HAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPI */
uint8_t hal_spi_init(void);
uint8_t hal_spi_deinit(void);
uint8_t hal_spi_write(const uint8_t *buf, uint16_t len);
uint8_t hal_spi_set_format(uint8_t bits);  /* 8 or 16 */

/* Chip Select GPIO (active low) */
uint8_t hal_gpio_cs_init(void);
uint8_t hal_gpio_cs_write(uint8_t value);

/* Data/Command GPIO (high = data, low = command) */
uint8_t hal_gpio_dc_init(void);
uint8_t hal_gpio_dc_write(uint8_t value);

/* Reset GPIO (active low) */
uint8_t hal_gpio_reset_init(void);
uint8_t hal_gpio_reset_write(uint8_t value);

/* Timing */
void hal_delay_ms(uint32_t ms);
void hal_setup_delay(void);  /* Short delay for CS/DC setup at high clock speeds */

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_HAL_H */
