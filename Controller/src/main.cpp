#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <EEPROM.h>
#include <INA.h>
#include "ArduinoJson.h"
#include "ESP32_MailClient.h"
#include "crc16.h"
#include <Ticker.h>
#include <Wire.h>
#include <PacketSerial.h>
#include "defines.h"
#include "PacketRequestGenerator.h"
#include "PacketReceiveProcessor.h"

//17
//16
#define GREEN_LED(x) digitalWrite(GPIO_NUM_4,x)
#define BLUE_LED(x) digitalWrite(GPIO_NUM_0,x)

Queue requestQueue(sizeof(packet), 16, FIFO);

PacketRequestGenerator prg = PacketRequestGenerator(&requestQueue);

PacketReceiveProcessor receiveProc = PacketReceiveProcessor();
uint16_t sequence = 0;
uint8_t counter = 0;

#define framingmarker (uint8_t)0x00

PacketSerial_<COBS, framingmarker, 128> myPacketSerial;

const int relayPins[RELAY_TOTAL] = { GPIO_NUM_23,GPIO_NUM_14,GPIO_NUM_27,GPIO_NUM_26,GPIO_NUM_25,GPIO_NUM_33 };

struct WiFiSettings wifiSets;
struct EmailSettings eSets;
struct BattSettings battSets;

char spb[1024];

uint32_t curMs = 0;
bool ledState = false;
CellModuleInfo cmi[maximum_bank_of_modules][maximum_cell_modules];
bool sendEmail = false;
AsyncWebServer server(80);
SMTPData smtpData;
uint8_t previousRelayState[RELAY_TOTAL];
bool doEmail = false;
String emailRes = "";

INA_Class         INA;
int INADevs;
int stateOfCharge,milliRolls,curTemp1,curTemp2;
int browserNBanks=-1,browserNCells=-1;
int32_t lastMicroAmps,lastPVMicroAmps,lastAdjMillAmpHrs = 0;
uint32_t lastMillis=0,lastShuntMillis;
int64_t milliAmpMillis,battMilliAmpMillis;
uint16_t lastPackMilliVolts = 0xffff;
int64_t aveAdjMilliAmpMillis = 0,curAdjMilliAmpMillis = 0;
int numAveAdj = 0,BatAHMeasured = 0,lastTrip = 0;
bool stateOfChargeValid=false;
bool cellsTimedOut,maxCellVState,minCellVState
  ,maxPackVState,minPackVState
  ,maxCellCState=false,minCellCState=false
  ,maxPackCState,minPackCState;
Ticker myShuntCounter,statusTimer,myLazyTimer,myTransmitTimer,myTimer;

void timerShuntCallback() {
  if (INADevs > 0) {
    lastPackMilliVolts = INA.getBusMilliVolts(0);
    if (INADevs > 1)
      lastPVMicroAmps = INA.getBusMicroAmps(1);
    lastMicroAmps = -INA.getBusMicroAmps(0);
    uint32_t thisMillis = millis();
    milliAmpMillis += (int64_t)lastMicroAmps * (thisMillis - lastShuntMillis) / 1000;
    lastShuntMillis = thisMillis;
    if (milliAmpMillis < 0) {
      curAdjMilliAmpMillis -= milliAmpMillis;
      milliAmpMillis = 0;
    } else if (milliAmpMillis > battMilliAmpMillis) {
      curAdjMilliAmpMillis += battMilliAmpMillis - milliAmpMillis;
      milliAmpMillis = battMilliAmpMillis;
    }
    if (battMilliAmpMillis != 0)
      stateOfCharge = milliAmpMillis * 100 / battMilliAmpMillis;
  }
  if (battSets.nBanks > 0 && (INADevs == 0 || lastPackMilliVolts < 1000)) { // low side shunt probably
    lastPackMilliVolts = 0;
    for (int8_t i = 0; i < battSets.nCells; i++)
      lastPackMilliVolts += cmi[0][i].voltagemV;
  }
}


//Lazy load the config data - Every 10 seconds see if there is a module we don't have configuration data for, if so request it
void timerLazyCallback()
{
//Find the first module that doesn't have settings cached and request them
//we only do 1 module at a time to avoid flooding the queue
  for (uint8_t bank = 0; bank < battSets.nBanks; bank++)
  {
    for (uint8_t module = 0; module < battSets.nCells; module++)
    {
      if (!cmi[bank][module].settingsCached)
      {
        prg.sendGetSettingsRequest(bank, module);
      }
    }
  }
}

void onPacketReceived(const uint8_t *receivebuffer, size_t len)
{
  //Note that this function gets called frequently with zero length packets
  //due to the way the modules operate
  GREEN_LED(HIGH);
  if (len == sizeof(packet))
    receiveProc.ProcessReply(receivebuffer, sequence);

  GREEN_LED(LOW);
}

