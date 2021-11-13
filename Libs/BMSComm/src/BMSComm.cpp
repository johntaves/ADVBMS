
#include <esp_adc_cal.h>
#include <string.h>
#include "BMSComm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"

#define CPUBAUD 115200

MaxData serBuff;
void (*commCB)(const AMsg* buf);
volatile int cnt,nextCode;
volatile uint8_t lastCmd;

uint8_t CRC8(const uint8_t *data,uint16_t length) 
{
   uint8_t crc = 0x00;
   uint8_t extract;
   uint8_t sum;
   for(uint16_t i=0;i<length;i++)
   {
      extract = *data;
      for (uint8_t tempI = 8; tempI; tempI--) 
      {
         sum = (crc ^ extract) & 0x01;
         crc >>= 1;
         if (sum)
            crc ^= 0x8C;
         extract >>= 1;
      }
      data++;
   }
   return crc;
}

uint8_t txBuffer[sizeof(MaxData)]; // this is so that we do not mangle the data sent to BMSSendRaw

void BMSSendRaw(uint8_t *d,uint16_t len) {
  if (!len) return;
  *d = CRC8(d+1,len-1);
  if (((AMsg*)d)->cmd != Status)
    printf("Sending: %d, %d, 0x%x\n",d[1],len,*d);
  uint8_t lastI=0xff;
  uint8_t firstOne;
  for (uint8_t i=0;i<len;i++) {
    if (d[i]) {
      txBuffer[i]=d[i];
      continue;
    }
    if (lastI == 0xff) firstOne = i+1;
    else txBuffer[lastI] = i-lastI;
    lastI = i;
  }
  if (lastI == 0xff) firstOne = len+1;
  else txBuffer[lastI] = len-lastI;
  uart_write_bytes(UART_NUM_2, (const char *)&firstOne, 1);
  uart_write_bytes(UART_NUM_2, (const char *)txBuffer, len);
  firstOne = 0;
  uart_write_bytes(UART_NUM_2, (const char *)&firstOne, 1);
}

void BMSSend(StrMsg* m) {
  BMSSendRaw((uint8_t*)m,sizeof(StrMsg) - sizeof(m->msg) + strlen(m->msg));
}
volatile TaskHandle_t xTaskToNotify = NULL;
bool BMSWaitFor(AMsg* mp,uint8_t cmd,uint32_t waitMs) {
  xTaskToNotify = xTaskGetCurrentTaskHandle();
  BMSSend(mp);
  lastCmd = Panic;
  TickType_t stTick = xTaskGetTickCount();
  while ((ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(waitMs)) == 1) && lastCmd != cmd && (xTaskGetTickCount() - stTick) < pdMS_TO_TICKS(waitMs)) ;
  xTaskToNotify = NULL;
  return lastCmd != cmd;
}

void serialRxTask(void *arg) { // this is unpacking the COB packet thing
  uint8_t c;
  uint8_t *buffer = (uint8_t*)&serBuff;
  while (1) {
    int len;
    while ((len = uart_read_bytes(UART_NUM_2, &c, 1, 100000)) == 0)
      ;
    assert(len > 0);
    if (!nextCode) {
      nextCode = c;
    } else if (!c) {
      if (cnt && CRC8(&buffer[1],cnt-1) == buffer[0] && commCB) {
        if (serBuff.amsg.cmd != Status)
          printf("Rec: %d, %d, 0x%x\n",buffer[1],cnt,buffer[0]);
        commCB((AMsg*)buffer);
        if (xTaskToNotify)
          xTaskNotifyGive(xTaskToNotify);
        lastCmd = serBuff.amsg.cmd;
      }
      nextCode = 0;
      cnt=0;
    } else if (cnt < sizeof(serBuff)) {
      if (cnt == (nextCode-1)) {
        buffer[cnt++] = 0;
        nextCode = c + cnt;
      } else
        buffer[cnt++] = c;
    } else {
      cnt=0;
      nextCode=0;
    }
  }
}

