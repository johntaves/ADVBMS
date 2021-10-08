#include <Arduino.h>
#include <esp_adc_cal.h>

#ifndef JTBMS_BMSComm_H_
#define JTBMS_BMSComm_H_

extern esp_adc_cal_value_t BMSInit();
extern uint32_t BMSReadVoltage(uint8_t readPin,uint32_t cnt);
extern int16_t BMSGetTemp(uint8_t tPin,uint16_t Vs,int bCoef,uint32_t Ro,uint32_t R1,uint8_t cnt);

#endif