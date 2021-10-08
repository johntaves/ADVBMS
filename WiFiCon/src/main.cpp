#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <EEPROM.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <ESP32_MailClient.h>
#include <Wire.h>
#include <PacketSerial.h>

#include <time.h>
#include <Ticker.h>
#include "defines.h"
#include "CellData.h"
#include "CPUComm.h"

// improve server
// adjustable cell history
// cell dump safety, stop if cell gets low, show dump status
// charge control feature to reduce amps
// improve the SoC allow to run logic. Make the code bad, and see if the charge/loads go off
// email did not work going from unset to sent. A reboot seemed to help

char debugstr[200],lastEventMsg[1024];
int8_t lastEventMsgCnt=0;
bool emailSetup=false,writeCommSet=false,writeWifiSet=false,writeDispSet=false;
uint16_t resPwrMS=0;
HTTPClient http;
atomic_flag taskRunning(0);
bool OTAInProg = false;

BMSStatus st;

PacketSerial_<COBS, 0, sizeof(union MaxData)+10> dataSer;

#define frame (uint8_t)0x00

const int relayPins[RELAY_TOTAL] = { GPIO_NUM_19,GPIO_NUM_18,GPIO_NUM_2,GPIO_NUM_15,GPIO_NUM_13,GPIO_NUM_14 };

struct WiFiSettings wifiSets;
struct CommSettings commSets;
struct BattSettings battSets;
struct DispSettings dispSets;

char spb[1024];

uint32_t statusMS=0,connectMS=0,shuntPollMS=0,pvPollMS=0,analogPollMS=0,lastSync=0;
bool sendEmail = false,inAlertState = true;
AsyncWebServer server(80);
SMTPData smtpData;
uint8_t previousRelayState[RELAY_TOTAL];
String emailRes = "";

int milliRolls;

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

bool readEE(uint8_t *p,size_t s,uint32_t start) {
  EEPROM.readBytes(start,p,s);
  uint8_t checksum = CRC8(p, s);
  uint8_t ck = EEPROM.read(start+s);
  return checksum == ck;
}

void writeEE(uint8_t *p,size_t s,uint32_t start) {
  uint8_t crc = CRC8(p, s);
  EEPROM.writeBytes(start,p,s);
  EEPROM.write(start+s,crc);
  EEPROM.commit();
}

void trimLastEventMsg() {
  lastEventMsgCnt++;
  if (lastEventMsgCnt > LAST_EVT_MSG_CNT) {
    char* ptr = strchr(lastEventMsg,'|');
    if (ptr)
      snprintf(lastEventMsg,sizeof(lastEventMsg),"%s",ptr+2);
    lastEventMsgCnt = LAST_EVT_MSG_CNT;
  }
}

void InitOTA() {
    // Port defaults to 3232
  // ArduinoOTA.setPort(3232);

  // Hostname defaults to esp3232-[MAC]
  // ArduinoOTA.setHostname("myesp32");

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";
      OTAInProg = true;
      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();
}

