#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_adc_cal.h>
#include <Wire.h>
#include <BMSComm.h>
#include <CellData.h>
#include <soc/rtc.h>

#define BATTV GPIO_NUM_25
#define DUMP GPIO_NUM_27
#define TEMPPWR GPIO_NUM_26
#define VADC GPIO_NUM_34
#define TExADC GPIO_NUM_39
#define TBdADC GPIO_NUM_36
#define GLED GPIO_NUM_4
#define BCOEF 4050

#define V_RES_TOP 3000l
#define V_RES_BOT 10000l

NimBLECharacteristic *pStat,*pDump,*pSett,*pSleep;
NimBLEServer *pServer;
bool devConn=false,ledOn=false;
CellSettings cellSett;
CellStatus cs;
uint32_t drainMSecs = 0,startDrainMSecs=0,goalDrainMSecs=0,flashMSecs=0;
volatile uint32_t pauseMSecs=0;
uint32_t slTime=0,awTime=0,acTime=0;

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
    writeEE((uint8_t*)&cellSett,sizeof(cellSett),0);
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

uint32_t calibrate_one(rtc_cal_sel_t cal_clk)
{
	const uint32_t cal_count = 1000;
	//const float factor = (1 << 19) * 1000.0f;
	uint32_t cali_val;
	// printf("%s:\n", name);

	for (int i = 0; i < 5; ++i)
	{
		// printf("calibrate (%d): ", i);
		cali_val = rtc_clk_cal(cal_clk, cal_count);
		//printf("%.3f kHz\n", factor / (float)cali_val);
	}
	return cali_val;
}

void readData() {
  digitalWrite(GLED,HIGH);
  digitalWrite(BATTV,HIGH);
  digitalWrite(TEMPPWR,HIGH);
  if (cellSett.delay)
    delay(cellSett.delay);
  cs.volts=(uint16_t)(BMSReadVoltage(VADC,cellSett.cnt)*(V_RES_BOT+V_RES_TOP)/V_RES_BOT);
  cs.tempExt = BMSReadTemp(TExADC,cs.volts,BCOEF,47000,51000,cellSett.cnt);
  cs.tempBd = BMSReadTemp(TBdADC,cs.volts,BCOEF,47000,51000,cellSett.cnt);
  if (!cellSett.resPwrOn) {
    digitalWrite(TEMPPWR,LOW);
    digitalWrite(BATTV,LOW);
  }
  pStat->setValue<CellStatus>(cs);
  pStat->notify();
  Serial.printf("V: %d, Tb: %d, Tx: %d, Ms: %d\n",cs.volts,cs.tempBd,cs.tempExt,millis());
  pauseMSecs = millis();
  digitalWrite(GLED,LOW);
}

void setup() {
  Serial.begin(9600);    
  delay(1000);
  setCpuFrequencyMhz(80);
  Serial.println("Bootstrap");Serial.flush();
  rtc_clk_32k_bootstrap(512);
  rtc_clk_32k_enable(true);
  rtc_clk_slow_freq_set(RTC_SLOW_FREQ_32K_XTAL);
  uint32_t cali_val = rtc_clk_cal(RTC_CAL_32K_XTAL, 1000);
  Serial.printf("Cal %d\n",cali_val);
  EEPROM.begin(sizeof(cellSett));
  if (!readEE((uint8_t*)&cellSett, sizeof(cellSett),0) || !cellSett.cnt) {
    cellSett.cnt = 4;
    cellSett.delay = 2;
    cellSett.resPwrOn = false;
    cellSett.time = 2000;
  }
  pinMode(BATTV,OUTPUT);
  pinMode(DUMP,OUTPUT);
  pinMode(TEMPPWR,OUTPUT);
  pinMode(GLED,OUTPUT);

  BMSInit(); 
  adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
  adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11);
  adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_11);

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
  pAdvertising->setMinPreferred(0x12);
  pAdvertising->start();
}

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
      Serial.println("ad");
      NimBLEDevice::stopAdvertising();
    }
    if ((millis() - pauseMSecs) > cellSett.time) {
      pauseMSecs = 0;
      readData();
      if (!pauseMSecs)
        pauseMSecs = millis();
// do sleep here
//digitalWrite(GLED,HIGH);
#ifdef SLEEP
Serial.printf("Sleep\n");Serial.flush();
uint32_t sl=millis();
for (int i=0;i<1;i++) {

esp_sleep_enable_timer_wakeup(5000UL);
esp_light_sleep_start();
}
//digitalWrite(GLED,LOW);
Serial.printf("Wake %d\n",millis()-sl);
#endif
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
