#pragma once

extern uint32_t millis();
extern int8_t BMSComputeTemp(uint32_t Vout,bool highside, uint32_t Vs,int bCoef
        ,uint32_t Ro,uint32_t R1,uint16_t* vp=NULL,uint32_t* rtp=NULL,double* Tp=NULL);

#define LOW 0
#define HIGH 1