void timerEnqueueCallback()
{

  //this is called regularly on a timer, it determines what request to make to the modules (via the request queue)
  for (uint8_t b = 0; b < battSets.nBanks; b++)
  {

    prg.sendCellVoltageRequest(b);
    prg.sendCellTemperatureRequest(b);

    //If any module is in bypass then request PWM reading for whole bank
    for (uint8_t m = 0; m < battSets.nCells; m++)
    {
      if (cmi[b][m].inBypass)
      {
        prg.sendReadBalancePowerRequest(b);
        break;
      }
    }

    //Every 50 loops also ask for bad packet count (saves battery power if we dont ask for this all the time)
    if (counter % 50 == 0)
    {
      prg.sendReadBadPacketCounter(b);
    }
  }

  //It's an unsigned byte, let it overflow to reset
  counter++;
}

void dumpPacketToDebug(packet *buffer)
{
  Serial.print(buffer->address, HEX);
  Serial.print('/');
  Serial.print(buffer->command, HEX);
  Serial.print('/');
  Serial.print(buffer->sequence, HEX);
  Serial.print('=');
  for (size_t i = 0; i < maximum_cell_modules; i++)
  {
    Serial.print(buffer->moduledata[i], HEX);
    Serial.print(" ");
  }
  Serial.print(" =");
  Serial.print(buffer->crc, HEX);
}

void timerTransmitCallback()
{
  // Called to transmit the next packet in the queue need to ensure this procedure is called more frequently than
  // items are added into the queue
  if (!requestQueue.isEmpty())
  {
    packet transmitBuffer;


    //Wake up the connected cell module from sleep
    Serial2.write(framingmarker);
    delay(3);

    requestQueue.pop(&transmitBuffer);
    sequence++;
    transmitBuffer.sequence = sequence;
    transmitBuffer.crc = CRC16::CalculateArray((uint8_t *)&transmitBuffer, sizeof(packet) - 2);
    myPacketSerial.send((byte *)&transmitBuffer, sizeof(transmitBuffer));

    //Grab the time we sent this packet to time how long packets take to move
    //through the modules.  We only time the COMMAND::ReadVoltageAndStatus packets
    if (transmitBuffer.command == COMMAND::ReadVoltageAndStatus)
    {
      receiveProc.packetLastSentSequence = sequence;
      receiveProc.packetLastSentMillisecond = millis();
    }

    // Output the packet we just transmitted to debug console
#if 0
    Serial.print("S:");
    dumpPacketToDebug(&transmitBuffer);
    Serial.print("/Q:");
    Serial.println(requestQueue.getCount());
#endif

  }
}

void doAHCalcs() {
    aveAdjMilliAmpMillis = ((aveAdjMilliAmpMillis*numAveAdj) + curAdjMilliAmpMillis)/(numAveAdj+1);
    numAveAdj++;
    lastAdjMillAmpHrs = (int32_t)(curAdjMilliAmpMillis / ((int64_t)1000 * 60 * 60));
    curAdjMilliAmpMillis = 0;
}

int getTemp(int pin) {
  int a = analogRead(pin);
  float r = 10000.0*(4095.0-a)/a;
  float l = log(r/0.01763226979); //0.01763226979 =10000*exp(-3950/298.15)
  float t = 3950.0/l;
  return (int)(t-273.15);
}

