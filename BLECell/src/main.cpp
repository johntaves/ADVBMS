#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_adc_cal.h>
#include <EEPROM.h>
#include <CellData.h>
#include <BMSComm.h>

#define BATTV GPIO_NUM_25
#define DUMP GPIO_NUM_27
#define TEMPPWR GPIO_NUM_26
#define VADC GPIO_NUM_34
#define TExADC GPIO_NUM_39
#define TBdADC GPIO_NUM_36
#define GLED GPIO_NUM_4
#define BCOEF 4050

#define V_RES_TOP 3300l
#define V_RES_BOT 10000l

NimBLECharacteristic *pStat,*pDump,*pSett,*pSleep;
NimBLEServer *pServer;
bool devConn=false,ledOn=false;
CellSettings cellSett;
CellStatus cs;
uint32_t drainMSecs = 0,startDrainMSecs=0,goalDrainMSecs=0,flashMSecs=0;
volatile uint32_t pauseMSecs=0;

uint8_t CRC8(const uint8_t *data,int length) 
{
   uint8_t crc = 0x00;
   uint8_t extract;
   uint8_t sum;
   for(int i=0;i<length;i++)
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

bool readEE(uint8_t *p,size_t s) {
  EEPROM.readBytes(0,p,s);
  uint8_t checksum = CRC8(p, s);
  uint8_t ck = EEPROM.read(s);
  return checksum == ck;
}

void writeEE(uint8_t *p,size_t s) {
  uint8_t crc = CRC8(p, s);
  EEPROM.writeBytes(0,p,s);
  EEPROM.write(s,crc);
  EEPROM.commit();
}

void printSet() {
  Serial.printf("T: %d, C: %d, D: %d\n",cellSett.time,cellSett.cnt,cellSett.delay);
}
class MyServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      devConn = true;
    };
 
    void onDisconnect(BLEServer* pServer) {
      devConn = false;
    }
};

class SettCallback: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) {
    cellSett = pChar->getValue<CellSettings>();
    if (!cellSett.cnt)
      cellSett.cnt = 1;
    writeEE((uint8_t*)&cellSett,sizeof(cellSett));
    printSet();
  }
};

class DumpCallback: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) {
    goalDrainMSecs = 1000l * pChar->getValue<uint32_t>();
    drainMSecs = 0;
    Serial.printf("D: %d\n",goalDrainMSecs);
    cs.draining = 0;
    cs.drainSecs = 0;
    digitalWrite(DUMP,LOW);
  }
};

class SleepCallback: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) {
    Serial.printf("%d\n",pauseMSecs);
    pauseMSecs = millis();
  }
};

void setup() {
  Serial.begin(9600);
//  setCpuFrequencyMhz(80);
  if (!readEE((uint8_t*)&cellSett, sizeof(cellSett)) || !cellSett.cnt) {
    cellSett.cnt = 4;
    cellSett.delay = 2;
    cellSett.time = 2000;
  }
  printSet();
  pinMode(BATTV,OUTPUT);
  pinMode(DUMP,OUTPUT);
  pinMode(TEMPPWR,OUTPUT);
  pinMode(GLED,OUTPUT);

  analogReadResolution(11);
  // Calibration function
  esp_adc_cal_value_t val_type = BMSInit();
  if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
      Serial.println("eFuse Vref");
  } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
      Serial.println("Two Point");
  } else {
      Serial.println("Default");
  }
  adc1_config_channel_atten(ADC1_CHANNEL_5, ADC_ATTEN_DB_11);
  
  NimBLEDevice::init("LiFePo4 Cell");
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  NimBLEService *pService = pServer->createService(BLEUUID((uint16_t)0x180F));

  pStat = pService->createCharacteristic(BLEUUID((uint16_t)0x2B18),NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  pDump = pService->createCharacteristic(BLEUUID((uint16_t)0X2AE2),NIMBLE_PROPERTY::WRITE);
  pDump->setCallbacks(new DumpCallback());
  pSleep = pService->createCharacteristic(BLEUUID((uint16_t)0x2B45),NIMBLE_PROPERTY::WRITE);
  pSleep->setCallbacks(new SleepCallback());

  pSett = pService->createCharacteristic(BLEUUID((uint16_t)0x2B15),NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pSett->setCallbacks(new SettCallback());
  pSett->setValue<CellSettings>(cellSett);

  
  pService->start();
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLEUUID((uint16_t)0x180F));
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // functions that help with iPhone connections issue
  pAdvertising->setMinPreferred(0x12);
  pAdvertising->start();
}

void readData() {
  digitalWrite(GLED,HIGH);
  digitalWrite(BATTV,HIGH);
  if (cellSett.delay)
    delay(cellSett.delay);
  cs.volts=(uint16_t)(BMSReadVoltage(VADC,cellSett.cnt)*(V_RES_BOT+V_RES_TOP)/V_RES_BOT);
  digitalWrite(BATTV,LOW);
  digitalWrite(TEMPPWR,HIGH);
  if (cellSett.delay)
    delay(cellSett.delay);
  cs.tempBd = BMSGetTemp(TBdADC,cs.volts,BCOEF,47000,47000,cellSett.cnt);
  cs.tempExt = BMSGetTemp(TExADC,cs.volts,BCOEF,47000,47000,cellSett.cnt);
  digitalWrite(TEMPPWR,LOW);

  pStat->setValue<CellStatus>(cs);
  pStat->notify();
  //Serial.printf("V: %d, Tb: %d, Tx: %d, Ms: %d\n",cs.volts,cs.tempBd,cs.tempExt,millis());
  digitalWrite(GLED,LOW);
  pauseMSecs = millis();
  digitalWrite(GLED,LOW);
}

uint32_t slTime=0,awTime=0,acTime=0;

void loop() {
  if (!devConn) {
    if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
      digitalWrite(DUMP,LOW);
      cs.draining = 0;
      cs.drainSecs = 0;
      startDrainMSecs = 0;
      NimBLEDevice::startAdvertising();
    }
    if ((millis() - flashMSecs) > 2000) {
      digitalWrite(GLED,ledOn ? LOW : HIGH);
      ledOn = !ledOn;
      flashMSecs = millis();
    }
  } else {
    if (NimBLEDevice::getAdvertising()->isAdvertising()) {
      acTime = millis();
      Serial.printf("Sl: %d, Aw: %d, Ac: %d\n",slTime,awTime-slTime,acTime-awTime);

      NimBLEDevice::stopAdvertising();
    }
    if ((millis() - pauseMSecs) > cellSett.time) {
      Serial.printf("D: %d\n",millis() - pauseMSecs);
      pauseMSecs = 0;
      readData();
      if (!pauseMSecs)
        pauseMSecs = millis();
// do sleep here
    }
  }
  uint32_t diffMSec = 0;
  if (cs.draining) {
    diffMSec = millis() - startDrainMSecs;
    cs.drainSecs = (drainMSecs + diffMSec) / 1000;
    if (cs.tempBd >= 55) {
      digitalWrite(DUMP,LOW);
      cs.draining = 0;
      drainMSecs += diffMSec;
      startDrainMSecs = 0;
    } else if ((drainMSecs + diffMSec) > goalDrainMSecs) {
      digitalWrite(DUMP,LOW);
      cs.draining = 0;
      drainMSecs += diffMSec;
      startDrainMSecs = 0;
      cs.drainSecs = 0;
      goalDrainMSecs = 0;
    }
  } else if (goalDrainMSecs && cs.tempBd < 55) {
    digitalWrite(DUMP,HIGH);
    startDrainMSecs = millis();
    cs.draining = 1;
  }
}
