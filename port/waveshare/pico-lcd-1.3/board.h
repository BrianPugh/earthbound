#ifndef BOARD_H
#define BOARD_H

/* Waveshare Pico-LCD-1.3 pin assignments
 * https://www.waveshare.com/wiki/Pico-LCD-1.3 */

/* ST7789 display (SPI1) */
#define PIN_LCD_DIN   11  /* SPI1 MOSI */
#define PIN_LCD_CLK   10  /* SPI1 SCK */
#define PIN_LCD_CS     9  /* Chip select (active low) */
#define PIN_LCD_DC     8  /* Data/command */
#define PIN_LCD_RST   12  /* Reset (active low) */
#define PIN_LCD_BL    13  /* Backlight */

#define LCD_SPI_INST  spi1
#define LCD_SPI_BAUD  (62500000)  /* 62.5 MHz (250 MHz clk_peri / 4) */

#define LCD_WIDTH   240
#define LCD_HEIGHT  240

/* Buttons (directly connected to GPIO, active low with internal pull-up) */
#define PIN_BTN_A     15
#define PIN_BTN_B     17
#define PIN_BTN_X     19
#define PIN_BTN_Y     21

/* PWM audio output (wire external speaker/amplifier to this pin) */
#define PIN_AUDIO_PWM  0

/* Joystick (directly connected to GPIO, active low with internal pull-up) */
#define PIN_JOY_UP     2
#define PIN_JOY_DOWN  18
#define PIN_JOY_LEFT  16
#define PIN_JOY_RIGHT 20
#define PIN_JOY_CTRL   3  /* Center press */

#endif /* BOARD_H */
