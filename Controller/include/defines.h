#include <Arduino.h>
#include <INA.h>


#ifndef JTBMS_DEFINES_H_
#define JTBMS_DEFINES_H_

//Maximum of 16 cell modules (dont change this!)
#define maximum_cell_modules 16
#define maximum_bank_of_modules 4

#define COMMS_BAUD_RATE 2400
#define RELAY_ON 0xFF
#define RELAY_OFF 0x99
#define RELAY_X 0x00

#define RELAY_TOTAL 6

struct WiFiSettings {
  char ssid[33];
  char password[64];
  bool apMode;
};

enum {
  Relay_Load,Relay_Charge
};

struct limits {
  static const int N=4;
  enum { Volt, Temp, Max0 };
  enum { Cell, Pack, Max1 };
  enum { Max, Min, Max2 };
  enum { Trip, Rec, Max3 };
};

struct RelaySettings {
  char name[16];
  uint8_t type,doSOC,trip,rec,off;
};

struct EmailSettings {
  char email[256];
  char senderEmail[256];
  char senderPW[64];
  char senderServer[64];
  char senderSubject[64];
  uint16_t senderPort;
};

struct BattSettings {
  uint16_t limits[2][2][2][2];
  uint16_t BattAH,MaxAmps,PVMaxAmps;
  uint32_t ShuntUOhms,PVShuntUOhms;
  RelaySettings relays[RELAY_TOTAL];
  char errorEmail[128];
  bool doCelsius,useex;
  uint16_t Avg,ConvTime;
  uint32_t PollFreq;
  uint8_t nBanks,nCells;
};

#define MAX_CELLS 8

#define EEPROM_WIFI 0
#define EEPROM_EMAIL (EEPROM_WIFI+sizeof(WiFiSettings)+2)
#define EEPROM_BATT (EEPROM_EMAIL+sizeof(EmailSettings)+2)

typedef union {
  float number;
  uint8_t bytes[4];
  uint16_t word[2];
} FLOATUNION_t;

enum COMMAND : uint8_t
{
  SetBankIdentity = B00000000,
  ReadVoltageAndStatus = B00000001,
  Identify = B00000010,
  ReadTemperature = B00000011,
  ReadBadPacketCounter = B00000100,
  ReadSettings = B00000101,
  WriteSettings = B00000110,
  ReadBalancePowerPWM = B00000111

  // 0000 0000  = set bank identity
  // 0000 0001  = read voltage and status
  // 0000 0010  = identify module (flash leds)
  // 0000 0011  = Read temperature
  // 0000 0100  = Report number of bad packets
  // 0000 0101  = Report settings/configuration
  // 0000 0110  = Write settings/configuration
  // 0000 0111  = Read current level of PWM for power balance
};

//NOTE THIS MUST BE EVEN IN SIZE (BYTES) ESP8266 IS 32 BIT AND WILL ALIGN AS SUCH!
struct packet
{
  uint8_t address;
  uint8_t command;
  uint16_t sequence;
  uint16_t moduledata[maximum_cell_modules];
  uint16_t crc;
} __attribute__((packed));

struct CellModuleInfo
{
  //Used as part of the enquiry functions
  bool settingsCached;

  uint16_t voltagemV;
  uint16_t voltagemVMin;
  uint16_t voltagemVMax;
  //Signed integer - should these be byte?
  int8_t internalTemp;
  int8_t externalTemp;

  bool inBypass;
  bool bypassOverTemp;

  uint8_t BypassOverTempShutdown;
  uint16_t BypassThresholdmV;
  uint16_t badPacketCount;

  // Resistance of bypass load
  float LoadResistance;
  //Voltage Calibration
  float Calibration;
  //Reference voltage (millivolt) normally 2.00mV
  float mVPerADC;
  //Internal Thermistor settings
  uint16_t Internal_BCoefficient;
  //External Thermistor settings
  uint16_t External_BCoefficient;
  //Version number returned by code of module
  uint16_t BoardVersionNumber;
  //Value of PWM timer for load shedding
  uint16_t PWMValue;
};

//This holds all the cell information in a large array 2D array (4x16)
extern CellModuleInfo cmi[maximum_bank_of_modules][maximum_cell_modules];
#endif
