#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <ESP32_MailClient.h>
#include <Wire.h>

#include <time.h>
#include <Ticker.h>
#include <BMSCommArd.h>
#include "defines.h"

// improve server
// adjustable cell history
// cell dump safety, stop if cell gets low, show dump status
// charge control feature to reduce amps
// improve the SoC allow to run logic. Make the code bad, and see if the charge/loads go off
// email did not work going from unset to sent. A reboot seemed to help

char debugstr[200],lastEventMsg[1024];
int8_t lastEventMsgCnt=0;
bool emailSetup=false,writeCommSet=false,writeWifiSet=false,writeDispSet=false,writeRelaySet=false;
uint16_t statusMS=0;
HTTPClient http;
atomic_flag taskRunning(0);
bool OTAInProg = false;

BMSStatus st;

const int relayPins[W_RELAY_TOTAL] = { GPIO_NUM_19,GPIO_NUM_18,GPIO_NUM_2,GPIO_NUM_15,GPIO_NUM_13,GPIO_NUM_14 };

WiFiSettings wifiSets;
CommSettings commSets;
DynSetts dynSets;
StatSetts statSets;
DispSettings dispSets;
WRelaySettings relSets;

char spb[1024];

bool sendEmail = false,inAlertState = true;
AsyncWebServer server(80);
SMTPData smtpData;
uint8_t previousRelayState[W_RELAY_TOTAL];
String emailRes = "";

uint8_t milliRolls=0;
uint32_t lastMillis=0;

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
  Serial.println("Here");
}

