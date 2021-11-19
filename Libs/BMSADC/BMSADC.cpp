
#include <math.h>
#include "esp_adc_cal.h"
#include "driver/gpio.h"
#include "driver/adc.h"

esp_adc_cal_characteristics_t adc_chars;

void BMSADCInit() {
  adc1_config_width(ADC_WIDTH_BIT_12);

  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11
      , ADC_WIDTH_BIT_12, ESP_ADC_CAL_VAL_DEFAULT_VREF, &adc_chars);
}

uint32_t BMSReadVoltage(adc1_channel_t readPin,uint32_t cnt) {
  uint32_t av=0;
  if (!cnt)
    return 0;
  for (int i=0;i<cnt;i++)
    av += esp_adc_cal_raw_to_voltage(adc1_get_raw(readPin), &adc_chars);
  //Serial.printf("rv: %d\n",av / cnt);
  return av / cnt;
}

int16_t BMSReadTemp(adc1_channel_t tPin,uint16_t Vs,int bCoef,uint32_t Ro,uint32_t R1,uint8_t cnt,uint16_t* vp=NULL) {
  uint16_t Vout = BMSReadVoltage(tPin,cnt);
  if (vp) *vp = Vout;
  double Rt = (double)R1 * (double)Vout / ((double)Vs - (double)Vout);
  double T = 1/(1/298.15d + log(Rt/(double)Ro)/(double)bCoef);

  return (int16_t)(T-273.15);
}
