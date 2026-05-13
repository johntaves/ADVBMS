#include <stdio.h>
#include <string.h>
#include <ctime>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include <NimBLEDevice.h>
#include "BMSAll.h"
#include "CellData.h"
#include "ads1115.h"

#define DUMP GPIO_NUM_27
#define TEMPPWR GPIO_NUM_26
#define GLED GPIO_NUM_4
#define BCOEF 4050

#define V_RES_TOP 3000l
#define V_RES_BOT 10000l

NimBLECharacteristic *pStat,*pDump,*pSett;
NimBLEServer *pServer;
uint32_t connectedTime=0;
bool ledOn=false;
CellSettings cellSett;
CellStatus cs;
uint32_t startDrainMSecs=0,goalDrainMSecs=0;
i2c_master_bus_handle_t bus_handle;
// -30 inc by 1's
uint32_t Rvals[] {173247,168232,163358,158620,154015,149541,145194,140970,136869,132885,129016,125261,121614,118075,114640,111307,108073,104935,101892,98940,
96077,93301,90609,87999,85470,83018,80642,78339,76108,73946,71852,69823,67858,65955,64111,62326,60597,58923,57302,55732,
54212,52740,51316,49936,48600,47307,46055,44842,43668,42531,41429,40363,39329,38329,37359,36419,35509,34626,33771,32941,
32137,31357,30600,29865,29153,28461,27789,27136,26502,25885,25286,24704,24137,23585,23049,22526,22017,21520,21037,20565,
20105,19657,19218,18791,18373,17965,17566,17176,16794,16421,16055,15698,15348,15005,14669,14340,14017,13701,13391,13087,
12789,12497,12211,11930,11654,11384,11119,10859,10604,10354,10109,9869,9634,9404,9178,8957,8741,8530,8323,8121,
7924,7731,7543,7360,7181,7007,6837,6673,6513,6357,6206,6060,5919,5782,5650,5522,5399,5281,5167,5058,
4953,4853,4757,4665,4578,4495,4416,4341,4270,4203,4140,4080,4023,3971,3921,3874,3830,3789,3750,3713,
3679,3646,3614,3584,3555,3526,3498,3469,3440,3411,3380,3347,3312,3275,3235,3192,3144,3092,3035,2973,
2904,2828,2745,2653,2553,2443,2323,2192,2049,1893};
#define RVALS_CNT (sizeof(Rvals)/sizeof(Rvals[0]))

class ServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(BLEServer* pServer,NimBLEConnInfo& connInfo) {
      connectedTime = millis();
      if (!connectedTime) connectedTime = 1; // 1/4b chance of starting at rollover!
      fprintf(stderr,"Connected\n");
    };
 
    void onDisconnect(BLEServer* pServer,NimBLEConnInfo& connInfo, int reason) {
      connectedTime = 0;
      fprintf(stderr,"Disconnected\n");
    }
} serverCallbacks;

class SettCallback: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) {
    cellSett = pChar->getValue<CellSettings>();
  }
} settCallback;

void NoDrain() {
  gpio_set_level(DUMP,LOW);
  cs.draining = 0;
  goalDrainMSecs = 0;
}

void CheckDrain() {
  uint32_t diffMSec=0;
  if (goalDrainMSecs)
    diffMSec = millis() - startDrainMSecs;
  if (((cellSett.drainV && cs.volts > cellSett.drainV) || (diffMSec < goalDrainMSecs)) && cs.tempBd < 55) {
    gpio_set_level(DUMP,HIGH);
    cs.draining = 1;
  } else {
    gpio_set_level(DUMP,LOW);
    cs.draining = 0;
    if (cs.tempBd < 55)
      goalDrainMSecs = 0;
  }
}

class DumpCallback: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) {
    NoDrain();
    goalDrainMSecs = 1000l * pChar->getValue<uint32_t>();
    if (goalDrainMSecs > 604800000)
      goalDrainMSecs = 0; // 1 week, we don't believe it
    startDrainMSecs = millis();
    CheckDrain();
  }
} dumpCallback;

