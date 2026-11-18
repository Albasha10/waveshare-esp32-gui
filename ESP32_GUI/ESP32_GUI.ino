// ============================================================
//  ESP32 Dashboard  —  Waveshare ESP32-S3-POE-ETH-8DI-8DO
//  DO: PCF8574 I2C expander (active-LOW driver, addr 0x20)
//  DI: GPIO direct read (active-LOW opto, INPUT_PULLUP)
// ============================================================

#include <WiFi.h>
#include <ETH.h>
#include <SPI.h>
#include <Wire.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <PubSubClient.h>
#include <ModbusMaster.h>
#include <HardwareSerial.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "SD_MMC.h"
#include "FS.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// KPI signal source (defined up here: the Arduino IDE puts function prototypes above the first function)
struct KpiSrc { char src[40]; bool on; };
// efficiency formula parser state (also up here for the same reason)
struct EffCtx { const char* p; const char* err; double rt, dt; bool syntaxOnly; };
#include <UniversalTelegramBot.h>
void wsEventHandler(WStype_t type, uint8_t* payload, size_t length);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void resetMetricsByTarget(const String& target);

// ─── WebSocket bridge for cloud MQTT ─────────────────────────────
#define WSBRIDGE_RX_SIZE 1024
class WsClientBridge : public Client {
  WebSocketsClient& _ws;
  uint8_t  _buf[WSBRIDGE_RX_SIZE];
  uint32_t _head = 0, _tail = 0;    
  bool     _connected = false;
  uint32_t _avail() const { return _head - _tail; }
public:
  explicit WsClientBridge(WebSocketsClient& ws) : _ws(ws) {}
  void push(const uint8_t* d, size_t n) {
    for (size_t i=0; i<n && _avail()<WSBRIDGE_RX_SIZE-1; i++)
      _buf[(_head+i)%WSBRIDGE_RX_SIZE]=d[i];
    _head+=n;
  }
  void setConnected(bool c) { _connected=c; if(!c){_head=_tail=0;} }
  int connect(IPAddress,uint16_t)   override { return (int)_connected; }
  int connect(const char*,uint16_t) override { return (int)_connected; }
  size_t write(uint8_t b) override { return write(&b,1); }
  size_t write(const uint8_t* buf,size_t sz) override { _ws.sendBIN(buf,sz); return sz; }
  int available() override { if(!_avail()) _ws.loop(); return (int)_avail(); }
  int read() override {
    unsigned long t=millis();
    while(!_avail()){_ws.loop(); if(millis()-t>3000) return -1;}
    return _buf[_tail++%WSBRIDGE_RX_SIZE];
  }
  int read(uint8_t* buf,size_t sz) override { int n=0; while((size_t)n<sz){int b=read();if(b<0)break;buf[n++]=(uint8_t)b;} return n; }
  int peek() override { if(!_avail()) _ws.loop(); return _avail()?_buf[_tail%WSBRIDGE_RX_SIZE]:-1; }
  void flush() override {}
  void stop()  override { _ws.disconnect(); setConnected(false); }
  uint8_t connected() override { return _connected; }
  operator bool() override { return _connected; }
};

// ================= NETWORK ================
#include "secrets.h"   // copy secrets.example.h to secrets.h and fill in your Wi-Fi (secrets.h is not in git)
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;
bool eth_connected   = false;

// ================= ETHERNET (W5500 on the Waveshare ESP32-S3-POE-ETH-8DI-8DO) =================
// The W5500 is an SPI chip: ETH.begin() with no arguments does NOT start it on this board.
// Pins from the Waveshare wiki. Used for Modbus TCP to the PLC (and the web UI over Ethernet).
#define ETH_PIN_INT   12
#define ETH_PIN_MOSI  13
#define ETH_PIN_MISO  14
#define ETH_PIN_SCLK  15
#define ETH_PIN_CS    16
#define ETH_PIN_RST   39
struct EthConfig {
  bool dhcp      = false;              // PLC networks usually have no DHCP server -> fixed IP
  char ip[16]    = "192.168.250.10";   // this board on the PLC network (must be free)
  char mask[16]  = "255.255.255.0";
  char gw[16]    = "";                 // leave empty: internet stays on WiFi
} ethCfg;
bool eth_link = false;

// ---- TagoIO: the Modbus register values go straight to TagoIO over WiFi (HTTPS) ----
struct TagoConfig {
  bool enabled  = false;
  char token[64] = "";                         // TagoIO device token
  char url[96]   = "https://api.tago.io/data"; // other regions: e.g. https://api.us-e1.tago.io/data
} tagoCfg;
char     tagoStatus[64] = "off";
uint32_t tagoSentCount  = 0;
#define TAGO_MIN_GAP_MS   2000     // on/off changes: sent as soon as they happen, at most every 2 s
#define TAGO_ANALOG_MS    10000    // number values change constantly: at most every 10 s
#define TAGO_FULL_MS      300000   // everything every 5 min, even if unchanged
volatile bool restartPending = false;
unsigned long restartAtMs = 0;

// ================= DIGITAL IO =================
// DI: INPUT_PULLUP, active-LOW opto (read LOW = input ON)
// DO: PCF8574 at 0x20, active-LOW driver
//     outputState bit=1 → logical ON → write 0 to expander pin
const int inputPins[8] = {4,5,6,7,8,9,10,11};
uint8_t   outputState  = 0;          // logical: bit=1 means channel ON
#define   EXIO_ADDR  0x20

void writeOutputs() {
  Wire.beginTransmission(EXIO_ADDR);
  Wire.write(0x01);                    // TCA9554 Output Port register
  Wire.write(~outputState);            // invert for active-LOW driver
  byte i2cErr = Wire.endTransmission();
  Serial.printf("I2C write result: %d (0=OK, 2=no device found at address, other=error)\n", i2cErr);
}


// ================= DECLARATIONS NEEDED BY setOutput() =================
struct PinConfig {
  char     topic[50];
  uint32_t pubIntervalMs;   // 0 = on-change only, >0 = periodic heartbeat
};
PinConfig inputConfig[8];
PinConfig outputConfig[8];
char deviceName[30] = "esp32";

WiFiClient       tcpClient;
WiFiClientSecure mqttTlsClient;
WebSocketsClient wsClient;
WsClientBridge   wsBridge(wsClient);
PubSubClient     mqtt(tcpClient);

SemaphoreHandle_t ioMutex = NULL;

unsigned long doTriggerCount[8]  = {0};
unsigned long doTotalOnTimeMs[8] = {0};
unsigned long doLastOnStartMs[8] = {0};

void setOutput(int ch, bool state) {
  if (ch<0||ch>7) return;
  if (ioMutex) xSemaphoreTake(ioMutex, portMAX_DELAY);
  bool cur = (outputState>>ch)&1;
  if (cur != state) {
    unsigned long now = millis();
    if (state) {
      outputState |= (1<<ch);
      doTriggerCount[ch]++;
      doLastOnStartMs[ch] = now;
    } else {
      outputState &= ~(1<<ch);
      doTotalOnTimeMs[ch] += now - doLastOnStartMs[ch];
    }
    writeOutputs();
    // publish DO state change if topic configured
    if (strlen(outputConfig[ch].topic)>0 && mqtt.connected())
      mqtt.publish(outputConfig[ch].topic, state?"1":"0");
  }
  if (ioMutex) xSemaphoreGive(ioMutex);
}

// ================= CONFIG STRUCTS =================
struct MqttConfig {
  char mode[8];
  char proto[8];
  char host[100];
  int  port;
  char path[40];
} mqttCfg;

// ================= RS485 =================
struct RS485Config {
  int  baud              = 9600;
  char parity[8]         = "None";
  int  stopBits          = 1;
  int  dataBits          = 8;
  char addr[8]           = "1";
  int  pollIntervalMs    = 500;    // ms between register polls (user-configurable)
  int  publishInterval   = 1000;  // ms between MQTT publishes per register
  bool enabled           = false; // whether the polling task is actively running (off by default)
} rs485Config;

// ================= MODBUS TCP (alternate transport) =================
// Polls the SAME register list as RS485 (rs485Regs[]) but over Ethernet/WiFi
// instead of the physical RS485 pins. Only one transport should be enabled
// at a time -- handleToggleModbusTCP() enforces that.
struct ModbusTCPConfig {
  char ip[16]            = "192.168.250.7";  // PLC IP -- set to your unit
  int  port               = 502;
  int  unitId             = 1;
  int  pollIntervalMs     = 500;
  int  publishInterval    = 1000;
  bool enabled            = false;           // off by default, same pattern as RS485
} mbTcpConfig;

WiFiClient mbTcpClient;   // NetworkClient in core 3.x: goes out of whichever interface reaches the PLC (Ethernet)
uint16_t   mbTcpTxnId = 1;
char       mbTcpLastErr[48] = "";
uint32_t   mbTcpOkCount = 0, mbTcpErrCount = 0;
#define MBTCP_TIMEOUT_MS 1000

#define RS485_RX 18
#define RS485_TX 17
#define RS485_DE 21

HardwareSerial RS485Serial(1);
ModbusMaster   modbusNode;

#define MAX_RS485_REGS 32

struct RS485Register {
  char     name[32];
  uint8_t  fc;
  uint16_t addr;
  uint8_t  dt;           // 0=uint16, 1=int16, 2=float32, 3=scaled
  float    scale;
  char     mqttTopic[64];
  bool     wr;
  bool     en;
  char     onTxt[16];    // 1-bit registers: text for 1 / 0 (e.g. "DETECT" / "CLEAR"); empty = show 1/0
  char     offTxt[16];
  bool     toMqtt;       // publish this register to MQTT (Node-RED)
  bool     toTago;       // send this register to TagoIO
  char     tagoVar[32];  // TagoIO variable name; empty = made from the register name
  // runtime (not saved)
  float    lastVal;
  bool     lastOk;
  unsigned long lastPublishMs;
  unsigned long changedMs;  // millis() when the value last changed (time in current state)
  bool     haveVal;
};

RS485Register rs485Regs[MAX_RS485_REGS];
// ON/OFF texts: keep only safe characters, so they can go into JSON without escaping
void regTextCopy(char* dst, const char* src, size_t n) {
  size_t k = 0;
  for (size_t i = 0; src && src[i] && k + 1 < n; i++) {
    char c = src[i];
    if (c >= 32 && c < 127 && c != '"' && c != '\\') dst[k++] = c;
  }
  dst[k] = 0;
}
int           rs485RegCount        = 0;
bool          rs485Initialized     = false;
bool          rs485DeviceConnected = false;

SemaphoreHandle_t rs485Mutex      = NULL;
SemaphoreHandle_t mbTcpSock       = NULL;   // guards mbTcpClient (poll task vs web handlers)
// Modbus values -> MQTT: the poll task (core 0) queues, loop() (core 1) publishes.
// PubSubClient is not thread-safe, so only loop() may touch it.
struct MbPub { char topic[64]; char val[24]; };
QueueHandle_t mbPubQueue = NULL;
TaskHandle_t      rs485TaskHandle = NULL;

// ─── Fast connectivity check: separate from user poll rate ───────
// Runs internally every 2 s regardless of pollIntervalMs.
// Polls the first enabled register only to confirm device presence.
unsigned long rs485LastCheckMs = 0;
#define RS485_CHECK_INTERVAL_MS 2000

// ================= DO LOGIC RULES =================
// Each rule has:  doIdx (0-7), enabled, conditions[]
// Condition: diIdx(0-7), state(0=OFF,1=ON), op(0=first/IF, 1=AND, 2=OR)
#define MAX_DO_RULES  16
#define MAX_RULE_COND  8

struct RuleCond {
  uint8_t diIdx;  // 0-7
  uint8_t state;  // 0=OFF, 1=ON
  uint8_t op;     // 0=IF(first), 1=AND, 2=OR
};

struct DoRule {
  uint8_t  doIdx;
  bool     enabled;
  int      condCount;
  RuleCond conds[MAX_RULE_COND];
};

DoRule doRules[MAX_DO_RULES];
int    doRuleCount = 0;

// ================= diCurrentlyOn needed by evaluateDoRules() =================
bool diCurrentlyOn[8] = {false};

// Evaluate all enabled rules against current DI states
// Called from the DI scan after any state change
void evaluateDoRules() {
  for (int r=0; r<doRuleCount; r++) {
    DoRule& rule = doRules[r];
    if (!rule.enabled || rule.condCount==0) continue;
    // Evaluate left-to-right with AND/OR precedence (simple linear evaluation)
    bool result = (diCurrentlyOn[rule.conds[0].diIdx] ? 1:0) == rule.conds[0].state;
    for (int c=1; c<rule.condCount; c++) {
      bool cv = (diCurrentlyOn[rule.conds[c].diIdx] ? 1:0) == rule.conds[c].state;
      if (rule.conds[c].op==1) result = result && cv;  // AND
      else                     result = result || cv;  // OR
    }
    setOutput(rule.doIdx, result);
  }
}

void loadDoRules() {
  doRuleCount = 0;
  if (!LittleFS.exists(F("/do_rules.json"))) return;
  File f = LittleFS.open(F("/do_rules.json"),"r"); if(!f) return;
  StaticJsonDocument<4096> doc;
  if (deserializeJson(doc,f)){f.close();return;}
  f.close();
  for (JsonObject obj : doc.as<JsonArray>()) {
    if (doRuleCount>=MAX_DO_RULES) break;
    DoRule& rule = doRules[doRuleCount];
    rule.doIdx    = obj["doIdx"]   | 0;
    rule.enabled  = obj["enabled"] | false;
    rule.condCount = 0;
    for (JsonObject c : obj["conds"].as<JsonArray>()) {
      if (rule.condCount>=MAX_RULE_COND) break;
      rule.conds[rule.condCount].diIdx = c["diIdx"] | 0;
      rule.conds[rule.condCount].state = c["state"] | 1;
      rule.conds[rule.condCount].op    = c["op"]    | 0;
      rule.condCount++;
    }
    doRuleCount++;
  }
}

void saveDoRules() {
  StaticJsonDocument<4096> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i=0; i<doRuleCount; i++) {
    DoRule& rule = doRules[i];
    JsonObject obj = arr.createNestedObject();
    obj["doIdx"]   = rule.doIdx;
    obj["enabled"] = rule.enabled;
    JsonArray ca   = obj.createNestedArray("conds");
    for (int c=0; c<rule.condCount; c++) {
      JsonObject co = ca.createNestedObject();
      co["diIdx"] = rule.conds[c].diIdx;
      co["state"] = rule.conds[c].state;
      co["op"]    = rule.conds[c].op;
    }
  }
  File f = LittleFS.open(F("/do_rules.json"),"w"); if(!f) return;
  serializeJson(doc,f); f.close();
}

// ================= MACHINE STATE =================
#define MACHINE_STATE_TOPIC "esp32/machine/state"
#define COUNTER_RESET_TOPIC "esp32/counters/reset"   // publish target here to reset IO counters via MQTT
uint8_t machineStateIdx = 1;
const char* MACHINE_LABELS[] = {"UNKNOWN","RUNNING","STOPPED"};

// ================= METRICS =================
unsigned long triggerCount[8]  = {0};
unsigned long totalOnTimeMs[8] = {0};
unsigned long lastOnStartMs[8] = {0};
unsigned long cycleStartMs     = 0;
unsigned long lastCycleTimeMs  = 0;
bool          cycleActive      = false;
unsigned long rejectCount      = 0;
unsigned long runtimeMs        = 0;
unsigned long downtimeMs       = 0;
unsigned long lastMetricTickMs = 0;
unsigned long lastMetricAccMs  = 0;
unsigned long lastMetricsSaveMs= 0;
float         customEffPct     = -1.0f;

// ================= KPI SIGNALS =================
// Which signal drives each KPI. src: "" = not used, "di1".."di8" = board input, "mb:<register name>" = Modbus on/off register.
// on = the role is "active" when the signal is ON (true) or OFF (false).
#define KPI_ROLES 7
enum { KPI_RUN = 0, KPI_START, KPI_STOP, KPI_TOGGLE, KPI_CYC_START, KPI_CYC_END, KPI_REJECT };
const char* KPI_KEYS[KPI_ROLES] = {"run", "start", "stop", "toggle", "cycleStart", "cycleEnd", "reject"};
KpiSrc kpiCfg[KPI_ROLES];
int8_t kpiPrev[KPI_ROLES];   // last state per role: -1 unknown, 0 inactive, 1 active
// Defaults = the original fixed wiring: DI1 OFF toggles, DI2 start, DI3 stop, DI4 -> DI6 cycle, DI7 reject
void kpiDefaults() {
  const char* src[KPI_ROLES] = {"", "di2", "di3", "di1", "di4", "di6", "di7"};
  const bool  on [KPI_ROLES] = {true, true, true, false, true, true, true};
  for (int k = 0; k < KPI_ROLES; k++) { strlcpy(kpiCfg[k].src, src[k], sizeof(kpiCfg[k].src)); kpiCfg[k].on = on[k]; kpiPrev[k] = -1; }
}

