
#include <Preferences.h>
#include <string.h>
#include "BMSCommArd.h"
#include <PacketSerial.h>

#define CPUBAUD 115200

PacketSerial_<COBS, 0, sizeof(union MaxData)+10> dataSer;
Preferences pref;
void (*commCB)(const AMsg* buf);

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


bool readEE(const char* name,uint8_t *p,size_t s) {
  char buf[10];
  snprintf(buf,sizeof(buf),"%sC",name);
  if (!pref.getBytes(name,p,s))
    return false;
  uint8_t checksum = CRC8(p, s);
  uint8_t ck = pref.getUChar(buf);
  return checksum == ck;
}

void writeEE(const char* name,uint8_t *p,size_t s) {
  char buf[10];
  snprintf(buf,sizeof(buf),"%sC",name);
  uint8_t crc = CRC8(p, s);
  pref.putBytes(name,p,s);
  pref.putUChar(buf,crc);
}

void InitRelays(RelaySettings* rp,int num) {
  RelaySettings* r=rp;
  for (int i=0;i<num;i++,r++) {
      r->name[0] = 0;
      r->from[0] = 0;
      r->doSoC = false;
      r->off = true;
      r->fullChg = false;
      r->rec = 0;
      r->trip = 0;
      r->type = Relay_Unused;
  }
}

uint8_t txBuffer[sizeof(MaxData)]; // this is so that we do not mangle the data sent to BMSSendRaw

void BMSInitStatus(BMSStatus *sp) {
  sp->cmd = Status;
  sp->lastMillis = 0;
  sp->milliRolls = 0;
  sp->watchDogHits = 0;
  sp->lastPackMilliVolts = 0xffff;
  sp->lastPVMilliAmps=0;
  sp->lastMilliAmps=0;
  sp->doFullChg = true;
  sp->maxCellVState=false;sp->minCellVState=false;
  sp->maxPackVState=false;sp->minPackVState=false;
  sp->maxCellCState=false;sp->minCellCState=false;
  sp->maxPackCState=false;sp->minPackCState=false;
  sp->maxChargePctState=false;
  sp->lastAdjCoulomb = 0;
  sp->BatAHMeasured = 0;
  for (int i=0;i<C_RELAY_TOTAL;i++)
    sp->previousRelayState[i] = LOW;
  for (int i=0;i<MAX_CELLS;i++) {
    sp->cells[i].volts = 0;
    sp->cells[i].conn = false;
  }
}

void BMSSendRaw(uint8_t *d,uint16_t len) {
  if (!len) return;
  *d = CRC8(d+1,len-1);
/*  if (((AMsg*)d)->cmd != Status)
    printf("Sending: %d, %d, 0x%x\n",d[1],len,*d);*/
  dataSer.send((byte*)d,len);
}

void BMSSend(StrMsg* m) {
  BMSSendRaw((uint8_t*)m,sizeof(StrMsg) - sizeof(m->msg) + strlen(m->msg));
}

uint8_t lastCmd;

bool BMSWaitFor(AMsg* mp,uint8_t cmd) {
  uint32_t stMs = millis();
  BMSSend(mp);
  lastCmd = Panic;
  while (lastCmd != cmd && (millis() - stMs) < 100)
    dataSer.update();
  return lastCmd != cmd;
}

void onSerData(const uint8_t *d, size_t cnt)
{
  if (cnt && CRC8(&d[1],cnt-1) == d[0] && commCB) {
    AMsg* m = (AMsg*)d;
    commCB(m);
    lastCmd = m->cmd;
  }
}

void BMSInitCom(void (*func)(const AMsg* buf)) {

  commCB = func;
  Serial2.begin(CPUBAUD);
  dataSer.setStream(&Serial2); // start serial for output
  dataSer.setPacketHandler(&onSerData);
  pref.begin("ee");

}

void BMSGetSerial() {
  dataSer.update();
}
