#ifndef MAIN_H__
#define MAIN_H__

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#define NON_NULL_STR(x) ((x) ? (x) : "")

typedef struct {
    TaskFunction_t taskFunctionPtr;
    const char * const taskName;
    const uint32_t stackSize;
    void * const parametersPtr;
    UBaseType_t priority;
    TaskHandle_t * const taskHandle;
} taskTable_t;

#endif