// ================= EFFICIENCY FORMULA =================
// Set on the KPI page, saved in config.json ("effFormula"), worked out here so the Dashboard, the cards and the SD log agree.
// Names: runtime, downtime, total (= runtime + downtime), cycle (last cycle)  -> seconds
//        parts (completed cycles), rejects, good (= parts - rejects)
//        [Register name] -> current value of a Modbus register
// Operators: + - * / ( ) and numbers.   Empty formula = runtime / total * 100
#define EFF_DEFAULT "runtime / total * 100"
char effFormula[160] = "";
unsigned long partCount = 0;
static void effSkip(EffCtx& c) { while (*c.p == ' ' || *c.p == '\t') c.p++; }
static double effExpr(EffCtx& c);
static double effFactor(EffCtx& c) {
  effSkip(c);
  if (c.err) return 0;
  if (*c.p == '(') {
    c.p++; double v = effExpr(c); effSkip(c);
    if (*c.p != ')') { if (!c.err) c.err = "missing )"; return 0; }
    c.p++; return v;
  }
  if (*c.p == '-') { c.p++; return -effFactor(c); }
  if (*c.p == '+') { c.p++; return effFactor(c); }
  if ((*c.p >= '0' && *c.p <= '9') || *c.p == '.') {
    char* e; double v = strtod(c.p, &e);
    if (e == c.p) { c.err = "bad number"; return 0; }
    c.p = e; return v;
  }
  if (*c.p == '[') {
    const char* st = ++c.p;
    while (*c.p && *c.p != ']') c.p++;
    if (*c.p != ']') { c.err = "missing ]"; return 0; }
    char name[32]; size_t n = (size_t)(c.p - st); if (n > sizeof(name) - 1) n = sizeof(name) - 1;
    memcpy(name, st, n); name[n] = 0; c.p++;
    for (int i = 0; i < rs485RegCount; i++) if (!strcmp(rs485Regs[i].name, name)) {
      if (!rs485Regs[i].lastOk && !c.syntaxOnly) { c.err = "register has no value"; return 0; }
      return rs485Regs[i].lastVal;
    }
    c.err = "unknown register"; return 0;
  }
  if (isalpha((unsigned char)*c.p)) {
    char w[16]; int k = 0;
    while (isalnum((unsigned char)*c.p) || *c.p == '_') { if (k < 15) w[k++] = tolower((unsigned char)*c.p); c.p++; }
    w[k] = 0;
    double parts = partCount, rej = rejectCount;
    if (!strcmp(w, "runtime"))  return c.rt;
    if (!strcmp(w, "downtime")) return c.dt;
    if (!strcmp(w, "total"))    return c.rt + c.dt;
    if (!strcmp(w, "cycle"))    return lastCycleTimeMs / 1000.0;
    if (!strcmp(w, "parts"))    return parts;
    if (!strcmp(w, "rejects"))  return rej;
    if (!strcmp(w, "good"))     return parts > rej ? parts - rej : 0;
    c.err = "unknown name"; return 0;
  }
  c.err = *c.p ? "unexpected character" : "formula is incomplete"; return 0;
}
static double effTerm(EffCtx& c) {
  double v = effFactor(c);
  for (;;) {
    effSkip(c); char op = *c.p;
    if (op != '*' && op != '/') return v;
    c.p++; double r = effFactor(c);
    if (op == '*') v *= r;
    else if (r == 0) { if (!c.syntaxOnly && !c.err) c.err = "division by zero"; v = 0; }
    else v /= r;
  }
}
static double effExpr(EffCtx& c) {
  double v = effTerm(c);
  for (;;) {
    effSkip(c); char op = *c.p;
    if (op != '+' && op != '-') return v;
    c.p++; double r = effTerm(c);
    v = (op == '+') ? v + r : v - r;
  }
}
// Efficiency in %. false + err when it can't be worked out (e.g. nothing counted yet -> division by zero).
// syntaxOnly: only check the formula (division by zero / registers without a value are allowed).
bool effEval(const char* formula, double rtMs, double dtMs, double& out, const char** err, bool syntaxOnly) {
  EffCtx c{ (formula && formula[0]) ? formula : EFF_DEFAULT, nullptr, rtMs / 1000.0, dtMs / 1000.0, syntaxOnly };
  double v = effExpr(c); effSkip(c);
  if (!c.err && *c.p) c.err = "unexpected character";
  if (c.err) { if (err) *err = c.err; return false; }
  if (isnan(v) || isinf(v)) { if (err) *err = "not a number"; return false; }
  out = v; if (err) *err = nullptr; return true;
}
// runtime / downtime in ms including the part the 100 ms tick hasn't added yet
void liveRunDown(unsigned long& rt, unsigned long& dt) {
  unsigned long now = millis(); rt = runtimeMs; dt = downtimeMs;
  if (machineStateIdx == 1) rt += now - lastMetricTickMs; else if (machineStateIdx == 2) dt += now - lastMetricTickMs;
}
// current efficiency in %, or -1 when it can't be worked out
float effNow() { unsigned long rt, dt; liveRunDown(rt, dt); double v; return effEval(effFormula, rt, dt, v, nullptr, false) ? (float)v : -1.0f; }

// ================= DI DEBOUNCE =================
#define DEBOUNCE_MS 10
int           lastRawState[8];
unsigned long debounceTimer[8];

// ================= TIMING =================
unsigned long lastDiScanMs = 0;

// ── Per-topic heartbeat timers (DI/DO) ────────────────────────────
unsigned long diLastPubMs[8] = {0};
unsigned long doLastPubMs[8] = {0};

// ================= WEB SERVER / MQTT =================
WebServer        server(80);
unsigned long    lastMqttRetry     = 0;
const unsigned long mqttRetryInterval = 5000;

// ================= SESSION =================
char activeToken[33] = "";

// ================= TELEGRAM =================
struct TelegramConfig {
  char botToken[128];
  char chatId[24];
  bool enabled;
  int  rejectThreshold;
} tgConfig;

WiFiClientSecure     tgSecureClient;
UniversalTelegramBot tgBot("", tgSecureClient);
bool                 tgInitialized = false;

QueueHandle_t   tgOutQueue   = NULL;
TaskHandle_t    tgTaskHandle = NULL;
volatile bool   tgResetMetricsFlag = false;
const unsigned long TG_POLL_MS = 2500;

// ================= USER ACCOUNTS =================
struct UserAccount { char username[30]; char password[30]; };
UserAccount users[10];
int userCount = 0;

// ================= SD CARD =================
#define SD_CLK  48
#define SD_CMD  47
#define SD_D0   45
#define NET_SCS 16

bool          sdMounted            = false;
int           sdLogIntervalSec     = 60;
bool          sdLogEnabled         = true;
unsigned long lastSdLogMs          = 0;
char          currentLogFile[40]   = "";
unsigned long logRotateIntervalSec = 3600;
unsigned long logFileStartMs       = 0;

// ===========================================================
//  HELPERS
// ===========================================================
static String fmtMsBot(unsigned long ms) {
  unsigned long s=ms/1000,m=s/60,h=m/60;
  char buf[12]; snprintf(buf,sizeof(buf),"%02lu:%02lu:%02lu",h,m%60,s%60);
  return String(buf);
}

unsigned long getOnTime(int ch) {
  unsigned long t=totalOnTimeMs[ch];
  if(diCurrentlyOn[ch]) t+=millis()-lastOnStartMs[ch];
  return t;
}

unsigned long getDoOnTime(int ch) {
  unsigned long t = doTotalOnTimeMs[ch];
  if ((outputState>>ch)&1) t += millis() - doLastOnStartMs[ch];
  return t;
}

// ===========================================================
//  TELEGRAM
// ===========================================================
void loadTelegramConfig() {
  memset(&tgConfig,0,sizeof(tgConfig)); tgConfig.rejectThreshold=10;
  if (!LittleFS.exists(F("/telegram.json"))) return;
  File f=LittleFS.open(F("/telegram.json"),"r"); if(!f) return;
  StaticJsonDocument<384> doc;
  if (!deserializeJson(doc,f)){
    strlcpy(tgConfig.botToken, doc["botToken"]|"", sizeof(tgConfig.botToken));
    strlcpy(tgConfig.chatId,   doc["chatId"]  |"", sizeof(tgConfig.chatId));
    tgConfig.enabled         = doc["enabled"]         | false;
    tgConfig.rejectThreshold = doc["rejectThreshold"] | 10;
  }
  f.close();
}
void saveTelegramConfig() {
  StaticJsonDocument<384> doc;
  doc["botToken"]=tgConfig.botToken; doc["chatId"]=tgConfig.chatId;
  doc["enabled"]=tgConfig.enabled; doc["rejectThreshold"]=tgConfig.rejectThreshold;
  File f=LittleFS.open(F("/telegram.json"),"w"); if(!f) return;
  serializeJson(doc,f); f.close();
}
void initTelegramBot() {
  tgInitialized=false;
  if (strlen(tgConfig.botToken)<10) return;
  tgSecureClient.setInsecure(); tgSecureClient.setTimeout(8);
  tgBot=UniversalTelegramBot(tgConfig.botToken,tgSecureClient);
  tgInitialized=true;
}
void tgSend(const String& msg) {
  if (!tgInitialized||!tgConfig.enabled) return;
  if (!strlen(tgConfig.chatId)||tgOutQueue==NULL) return;
  char buf[256]={0}; msg.substring(0,254).toCharArray(buf,sizeof(buf));
  xQueueSend(tgOutQueue,buf,pdMS_TO_TICKS(0));
}

String buildStatusMsg() {
  char buf[512]; int p=0;
  String ip=eth_connected?ETH.localIP().toString():WiFi.localIP().toString();
  p+=snprintf(buf+p,sizeof(buf)-p,"*Status*\nMachine: *%s*\nIP: `%s`\n\n*Inputs:*\n",
    MACHINE_LABELS[machineStateIdx],ip.c_str());
  for(int i=0;i<8;i++) p+=snprintf(buf+p,sizeof(buf)-p,"DI%d: %s\n",i+1,diCurrentlyOn[i]?"ON":"OFF");
  p+=snprintf(buf+p,sizeof(buf)-p,"\n*Outputs:*\n");
  for(int i=0;i<8;i++) p+=snprintf(buf+p,sizeof(buf)-p,"DO%d: %s\n",i+1,((outputState>>i)&1)?"ON":"OFF");
  return String(buf);
}
String buildMetricsMsg() {
  unsigned long now=millis(),rt=runtimeMs,dt=downtimeMs;
  if(machineStateIdx==1) rt+=now-lastMetricTickMs;
  else if(machineStateIdx==2) dt+=now-lastMetricTickMs;
  float eff=effNow(); if(eff<0) eff=0;   // efficiency formula (KPI page)
  char buf[256];
  snprintf(buf,sizeof(buf),"*Metrics*\nRuntime: `%s`\nDowntime: `%s`\nCycle: `%s`\nRejects: `%lu`\nEfficiency: `%.1f%%`",
    fmtMsBot(rt).c_str(),fmtMsBot(dt).c_str(),
    lastCycleTimeMs>0?(String(lastCycleTimeMs/1000.0,2)+" s").c_str():"--",rejectCount,eff);
  return String(buf);
}
String buildHealthMsg() {
  unsigned long upS=millis()/1000,upM=upS/60,upH=upM/60,upD=upH/24;
  char up[32],buf[256];
  if(upD>0) snprintf(up,sizeof(up),"%lud %02lu:%02lu:%02lu",upD,upH%24,upM%60,upS%60);
  else       snprintf(up,sizeof(up),"%02lu:%02lu:%02lu",upH,upM%60,upS%60);
  snprintf(buf,sizeof(buf),"*Health*\nTemp: `%.1f C`\nHeap: `%lu KB`\nNet: `%s`\nCPU: `%u MHz`\nUp: `%s`",
    temperatureRead(),(unsigned long)(ESP.getFreeHeap()/1024),
    eth_connected?"Ethernet":(String(WiFi.RSSI())+" dBm").c_str(),getCpuFrequencyMhz(),up);
  return String(buf);
}

void handleTelegramMessage(const telegramMessage& m) {
  if (strlen(tgConfig.chatId)>0 && m.chat_id!=String(tgConfig.chatId)){
    tgBot.sendMessage(m.chat_id,"Unauthorized.",""); return;
  }
  String text=m.text; text.trim();
  if(text=="/help"){
    tgBot.sendMessage(m.chat_id,
      "*ESP32 Bot Commands*\n\n"
      "/status — DI/DO states\n/metrics — Runtime, downtime, rejects\n"
      "/health — Temp, heap, WiFi, uptime\n"
      "/do on N — Turn DO N ON (1-8)\n/do off N — Turn DO N OFF (1-8)\n"
      "/resetmetrics — Reset all counters\n/help — Show this menu","Markdown");
  }
  else if(text=="/status")  tgBot.sendMessage(m.chat_id,buildStatusMsg(),"Markdown");
  else if(text=="/metrics") tgBot.sendMessage(m.chat_id,buildMetricsMsg(),"Markdown");
  else if(text=="/health")  tgBot.sendMessage(m.chat_id,buildHealthMsg(),"Markdown");
  else if(text.startsWith("/do ")){
    String sub=text.substring(4); sub.trim();
    bool on=false; int ch=-1;
    if(sub.startsWith("on "))      {on=true;  ch=sub.substring(3).toInt()-1;}
    else if(sub.startsWith("off ")){on=false; ch=sub.substring(4).toInt()-1;}
    if(ch>=0&&ch<=7){setOutput(ch,on); tgBot.sendMessage(m.chat_id,"DO"+String(ch+1)+" turned "+(on?"ON":"OFF"),"");}
    else tgBot.sendMessage(m.chat_id,"Usage: /do on N  or  /do off N  (N=1-8)","");
  }
  else if(text=="/resetmetrics"){ tgResetMetricsFlag=true; tgBot.sendMessage(m.chat_id,"Reset requested.",""); }
  else tgBot.sendMessage(m.chat_id,"Unknown command. Send /help for the list.","");
}

void telegramTask(void* pv) {
  for(;;){
    vTaskDelay(pdMS_TO_TICKS(TG_POLL_MS));
    if(!tgInitialized||!tgConfig.enabled) continue;
    if(WiFi.status()!=WL_CONNECTED&&!eth_connected) continue;
    char outMsg[256];
    if(xQueueReceive(tgOutQueue,outMsg,0)==pdTRUE){ tgBot.sendMessage(tgConfig.chatId,outMsg,"Markdown"); continue; }
    int n=tgBot.getUpdates(tgBot.last_message_received+1);
    for(int i=0;i<n;i++) handleTelegramMessage(tgBot.messages[i]);
  }
}

// ===========================================================
//  MACHINE STATE
// ===========================================================
void setMachineState(uint8_t idx) {
  if(machineStateIdx==idx) return;
  machineStateIdx=idx;
  if(idx==1) tgSend(F("Machine: *RUNNING*"));
  else if(idx==2) tgSend(F("Machine: *STOPPED*"));
}
// 1 = role active, 0 = inactive, -1 = no valid value (not set, register missing, not polled, read error)
int kpiRead(const KpiSrc& k) {
  if (!k.src[0]) return -1;
  bool v;
  if (k.src[0] == 'd' && k.src[1] == 'i') {
    int n = atoi(k.src + 2) - 1; if (n < 0 || n > 7) return -1;
    v = diCurrentlyOn[n];
  } else if (!strncmp(k.src, "mb:", 3)) {
    int idx = -1;
    for (int i = 0; i < rs485RegCount; i++) if (!strcmp(rs485Regs[i].name, k.src + 3)) { idx = i; break; }
    if (idx < 0) return -1;
    RS485Register& r = rs485Regs[idx];
    if (!r.en || !r.lastOk || !(mbTcpConfig.enabled || rs485Config.enabled)) return -1;
    v = r.lastVal != 0;
  } else return -1;
  return (v == k.on) ? 1 : 0;
}
// Called from loop(): turns signal changes into KPI events.
void kpiTick() {
  unsigned long now = millis();
  for (int k = 0; k < KPI_ROLES; k++) {
    int v = kpiRead(kpiCfg[k]);
    if (v < 0) continue;                        // no value: keep the last state, no events
    int p = kpiPrev[k]; kpiPrev[k] = v;
    if (k == KPI_RUN) { if (p != v) setMachineState(v ? 1 : 2); continue; }   // level: machine state follows the signal
    if (p != 0 || v != 1) continue;             // other roles fire when the signal BECOMES active (not at start-up)
    switch (k) {
      case KPI_START:     setMachineState(1); break;
      case KPI_STOP:      setMachineState(2); break;
      case KPI_TOGGLE:    setMachineState(machineStateIdx == 1 ? 2 : 1); break;
      case KPI_CYC_START: cycleStartMs = now; cycleActive = true; break;
      case KPI_CYC_END:   if (cycleActive) { lastCycleTimeMs = now - cycleStartMs; cycleActive = false; partCount++; } break;
      case KPI_REJECT:
        rejectCount++;
        if (tgConfig.rejectThreshold > 0 && (rejectCount % (unsigned long)tgConfig.rejectThreshold == 0))
          tgSend("*Reject Alert!* Total: *" + String(rejectCount) + "*");
        break;
    }
  }
}
void autoUpdateMachineState(int ch, int state) {
  if(ch==0&&state==0){ setMachineState(machineStateIdx==1?2:1); return; }
  if(ch==1&&state==1){ setMachineState(1); return; }
  if(ch==2&&state==1){ setMachineState(2); return; }
}