void initRelays() {
  for (int i=0;i<RELAY_TOTAL;i++) {
    digitalWrite(relayPins[i], LOW);
    previousRelayState[i] = LOW;
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

void doCommSettings() {
  smtpData.setLogin(commSets.senderServer, commSets.senderPort, commSets.senderEmail, commSets.senderPW);
  smtpData.setSender("Your Battery", commSets.senderEmail);
  smtpData.setPriority("High");
  smtpData.setSubject(commSets.senderSubject);
  smtpData.addRecipient(commSets.email);
  emailSetup = true;
/*  smtpData.setLogin("smtp.gmail.com", 465, "john.taves@gmail.com","erwsmuigvggpvmtf");
*/
}

void sendSuccess(AsyncWebServerRequest *request,const char* mess=NULL,bool suc=true) { 
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(100);
  doc["success"] = suc;
  doc["errmess"] = mess;
  serializeJson(doc, *response);
  request->send(response);
}

int toCel(String val) {
  if (dispSets.doCelsius)
    return val.toInt();
  return (val.toInt() - 32) * 5/9;
}

int fromCel(int c) {
  if (dispSets.doCelsius)
    return c;
  return c*9/5+32;
}

void settings(AsyncWebServerRequest *request){
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  root["cmd"] = battSets.cmd;
  root["apName"] = wifiSets.apName;
  root["ssid"] = wifiSets.ssid;
  root["email"] = commSets.email;
  root["senderEmail"] = commSets.senderEmail;
  root["senderSubject"] = commSets.senderSubject;
  root["senderServer"] = commSets.senderServer;
  root["senderSubject"] = commSets.senderSubject;
  root["senderPort"] = commSets.senderPort;
  root["logEmail"] = commSets.logEmail;
  root["doLogging"] = commSets.doLogging;
 
  root["Avg"] = battSets.Avg;
  root["PollFreq"] = battSets.PollFreq;
  root["ConvTime"] = battSets.ConvTime;
  root["BattAH"] = battSets.BattAH;
  root["TopAmps"] = battSets.TopAmps;
  root["socLastAdj"] = lastAdjMillAmpHrs;
  snprintf(spb,sizeof(spb),"%d:%d",(int)(aveAdjMilliAmpMillis / ((int64_t)1000 * 60 * 60)),adjCnt);
  root["socAvgAdj"] = spb;
  root["BatAHMeasured"] = BatAHMeasured > 0 ? String(BatAHMeasured) : String("N/A");

  root["MaxAmps"] = battSets.MaxAmps;
  root["ShuntUOhms"] = battSets.ShuntUOhms;
  root["PVMaxAmps"] = battSets.PVMaxAmps;
  root["PVShuntUOhms"] = battSets.PVShuntUOhms;
  root["nCells"] = battSets.nCells;
  root["useCellC"]=battSets.useCellC;
  root["useTemp1"]=battSets.useTemp1;
  root["ChargePct"]=battSets.ChargePct;
  root["ChargePctRec"]=battSets.ChargePctRec;
  root["FloatV"]=battSets.FloatV;
  root["ChargeRate"]=battSets.ChargeRate;
  root["CellsOutMin"]=battSets.CellsOutMin;
  root["CellsOutMax"]=battSets.CellsOutMax;
  root["CellsOutTime"]=battSets.CellsOutTime;

  root["cellCnt"] = battSets.sets.cnt;
  root["cellDelay"] = battSets.sets.delay;
  root["cellTime"] = battSets.sets.time;

  JsonObject obj = root.createNestedObject("limitSettings");
  for (int l0=0;l0<limits::Max0;l0++) {
    for (int l1=0;l1<limits::Max1;l1++) {
      for (int l2=0;l2<limits::Max2;l2++) {
        for (int l3=0;l3<limits::Max3;l3++) {
          char name[5];
          sprintf(name,"%d%d%d%d",l0,l1,l2,l3);
          if (l0 == limits::Temp)
            obj[name] = fromCel(battSets.limits[l0][l1][l2][l3]);
          else obj[name] = battSets.limits[l0][l1][l2][l3];
        }
      }
    }
  }

  JsonArray rsArray = root.createNestedArray("relaySettings");
  for (uint8_t r = 0; r < MAINRELAY_TOTAL; r++) {
    JsonObject rule1 = rsArray.createNestedObject();
    RelaySettings *rp = &battSets.relays[r];
    rule1["name"] = rp->name;
    rule1["from"] = rp->from;
    switch (rp->type) {
      default: case Relay_Connect: rule1["type"] = "E"; break;
      case Relay_Load: rule1["type"] = (rp->doSoC?"LP":"L"); break;
      case Relay_Charge: rule1["type"] = (rp->doSoC?"CP":(rp->fullChg?"CF":"C")); break;
    }
    
    rule1["trip"] =rp->trip;
    rule1["rec"] =rp->rec;
    rule1["rank"] =rp->rank;
  }

  serializeJson(doc, *response);
  request->send(response);
}

void saveOff(AsyncWebServerRequest *request) {
  if (request->hasParam("relay", true)) {
    msg.cmd = SetRelayOff;
    msg.val = request->getParam("relay", true)->value().toInt();
    dataSer.send((byte*)&msg,sizeof(msg));

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    doc["relay"] = r;
    doc["val"] = rp->off ? "off" : "on";
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);

  } else request->send(500, "text/plain", "Missing parameters");
}

void clrMaxDiff(AsyncWebServerRequest *request) {
  uint8_t cmd =   ClrMaxDiff;
  dataSer.send((byte*)&cmd,sizeof(cmd));
  sendSuccess(request);
}

void fullChg(AsyncWebServerRequest *request) {
  uint8_t cmd = FullChg;
  dataSer.send((byte*)&cmd,sizeof(cmd));
  sendSuccess(request);
}

void dump(AsyncWebServerRequest *request) {
  if (request->hasParam("cell", true)) {
    uint32_t h = request->getParam("hrs", true)->value().toInt();
    uint32_t m = request->getParam("min", true)->value().toInt();
    DumpMsg dm;
    dm.cmd = DumpCell;
    dm.cell = request->getParam("cell", true)->value().toInt();
    dm.secs = ((h*60)+m) * 60;
    dataSer.send((byte*)&dm,sizeof(dm));
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);

  } else request->send(500, "text/plain", "Missing parameters");
}

