#include "esp_timer.h"
#include "esp_attr.h"
uint32_t IRAM_ATTR millis()
{
    return (uint32_t) (esp_timer_get_time() / 1000ULL);
}