// ===========================================================
//  METRICS PERSIST
// ===========================================================
void saveMetrics() {
  StaticJsonDocument<768> doc;
  doc["rejects"]=rejectCount; doc["runtime"]=runtimeMs; doc["downtime"]=downtimeMs;
  doc["machineState"]=machineStateIdx; doc["customEff"]=customEffPct; doc["parts"]=partCount;
  JsonArray tc=doc.createNestedArray("tc"), ot=doc.createNestedArray("ot");
  JsonArray dtc=doc.createNestedArray("dtc"), dot=doc.createNestedArray("dot");
  for(int i=0;i<8;i++){
    tc.add(triggerCount[i]); ot.add(totalOnTimeMs[i]);
    dtc.add(doTriggerCount[i]); dot.add(doTotalOnTimeMs[i]);
  }
  File f=LittleFS.open(F("/metrics.json"),"w"); if(f){serializeJson(doc,f);f.close();}
}
void loadMetrics() {
  if(!LittleFS.exists(F("/metrics.json"))) return;
  File f=LittleFS.open(F("/metrics.json"),"r"); if(!f) return;
  StaticJsonDocument<768> doc;
  if(!deserializeJson(doc,f)){
    rejectCount=doc["rejects"]|(unsigned long)0; runtimeMs=doc["runtime"]|(unsigned long)0;
    downtimeMs=doc["downtime"]|(unsigned long)0; machineStateIdx=doc["machineState"]|(uint8_t)1;
    customEffPct=doc["customEff"]|-1.0f; partCount=doc["parts"]|(unsigned long)0;
    for(int i=0;i<8;i++){
      triggerCount[i]=doc["tc"][i]|(unsigned long)0; totalOnTimeMs[i]=doc["ot"][i]|(unsigned long)0;
      doTriggerCount[i]=doc["dtc"][i]|(unsigned long)0; doTotalOnTimeMs[i]=doc["dot"][i]|(unsigned long)0;
    }
  }
  f.close();
}

// ===========================================================
//  SD CARD
// ===========================================================
void getLogFilename(char* buf,size_t len) {
  struct tm t;
  if(getLocalTime(&t,200)) snprintf(buf,len,"/log_%04d%02d%02d_%02d%02d.csv",t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min);
  else snprintf(buf,len,"/log_boot_%luh.csv",millis()/3600000UL);
}
void ensureLogHeader() {
  if(currentLogFile[0]=='\0'||SD_MMC.exists(currentLogFile)) return;
  File f=SD_MMC.open(currentLogFile,FILE_WRITE);
  if(f){f.println(F("millis,state,runtime_ms,downtime_ms,cycle_ms,rejects,eff_pct,di1_cnt,di2_cnt,di3_cnt,di4_cnt,di5_cnt,di6_cnt,di7_cnt,di8_cnt"));f.close();}
}
void initLogFile() {
  getLogFilename(currentLogFile,sizeof(currentLogFile));
  logFileStartMs=millis(); ensureLogHeader();
}
void publishSDInfo() {
  if(!mqtt.connected()) return;
  StaticJsonDocument<128> doc; doc["mounted"]=sdMounted; doc["logFile"]=currentLogFile;
  if(sdMounted) doc["freeMB"]=(uint32_t)((SD_MMC.cardSize()-SD_MMC.usedBytes())/(1024*1024));
  String out; serializeJson(doc,out); mqtt.publish("esp32/sd/info",out.c_str());
}
void sdLogRow() {
  if(!sdMounted||currentLogFile[0]=='\0') return;
  ensureLogHeader();
  File f=SD_MMC.open(currentLogFile,FILE_APPEND); if(!f) return;
  unsigned long now=millis(),rt=runtimeMs,dt=downtimeMs;
  if(machineStateIdx==1) rt+=now-lastMetricTickMs;
  else if(machineStateIdx==2) dt+=now-lastMetricTickMs;
  float eff=effNow(); if(eff<0) eff=0;   // efficiency formula (KPI page)
  char row[320]; int p=0;
  p+=snprintf(row+p,sizeof(row)-p,"%lu,%s,%lu,%lu,%lu,%lu,%.1f",now,MACHINE_LABELS[machineStateIdx],rt,dt,lastCycleTimeMs,rejectCount,eff);
  for(int i=0;i<8;i++) p+=snprintf(row+p,sizeof(row)-p,",%lu",triggerCount[i]);
  f.println(row); f.close(); publishSDInfo();
}

// ===========================================================
//  CONFIG LOAD / SAVE
// ===========================================================
void loadConfig() {
  // Defaults
  strlcpy(mqttCfg.mode,"cloud",sizeof(mqttCfg.mode));
  strlcpy(mqttCfg.proto,"wss",sizeof(mqttCfg.proto));
  strlcpy(mqttCfg.host,"1b88e7df-d8af-4d46-ac2b-43963d26bdf2-00-if445lcva84n.pike.replit.dev",sizeof(mqttCfg.host));
  mqttCfg.port=443; strlcpy(mqttCfg.path,"/mqtt",sizeof(mqttCfg.path));
  for(int i=0;i<8;i++){inputConfig[i].pubIntervalMs=0; outputConfig[i].pubIntervalMs=0;}

  if(!LittleFS.exists(F("/config.json"))) return;
  File f=LittleFS.open(F("/config.json"),"r"); if(!f) return;
  DynamicJsonDocument doc(4096);
  if(deserializeJson(doc,f)){f.close();return;} f.close();
  strlcpy(deviceName,doc["deviceName"]|"esp32",sizeof(deviceName));
  strlcpy(mqttCfg.mode, doc["mqttMode"] |"cloud",sizeof(mqttCfg.mode));
  strlcpy(mqttCfg.proto,doc["mqttProto"]|"wss",  sizeof(mqttCfg.proto));
  strlcpy(mqttCfg.host, doc["mqttHost"] |"",     sizeof(mqttCfg.host));
  mqttCfg.port=doc["mqttPort"]|443;
  strlcpy(mqttCfg.path, doc["mqttPath"] |"/mqtt",sizeof(mqttCfg.path));

  for(int i=0;i<8;i++){
    strlcpy(inputConfig[i].topic,  doc["inputs"][i]  |"",50);
    strlcpy(outputConfig[i].topic, doc["outputs"][i] |"",50);
    inputConfig[i].pubIntervalMs  = doc["inputPubMs"][i]  | (uint32_t)0;
    outputConfig[i].pubIntervalMs = doc["outputPubMs"][i] | (uint32_t)0;
  }

  rs485Config.baud           = doc["rs485"]["baud"]          |9600;
  rs485Config.stopBits       = doc["rs485"]["stopBits"]       |1;
  rs485Config.dataBits       = doc["rs485"]["dataBits"]       |8;
  rs485Config.pollIntervalMs = doc["rs485"]["pollIntervalMs"] |500;
  rs485Config.publishInterval= doc["rs485"]["publishInterval"]|1000;
  rs485Config.enabled        = doc["rs485"]["enabled"]        |false;
  strlcpy(rs485Config.parity,doc["rs485"]["parity"]|"None",sizeof(rs485Config.parity));
  strlcpy(rs485Config.addr,  doc["rs485"]["addr"]  |"1",   sizeof(rs485Config.addr));

  mbTcpConfig.port            = doc["mbtcp"]["port"]           |502;
  mbTcpConfig.unitId          = doc["mbtcp"]["unitId"]          |1;
  mbTcpConfig.pollIntervalMs  = doc["mbtcp"]["pollIntervalMs"]  |500;
  mbTcpConfig.publishInterval = doc["mbtcp"]["publishInterval"] |1000;
  mbTcpConfig.enabled         = doc["mbtcp"]["enabled"]         |false;
  strlcpy(mbTcpConfig.ip, doc["mbtcp"]["ip"] |"192.168.250.7", sizeof(mbTcpConfig.ip));
  strlcpy(effFormula, doc["effFormula"] | "", sizeof(effFormula));
  JsonObject kj = doc["kpi"];
  if (!kj.isNull()) for (int k = 0; k < KPI_ROLES; k++) {
    JsonObject o = kj[KPI_KEYS[k]]; if (o.isNull()) continue;
    strlcpy(kpiCfg[k].src, o["src"] | "", sizeof(kpiCfg[k].src)); kpiCfg[k].on = o["on"] | true;
  }
  tagoCfg.enabled = doc["tago"]["enabled"] | false;
  strlcpy(tagoCfg.token, doc["tago"]["token"] | "", sizeof(tagoCfg.token));
  strlcpy(tagoCfg.url,   doc["tago"]["url"]   | "https://api.tago.io/data", sizeof(tagoCfg.url));
  ethCfg.dhcp = doc["eth"]["dhcp"] | false;
  strlcpy(ethCfg.ip,   doc["eth"]["ip"]   | "192.168.250.10", sizeof(ethCfg.ip));
  strlcpy(ethCfg.mask, doc["eth"]["mask"] | "255.255.255.0",  sizeof(ethCfg.mask));
  strlcpy(ethCfg.gw,   doc["eth"]["gw"]   | "",               sizeof(ethCfg.gw));
  sdLogIntervalSec     = doc["sdLogInterval"]      |60;
  sdLogEnabled         = doc["sdLogEnabled"]        |true;
  logRotateIntervalSec = doc["sdLogRotateInterval"] |3600UL;

  // Load users embedded in config.json (fallback if users.json absent)
  if (doc.containsKey("users")) {
    userCount = 0;
    for (JsonObject u : doc["users"].as<JsonArray>()) {
      if (userCount >= 10) break;
      strlcpy(users[userCount].username, u["username"] | "", 30);
      strlcpy(users[userCount].password, u["password"] | "", 30);
      userCount++;
    }
  }
}

void saveConfig() {
  DynamicJsonDocument doc(4096);
  doc["deviceName"]=deviceName; doc["mqttMode"]=mqttCfg.mode;
  doc["mqttProto"]=mqttCfg.proto; doc["mqttHost"]=mqttCfg.host;
  doc["mqttPort"]=mqttCfg.port; doc["mqttPath"]=mqttCfg.path;
  for(int i=0;i<8;i++){
    doc["inputs"][i]=inputConfig[i].topic; doc["outputs"][i]=outputConfig[i].topic;
    doc["inputPubMs"][i]=inputConfig[i].pubIntervalMs;
    doc["outputPubMs"][i]=outputConfig[i].pubIntervalMs;
  }
  JsonObject rs=doc.createNestedObject("rs485");
  rs["baud"]=rs485Config.baud; rs["parity"]=rs485Config.parity;
  rs["stopBits"]=rs485Config.stopBits; rs["dataBits"]=rs485Config.dataBits;
  rs["addr"]=rs485Config.addr;
  rs["pollIntervalMs"]=rs485Config.pollIntervalMs;
  rs["publishInterval"]=rs485Config.publishInterval;
  rs["enabled"]=rs485Config.enabled;
  JsonObject mb=doc.createNestedObject("mbtcp");
  mb["ip"]=mbTcpConfig.ip; mb["port"]=mbTcpConfig.port; mb["unitId"]=mbTcpConfig.unitId;
  mb["pollIntervalMs"]=mbTcpConfig.pollIntervalMs;
  mb["publishInterval"]=mbTcpConfig.publishInterval;
  mb["enabled"]=mbTcpConfig.enabled;
  doc["effFormula"]=effFormula;
  JsonObject kp=doc.createNestedObject("kpi");
  for(int k=0;k<KPI_ROLES;k++){ JsonObject o=kp.createNestedObject(KPI_KEYS[k]); o["src"]=kpiCfg[k].src; o["on"]=kpiCfg[k].on; }
  JsonObject tg=doc.createNestedObject("tago");
  tg["enabled"]=tagoCfg.enabled; tg["token"]=tagoCfg.token; tg["url"]=tagoCfg.url;
  JsonObject et=doc.createNestedObject("eth");
  et["dhcp"]=ethCfg.dhcp; et["ip"]=ethCfg.ip; et["mask"]=ethCfg.mask; et["gw"]=ethCfg.gw;
  doc["sdLogInterval"]=sdLogIntervalSec; doc["sdLogEnabled"]=sdLogEnabled;
  doc["sdLogRotateInterval"]=logRotateIntervalSec;
  File f=LittleFS.open(F("/config.json"),"w"); if(!f) return;
  serializeJson(doc,f); f.close();
}

// ===========================================================
//  RS485 REGISTER PERSIST
// ===========================================================
void loadRS485Regs() {
  rs485RegCount=0;
  if(!LittleFS.exists(F("/rs485_regs.json"))) return;
  File f=LittleFS.open(F("/rs485_regs.json"),"r"); if(!f) return;
  DynamicJsonDocument doc(12288);
  if(deserializeJson(doc,f)){f.close();return;} f.close();
  for(JsonObject obj:doc.as<JsonArray>()){
    if(rs485RegCount>=MAX_RS485_REGS) break;
    RS485Register& r=rs485Regs[rs485RegCount];
    strlcpy(r.name,     obj["name"]     |"Reg",sizeof(r.name));
    strlcpy(r.mqttTopic,obj["mqttTopic"]|"",   sizeof(r.mqttTopic));
    r.fc=obj["fc"]|3; r.addr=obj["addr"]|0; r.dt=obj["dt"]|0;
    r.scale=obj["scale"]|1.0f; r.wr=obj["wr"]|false; r.en=obj["en"]|true;
    regTextCopy(r.onTxt, obj["on"]|"", sizeof(r.onTxt)); regTextCopy(r.offTxt, obj["off"]|"", sizeof(r.offTxt));
    r.toMqtt = obj["mqtt"] | true; r.toTago = obj["tago"] | true;
    regTextCopy(r.tagoVar, obj["tv"]|"", sizeof(r.tagoVar));
    r.lastOk=false; r.lastVal=0.0f; r.lastPublishMs=0; r.haveVal=false; r.changedMs=0;
    rs485RegCount++;
  }
}
void saveRS485Regs() {
  DynamicJsonDocument doc(12288);
  JsonArray arr=doc.to<JsonArray>();
  for(int i=0;i<rs485RegCount;i++){
    RS485Register& r=rs485Regs[i];
    JsonObject obj=arr.createNestedObject();
    obj["name"]=r.name; obj["fc"]=r.fc; obj["addr"]=r.addr;
    obj["dt"]=r.dt; obj["scale"]=r.scale; obj["mqttTopic"]=r.mqttTopic;
    obj["wr"]=r.wr; obj["en"]=r.en;
    if(r.onTxt[0]||r.offTxt[0]){ obj["on"]=r.onTxt; obj["off"]=r.offTxt; }
    obj["mqtt"]=r.toMqtt; obj["tago"]=r.toTago;
    if(r.tagoVar[0]) obj["tv"]=r.tagoVar;
  }
  File f=LittleFS.open(F("/rs485_regs.json"),"w"); if(!f) return;
  serializeJson(doc,f); f.close();
}

// ===========================================================
//  RS485 INIT + TASK
// ===========================================================
void rs485PreTx()  { digitalWrite(RS485_DE,HIGH); }
void rs485PostTx() { digitalWrite(RS485_DE,LOW);  }

uint32_t buildSerialConfig() {
  int db=rs485Config.dataBits,sb=rs485Config.stopBits; char p=rs485Config.parity[0];
  if(db==8){if(p=='N')return sb==2?SERIAL_8N2:SERIAL_8N1; if(p=='E')return sb==2?SERIAL_8E2:SERIAL_8E1; if(p=='O')return sb==2?SERIAL_8O2:SERIAL_8O1;}
  if(db==7){if(p=='N')return SERIAL_7N1; if(p=='E')return SERIAL_7E1; if(p=='O')return SERIAL_7O1;}
  return SERIAL_8N1;
}

void initRS485() {
  RS485Serial.end();
  RS485Serial.begin(rs485Config.baud,buildSerialConfig(),RS485_RX,RS485_TX);
  int sa=String(rs485Config.addr).toInt(); if(sa<1||sa>247) sa=1;
  modbusNode.begin((uint8_t)sa,RS485Serial);
  modbusNode.preTransmission(rs485PreTx);
  modbusNode.postTransmission(rs485PostTx);
  rs485Initialized=true; rs485DeviceConnected=false;
  Serial.printf("RS485: %d baud %s slave %d poll %dms publish %dms\n",
    rs485Config.baud,rs485Config.parity,sa,rs485Config.pollIntervalMs,rs485Config.publishInterval);
}

// Poll a single register and update its live value.
// Returns true on Modbus success.
bool pollRS485Reg(int idx) {
  if(idx<0||idx>=rs485RegCount) return false;
  RS485Register& r=rs485Regs[idx];
  if(!r.en) return false;
  uint8_t result=ModbusMaster::ku8MBInvalidCRC;
  switch(r.fc){
    case 1: result=modbusNode.readCoils(r.addr,1); break;
    case 2: result=modbusNode.readDiscreteInputs(r.addr,1); break;
    case 3: result=modbusNode.readHoldingRegisters(r.addr,r.dt==2?2:1); break;
    case 4: result=modbusNode.readInputRegisters(r.addr,r.dt==2?2:1); break;
  }
  if(result==ModbusMaster::ku8MBSuccess){
    r.lastOk=true;
    float prevVal=r.lastVal; bool hadVal=r.haveVal;
    if(r.fc==1||r.fc==2){
      r.lastVal=(float)(modbusNode.getResponseBuffer(0)&0x01);
    } else {
      switch(r.dt){
        case 0: r.lastVal=(float)(uint16_t)modbusNode.getResponseBuffer(0); break;
        case 1: r.lastVal=(float)(int16_t) modbusNode.getResponseBuffer(0); break;
        case 2:{uint16_t hi=modbusNode.getResponseBuffer(0),lo=modbusNode.getResponseBuffer(1);uint32_t raw=((uint32_t)hi<<16)|lo;memcpy(&r.lastVal,&raw,sizeof(float));break;}
        case 3: r.lastVal=(float)(uint16_t)modbusNode.getResponseBuffer(0)*r.scale; break;
        default: r.lastVal=(float)modbusNode.getResponseBuffer(0); break;
      }
    }
    // MQTT publish — independent of poll rate
    unsigned long now=millis();
    if(!hadVal || r.lastVal!=prevVal){ r.changedMs=now; r.haveVal=true; }
    if(r.toMqtt && strlen(r.mqttTopic)>0 && mqtt.connected() &&
       (now-r.lastPublishMs>=(unsigned long)rs485Config.publishInterval)){
      char vs[32];
      if(r.fc==1||r.fc==2) snprintf(vs,sizeof(vs),"%d",(int)r.lastVal);
      else if(r.dt==0||r.dt==1) snprintf(vs,sizeof(vs),"%d",(int)r.lastVal);
      else snprintf(vs,sizeof(vs),"%.6g",r.lastVal);
      mqtt.publish(r.mqttTopic,vs);
      r.lastPublishMs=now;
    }
    return true;
  } else {
    r.lastOk=false;
    Serial.printf("Modbus err reg[%d] fc=%d addr=%d: 0x%02X\n",idx,r.fc,r.addr,result);
    return false;
  }
}

