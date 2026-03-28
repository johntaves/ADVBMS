#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_pm.h"
#define GLED GPIO_NUM_4

const TickType_t xDelay = 1000 / portTICK_PERIOD_MS;

extern "C" void app_main() {   
    printf("alive\n");
  gpio_set_direction(GLED, GPIO_MODE_OUTPUT);
  esp_pm_config_t pm_config = {
      .max_freq_mhz = 240, // e.g. 80, 160, 240
      .min_freq_mhz = 40, // e.g. 40
      .light_sleep_enable = true, // enable light sleep
  };
  ESP_ERROR_CHECK( esp_pm_configure(&pm_config) );
  for( ;; ) {
    printf("hi\n");
    gpio_set_level(GLED,0);
    vTaskDelay(xDelay);
    fprintf(stderr,"Yo\n");
    gpio_set_level(GLED,1);
    vTaskDelay(xDelay);
  }
}
