#include "st7789.h"
#include "hal/hal.h"

#include <stddef.h>

void st7789_write_cmd(uint8_t cmd) {
    hal_gpio_cs_write(1);
    hal_gpio_dc_write(0);
    hal_setup_delay();
    hal_gpio_cs_write(0);
    hal_setup_delay();
    hal_spi_write(&cmd, 1);
    hal_gpio_cs_write(1);
}

void st7789_write_data(const uint8_t *data, uint16_t len) {
    hal_gpio_cs_write(1);
    hal_gpio_dc_write(1);
    hal_setup_delay();
    hal_gpio_cs_write(0);
    hal_setup_delay();
    hal_spi_write(data, len);
    hal_gpio_cs_write(1);
}

/* Send command followed by data parameter(s) in one CS assertion. */
static void st7789_cmd(uint8_t cmd, const uint8_t *data, uint16_t len) {
    st7789_write_cmd(cmd);
    if (len > 0)
        st7789_write_data(data, len);
}

static uint8_t madctl_from_rotation(st7789_rotation_t rotation) {
    switch (rotation) {
    case ST7789_ROTATION_0:
        return ST7789_MADCTL_BGR;
    case ST7789_ROTATION_90:
        return ST7789_MADCTL_MV | ST7789_MADCTL_MX | ST7789_MADCTL_ML | ST7789_MADCTL_BGR;
    case ST7789_ROTATION_180:
        return ST7789_MADCTL_MY | ST7789_MADCTL_MX | ST7789_MADCTL_BGR;
    case ST7789_ROTATION_270:
        return ST7789_MADCTL_MY | ST7789_MADCTL_MV | ST7789_MADCTL_BGR;
    default:
        return ST7789_MADCTL_BGR;
    }
}

/* Default gamma curves (from Waveshare Pico-LCD-1.3 reference). */
static const uint8_t gamma_positive[14] = {
    0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
    0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23,
};
static const uint8_t gamma_negative[14] = {
    0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
    0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23,
};

void st7789_init(st7789_t *lcd, const st7789_config_t *config) {
    lcd->width = config->width;
    lcd->height = config->height;

    /* Initialize HAL peripherals */
    hal_gpio_reset_init();
    hal_spi_init();
    hal_gpio_cs_init();
    hal_gpio_dc_init();

    /* Hardware reset */
    hal_gpio_reset_write(1);
    hal_delay_ms(10);
    hal_gpio_reset_write(0);
    hal_delay_ms(10);
    hal_gpio_reset_write(1);
    hal_delay_ms(120);

    /* Display configuration */
    uint8_t madctl = madctl_from_rotation(config->rotation);
    st7789_cmd(ST7789_CMD_MADCTL, &madctl, 1);

    uint8_t colmod = ST7789_COLMOD_16BIT;
    st7789_cmd(ST7789_CMD_COLMOD, &colmod, 1);

    /* Porch setting */
    uint8_t porch[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    st7789_cmd(ST7789_CMD_PORCTRL, porch, sizeof(porch));

    /* Gate control */
    uint8_t gctrl = 0x35;
    st7789_cmd(ST7789_CMD_GCTRL, &gctrl, 1);

    /* VCOMS */
    uint8_t vcoms = 0x25;
    st7789_cmd(ST7789_CMD_VCOMS, &vcoms, 1);

    /* LCM control */
    uint8_t lcm = 0x2C;
    st7789_cmd(ST7789_CMD_LCMCTRL, &lcm, 1);

    /* VDV and VRH enable */
    uint8_t vdvvrhen = 0x01;
    st7789_cmd(ST7789_CMD_VDVVRHEN, &vdvvrhen, 1);

    /* VRH set */
    uint8_t vrhs = 0x12;
    st7789_cmd(ST7789_CMD_VRHS, &vrhs, 1);

    /* VDV set */
    uint8_t vdvs = 0x20;
    st7789_cmd(ST7789_CMD_VDVS, &vdvs, 1);

    /* Frame rate: 60Hz */
    uint8_t frctrl = 0x0F;
    st7789_cmd(ST7789_CMD_FRCTRL2, &frctrl, 1);

    /* Power control */
    uint8_t pwctrl[] = {0xA4, 0xA1};
    st7789_cmd(ST7789_CMD_PWCTRL1, pwctrl, sizeof(pwctrl));

    /* Gamma curves */
    st7789_cmd(ST7789_CMD_PVGAMCTRL, gamma_positive, 14);
    st7789_cmd(ST7789_CMD_NVGAMCTRL, gamma_negative, 14);

    /* Display inversion on, sleep out, display on */
    st7789_cmd(ST7789_CMD_INVON, NULL, 0);
    st7789_cmd(ST7789_CMD_SLPOUT, NULL, 0);
    hal_delay_ms(120);
    st7789_cmd(ST7789_CMD_DISPON, NULL, 0);
    hal_delay_ms(50);
}

void st7789_set_window(const st7789_t *lcd) {
    uint8_t caset[] = {0x00, 0x00, (lcd->width - 1) >> 8, (lcd->width - 1) & 0xFF};
    st7789_cmd(ST7789_CMD_CASET, caset, 4);

    uint8_t raset[] = {0x00, 0x00, (lcd->height - 1) >> 8, (lcd->height - 1) & 0xFF};
    st7789_cmd(ST7789_CMD_RASET, raset, 4);

    /* Issue RAMWR, then hold CS low and DC high for pixel streaming */
    st7789_write_cmd(ST7789_CMD_RAMWR);
    hal_gpio_dc_write(1);
    hal_gpio_cs_write(0);
}

void st7789_end_write(void) {
    hal_gpio_cs_write(1);
}