void fillStatusDoc(JsonVariant root) {
  
  uint32_t upsecs = (milliRolls * 4294967ul) + (millis()/1000ul);
  int days = upsecs / (24ul*60*60);
  upsecs = upsecs % (24ul*60*60);
  int hrs = upsecs / (60*60);
  upsecs = upsecs % (60*60);
  int min = upsecs / 60;

  snprintf(spb,sizeof(spb),"%d:%02d:%02d:%02d",days,hrs,min,upsecs % 60);
  root["uptime"] = spb;
  root["now"]=time(nullptr);
  root["version"] = "V: 0.6";
  root["debugstr"] = debugstr;
  root["lastEventMsg"] = lastEventMsg;
  root["watchDogHits"] = st.watchDogHits;
  root["RELAY_TOTAL"] = RELAY_TOTAL;
  for (int i=0;i<RELAY_TOTAL;i++) {
    char dodad[16];
    if (strlen(battSets.relays[i].name) == 0)
      continue;
    sprintf(dodad,"relayStatus%d",i);
    root[dodad] = st.previousRelayState[i]==HIGH?"ON":"OFF";
    sprintf(dodad,"relayName%d",i);
    root[dodad] = battSets.relays[i].name;
    sprintf(dodad,"relayOff%d",i);
    root[dodad] = battSets.relays[i].off ? "off" : "on";
  }

  root["packcurrent"] = st.lastMicroAmps/1000;
  root["packvolts"] = st.lastPackMilliVolts;
  root["maxdiffvolts"] = st.maxDiffMilliVolts;
  root["pvcurrent"] = st.lastPVMicroAmps/1000;
  snprintf(spb,sizeof(spb),"%d%%",st.stateOfCharge);
  root["soc"] = spb;
  root["socvalid"] = st.stateOfChargeValid;
  root["temp1"] = fromCel(st.curTemp1);
  root["fullChg"] = st.doFullChg;

  root["maxCellVState"] = st.maxCellVState;
  root["minCellVState"] = st.minCellVState;
  root["maxPackVState"] = st.maxPackVState;
  root["minPackVState"] = st.minPackVState;
  root["maxCellCState"] = st.maxCellCState;
  root["minCellCState"] = st.minCellCState;
  root["maxPackCState"] = st.maxPackCState;
  root["minPackCState"] = st.minPackCState;

  root["nCells"] = battSets.nCells;

  JsonArray data = root.createNestedArray("cells");
  for (uint8_t i = 0; i < battSets.nCells; i++) {
    JsonObject cell = data.createNestedObject();
    cell["c"] = i;
    cell["v"] = st.cellVolts[i];
    cell["t"] = fromCel(st.cellTemps[i]);
    cell["d"] = st.cellDumping[i];
    cell["l"] = !st.cellConn[i];
  }
}

void status(AsyncWebServerRequest *request){
//Serial.printf("SP: %d, C: %d H: %d\n",uxTaskPriorityGet(NULL),xPortGetCoreID(),uxTaskGetStackHighWaterMark(NULL));
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");

  DynamicJsonDocument doc(4096);
  fillStatusDoc(doc.to<JsonVariant>());
  serializeJson(doc, *response);
  request->send(response);
}

