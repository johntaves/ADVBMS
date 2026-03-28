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
bool devConn=false,ledOn=false;
CellSettings cellSett;
CellStatus cs;
uint32_t startDrainMSecs=0,goalDrainMSecs=0;
uint32_t acTime=0;
i2c_master_bus_handle_t bus_handle;

class ServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(BLEServer* pServer,NimBLEConnInfo& connInfo) {
      devConn = true;
      fprintf(stderr,"Connected\n");
    };
 
    void onDisconnect(BLEServer* pServer,NimBLEConnInfo& connInfo, int reason) {
      devConn = false;
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
  uint16_t mV0,mV1;
  ads1115_t ads = ads1115_config(dev_handle);
  ads1115_set_pga(&ads,ADS1115_FSR_6_144);
  ads1115_set_mode(&ads,ADS1115_MODE_SINGLE);
  //ads1115_set_rdy_pin(&ads, GPIO_NUM_19);
  ads1115_set_sps(&ads,ADS1115_SPS_860);

  ads1115_set_mux(&ads, ADS1115_MUX_2_GND);
  cs.volts=ads1115_get_mV(&ads);

  uint32_t curTime = millis();
  while ((millis() - curTime) < 10) ; // let temp resistor get volts because of the capacitor and resistor delay

  ads1115_set_mux(&ads, ADS1115_MUX_0_GND);
  mV0=ads1115_get_mV(&ads);

  cs.tempExt = BMSComputeTemp(mV0,false,cs.volts ? cs.volts : 3300,BCOEF,47000,51000);

  ads1115_set_mux(&ads, ADS1115_MUX_1_GND);
  mV1=ads1115_get_mV(&ads);
  cs.tempBd = BMSComputeTemp(mV1,false,cs.volts ? cs.volts : 3300,BCOEF,47000,51000);
  gpio_set_level(TEMPPWR,LOW);
  fprintf(stderr,"V: %d, Tb: %d, Tx: %d, D: %d, Ms: %lu\n",cs.volts,cs.tempBd,cs.tempExt,cs.draining, millis());
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
  NimBLEDevice::init("LiFePo4 Cell");
  fprintf(stderr,"MAC: %s\n",NimBLEDevice::toString().c_str());
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&serverCallbacks);
  NimBLEService *pService = pServer->createService(BLEUUID((uint16_t)0x180F));

  pStat = pService->createCharacteristic(BLEUUID((uint16_t)0x2B18),NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  pDump = pService->createCharacteristic(BLEUUID((uint16_t)0X2AE2),NIMBLE_PROPERTY::WRITE);
  pDump->setCallbacks(&dumpCallback);

  pSett = pService->createCharacteristic(BLEUUID((uint16_t)0x2B15),NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pSett->setCallbacks(&settCallback);
  pSett->setValue<CellSettings>(cellSett);

  
  pService->start();
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLEUUID((uint16_t)0x180F));
  pAdvertising->setName("LiFePo4 Cell");
  pAdvertising->setPreferredParams(12,4000);
  pAdvertising->enableScanResponse(true);
  pAdvertising->start();

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
    if (!devConn) {
      NoDrain();
        fprintf(stderr,"start ad??\n");
      if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
        fprintf(stderr,"start ad\n");
        NimBLEDevice::startAdvertising();
      }
      gpio_set_level(GLED,ledOn ? LOW : HIGH);
      ledOn = !ledOn;
    } else {
      gpio_set_level(GLED, HIGH);
      if (NimBLEDevice::getAdvertising()->isAdvertising()) {
        acTime = millis();
        fprintf(stderr,"stop ad\n");
        NimBLEDevice::stopAdvertising();
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
