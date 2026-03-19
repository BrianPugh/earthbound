#ifndef DRIVERS_DISPLAY_ST7789_H
#define DRIVERS_DISPLAY_ST7789_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ST7789 register commands */
#define ST7789_CMD_SLPOUT   0x11  /* Sleep out */
#define ST7789_CMD_INVON    0x21  /* Display inversion on */
#define ST7789_CMD_DISPON   0x29  /* Display on */
#define ST7789_CMD_CASET    0x2A  /* Column address set */
#define ST7789_CMD_RASET    0x2B  /* Row address set */
#define ST7789_CMD_RAMWR    0x2C  /* Memory write */
#define ST7789_CMD_MADCTL   0x36  /* Memory data access control */
#define ST7789_CMD_COLMOD   0x3A  /* Interface pixel format */
#define ST7789_CMD_PORCTRL  0xB2  /* Porch setting */
#define ST7789_CMD_GCTRL    0xB7  /* Gate control */
#define ST7789_CMD_VCOMS    0xBB  /* VCOMS setting */
#define ST7789_CMD_LCMCTRL  0xC0  /* LCM control */
#define ST7789_CMD_VDVVRHEN 0xC2  /* VDV and VRH command enable */
#define ST7789_CMD_VRHS     0xC3  /* VRH set */
#define ST7789_CMD_VDVS     0xC4  /* VDV set */
#define ST7789_CMD_FRCTRL2  0xC6  /* Frame rate control in normal mode */
#define ST7789_CMD_PWCTRL1  0xD0  /* Power control 1 */
#define ST7789_CMD_PVGAMCTRL 0xE0 /* Positive voltage gamma control */
#define ST7789_CMD_NVGAMCTRL 0xE1 /* Negative voltage gamma control */

/* MADCTL bits */
#define ST7789_MADCTL_MY    0x80  /* Row address order */
#define ST7789_MADCTL_MX    0x40  /* Column address order */
#define ST7789_MADCTL_MV    0x20  /* Row/column exchange */
#define ST7789_MADCTL_ML    0x10  /* Vertical refresh order */
#define ST7789_MADCTL_BGR   0x08  /* BGR color order */

/* COLMOD: always 16-bit BGR565 */
#define ST7789_COLMOD_16BIT 0x05

typedef enum {
    ST7789_ROTATION_0 = 0,
    ST7789_ROTATION_90,
    ST7789_ROTATION_180,
    ST7789_ROTATION_270,
} st7789_rotation_t;

typedef struct {
    uint16_t width;              /* Logical width after rotation */
    uint16_t height;             /* Logical height after rotation */
    st7789_rotation_t rotation;
} st7789_config_t;

/* Driver state (caller allocates, driver fills in st7789_init) */
typedef struct {
    uint16_t width;
    uint16_t height;
} st7789_t;

/* Initialize the ST7789: hardware reset, SPI init, display configuration.
 * Caller must have board.h pin definitions available to the HAL. */
void st7789_init(st7789_t *lcd, const st7789_config_t *config);

/* Set the active drawing window and issue RAMWR.
 * After this call, CS is held low and DC is high — ready for pixel streaming. */
void st7789_set_window(const st7789_t *lcd);

/* Release CS after pixel streaming is complete. */
void st7789_end_write(void);

/* Low-level: send a command byte (manages CS/DC per ST7789 SPI protocol). */
void st7789_write_cmd(uint8_t cmd);

/* Low-level: send data bytes with CS held low for the entire buffer. */
void st7789_write_data(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_DISPLAY_ST7789_H */