void saverules(AsyncWebServerRequest *request) {
  int16_t curCellVMax = MAX_CELLV;
  for (int l0=0;l0<limits::Max0;l0++) {
    for (int l1=0;l1<limits::Max1;l1++) {
      for (int l2=0;l2<limits::Max2;l2++) {
        for (int l3=0;l3<limits::Max3;l3++) {
          char name[5];
          sprintf(name,"%d%d%d%d",l0,l1,l2,l3);
          if (request->hasParam(name, true, false)) {
            if (l0 == limits::Temp)
              battSets.limits[l0][l1][l2][l3] = toCel(request->getParam(name, true, false)->value());
            else battSets.limits[l0][l1][l2][l3] = request->getParam(name, true, false)->value().toInt();
          }
        }
      }
    }
  }

  for (int relay=0;relay<MAINRELAY_TOTAL;relay++) {
    char name[16],type[3];
    RelaySettings *rp = &battSets.relays[relay];
    sprintf(name,"relayName%d",relay);
    if (request->hasParam(name, true))
      request->getParam(name, true)->value().toCharArray(rp->name,sizeof(rp->name));;
    sprintf(name,"relayFrom%d",relay);
    if (request->hasParam(name, true))
      request->getParam(name, true)->value().toCharArray(rp->from,sizeof(rp->from));;
    
    sprintf(name,"relayType%d",relay);
    if (request->hasParam(name, true)) {
      request->getParam(name, true)->value().toCharArray(type,sizeof(type));
      switch (type[0]) {
        default: case 'E':rp->type = Relay_Connect;break;
        case 'L':rp->type = Relay_Load;break;
        case 'C':rp->type = Relay_Charge; break;
      }
      rp->doSoC = type[1] == 'P';
      rp->fullChg = type[1] == 'F';
    }

    sprintf(name,"relayTrip%d",relay);
    if (request->hasParam(name, true))
      rp->trip = request->getParam(name, true)->value().toInt();

    sprintf(name,"relayRec%d",relay);
    if (request->hasParam(name, true))
      rp->rec = request->getParam(name, true)->value().toInt();

  }
  battSets.useTemp1 = request->hasParam("useTemp1", true) && request->getParam("useTemp1", true)->value().equals("on");
  battSets.useCellC = request->hasParam("useCellC", true) && request->getParam("useCellC", true)->value().equals("on");

  if (request->hasParam("ChargePct", true))
    battSets.ChargePct = request->getParam("ChargePct", true)->value().toInt();
  if (request->hasParam("ChargePctRec", true))
    battSets.ChargePctRec = request->getParam("ChargePctRec", true)->value().toInt();
  if (request->hasParam("FloatV", true))
    battSets.FloatV = request->getParam("FloatV", true)->value().toInt();
  if (request->hasParam("ChargeRate", true))
    battSets.ChargeRate = request->getParam("ChargeRate", true)->value().toInt();
  if (request->hasParam("CellsOutMin", true))
    battSets.CellsOutMin = request->getParam("CellsOutMin", true)->value().toInt();
  if (request->hasParam("CellsOutMax", true))
    battSets.CellsOutMax = request->getParam("CellsOutMax", true)->value().toInt();
  if (request->hasParam("CellsOutTime", true))
    battSets.CellsOutTime = request->getParam("CellsOutTime", true)->value().toInt();

  if (!battSets.useCellC) {
    maxCellCState = false;
    minCellCState = false;
  }
  if (!battSets.useTemp1) {
    maxPackCState = false;
    minPackCState = false;
  }
  dataSer.send((byte*)&battSets,sizeof(battSets));

  sendSuccess(request);
}

