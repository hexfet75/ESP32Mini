#include "status_led.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_GPIO 48
static led_strip_handle_t led_strip;
static TaskHandle_t blink_task_handle;
static system_status_t current_status = STATUS_OFF;

static void blink_task(void *arg)
{
    bool on = false;
    while (1) {
        switch (current_status) {
            case STATUS_BOOT:
                led_strip_set_pixel(led_strip, 0, 0, 0, 20);
                led_strip_refresh(led_strip);
                vTaskDelay(pdMS_TO_TICKS(200));
                break;
            case STATUS_CONNECTING:
                on = !on;
                if (on) led_strip_set_pixel(led_strip, 0, 15, 0, 20);
                else led_strip_clear(led_strip);
                led_strip_refresh(led_strip);
                vTaskDelay(pdMS_TO_TICKS(400));
                break;
            case STATUS_OK:
                led_strip_set_pixel(led_strip, 0, 0, 20, 0);
                led_strip_refresh(led_strip);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            case STATUS_WARNING:
                on = !on;
                if (on) led_strip_set_pixel(led_strip, 0, 20, 15, 0);
                else led_strip_clear(led_strip);
                led_strip_refresh(led_strip);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
            case STATUS_ERROR:
                on = !on;
                if (on) led_strip_set_pixel(led_strip, 0, 20, 0, 0);
                else led_strip_clear(led_strip);
                led_strip_refresh(led_strip);
                vTaskDelay(pdMS_TO_TICKS(150));
                break;
            case STATUS_MQTT_ERROR:
                on = !on;
                if (on) led_strip_set_pixel(led_strip, 0, 20, 8, 0); // arancione
                else led_strip_clear(led_strip);
                led_strip_refresh(led_strip);
                vTaskDelay(pdMS_TO_TICKS(300)); // lampeggio medio-veloce
                break;
            case STATUS_OFF:
                led_strip_clear(led_strip);
                led_strip_refresh(led_strip);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
        }
    }
}

void status_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    xTaskCreate(blink_task, "status_led", 2048, NULL, 5, &blink_task_handle);
}

void status_led_set(system_status_t status)
{
    current_status = status;
}

void status_led_off(void)
{
    current_status = STATUS_OFF;

    if (blink_task_handle != NULL) {
        vTaskSuspend(blink_task_handle);
    }

    // WS2812 keeps its last received color until a black frame is latched.
    led_strip_set_pixel(led_strip, 0, 0, 0, 0);
    led_strip_refresh(led_strip);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Keep the data line low while the ESP32 is in deep sleep.
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);
}