void checkStatus()
{
  if (INADevs > 0 && lastPackMilliVolts == 0xffff)
    return; // blow out of here while waiting for first info

  uint32_t thisMillis = millis();
  digitalWrite(GPIO_NUM_32,HIGH);

  cellsTimedOut = receiveProc.HasCommsTimedOut();
  //if (cellsTimedOut)
  //  sendEmail("Cells Timed Out");

  bool allovervoltrec = true,allundervoltrec = true,hitOver=false,hitUnder=false;
  bool allovertemprec = true,allundertemprec = true;
  //Loop through cells
  for (int8_t m=0;m<battSets.nBanks;m++)
    for (int8_t i = 0; i < battSets.nCells; i++)
    {
      uint16_t cellV = cmi[m][i].voltagemV;

      if (cellV > battSets.limits[limits::Volt][limits::Cell][limits::Max][limits::Trip]) {
        if (!maxCellVState)
          hitOver = true;
        maxCellVState = true;
      }
      if (cellV > battSets.limits[limits::Volt][limits::Cell][limits::Max][limits::Rec])
        allovervoltrec = false;

      if (cellV < battSets.limits[limits::Volt][limits::Cell][limits::Min][limits::Trip]) {
        if (!minCellVState)
          hitUnder = true;
        minCellVState = true;
      }
      if (cellV < battSets.limits[limits::Volt][limits::Cell][limits::Min][limits::Rec])
        allundervoltrec = false;

      int8_t cellT = cmi[m][i].externalTemp;
      if (battSets.useex && cellT != -40) {
        if (cellT > battSets.limits[limits::Temp][limits::Cell][limits::Max][limits::Trip])
          maxCellCState = true;

        if (cellT > battSets.limits[limits::Temp][limits::Cell][limits::Max][limits::Rec])
          allovertemprec = false;

        if (cellT < battSets.limits[limits::Temp][limits::Cell][limits::Min][limits::Trip])
          minCellCState = true;

        if (cellT < battSets.limits[limits::Temp][limits::Cell][limits::Min][limits::Rec])
          allundertemprec = false;
      }
    }

  if (maxCellVState && allovervoltrec)
    maxCellVState = false;
  if (minCellVState && allundervoltrec)
    minCellVState = false;

  if (!battSets.useex || (maxCellCState && allovertemprec))
    maxCellCState = false;
  if (!battSets.useex || (minCellCState && allundertemprec))
    minCellCState = false;

  curTemp1 = getTemp(GPIO_NUM_35);
  curTemp2 = getTemp(GPIO_NUM_13);
  digitalWrite(GPIO_NUM_32,LOW);
  Serial.println(millis() - thisMillis);

  if (curTemp1 > battSets.limits[limits::Temp][limits::Pack][limits::Max][limits::Trip])
    maxPackCState = true;
  if (curTemp1 < battSets.limits[limits::Temp][limits::Pack][limits::Max][limits::Rec])
    maxPackCState = false;
  if (curTemp1 < battSets.limits[limits::Temp][limits::Pack][limits::Min][limits::Trip])
    minPackCState = true;
  if (curTemp1 > battSets.limits[limits::Temp][limits::Pack][limits::Min][limits::Rec])
    minPackCState = false;

  if (lastPackMilliVolts > battSets.limits[limits::Volt][limits::Pack][limits::Max][limits::Trip]) {
    if (!maxPackVState)
      hitOver = true;
    maxPackVState = true;
  } else if (lastPackMilliVolts < battSets.limits[limits::Volt][limits::Pack][limits::Max][limits::Rec])
    maxPackVState = false;

  if (lastPackMilliVolts < battSets.limits[limits::Volt][limits::Pack][limits::Min][limits::Trip]) {
    if (!minPackVState)
      hitUnder = true;
    minPackVState = true;
  } else if (lastPackMilliVolts > battSets.limits[limits::Volt][limits::Pack][limits::Min][limits::Rec])
    minPackVState = false;

  if (hitOver && milliAmpMillis < battMilliAmpMillis) {
    if (lastTrip != 0) {
      curAdjMilliAmpMillis += battMilliAmpMillis - milliAmpMillis;
      if (lastTrip < 0)
        BatAHMeasured = (milliAmpMillis + curAdjMilliAmpMillis) / ((uint64_t)1000 * 1000 * 60 * 60);
      lastTrip = 1;
      doAHCalcs();
    }
    milliAmpMillis = battMilliAmpMillis;
    stateOfChargeValid = true;
  }
  if (hitUnder && milliAmpMillis > 0) {
    if (lastTrip != 0) {
      curAdjMilliAmpMillis -= milliAmpMillis;
      if (lastTrip > 0)
        BatAHMeasured = (battMilliAmpMillis - milliAmpMillis + curAdjMilliAmpMillis) / ((uint64_t)1000 * 1000 * 60 * 60);
      lastTrip = -1;
      doAHCalcs();
    }
    milliAmpMillis = 0;
    stateOfChargeValid = true;
  }

  uint8_t relay[RELAY_TOTAL];
  //Set defaults based on configuration
  for (int8_t y = 0; y < RELAY_TOTAL; y++)
  {
    RelaySettings *rp = &battSets.relays[y];
    if (rp->off)
      relay[y] = LOW;
    else {
      relay[y] = previousRelayState[y]; // don't change it because we might be in the SOC trip/rec area
      if (rp->type == Relay_Load) {
        if (minCellVState || minPackVState || maxCellCState || maxPackCState || (rp->doSOC && (!stateOfChargeValid || stateOfCharge < rp->trip))
             || (cellsTimedOut && (!stateOfChargeValid || stateOfCharge < rp->trip)))
          relay[y] = LOW; // turn if off
        else if (!rp->doSOC || (rp->doSOC && stateOfChargeValid && stateOfCharge > rp->rec))
          relay[y] = HIGH; // turn it on
      } else {
        if (maxCellVState || maxPackVState || minCellCState || maxCellCState || minPackCState || maxPackCState || (rp->doSOC && (!stateOfChargeValid || stateOfCharge > rp->trip))
            || (cellsTimedOut && (!stateOfChargeValid || stateOfCharge > rp->trip)))
          relay[y] = LOW; // off
        else if (!rp->doSOC || (rp->doSOC && stateOfChargeValid && stateOfCharge < rp->rec))
          relay[y] = HIGH; // on
      }
    }
  }
  for (int8_t n = 0; n < RELAY_TOTAL; n++)
  {
    if (previousRelayState[n] != relay[n])
    {
      digitalWrite(relayPins[n], relay[n]);
      previousRelayState[n] = relay[n];
    }
  }
}