void saveemail(AsyncWebServerRequest *request){
  if (request->hasParam("email", true))
    request->getParam("email", true)->value().toCharArray(commSets.email,sizeof(commSets.email));
  if (request->hasParam("senderEmail", true))
    request->getParam("senderEmail", true)->value().toCharArray(commSets.senderEmail,sizeof(commSets.senderEmail));
  if (request->hasParam("senderPW", true))
    request->getParam("senderPW", true)->value().toCharArray(commSets.senderPW,sizeof(commSets.senderPW));
  if (request->hasParam("senderServer", true))
    request->getParam("senderServer", true)->value().toCharArray(commSets.senderServer,sizeof(commSets.senderServer));
  if (request->hasParam("senderSubject", true))
    request->getParam("senderSubject", true)->value().toCharArray(commSets.senderSubject,sizeof(commSets.senderSubject));
  if (request->hasParam("senderPort", true))
    commSets.senderPort = request->getParam("senderPort", true)->value().toInt();
  if (request->hasParam("logEmail", true))
    request->getParam("logEmail", true)->value().toCharArray(commSets.logEmail,sizeof(commSets.logEmail));
  if (request->hasParam("logPW", true))
    request->getParam("logPW", true)->value().toCharArray(commSets.logPW,sizeof(commSets.logPW));
  commSets.doLogging = request->hasParam("doLogging", true) && request->getParam("doLogging", true)->value().equals("on");

  writeCommSet = true;
  doCommSettings();
  sendSuccess(request);
}

void onconnect(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.println(WiFi.localIP());
}

void WiFiInit() {
  WiFi.persistent(false);
  WiFi.disconnect(true,true);
  if (strlen(wifiSets.apName)) {
    Serial.println(wifiSets.apName);
    WiFi.setHostname(wifiSets.apName);
    WiFi.softAP(wifiSets.apName,wifiSets.apPW,1,1);
  } else WiFi.softAP("ADVBMS_CONTROLLER");
  if (wifiSets.ssid[0] != 0)
    WiFi.begin(wifiSets.ssid,wifiSets.password);
}

void savewifi(AsyncWebServerRequest *request){
  if (request->hasParam("apName", true))
    request->getParam("apName", true)->value().toCharArray(wifiSets.apName,sizeof(wifiSets.apName));
  if (request->hasParam("apPW", true))
    request->getParam("apPW", true)->value().toCharArray(wifiSets.apPW,sizeof(wifiSets.apPW));
  if (strlen(wifiSets.apPW) && strlen(wifiSets.apPW) < 8) {
    sendSuccess(request,"apPW too short, 8 chars or none",false);
    return;
  }

  if (request->hasParam("ssid", true))
    request->getParam("ssid", true)->value().toCharArray(wifiSets.ssid,sizeof(wifiSets.ssid));
  if (request->hasParam("password", true))
    request->getParam("password", true)->value().toCharArray(wifiSets.password,sizeof(wifiSets.password));
  writeWifiSet = true;
  sendSuccess(request);
}

void savecellset(AsyncWebServerRequest *request) {
  if (request->hasParam("cellCnt", true)) {
    AsyncWebParameter *p1 = request->getParam("cellCnt", true);
    msg.cmd = SetCellCnt;
    msg.val = p1->value().toInt();
    dataSer.send((byte*)&msg,sizeof(msg));
  }
  if (request->hasParam("cellDelay", true)) {
    AsyncWebParameter *p1 = request->getParam("cellDelay", true);
    msg.cmd = SetCellDelay;
    msg.val = p1->value().toInt();
    dataSer.send((byte*)&msg,sizeof(msg));
  }
  if (request->hasParam("cellTime", true)) {
    AsyncWebParameter *p1 = request->getParam("cellTime", true);
    msg.cmd = SetCellTime;
    msg.val = p1->value().toInt();
    dataSer.send((byte*)&msg,sizeof(msg));
  }
  sendSuccess(request);
}

