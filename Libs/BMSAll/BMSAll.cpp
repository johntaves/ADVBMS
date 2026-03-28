#include "esp_timer.h"
#include "esp_attr.h"
#include <math.h>
uint32_t IRAM_ATTR millis()
{
    return (uint32_t) (esp_timer_get_time() / 1000ULL);
}

int8_t BMSComputeTemp(uint32_t Vout,bool highside, uint32_t Vs,int bCoef
        ,uint32_t Ro,uint32_t R1,uint16_t* vp=NULL,uint32_t* rtp=NULL,double* Tp=NULL) {
  if (!Vout || !(Vs - Vout)) return INT8_MIN;
  if (vp) *vp = Vout;
  if (highside && Vout < 100) return INT8_MIN;
  if (!highside && Vout > (Vs-100)) return INT8_MIN;
  uint32_t Rt;
  if (highside) Rt = (R1 * (Vs - Vout))/Vout;
  else Rt = R1 * Vout / (Vs - Vout);
  double T = 1/(1/298.15 + log(Rt/(double)Ro)/(double)bCoef);

  if (rtp) *rtp = Rt;
  if (Tp) *Tp = T;
  return (int8_t)(T-273.15);
}