void emailCallback(SendStatus msg) {
  // Print the current status
  Serial.println(msg.info());

  // Do something when complete
  if (msg.success()) {
    Serial.println("----------------");
  }
}
void onRequest(AsyncWebServerRequest *request){
  //Handle Unknown Request
  request->send(404);
}

String UUID; // unused, maybe later for xss
void GenUUID() {
  uint32_t r;
  UUID = "";
  for (int i=0;i<2;i++) {
    r = esp_random();
    for (int j=0;j<(32/4);j++) {
      UUID += "0123456789abcdef"[r & 0xf];
      r >>= 4;
    }
  }
}

#define EEPROMSize 2048

bool readEE(uint8_t *p,size_t s,uint32_t start) {
  EEPROM.begin(EEPROMSize);
  for (int i=0;i<s;i++)
    *p++ = EEPROM.read(i+start);
  uint16_t ck;
  EEPROM.get(start + s, ck);
  EEPROM.end();
  uint16_t checksum = CRC16::CalculateArray(p, s);
  return checksum == ck;
}

void writeEE(uint8_t *p,size_t s,uint32_t start) {
  EEPROM.begin(EEPROMSize);
  for (int i=0;i<s;i++)
    EEPROM.write(i+start,*p++);
  uint16_t checksum = CRC16::CalculateArray(p, s);
  EEPROM.put(start+s,checksum);
  EEPROM.commit();
}

void doEmailSettings() {
  smtpData.setLogin(eSets.senderServer, eSets.senderPort, eSets.senderEmail, eSets.senderPW);
  smtpData.setSender("Your Battery", eSets.senderEmail);
  smtpData.setPriority("High");
  smtpData.setSubject(eSets.senderSubject);
  smtpData.setMessage("<div style=\"color:#2f4468;\"><h1>Hello World!</h1><p>- Sent from ESP32 board</p></div>", true);
  smtpData.addRecipient(eSets.email);

/*  smtpData.setLogin("smtp.gmail.com", 465, "john.taves@gmail.com","erwsmuigvggpvmtf");
*/
}

void setINAs() {
  myShuntCounter.detach();
  myShuntCounter.attach_ms(battSets.PollFreq < 500 ? 500 : battSets.PollFreq,timerShuntCallback);
  if (INADevs == 0)
    return;
  INA.begin(battSets.MaxAmps, battSets.ShuntUOhms,0);
  INA.begin(battSets.PVMaxAmps,battSets.PVShuntUOhms,1);
  INA.setShuntConversion(300,0);
  INA.setBusConversion(300,0);
  INA.setAveraging(10000,0);
  INA.setShuntConversion(battSets.ConvTime,1);
  INA.setBusConversion(battSets.ConvTime,1);
  INA.setAveraging(battSets.Avg,1);
}

void sendSuccess(AsyncWebServerRequest *request) { 
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  StaticJsonDocument<100> doc;
  doc["success"] = true;
  serializeJson(doc, *response);
  request->send(response);
}

void settings(AsyncWebServerRequest *request){
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(4096);
  JsonObject root = doc.to<JsonObject>();
  root["email"] = eSets.email;
  root["senderEmail"] = eSets.senderEmail;
  root["senderSubject"] = eSets.senderSubject;
  root["senderServer"] = eSets.senderServer;
  root["senderSubject"] = eSets.senderSubject;
  root["senderPort"] = eSets.senderPort;
 
  root["Avg"] = battSets.Avg;
  root["PollFreq"] = battSets.PollFreq;
  root["ConvTime"] = battSets.ConvTime;
  root["BattAH"] = battSets.BattAH;
  root["socLastAdj"] = lastAdjMillAmpHrs;
  snprintf(spb,sizeof(spb),"%d:%d",(int)(aveAdjMilliAmpMillis / ((int64_t)1000 * 60 * 60)),numAveAdj);
  root["socAvgAdj"] = spb;
  root["BatAHMeasured"] = BatAHMeasured > 0 ? String(BatAHMeasured) : String("N/A");

  root["MaxAmps"] = battSets.MaxAmps;
  root["ShuntUOhms"] = battSets.ShuntUOhms;
  root["PVMaxAmps"] = battSets.PVMaxAmps;
  root["PVShuntUOhms"] = battSets.PVShuntUOhms;
  root["nBanks"] = battSets.nBanks;
  root["nCells"] = battSets.nCells;
  root["useex"]=battSets.useex;

  JsonObject obj = root.createNestedObject("limitSettings");
  for (int l0=0;l0<limits::Max0;l0++) {
    for (int l1=0;l1<limits::Max1;l1++) {
      for (int l2=0;l2<limits::Max2;l2++) {
        for (int l3=0;l3<limits::Max3;l3++) {
          char name[5];
          sprintf(name,"%d%d%d%d",l0,l1,l2,l3);
          obj[name] = battSets.limits[l0][l1][l2][l3];
        }
      }
    }
  }

  JsonArray rsArray = root.createNestedArray("relaySettings");
  for (uint8_t r = 0; r < RELAY_TOTAL; r++) {
    JsonObject rule1 = rsArray.createNestedObject();
    rule1["name"] =battSets.relays[r].name;
    rule1["type"] =battSets.relays[r].type;
    rule1["doSOC"] =battSets.relays[r].doSOC;
    rule1["trip"] =battSets.relays[r].trip;
    rule1["rec"] =battSets.relays[r].rec;
  }

  root["ssid"] = wifiSets.ssid;
  serializeJson(doc, *response);
  request->send(response);
}