void WriteRelays(std::shared_ptr<nvs::NVSHandle> h,RelaySettings* r,int num) {
  char name[10];
  for (int i=0;i<num;i++,r++) {
    snprintf(name,sizeof(name),"%s%d","name",i);
    assert(h->set_string(name,r->name) == ESP_OK);
    snprintf(name,sizeof(name),"%s%d","from",i);
    assert(h->set_string(name,r->from) == ESP_OK);
    snprintf(name,sizeof(name),"%s%d","doSoC",i);
    assert(h->set_item(name,r->doSoC) == ESP_OK);
    snprintf(name,sizeof(name),"%s%d","off",i);
    assert(h->set_item(name,r->off) == ESP_OK);
    snprintf(name,sizeof(name),"%s%d","fullChg",i);
    assert(h->set_item(name,r->fullChg) == ESP_OK);
    snprintf(name,sizeof(name),"%s%d","rec",i);
    assert(h->set_item(name,r->rec) == ESP_OK);
    snprintf(name,sizeof(name),"%s%d","trip",i);
    assert(h->set_item(name,r->trip) == ESP_OK);
    snprintf(name,sizeof(name),"%s%d","type",i);
    assert(h->set_item(name,r->type) == ESP_OK);
  }
}

void InitRelays(std::shared_ptr<nvs::NVSHandle> h,RelaySettings* rp,int num) {
  char name[10];
  RelaySettings* r=rp;
  for (int i=0;i<num;i++,r++) {
    snprintf(name,sizeof(name),"%s%d","name",i);
    if (h->get_string(name,r->name,sizeof(r->name)) == ESP_ERR_NVS_NOT_FOUND)
      r->name[0] = 0;
    snprintf(name,sizeof(name),"%s%d","from",i);
    if (h->get_string(name,r->from,sizeof(r->from)) == ESP_ERR_NVS_NOT_FOUND)
      r->from[0] = 0;
    snprintf(name,sizeof(name),"%s%d","doSoC",i);
    if (h->get_item(name,r->doSoC) == ESP_ERR_NVS_NOT_FOUND)
      r->doSoC = false;
    snprintf(name,sizeof(name),"%s%d","off",i);
    if (h->get_item(name,r->off) == ESP_ERR_NVS_NOT_FOUND)
      r->off = true;
    snprintf(name,sizeof(name),"%s%d","fullChg",i);
    if (h->get_item(name,r->fullChg) == ESP_ERR_NVS_NOT_FOUND)
      r->fullChg = false;
    snprintf(name,sizeof(name),"%s%d","rec",i);
    if (h->get_item(name,r->rec) == ESP_ERR_NVS_NOT_FOUND)
      r->rec = 0;
    snprintf(name,sizeof(name),"%s%d","trip",i);
    if (h->get_item(name,r->trip) == ESP_ERR_NVS_NOT_FOUND)
      r->trip = 0;
    snprintf(name,sizeof(name),"%s%d","type",i);
    if (h->get_item(name,r->type) == ESP_ERR_NVS_NOT_FOUND)
      r->type = 0;
  }
}

void BMSInitCom(void (*func)(const AMsg* buf)) {

  commCB = func;
  uart_config_t uart_config = {
      .baud_rate = CPUBAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity    = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_APB,
      .rx_flow_ctrl_thresh = 122
  };

  ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, sizeof(MaxData)*2, sizeof(MaxData)+1, 0, NULL, 0));
  ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &uart_config));
  nextCode = 0;
  cnt = 0;
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      // NVS partition was truncated and needs to be erased
      // Retry nvs_flash_init
      ESP_ERROR_CHECK(nvs_flash_erase());
      err = nvs_flash_init();
  }
  ESP_ERROR_CHECK( err );

  xTaskCreate(serialRxTask, "serial RX task", 2048, NULL, 10, NULL); // priority value?
}