void savecapacity(AsyncWebServerRequest *request) {
  SettingMsg msg;
  if (request->hasParam("CurSOC", true)) {
    AsyncWebParameter *p1 = request->getParam("CurSOC", true);
    msg.cmd = SetCurSOC;
    msg.val = p1->value().toInt();
    dataSer.send((byte*)&msg,sizeof(msg));
  }
  if (request->hasParam("PollFreq", true)) {
    AsyncWebParameter *p1 = request->getParam("PollFreq", true);
    msg.cmd = SetPollFreq;
    msg.val = p1->value().toInt();
    dataSer.send((byte*)&msg,sizeof(msg));
  }
  if (request->hasParam("Avg", true)) {
    AsyncWebParameter *p1 = request->getParam("Avg", true);
    msg.cmd = SetAvg;
    msg.val = p1->value().toInt();
    dataSer.send((byte*)&msg,sizeof(msg));
  }
  if (request->hasParam("ConvTime", true)) {
    AsyncWebParameter *p1 = request->getParam("ConvTime", true);
    msg.cmd = SetConvTime;
    msg.val = p1->value().toInt();
    dataSer.send((byte*)&msg,sizeof(msg));
  }
  if (request->hasParam("BattAH", true)) {
    AsyncWebParameter *p1 = request->getParam("BattAH", true);
    msg.cmd = SetBattAH;
    msg.val = p1->value().toInt();
    dataSer.send((byte*)&msg,sizeof(msg));
  }
  if (request->hasParam("MaxAmps", true)) {
    AsyncWebParameter *p1 = request->getParam("MaxAmps", true);
    msg.cmd = SetMaxAmps;
    msg.val = p1->value().toInt();
    dataSer.send((byte*)&msg,sizeof(msg));
  }
  if (request->hasParam("ShuntUOhms", true)) {
    AsyncWebParameter *p1 = request->getParam("ShuntUOhms", true);
    msg.cmd = SetShuntUOhms;
    msg.val = p1->value().toInt();
    dataSer.send((byte*)&msg,sizeof(msg));
  }
  if (request->hasParam("PVMaxAmps", true)) {
    AsyncWebParameter *p1 = request->getParam("PVMaxAmps", true);
    msg.cmd = SetPVMaxAmps;
    msg.val = p1->value().toInt();
    dataSer.send((byte*)&msg,sizeof(msg));
  }
  if (request->hasParam("PVShuntUOhms", true)) {
    AsyncWebParameter *p1 = request->getParam("PVShuntUOhms", true);
    msg.cmd = SetPVShuntUOhms;
    msg.val = p1->value().toInt();
    dataSer.send((byte*)&msg,sizeof(msg));
  }
  if (request->hasParam("nCells", true)) {
    AsyncWebParameter *p1 = request->getParam("nCells", true);
    msg.cmd = SetNCells;
    msg.val = p1->value().toInt();
    dataSer.send((byte*)&msg,sizeof(msg));
  }
  sendSuccess(request);
}

void hideLastEventMsg(AsyncWebServerRequest *request) {
  lastEventMsg[0] = 0;
  lastEventMsgCnt = 0;
  sendSuccess(request);
}

void toggleTemp(AsyncWebServerRequest *request) {
  dispSets.doCelsius = !dispSets.doCelsius;
  writeDispSet = true;
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(100);
  doc["val"] = dispSets.doCelsius;
  serializeJson(doc, *response);
  request->send(response);
}

void startServer() {
  server.on("/email", HTTP_POST, [](AsyncWebServerRequest *request){
    sendEmail = true;
    smtpData.setMessage("This is a test", true);
    request->send(200, "text/plain", "OK Gonna send it");
  });

  server.on("/toggleTemp", HTTP_GET, toggleTemp);
  server.on("/hideLastEventMsg", HTTP_GET, hideLastEventMsg);
  server.on("/saveemail", HTTP_POST, saveemail);
  server.on("/saveOff", HTTP_POST, saveOff);
  server.on("/fullChg", HTTP_POST, fullChg);
  server.on("/clrMaxDiff", HTTP_GET, clrMaxDiff);
  server.on("/dump", HTTP_POST, dump);
  server.on("/savewifi", HTTP_POST, savewifi);
  server.on("/savecapacity", HTTP_POST, savecapacity);
  server.on("/savecellset", HTTP_POST, savecellset);
  server.on("/saverules", HTTP_POST, saverules);
  server.on("/settings", HTTP_GET, settings);
  server.on("/status", HTTP_GET, status);

  server.serveStatic("/static", SPIFFS, "/static").setLastModified("Mon, 20 Jun 2016 14:00:00 GMT");
  server.serveStatic("/", SPIFFS, "/");
  server.onNotFound(onRequest);
  server.begin();
}