// ===========================================================
//  MODBUS TCP -- same register semantics as pollRS485Reg(), different wire
// ===========================================================
static void mbTcpSetErr(const char* e) {
  strlcpy(mbTcpLastErr, e, sizeof(mbTcpLastErr));
  mbTcpErrCount++;
}

bool mbTcpEnsureConnected() {
  if (mbTcpClient.connected()) return true;
  mbTcpClient.stop();
  IPAddress plc;
  if (!plc.fromString(mbTcpConfig.ip)) { mbTcpSetErr("invalid PLC IP"); return false; }
  if (!mbTcpClient.connect(plc, mbTcpConfig.port, MBTCP_TIMEOUT_MS)) {
    mbTcpSetErr(eth_link ? "cannot connect to PLC" : "no Ethernet link");
    return false;
  }
  mbTcpClient.setNoDelay(true);
  return true;
}

// Read exactly n bytes or give up after MBTCP_TIMEOUT_MS.
static bool mbTcpReadExact(uint8_t* b, size_t n) {
  unsigned long t0 = millis();
  size_t got = 0;
  while (got < n) {
    int a = mbTcpClient.available();
    if (a > 0) { int r = mbTcpClient.read(b + got, n - got); if (r > 0) got += r; }
    else {
      if (!mbTcpClient.connected() || millis() - t0 > MBTCP_TIMEOUT_MS) return false;
      delay(2);
    }
  }
  return true;
}

// Send one request PDU (fc + data) and receive the reply PDU into resp (after the unit id).
// Returns reply PDU length, or -1 on error (mbTcpLastErr says why).
static bool mbSockLock(uint32_t ms) { return mbTcpSock && xSemaphoreTakeRecursive(mbTcpSock, pdMS_TO_TICKS(ms)) == pdTRUE; }
static void mbSockUnlock() { xSemaphoreGiveRecursive(mbTcpSock); }
void mbTcpStopSafe() { if (mbSockLock(3000)) { mbTcpClient.stop(); mbSockUnlock(); } }
static int mbTcpExchangeRaw(const uint8_t* pdu, uint8_t pduLen, uint8_t* resp, int respMax);
static int mbTcpExchange(const uint8_t* pdu, uint8_t pduLen, uint8_t* resp, int respMax) {
  if (!mbSockLock(3000)) { mbTcpSetErr("socket busy"); return -1; }
  int r = mbTcpExchangeRaw(pdu, pduLen, resp, respMax);
  mbSockUnlock();
  return r;
}
static int mbTcpExchangeRaw(const uint8_t* pdu, uint8_t pduLen, uint8_t* resp, int respMax) {
  if (!mbTcpEnsureConnected()) return -1;
  while (mbTcpClient.available()) mbTcpClient.read();          // drop anything stale
  uint16_t txn = mbTcpTxnId++;
  uint8_t req[260];
  uint16_t len = pduLen + 1;                                  // unit id + PDU
  req[0] = txn >> 8; req[1] = txn & 0xFF; req[2] = 0; req[3] = 0;
  req[4] = len >> 8; req[5] = len & 0xFF; req[6] = (uint8_t)mbTcpConfig.unitId;
  memcpy(req + 7, pdu, pduLen);
  if (mbTcpClient.write(req, 7 + pduLen) != (size_t)(7 + pduLen)) { mbTcpClient.stop(); mbTcpSetErr("send failed"); return -1; }
  uint8_t hdr[7];
  if (!mbTcpReadExact(hdr, 7)) { mbTcpClient.stop(); mbTcpSetErr("no reply (timeout)"); return -1; }
  uint16_t rlen = (hdr[4] << 8) | hdr[5];
  if (((hdr[0] << 8) | hdr[1]) != txn || hdr[2] || hdr[3] || rlen < 2 || rlen > 254) {
    mbTcpClient.stop(); mbTcpSetErr("bad reply header"); return -1;
  }
  int plen = rlen - 1;
  if (plen > respMax) { mbTcpClient.stop(); mbTcpSetErr("reply too long"); return -1; }
  if (!mbTcpReadExact(resp, plen)) { mbTcpClient.stop(); mbTcpSetErr("short reply"); return -1; }
  if (resp[0] == (pdu[0] | 0x80)) {                         // Modbus exception
    char e[48]; snprintf(e, sizeof(e), "PLC exception %u (fc %u addr %u)", resp[1], pdu[0], (pdu[1] << 8) | pdu[2]);
    mbTcpSetErr(e); return -1;
  }
  if (resp[0] != pdu[0]) { mbTcpClient.stop(); mbTcpSetErr("unexpected function code"); return -1; }
  return plen;
}

