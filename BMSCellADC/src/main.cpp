#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include <NimBLEDevice.h>
#include <BMSAll.h>
#include <CellData.h>
#include "ads1115.h"

#define BATTV GPIO_NUM_25
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
uint32_t drainMSecs = 0,startDrainMSecs=0,goalDrainMSecs=0;
uint32_t slTime=0,awTime=0,acTime=0;

class MyServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      devConn = true;
      fprintf(stderr,"Connected\n");
    };
 
    void onDisconnect(BLEServer* pServer) {
      devConn = false;
      fprintf(stderr,"Disconnected\n");
    }
};

class SettCallback: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) {
    cellSett = pChar->getValue<CellSettings>();
    if (!cellSett.cnt)
      cellSett.cnt = 1;
    //fprintf(stderr,"T: %d, C: %d, D: %d Leave On: %s drain V: %d\n",cellSett.time,cellSett.cnt,cellSett.delay,cellSett.resPwrOn?"Y":"N",cellSett.drainV);
  }
};

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
  void onWrite(NimBLECharacteristic* pChar) {
    NoDrain();
    goalDrainMSecs = 1000l * pChar->getValue<uint32_t>();
    if (goalDrainMSecs > 604800000)
      goalDrainMSecs = 0; // 1 week, we don't believe it
    startDrainMSecs = millis();
    fprintf(stderr,"D: %d\n",goalDrainMSecs);
    CheckDrain();
  }
};

void readData(ads1115_t * ads) {
  gpio_set_level(GLED, HIGH);
  gpio_set_level(BATTV, HIGH);
  gpio_set_level(TEMPPWR, HIGH);
  uint32_t ct = millis();
  while ((millis() - ct) < cellSett.delay) ; // We don't want a vtaskdelay, because that will shut down things
fprintf(stderr,"reading\n");
  cs.volts=ads1115_get_mV(ads);
fprintf(stderr,"read %d\n",cs.volts);
//  cs.tempExt = BMSReadTemp(TExADC,false,cs.volts,BCOEF,47000,51000,cellSett.cnt);
//  cs.tempBd = BMSReadTemp(TBdADC,false,cs.volts,BCOEF,47000,51000,cellSett.cnt);
  if (!cellSett.resPwrOn) {
    gpio_set_level(TEMPPWR,LOW);
    gpio_set_level(BATTV,LOW);
  }
//  fprintf(stderr,"V: %d, Tb: %d, Tx: %d, D: %d, Ms: %d\n",cs.volts,cs.tempBd,cs.tempExt,cs.draining, millis());
  gpio_set_level(GLED,LOW);
}

extern "C" void app_main() {   
  gpio_set_direction(GLED, GPIO_MODE_OUTPUT);
        gpio_set_level(GLED,HIGH);
  gpio_set_direction(BATTV, GPIO_MODE_OUTPUT);
  gpio_set_direction(TEMPPWR, GPIO_MODE_OUTPUT);
  gpio_set_direction(DUMP, GPIO_MODE_OUTPUT);

  cellSett.time = 2000;
  cellSett.cnt = 4;
  cellSett.delay = 10;
  cellSett.drainV = 3400;
  esp_pm_config_esp32_t pm_config = {
      .max_freq_mhz = 80, // e.g. 80, 160, 240
      .min_freq_mhz = 40, // e.g. 40
      .light_sleep_enable = true, // enable light sleep
  };
  ESP_ERROR_CHECK( esp_pm_configure(&pm_config) );
  NimBLEDevice::init("LiFePo4 Cell");
  fprintf(stderr,"MAC: %s\n",NimBLEDevice::toString().c_str());
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  NimBLEService *pService = pServer->createService(BLEUUID((uint16_t)0x180F));
  fprintf(stderr,"------qert!!!!!!-------------\n");

  pStat = pService->createCharacteristic(BLEUUID((uint16_t)0x2B18),NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  pDump = pService->createCharacteristic(BLEUUID((uint16_t)0X2AE2),NIMBLE_PROPERTY::WRITE);
  pDump->setCallbacks(new DumpCallback());

  pSett = pService->createCharacteristic(BLEUUID((uint16_t)0x2B15),NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pSett->setCallbacks(new SettCallback());
  pSett->setValue<CellSettings>(cellSett);

  
  pService->start();
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLEUUID((uint16_t)0x180F));
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x12);
  pAdvertising->start();

  int i2c_master_port = 0;
  i2c_config_t conf;
  conf.mode = I2C_MODE_MASTER;
  conf.sda_io_num = GPIO_NUM_21;         // select GPIO specific to your project
  conf.sda_pullup_en = GPIO_PULLUP_DISABLE;
  conf.scl_io_num = GPIO_NUM_22;         // select GPIO specific to your project
  conf.scl_pullup_en = GPIO_PULLUP_DISABLE;
  conf.master.clk_speed = 100000;  // select frequency specific to your project
  //conf.clk_flags = I2C_SCLK_SRC_FLAG_LIGHT_SLEEP;          /*!< Optional, you can use I2C_SCLK_SRC_FLAG_* flags to choose i2c source clock here. */
  i2c_param_config(i2c_master_port, &conf);

  i2c_driver_install(i2c_master_port, conf.mode, 0, 0, 0);

  ads1115_t ads = ads1115_config(i2c_master_port,0x48);
  ads1115_set_pga(&ads,ADS1115_FSR_6_144);
  ads1115_set_mode(&ads,ADS1115_MODE_SINGLE);
  ads1115_set_mux(&ads,   ADS1115_MUX_2_GND);

  cs.volts=ads1115_get_mV(&ads);

  TickType_t xLastWakeTime= xTaskGetTickCount();
  for( ;; ) {
    i2c_param_config(i2c_master_port, &conf);
    i2c_driver_install(i2c_master_port, conf.mode, 0, 0, 0);
    if (!devConn) {
      NoDrain();
      if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
        fprintf(stderr,"start ad\n");
        NimBLEDevice::startAdvertising();
      }
      gpio_set_level(GLED,ledOn ? LOW : HIGH);
      ledOn = !ledOn;
    } else {
      if (NimBLEDevice::getAdvertising()->isAdvertising()) {
        acTime = millis();
        fprintf(stderr,"stop ad\n");
        NimBLEDevice::stopAdvertising();
      }
      readData(&ads);
      CheckDrain();
      pStat->setValue<CellStatus>(cs); // send status
      pStat->notify();
    }
    vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS(cellSett.time) );
  }
}