void sendStatus() {
//Serial.printf("XP: %d, C: %d H: %d\n",uxTaskPriorityGet(NULL),xPortGetCoreID(),uxTaskGetStackHighWaterMark(NULL));
  http.begin("http://advbms.com/PostData.aspx");
  http.addHeader("Content-Type", "application/json");
  DynamicJsonDocument doc(4096);
  fillStatusDoc(doc.to<JsonVariant>());
  doc["userid"] = 1;
  String json;
  serializeJson(doc, json);
//Serial.println(json);
  http.POST(json);
  String res = http.getString();
  if (res != "OK")
    Serial.println(res);
  http.end();
  taskRunning.clear();
}

void onSerData(const uint8_t *receivebuffer, size_t len)
{
  digitalWrite(GREEN_LED,1);
  switch(*receivebuffer) {
    case BattSets:
      battSets = *(BattSettings*)receivebuffer;
      break;
    case DebugStr:
      snprintf(debugstr,sizeof(debugstr),"%s",((StrMsg*)receivebuffer)->msg);
      break;
  }
  digitalWrite(GREEN_LED,0);
}

void xSendStatus(void* unused) {
  sendStatus();
  vTaskDelete(NULL);
}

void setup() {
  pinMode(GREEN_LED, OUTPUT);
  digitalWrite(GREEN_LED,1);
  Serial.begin(9600);
  EEPROM.begin(EEPROMSize);
  Wire.begin();
  if(!SPIFFS.begin()){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  battSets.cmd = Panic;
  if (!readEE((uint8_t*)&wifiSets, sizeof(wifiSets), EEPROM_WIFI)) {
    wifiSets.ssid[0] = 0;
    wifiSets.password[0] = 0;
    wifiSets.apName[0] = 0;
    wifiSets.apPW[0] = 0;
  }
  WiFi.onEvent(onconnect, WiFiEvent_t::SYSTEM_EVENT_STA_GOT_IP);
  WiFiInit();
  Serial2.begin(CPUBAUD);
  dataSer.setStream(&Serial2); // start serial for output
  dataSer.setPacketHandler(&onSerData);

  if (!readEE((uint8_t*)&commSets,sizeof(commSets),EEPROM_COMM)) {
    commSets.email[0] = 0;
    commSets.senderEmail[0] = 0;
    commSets.senderServer[0] = 0;
    commSets.senderPort = 587;
    commSets.senderPW[0] = 0;
    commSets.senderSubject[0] = 0;
    commSets.logEmail[0] = 0;
    commSets.logPW[0] = 0;
    commSets.doLogging = false;
  } else doCommSettings();

  if (!readEE((uint8_t*)&dispSets,sizeof(dispSets),EEPROM_DISP))
    dispSets.doCelsius = true;

  for (int i=0;i<RELAY_TOTAL;i++)
    pinMode(relayPins[i],OUTPUT);
  initRelays();
  GenUUID();

  smtpData.setSendCallback(emailCallback);
  startServer();
  configTime(0,0,"pool.ntp.org");
  InitOTA();
  digitalWrite(GREEN_LED,0);
}

time_t saveTimeDiff = 0;
void loop() {
  if (lastMillis > millis()) milliRolls++;
  lastMillis = millis(); // for uptime to continue after 50 days

  if (writeCommSet) {
    writeEE((uint8_t*)&commSets,sizeof(commSets),EEPROM_COMM);
    writeCommSet = false;
  } else if (writeWifiSet) {
    writeEE((uint8_t*)&wifiSets,sizeof(wifiSets),EEPROM_WIFI);
    WiFiInit();
    writeWifiSet = false;
  } else if (writeDispSet) {
    writeEE((uint8_t*)&dispSets,sizeof(dispSets),EEPROM_DISP);
    writeDispSet = false;
  }

  dataSer.update();

  if (sendEmail && emailSetup && strlen(commSets.senderServer)) {
    if (!MailClient.sendMail(smtpData))
      Serial.println("Error sending Email, " + MailClient.smtpErrorReason());
    sendEmail = false;
  }
  ArduinoOTA.handle(); // this does nothing until it is initialized
}