void clearRelays() {
  for (int i=0;i<W_RELAY_TOTAL;i++) {
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

void cells(AsyncWebServerRequest *request){
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  root["notRecd"] = false;

  serializeJson(doc, *response);
  request->send(response);
}

void net(AsyncWebServerRequest *request){
  AsyncResponseStream *response =
    request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  root["notRecd"] = false;
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
  serializeJson(doc, *response);
  request->send(response);
}

void limits(AsyncWebServerRequest *request){  
  AMsg msg;
  msg.cmd = StatQuery;
  bool notRecd = BMSWaitFor(&msg,StatSets);
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  root["notRecd"] = notRecd;
  root["useCellC"]=statSets.useCellC;
  root["useTemp1"]=statSets.useTemp1;
  root["bdVolts"]=statSets.bdVolts;
  root["ChargePct"]=statSets.ChargePct;
  root["ChargePctRec"]=statSets.ChargePctRec;
  root["FloatV"]=statSets.FloatV;
  root["ChargeRate"]=statSets.ChargeRate;
  root["CellsOutMin"]=statSets.CellsOutMin;
  root["CellsOutMax"]=statSets.CellsOutMax;
  root["CellsOutTime"]=statSets.CellsOutTime;
  JsonObject obj = root.createNestedObject("limitSettings");
  for (int l0=0;l0<LimitConsts::Max0;l0++) {
    for (int l1=0;l1<LimitConsts::Max1;l1++) {
      for (int l2=0;l2<LimitConsts::Max2;l2++) {
        for (int l3=0;l3<LimitConsts::Max3;l3++) {
          char name[5];
          sprintf(name,"%d%d%d%d",l0,l1,l2,l3);
          if (l0 == LimitConsts::Temp)
            obj[name] = fromCel(statSets.limits[l0][l1][l2][l3]);
          else obj[name] = statSets.limits[l0][l1][l2][l3];
        }
      }
    }
  }

  JsonArray rsArray = root.createNestedArray("relaySettings");
  for (uint8_t r = 0; r < RELAY_TOTAL; r++) {
    JsonObject rule1 = rsArray.createNestedObject();
    RelaySettings *rp;
    if (r < C_RELAY_TOTAL) rp = &statSets.relays[r];
    else rp = &relSets.relays[r - C_RELAY_TOTAL];
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

void batt(AsyncWebServerRequest *request){
  AMsg msg;
  msg.cmd = DynQuery;
  bool notRecd = BMSWaitFor(&msg,DynSets);
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  root["notRecd"] = notRecd;

  root["Avg"] = dynSets.Avg;
  root["PollFreq"] = dynSets.PollFreq;
  root["ConvTime"] = dynSets.ConvTime;
  root["BattAH"] = dynSets.BattAH;
  root["TopAmps"] = dynSets.TopAmps;

  root["socLastAdj"] = st.lastAdjMillAmpHrs;
  snprintf(spb,sizeof(spb),"%d:%d",(int)(st.aveAdjMilliAmpMillis / ((int64_t)1000 * 60 * 60)),st.adjCnt);
  root["socAvgAdj"] = spb;
  root["BatAHMeasured"] = st.BatAHMeasured > 0 ? String(st.BatAHMeasured) : String("N/A");

  root["MaxAmps"] = dynSets.MaxAmps;
  root["ShuntUOhms"] = dynSets.ShuntUOhms;
  root["PVMaxAmps"] = dynSets.PVMaxAmps;
  root["PVShuntUOhms"] = dynSets.PVShuntUOhms;
  root["nCells"] = dynSets.nCells;

  root["cellCnt"] = dynSets.cellSets.cnt;
  root["cellDelay"] = dynSets.cellSets.delay;
  root["resPwrOn"] = dynSets.cellSets.resPwrOn;
  root["cellTime"] = dynSets.cellSets.time;

  serializeJson(doc, *response);
  request->send(response);
}

void saveOff(AsyncWebServerRequest *request) {
  SettingMsg msg;
  if (request->hasParam("relay", true)) {
    RelaySettings *rp;
    int r = request->getParam("relay", true)->value().toInt();
    if (r < C_RELAY_TOTAL) {
      msg.cmd = SetRelayOff;
      msg.val = r;
      BMSSend(&msg);
      rp = &statSets.relays[r];
    } else {
      rp = &relSets.relays[r - C_RELAY_TOTAL];
      writeRelaySet = true;
    }
    rp->off = !rp->off;
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    doc["relay"] = msg.val;
    doc["val"] = rp->off ? "off" : "on";
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);

  } else request->send(500, "text/plain", "Missing parameters");
}

void clrMaxDiff(AsyncWebServerRequest *request) {
  AMsg msg;
  msg.cmd =   ClrMaxDiff;
  BMSSend(&msg);
  sendSuccess(request);
}

void fullChg(AsyncWebServerRequest *request) {
  AMsg msg;
  msg.cmd = FullChg;
  BMSSend(&msg);
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
    BMSSend(&dm);
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);

  } else request->send(500, "text/plain", "Missing parameters");
}
void forget(AsyncWebServerRequest *request) {
  if (request->hasParam("cell", true)) {
    SettingMsg dm;
    dm.cmd = ForgetCell;
    dm.val = request->getParam("cell", true)->value().toInt();
    BMSSend(&dm);
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);

  } else request->send(500, "text/plain", "Missing parameters");
}
void doMove(AsyncWebServerRequest *request) {
  if (request->hasParam("cell", true)) {
    SettingMsg dm;
    dm.cmd = MoveCell;
    dm.val = request->getParam("cell", true)->value().toInt();
    BMSSend(&dm);
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);
  } else request->send(500, "text/plain", "Missing parameters");
}
char *getUpTimeStr(uint32_t ms,uint8_t rolls) {
  uint32_t upsecs = (rolls * 4294967ul) + (ms/1000ul);
  int days = upsecs / (24ul*60*60);
  upsecs = upsecs % (24ul*60*60);
  int hrs = upsecs / (60*60);
  upsecs = upsecs % (60*60);
  int min = upsecs / 60;

  snprintf(spb,sizeof(spb),"%d:%02d:%02d:%02d",days,hrs,min,upsecs % 60);
  return spb;
}

