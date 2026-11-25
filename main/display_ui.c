#include "display_ui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_log.h"

#define PIN_NUM_MOSI  13
#define PIN_NUM_CLK   14
#define PIN_NUM_CS    15
#define PIN_NUM_DC    2
#define PIN_NUM_RST   4
#define PIN_NUM_BCKL  27
#define PIN_NUM_BCKL2 21
#define PIN_TOUCH_IRQ 36

#define SCREEN_TIMEOUT_MS 15000 // 15 sekund świecenia po dotknięciu

#define LCD_H_RES 480
#define LCD_V_RES 320

#define COLOR_BLACK  0x0000
#define COLOR_WHITE  0xFFFF
#define COLOR_ORANGE 0xFCE0
#define COLOR_GREEN  0x07E0
#define COLOR_GRAY   0x4208

static esp_lcd_panel_handle_t panel_handle = NULL;
static bool screen_on = true;
static int64_t last_touch_time_ms = 0;

static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
};

static void set_backlight(bool on) {
    screen_on = on;
    gpio_set_level(PIN_NUM_BCKL, on ? 1 : 0);
    gpio_set_level(PIN_NUM_BCKL2, on ? 1 : 0);
}

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    static uint16_t line_buf[LCD_H_RES];
    for (int i = 0; i < w; i++) line_buf[i] = color;
    for (int row = y; row < y + h; row++) {
        esp_lcd_panel_draw_bitmap(panel_handle, x, row, x + w, row + 1, line_buf);
    }
}

static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < 32 || c > 90) c = ' ';
    const uint8_t *glyph = font5x7[c - 32];
    uint16_t buf[6 * 8 * 4];
    if (scale > 2) scale = 2;

    int bw = 6 * scale;
    int bh = 8 * scale;
    for (int py = 0; py < bh; py++) {
        for (int px = 0; px < bw; px++) {
            int gx = px / scale;
            int gy = py / scale;
            bool on = (gx < 5) && (glyph[gx] & (1 << gy));
            buf[py * bw + px] = on ? fg : bg;
        }
    }
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + bw, y + bh, buf);
}

static void draw_text(int x, int y, const char *str, uint16_t fg, uint16_t bg, int scale) {
    while (*str) {
        if (x + 6 * scale > LCD_H_RES) break;
        draw_char(x, y, *str++, fg, bg, scale);
        x += 6 * scale;
    }
}

static void init_lcd(void) {
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_NUM_BCKL) | (1ULL << PIN_NUM_BCKL2)
    };
    gpio_config(&bk_gpio_config);

    gpio_config_t touch_gpio_config = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << PIN_TOUCH_IRQ,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE
    };
    gpio_config(&touch_gpio_config);

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_CLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 20 * sizeof(uint16_t)
    };
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle);

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_new_panel_st7796(io_handle, &panel_config, &panel_handle);
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_swap_xy(panel_handle, true);
    esp_lcd_panel_mirror(panel_handle, true, true);
    esp_lcd_panel_disp_on_off(panel_handle, true);

    fill_rect(0, 0, LCD_H_RES, LCD_V_RES, COLOR_BLACK);
    set_backlight(true);
    last_touch_time_ms = esp_timer_get_time() / 1000;
}

static void init_sntp_time(void) {
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

static void refresh_screen_content(void) {
    char buf[128];

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year > (2020 - 1900)) {
        strftime(buf, sizeof(buf), "%d.%m.%Y   %H:%M:%S   ", &timeinfo);
    } else {
        snprintf(buf, sizeof(buf), "SYNCHRONIZACJA CZASU... ");
    }
    draw_text(15, 15, buf, COLOR_ORANGE, COLOR_BLACK, 2);
    fill_rect(10, 42, 460, 2, COLOR_GRAY);

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(buf, sizeof(buf), "WAN IP: " IPSTR " (ONLINE)   ", IP2STR(&ip_info.ip));
        draw_text(15, 55, buf, COLOR_GREEN, COLOR_BLACK, 2);
    } else {
        draw_text(15, 55, "WAN: BRAK POLACZENIA Z INTERNETEM ", COLOR_WHITE, COLOR_BLACK, 2);
    }

    wifi_sta_list_t wifi_sta_list;
    memset(&wifi_sta_list, 0, sizeof(wifi_sta_list));

    if (esp_wifi_ap_get_sta_list(&wifi_sta_list) == ESP_OK) {
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_pair_mac_ip_t mac_ip_pair[10];
        int count = wifi_sta_list.num > 10 ? 10 : wifi_sta_list.num;

        for (int k = 0; k < count; k++) {
            memcpy(mac_ip_pair[k].mac, wifi_sta_list.sta[k].mac, 6);
            mac_ip_pair[k].ip.addr = 0;
        }
        if (ap_netif && count > 0) {
            esp_netif_dhcps_get_clients_by_mac(ap_netif, count, mac_ip_pair);
        }

        snprintf(buf, sizeof(buf), "PODLACZONE URZADZENIA (%d):   ", wifi_sta_list.num);
        draw_text(15, 95, buf, COLOR_ORANGE, COLOR_BLACK, 2);

        for (int i = 0; i < 6; i++) {
            int y_pos = 125 + (i * 28);
            if (i < count) {
                snprintf(buf, sizeof(buf), "%d. IP: " IPSTR "  MAC: %02X:%02X:%02X:%02X:%02X:%02X   ",
                         i + 1,
                         IP2STR(&mac_ip_pair[i].ip),
                         mac_ip_pair[i].mac[0], mac_ip_pair[i].mac[1], mac_ip_pair[i].mac[2],
                         mac_ip_pair[i].mac[3], mac_ip_pair[i].mac[4], mac_ip_pair[i].mac[5]);
                draw_text(15, y_pos, buf, COLOR_WHITE, COLOR_BLACK, 1);
            } else {
                fill_rect(15, y_pos, 450, 16, COLOR_BLACK);
            }
        }
    }
}

static void display_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(3000));
    init_lcd();
    init_sntp_time();
    refresh_screen_content();

    int tick_counter = 0;
    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (gpio_get_level(PIN_TOUCH_IRQ) == 0) {
            last_touch_time_ms = now_ms;
            if (!screen_on) {
                refresh_screen_content();
                set_backlight(true);
            }
        }

        if (screen_on && (now_ms - last_touch_time_ms > SCREEN_TIMEOUT_MS)) {
            set_backlight(false);
        }

        if (screen_on && (++tick_counter >= 20)) {
            tick_counter = 0;
            refresh_screen_content();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void start_display_ui(void) {
    xTaskCreate(display_task, "display_task", 4096, NULL, 5, NULL);
}
