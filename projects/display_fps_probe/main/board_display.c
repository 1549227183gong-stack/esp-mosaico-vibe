// SPDX-License-Identifier: Apache-2.0

#include "board_display.h"

#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "mosaico_boot_handoff.h"
#include "sdkconfig.h"

static const char *TAG = "fps_board";

/*
 * BSP 2.0 的 CO5300 面板 IO 固定为 40MHz。本探针需要验证 QSPI 带宽是否
 * 是整屏 20fps 的直接原因，因此在工程内复制最小初始化路径，只把面板
 * IO 时钟提高到 80MHz；电源、启动画面交接和触摸仍复用 BSP 公共接口。
 */
#define FPS_PROBE_QSPI_CLOCK_HZ (80 * 1000 * 1000)

static const co5300_lcd_init_cmd_t s_vendor_init[] = {
    {0x11, NULL, 0, 600},
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
#if CONFIG_BSP_CO5300_ENABLE_TE
    {0x35, (uint8_t[]){0x00}, 1, 0},
#endif
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x29, NULL, 0, 600},
};

static const co5300_lcd_init_cmd_t s_handoff_init[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
#if CONFIG_BSP_CO5300_ENABLE_TE
    {0x35, (uint8_t[]){0x00}, 1, 0},
#endif
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
};

static esp_err_t apply_qspi_drive_cap(gpio_num_t lcd_scl)
{
    const gpio_drive_cap_t strength = (gpio_drive_cap_t)CONFIG_BSP_LCD_QSPI_DRIVE_CAP;
    const gpio_num_t pins[] = {
        lcd_scl, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3,
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        ESP_RETURN_ON_ERROR(gpio_set_drive_capability(pins[i], strength), TAG,
                            "set QSPI GPIO%d drive capability failed", (int)pins[i]);
    }
    return ESP_OK;
}

static esp_err_t create_80mhz_panel(esp_lcd_panel_handle_t *out_panel,
                                    esp_lcd_panel_io_handle_t *out_io)
{
    ESP_RETURN_ON_FALSE(out_panel != NULL && out_io != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "panel outputs are null");

    esp_err_t ret = ESP_OK;
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    bool bus_initialized = false;

    const bool boot_panel_ready = mosaico_boot_handoff_consume();
    ESP_GOTO_ON_ERROR(bsp_power_set_vcc_3v3(true), fail, TAG,
                      "enable VCC_3V3 rail failed");

    bsp_board_variant_t variant;
    ESP_GOTO_ON_ERROR(bsp_board_variant_get(&variant), fail, TAG,
                      "get board variant failed");
    const bool v1_0 = variant == BSP_BOARD_VARIANT_V1_0;
    const gpio_num_t lcd_scl = v1_0 ? BSP_LCD_SCL_V1_0 : BSP_LCD_SCL_V1_2;
    const gpio_num_t lcd_rst = v1_0 ? BSP_LCD_RST_V1_0 : BSP_LCD_RST_V1_2;

    const spi_bus_config_t bus_config = CO5300_PANEL_BUS_QSPI_CONFIG(
        lcd_scl, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3,
        BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8);
    ESP_GOTO_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO),
                      fail, TAG, "initialize CO5300 QSPI bus failed");
    bus_initialized = true;
    ESP_GOTO_ON_ERROR(apply_qspi_drive_cap(lcd_scl), fail, TAG,
                      "configure QSPI drive capability failed");

    esp_lcd_panel_io_spi_config_t io_config =
        CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL);
    io_config.pclk_hz = FPS_PROBE_QSPI_CLOCK_HZ;
    io_config.flags.psram_dma_direct = true;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi(
                          (esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST,
                          &io_config, &io),
                      fail, TAG, "create CO5300 QSPI panel IO failed");

    const co5300_vendor_config_t vendor_config = {
        .init_cmds = boot_panel_ready ? s_handoff_init : s_vendor_init,
        .init_cmds_size = boot_panel_ready
            ? sizeof(s_handoff_init) / sizeof(s_handoff_init[0])
            : sizeof(s_vendor_init) / sizeof(s_vendor_init[0]),
        .flags.use_qspi_interface = true,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = lcd_rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_co5300(io, &panel_config, &panel),
                      fail, TAG, "create CO5300 panel failed");
    if (!boot_panel_ready) {
        ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel), fail, TAG,
                          "reset CO5300 failed");
    }
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel), fail, TAG,
                      "initialize CO5300 failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_set_gap(panel, BSP_LCD_X_GAP, BSP_LCD_Y_GAP),
                      fail, TAG, "set CO5300 gap failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_swap_xy(panel, false), fail, TAG,
                      "set CO5300 axis swap failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_mirror(panel, false, false), fail, TAG,
                      "set CO5300 mirror failed");
    if (!boot_panel_ready) {
        ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), fail, TAG,
                          "turn on CO5300 failed");
    }

    *out_panel = panel;
    *out_io = io;
    ESP_LOGI(TAG,
             "CO5300 ready: QSPI=%dHz lines=%d data=%d-%d-%d-%d CS=%d RST=%d TE=%d boot_handoff=%d",
             FPS_PROBE_QSPI_CLOCK_HZ, BSP_LCD_DATA_WIDTH, BSP_LCD_DATA0,
             BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3, BSP_LCD_CS,
             lcd_rst, BSP_LCD_TE, boot_panel_ready ? 1 : 0);
    return ESP_OK;

fail:
    if (panel != NULL) {
        esp_lcd_panel_del(panel);
    }
    if (io != NULL) {
        esp_lcd_panel_io_del(io);
    }
    if (bus_initialized) {
        spi_bus_free(BSP_LCD_SPI_HOST);
    }
    return ret;
}

esp_err_t board_display_init(esp_display_present_target_config_t *out_target)
{
    ESP_RETURN_ON_FALSE(out_target != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "present target is null");

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(create_80mhz_panel(&panel, &io), TAG,
                        "initialize 80MHz CO5300");

    *out_target = (esp_display_present_target_config_t) {
        .hw = {
            .panel = panel,
            .io = io,
            .panel_type = ESP_DISPLAY_PRESENT_PANEL_IO,
            .input_pixel_format = ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565,
            .rotation = ESP_DISPLAY_PRESENT_ROTATE_0,
            .swap_bytes = true,
#if CONFIG_BSP_CO5300_ENABLE_TE
            .te_enabled = true,
            .te_sync = {
                .gpio_num = BSP_LCD_TE,
                .bus_freq_hz = FPS_PROBE_QSPI_CLOCK_HZ,
                .data_lines = BSP_LCD_DATA_WIDTH,
            },
#endif
        },
        .fb = {
            .mode = ESP_DISPLAY_PRESENT_MODE_AUTO,
        },
    };
    ESP_LOGI(TAG, "GSP present target %dx%d RGB565 TE=%s GPIO=%d",
             BSP_LCD_H_RES, BSP_LCD_V_RES,
#if CONFIG_BSP_CO5300_ENABLE_TE
             "on", BSP_LCD_TE
#else
             "off", -1
#endif
    );
    return ESP_OK;
}

esp_err_t board_touch_init(esp_lcd_touch_handle_t *out_touch)
{
    ESP_RETURN_ON_FALSE(out_touch != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "touch output is null");
    const esp_err_t err = bsp_touch_new(BSP_LCD_ROTATION_DEFAULT, out_touch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch unavailable (%s); continuing without it",
                 esp_err_to_name(err));
        *out_touch = NULL;
        return ESP_OK;
    }
    return ESP_OK;
}