// Read: out[0..1] receive up to 2 register values (fc3/4); out[0] holds a single bit for fc1/2.
bool mbTcpTransaction(uint8_t fc, uint16_t addr, uint16_t qty, uint16_t* out) {
  uint8_t pdu[5] = { fc, (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(qty >> 8), (uint8_t)qty };
  uint8_t resp[16];
  int n = mbTcpExchange(pdu, 5, resp, sizeof(resp));
  if (n < 0) return false;
  uint8_t need = (fc == 1 || fc == 2) ? (qty + 7) / 8 : qty * 2;
  if (n < 2 + need || resp[1] != need) { mbTcpSetErr("byte count mismatch"); return false; }
  if (fc == 1 || fc == 2) out[0] = resp[2] & 0x01;
  else for (uint16_t i = 0; i < qty && i < 2; i++) out[i] = (resp[2 + 2 * i] << 8) | resp[3 + 2 * i];
  mbTcpOkCount++;
  return true;
}

// Write a single coil (FC05)
bool mbTcpWriteCoil(uint16_t addr, bool on) {
  uint8_t pdu[5] = { 0x05, (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(on ? 0xFF : 0x00), 0x00 };
  uint8_t resp[8];
  return mbTcpExchange(pdu, 5, resp, sizeof(resp)) >= 0;
}
// Write a single holding register (FC06)
bool mbTcpWriteReg(uint16_t addr, uint16_t v) {
  uint8_t pdu[5] = { 0x06, (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(v >> 8), (uint8_t)v };
  uint8_t resp[8];
  return mbTcpExchange(pdu, 5, resp, sizeof(resp)) >= 0;
}
// Write two holding registers (FC16) - for FLOAT32
bool mbTcpWriteRegs2(uint16_t addr, uint16_t hi, uint16_t lo) {
  uint8_t pdu[10] = { 0x10, (uint8_t)(addr >> 8), (uint8_t)addr, 0x00, 0x02, 0x04,
                      (uint8_t)(hi >> 8), (uint8_t)hi, (uint8_t)(lo >> 8), (uint8_t)lo };
  uint8_t resp[8];
  return mbTcpExchange(pdu, 10, resp, sizeof(resp)) >= 0;
}

// Read a block: fc1/2 -> one bit per out[] entry, fc3/4 -> one register per out[] entry.
#define MBTCP_MAX_BITS  240
#define MBTCP_MAX_REGS  100
bool mbTcpReadBlock(uint8_t fc, uint16_t addr, uint16_t qty, uint16_t* out) {
  uint8_t pdu[5] = { fc, (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(qty >> 8), (uint8_t)qty };
  static uint8_t resp[256];
  int n = mbTcpExchange(pdu, 5, resp, sizeof(resp));
  if (n < 0) return false;
  int need = (fc == 1 || fc == 2) ? (qty + 7) / 8 : qty * 2;
  if (n < 2 + need || resp[1] != need) { mbTcpSetErr("byte count mismatch"); return false; }
  if (fc == 1 || fc == 2) for (uint16_t i = 0; i < qty; i++) out[i] = (resp[2 + i / 8] >> (i % 8)) & 1;
  else for (uint16_t i = 0; i < qty; i++) out[i] = (resp[2 + 2 * i] << 8) | resp[3 + 2 * i];
  mbTcpOkCount++;
  return true;
}

// One Modbus TCP poll cycle over all enabled registers.
//  - neighbouring registers with the same function code are read in ONE request
//    (e.g. 20 coils = 1 request instead of 20), like a SCADA/HMI does;
//  - network I/O runs WITHOUT holding rs485Mutex, so the web pages never wait on the PLC;
//  - a register / the PLC is only shown as failed after MBTCP_FAIL_LIMIT bad cycles in a row,
//    so one slow reply doesn't make the dashboard flicker.
#define MBTCP_FAIL_LIMIT 3
#define MBTCP_MAX_GAP    8          // merge registers up to 8 addresses apart into one request
static uint8_t mbRegFails[MAX_RS485_REGS];
static uint8_t mbCycleFails = 0;
struct MbDef { uint8_t fc, dt; uint16_t addr; bool en; };

void mbTcpPollCycle() {
  static MbDef def[MAX_RS485_REGS];
  static uint16_t v0[MAX_RS485_REGS], v1[MAX_RS485_REGS];
  static bool got[MAX_RS485_REGS], done[MAX_RS485_REGS];
  static uint16_t blk[MBTCP_MAX_BITS];
  int n;
  // 1) snapshot the register list (microseconds under the lock)
  if (xSemaphoreTake(rs485Mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  n = rs485RegCount;
  for (int i = 0; i < n; i++) def[i] = { rs485Regs[i].fc, rs485Regs[i].dt, rs485Regs[i].addr, rs485Regs[i].en };
  xSemaphoreGive(rs485Mutex);
  for (int i = 0; i < n; i++) { got[i] = false; done[i] = !def[i].en; }

  // 2) talk to the PLC
  bool linkLost = false;
  for (int i = 0; i < n && !linkLost; i++) {
    if (done[i]) continue;
    uint8_t fc = def[i].fc;
    bool bits = (fc == 1 || fc == 2);
    int maxSpan = bits ? MBTCP_MAX_BITS : MBTCP_MAX_REGS;
    auto endOf = [&](int k) { return (uint16_t)(def[k].addr + ((!bits && def[k].dt == 2) ? 1 : 0)); };
    uint16_t lo = def[i].addr, hi = endOf(i);
    int mem[MAX_RS485_REGS], mc = 0; mem[mc++] = i;
    bool grew = true;                      // grow the block with same-fc registers close by
    while (grew) {
      grew = false;
      for (int k = i + 1; k < n; k++) {
        if (done[k] || def[k].fc != fc) continue;
        bool in = false; for (int m = 0; m < mc; m++) if (mem[m] == k) { in = true; break; }
        if (in) continue;
        uint16_t a = def[k].addr, e = endOf(k);
        if (a + MBTCP_MAX_GAP < lo || a > hi + MBTCP_MAX_GAP) continue;
        uint16_t nlo = min(lo, a), nhi = max(hi, e);
        if (nhi - nlo + 1 > maxSpan) continue;
        lo = nlo; hi = nhi; mem[mc++] = k; grew = true;
      }
    }
    bool ok = mbTcpReadBlock(fc, lo, hi - lo + 1, blk);
    if (ok) {
      for (int m = 0; m < mc; m++) { int k = mem[m]; v0[k] = blk[def[k].addr - lo]; v1[k] = (def[k].addr - lo + 1 <= hi - lo) ? blk[def[k].addr - lo + 1] : 0; got[k] = true; }
    } else if (mbTcpClient.connected()) {
      // PLC answered with an exception (e.g. a gap address it doesn't have): fall back to one by one
      for (int m = 0; m < mc; m++) {
        int k = mem[m]; uint16_t t[2] = {0, 0};
        if (mbTcpReadBlock(fc, def[k].addr, (!bits && def[k].dt == 2) ? 2 : 1, t)) { v0[k] = t[0]; v1[k] = t[1]; got[k] = true; }
        else if (!mbTcpClient.connected()) { linkLost = true; break; }
      }
    } else linkLost = true;                // timeout / connection dropped: retry next cycle
    for (int m = 0; m < mc; m++) done[mem[m]] = true;
  }

  // 3) store results (and pick what to publish) under the lock, only if the list didn't change meanwhile
  struct Pub { char topic[64]; char val[24]; };
  static Pub pubs[MAX_RS485_REGS]; int np = 0;
  bool anyGot = false;
  if (xSemaphoreTake(rs485Mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
  bool same = (rs485RegCount == n);
  for (int i = 0; same && i < n; i++) same = rs485Regs[i].fc == def[i].fc && rs485Regs[i].addr == def[i].addr && rs485Regs[i].dt == def[i].dt;
  if (same) {
    unsigned long now = millis();
    for (int i = 0; i < n; i++) {
      RS485Register& r = rs485Regs[i];
      if (!def[i].en) continue;
      if (!got[i]) {
        if (mbRegFails[i] < 255) mbRegFails[i]++;
        if (mbRegFails[i] >= MBTCP_FAIL_LIMIT) r.lastOk = false;
        continue;
      }
      anyGot = true; mbRegFails[i] = 0; r.lastOk = true;
      float prevVal = r.lastVal; bool hadVal = r.haveVal;
      if (r.fc == 1 || r.fc == 2) r.lastVal = (float)v0[i];
      else switch (r.dt) {
        case 1: r.lastVal = (float)(int16_t)v0[i]; break;
        case 2: { uint32_t raw = ((uint32_t)v0[i] << 16) | v1[i]; memcpy(&r.lastVal, &raw, sizeof(float)); break; }
        case 3: r.lastVal = (float)v0[i] * r.scale; break;
        default: r.lastVal = (float)v0[i]; break;
      }
      if (!hadVal || r.lastVal != prevVal) { r.changedMs = now; r.haveVal = true; }
      if (r.toMqtt && strlen(r.mqttTopic) > 0 && now - r.lastPublishMs >= (unsigned long)mbTcpConfig.publishInterval) {
        strlcpy(pubs[np].topic, r.mqttTopic, sizeof(pubs[np].topic));
        if (r.fc == 1 || r.fc == 2 || r.dt == 0 || r.dt == 1) snprintf(pubs[np].val, sizeof(pubs[np].val), "%d", (int)r.lastVal);
        else snprintf(pubs[np].val, sizeof(pubs[np].val), "%.6g", r.lastVal);
        np++; r.lastPublishMs = now;
      }
    }
    bool allGot = true; for (int i = 0; i < n; i++) if (def[i].en && !got[i]) allGot = false;
    if (anyGot && allGot) mbTcpLastErr[0] = 0;   // everything read fine: clear the old error
    if (anyGot) mbCycleFails = 0; else if (mbCycleFails < 255) mbCycleFails++;
    rs485DeviceConnected = anyGot || (rs485DeviceConnected && mbCycleFails < MBTCP_FAIL_LIMIT);
  }
  xSemaphoreGive(rs485Mutex);

  // 4) hand the values to loop() for MQTT (dropped if the queue is full, e.g. while the broker is down)
  for (int i = 0; i < np && mbPubQueue; i++) { MbPub m; strlcpy(m.topic, pubs[i].topic, sizeof(m.topic)); strlcpy(m.val, pubs[i].val, sizeof(m.val)); xQueueSend(mbPubQueue, &m, 0); }
}

// Poll a single register over Modbus TCP. Mirrors pollRS485Reg()'s decode
// and MQTT-publish logic exactly, so the register list and dashboard behave
// identically regardless of which transport is active.
bool pollModbusTCPReg(int idx) {
  if (idx < 0 || idx >= rs485RegCount) return false;
  RS485Register& r = rs485Regs[idx];
  if (!r.en) return false;
  uint16_t regs[2] = {0, 0};
  uint16_t qty = (r.dt == 2) ? 2 : 1;
  bool ok = mbTcpTransaction(r.fc, r.addr, qty, regs);
  if (ok) {
    r.lastOk = true;
    if (r.fc == 1 || r.fc == 2) {
      r.lastVal = (float)regs[0];
    } else {
      switch (r.dt) {
        case 0: r.lastVal = (float)(uint16_t)regs[0]; break;
        case 1: r.lastVal = (float)(int16_t)regs[0]; break;
        case 2: { uint32_t raw = ((uint32_t)regs[0] << 16) | regs[1]; memcpy(&r.lastVal, &raw, sizeof(float)); break; }
        case 3: r.lastVal = (float)(uint16_t)regs[0] * r.scale; break;
        default: r.lastVal = (float)regs[0]; break;
      }
    }
    unsigned long now = millis();
    if (r.toMqtt && strlen(r.mqttTopic) > 0 && mqtt.connected() &&
        (now - r.lastPublishMs >= (unsigned long)mbTcpConfig.publishInterval)) {
      char vs[32];
      if (r.fc == 1 || r.fc == 2) snprintf(vs, sizeof(vs), "%d", (int)r.lastVal);
      else if (r.dt == 0 || r.dt == 1) snprintf(vs, sizeof(vs), "%d", (int)r.lastVal);
      else snprintf(vs, sizeof(vs), "%.6g", r.lastVal);
      mqtt.publish(r.mqttTopic, vs);
      r.lastPublishMs = now;
    }
    return true;
  } else {
    r.lastOk = false;
    Serial.printf("ModbusTCP err reg[%d] fc=%d addr=%d\n", idx, r.fc, r.addr);
    return false;
  }
}

// ===========================================================
//  TAGOIO  -  register values -> https://api.tago.io/data (WiFi)
// ===========================================================
// TagoIO variable name from the register name: lowercase letters, digits, '_'   ("AnalogIn1" -> "analog_in1", "TL_1_R" -> "tl_1_r")
static void tagoVarName(const char* in, char* out, size_t n) {
  size_t k = 0;
  for (size_t i = 0; in[i] && k + 1 < n; i++) {
    char c = in[i];
    if (c >= 'A' && c <= 'Z' && i > 0 && in[i-1] >= 'a' && in[i-1] <= 'z' && k + 2 < n) out[k++] = '_';   // AnalogIn1 -> analog_in1
    if (c >= 'A' && c <= 'Z') c += 32;
    out[k++] = ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? c : '_';
  }
  out[k] = 0;
}
static void tagoSetStatus(const String& s) { strlcpy(tagoStatus, s.c_str(), sizeof(tagoStatus)); }

void tagoTask(void* pv) {
  static float sentVal[MAX_RS485_REGS];
  static bool  sentOk[MAX_RS485_REGS];
  static char  sentSig[MAX_RS485_REGS * 6 + 4];   // register list signature: reset memory when it changes
  int8_t sentOnline = -1;
  unsigned long lastSend = 0, lastAnalog = 0, lastFull = 0;
  WiFiClientSecure tls; tls.setInsecure();        // TLS without certificate check (lab use)
  vTaskDelay(pdMS_TO_TICKS(5000));
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!tagoCfg.enabled) { tagoSetStatus("off"); sentOnline = -1; continue; }
    if (!tagoCfg.token[0]) { tagoSetStatus("off (no token)"); continue; }
    if (WiFi.status() != WL_CONNECTED) { tagoSetStatus("waiting for WiFi (internet)"); continue; }
    bool polling = mbTcpConfig.enabled || rs485Config.enabled;
    if (!polling) { tagoSetStatus("waiting - Modbus polling is off"); continue; }
    unsigned long now = millis();
    if (now - lastSend < TAGO_MIN_GAP_MS) continue;
    bool fullDue   = (now - lastFull >= TAGO_FULL_MS) || lastFull == 0;
    bool analogDue = now - lastAnalog >= TAGO_ANALOG_MS;

    // snapshot under the lock, build the JSON outside it
    String body = "["; int count = 0; bool anyAnalog = false;
    char sig[sizeof(sentSig)] = "";
    if (xSemaphoreTake(rs485Mutex, pdMS_TO_TICKS(200)) != pdTRUE) continue;
    bool online = rs485DeviceConnected;
    int n = rs485RegCount;
    for (int i = 0; i < n; i++) { char t[8]; snprintf(t, sizeof t, "%d%d%c,", rs485Regs[i].fc, rs485Regs[i].addr % 100, rs485Regs[i].toTago ? 't' : '-'); strlcat(sig, t, sizeof sig); }
    if (strcmp(sig, sentSig)) { strlcpy(sentSig, sig, sizeof sentSig); for (int i = 0; i < MAX_RS485_REGS; i++) sentOk[i] = false; fullDue = true; }
    if (sentOnline != (online ? 1 : 0) || fullDue) {
      body += String("{\"variable\":\"plc_online\",\"value\":") + (online ? "true" : "false") + "}"; count++;
    }
    struct { int i; float v; } chg[MAX_RS485_REGS]; int nc = 0;
    if (online) for (int i = 0; i < n; i++) {
      RS485Register& r = rs485Regs[i];
      if (!r.en || !r.lastOk || !r.toTago) continue;
      bool isBit = (r.fc == 1 || r.fc == 2);
      bool changed = !sentOk[i] || sentVal[i] != r.lastVal;
      if (!fullDue && (!changed || (!isBit && !analogDue))) continue;
      char var[40]; tagoVarName(r.tagoVar[0] ? r.tagoVar : r.name, var, sizeof var);
      char vs[24];
      const char* txt = isBit ? (r.lastVal != 0 ? r.onTxt : r.offTxt) : "";
      if (isBit && txt[0]) snprintf(vs, sizeof vs, "\"%s\"", txt);          // e.g. "DETECT" / "CLEAR"
      else if (isBit || r.dt == 0 || r.dt == 1) snprintf(vs, sizeof vs, "%d", (int)r.lastVal); else snprintf(vs, sizeof vs, "%.6g", r.lastVal);
      if (count) body += ",";
      body += "{\"variable\":\""; body += var; body += "\",\"value\":"; body += vs; body += "}";
      count++; chg[nc++] = { i, r.lastVal }; if (!isBit) anyAnalog = true;
    }
    xSemaphoreGive(rs485Mutex);
    body += "]";
    if (!count) { tagoSetStatus("OK, no changes (" + String(tagoSentCount) + " sent)"); continue; }

    HTTPClient http;
    http.setReuse(true); http.setTimeout(8000);
    int code = -1;
    if (http.begin(tls, tagoCfg.url)) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Device-Token", tagoCfg.token);
      http.addHeader("Authorization", tagoCfg.token);
      code = http.POST(body);
      http.end();
    }
    lastSend = millis();
    if (code >= 200 && code < 300) {
      for (int k = 0; k < nc; k++) { sentVal[chg[k].i] = chg[k].v; sentOk[chg[k].i] = true; }
      sentOnline = online ? 1 : 0;
      if (anyAnalog || fullDue) lastAnalog = lastSend;
      if (fullDue) lastFull = lastSend;
      tagoSentCount += count;
      tagoSetStatus("OK, sent " + String(tagoSentCount) + " values");
    } else if (code == 401 || code == 403) {
      tagoSetStatus("rejected (" + String(code) + ") - check the device token");
      vTaskDelay(pdMS_TO_TICKS(10000));
    } else {
      tagoSetStatus("failed (" + (code < 0 ? HTTPClient::errorToString(code) : String(code)) + ")");
      tls.stop(); vTaskDelay(pdMS_TO_TICKS(3000));
    }
  }
}

bool requireAuth();
// Dashboard "Process Metrics" cards: [{name, src, unit}, ...] stored as /tiles.json (null = page defaults)
void handleGetTiles() {
  if (!requireAuth()) return;
  if (!LittleFS.exists("/tiles.json")) { server.sendHeader(F("Cache-Control"), F("no-cache")); server.send(200, F("application/json"), F("null")); return; }
  File f = LittleFS.open("/tiles.json", "r");
  if (!f) { server.send(200, F("application/json"), F("null")); return; }
  server.sendHeader(F("Cache-Control"), F("no-cache"));
  server.streamFile(f, "application/json"); f.close();
}
void handleSaveTiles() {
  if (!requireAuth()) return;
  String body = server.arg("plain");
  if (body.length() > 4096) { server.send(400, F("text/plain"), F("Too long")); return; }
  DynamicJsonDocument doc(6144);
  if (deserializeJson(doc, body) || !doc.is<JsonArray>()) { server.send(400, F("text/plain"), F("JSON error")); return; }
  if (doc.size() > 12) { server.send(400, F("text/plain"), F("Max 12 cards")); return; }
  for (JsonObject o : doc.as<JsonArray>()) {
    const char* src = o["src"] | "";
    if (!src[0] || strlen(src) > 48 || strlen(o["name"] | "") > 24) { server.send(400, F("text/plain"), F("Invalid card")); return; }
  }
  File f = LittleFS.open("/tiles.json", "w");
  if (!f) { server.send(500, F("text/plain"), F("Cannot write file")); return; }
  serializeJson(doc, f); f.close();
  server.send(200, F("text/plain"), F("OK"));
}
void handleSaveEffFormula() {
  if (!requireAuth()) return;
  String f = server.arg("formula"); f.trim();
  if (f == EFF_DEFAULT) f = "";
  if (f.length() >= sizeof(effFormula)) { server.send(400, F("text/plain"), F("Formula too long (max 159 characters)")); return; }
  unsigned long rt, dt; liveRunDown(rt, dt);
  double v; const char* err = nullptr;
  if (f.length() && !effEval(f.c_str(), rt, dt, v, &err, true)) { server.send(400, F("text/plain"), String("Formula error: ") + err); return; }
  strlcpy(effFormula, f.c_str(), sizeof(effFormula));
  saveConfig();
  server.send(200, F("text/plain"), F("OK"));
}
void handleGetKpi() {
  if (!requireAuth()) return;
  DynamicJsonDocument doc(4096);
  JsonObject roles = doc.createNestedObject("roles");
  for (int k = 0; k < KPI_ROLES; k++) {
    JsonObject o = roles.createNestedObject(KPI_KEYS[k]);
    o["src"] = kpiCfg[k].src; o["on"] = kpiCfg[k].on; o["now"] = kpiRead(kpiCfg[k]);
  }
  JsonArray regs = doc.createNestedArray("regs");          // on/off registers that can be picked
  for (int i = 0; i < rs485RegCount; i++) if (rs485Regs[i].fc == 1 || rs485Regs[i].fc == 2) regs.add(rs485Regs[i].name);
  JsonArray di = doc.createNestedArray("di");
  for (int i = 0; i < 8; i++) di.add(diCurrentlyOn[i]);
  doc["machine"] = MACHINE_LABELS[machineStateIdx]; doc["cycleActive"] = cycleActive;
  doc["polling"] = mbTcpConfig.enabled || rs485Config.enabled;
  doc["formula"] = effFormula[0] ? effFormula : EFF_DEFAULT; doc["formulaCustom"] = effFormula[0] != 0;
  String out; serializeJson(doc, out);
  server.sendHeader(F("Cache-Control"), F("no-cache"));
  server.send(200, F("application/json"), out);
}
void handleSaveKpi() {
  if (!requireAuth()) return;
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, F("text/plain"), F("JSON error")); return; }
  JsonObject roles = doc["roles"];
  if (roles.isNull()) { server.send(400, F("text/plain"), F("missing roles")); return; }
  for (int k = 0; k < KPI_ROLES; k++) {
    JsonObject o = roles[KPI_KEYS[k]]; if (o.isNull()) continue;
    const char* src = o["src"] | "";
    bool okSrc = !src[0] || (!strncmp(src, "di", 2) && atoi(src + 2) >= 1 && atoi(src + 2) <= 8) || (!strncmp(src, "mb:", 3) && strlen(src) > 3);
    if (!okSrc || strlen(src) >= sizeof(kpiCfg[k].src)) { server.send(400, F("text/plain"), String("Invalid source for ") + KPI_KEYS[k]); return; }
    strlcpy(kpiCfg[k].src, src, sizeof(kpiCfg[k].src)); kpiCfg[k].on = o["on"] | true; kpiPrev[k] = -1;
  }
  cycleActive = false;
  saveConfig();
  server.send(200, F("text/plain"), F("OK"));
}
void handleGetTago() {
  if (!requireAuth()) return;
  DynamicJsonDocument doc(512);
  doc["enabled"] = tagoCfg.enabled; doc["hasToken"] = tagoCfg.token[0] != 0;
  doc["url"] = tagoCfg.url; doc["status"] = tagoStatus; doc["sent"] = tagoSentCount;
  doc["wifi"] = WiFi.status() == WL_CONNECTED;
  String out; serializeJson(doc, out);
  server.send(200, F("application/json"), out);
}
void handleSaveTago() {
  if (!requireAuth()) return;
  if (server.hasArg("enabled")) tagoCfg.enabled = server.arg("enabled") == "1";
  if (server.arg("clear") == "1") tagoCfg.token[0] = 0;
  else { String t = server.arg("token"); t.trim(); if (t.length()) strlcpy(tagoCfg.token, t.c_str(), sizeof(tagoCfg.token)); }
  String u = server.arg("url"); u.trim();
  if (u.length()) {
    if (!u.startsWith("https://")) { server.send(400, F("text/plain"), F("URL must start with https://")); return; }
    strlcpy(tagoCfg.url, u.c_str(), sizeof(tagoCfg.url));
  }
  saveConfig();
  server.send(200, F("text/plain"), F("OK"));
}

// RS485 task: normal polling at user-configured pollIntervalMs,
//             fast connectivity check at RS485_CHECK_INTERVAL_MS.
// Also drives Modbus TCP polling when mbTcpConfig.enabled -- same task,
// same register list, just a different poll function underneath.
void rs485Task(void* pv) {
  vTaskDelay(pdMS_TO_TICKS(3000));
  int pollIdx=0;
  unsigned long lastPollMs=0;
  for(;;){
    vTaskDelay(pdMS_TO_TICKS(50));          // 50 ms base tick
    bool useTcp = mbTcpConfig.enabled;

    // Modbus TCP: a LAN round trip takes a few ms, so read EVERY enabled register once per poll interval
    if(useTcp){
      if(rs485RegCount==0) continue;
      unsigned long nowT=millis();
      if(nowT-lastPollMs < (unsigned long)max(mbTcpConfig.pollIntervalMs,50)) continue;
      lastPollMs=nowT;
      mbTcpPollCycle();
      continue;
    }
    if(!useTcp && !rs485Config.enabled) continue;   // neither transport enabled -- idle, safe to toggle anytime
    if(rs485RegCount==0) continue;
    if(!useTcp && !rs485Initialized) continue;
    unsigned long now=millis();
    int curPollMs = useTcp ? mbTcpConfig.pollIntervalMs : rs485Config.pollIntervalMs;

    // Fast device-connected check (always runs at RS485_CHECK_INTERVAL_MS)
    if(now-rs485LastCheckMs>=RS485_CHECK_INTERVAL_MS){
      rs485LastCheckMs=now;
      // Find first enabled register and quick-poll it
      for(int i=0;i<rs485RegCount;i++){
        if(!rs485Regs[i].en) continue;
        if(xSemaphoreTake(rs485Mutex,pdMS_TO_TICKS(200))==pdTRUE){
          bool ok=useTcp?pollModbusTCPReg(i):pollRS485Reg(i);
          rs485DeviceConnected=ok;
          if(!ok){
            bool anyOk=false;
            for(int k=0;k<rs485RegCount;k++) if(rs485Regs[k].en&&rs485Regs[k].lastOk) anyOk=true;
            rs485DeviceConnected=anyOk;
          }
          xSemaphoreGive(rs485Mutex);
        }
        break;  // only first enabled register for the connectivity check
      }
    }

    // Normal polling at user-configured rate
    if(now-lastPollMs < (unsigned long)max(curPollMs,50)) continue;
    lastPollMs=now;
    int tries=0;
    do{ pollIdx=(pollIdx+1)%rs485RegCount; tries++; }
    while(!rs485Regs[pollIdx].en && tries<rs485RegCount);
    if(!rs485Regs[pollIdx].en) continue;
    if(xSemaphoreTake(rs485Mutex,pdMS_TO_TICKS(300))==pdTRUE){
      bool ok=useTcp?pollModbusTCPReg(pollIdx):pollRS485Reg(pollIdx);
      // Update global connected flag
      if(ok) rs485DeviceConnected=true;
      else {
        bool anyOk=false;
        for(int k=0;k<rs485RegCount;k++) if(rs485Regs[k].en&&rs485Regs[k].lastOk) anyOk=true;
        rs485DeviceConnected=anyOk;
      }
      xSemaphoreGive(rs485Mutex);
    }
  }
}

// ===========================================================
//  USER MANAGEMENT + AUTH
// ===========================================================
void loadUsers() {
  userCount=0;
  if(!LittleFS.exists(F("/users.json"))) return;
  File f=LittleFS.open(F("/users.json"),"r"); if(!f) return;
  StaticJsonDocument<768> doc;
  if(deserializeJson(doc,f)){f.close();return;} f.close();
  for(JsonObject u:doc.as<JsonArray>()){
    if(userCount>=10) break;
    strlcpy(users[userCount].username,u["username"]|"",30);
    strlcpy(users[userCount].password,u["password"]|"",30);
    userCount++;
  }
}
bool checkCredentials(const String& u,const String& p){
  for(int i=0;i<userCount;i++) if(strcmp(users[i].username,u.c_str())==0&&strcmp(users[i].password,p.c_str())==0) return true;
  return false;
}
void generateToken(char* buf){ randomSeed(micros()^millis()); for(int i=0;i<32;i++){int r=random(16);buf[i]=r<10?'0'+r:'a'+r-10;} buf[32]='\0'; }
bool isAuthenticated(){
  if(activeToken[0]=='\0') return false;
  String c=server.header("Cookie"); int idx=c.indexOf(F("session=")); if(idx==-1) return false;
  String v=c.substring(idx+8); int e=v.indexOf(';'); if(e!=-1) v=v.substring(0,e);
  v.trim(); return strcmp(activeToken,v.c_str())==0;
}
bool requireAuth(){ if(!isAuthenticated()){server.sendHeader(F("Location"),F("/login"));server.send(302);return false;} return true; }
void serveStatic(const char* path,const char* mime,bool noCache=false){
  File f=LittleFS.open(path,"r"); if(!f){server.send(404,F("text/plain"),F("Not Found"));return;}
  if(!noCache) server.sendHeader(F("Cache-Control"),F("public, max-age=86400"));
  server.streamFile(f,mime); f.close();
}

// ===========================================================
//  MQTT TRANSPORT
// ===========================================================
void applyMqttTransport(){
  mqtt.disconnect(); wsBridge.setConnected(false); wsClient.disconnect();
  if(mqttCfg.host[0]=='\0') return;                              // no broker configured yet
  if(strcmp(mqttCfg.mode,"cloud")==0){
    mqtt.setClient(wsBridge);
    if(strcmp(mqttCfg.proto,"wss")==0) wsClient.beginSSL(mqttCfg.host,mqttCfg.port,mqttCfg.path,"","mqtt");
    else wsClient.begin(mqttCfg.host,mqttCfg.port,mqttCfg.path,"mqtt");
    wsClient.onEvent(wsEventHandler); wsClient.setReconnectInterval(5000);
  } else {
    if(strcmp(mqttCfg.proto,"ssl")==0){mqttTlsClient.setInsecure();mqtt.setClient(mqttTlsClient);}
    else mqtt.setClient(tcpClient);
  }
  mqtt.setServer(mqttCfg.host,mqttCfg.port); mqtt.setCallback(mqttCallback); mqtt.setKeepAlive(15);
  lastMqttRetry=0;
}
void wsEventHandler(WStype_t type,uint8_t* payload,size_t length){
  switch(type){
    case WStype_CONNECTED:    wsBridge.setConnected(true);  break;
    case WStype_DISCONNECTED: wsBridge.setConnected(false); break;
    case WStype_BIN:          wsBridge.push(payload,length); break;
    default: break;
  }
}
void mqttCallback(char* topic,byte* payload,unsigned int length){
  if(strcmp(topic,MACHINE_STATE_TOPIC)==0){
    char msg[12]={0}; memcpy(msg,payload,min(length,(unsigned int)10));
    if(strcmp(msg,"RUNNING")==0) setMachineState(1);
    else if(strcmp(msg,"STOPPED")==0) setMachineState(2);
    return;
  }
  if(strcmp(topic,"esp32/sd/cmd")==0){
    StaticJsonDocument<128> cmd;
    if(!deserializeJson(cmd,payload,length)){
      const char* c=cmd["cmd"]|"";
      if(strcmp(c,"getInfo")==0) publishSDInfo();
      else if(strcmp(c,"snapshot")==0) sdLogRow();
    }
    return;
  }
  if(strcmp(topic,COUNTER_RESET_TOPIC)==0){
    // Payload is the reset target as plain text, e.g. "all", "di0"..."di7",
    // "do0".."do7", "rejects", "runtime", or "cycle" — same vocabulary as the HTTP endpoint.
    char t[16]={0}; memcpy(t,payload,min(length,(unsigned int)15));
    resetMetricsByTarget(String(t));
    return;
  }
  char msg[4]={0}; memcpy(msg,payload,min(length,(unsigned int)3));
  for(int i=0;i<8;i++)
    if(strlen(outputConfig[i].topic)>0&&strcmp(topic,outputConfig[i].topic)==0){ setOutput(i,strcmp(msg,"1")==0); break; }
}
void reconnectMQTT(){
  if(mqttCfg.host[0]=='\0') return;                              // no broker configured — skip silently
  if(strcmp(mqttCfg.mode,"cloud")==0&&!wsBridge.connected()) return;
  if(mqtt.connect(deviceName)){
    for(int i=0;i<8;i++) if(strlen(outputConfig[i].topic)>0) mqtt.subscribe(outputConfig[i].topic);
    mqtt.subscribe(MACHINE_STATE_TOPIC); mqtt.subscribe("esp32/sd/cmd"); mqtt.subscribe(COUNTER_RESET_TOPIC);;
  }
}

// ===========================================================
//  HTTP LOGIN / LOGOUT
// ===========================================================
void handleLogin(){
  if(server.method()==HTTP_GET){serveStatic("/login.html","text/html",true);}
  else{
    if(checkCredentials(server.arg("username"),server.arg("password"))){
      generateToken(activeToken);
      server.sendHeader(F("Set-Cookie"),String(F("session="))+activeToken+F("; Path=/"));
      server.sendHeader(F("Location"),F("/")); server.send(302);
    } else { server.sendHeader(F("Location"),F("/login?error=1")); server.send(302); }
  }
}
void handleLogout(){ activeToken[0]='\0'; server.sendHeader(F("Set-Cookie"),F("session=; Path=/; Max-Age=0")); server.sendHeader(F("Location"),F("/login")); server.send(302); }

// ===========================================================
//  API HANDLERS
// ===========================================================
void handleToggle(){
  if(!requireAuth()) return;
  int ch=server.arg("ch").toInt();
  if(ch<0||ch>7){server.send(400,F("text/plain"),F("Invalid channel"));return;}
  bool ns=!((outputState>>ch)&1);
  setOutput(ch,ns);
  server.send(200,F("text/plain"),ns?"1":"0");
}

void handleStatus(){
  if(!requireAuth()) return;
  char buf[400]; int pos=0;
  pos+=snprintf(buf+pos,sizeof(buf)-pos,"{\"inputs\":[");
  for(int i=0;i<8;i++){buf[pos++]=diCurrentlyOn[i]?'1':'0';if(i<7)buf[pos++]=',';}
  pos+=snprintf(buf+pos,sizeof(buf)-pos,"],\"outputs\":[");
  for(int i=0;i<8;i++){buf[pos++]=(outputState&(1<<i))?'1':'0';if(i<7)buf[pos++]=',';}
  bool wifiOk=WiFi.status()==WL_CONNECTED;
  String ip=wifiOk?WiFi.localIP().toString():(eth_connected?ETH.localIP().toString():String("-"));
  const char* net=(wifiOk&&eth_connected)?"WiFi + Ethernet":(wifiOk?"WiFi":(eth_connected?"Ethernet":"None"));
  snprintf(buf+pos,sizeof(buf)-pos,"],\"ip\":\"%s\",\"ethIp\":\"%s\",\"network\":\"%s\",\"machineState\":\"%s\"}",
    ip.c_str(),eth_connected?ETH.localIP().toString().c_str():"",net,MACHINE_LABELS[machineStateIdx]);
  server.sendHeader(F("Cache-Control"),F("no-cache"));
  server.send(200,F("application/json"),buf);
}

void handleGetMetrics(){
  if(!requireAuth()) return;
  unsigned long now=millis(),rt=runtimeMs,dt=downtimeMs;
  if(machineStateIdx==1) rt+=now-lastMetricTickMs;
  else if(machineStateIdx==2) dt+=now-lastMetricTickMs;
  DynamicJsonDocument doc(1536);
  JsonArray trig=doc.createNestedArray("triggers"), ont=doc.createNestedArray("onTimes");
  JsonArray doTrig=doc.createNestedArray("doTriggers"), doOnt=doc.createNestedArray("doOnTimes");
  for(int i=0;i<8;i++){
    trig.add(triggerCount[i]);ont.add(getOnTime(i));
    doTrig.add(doTriggerCount[i]); doOnt.add(getDoOnTime(i));
  }
  doc["cycleTime"]=lastCycleTimeMs; doc["rejects"]=rejectCount; doc["runtime"]=rt; doc["downtime"]=dt;
  doc["parts"]=partCount; doc["good"]=partCount>rejectCount?partCount-rejectCount:0;
  { double ev; const char* ee=nullptr;
    if(effEval(effFormula,rt,dt,ev,&ee,false)) doc["eff"]=ev; else { doc["eff"]=nullptr; doc["effErr"]=ee; } }
  doc["formula"]=effFormula[0]?effFormula:EFF_DEFAULT; doc["formulaCustom"]=effFormula[0]!=0;
  String out; serializeJson(doc,out);
  server.sendHeader(F("Cache-Control"),F("no-cache"));
  server.send(200,F("application/json"),out);
}

// Shared reset logic used by both the HTTP /resetmetrics endpoint and the
// MQTT COUNTER_RESET_TOPIC handler, so both entry points stay in sync.
// Valid targets: "all", "di0".."di7" (per-DI counter), "do0".."do7" (per-DO counter),
//                "rejects", "runtime", "cycle"
void resetMetricsByTarget(const String& target){
  unsigned long now=millis();
  if(target=="all"){
    for(int i=0;i<8;i++){
      triggerCount[i]=0;totalOnTimeMs[i]=0;if(diCurrentlyOn[i])lastOnStartMs[i]=now;
      doTriggerCount[i]=0;doTotalOnTimeMs[i]=0;if((outputState>>i)&1)doLastOnStartMs[i]=now;
    }
    rejectCount=0;runtimeMs=0;downtimeMs=0;lastCycleTimeMs=0;partCount=0;lastMetricTickMs=now;
  } else if(target.startsWith("do")){
    int ch=target.substring(2).toInt();
    if(ch>=0&&ch<8){doTriggerCount[ch]=0;doTotalOnTimeMs[ch]=0;if((outputState>>ch)&1)doLastOnStartMs[ch]=now;}
  } else if(target.startsWith("di")){
    int ch=target.substring(2).toInt();
    if(ch>=0&&ch<8){triggerCount[ch]=0;totalOnTimeMs[ch]=0;if(diCurrentlyOn[ch])lastOnStartMs[ch]=now;}
  } else if(target=="rejects") rejectCount=0;
  else if(target=="runtime"){ runtimeMs=0;downtimeMs=0;lastMetricTickMs=now; }
  else if(target=="cycle") lastCycleTimeMs=0;
  else if(target=="parts") partCount=0;
  saveMetrics();
}

void handleResetMetrics(){
  if(!requireAuth()) return;
  resetMetricsByTarget(server.arg("target"));
  server.send(200,F("text/plain"),F("OK"));
}


void handleGetConfig(){
  if(!requireAuth()) return;
  StaticJsonDocument<1024> doc;
  doc["deviceName"]=deviceName; doc["mqttMode"]=mqttCfg.mode;
  doc["mqttProto"]=mqttCfg.proto; doc["mqttHost"]=mqttCfg.host;
  doc["mqttPort"]=mqttCfg.port; doc["mqttPath"]=mqttCfg.path;
  doc["mqttConnected"]=mqtt.connected();
  for(int i=0;i<8;i++){
    doc["inputs"][i]=inputConfig[i].topic; doc["outputs"][i]=outputConfig[i].topic;
    doc["inputPubMs"][i]=inputConfig[i].pubIntervalMs;
    doc["outputPubMs"][i]=outputConfig[i].pubIntervalMs;
  }
  String out; serializeJson(doc,out);
  server.send(200,F("application/json"),out);
}

void handleGetRS485(){
  if(!requireAuth()) return;
  StaticJsonDocument<192> doc;
  doc["baud"]=rs485Config.baud; doc["parity"]=rs485Config.parity;
  doc["stopBits"]=rs485Config.stopBits; doc["dataBits"]=rs485Config.dataBits;
  doc["addr"]=rs485Config.addr;
  doc["pollIntervalMs"]=rs485Config.pollIntervalMs;
  doc["publishInterval"]=rs485Config.publishInterval;
  doc["deviceConnected"]=rs485DeviceConnected;
  doc["regCount"]=rs485RegCount;
  doc["enabled"]=rs485Config.enabled;
  String out; serializeJson(doc,out);
  server.send(200,F("application/json"),out);
}

void handleToggleRS485(){
  if(!requireAuth()) return;
  rs485Config.enabled = !rs485Config.enabled;
  if(rs485Config.enabled && mbTcpConfig.enabled){ mbTcpConfig.enabled=false; mbTcpStopSafe(); }  // transports are mutually exclusive
  if(!rs485Config.enabled) rs485DeviceConnected=false;   // clear stale "connected" state when turning off
  saveConfig();
  server.send(200,F("text/plain"),rs485Config.enabled?"1":"0");
}

void handleGetModbusTCP(){
  if(!requireAuth()) return;
  DynamicJsonDocument doc(768);
  doc["ip"]=mbTcpConfig.ip; doc["port"]=mbTcpConfig.port; doc["unitId"]=mbTcpConfig.unitId;
  doc["pollIntervalMs"]=mbTcpConfig.pollIntervalMs;
  doc["publishInterval"]=mbTcpConfig.publishInterval;
  doc["deviceConnected"]=rs485DeviceConnected;
  doc["regCount"]=rs485RegCount;
  doc["enabled"]=mbTcpConfig.enabled;
  doc["okCount"]=mbTcpOkCount; doc["errCount"]=mbTcpErrCount; doc["lastErr"]=mbTcpLastErr;
  JsonObject e=doc.createNestedObject("eth");
  e["link"]=eth_link; e["hasIp"]=eth_connected;
  e["curIp"]=ETH.localIP().toString();
  e["dhcp"]=ethCfg.dhcp; e["ip"]=ethCfg.ip; e["mask"]=ethCfg.mask; e["gw"]=ethCfg.gw;
  doc["wifiIp"]=WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():String("");
  String out; serializeJson(doc,out);
  server.send(200,F("application/json"),out);
}

void handleToggleModbusTCP(){
  if(!requireAuth()) return;
  mbTcpConfig.enabled = !mbTcpConfig.enabled;
  if(mbTcpConfig.enabled) rs485Config.enabled=false;   // transports are mutually exclusive
  if(!mbTcpConfig.enabled){ rs485DeviceConnected=false; mbTcpStopSafe(); }
  saveConfig();
  server.send(200,F("text/plain"),mbTcpConfig.enabled?"1":"0");
}

void handleSaveModbusTCP(){
  if(!requireAuth()) return;
  String ip = server.arg("ip");
  if(ip.length()>0) strlcpy(mbTcpConfig.ip, ip.c_str(), sizeof(mbTcpConfig.ip));
  if(server.arg("port").length()>0)   mbTcpConfig.port   = constrain(server.arg("port").toInt(),1,65535);
  if(server.arg("unitId").length()>0) mbTcpConfig.unitId = constrain(server.arg("unitId").toInt(),0,255);
  mbTcpConfig.publishInterval = constrain(server.arg("publishInterval").toInt(),100,60000);
  mbTcpConfig.pollIntervalMs  = constrain(server.arg("pollInterval").toInt(),50,60000);
  saveConfig();
  mbTcpStopSafe();   // force reconnect with new settings on next poll
  server.send(200,F("text/plain"),F("OK"));
}

// Test the PLC connection with the settings typed in the form (or the saved ones), without saving.
void handleTestModbusTCP(){
  if(!requireAuth()) return;
  if(!mbSockLock(3000)){ server.send(200,F("application/json"),F("{\"ok\":false,\"msg\":\"Modbus busy, try again.\"}")); return; }
  ModbusTCPConfig saved=mbTcpConfig;
  if(server.arg("ip").length())     strlcpy(mbTcpConfig.ip, server.arg("ip").c_str(), sizeof(mbTcpConfig.ip));
  if(server.arg("port").length())   mbTcpConfig.port   = constrain(server.arg("port").toInt(),1,65535);
  if(server.arg("unitId").length()) mbTcpConfig.unitId = constrain(server.arg("unitId").toInt(),0,255);
  String msg; bool ok=false;
  if(xSemaphoreTake(rs485Mutex,pdMS_TO_TICKS(1500))==pdTRUE){
    mbTcpClient.stop();
    if(!eth_link) msg="No Ethernet link - check the cable from the Waveshare to the switch/PLC.";
    else if(!mbTcpEnsureConnected()) msg=String("Cannot connect to ")+mbTcpConfig.ip+":"+mbTcpConfig.port+" - check the PLC IP and that its Modbus TCP server is running.";
    else {
      int idx=-1; for(int i=0;i<rs485RegCount;i++) if(rs485Regs[i].en){idx=i;break;}
      if(idx<0){ ok=true; msg="Connected to the PLC. Add a register below to read data."; }
      else {
        RS485Register& r=rs485Regs[idx]; uint16_t v[2]={0,0};
        if(mbTcpTransaction(r.fc,r.addr,r.dt==2?2:1,v)){ ok=true; msg=String("OK - read '")+r.name+"' = "+v[0]; }
        else msg=String("Connected, but reading '")+r.name+"' failed: "+mbTcpLastErr;
      }
    }
    mbTcpClient.stop();
    xSemaphoreGive(rs485Mutex);
  } else msg="Modbus busy, try again.";
  mbTcpConfig=saved;
  mbSockUnlock();
  DynamicJsonDocument doc(512); doc["ok"]=ok; doc["msg"]=msg;
  String out; serializeJson(doc,out);
  server.send(200,F("application/json"),out);
}

// Ethernet settings take effect after a restart.
void handleSaveEthernet(){
  if(!requireAuth()) return;
  IPAddress a;
  String ip=server.arg("ip"), mask=server.arg("mask"), gw=server.arg("gw");
  ip.trim(); mask.trim(); gw.trim();
  bool dhcp=server.arg("dhcp")=="1";
  if(!dhcp){
    if(!a.fromString(ip))   {server.send(400,F("text/plain"),F("Invalid IP address"));return;}
    if(!a.fromString(mask)) {server.send(400,F("text/plain"),F("Invalid subnet mask"));return;}
    if(gw.length() && !a.fromString(gw)) {server.send(400,F("text/plain"),F("Invalid gateway"));return;}
  }
  ethCfg.dhcp=dhcp;
  strlcpy(ethCfg.ip,ip.c_str(),sizeof(ethCfg.ip));
  strlcpy(ethCfg.mask,mask.c_str(),sizeof(ethCfg.mask));
  strlcpy(ethCfg.gw,gw.c_str(),sizeof(ethCfg.gw));
  saveConfig();
  server.send(200,F("text/plain"),F("Saved - restarting"));
  restartPending=true; restartAtMs=millis()+1500;
}

void handleGetHealth(){
  if(!requireAuth()) return;
  StaticJsonDocument<160> doc;
  doc["temp"]=temperatureRead(); doc["freeHeap"]=(uint32_t)ESP.getFreeHeap();
  doc["rssi"]=eth_connected?0:(int)WiFi.RSSI(); doc["cpuFreq"]=getCpuFrequencyMhz(); doc["uptime"]=millis();
  String out; serializeJson(doc,out);
  server.sendHeader(F("Cache-Control"),F("no-cache"));
  server.send(200,F("application/json"),out);
}

void handleSave(){
  if(!requireAuth()) return;
  String mode=server.arg("mqttMode"),proto=server.arg("mqttProto"),
         host=server.arg("mqttHost"),port=server.arg("mqttPort"),path=server.arg("mqttPath");
  bool mqttChanged=false;
  if(mode.length()>0){
    mqttChanged=(strcmp(mode.c_str(),mqttCfg.mode)!=0||strcmp(proto.c_str(),mqttCfg.proto)!=0||
                 (host.length()>0&&strcmp(host.c_str(),mqttCfg.host)!=0)||(port.length()>0&&port.toInt()!=mqttCfg.port));
    strlcpy(mqttCfg.mode,mode.c_str(),sizeof(mqttCfg.mode));
    strlcpy(mqttCfg.proto,proto.c_str(),sizeof(mqttCfg.proto));
    if(host.length()>0) strlcpy(mqttCfg.host,host.c_str(),sizeof(mqttCfg.host));
    if(port.length()>0) mqttCfg.port=port.toInt();
    if(path.length()>0) strlcpy(mqttCfg.path,path.c_str(),sizeof(mqttCfg.path));
    else if(strcmp(mqttCfg.mode,"local")==0) mqttCfg.path[0]='\0';
  }
  for(int i=0;i<8;i++){
    strlcpy(inputConfig[i].topic,  server.arg("in" +String(i)).c_str(),50);
    strlcpy(outputConfig[i].topic, server.arg("out"+String(i)).c_str(),50);
    String inPubMs  = server.arg("inPubMs"  +String(i));
    String outPubMs = server.arg("outPubMs" +String(i));
    if(inPubMs.length()>0)  inputConfig[i].pubIntervalMs  = (uint32_t)constrain(inPubMs.toInt(),0,60000);
    if(outPubMs.length()>0) outputConfig[i].pubIntervalMs = (uint32_t)constrain(outPubMs.toInt(),0,60000);
  }
  saveConfig();
  if(mqttChanged) applyMqttTransport();
  server.send(200,F("text/plain"),F("OK"));
}

void handleSaveRS(){
  if(!requireAuth()) return;
  rs485Config.baud           = server.arg("baud").toInt();
  rs485Config.stopBits       = server.arg("stopBits").toInt();
  rs485Config.dataBits       = server.arg("dataBits").toInt();
  rs485Config.publishInterval= constrain(server.arg("publishInterval").toInt(),100,60000);
  rs485Config.pollIntervalMs = constrain(server.arg("pollInterval").toInt(),50,60000);
  strlcpy(rs485Config.parity,server.arg("parity").c_str(),sizeof(rs485Config.parity));
  strlcpy(rs485Config.addr,  server.arg("addr").c_str(),  sizeof(rs485Config.addr));
  saveConfig();
  if(xSemaphoreTake(rs485Mutex,pdMS_TO_TICKS(500))==pdTRUE){ initRS485(); xSemaphoreGive(rs485Mutex); }
  else initRS485();
  server.send(200,F("text/plain"),F("OK"));
}

void handleGetRS485Regs(){
  if(!requireAuth()) return;
  String json="[";
  if(xSemaphoreTake(rs485Mutex,pdMS_TO_TICKS(200))==pdTRUE){
    for(int i=0;i<rs485RegCount;i++){
      RS485Register& r=rs485Regs[i];
      if(i>0) json+=",";
      char vs[32]; if(r.lastOk) snprintf(vs,sizeof(vs),"%.6g",r.lastVal); else strcpy(vs,"null");
      char te[80]={0}; int tp=0;
      for(int k=0;r.mqttTopic[k]&&tp<78;k++){if(r.mqttTopic[k]=='"')te[tp++]='\\'; te[tp++]=r.mqttTopic[k];}
      char tmp[460];
      snprintf(tmp,sizeof(tmp),"{\"name\":\"%s\",\"fc\":%d,\"addr\":%d,\"dt\":%d,\"scale\":%.6g"
        ",\"mqttTopic\":\"%s\",\"wr\":%s,\"en\":%s,\"on\":\"%s\",\"off\":\"%s\",\"mqtt\":%s,\"tago\":%s,\"tv\":\"%s\",\"since\":%ld,\"ok\":%s,\"val\":%s}",
        r.name,r.fc,r.addr,r.dt,r.scale,te,r.wr?"true":"false",r.en?"true":"false",
        r.onTxt,r.offTxt,r.toMqtt?"true":"false",r.toTago?"true":"false",r.tagoVar,r.haveVal?(long)((millis()-r.changedMs)/1000):-1L,r.lastOk?"true":"false",vs);
      json+=tmp;
    }
    xSemaphoreGive(rs485Mutex);
  } else { server.send(503,F("text/plain"),F("busy")); return; }
  json+="]";
  server.sendHeader(F("Cache-Control"),F("no-cache"));
  server.send(200,F("application/json"),json);
}

void handleSaveRS485Regs(){
  if(!requireAuth()) return;
  String body=server.arg("plain");
  if(!body.length()){server.send(400,F("text/plain"),F("Empty body"));return;}
  DynamicJsonDocument doc(12288);
  if(deserializeJson(doc,body)){server.send(400,F("text/plain"),F("JSON error"));return;}
  if(xSemaphoreTake(rs485Mutex,pdMS_TO_TICKS(500))==pdTRUE){
    rs485RegCount=0;
    for(JsonObject obj:doc.as<JsonArray>()){
      if(rs485RegCount>=MAX_RS485_REGS) break;
      RS485Register& r=rs485Regs[rs485RegCount];
      strlcpy(r.name,     obj["name"]     |"Reg",sizeof(r.name));
      strlcpy(r.mqttTopic,obj["mqttTopic"]|"",   sizeof(r.mqttTopic));
      r.fc=obj["fc"]|3; r.addr=obj["addr"]|0; r.dt=obj["dt"]|0;
      r.scale=obj["scale"]|1.0f; r.wr=obj["wr"]|false; r.en=obj["en"]|true;
      regTextCopy(r.onTxt, obj["on"]|"", sizeof(r.onTxt)); regTextCopy(r.offTxt, obj["off"]|"", sizeof(r.offTxt));
    r.toMqtt = obj["mqtt"] | true; r.toTago = obj["tago"] | true;
    regTextCopy(r.tagoVar, obj["tv"]|"", sizeof(r.tagoVar));
      r.lastOk=false; r.lastVal=0.0f; r.lastPublishMs=0; r.haveVal=false; r.changedMs=0;
      rs485RegCount++;
    }
    xSemaphoreGive(rs485Mutex);
  }
  saveRS485Regs();
  server.send(200,F("text/plain"),F("OK"));
}

void handleWriteRS485Reg(){
  if(!requireAuth()) return;
  if(mbTcpConfig.enabled){                         // Modbus TCP is the active transport
    int idx=server.arg("idx").toInt(); float val=server.arg("val").toFloat();
    if(idx<0||idx>=rs485RegCount){server.send(400,F("text/plain"),F("Invalid index"));return;}
    RS485Register& r=rs485Regs[idx];
    if(!r.wr){server.send(400,F("text/plain"),F("Read-only register"));return;}
    if(r.fc!=1&&r.fc!=3){server.send(400,F("text/plain"),F("Write only for FC01/FC03"));return;}
    bool ok=false;
    if(xSemaphoreTake(rs485Mutex,pdMS_TO_TICKS(500))==pdTRUE){
      if(r.fc==1) ok=mbTcpWriteCoil(r.addr,val!=0);
      else switch(r.dt){
        case 1: ok=mbTcpWriteReg(r.addr,(uint16_t)(int16_t)val); break;
        case 2:{float fv=val;uint32_t raw;memcpy(&raw,&fv,sizeof(float));ok=mbTcpWriteRegs2(r.addr,(uint16_t)(raw>>16),(uint16_t)(raw&0xFFFF));break;}
        case 3: ok=mbTcpWriteReg(r.addr,(uint16_t)(val/(r.scale!=0?r.scale:1.0f))); break;
        default: ok=mbTcpWriteReg(r.addr,(uint16_t)val); break;
      }
      xSemaphoreGive(rs485Mutex);
    } else {server.send(503,F("text/plain"),F("Modbus busy"));return;}
    if(ok) server.send(200,F("text/plain"),F("OK"));
    else server.send(500,F("text/plain"),String("Modbus TCP: ")+mbTcpLastErr);
    return;
  }
  if(!rs485Initialized){server.send(503,F("text/plain"),F("RS485 not ready"));return;}
  int idx=server.arg("idx").toInt(); float val=server.arg("val").toFloat();
  if(idx<0||idx>=rs485RegCount){server.send(400,F("text/plain"),F("Invalid index"));return;}
  RS485Register& r=rs485Regs[idx];
  if(!r.wr){server.send(400,F("text/plain"),F("Read-only register"));return;}
  if(r.fc!=1&&r.fc!=3){server.send(400,F("text/plain"),F("Write only for FC01/FC03"));return;}
  uint8_t result=ModbusMaster::ku8MBInvalidCRC;
  if(xSemaphoreTake(rs485Mutex,pdMS_TO_TICKS(500))==pdTRUE){
    if(r.fc==1){ result=modbusNode.writeSingleCoil(r.addr,(uint16_t)(val!=0?0xFF00:0x0000)); }
    else {
      switch(r.dt){
        case 0: result=modbusNode.writeSingleRegister(r.addr,(uint16_t)val); break;
        case 1: result=modbusNode.writeSingleRegister(r.addr,(uint16_t)(int16_t)val); break;
        case 3: result=modbusNode.writeSingleRegister(r.addr,(uint16_t)(val/(r.scale!=0?r.scale:1.0f))); break;
        case 2:{float fv=val;uint32_t raw;memcpy(&raw,&fv,sizeof(float));
          modbusNode.setTransmitBuffer(0,(uint16_t)(raw>>16));modbusNode.setTransmitBuffer(1,(uint16_t)(raw&0xFFFF));
          result=modbusNode.writeMultipleRegisters(r.addr,2);break;}
        default: result=modbusNode.writeSingleRegister(r.addr,(uint16_t)val); break;
      }
    }
    xSemaphoreGive(rs485Mutex);
  } else {server.send(503,F("text/plain"),F("RS485 busy"));return;}
  if(result==ModbusMaster::ku8MBSuccess) server.send(200,F("text/plain"),F("OK"));
  else {char m[32];snprintf(m,sizeof(m),"Modbus error 0x%02X",result);server.send(500,F("text/plain"),m);}
}

// ── DO Rules API ──────────────────────────────────────────────────
void handleGetDoRules(){
  if(!requireAuth()) return;
  StaticJsonDocument<4096> doc;
  JsonArray arr=doc.to<JsonArray>();
  for(int i=0;i<doRuleCount;i++){
    DoRule& rule=doRules[i]; JsonObject obj=arr.createNestedObject();
    obj["doIdx"]=rule.doIdx; obj["enabled"]=rule.enabled;
    JsonArray ca=obj.createNestedArray("conds");
    for(int c=0;c<rule.condCount;c++){
      JsonObject co=ca.createNestedObject();
      co["diIdx"]=rule.conds[c].diIdx; co["state"]=rule.conds[c].state; co["op"]=rule.conds[c].op;
    }
  }
  String out; serializeJson(doc,out);
  server.sendHeader(F("Cache-Control"),F("no-cache"));
  server.send(200,F("application/json"),out);
}

void handleSaveDoRules(){
  if(!requireAuth()) return;
  String body=server.arg("plain");
  if(!body.length()){server.send(400,F("text/plain"),F("Empty body"));return;}
  StaticJsonDocument<4096> doc;
  if(deserializeJson(doc,body)){server.send(400,F("text/plain"),F("JSON error"));return;}
  doRuleCount=0;
  for(JsonObject obj:doc.as<JsonArray>()){
    if(doRuleCount>=MAX_DO_RULES) break;
    DoRule& rule=doRules[doRuleCount];
    rule.doIdx=obj["doIdx"]|0; rule.enabled=obj["enabled"]|false; rule.condCount=0;
    for(JsonObject c:obj["conds"].as<JsonArray>()){
      if(rule.condCount>=MAX_RULE_COND) break;
      rule.conds[rule.condCount].diIdx=c["diIdx"]|0;
      rule.conds[rule.condCount].state=c["state"]|1;
      rule.conds[rule.condCount].op   =c["op"]   |0;
      rule.condCount++;
    }
    doRuleCount++;
  }
  saveDoRules();
  server.send(200,F("text/plain"),F("OK"));
}

void handleSetCustomEff(){
  if(!requireAuth()) return;
  String v=server.arg("eff"); customEffPct=v.length()>0?v.toFloat():-1.0f;
  server.send(200,F("text/plain"),F("OK"));
}

// ── Telegram API ──────────────────────────────────────────────────
void handleGetTelegramConfig(){
  if(!requireAuth()) return;
  StaticJsonDocument<256> doc;
  bool ht=strlen(tgConfig.botToken)>8;
  doc["tokenMasked"]=ht?String(tgConfig.botToken).substring(0,8)+"........":"";
  doc["hasToken"]=ht; doc["chatId"]=tgConfig.chatId;
  doc["enabled"]=tgConfig.enabled; doc["rejectThreshold"]=tgConfig.rejectThreshold;
  doc["botReady"]=tgInitialized;
  String out; serializeJson(doc,out);
  server.send(200,F("application/json"),out);
}
void handleSaveTelegramConfig(){
  if(!requireAuth()) return;
  String token=server.arg("botToken"),chatId=server.arg("chatId"),en=server.arg("enabled"),thresh=server.arg("rejectThreshold");
  if(token.length()>8&&!token.endsWith("........")) strlcpy(tgConfig.botToken,token.c_str(),sizeof(tgConfig.botToken));
  if(chatId.length()>0) strlcpy(tgConfig.chatId,chatId.c_str(),sizeof(tgConfig.chatId));
  tgConfig.enabled=(en=="1"||en=="true");
  if(thresh.length()>0) tgConfig.rejectThreshold=constrain(thresh.toInt(),0,9999);
  saveTelegramConfig(); initTelegramBot();
  server.send(200,F("text/plain"),F("OK"));
}
void handleTelegramTest(){
  if(!requireAuth()) return;
  if(!tgInitialized||!tgConfig.enabled||!strlen(tgConfig.chatId)){server.send(400,F("text/plain"),F("Bot not configured or disabled"));return;}
  sdLogRow(); saveConfig(); saveTelegramConfig(); saveMetrics();
  tgSend(F("*Test OK!* All configs saved. Bot is live."));
  server.send(200,F("text/plain"),F("Queued — check Telegram in ~3 seconds"));
}

// ── SD Card API ───────────────────────────────────────────────────
void handleSDInfo(){
  if(!requireAuth()) return;
  StaticJsonDocument<256> doc;
  doc["mounted"]=sdMounted; doc["logEnabled"]=sdLogEnabled;
  doc["logIntervalSec"]=sdLogIntervalSec; doc["logRotateIntervalSec"]=logRotateIntervalSec;
  String clf=String(currentLogFile); if(clf.startsWith("/")) clf=clf.substring(1);
  doc["currentLogFile"]=clf;
  if(sdMounted){
    uint8_t t=SD_MMC.cardType();
    doc["cardType"]=(t==CARD_MMC)?"MMC":(t==CARD_SD)?"SDSC":(t==CARD_SDHC)?"SDHC":"?";
    doc["totalMB"]=(uint32_t)(SD_MMC.cardSize()/(1024*1024));
    doc["usedMB"] =(uint32_t)(SD_MMC.usedBytes()/(1024*1024));
    doc["freeMB"] =(uint32_t)((SD_MMC.cardSize()-SD_MMC.usedBytes())/(1024*1024));
  }
  String out; serializeJson(doc,out);
  server.sendHeader(F("Cache-Control"),F("no-cache"));
  server.send(200,F("application/json"),out);
}
void handleSDFiles(){
  if(!requireAuth()) return;
  if(!sdMounted){server.send(503,F("application/json"),F("{\"error\":\"SD not mounted\"}"));return;}
  String json="["; File root=SD_MMC.open("/"); bool first=true;
  if(root){ File entry=root.openNextFile();
    while(entry){
      if(!entry.isDirectory()){
        if(!first) json+=','; String nm=String(entry.name()); if(nm.startsWith("/")) nm=nm.substring(1);
        char ts[20]="--"; time_t lw=entry.getLastWrite();
        if(lw>1577836800L){struct tm* t=localtime(&lw);strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M",t);}
        json+="{\"name\":\""+nm+"\",\"size\":"+String(entry.size())+",\"lastWrite\":\""+String(ts)+"\"}";
        first=false;
      }
      entry=root.openNextFile();
    }
    root.close();
  }
  json+=']';
  server.sendHeader(F("Cache-Control"),F("no-cache"));
  server.send(200,F("application/json"),json);
}
void handleSDRead(){
  if(!requireAuth()) return;
  if(!sdMounted){server.send(503,F("text/plain"),F("SD not mounted"));return;}
  String fname="/"+server.arg("file");
  File f=SD_MMC.open(fname.c_str(),FILE_READ); if(!f){server.send(404,F("text/plain"),F("Not found"));return;}
  size_t toRead=min((size_t)f.size(),(size_t)16384); String content=""; content.reserve(toRead+1);
  for(size_t i=0;i<toRead;i++) content+=(char)f.read(); f.close();
  server.sendHeader(F("Cache-Control"),F("no-cache"));
  server.send(200,F("text/plain"),content);
}
void handleSDDownload(){
  if(!requireAuth()) return;
  if(!sdMounted){server.send(503,F("text/plain"),F("SD not mounted"));return;}
  String fname="/"+server.arg("file");
  File f=SD_MMC.open(fname.c_str(),FILE_READ); if(!f){server.send(404,F("text/plain"),F("Not found"));return;}
  server.sendHeader(F("Content-Disposition"),"attachment; filename=\""+server.arg("file")+"\"");
  server.sendHeader(F("Cache-Control"),F("no-cache"));
  server.streamFile(f,F("text/csv")); f.close();
}
void handleSDDelete(){
  if(!requireAuth()) return;
  if(!sdMounted){server.send(503,F("text/plain"),F("SD not mounted"));return;}
  String fname="/"+server.arg("file");
  server.send(SD_MMC.remove(fname.c_str())?200:500,F("text/plain"),SD_MMC.remove(fname.c_str())?F("Deleted"):F("Failed"));
}
void handleSDClearAllLogs(){
  if(!requireAuth()) return;
  if(!sdMounted){server.send(503,F("text/plain"),F("SD not mounted"));return;}
  int deleted=0; File root=SD_MMC.open("/");
  if(root){ File entry=root.openNextFile();
    while(entry){
      if(!entry.isDirectory()){ String nm="/"+String(entry.name());
        if(strcmp(nm.c_str(),currentLogFile)!=0){ entry.close(); SD_MMC.remove(nm.c_str()); deleted++; entry=root.openNextFile(); continue; }
      }
      entry=root.openNextFile();
    }
    root.close();
  }
  char msg[40]; snprintf(msg,sizeof(msg),"Deleted %d file(s)",deleted);
  server.send(200,F("text/plain"),msg);
}
void handleSDSnapshot(){ if(!requireAuth()) return; sdLogRow(); server.send(200,F("text/plain"),F("Snapshot written")); }
void handleSDLogConfig(){
  if(!requireAuth()) return;
  if(server.method()==HTTP_POST){
    String iv=server.arg("interval"),en=server.arg("enabled"),ri=server.arg("rotateInterval");
    if(iv.length()>0){int v=iv.toInt();if(v>=5&&v<=3600)sdLogIntervalSec=v;}
    if(en.length()>0) sdLogEnabled=(en=="1"||en=="true");
    if(ri.length()>0){long v=ri.toInt();if(v>=60&&v<=86400)logRotateIntervalSec=(unsigned long)v;}
    saveConfig(); server.send(200,F("text/plain"),F("OK"));
  } else {
    StaticJsonDocument<96> doc; doc["interval"]=sdLogIntervalSec; doc["enabled"]=sdLogEnabled; doc["rotateInterval"]=logRotateIntervalSec;
    String out; serializeJson(doc,out); server.send(200,F("application/json"),out);
  }
}

// ===========================================================
//  NETWORK EVENT
// ===========================================================
// Keep internet traffic (MQTT cloud, Telegram, NTP) on WiFi when the Ethernet side has no gateway.
void preferWifiRoute(){
  if(WiFi.status()==WL_CONNECTED && strlen(ethCfg.gw)==0 && !ethCfg.dhcp) WiFi.STA.setDefault();
}

void WiFiEvent(WiFiEvent_t event){
  switch(event){
    case ARDUINO_EVENT_ETH_START:        ETH.setHostname("esp32-gui"); break;
    case ARDUINO_EVENT_ETH_CONNECTED:    eth_link=true;  Serial.println(F("ETH link up")); break;
    case ARDUINO_EVENT_ETH_DISCONNECTED: eth_link=false; eth_connected=false; Serial.println(F("ETH link down")); break;
    case ARDUINO_EVENT_ETH_GOT_IP:       eth_connected=true; Serial.print(F("ETH IP: ")); Serial.println(ETH.localIP()); preferWifiRoute(); break;
    case ARDUINO_EVENT_ETH_LOST_IP:      eth_connected=false; break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:  Serial.print(F("WiFi IP: ")); Serial.println(WiFi.localIP()); preferWifiRoute(); break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: Serial.println(F("WiFi disconnected")); break;
    default: break;
  }
}

void startEthernet(){
  SPI.begin(ETH_PIN_SCLK, ETH_PIN_MISO, ETH_PIN_MOSI);
  if(!ETH.begin(ETH_PHY_W5500, 1, ETH_PIN_CS, ETH_PIN_INT, ETH_PIN_RST, SPI)){ Serial.println(F("ETH (W5500) init failed")); return; }
  if(!ethCfg.dhcp){
    IPAddress ip, mask, gw(0,0,0,0);
    if(!ip.fromString(ethCfg.ip)) ip=IPAddress(192,168,250,10);
    if(!mask.fromString(ethCfg.mask)) mask=IPAddress(255,255,255,0);
    if(strlen(ethCfg.gw)) gw.fromString(ethCfg.gw);
    ETH.config(ip, gw, mask, IPAddress(8,8,8,8));
  }
}

// ===========================================================
//  SETUP
// ===========================================================
void setup(){
  Serial.begin(115200);
  Serial.println(F("Booting..."));

  for(int i=0;i<8;i++){ pinMode(inputPins[i],INPUT_PULLUP); lastRawState[i]=HIGH; debounceTimer[i]=0; }
  
Wire.begin(42,41);
  // TCA9554 needs its 8 pins explicitly configured as outputs first (register 0x03, 0=output)
  Wire.beginTransmission(EXIO_ADDR);
  Wire.write(0x03);
  Wire.write(0x00);
  Wire.endTransmission();
  writeOutputs();                      // initialise all DO OFF           // initialise all DO OFF

  pinMode(RS485_DE,OUTPUT); digitalWrite(RS485_DE,LOW);

  if(!LittleFS.begin(true)) Serial.println(F("LittleFS failed!"));
  else Serial.println(F("LittleFS OK"));

  kpiDefaults(); loadConfig(); loadUsers(); loadTelegramConfig(); loadMetrics(); loadRS485Regs(); loadDoRules();

  // SD Card
  pinMode(NET_SCS,OUTPUT); digitalWrite(NET_SCS,HIGH);
  if(!SD_MMC.setPins(SD_CLK,SD_CMD,SD_D0)) Serial.println(F("SD pin map failed"));
  else if(!SD_MMC.begin("/sdcard",true,false,10000)) Serial.println(F("SD mount failed"));
  else { sdMounted=true; Serial.println(F("SD mounted OK")); }

  // Network
  WiFi.onEvent(WiFiEvent);
  startEthernet();                                  // PLC network (fixed IP by default)
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid,password); WiFi.setSleep(false);  // internet / office network
  unsigned long cs=millis();
  while(WiFi.status()!=WL_CONNECTED){ if(millis()-cs>15000) break; Serial.print(F(".")); delay(200); }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED) { Serial.print(F("WiFi IP: ")); Serial.println(WiFi.localIP()); preferWifiRoute(); }
  else Serial.println(F("WiFi not connected"));
  if(eth_connected) { Serial.print(F("ETH IP: ")); Serial.println(ETH.localIP()); }
  else Serial.println(eth_link ? F("ETH link up, no IP yet") : F("ETH: no cable"));

  lastMetricTickMs=millis(); lastMetricAccMs=millis(); lastMetricsSaveMs=millis();

  configTime(8*3600,0,"pool.ntp.org","time.cloudflare.com");
  struct tm ntpCheck; for(int i=0;i<5;i++) if(getLocalTime(&ntpCheck,1000)) break;

  if(sdMounted) initLogFile();
  initRS485();
  applyMqttTransport();

  // FreeRTOS primitives
  ioMutex    = xSemaphoreCreateMutex();
  rs485Mutex = xSemaphoreCreateMutex();
  mbTcpSock  = xSemaphoreCreateRecursiveMutex();
  mbPubQueue = xQueueCreate(64, sizeof(MbPub));
  tgOutQueue = xQueueCreate(6,256);
  initTelegramBot();
  xTaskCreatePinnedToCore(telegramTask,"TGTask",  12288,NULL,1,&tgTaskHandle,   0);
  xTaskCreatePinnedToCore(rs485Task,  "RS485Task", 6144,NULL,1,&rs485TaskHandle,0);
  xTaskCreatePinnedToCore(tagoTask,   "TagoTask", 16384,NULL,1,NULL,0);   // slow HTTPS never delays polling
  

  // HTTP routes
  const char* headerKeys[]={"Cookie"};
  server.collectHeaders(headerKeys,1);

  server.on("/login",  handleLogin);
  server.on("/logout", handleLogout);

  // Static assets
  server.on("/style.css",      HTTP_GET,[](){serveStatic("/style.css",     "text/css");});
  server.on("/shrdc_logo.png", HTTP_GET,[](){serveStatic("/shrdc_logo.png","image/png");});
  server.on("/msf_logo.png",   HTTP_GET,[](){serveStatic("/msf_logo.png",  "image/png");});
  server.on("/variables.json", HTTP_GET,[](){if(!requireAuth()) return; serveStatic("/variables.json","application/json");});

  // Pages
  server.on("/",         HTTP_GET,[](){if(!requireAuth()) return; serveStatic("/index.html",  "text/html",true);});
  server.on("/iot",      HTTP_GET,[](){if(!requireAuth()) return; serveStatic("/iot.html",    "text/html",true);});
  server.on("/modbus",   HTTP_GET,[](){if(!requireAuth()) return; serveStatic("/modbus.html", "text/html",true);});
  server.on("/tagoio",   HTTP_GET,[](){if(!requireAuth()) return; serveStatic("/tagoio.html", "text/html",true);});
  server.on("/sdcard",   HTTP_GET,[](){if(!requireAuth()) return; serveStatic("/sdcard.html", "text/html",true);});
  server.on("/telegram", HTTP_GET,[](){if(!requireAuth()) return; serveStatic("/telegram.html","text/html",true);});

  // Legacy redirects
  server.on("/mqtt",       HTTP_GET,[](){server.sendHeader("Location","/iot"); server.send(301);});
  server.on("/rs485",      HTTP_GET,[](){server.sendHeader("Location","/iot"); server.send(301);});
  server.on("/monitoring", HTTP_GET,[](){server.sendHeader("Location","/");   server.send(301);});

  // API
  server.on("/toggle",        handleToggle);
  server.on("/status",        handleStatus);
  server.on("/getConfig",     handleGetConfig);
  server.on("/getRS485",      handleGetRS485);
  server.on("/toggleRS485",   HTTP_POST,handleToggleRS485);
  server.on("/getModbusTCP",    HTTP_GET, handleGetModbusTCP);
  server.on("/toggleModbusTCP", HTTP_POST,handleToggleModbusTCP);
  server.on("/saveModbusTCP",   HTTP_POST,handleSaveModbusTCP);
  server.on("/testModbusTCP",   HTTP_POST,handleTestModbusTCP);
  server.on("/saveEthernet",    HTTP_POST,handleSaveEthernet);
  server.on("/getTago",         HTTP_GET, handleGetTago);
  server.on("/getKpi",          HTTP_GET, handleGetKpi);
  server.on("/getTiles",        HTTP_GET, handleGetTiles);
  server.on("/saveTiles",       HTTP_POST,handleSaveTiles);
  server.on("/signals",         HTTP_GET,[](){if(!requireAuth()) return; serveStatic("/signals.html","text/html",true);});
  server.on("/saveKpi",         HTTP_POST,handleSaveKpi);
  server.on("/saveEffFormula",  HTTP_POST,handleSaveEffFormula);
  server.on("/kpi",             HTTP_GET,[](){if(!requireAuth()) return; serveStatic("/kpi.html",    "text/html",true);});
  server.on("/saveTago",        HTTP_POST,handleSaveTago);
  server.on("/getMetrics",    handleGetMetrics);
  server.on("/getHealth",     handleGetHealth);
  server.on("/save",          HTTP_POST,handleSave);
  server.on("/saveRS",        HTTP_POST,handleSaveRS);
  server.on("/resetMetrics",  HTTP_POST,handleResetMetrics);
  server.on("/getRS485Regs",  HTTP_GET, handleGetRS485Regs);
  server.on("/saveRS485Regs", HTTP_POST,handleSaveRS485Regs);
  server.on("/writeRS485Reg", HTTP_POST,handleWriteRS485Reg);
  server.on("/getDoRules",    HTTP_GET, handleGetDoRules);
  server.on("/saveDoRules",   HTTP_POST,handleSaveDoRules);
  server.on("/setCustomEff",  HTTP_POST,handleSetCustomEff);
  server.on("/sdInfo",         HTTP_GET, handleSDInfo);
  server.on("/sdFiles",        HTTP_GET, handleSDFiles);
  server.on("/sdRead",         HTTP_GET, handleSDRead);
  server.on("/sdDownload",     HTTP_GET, handleSDDownload);
  server.on("/sdDelete",       HTTP_POST,handleSDDelete);
  server.on("/sdSnapshot",     HTTP_POST,handleSDSnapshot);
  server.on("/sdLogConfig",    handleSDLogConfig);
  server.on("/sdClearAllLogs", HTTP_POST,handleSDClearAllLogs);
  server.on("/getTelegramConfig",  HTTP_GET, handleGetTelegramConfig);
  server.on("/saveTelegramConfig", HTTP_POST,handleSaveTelegramConfig);
  server.on("/telegramTest",       HTTP_POST,handleTelegramTest);

  server.begin();
  Serial.println(F("HTTP server started"));
  String ip=eth_connected?ETH.localIP().toString():WiFi.localIP().toString();
  tgSend("*ESP32 Online*\nIP: `"+ip+"`");
}

// ===========================================================
//  LOOP
// ===========================================================
void loop(){
  server.handleClient();
  if(restartPending && (long)(millis()-restartAtMs)>0) ESP.restart();
  if(strcmp(mqttCfg.mode,"cloud")==0) wsClient.loop();
  mqtt.loop();
  // WiFi watchdog: the built-in auto-reconnect sometimes gives up (e.g. after the access point
  // kicks the board off). If WiFi has been down for 20 s, start a fresh connection attempt.
  { static unsigned long wifiDownSince=0, lastWifiKick=0; unsigned long nw=millis();
    if(WiFi.status()==WL_CONNECTED) wifiDownSince=0;
    else { if(!wifiDownSince) wifiDownSince=nw;
           if(nw-wifiDownSince>20000 && nw-lastWifiKick>20000){ lastWifiKick=nw; Serial.println(F("WiFi down 20 s - reconnecting")); WiFi.disconnect(); WiFi.begin(ssid,password); } } }
  if(mbPubQueue){                                   // Modbus values queued by the poll task
    MbPub m; int k=0;
    while(k++<32 && xQueueReceive(mbPubQueue,&m,0)==pdTRUE) if(mqtt.connected()) mqtt.publish(m.topic,m.val);
  }

  if(!mqtt.connected()){
    unsigned long now=millis();
    if(now-lastMqttRetry>mqttRetryInterval){ lastMqttRetry=now; reconnectMQTT(); }
  }

  unsigned long now=millis();

  // Telegram reset flag
  if(tgResetMetricsFlag){
    tgResetMetricsFlag=false;
    resetMetricsByTarget("all");
  }

  // Runtime/Downtime accumulation (100 ms tick)
  if(now-lastMetricAccMs>=100){
    unsigned long elapsed=now-lastMetricTickMs;
    if(machineStateIdx==1) runtimeMs+=elapsed;
    else if(machineStateIdx==2) downtimeMs+=elapsed;
    lastMetricTickMs=now; lastMetricAccMs=now;
  }

  // Periodic metrics save every 5 min
  if(now-lastMetricsSaveMs>=300000UL){ lastMetricsSaveMs=now; saveMetrics(); }

  // SD log rotation
  if(sdMounted&&currentLogFile[0]!='\0')
    if(now-logFileStartMs>=logRotateIntervalSec*1000UL) initLogFile();

  // SD periodic logging (live data only — no read-back from SD)
  if(sdMounted&&sdLogEnabled)
    if(now-lastSdLogMs>=(unsigned long)sdLogIntervalSec*1000UL){ lastSdLogMs=now; sdLogRow(); }

  // Per-topic periodic MQTT heartbeat for DI
  if(mqtt.connected()){
    for(int i=0;i<8;i++){
      if(inputConfig[i].pubIntervalMs>0 && strlen(inputConfig[i].topic)>0){
        if(now-diLastPubMs[i]>=inputConfig[i].pubIntervalMs){
          diLastPubMs[i]=now;
          mqtt.publish(inputConfig[i].topic, diCurrentlyOn[i]?"1":"0");
        }
      }
      if(outputConfig[i].pubIntervalMs>0 && strlen(outputConfig[i].topic)>0){
        if(now-doLastPubMs[i]>=outputConfig[i].pubIntervalMs){
          doLastPubMs[i]=now;
          mqtt.publish(outputConfig[i].topic, ((outputState>>i)&1)?"1":"0");
        }
      }
    }
  }

  // DI scan every 5 ms
  if(now-lastDiScanMs<5) return;
  lastDiScanMs=now;
  bool anyDiChanged=false;

  for(int i=0;i<8;i++){
    int raw=digitalRead(inputPins[i]);
    if(raw!=lastRawState[i]){ lastRawState[i]=raw; debounceTimer[i]=now; }
    if((now-debounceTimer[i])>=DEBOUNCE_MS){
      // Active-LOW opto: LOW = input ON, HIGH = input OFF
      int logical=(raw==LOW)?1:0;
      if(logical!=(int)diCurrentlyOn[i]){
        diCurrentlyOn[i]=(logical==1);
        anyDiChanged=true;
        if(logical==1){
          triggerCount[i]++; lastOnStartMs[i]=now;
          // cycle start/end and rejects: see kpiTick() (KPI page)
        } else { totalOnTimeMs[i]+=now-lastOnStartMs[i]; }
        // machine RUNNING/STOPPED: see kpiTick() (KPI page)
        // On-change MQTT publish for DI
        if(strlen(inputConfig[i].topic)>0&&mqtt.connected())
          mqtt.publish(inputConfig[i].topic,logical?"1":"0");
      }
    }
  }

  // Re-evaluate all DO logic rules whenever any DI changed
  if(anyDiChanged) evaluateDoRules();
  kpiTick();
}