int toCel(String val) {
  if (battSets.doCelsius)
    return val.toInt();
  return (val.toInt() - 32) * 5/9;
}

int fromCel(int c) {
  if (battSets.doCelsius)
    return c;
  return c*9/5+32;
}

char* milliToStr(int32_t val) {
  int32_t v = val/10;
  snprintf(spb,sizeof(spb),"%s%d.%02d",(v < 0 ?"-":""),abs(v)/100,(abs(v) % 100));
  return spb;
}

void saveOff(AsyncWebServerRequest *request) {
  if (request->hasParam("relay", true)) {
    int r = request->getParam("relay", true)->value().toInt();
    RelaySettings *rp = &battSets.relays[r];
    rp->off = (rp->off + 1) % 2;
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    StaticJsonDocument<100> doc;
    writeEE((uint8_t*)&battSets, sizeof(battSets), EEPROM_BATT);
    doc["relay"] = r;
    doc["val"] = rp->off == 1 ? "off" : "on";
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);

  } else request->send(500, "text/plain", "Missing parameters");
}

void status(AsyncWebServerRequest *request){
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");

  DynamicJsonDocument doc(2048);
  JsonObject root = doc.to<JsonObject>();
 
  uint32_t upsecs = (milliRolls * 4294967ul) + (millis()/1000ul);
  int days = upsecs / (24ul*60*60);
  upsecs = upsecs % (24ul*60*60);
  int hrs = upsecs / (60*60);
  upsecs = upsecs % (60*60);
  int min = upsecs / 60;

  snprintf(spb,sizeof(spb),"%d:%02d:%02d:%02d",days,hrs,min,upsecs % 60);
  root["uptime"] = spb;
  root["RELAY_TOTAL"] = RELAY_TOTAL;
  for (int i=0;i<RELAY_TOTAL;i++) {
    char dodad[16];
    if (strlen(battSets.relays[i].name) == 0)
      continue;
    sprintf(dodad,"relayStatus%d",i);
    root[dodad] = previousRelayState[i]==HIGH?"ON":"OFF";
    sprintf(dodad,"relayName%d",i);
    root[dodad] = battSets.relays[i].name;
    sprintf(dodad,"relayOff%d",i);
    root[dodad] = battSets.relays[i].off == 1 ? "off" : "on";
  }

//  root["debuginfo"] = debugstr;

  root["packcurrent"] = milliToStr(lastMicroAmps/1000);
  root["packvolts"] = milliToStr(lastPackMilliVolts);
  root["pvcurrent"] = milliToStr(lastPVMicroAmps/1000);
  root["loadcurrent"] = milliToStr( (lastPVMicroAmps - lastMicroAmps)/1000);
  snprintf(spb,sizeof(spb),"%d%%",stateOfCharge);
  root["soc"] = spb;
  root["socvalid"] = stateOfChargeValid;
  root["temp1"] = fromCel(curTemp1);
  root["temp2"] = fromCel(curTemp2);
  root["maxCellVState"] = maxCellVState;
  root["minCellVState"] = minCellVState;
  root["maxPackVState"] = maxPackVState;
  root["minPackVState"] = minPackVState;
  root["maxCellCState"] = maxCellCState;
  root["minCellCState"] = minCellCState;
  root["maxPackCState"] = maxPackCState;
  root["minPackCState"] = minPackCState;
  root["nocells"] = cellsTimedOut;
  snprintf(spb,sizeof(spb),"%d/%d %d %dms",receiveProc.packetsReceived,prg.packetsGenerated,receiveProc.totalCRCErrors,receiveProc.packetTimerMillisecond);
  root["pkts"] = spb;

  if (browserNBanks != battSets.nBanks || browserNCells != battSets.nCells) {
    root["nBanks"] = battSets.nBanks;
    root["nCells"] = battSets.nCells;
    browserNBanks = battSets.nBanks;
    browserNCells = battSets.nCells;
  }

  JsonArray bankArray = root.createNestedArray("bank");
  for (uint8_t bank = 0; bank < battSets.nBanks; bank++) {
    JsonArray data = bankArray.createNestedArray();

    for (uint8_t i = 0; i < battSets.nCells; i++) {
      JsonObject cell = data.createNestedObject();
      cell["v"] = cmi[bank][i].voltagemV;
      cell["minv"] = cmi[bank][i].voltagemVMin;
      cell["maxv"] = cmi[bank][i].voltagemVMax;
      cell["bypass"] = cmi[bank][i].inBypass;
      cell["bypasshot"] = cmi[bank][i].bypassOverTemp;
      cell["int"] = cmi[bank][i].internalTemp;
      cell["ext"] = cmi[bank][i].externalTemp;
      cell["badpkt"] = cmi[bank][i].badPacketCount;
      cell["pwm"] = cmi[bank][i].inBypass? cmi[bank][i].PWMValue:0;
    }
  }

  serializeJson(doc, *response);
  request->send(response);
}
void firstStatus(AsyncWebServerRequest *request){
  browserNBanks = -1;
  return status(request);
}

