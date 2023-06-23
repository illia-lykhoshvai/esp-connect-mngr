#include <stdio.h>

#define COMPONENT_TAG "main"
#include "esp_log.h"

#include "main.h"
#include "blink.h"
#include "wifi.h"

#define MIN_STACK_SIZE 2048
taskTable_t osTasks[] = {
    {blinkTask, "BLINK LED", MIN_STACK_SIZE, NULL, 2, NULL},
    {networkTask,"WI-FI subsystem control", 5*MIN_STACK_SIZE, NULL, 3, NULL}
};

void app_main(void)
{
    size_t i;

    ESP_LOGI(COMPONENT_TAG, "Started app_main");
    for(i = 0; i < sizeof(osTasks)/sizeof(osTasks[0]); i++) {
        ESP_LOGI(COMPONENT_TAG, "Creating task: %s", osTasks[i].taskName);
        xTaskCreate(
            osTasks[i].taskFunctionPtr,
            osTasks[i].taskName,
            osTasks[i].stackSize,
            osTasks[i].parametersPtr,
            osTasks[i].priority,
            osTasks[i].taskHandle
        );
    }
}