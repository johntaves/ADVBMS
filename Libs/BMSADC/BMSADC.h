
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
extern void BMSADCInit();
extern uint32_t BMSReadVoltage(adc1_channel_t readPin,uint32_t cnt);
extern int8_t BMSReadTemp(adc1_channel_t tPin,bool highside, uint32_t Vs
        ,int bCoef,uint32_t Ro,uint32_t R1,uint8_t cnt,uint16_t* vp=NULL,uint32_t* rtp=NULL,double* Tp=NULL);
