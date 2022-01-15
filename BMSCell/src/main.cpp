#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include <NimBLEDevice.h>
#include <BMSAll.h>
#include <BMSADC.h>
#include <CellData.h>

#define BATTV GPIO_NUM_25
#define DUMP GPIO_NUM_27
#define TEMPPWR GPIO_NUM_26
#define VADC ADC1_CHANNEL_6
#define TExADC ADC1_CHANNEL_3
#define TBdADC ADC1_CHANNEL_0
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
      printf("Connected\n");
    };
 
    void onDisconnect(BLEServer* pServer) {
      devConn = false;
      printf("Disconnected\n");
    }
};

class SettCallback: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) {
    cellSett = pChar->getValue<CellSettings>();
    if (!cellSett.cnt)
      cellSett.cnt = 1;
    printf("T: %d, C: %d, D: %d Leave On: %s\n",cellSett.time,cellSett.cnt,cellSett.delay,cellSett.resPwrOn?"Y":"N");
  }
};

void EndDrain() {
  gpio_set_level(DUMP,LOW);
  cs.draining = 0;
  cs.drainSecs = 0;
  startDrainMSecs = 0;
  goalDrainMSecs = 0;
}

void CheckDrain() {
  uint32_t diffMSec = 0;
  if (cs.draining) {
    diffMSec = millis() - startDrainMSecs;
    cs.drainSecs = (drainMSecs + diffMSec) / 1000;
    if (cs.tempBd >= 55) {
      gpio_set_level(DUMP,LOW);
      cs.draining = 0;
      drainMSecs += diffMSec;
      startDrainMSecs = 0;
    } else if ((drainMSecs + diffMSec) > goalDrainMSecs)
      EndDrain();
  } else if (goalDrainMSecs && cs.tempBd < 55) {
    gpio_set_level(DUMP,HIGH);
    startDrainMSecs = millis();
    cs.draining = 1;
  }
}

class DumpCallback: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) {
    EndDrain();
    goalDrainMSecs = 1000l * pChar->getValue<uint32_t>();
    printf("D: %d\n",goalDrainMSecs);
    CheckDrain();
  }
};

void readData() {
  gpio_set_level(GLED, HIGH);
  gpio_set_level(BATTV, HIGH);
  gpio_set_level(TEMPPWR, HIGH);
  uint32_t ct = millis();
  while ((millis() - ct) < cellSett.delay) ; // We don't want a vtaskdelay, because that will shut down things

  cs.volts=(uint16_t)(BMSReadVoltage(VADC,cellSett.cnt)*(V_RES_BOT+V_RES_TOP)/V_RES_BOT);
  cs.tempExt = BMSReadTemp(TExADC,false,cs.volts,BCOEF,47000,51000,cellSett.cnt);
  cs.tempBd = BMSReadTemp(TBdADC,false,cs.volts,BCOEF,47000,51000,cellSett.cnt);
  if (!cellSett.resPwrOn) {
    gpio_set_level(TEMPPWR,LOW);
    gpio_set_level(BATTV,LOW);
  }
  pStat->setValue<CellStatus>(cs);
  pStat->notify();
//  printf("V: %d, Tb: %d, Tx: %d, D: %d, Ms: %d\n",cs.volts,cs.tempBd,cs.tempExt,cs.draining, millis());
  gpio_set_level(GLED,LOW);
}

extern "C" void app_main() {   
  gpio_set_direction(GLED, GPIO_MODE_OUTPUT);
  gpio_set_direction(BATTV, GPIO_MODE_OUTPUT);
  gpio_set_direction(TEMPPWR, GPIO_MODE_OUTPUT);
  gpio_set_direction(DUMP, GPIO_MODE_OUTPUT);

  BMSADCInit();
  adc1_config_channel_atten(VADC, ADC_ATTEN_DB_11);
  adc1_config_channel_atten(TExADC, ADC_ATTEN_DB_11);
  adc1_config_channel_atten(TBdADC, ADC_ATTEN_DB_11);
  cellSett.time = 2000;
  cellSett.cnt = 4;
  cellSett.delay = 10;
  esp_pm_config_esp32_t pm_config = {
      .max_freq_mhz = 80, // e.g. 80, 160, 240
      .min_freq_mhz = 40, // e.g. 40
      .light_sleep_enable = true, // enable light sleep
  };
  ESP_ERROR_CHECK( esp_pm_configure(&pm_config) );
  NimBLEDevice::init("LiFePo4 Cell");
  printf("MAC: %s\n",NimBLEDevice::toString().c_str());fflush(stdout);
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  NimBLEService *pService = pServer->createService(BLEUUID((uint16_t)0x180F));

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

  TickType_t xLastWakeTime= xTaskGetTickCount();
  for( ;; ) {
    if (!devConn) {
      if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
        EndDrain();
        printf("start ad\n");
        NimBLEDevice::startAdvertising();
      }
      gpio_set_level(GLED,ledOn ? LOW : HIGH);
      ledOn = !ledOn;
    } else {
      if (NimBLEDevice::getAdvertising()->isAdvertising()) {
        acTime = millis();
        printf("stop ad\n");
        NimBLEDevice::stopAdvertising();
      }
      readData();
    }
    CheckDrain();
    vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS(cellSett.time) );
  }
}