void readData(i2c_master_dev_handle_t dev_handle) {
  gpio_set_level(TEMPPWR, HIGH);
  uint32_t mV0;
  uint16_t mV1;
  ads1115_t ads = ads1115_config(dev_handle);
  ads1115_set_pga(&ads,ADS1115_FSR_6_144);
  ads1115_set_mode(&ads,ADS1115_MODE_SINGLE);
  //ads1115_set_rdy_pin(&ads, GPIO_NUM_19);
  ads1115_set_sps(&ads,ADS1115_SPS_860);

  ads1115_set_mux(&ads, ADS1115_MUX_2_GND);
  cs.volts=ads1115_get_mV(&ads);
  uint32_t curTime = millis();
  while ((millis() - curTime) < 10) ; // let temp resistor get volts because of the capacitor and resistor delay

  ads1115_set_mux(&ads, ADS1115_MUX_1_GND);
  mV0=ads1115_get_mV(&ads);
  ads1115_set_mux(&ads, ADS1115_MUX_0_GND);
  mV1=ads1115_get_mV(&ads);
  gpio_set_level(TEMPPWR,LOW);

  cs.tempExt = INT8_MIN;
  if (cs.volts > mV0) {
    uint32_t Rt = 20000 * mV0 / (cs.volts - mV0);
    uint32_t l=0,h=RVALS_CNT,lastV=RVALS_CNT+1,v=0;

    while (v != lastV) {
      lastV = v;
      v = (l + h) / 2;
      if (Rt < Rvals[v]) l = v;
      else h = v;
    }
    if (v && v < RVALS_CNT) cs.tempExt = v/2-30;
  }
  cs.tempBd = BMSComputeTemp(mV1,false,cs.volts ? cs.volts : 3300,BCOEF,47000,51000);
  //fprintf(stderr,"V: %d, Tb: %d, Tx: %d, D: %d, Ms: %lu\n",cs.volts,cs.tempBd,cs.tempExt,cs.draining, millis());
}
void startAdvertising(NimBLEAdvertising *pAdvertising) {
  pAdvertising->addServiceUUID(NimBLEUUID((uint16_t)0x180F));
  pAdvertising->setName("JBT");
  pAdvertising->setPreferredParams(12,4000);
  pAdvertising->enableScanResponse(true);
  pAdvertising->start();
}
extern "C" void app_main() {
  gpio_set_direction(GLED, GPIO_MODE_OUTPUT);
  gpio_set_direction(TEMPPWR, GPIO_MODE_OUTPUT);
  gpio_set_direction(DUMP, GPIO_MODE_OUTPUT);
  gpio_set_level(GLED,HIGH);
  vTaskDelay(100);
  gpio_set_level(GLED,LOW);
  fprintf(stderr,"alive\n");

  cellSett.time = 2000;
  cellSett.drainV = 3400;
  esp_pm_config_t pm_config = {
      .max_freq_mhz = 240, // e.g. 80, 160, 240
      .min_freq_mhz = 40, // e.g. 40
      .light_sleep_enable = true, // enable light sleep
  };
  ESP_ERROR_CHECK( esp_pm_configure(&pm_config) );
  fprintf(stderr,"start nimble\n");
  NimBLEDevice::init("JBT");
  fprintf(stderr,"MAC: %s\n",NimBLEDevice::toString().c_str());
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&serverCallbacks);
  NimBLEService *pService = pServer->createService(NimBLEUUID((uint16_t)0x180F));

  pStat = pService->createCharacteristic(NimBLEUUID((uint16_t)0x2B18),NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  pDump = pService->createCharacteristic(NimBLEUUID((uint16_t)0X2AE2),NIMBLE_PROPERTY::WRITE);
  pDump->setCallbacks(&dumpCallback);

  pSett = pService->createCharacteristic(NimBLEUUID((uint16_t)0x2B15),NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pSett->setCallbacks(&settCallback);
  pSett->setValue<CellSettings>(cellSett);
  pServer->start();

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  startAdvertising(pAdvertising);

  TickType_t xLastWakeTime= xTaskGetTickCount();
  i2c_master_bus_config_t conf = {
    .i2c_port = I2C_NUM_0,
    .sda_io_num = GPIO_NUM_21,         // select GPIO specific to your project
    .scl_io_num = GPIO_NUM_22,         // select GPIO specific to your project
    .clk_source = I2C_CLK_SRC_APB ,
    .glitch_ignore_cnt = 7,
    .intr_priority = 0,
    .trans_queue_depth = 0, 
    .flags = {
      .enable_internal_pullup = false,
      .allow_pd = false
    }
  };

  ESP_ERROR_CHECK(i2c_new_master_bus(&conf,&bus_handle));
  i2c_device_config_t dev_cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address = 0x48,
    .scl_speed_hz = 100000,
    .scl_wait_us = 0,
    .flags = { .disable_ack_check = false }
  };
  i2c_master_dev_handle_t dev_handle;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle));
  //conf.clk_flags = I2C_SCLK_SRC_FLAG_LIGHT_SLEEP;          /*!< Optional, you can use I2C_SCLK_SRC_FLAG_* flags to choose i2c source clock here. */
  for( ;; ) {
    if (!connectedTime) {
      NoDrain();
      NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
      if (!pAdvertising->isAdvertising()) {
        fprintf(stderr,"start ad\n");
        esp_restart();
        startAdvertising(pAdvertising);
//        pm_config.light_sleep_enable = false;
//        ESP_ERROR_CHECK( esp_pm_configure(&pm_config) );
      }
      gpio_set_level(GLED,ledOn ? LOW : HIGH);
      ledOn = !ledOn;
    } else if ((millis() - connectedTime) > 1000) { // wait 1 sec to hopefully let the client get the service
      gpio_set_level(GLED, HIGH);
      if (NimBLEDevice::getAdvertising()->isAdvertising()) {
        fprintf(stderr,"stop ad\n");
        NimBLEDevice::stopAdvertising();
//        pm_config.light_sleep_enable = true;
//        ESP_ERROR_CHECK( esp_pm_configure(&pm_config) );
      }
      readData(dev_handle);
      CheckDrain();
      pStat->setValue<CellStatus>(cs); // send status
      pStat->notify();
      gpio_set_level(GLED, LOW);
    }

    vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS(cellSett.time) );
  }
}

