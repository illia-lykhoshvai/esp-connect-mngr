#include <stdio.h>

#include "driver/gpio.h"

#define COMPONENT_TAG "blink"
#include "esp_log.h"

#include "main.h"
#include "blink.h"

void blinkTask(void* params) {
    uint32_t level = 0;

    ESP_LOGI(COMPONENT_TAG, "Starting blinkTask");

    gpio_reset_pin(GPIO_NUM_15);
    gpio_set_direction(GPIO_NUM_15, GPIO_MODE_OUTPUT);

    while(1) {
        gpio_set_level(GPIO_NUM_15, level);
        ESP_LOGV(COMPONENT_TAG, "Changed LED state to %s", (level ? "ON" : "OFF"));
        level = !level;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelete(NULL);
}
