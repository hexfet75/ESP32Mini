#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define LED_GPIO 48
#define LED_COUNT 1

static const char *TAG = "led_test";

void app_main(void)
{
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM available: %s (%u free bytes)", psram_free > 0 ? "yes" : "no", (unsigned)psram_free);

    uint8_t *psram_test = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (psram_test == NULL) {
        ESP_LOGE(TAG, "PSRAM test failed: allocation unavailable");
    } else {
        for (size_t i = 0; i < 1024; ++i) {
            psram_test[i] = (uint8_t)(i ^ 0xA5);
        }

        bool test_ok = true;
        for (size_t i = 0; i < 1024; ++i) {
            if (psram_test[i] != (uint8_t)(i ^ 0xA5)) {
                test_ok = false;
                break;
            }
        }

        ESP_LOGI(TAG, "PSRAM read/write test: %s", test_ok ? "PASS" : "FAIL");
        heap_caps_free(psram_test);
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };

    led_strip_handle_t led_strip;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

    ESP_LOGI(TAG, "LED WS2812 inizializzato su GPIO%d", LED_GPIO);

    while (1) {
        // Acceso rosso
        led_strip_set_pixel(led_strip, 0, 0, 20, 20); // R,G,B (valori bassi = luminosità contenuta)
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Spento
        led_strip_clear(led_strip);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}