void fillStatusDoc(JsonVariant root) {
  if (dynSets.cmd == Nada) {
    AMsg msg;
    msg.cmd = DynQuery;
    BMSWaitFor(&msg,DynSets);
  }
  root["uptimew"] = getUpTimeStr(millis(),milliRolls);
  root["uptimec"] = getUpTimeStr(st.lastMillis,st.milliRolls);
  root["now"]=time(nullptr);
  root["version"] = "V: 0.6";
  root["debugstr"] = debugstr;
  root["lastEventMsg"] = lastEventMsg;
  root["watchDogHits"] = st.watchDogHits;
  root["RELAY_TOTAL"] = RELAY_TOTAL;
  for (int i=0;i<RELAY_TOTAL;i++) {
    char dodad[16];
    int state;
    RelaySettings *rp;
    if (i < C_RELAY_TOTAL) {
      state = st.previousRelayState[i];
      rp = &statSets.relays[i];
    } else {
      state = previousRelayState[i - C_RELAY_TOTAL];
      rp = &relSets.relays[i - C_RELAY_TOTAL];
    }
    if (strlen(rp->name) == 0)
      continue;
    sprintf(dodad,"relayStatus%d",i);
    root[dodad] = state==HIGH?"ON":"OFF";
    sprintf(dodad,"relayName%d",i);
    root[dodad] = rp->name;
    sprintf(dodad,"relayOff%d",i);
    root[dodad] = rp->off ? "off" : "on";
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

  root["nCells"] = dynSets.nCells;

  JsonArray data = root.createNestedArray("cells");
  for (uint8_t i = 0; i < dynSets.nCells; i++) {
    JsonObject cell = data.createNestedObject();
    cell["c"] = i;
    cell["v"] = st.cells[i].volts;
    cell["t"] = fromCel(st.cells[i].exTemp);
    cell["bt"] = fromCel(st.cells[i].bdTemp);
    cell["d"] = st.cells[i].dumping;
    cell["l"] = !st.cells[i].conn;
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
  for (int l0=0;l0<LimitConsts::Max0;l0++) {
    for (int l1=0;l1<LimitConsts::Max1;l1++) {
      for (int l2=0;l2<LimitConsts::Max2;l2++) {
        for (int l3=0;l3<LimitConsts::Max3;l3++) {
          char name[5];
          sprintf(name,"%d%d%d%d",l0,l1,l2,l3);
          if (request->hasParam(name, true, false)) {
            if (l0 == LimitConsts::Temp)
              statSets.limits[l0][l1][l2][l3] = toCel(request->getParam(name, true, false)->value());
            else statSets.limits[l0][l1][l2][l3] = request->getParam(name, true, false)->value().toInt();
          }
        }
      }
    }
  }

  for (int relay=0;relay<RELAY_TOTAL;relay++) {
    char name[16],type[3];
    RelaySettings *rp;
    if (relay < C_RELAY_TOTAL)
      rp = &statSets.relays[relay];
    else rp = &relSets.relays[relay - C_RELAY_TOTAL];
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
  statSets.useTemp1 = request->hasParam("useTemp1", true) && request->getParam("useTemp1", true)->value().equals("on");
  statSets.useCellC = request->hasParam("useCellC", true) && request->getParam("useCellC", true)->value().equals("on");

  if (request->hasParam("ChargePct", true))
    statSets.ChargePct = request->getParam("ChargePct", true)->value().toInt();
  if (request->hasParam("bdVolts", true))
    statSets.bdVolts = request->getParam("bdVolts", true)->value().toInt();
  if (request->hasParam("ChargePctRec", true))
    statSets.ChargePctRec = request->getParam("ChargePctRec", true)->value().toInt();
  if (request->hasParam("FloatV", true))
    statSets.FloatV = request->getParam("FloatV", true)->value().toInt();
  if (request->hasParam("ChargeRate", true))
    statSets.ChargeRate = request->getParam("ChargeRate", true)->value().toInt();
  if (request->hasParam("CellsOutMin", true))
    statSets.CellsOutMin = request->getParam("CellsOutMin", true)->value().toInt();
  if (request->hasParam("CellsOutMax", true))
    statSets.CellsOutMax = request->getParam("CellsOutMax", true)->value().toInt();
  if (request->hasParam("CellsOutTime", true))
    statSets.CellsOutTime = request->getParam("CellsOutTime", true)->value().toInt();

  if (!statSets.useCellC) {
    st.maxCellCState = false;
    st.minCellCState = false;
  }
  if (!statSets.useTemp1) {
    st.maxPackCState = false;
    st.minPackCState = false;
  }
  writeRelaySet = true;
  BMSSend(&statSets);

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
  CellSetts msg;
  msg.cmd = SetCellSetts;
  msg.cellSets.cnt = request->getParam("cellCnt", true)->value().toInt();
  msg.cellSets.delay = request->getParam("cellDelay", true)->value().toInt();
  msg.cellSets.resPwrOn = request->hasParam("resPwrOn", true) && request->getParam("resPwrOn", true)->value().equals("on");
  msg.cellSets.time = request->getParam("cellTime", true)->value().toInt();
  BMSSend(&msg);
  sendSuccess(request);
}

void saveItem(AsyncWebServerRequest *request,const char* n,uint8_t cmd,uint16_t val) {
  SettingMsg msg;
  if (!request->hasParam(n, true)) return;
  AsyncWebParameter *p1 = request->getParam(n, true);
  if (p1->value().length() == 0)
    return;
  msg.cmd = cmd;
  msg.val = p1->value().toInt();
  if (msg.val != val)
    BMSSend(&msg);
}

void savecapacity(AsyncWebServerRequest *request) {
  saveItem(request,"CurSOC",SetCurSOC,101);
  saveItem(request,"PollFreq",SetPollFreq,dynSets.PollFreq);
  saveItem(request,"Avg",SetAvg,dynSets.Avg);
  saveItem(request,"ConvTime", SetConvTime,dynSets.ConvTime);
  saveItem(request,"BattAH",SetBattAH,dynSets.BattAH);
  saveItem(request,"TopAmps",SetTopAmps,dynSets.TopAmps);
  saveItem(request,"MaxAmps",SetMaxAmps,dynSets.MaxAmps);
  saveItem(request,"ShuntUOhms",SetShuntUOhms,dynSets.ShuntUOhms);
  saveItem(request,"PVMaxAmps",SetPVMaxAmps,dynSets.PVMaxAmps);
  saveItem(request,"PVShuntUOhms", SetPVShuntUOhms,dynSets.PVShuntUOhms);
  saveItem(request,"nCells",SetNCells,dynSets.nCells);
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
  server.on("/forget", HTTP_POST, forget);
  server.on("/move", HTTP_POST, doMove);
  server.on("/savewifi", HTTP_POST, savewifi);
  server.on("/savecapacity", HTTP_POST, savecapacity);
  server.on("/savecellset", HTTP_POST, savecellset);
  server.on("/saverules", HTTP_POST, saverules);
  server.on("/cells", HTTP_GET, cells);
  server.on("/limits", HTTP_GET, limits);
  server.on("/batt", HTTP_GET, batt);
  server.on("/net", HTTP_GET, net);
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

void checkStatus()
{
  statusMS = millis();
  uint8_t relay[W_RELAY_TOTAL];
  for (int8_t n = 0; n < W_RELAY_TOTAL; n++)
  {
    if (previousRelayState[n] != relay[n])
    {
      digitalWrite(relayPins[n], relay[n]);
      previousRelayState[n] = relay[n];
    }
  }
}

void MsgEvent(EventMsg *mp) {
  trimLastEventMsg();
  if (mp->cmd >= CellTopV && mp->cmd <= CellBotV)
    snprintf(lastEventMsg,sizeof(lastEventMsg),"%s #%d %dV,",lastEventMsg,mp->cell,mp->amps);
  else if (mp->cmd >= CellTopT && mp->cmd <= CellBotT)
    snprintf(lastEventMsg,sizeof(lastEventMsg),"%s #%d %dT,",lastEventMsg,mp->cell,mp->amps);
  else if (mp->cmd >= PackTopV && mp->cmd <= PackBotV)
    snprintf(lastEventMsg,sizeof(lastEventMsg),"%s %dV,",lastEventMsg,mp->amps);
  else if (mp->cmd >= PackTopV && mp->cmd <= PackBotV)
    snprintf(lastEventMsg,sizeof(lastEventMsg),"%s %dT,",lastEventMsg,mp->amps);
  else if (mp->cmd == CellsOverDue)
    snprintf(lastEventMsg,sizeof(lastEventMsg),"%s #%d %dX,",lastEventMsg,mp->cell,mp->amps);
  else if (mp->cmd == CellsDisc)
    snprintf(lastEventMsg,sizeof(lastEventMsg),"%s #%d %dD,",lastEventMsg,mp->cell,mp->amps);
}
void WonSerData(const AMsg *mp)
{
  if (mp->cmd > FirstEvent && mp->cmd < LastEvent)
    MsgEvent((EventMsg*)mp);
  else switch(mp->cmd) {
    case DiscCell:
    case ConnCell: 
      trimLastEventMsg();
      snprintf(lastEventMsg,sizeof(lastEventMsg),"%s %s%d,",lastEventMsg,mp->cmd == DiscCell?"D":"C"
          ,((SettingMsg*)mp)->val);
      break;
    case StatSets: statSets = *(StatSetts*)mp; break;
    case DynSets: dynSets = *(DynSetts*)mp; break;
    case DebugStr:
      snprintf(debugstr,sizeof(debugstr),"%s",((StrMsg*)mp)->msg);
      break;
    case Status:
      st = *(BMSStatus*)mp;
      checkStatus();
      break;  
  }
  digitalWrite(GREEN_LED,0);
}

void xSendStatus(void* unused) {
  sendStatus();
  vTaskDelete(NULL);
}

void setup() {
  Serial.begin(9600);
  pinMode(GREEN_LED, OUTPUT);
  digitalWrite(GREEN_LED,1);
  BMSInitCom(EEPROMSize,&WonSerData);
  Wire.begin();
  if(!SPIFFS.begin()){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  debugstr[0] = 0;
  dynSets.cmd = Nada;
  if (!readEE((uint8_t*)&wifiSets, sizeof(wifiSets), EEPROM_WIFI)) {
    wifiSets.ssid[0] = 0;
    wifiSets.password[0] = 0;
    wifiSets.apName[0] = 0;
    wifiSets.apPW[0] = 0;
  }
  WiFi.onEvent(onconnect, WiFiEvent_t::SYSTEM_EVENT_STA_GOT_IP);
  WiFiInit();
  InitOTA();

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

  if (!readEE((uint8_t*)&relSets,sizeof(relSets),EEPROM_RELAYS))
    InitRelays(&relSets.relays[0],W_RELAY_TOTAL);

  if (!readEE((uint8_t*)&dispSets,sizeof(dispSets),EEPROM_DISP))
    dispSets.doCelsius = true;

  for (int i=0;i<W_RELAY_TOTAL;i++)
    pinMode(relayPins[i],OUTPUT);
  clearRelays();
  GenUUID();

  smtpData.setSendCallback(emailCallback);
  startServer();
  configTime(0,0,"pool.ntp.org");
  digitalWrite(GREEN_LED,0);
}

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
  } else if (writeRelaySet) {
    writeEE((uint8_t*)&relSets,sizeof(relSets),EEPROM_RELAYS);
    writeRelaySet = false;
  }

  BMSGetSerial();

  if (sendEmail && emailSetup && strlen(commSets.senderServer)) {
    if (!MailClient.sendMail(smtpData))
      Serial.println("Error sending Email, " + MailClient.smtpErrorReason());
    sendEmail = false;
  }
  if ((millis() - statusMS) > CHECKSTATUS)
    checkStatus();

  ArduinoOTA.handle(); // this does nothing until it is initialized
}