void saverules(AsyncWebServerRequest *request) {
  for (int l0=0;l0<limits::Max0;l0++) {
    for (int l1=0;l1<limits::Max1;l1++) {
      for (int l2=0;l2<limits::Max2;l2++) {
        for (int l3=0;l3<limits::Max3;l3++) {
          char name[5];
          sprintf(name,"%d%d%d%d",l0,l1,l2,l3);
          if (request->hasParam(name, true, false))
            battSets.limits[l0][l1][l2][l3] = request->getParam(name, true, false)->value().toInt();
        }
      }
    }
  }

  for (int relay=0;relay<RELAY_TOTAL;relay++) {
    char name[16];
    RelaySettings *rp = &battSets.relays[relay];
    sprintf(name,"relayName%d",relay);
    if (request->hasParam(name, true))
      request->getParam(name, true)->value().toCharArray(rp->name,sizeof(rp->name));;
    
    sprintf(name,"relayType%d",relay);
    if (request->hasParam(name, true))
      rp->type = request->getParam(name, true)->value().toInt();

    sprintf(name,"relayDoSOC%d",relay);
    if (request->hasParam(name, true))
      rp->doSOC = request->getParam(name, true)->value().equals("on");
    else
      rp->doSOC = false;

    sprintf(name,"relayTrip%d",relay);
    if (request->hasParam(name, true))
      rp->trip = request->getParam(name, true)->value().toInt();

    sprintf(name,"relayRec%d",relay);
    if (request->hasParam(name, true))
      rp->rec = request->getParam(name, true)->value().toInt();

  }
  battSets.useex = request->hasParam("useex", true) && request->getParam("useex", true)->value().equals("on");

  if (!battSets.useex) {
    maxCellCState = false;
    minCellCState = false;
  }
  writeEE((uint8_t*)&battSets, sizeof(battSets), EEPROM_BATT);

  sendSuccess(request);
}

void saveemail(AsyncWebServerRequest *request){
  if (request->hasParam("email", true))
    request->getParam("email", true)->value().toCharArray(eSets.email,sizeof(eSets.email));
  if (request->hasParam("senderEmail", true))
    request->getParam("senderEmail", true)->value().toCharArray(eSets.senderEmail,sizeof(eSets.senderEmail));
  if (request->hasParam("senderPW", true))
    request->getParam("senderPW", true)->value().toCharArray(eSets.senderPW,sizeof(eSets.senderPW));
  if (request->hasParam("senderServer", true))
    request->getParam("senderServer", true)->value().toCharArray(eSets.senderServer,sizeof(eSets.senderServer));
  if (request->hasParam("senderSubject", true))
    request->getParam("senderSubject", true)->value().toCharArray(eSets.senderSubject,sizeof(eSets.senderSubject));
  if (request->hasParam("senderPort", true))
    eSets.senderPort = request->getParam("senderPort", true)->value().toInt();
  writeEE((uint8_t*)&eSets,sizeof(eSets),EEPROM_EMAIL);
  doEmailSettings();
  sendSuccess(request);
}

void startServer();
void WifiSetup(bool doAP) {
  browserNBanks = -1;
  browserNCells = -1;
  if (doAP || wifiSets.apMode)
  {
    WiFi.mode(WIFI_OFF);
    WiFi.mode(WIFI_AP);
    WiFi.softAP("BMS_CONTROLLER");
    Serial.print("AP: ");
    Serial.println(WiFi.softAPIP());
  }
  else
  {
    WiFi.mode(WIFI_OFF);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("BMS_CONTROLLER");
    WiFi.begin(wifiSets.ssid,wifiSets.password);
    WiFi.waitForConnectResult();
  }
}

void ondisconnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
  Serial.println("disconnected");
  WifiSetup(true);
}

void kickwifi(AsyncWebServerRequest *request) {
  WifiSetup(false);
}

void savewifi(AsyncWebServerRequest *request){
  if (request->hasParam("ssid", true))
    request->getParam("ssid", true)->value().toCharArray(wifiSets.ssid,sizeof(wifiSets.ssid));
  if (request->hasParam("password", true))
    request->getParam("password", true)->value().toCharArray(wifiSets.password,sizeof(wifiSets.password));

  wifiSets.apMode = strlen(wifiSets.ssid) == 0;
  writeEE((uint8_t*)&wifiSets,sizeof(wifiSets),EEPROM_WIFI);
  WifiSetup(false);
  sendSuccess(request);
}

void setStateOfCharge(int val) {
  milliAmpMillis = battMilliAmpMillis * val/100;
  stateOfChargeValid = true;
  curAdjMilliAmpMillis = 0;
  lastTrip = 0;
  aveAdjMilliAmpMillis = 0;
  numAveAdj = 0;
}

void savecapacity(AsyncWebServerRequest *request) {
  if (request->hasParam("CurSOC", true)) {
    AsyncWebParameter *p1 = request->getParam("CurSOC", true);
    if (p1->value().length() > 0) setStateOfCharge(p1->value().toInt());
  }
  if (request->hasParam("PollFreq", true)) {
    AsyncWebParameter *p1 = request->getParam("PollFreq", true);
    battSets.PollFreq =p1->value().toInt();
  }

  if (request->hasParam("Avg", true)) {
    AsyncWebParameter *p1 = request->getParam("Avg", true);
    battSets.Avg =p1->value().toInt();
  }
  if (request->hasParam("ConvTime", true)) {
    AsyncWebParameter *p1 = request->getParam("ConvTime", true);
    battSets.ConvTime =p1->value().toInt();
  }
  if (request->hasParam("BattAH", true)) {
    AsyncWebParameter *p1 = request->getParam("BattAH", true);
    battSets.BattAH =p1->value().toInt();
  }
  if (request->hasParam("MaxAmps", true)) {
    AsyncWebParameter *p1 = request->getParam("MaxAmps", true);
    battSets.MaxAmps =p1->value().toInt();
  }
  if (request->hasParam("ShuntUOhms", true)) {
    AsyncWebParameter *p1 = request->getParam("ShuntUOhms", true);
    battSets.ShuntUOhms =p1->value().toInt();
  }

  if (request->hasParam("PVMaxAmps", true)) {
    AsyncWebParameter *p1 = request->getParam("PVMaxAmps", true);
    battSets.PVMaxAmps =p1->value().toInt();
  }
  if (request->hasParam("PVShuntUOhms", true)) {
    AsyncWebParameter *p1 = request->getParam("PVShuntUOhms", true);
    battSets.PVShuntUOhms =p1->value().toInt();
  }
  if (request->hasParam("nBanks", true)) {
    AsyncWebParameter *p1 = request->getParam("nBanks", true);
    battSets.nBanks =p1->value().toInt();
  }
  if (request->hasParam("nCells", true)) {
    AsyncWebParameter *p1 = request->getParam("nCells", true);
    battSets.nCells =p1->value().toInt();
  }

  writeEE((uint8_t*)&battSets, sizeof(battSets), EEPROM_BATT);

  setINAs();

  sendSuccess(request);
}

void toggleTemp(AsyncWebServerRequest *request) {
  battSets.doCelsius = !battSets.doCelsius;
  writeEE((uint8_t*)&battSets, sizeof(battSets), EEPROM_BATT);
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  StaticJsonDocument<100> doc;
  doc["val"] = battSets.doCelsius;
  serializeJson(doc, *response);
  request->send(response);
}

void startServer() {
  Serial.println("In Connect");
  Serial.println(WiFi.localIP());

  server.on("/email", HTTP_POST, [](AsyncWebServerRequest *request){
    sendEmail = true;
    request->send(200, "text/plain", "OK Gonna send it");
  });

  server.on("/toggleTemp", HTTP_GET, toggleTemp);
  server.on("/saveemail", HTTP_POST, saveemail);
  server.on("/saveOff", HTTP_POST, saveOff);
  server.on("/kickwifi", HTTP_POST, kickwifi);
  server.on("/savewifi", HTTP_POST, savewifi);
  server.on("/savecapacity", HTTP_POST, savecapacity);
  server.on("/saverules", HTTP_POST, saverules);
  server.on("/settings", HTTP_GET, settings);
  server.on("/status1", HTTP_GET, firstStatus);
  server.on("/status", HTTP_GET, status);

  server.serveStatic("/static", SPIFFS, "/static").setLastModified("Mon, 20 Jun 2016 14:00:00 GMT");
  server.serveStatic("/", SPIFFS, "/");
  server.onNotFound(onRequest);

  server.begin();
}
void onconnect(WiFiEvent_t event, WiFiEventInfo_t info) {
  startServer();
}

