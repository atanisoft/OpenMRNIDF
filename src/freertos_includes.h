#ifdef ESP32

#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "sdkconfig.h"

#define NSEC_TO_TICK(ns)                                                       \
    (((((long long)(ns)) / 1000 * configTICK_RATE_HZ) + 999999) / 1000000)

// IDF v5.0 has introduced a configuration option (disabled by default) which
// enables the usage of legacy FreeRTOS data types, if that configuration option
// is *NOT* selected *AND* IDF v5.0+ is in use we need to add compatibility
// defines in order to compile OpenMRN successfully.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,0,0) &&                           \
    !defined(CONFIG_FREERTOS_ENABLE_BACKWARD_COMPATIBILITY)

// used in os/os.c and os/os.h
#define portTickType                  TickType_t
#define xTaskHandle                   TaskHandle_t
#define xQueueHandle                  QueueHandle_t
#define xSemaphoreHandle              SemaphoreHandle_t

// used in freertos_drivers/arduino/CpuLoad.hxx and os/os.c
#define pcTaskGetTaskName             pcTaskGetName

#endif // IDF v5.0+ and !CONFIG_FREERTOS_ENABLE_BACKWARD_COMPATIBILITY

#else

#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>

#define NSEC_TO_TICK(ns) ((ns) >> NSEC_TO_TICK_SHIFT)

#endif
