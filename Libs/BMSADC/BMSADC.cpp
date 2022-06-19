
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
  return av / cnt;
}

int8_t BMSReadTemp(adc1_channel_t tPin,bool highside, uint32_t Vs,int bCoef
        ,uint32_t Ro,uint32_t R1,uint8_t cnt,uint16_t* vp=NULL,uint32_t* rtp=NULL,double* Tp=NULL) { // Ro is the thermistor resistance at 25c
  uint32_t Vout = BMSReadVoltage(tPin,cnt);
  if (!Vout || !(Vs - Vout)) return INT8_MIN;
  if (vp) *vp = Vout;
  if (highside && Vout < 100) return INT8_MIN;
  if (!highside && Vout > (Vs-100)) return INT8_MIN;
  uint32_t Rt;
  if (highside) Rt = (R1 * (Vs - Vout))/Vout;
  else Rt = R1 * Vout / (Vs - Vout);
  double T = 1/(1/298.15d + log(Rt/(double)Ro)/(double)bCoef);

  if (rtp) *rtp = Rt;
  if (Tp) *Tp = T;
  return (int8_t)(T-273.15);
}