void setup() {
  Serial.begin(9600);
  Serial2.begin(2400, SERIAL_8N1); // Serial for comms to modules
  if (!readEE((uint8_t*)&wifiSets, sizeof(wifiSets), EEPROM_WIFI))
    wifiSets.apMode = true;
  WiFi.onEvent(onconnect, WiFiEvent_t::SYSTEM_EVENT_STA_GOT_IP);
  WiFi.onEvent(onconnect, WiFiEvent_t::SYSTEM_EVENT_AP_START);
  WiFi.onEvent(ondisconnect, WiFiEvent_t::SYSTEM_EVENT_STA_DISCONNECTED);
  WifiSetup(false);
  uint8_t wifiStatus = WiFi.waitForConnectResult();
  if (wifiStatus != WL_CONNECTED) {
    Serial.println("Failed wifi trying AP");
    WifiSetup(true);
  }
  wifiStatus = WiFi.waitForConnectResult();
  if (!readEE((uint8_t*)&eSets,sizeof(eSets),EEPROM_EMAIL)) {
    eSets.email[0] = 0;
    eSets.senderEmail[0] = 0;
    eSets.senderServer[0] = 0;
    eSets.senderPort = 587;
    eSets.senderPW[0] = 0;
    eSets.senderSubject[0] = 0;
  } else doEmailSettings();

  if (!readEE((uint8_t*)&battSets,sizeof(battSets),EEPROM_BATT)) {
    battSets.ConvTime = 1000;
    battSets.PVMaxAmps = 100;
    battSets.PVShuntUOhms = 500;
    battSets.ShuntUOhms = 167;
    battSets.MaxAmps = 300;
    battSets.nBanks=0;
    battSets.nCells=0;
    battSets.doCelsius = true;
    battSets.PollFreq = 500;
    battSets.BattAH = 1;
    battSets.ConvTime = 1000;
    battSets.Avg = 1000;
    for (int i=0;i<RELAY_TOTAL;i++) {
      RelaySettings* r = &battSets.relays[i];
      r->name[0] = 0;
      r->doSOC = false;
      r->off = 0;
      r->rec = 0;
      r->trip = 0;
      r->type = 0;
    }
  }

  Wire.begin();
  if(!SPIFFS.begin()){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  myPacketSerial.setStream(&Serial2); // start serial for output
  myPacketSerial.setPacketHandler(&onPacketReceived);

  pinMode(GPIO_NUM_4, OUTPUT);
  pinMode(GPIO_NUM_0, OUTPUT);
  pinMode(GPIO_NUM_32, OUTPUT); // drive thermisters
  for (int i=0;i<RELAY_TOTAL;i++)
    pinMode(relayPins[i],OUTPUT);

  GenUUID();

  INADevs = INA.begin(battSets.MaxAmps, battSets.ShuntUOhms);
  lastShuntMillis = millis();
  battMilliAmpMillis = (uint64_t)battSets.BattAH * (1000 * 60 * 60) * 1000; // convert to milliampmilliseconds
  milliAmpMillis = battMilliAmpMillis * 80 / 100; /* No clue what the SOC was, so assume 80% */
  setINAs();

  //Ensure we service the cell modules every 4 seconds
  myTimer.attach(4, timerEnqueueCallback);

  //We process the transmit queue every 0.5 seconds (this needs to be lower delay than the queue fills)
  myTransmitTimer.attach(0.5, timerTransmitCallback);
  myLazyTimer.attach(10, timerLazyCallback);
  statusTimer.attach(5, checkStatus);
  smtpData.setSendCallback(emailCallback);
  Serial.println("done setup");
}

void loop() {
  if (sendEmail) {
      //Start sending Email, can be set callback function to track the status
    if (!MailClient.sendMail(smtpData))
      Serial.println("Error sending Email, " + MailClient.smtpErrorReason());

    sendEmail = false;
    Serial.printf("Hi");
  }
  if (lastMillis > millis()) milliRolls++;
  lastMillis = millis();
  myPacketSerial.update();

  if ((millis() - curMs) > (WiFi.getMode() == WIFI_MODE_AP ? 1000 : 2000)) {
    BLUE_LED(ledState ? LOW : HIGH);
    ledState = !ledState;
    curMs = millis();
  }
}
