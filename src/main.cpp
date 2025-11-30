#include <AsyncJson.h>
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>
#include "DeviceConfig.h"
#include "MeshlessNetwork.h"
#include "Logger.h"
#include "WebUI.h"
#if HAS_DHT
#include <DHT.h>
#endif

// ===================== Globals =====================

bool Public_MotionDetected = false;
bool sendAtWait(const String &cmd, const String &expect, int timeoutMs, String *out);

// NOTE: per your request earlier, keep TaskDelay and printTimestampReadable behavior (vTaskDelay + fixed +12600 & localtime)
unsigned long epochStartTime = 0;
char lastOktime[32];

void SetPublicVariablesFromPrefs();
bool enqueueSms(String number, String text, int priority);
NodeCapabilities buildLocalCaps();
void handleMeshEvent(const MeshEventInfo &info);
void EnsurePreferencesFresh();
void ApplyClockFromMs(uint64_t timestampMs, bool broadcastMesh);
String formatTimestampReadable(uint64_t timestampMs);
bool IsValidEpoch(uint64_t ms);
bool matchesLocalNode(const String &target);
void sendMeshStateResponse(uint8_t ttl = 6, const String &requesterMac = "");
void sendMeshKeysResponse(uint8_t ttl = 6, const String &requesterMac = "");
String validateKey(const String &key, const String &value);
bool matchesLocalRequester(const String &req);
extern const int numKeys;
void StartSoftAP();
void restartLocalNodeDelayed(int ms = 200);
struct ConfigKey
{
  String key;
  String title; // caption (fa-IR)
  String type;  // string, int, bool, dropdown, mobile
  String defaultVal;
  String min;
  String max;
  String options; // dropdown options
  bool isSystem;
};
extern ConfigKey defaultKeys[];
int keyIndexByName(const String &k)
{
  for (int i = 0; i < numKeys; ++i)
  {
    if (defaultKeys[i].key == k)
      return i;
  }
  return -1;
}

String formatMacFull(uint64_t mac)
{
  uint8_t bytes[6];
  for (int i = 0; i < 6; ++i)
    bytes[5 - i] = (mac >> (8 * i)) & 0xFF;
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
  return String(buf);
}

String formatMacShort(uint64_t mac)
{
  char buf[5];
  snprintf(buf, sizeof(buf), "%04X", (uint16_t)(mac & 0xFFFF));
  return String(buf);
}

String formatMacCompact(uint64_t mac)
{
  char buf[13];
  snprintf(buf, sizeof(buf), "%012llX", (unsigned long long)mac);
  return String(buf);
}

struct MeshRemoteAlarm
{
  bool pending = false;
  String description;
  bool requiresSms = false;
  bool requiresSiren = false;
  uint32_t expireAtMs = 0;
};

MeshRemoteAlarm g_remoteAlarm;
uint32_t g_lastNetworkAlarmMs = 0;
const uint32_t kNetworkAlarmMinIntervalMs = 5000;

struct RemoteStateEntry
{
  String id;
  String friendly;
  uint64_t ts = 0;
  uint64_t originMac = 0;
  String rawJson;
};

struct RemoteKeysEntry
{
  String id;
  String sid;
  String friendly;
  uint64_t ts = 0;
  uint64_t originMac = 0;
  String rawJson;
};

std::vector<RemoteStateEntry> g_remoteStates;
std::vector<RemoteKeysEntry> g_remoteKeys;
Preferences prefs;
Preferences prefsClock;

// ---- Time formatting (UNCHANGED: +12600 and localtime) ----
void printTimestampReadable(uint64_t timestampMs)
{
  time_t timestampSec = (timestampMs / 1000) + 12600; // UTC+3:30
  struct tm *timeinfo = localtime(&timestampSec);
  strftime(lastOktime, sizeof(lastOktime), "%Y-%m-%d %H:%M:%S", timeinfo);
}

RemoteStateEntry *findRemoteState(const String &sid, uint64_t originMac)
{
  for (auto &s : g_remoteStates)
  {
    if ((sid.length() && s.id.equalsIgnoreCase(sid)) || (originMac && s.originMac == originMac))
      return &s;
  }
  return nullptr;
}

RemoteKeysEntry *findRemoteKeys(const String &sid, uint64_t originMac)
{
  for (auto &k : g_remoteKeys)
  {
    if ((sid.length() && (k.sid.equalsIgnoreCase(sid) || k.id.equalsIgnoreCase(sid))) || (originMac && k.originMac == originMac))
      return &k;
  }
  return nullptr;
}

void pruneRemoteCaches()
{
  // C++11-friendly helper to cap vector size
  const size_t kMaxKeep = 6;
  while (g_remoteStates.size() > kMaxKeep)
    g_remoteStates.erase(g_remoteStates.begin());
  while (g_remoteKeys.size() > kMaxKeep)
    g_remoteKeys.erase(g_remoteKeys.begin());
}

void cacheRemoteKeys(const MeshEventInfo &info);
void cacheRemoteData(const MeshEventInfo &info, const char *kind);

// salam: in func payload STATE_RES ro az node remote migire va cache mikone ta az tarafe web local bebini
void cacheRemoteState(const MeshEventInfo &info)
{
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, info.payload) != DeserializationError::Ok)
  {
    logf(1, "[MESH] STATE_RES parse failed from %s payload=%s\n", formatMacFull(info.originMac).c_str(), info.payload.c_str());
    return;
  }
  String req = doc["r"] | doc["req"] | "";
  if (req.length() && !matchesLocalRequester(req))
  {
    logf(1, "[MESH] STATE_RES ignored (not for me) from %s req=%s\n", formatMacFull(info.originMac).c_str(), req.c_str());
    return;
  }
  String sid = doc["id"] | doc["i"] | formatMacShort(info.originMac);
  String friendly = doc["node"] | info.originNode;
  String raw;
  serializeJson(doc, raw);

  RemoteStateEntry *existing = findRemoteState(sid, info.originMac);
  if (existing)
  {
    existing->ts = info.timestampMs;
    existing->friendly = friendly;
    existing->rawJson = raw;
    existing->originMac = info.originMac;
  }
  else
  {
    RemoteStateEntry ent;
    ent.id = sid;
    ent.friendly = friendly;
    ent.ts = info.timestampMs;
    ent.originMac = info.originMac;
    ent.rawJson = raw;
    g_remoteStates.push_back(ent);
    pruneRemoteCaches();
  }
  logf(1, "[MESH] Cached STATE_RES for %s (%s)\n", friendly.c_str(), sid.c_str());
}


// salam: in handler tamame event haye mesh ro migire va bar asas type ya cache mikone ya response midahad
void handleMeshEvent(const MeshEventInfo &info)
{
  String macFull = formatMacFull(info.originMac);
  String macShort = formatMacShort(info.originMac);
  String originName = info.originNode.length() ? info.originNode : String("Node_") + macShort;
  bool isLocal = (info.originMac == ESP.getEfuseMac());
  String tag = isLocal ? " (me)" : "";
  String labeledName = originName + tag;
  String payloadTrim = info.payload;
  payloadTrim.trim();
  String typeUpper = info.type;
  typeUpper.trim();
  typeUpper.toUpperCase();
  bool noisyType = (typeUpper == "TIME");
  if (!noisyType)
  {
    logf(1, "[MESH_EVT] msg=%s from=%s [%s] type=%s payload=%s sms=%d siren=%d ttl=%u ts=%llu\n",
         info.messageId.c_str(),
         labeledName.c_str(),
         macFull.c_str(),
         info.type.c_str(),
         info.payload.c_str(),
         (int)info.requiresSms,
         (int)info.requiresSiren,
         (unsigned)info.ttl,
         (unsigned long long)info.timestampMs);
  }
  if (typeUpper == "TIME")
  {
    uint64_t remoteMs = strtoull(info.payload.c_str(), nullptr, 10);
    if (IsValidEpoch(remoteMs))
    {
      ApplyClockFromMs(remoteMs, false);
      logf(1, "[MESH] Applied time sync from %s (%s) -> %s\n",
           labeledName.c_str(),
           macFull.c_str(),
           formatTimestampReadable(remoteMs).c_str());
    }
    return;
  }
  if (typeUpper == "STATE_RES")
  {
    cacheRemoteState(info);
    return;
  }
  if (typeUpper == "KEYS_RES")
  {
    cacheRemoteKeys(info);
    return;
  }
  if (typeUpper == "BOOLDATA" || typeUpper == "INTDATA" || typeUpper == "STRDATA")
  {
    cacheRemoteData(info, typeUpper.c_str());
    return;
  }
  if (typeUpper == "STATE_REQ")
  {
    DynamicJsonDocument doc(128);
    if (deserializeJson(doc, info.payload) != DeserializationError::Ok)
      return;
    String target = doc["t"] | doc["target"] | info.payload;
    String requester = doc["req"] | doc["r"] | "";
    bool match = matchesLocalNode(target);
    logf(1, "[MESH] STATE_REQ tgt=%s req=%s match=%d\n", target.c_str(), requester.c_str(), (int)match);
    if (match)
      sendMeshStateResponse(6, requester);
    return;
  }
  if (typeUpper == "KEYS_REQ")
  {
    DynamicJsonDocument doc(128);
    if (deserializeJson(doc, info.payload) != DeserializationError::Ok)
      return;
    String target = doc["t"] | doc["target"] | info.payload;
    String requester = doc["req"] | doc["r"] | "";
    bool match = matchesLocalNode(target);
    logf(1, "[MESH] KEYS_REQ tgt=%s req=%s match=%d\n", target.c_str(), requester.c_str(), (int)match);
    if (match)
      sendMeshKeysResponse(6, requester);
    return;
  }
  if (typeUpper == "RESET_REQ")
  {
    DynamicJsonDocument doc(128);
    if (deserializeJson(doc, info.payload) != DeserializationError::Ok)
      return;
    String target = doc["t"] | doc["target"] | info.payload;
    if (matchesLocalNode(target))
    {
      logf(1, "[MESH] RESET_REQ matched -> restarting...\n");
      restartLocalNodeDelayed(200);
    }
    return;
  }
  if (typeUpper == "KEY_IDX")
  {
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, info.payload) != DeserializationError::Ok)
      return;
    String target = doc["t"] | doc["target"] | "";
    if (!matchesLocalNode(target))
      return;
    int idx = doc["p"] | -1;
    String val = doc["v"] | "";
    if (idx < 0 || idx >= numKeys)
      return;
    const auto &dk = defaultKeys[idx];
    String validation = validateKey(dk.key, val);
    if (validation != "1")
    {
      logf(1, "[MESH] KEY_IDX rejected (%s) idx=%d key=%s\n", validation.c_str(), idx, dk.key.c_str());
      return;
    }
    prefs.putString(dk.key.c_str(), val);
    SetPublicVariablesFromPrefs();
    // اگر SSID یا پسورد تغییر کرد، SoftAP را با تنظیمات جدید بالا بیاور
    if (dk.key == "wifi_Ssid_Name" || dk.key == "Ssid_Password")
      StartSoftAP();
    logf(1, "[MESH] Applied KEY_IDX: %s=%s\n", dk.key.c_str(), val.c_str());
    // Broadcast full keys (no req filter) so همه نودها نسخه جدید را کش کنند
    sendMeshKeysResponse(6, "");
    return;
  }
  if (payloadTrim.startsWith("GAS 0"))
  {
    logf(1, "[MESH] Ignoring gas alarm from %s (%s) due to missing sensor indication.\n",
         labeledName.c_str(), macFull.c_str());
    return;
  }
  g_remoteAlarm.pending = true;
  g_remoteAlarm.description = labeledName + "[" + macShort + "]:" + info.type + ":" + info.payload;
  g_remoteAlarm.requiresSms = info.requiresSms;
  g_remoteAlarm.requiresSiren = info.requiresSiren;
  g_remoteAlarm.expireAtMs = millis() + 30000;
  logf(1, "[MESH] Remote event from %s (%s): %s:%s (sms=%d siren=%d)\n",
       labeledName.c_str(),
       macFull.c_str(),
       info.type.c_str(),
       info.payload.c_str(),
       (int)g_remoteAlarm.requiresSms,
       (int)g_remoteAlarm.requiresSiren);
}

// ---- Session token for web auth ----
String sessionToken;
// ===== Blink/Pulse State Machines =====
enum BlinkState
{
  BLINK_IDLE,
  BLINK_ON,
  BLINK_OFF
};

struct Blinker
{
  int pin;
  BlinkState st;
  uint32_t lastChange;
  uint16_t onMs;
  uint16_t offMs;
  int repeats; //                        ON/OFF         
  bool active; //         /              
};

const int AllarmLedPin = 22;    // LED
const int AllarmBuzzerPin = 21; // Buzzer
//                :                 LED                    Buzzer
Blinker ledBlink = {/*pin*/ AllarmLedPin, BLINK_IDLE, 0, 0, 0, 0, false};
Blinker buzzBlink = {/*pin*/ AllarmBuzzerPin, BLINK_IDLE, 0, 0, 0, 0, false};

//                         /        
void BlinkStart(Blinker &b, uint16_t onMs, uint16_t offMs, int repeats)
{
  b.onMs = onMs;
  b.offMs = offMs;
  b.repeats = repeats;
  b.st = BLINK_ON;
  b.active = true;
  b.lastChange = millis();
  digitalWrite(b.pin, HIGH);
}

//                              (                                       )
void BlinkTick(Blinker &b)
{
  if (!b.active)
    return;

  uint32_t now = millis();
  switch (b.st)
  {
  case BLINK_ON:
    if (now - b.lastChange >= b.onMs)
    {
      digitalWrite(b.pin, LOW);
      b.st = BLINK_OFF;
      b.lastChange = now;
    }
    break;

  case BLINK_OFF:
    if (now - b.lastChange >= b.offMs)
    {
      if (b.repeats > 1)
      {
        b.repeats--;
        digitalWrite(b.pin, HIGH);
        b.st = BLINK_ON;
        b.lastChange = now;
      }
      else
      {
        //              
        b.st = BLINK_IDLE;
        b.active = false;
        digitalWrite(b.pin, LOW);
      }
    }
    break;

  case BLINK_IDLE:
  default:
    //       
    break;
  }
}

//                                      
inline void LedBlinkStart(uint16_t onMs = 500, uint16_t offMs = 500, int repeats = 2)
{
  BlinkStart(ledBlink, onMs, offMs, repeats);
}
inline void BuzzerPulseStart(uint16_t onMs = 700, uint16_t offMs = 700, int repeats = 2)
{
  BlinkStart(buzzBlink, onMs, offMs, repeats);
}

// ---- Continuous device clock (from last sync) ----
uint64_t g_lastSyncUnixMs = 0; // ms                         sync
uint32_t g_lastSyncMillis = 0; // millis()            sync
uint64_t g_deviceNowMs = 0;    //                               
uint32_t g_clockTickMs = 0;
const uint8_t kTimeSyncTtl = 8;
const uint64_t kMinValidEpochMs = 1735689600000ULL; // 2025-01-01 UTC

// Helper to format ms->readable string (keeps +12600 & localtime logic)
String formatTimestampReadable(uint64_t timestampMs)
{
  time_t timestampSec = (timestampMs / 1000) + 12600;
  struct tm *timeinfo = localtime(&timestampSec);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", timeinfo);
  return String(buf);
}

String nowTimeReadable()
{
  uint64_t nowMs = g_lastSyncUnixMs + (uint64_t)(millis() - g_lastSyncMillis);
  return formatTimestampReadable(nowMs);
}

uint64_t DeviceNowMs()
{
  return g_lastSyncUnixMs + (uint64_t)(millis() - g_lastSyncMillis);
}

inline bool IsValidEpoch(uint64_t ms)
{
  return ms >= kMinValidEpochMs;
}

void UpdateDeviceClockIfNeeded()
{
  uint32_t nowMs = millis();
  if (nowMs - g_clockTickMs >= 1000)
  {
    g_clockTickMs = nowMs;
    g_deviceNowMs = g_lastSyncUnixMs + (uint64_t)(nowMs - g_lastSyncMillis);
    printTimestampReadable(g_deviceNowMs);
  }
}

// ===================== SIM808 / SMS =====================

struct SmsMessage
{
  String number;
  String text;
  int priority; // 0 =            1 =              2 =           
  unsigned long timestamp;
  uint32_t archiveId; //                                                        
};

struct IncomingSms
{
  String sender;
  String text;
};

enum SmsReceiveState
{
  SMS_RX_IDLE,
  SMS_RX_WAITING,
  SMS_RX_READING
};
enum SmsState
{
  SMS_IDLE,
  SMS_INIT,
  SMS_SEND_HEADER,
  SMS_SEND_BODY,
  SMS_SEND_CTRLZ,
  SMS_WAIT_RESPONSE
};

unsigned long smsTimer = 0;
HardwareSerial SIM808(1);
const int SIM808_RX = 16;
const int SIM808_TX = 17;
String latitude = "";
String longitude = "";
int currentPriority = -1;
SmsMessage currentMessage;
const int SMS_QUEUE_SIZE = 50;
const int INCOMING_SMS_QUEUE_SIZE = 5;
int incomingSmsHead = 0;
int incomingSmsTail = 0;
unsigned long smsRxStart = 0;
unsigned long smsRxLastReceive = 0;
bool simIsOnline = false;
int g_lastCSQ = -1;
int smsQueueHead[3] = {0, 0, 0};
int smsQueueTail[3] = {0, 0, 0};
SmsMessage smsQueues[3][SMS_QUEUE_SIZE];
IncomingSms incomingSmsQueue[INCOMING_SMS_QUEUE_SIZE];
SmsReceiveState smsRxState = SMS_RX_IDLE;
String smsRxBuffer = "";
SmsState smsState = SMS_IDLE;
unsigned long lastCommandTime = 0;
// buffer for send response parsing
String smsSendRespBuf = "";
 

// ===================== Web Server / Preferences =====================

AsyncWebServer server(80);

void ApplyClockFromMs(uint64_t timestampMs, bool broadcastMesh)
{
  prefs.putULong("epochStartTime", (unsigned long)timestampMs);
  epochStartTime = (unsigned long)timestampMs;

  g_lastSyncUnixMs = timestampMs;
  g_lastSyncMillis = millis();
  g_deviceNowMs = g_lastSyncUnixMs;

  printTimestampReadable(timestampMs);

  if (broadcastMesh)
  {
    MeshNet_RecordNetworkEvent("TIME", String(timestampMs), false, false, kTimeSyncTtl, timestampMs);
  }
}

// ---            /                       ---
String ssidNameDefault = "ElixHome";
String ssidPasswordDefault = "12345678";
String ssidName;
String ssidPassword;
String meshNameDefault = "elixMesh";
String meshPasswordDefault = "12345678";
String meshName;
String meshPassword;
String username = "admin";
String userPassword = "1234";
String deviceName;

// ===================== Config Keys =====================

//                           :                                                    +                                    
ConfigKey defaultKeys[] = {
    {"wifi_Ssid_Name",   "??? ???? ?? (SSID)",     "string", ssidNameDefault,      "2",  "32",  "",            false},
    {"Ssid_Password",    "??? ???? ??",                  "string", ssidPasswordDefault,  "8",  "32",  "",            false},

    {"deviceName",       "                   ",                "string", "            ",             "3",  "20",  "",            false},

    //                             :
    {"SystemEnabled",    "                            ",           "bool",   "true",               "",   "",    "true,false", true},
    #if HAS_PIR
{"PirEnabled",       "                             PIR",       "bool",   "true",               "",   "",    "true,false", false},
    #endif
#if HAS_VIB
{"VibEnabled",       "                                     ",      "bool",   "true",               "",   "",    "true,false", false},
    #endif
{"LedEnabled",       "                  LED",             "bool",   "true",               "",   "",    "true,false", false},
    {"BuzzerEnabled",    "                          ",            "bool",   "false",              "",   "",    "true,false", false},
    {"SmsAlertEnabled",  "                                        ",     "bool",   "true",               "",   "",    "true,false", false},
    {"SmsTxEnabled",     "                  SMS",             "bool",   "true",               "",   "",    "true,false", false},
    {"WifiEnabled",      "                  WiFi (SoftAP)",   "bool",   "true",               "",   "",    "true,false", false},
    {"mesh_name",        "Mesh Name",                        "string", meshNameDefault,       "3",  "32",  "",            true},
    {"mesh_pass",        "Mesh Password",                    "string", meshPasswordDefault,   "8",  "32",  "",            true},

    //                   (MQ)
    #if HAS_GAS
{"GasEnabled",       "                                    (MQ)",  "bool",   "true",               "",   "",    "true,false", false},
    {"GasMin",           "                               ",         "int",    "300",               "0",  "4095","",            false},
    {"GasMax",           "                               ",         "int",    "2500",              "0",  "4095","",            false},

    #endif
    #if HAS_DHT
    {"DhtEnabled",       "DHT Enabled",                  "bool",   "true",               "",   "",    "true,false",  false},
    {"TempMin",          "Temp Min (C)",                 "int",    "10",                "-40","125", "",            false},
    {"TempMax",          "Temp Max (C)",                 "int",    "40",                "-40","125", "",            false},
    {"HumMin",           "Humidity Min (%)",           "int",    "20",                "0","100", "",            false},
    {"HumMax",           "Humidity Max (%)",           "int",    "80",                "0","100", "",            false},
    #endif
{"OwnerMobile",      "                   ",                "mobile", "",                   "10", "12",  "",            false},
    {"alternetMobile",   "                         ",            "mobile", "",                   "10", "10",  "",            false},
    {"AlternetMobiles",  "                                   (*      )",  "string", "",                   "10", "100", "",            false},
};

const int numKeys = sizeof(defaultKeys) / sizeof(defaultKeys[0]);

void cacheRemoteKeys(const MeshEventInfo &info)
{
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, info.payload) != DeserializationError::Ok)
  {
    logf(1, "[MESH] KEYS_RES parse failed from %s payload=%s\n", formatMacFull(info.originMac).c_str(), info.payload.c_str());
    return;
  }
  String req = doc["r"] | doc["req"] | "";
  if (req.length() && !matchesLocalRequester(req))
  {
    logf(1, "[MESH] KEYS_RES ignored (not for me) from %s req=%s\n", formatMacFull(info.originMac).c_str(), req.c_str());
    return;
  }
  String sid = doc["sid"] | doc["s"] | doc["id"] | doc["i"] | formatMacShort(info.originMac);
  String id = doc["id"] | doc["i"] | sid;
  String friendly = doc["node"] | info.originNode;

  RemoteKeysEntry *existing = findRemoteKeys(sid, info.originMac);
  if (!existing)
  {
    RemoteKeysEntry ent;
    ent.id = id;
    ent.sid = sid;
    ent.friendly = friendly;
    ent.ts = info.timestampMs;
    ent.rawJson = "";
    ent.originMac = info.originMac;
    g_remoteKeys.push_back(ent);
    pruneRemoteCaches();
    existing = &g_remoteKeys.back();
  }

  DynamicJsonDocument acc(8192);
  if (existing->rawJson.length() && deserializeJson(acc, existing->rawJson) != DeserializationError::Ok)
  {
    acc.clear();
  }
  acc["sid"] = sid;
  acc["id"] = id;
  acc["node"] = friendly;
  acc["req"] = req;
  JsonArray keysArr;
  bool rebuild = true;
  if (acc.containsKey("keys") && acc["keys"].is<JsonArray>())
  {
    keysArr = acc["keys"].as<JsonArray>();
    if (keysArr.size() == (size_t)numKeys)
      rebuild = false;
  }
  if (rebuild)
  {
    acc.remove("keys");
    keysArr = acc.createNestedArray("keys");
    for (int j = 0; j < numKeys; ++j)
    {
      const auto &dk = defaultKeys[j];
      JsonObject o = keysArr.createNestedObject();
      o["key"] = dk.key;
      o["value"] = dk.defaultVal;
      o["type"] = dk.type;
      o["min"] = dk.min;
      o["max"] = dk.max;
      o["options"] = dk.options;
      o["isSystem"] = dk.isSystem;
      o["title"] = dk.title;
      o["defaultVal"] = dk.defaultVal;
      o["idx"] = j;
    }
  }

  int p = doc["p"] | -1;
  if (p >= 0 && p < (int)numKeys)
  {
    const auto &dk = defaultKeys[p];
    String kname = dk.key;
    String kval = doc["v"] | "";
    bool replaced = false;
    for (JsonObject existing : keysArr)
    {
      String exk = existing["key"] | "";
      if (exk == kname)
      {
        existing["value"] = kval;
        existing["type"] = dk.type;
        existing["min"] = dk.min;
        existing["max"] = dk.max;
        existing["options"] = dk.options;
        existing["isSystem"] = dk.isSystem;
        existing["title"] = dk.title;
        existing["defaultVal"] = dk.defaultVal;
        existing["idx"] = p;
        replaced = true;
        break;
      }
    }
    if (!replaced)
    {
      JsonObject o = keysArr.createNestedObject();
      o["key"] = kname;
      o["value"] = kval;
      o["type"] = dk.type;
      o["min"] = dk.min;
      o["max"] = dk.max;
      o["options"] = dk.options;
      o["isSystem"] = dk.isSystem;
      o["title"] = dk.title;
      o["defaultVal"] = dk.defaultVal;
      o["idx"] = p;
    }
  }

  existing->ts = info.timestampMs;
  existing->friendly = friendly;
  existing->sid = sid;
  existing->id = id;
  existing->originMac = info.originMac;
  existing->rawJson = "";
  serializeJson(acc, existing->rawJson);

  logf(1, "[MESH] Cached KEYS_RES for %s (%s) keys=%u\n", friendly.c_str(), sid.c_str(), keysArr.size());
}

// ????????? ???????? ????? boolData/intData/strData (???? = ????? defaultKeys)
void cacheRemoteData(const MeshEventInfo &info, const char *kind)
{
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, info.payload) != DeserializationError::Ok)
    return;
  String req = doc["r"] | doc["req"] | "";
  if (req.length() && !matchesLocalRequester(req))
    return;

  String sid = doc["src"] | doc["s"] | doc["i"] | formatMacShort(info.originMac);
  String id = sid;
  String friendly = doc["node"] | info.originNode;

  RemoteKeysEntry *existing = findRemoteKeys(sid, info.originMac);
  if (!existing)
  {
    RemoteKeysEntry ent;
    ent.id = id;
    ent.sid = sid;
    ent.friendly = friendly;
    ent.ts = info.timestampMs;
    ent.rawJson = "";
    ent.originMac = info.originMac;
    g_remoteKeys.push_back(ent);
    pruneRemoteCaches();
    existing = &g_remoteKeys.back();
  }

  DynamicJsonDocument acc(8192);
  if (existing->rawJson.length() && deserializeJson(acc, existing->rawJson) != DeserializationError::Ok)
    acc.clear();

  acc["sid"] = sid;
  acc["id"] = id;
  acc["node"] = friendly;
  acc["req"] = req;

  JsonArray keysArr;
  bool rebuild = true;
  if (acc.containsKey("keys") && acc["keys"].is<JsonArray>())
  {
    keysArr = acc["keys"].as<JsonArray>();
    if (keysArr.size() == (size_t)numKeys)
      rebuild = false;
  }
  if (rebuild)
  {
    acc.remove("keys");
    keysArr = acc.createNestedArray("keys");
    for (int j = 0; j < numKeys; ++j)
    {
      const auto &dk = defaultKeys[j];
      JsonObject o = keysArr.createNestedObject();
      o["key"] = dk.key;
      o["value"] = dk.defaultVal;
      o["type"] = dk.type;
      o["min"] = dk.min;
      o["max"] = dk.max;
      o["options"] = dk.options;
      o["isSystem"] = dk.isSystem;
      o["idx"] = j;
    }
  }

  JsonObject data = doc["data"].as<JsonObject>();
  String idxList;
  for (JsonPair kv : data)
  {
    int idx = String(kv.key().c_str()).toInt();
    if (idx < 0 || idx >= numKeys)
      continue;
    const auto &dk = defaultKeys[idx];
    String kname = dk.key;
    String ktype = dk.type;

    String newVal;
    if (strcasecmp(kind, "BOOLDATA") == 0)
    {
      int v = kv.value().as<int>();
      newVal = v ? "true" : "false";
    }
    else if (strcasecmp(kind, "INTDATA") == 0)
    {
      String v = kv.value().as<String>();
      int dash1 = v.indexOf('-');
      int dash2 = v.indexOf('-', dash1 + 1);
      String cur = v;
      if (dash1 > 0 && dash2 > dash1)
      {
        cur = v.substring(0, dash1);
        // min/max ????? ??? ??? ???? ????? ??? ????? ???? ???? ???? ?????? min/max ???? defaultKeys ?????
      }
      newVal = cur;
    }
    else
    {
      newVal = kv.value().as<String>();
    }

    if (idxList.length())
      idxList += ",";
    idxList += String(idx);

    bool replaced = false;
    for (JsonObject existingObj : keysArr)
    {
      String exk = existingObj["key"] | "";
      if (exk == kname)
      {
        existingObj["value"] = newVal;
        existingObj["type"] = ktype;
        existingObj["min"] = dk.min;
        existingObj["max"] = dk.max;
        existingObj["options"] = dk.options;
        existingObj["isSystem"] = dk.isSystem;
        existingObj["idx"] = idx;
        replaced = true;
        break;
      }
    }
    if (!replaced)
    {
      JsonObject o = keysArr.createNestedObject();
      o["key"] = kname;
      o["value"] = newVal;
      o["type"] = ktype;
      o["min"] = dk.min;
      o["max"] = dk.max;
      o["options"] = dk.options;
      o["isSystem"] = dk.isSystem;
      o["idx"] = idx;
    }
  }
  logf(1, "[MESH] cache %s idx=%s from %s\n", kind, idxList.c_str(), formatMacFull(info.originMac).c_str());

  existing->ts = info.timestampMs;
  existing->friendly = friendly;
  existing->sid = sid;
  existing->id = id;
  existing->originMac = info.originMac;
  existing->rawJson = "";
  serializeJson(acc, existing->rawJson);

  logf(1, "[MESH] Cached %s for %s (%s) keys=%u\n", kind, friendly.c_str(), sid.c_str(), keysArr.size());
}

// ===================== HTML =====================

// moved to WebUI.cpp

// ===================== Public Variables =====================

bool public_SystemStatus;
bool public_AlertEnabled_Sms; //                  
bool public_SmsTxEnabled;     //                     SMS
bool public_AlertEnabled_Buzzer;
bool public_LedEnabled;
bool public_PirEnabled;
bool public_VibEnabled;
bool public_WifiEnabled;

#if HAS_DHT
bool public_DhtEnabled;
int  public_TempMin = 10;
int  public_TempMax = 40;
int  public_HumMin  = 20;
int  public_HumMax  = 80;
float public_TempValue = 0;
float public_HumValue = 0;
#endif

String public_OwnerMobileNumber;
String public_AlternetMobile;
String public_AllAlternetMobiles;
bool public_SimIsOnline;
std::vector<String> public_List_AllAlternetMobiles;

//                                        22                    :
unsigned long public_DeviceTime;
//                  
bool public_GasEnabled = true;
int  public_GasMin = 300;
int  public_GasMax = 2500;
int  public_GasValue = 0;
const int GasAnalogPin = GAS_PIN; // configured via DeviceConfig.h (0 = disabled)
#if HAS_DHT
DHT dht(DHT_PIN, DHT_TYPE);
#endif

// ===================== Web Helpers =====================

bool authenticateWeb(AsyncWebServerRequest *request)
{
  if (!request->hasHeader("X-Auth"))
    return false;
  auto h = request->getHeader("X-Auth");
  return h && h->value() == sessionToken;
}

String validateKey(const String &key, const String &value)
{
  for (int i = 0; i < numKeys; i++)
  {
    if (defaultKeys[i].key == key)
    {
      String t = defaultKeys[i].type;
      if (t == "int")
      {
        int v = value.toInt();
        int mn = defaultKeys[i].min.toInt();
        int mx = defaultKeys[i].max.toInt();
        if (v < mn || v > mx)
          return "                       " + defaultKeys[i].min + "    " + defaultKeys[i].max + "         ";
      }
      else if (t == "string")
      {
        int len = value.length();
        int mn = defaultKeys[i].min.toInt();
        int mx = defaultKeys[i].max.toInt();
        if (len < mn || (mx > 0 && len > mx))
          return "                       " + String(mn) + "    " + String(mx) + "         ";
      }
      else if (t == "mobile")
      {
        if (value.length() != 11 || value[0] != '0')
          return "        : 09121234567";
        for (int j = 1; j < 11; j++)
          if (!isDigit(value[j]))
            return "             ";
      }
      else if (t == "dropdown" || t == "bool")
      {
        if (defaultKeys[i].options.indexOf(value) == -1 && defaultKeys[i].options.indexOf("," + value) == -1)
        {
          return "                            ";
        }
      }
      return "1";
    }
  }
  return "                       ";
}

// ---- Mesh helpers for addressing + responses ----
// salam: in func check mikone aya target id/mac baraye in node locale ya na (request ha faghat baraye khodemun ejra she)
bool matchesLocalNode(const String &target)
{
  String t = target;
  t.trim();
  if (!t.length())
    return false;
  t.toUpperCase();
  String shortId = MeshNet_GetLocalNodeId();
  shortId.toUpperCase();
  String macShort = formatMacShort(ESP.getEfuseMac());
  macShort.toUpperCase();
  String macFull = formatMacFull(ESP.getEfuseMac());
  macFull.toUpperCase();
  String macCompact = formatMacCompact(ESP.getEfuseMac());
  macCompact.toUpperCase();
  String friendly = deviceName.length() ? deviceName : shortId;
  friendly.toUpperCase();
  return t == shortId || t == macShort || t == macFull || t == macCompact || t == friendly;
}

// salam: in func moteasel mikone ke requester ke dar payload hast mac/short locale hast ya na
bool matchesLocalRequester(const String &req)
{
  String r = req;
  r.trim();
  if (!r.length())
    return false;
  r.toUpperCase();
  String macFull = formatMacFull(ESP.getEfuseMac());
  String macShort = formatMacShort(ESP.getEfuseMac());
  macFull.toUpperCase();
  macShort.toUpperCase();
  return r == macFull || r == macShort;
}

// salam: in func state fa'ali node locale ro be soorate event STATE_RES dar mesh publish mikone
void sendMeshStateResponse(uint8_t ttl, const String &requesterMac)
{
  DynamicJsonDocument doc(192);
  String shortId = MeshNet_GetLocalNodeId();
  String fullId = formatMacFull(ESP.getEfuseMac());
  doc["s"] = shortId;       // sid ?????
  doc["i"] = fullId;        // mac ????
  doc["r"] = requesterMac;  // requester
  doc["t"] = DeviceNowMs(); // time
  // ????????? ???????? ?? ?? ???????? (???? ????? ???)
  uint16_t flags = 0;
  flags |= public_SystemStatus ? 1 << 0 : 0;
  flags |= public_PirEnabled ? 1 << 1 : 0;
  flags |= public_VibEnabled ? 1 << 2 : 0;
  flags |= public_GasEnabled ? 1 << 3 : 0;
  flags |= public_WifiEnabled ? 1 << 4 : 0;
  flags |= public_SmsTxEnabled ? 1 << 5 : 0;
  flags |= public_AlertEnabled_Buzzer ? 1 << 6 : 0;
  flags |= public_LedEnabled ? 1 << 7 : 0;
  doc["f"] = flags;
  String payload;
  serializeJson(doc, payload);
  if (payload.length() > 150)
    return;
  logf(1, "[MESH] Send STATE_RES ttl=%u req=%s payload=%s\n", (unsigned)ttl, requesterMac.c_str(), payload.c_str());
  MeshNet_RecordNetworkEvent("STATE_RES", payload, false, false, ttl, DeviceNowMs());
}

// salam: in func tamame key/value config haye locale ro joda joda (bool/int/str) baraye requester mifereste
void sendMeshKeysResponse(uint8_t ttl, const String &requesterMac)
{
  String srcMac = formatMacFull(ESP.getEfuseMac());

  DynamicJsonDocument bdoc(384);
  bdoc["t"] = "boolData";
  bdoc["r"] = requesterMac;
  bdoc["src"] = srcMac;
  JsonObject bdata = bdoc.createNestedObject("data");

  std::vector<int> boolIdx;
  std::vector<std::pair<String, String>> intItems;
  std::vector<std::pair<String, String>> strItems;

  for (int idx = 0; idx < numKeys; ++idx)
  {
    const auto &dk = defaultKeys[idx];
    String val = prefs.getString(dk.key.c_str(), dk.defaultVal);
    if (dk.type == "bool")
    {
      bdata[String(idx)] = (val == "true" || val == "1") ? 1 : 0;
      boolIdx.push_back(idx);
    }
    else if (dk.type == "int")
    {
      String cur = val.length() ? val : dk.defaultVal;
      String mn = dk.min;
      String mx = dk.max;
      intItems.push_back({String(idx), cur + "-" + mn + "-" + mx}); // current-min-max
    }
    else
    {
      strItems.push_back({String(idx), val});
    }
  }

  auto sendDoc = [&](DynamicJsonDocument &doc, const char *label) -> bool
  {
    String payload;
    serializeJson(doc, payload);
    if (payload.length() > 200)
    {
      logf(1, "[MESH] %s payload too large (%u)\n", label, (unsigned)payload.length());
      return false;
    }
    logf(1, "[MESH] TX %s -> %s len=%u payload=%s\n", label, requesterMac.c_str(), (unsigned)payload.length(), payload.c_str());
    MeshNet_RecordNetworkEvent(label, payload, false, false, ttl, DeviceNowMs());
    return true;
  };

  auto joinIdx = [](const std::vector<int> &v) -> String {
    String out;
    for (size_t i = 0; i < v.size(); ++i)
    {
      if (i)
        out += ",";
      out += String(v[i]);
    }
    return out.length() ? out : String("-");
  };
  auto joinIdxPair = [](const std::vector<std::pair<String, String>> &v) -> String {
    String out;
    for (size_t i = 0; i < v.size(); ++i)
    {
      if (i)
        out += ",";
      out += v[i].first;
    }
    return out.length() ? out : String("-");
  };
  logf(1, "[MESH] KEYS_RES build -> boolIdx=%s intIdx=%s strIdx=%s\n",
       joinIdx(boolIdx).c_str(),
       joinIdxPair(intItems).c_str(),
       joinIdxPair(strItems).c_str());

  // اگر پکت بزرگ شد، برای هر entry جدا بفرست
  if (!boolIdx.empty())
  {
    DynamicJsonDocument d(192);
    d["t"] = "boolData";
    d["r"] = requesterMac;
    d["src"] = srcMac;
    JsonObject dd = d.createNestedObject("data");
    for (size_t i = 0; i < boolIdx.size(); ++i)
    {
      String k = String(boolIdx[i]);
      dd[k] = bdata[k];
    }
    sendDoc(d, "boolData");
    delay(2);
  }
  delay(3);

  // ints: split into batches of up to 3 entries per packet
  if (!intItems.empty())
  {
    const size_t chunkSize = 3;
    size_t sent = 0;
    while (sent < intItems.size())
    {
      DynamicJsonDocument d(256);
      d["t"] = "intData";
      d["r"] = requesterMac;
      d["src"] = srcMac;
      JsonObject dd = d.createNestedObject("data");
      size_t added = 0;
      while (sent < intItems.size() && added < chunkSize)
      {
        dd[intItems[sent].first] = intItems[sent].second;
        ++sent;
        ++added;
      }
      sendDoc(d, "intData");
      delay(2);
    }
  }
  delay(3);

  // strings: send in small batches to ensure همه شاخص‌ها (از جمله 22،23) ارسال شوند
  // strings: each key in its own packet
  for (auto &kv : strItems)
  {
    DynamicJsonDocument d(192);
    d["t"] = "strData";
    d["r"] = requesterMac;
    d["src"] = srcMac;
    JsonObject dd = d.createNestedObject("data");
    dd[kv.first] = kv.second;
    sendDoc(d, "strData");
    delay(2);
  }
}

// ===================== SMS LOG (Archive 200) =====================

struct SmsArchive
{
  uint32_t id;        //                          
  String number;      //           
  String reason;      //                  /        
  uint64_t tsMs;      //                        epoch ms             
  String text;        //       
  String status;      // queued/sending/sent/failed/blocked/delivered
  int priority;       // 0/1/2
  uint8_t attempts;   //                                        
  uint64_t lastSentAtMs; //                                                              
};

const uint16_t SMS_LOG_CAP = 200;
SmsArchive g_smsLog[SMS_LOG_CAP];
uint16_t g_smsLogHead = 0; //                                                       
uint16_t g_smsLogCount = 0;
uint32_t g_smsLogNextId = 1;

//                                                    id
uint32_t smsLogAppend(const String &number, const String &reason, uint64_t tsMs, const String &text, const String &status, int priority)
{
  SmsArchive &slot = g_smsLog[g_smsLogHead];
  slot.id = g_smsLogNextId++;
  slot.number = number;
  slot.reason = reason;
  slot.tsMs = tsMs;
  slot.text = text;
  slot.status = status;
  slot.priority = priority;
  slot.attempts = 0;
  slot.lastSentAtMs = 0;

  g_smsLogHead = (g_smsLogHead + 1) % SMS_LOG_CAP;
  if (g_smsLogCount < SMS_LOG_CAP)
    g_smsLogCount++;

  return slot.id;
}

//                                     id
void smsLogUpdateStatus(uint32_t id, const String &newStatus)
{
  for (uint16_t i = 0; i < g_smsLogCount; i++)
  {
    int idx = (int)g_smsLogHead - 1 - i;
    if (idx < 0)
      idx += SMS_LOG_CAP;
    if (g_smsLog[idx].id == id)
    {
      g_smsLog[idx].status = newStatus;
      return;
    }
  }
}

//                                                                            (CMGS+OK):              attempts           lastSentAtMs
void smsLogOnSent(uint32_t id)
{
  uint64_t nowMs = g_lastSyncUnixMs + (uint64_t)(millis() - g_lastSyncMillis);
  for (uint16_t i = 0; i < g_smsLogCount; i++)
  {
    int idx = (int)g_smsLogHead - 1 - i;
    if (idx < 0) idx += SMS_LOG_CAP;
    if (g_smsLog[idx].id == id)
    {
      g_smsLog[idx].status = "sent";
      if (g_smsLog[idx].attempts < 255) g_smsLog[idx].attempts++;
      g_smsLog[idx].lastSentAtMs = nowMs;
      return;
    }
  }
}

//                                                                           (                                     )
bool requeueArchivedSms(uint32_t id)
{
  for (uint16_t i = 0; i < g_smsLogCount; i++)
  {
    int idx = (int)g_smsLogHead - 1 - i;
    if (idx < 0) idx += SMS_LOG_CAP;
    if (g_smsLog[idx].id == id)
    {
      int p = g_smsLog[idx].priority;
      int head = smsQueueHead[p];
      int tail = smsQueueTail[p];
      int nextTail = (tail + 1) % SMS_QUEUE_SIZE;
      if (nextTail == head) return false; // queue full
      smsQueues[p][tail] = {g_smsLog[idx].number, g_smsLog[idx].text, p, millis(), id};
      smsQueueTail[p] = nextTail;
      g_smsLog[idx].status = "queued";
      return true;
    }
  }
  return false;
}

//      loop:                           sent               delivered                           requeue     
void SmsRetryTick()
{
  uint64_t nowMs = g_lastSyncUnixMs + (uint64_t)(millis() - g_lastSyncMillis);
  for (uint16_t i = 0; i < g_smsLogCount; i++)
  {
    int idx = (int)g_smsLogHead - 1 - i;
    if (idx < 0) idx += SMS_LOG_CAP;
    auto &e = g_smsLog[idx];
    // Policy:
    // - Only ALARM messages are retried
    // - Retry at most once (attempts < 2)
    // - Non-ALARM messages: no retry
    if (e.status == "sent" && e.attempts > 0 && e.attempts < 2 && e.reason == "ALARM")
    {
      if (e.lastSentAtMs > 0 && (nowMs - e.lastSentAtMs) >= 180000ULL)
      {
        Serial.printf("[RETRY] ALARM id=%lu attempts=%u -> requeue\n", (unsigned long)e.id, (unsigned)e.attempts);
        requeueArchivedSms(e.id);
      }
    }
  }
}

// ===================== INBOX LOG (Archive 200 received) =====================
struct InboxItem
{
  uint32_t id;
  String from;
  String text;
  uint64_t tsMs;
};

const uint16_t INBOX_LOG_CAP = 200;
InboxItem g_inbox[INBOX_LOG_CAP];
uint16_t g_inboxHead = 0;
uint16_t g_inboxCount = 0;
uint32_t g_inboxNextId = 1;

uint32_t inboxAppend(const String &from, const String &text, uint64_t tsMs)
{
  InboxItem &slot = g_inbox[g_inboxHead];
  slot.id = g_inboxNextId++;
  slot.from = from;
  slot.text = text;
  slot.tsMs = tsMs;
  g_inboxHead = (g_inboxHead + 1) % INBOX_LOG_CAP;
  if (g_inboxCount < INBOX_LOG_CAP) g_inboxCount++;
  return slot.id;
}

//                                             Sent/Sending      Delivered (         PDU parsing)
void smsLogMarkLatestDelivered()
{
  for (uint16_t i = 0; i < g_smsLogCount; i++)
  {
    int idx = (int)g_smsLogHead - 1 - i;
    if (idx < 0) idx += SMS_LOG_CAP;
    if (g_smsLog[idx].status == "sent" || g_smsLog[idx].status == "sending")
    {
      g_smsLog[idx].status = "delivered";
      Serial.printf("[LOG ] delivery confirmed for id=%lu number=%s\n",
                    (unsigned long)g_smsLog[idx].id, g_smsLog[idx].number.c_str());
      return;
    }
  }
}

//              /                             
String detectReasonFromText(const String &txtIn)
{
  String t = txtIn;
  String tUpper = t;
  tUpper.toUpperCase();
  if (tUpper.indexOf("ALARM") != -1)
    return "ALARM";
  if (tUpper.indexOf("GPS NOT READY") != -1 || tUpper.indexOf("GPSNR") != -1)
    return "GPSNR";
  if (tUpper.indexOf("HTTPS://MAPS.GOOGLE") != -1 || tUpper.indexOf("GPS") != -1)
    return "GPS";
  if (tUpper.indexOf("SYSTEM IS ARM") != -1 || tUpper.indexOf("ARM") != -1)
    return "ARM";
  if (tUpper.indexOf("SYSTEM IS DISARM") != -1 || tUpper.indexOf("DISARM") != -1)
    return "DISARM";
  if (tUpper.indexOf("TEST MESSAGE") != -1 || tUpper.indexOf("TEST") != -1)
    return "TEST";
  if (tUpper.indexOf("ACCESS DENIED") != -1 || tUpper.indexOf("ACCESSDENIED") != -1)
    return "DENY";
  if (tUpper.indexOf("UNKNOWN COMMAND") != -1 || tUpper.indexOf("UNK") != -1)
    return "UNK";
  if (tUpper.indexOf("CHECK") != -1)
    return "CHECK";
  return "GEN";
}

// ===================== HTML/REST Registration =====================

void HtmlFunctions()
{
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/setTime", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
    if (!authenticateWeb(request)) { request->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    if (!json.is<JsonObject>()) { request->send(400,"application/json","{\"error\":\"         JSON                      \"}"); return; }
    JsonObject obj = json.as<JsonObject>();
    unsigned long long timestamp = obj["timestamp"]; // ms
    Serial.print("browser time "); Serial.println((unsigned long long)timestamp);

    ApplyClockFromMs(timestamp, true);
    request->send(200, "application/json", "{\"success\":true}"); }));

  server.on("/api/time", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateWeb(request)) { request->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    uint64_t nowMs = g_lastSyncUnixMs + (uint64_t)(millis() - g_lastSyncMillis);
    request->send(200, "application/json", "{\"timestamp\":" + String((unsigned long)nowMs) + "}"); });

  // Gas sensor value API
  #if HAS_GAS
  server.on("/api/gas", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateWeb(request)) { request->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    int gasRaw = analogRead(GasAnalogPin);
    public_GasValue = (public_GasValue * 7 + gasRaw) / 8;
    Serial.printf("[GAS/API] pin=%d raw=%d filtered=%d\n", GasAnalogPin, gasRaw, public_GasValue);
    DynamicJsonDocument doc(256);
    doc["success"] = true;
    doc["value"] = public_GasValue;
    doc["min"] = public_GasMin;
    doc["max"] = public_GasMax;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out); });
#endif

#if HAS_DHT
  server.on("/api/dht", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateWeb(request)) { request->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) public_TempValue = t;
    if (!isnan(h)) public_HumValue = h;
    Serial.printf("[DHT/API] t=%.1fC h=%.0f%%\n", public_TempValue, public_HumValue);
    DynamicJsonDocument doc(256);
    doc["success"] = true;
    doc["t"] = public_TempValue;
    doc["h"] = public_HumValue;
    doc["min"] = public_TempMin;
    doc["max"] = public_TempMax;
    doc["hmin"] = public_HumMin;
    doc["hmax"] = public_HumMax;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out); });
#endif

  server.on("/api/sendTestSms", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!authenticateWeb(request)) { request->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    Serial.println("send test message ");
    enqueueSms(public_OwnerMobileNumber , "test message at " + nowTimeReadable(), 2);
    request->send(200, "application/json", "{\"success\":true}"); });

  server.on("/ResetEsp", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!authenticateWeb(request)) { request->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    request->send(200, "application/json", "{\"success\":true}");
    restartLocalNodeDelayed(200);
  });

  server.addHandler(new AsyncCallbackJsonWebHandler("/api/mesh/resetRemote", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
    if (!authenticateWeb(request)) { request->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    if (!json.is<JsonObject>()) { request->send(400,"application/json","{\"error\":\"invalid json\"}"); return; }
    String nodeId = json["nodeId"] | "";
    nodeId.trim();
    if (!nodeId.length()) { request->send(400,"application/json","{\"error\":\"nodeId required\"}"); return; }
    DynamicJsonDocument doc(128);
    doc["t"] = nodeId;
    doc["req"] = MeshNet_GetLocalMacStr();
    String payload; serializeJson(doc, payload);
    logf(1, "[API] resetRemote start nodeId=%s payload=%s\n", nodeId.c_str(), payload.c_str());
    MeshNet_RecordNetworkEvent("RESET_REQ", payload, false, false, 8, DeviceNowMs());
    request->send(200, "application/json", "{\"success\":true}");
    logf(1, "[API] resetRemote done nodeId=%s -> HTTP200\n", nodeId.c_str()); }));

  server.on("/api/keys", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    if (!authenticateWeb(req)) { req->send(401); return; }
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.createNestedArray("keys");
    for (int i = 0; i < numKeys; i++) {
      JsonObject obj = arr.createNestedObject();
      obj["key"] = defaultKeys[i].key;
      obj["title"] = defaultKeys[i].title;
      obj["type"] = defaultKeys[i].type;
      obj["value"] = prefs.getString(defaultKeys[i].key.c_str(), defaultKeys[i].defaultVal);
      obj["min"] = defaultKeys[i].min;
      obj["max"] = defaultKeys[i].max;
      obj["options"] = defaultKeys[i].options;
      obj["isSystem"] = defaultKeys[i].isSystem;
    }
    doc["theme"] = prefs.getString("theme", "dark");
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out); });

  // NEW: INBOX LOG (                                     )
  server.on("/api/inboxLog", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    if (!authenticateWeb(req)) { req->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    DynamicJsonDocument doc(16384);
    doc["success"] = true;
    JsonArray items = doc.createNestedArray("items");
    uint16_t cnt = g_inboxCount;
    for (uint16_t i = 0; i < cnt; i++)
    {
      int idx = (int)g_inboxHead - 1 - i;
      if (idx < 0) idx += INBOX_LOG_CAP;
      JsonObject o = items.createNestedObject();
      o["id"] = g_inbox[idx].id;
      o["from"] = g_inbox[idx].from;
      o["text"] = g_inbox[idx].text;
      o["timestamp"] = (uint64_t)g_inbox[idx].tsMs;
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out); });

  // NEW: SMS LOG (                      )
  server.on("/api/smsLog", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    if (!authenticateWeb(req)) { req->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    DynamicJsonDocument doc(16384);
    doc["success"] = true;
    JsonArray items = doc.createNestedArray("items");

    //                                             
    uint16_t cnt = g_smsLogCount;
    for (uint16_t i = 0; i < cnt; i++)
    {
      int idx = (int)g_smsLogHead - 1 - i;
      if (idx < 0) idx += SMS_LOG_CAP;
      JsonObject o = items.createNestedObject();
      o["id"] = g_smsLog[idx].id;
      o["number"] = g_smsLog[idx].number;
      o["reason"] = g_smsLog[idx].reason;
      o["timestamp"] = (uint64_t)g_smsLog[idx].tsMs; // epoch ms
      o["text"] = g_smsLog[idx].text;
      o["status"] = g_smsLog[idx].status;
      o["priority"] = g_smsLog[idx].priority;
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out); });

  // salam: in route list node/event/state/key mesh ro baraye dashboard neshan midahad
  server.on("/api/mesh/state", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    if (!authenticateWeb(req)) { req->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    DynamicJsonDocument doc(65536);
    static String lastGoodStateJson;
    int stateCount = 0;
    int keyCount = 0;
    int nodeCount = 0;
    int eventCount = 0;
    logf(1, "[API] /mesh/state start\n");
    auto fillFull = [&]()
    {
      JsonArray nodes = doc.createNestedArray("nodes");
      MeshNet_SerializeNodes(nodes);
      nodeCount = nodes.size();
      // Events intentionally omitted (only empty array) to keep payload minimal
      JsonArray events = doc.createNestedArray("events");
      eventCount = events.size();
      JsonArray states = doc.createNestedArray("states");
      for (const auto &st : g_remoteStates)
      {
        JsonObject obj = states.createNestedObject();
        obj["id"] = st.id;
        obj["friendly"] = st.friendly;
        obj["ts"] = (uint64_t)st.ts;
        if (st.rawJson.length())
        {
          DynamicJsonDocument tmp(1024);
          if (deserializeJson(tmp, st.rawJson) == DeserializationError::Ok)
            obj["raw"] = tmp.as<JsonVariant>();
          else
            obj["rawJson"] = st.rawJson;
        }
      }
      JsonArray keys = doc.createNestedArray("keys");
      for (const auto &k : g_remoteKeys)
      {
        JsonObject obj = keys.createNestedObject();
        obj["id"] = k.id;
        obj["sid"] = k.sid;
        obj["friendly"] = k.friendly;
        obj["ts"] = (uint64_t)k.ts;
        if (k.rawJson.length())
        {
          DynamicJsonDocument tmp(8192);
          if (deserializeJson(tmp, k.rawJson) == DeserializationError::Ok)
            obj["raw"] = tmp.as<JsonVariant>();
          else
            obj["rawJson"] = k.rawJson;
        }
      }
      stateCount = doc["states"].is<JsonArray>() ? doc["states"].size() : 0;
      keyCount = doc["keys"].is<JsonArray>() ? doc["keys"].size() : 0;
    };

    fillFull();
    bool docOverflowed = doc.overflowed();
    if (docOverflowed)
    {
      // اگر باز هم پر شد، نسخه ساده فقط با nodes و خطای overflow بده
      doc.clear();
      doc["overflow"] = true;
      JsonArray nodes = doc.createNestedArray("nodes");
      MeshNet_SerializeNodes(nodes);
      nodeCount = nodes.size();
    }
    String out; serializeJson(doc, out);
    if (out.length() < 4)
    {
      out = "{\"overflow\":true,\"nodes\":[],\"events\":[],\"states\":[],\"keys\":[]}";
      logf(1, "[API] /mesh/state minimal fallback response used\n");
    }
    logf(1, "[API] /mesh/state nodes=%d events=%d states=%d keys=%d overflow=%d len=%u\n",
         nodeCount, eventCount, stateCount, keyCount, doc.overflowed() ? 1 : 0, (unsigned)out.length());
    if (!docOverflowed && ((stateCount > 0) || (keyCount > 0) || nodeCount > 0))
    {
      lastGoodStateJson = out;
      req->send(200, "application/json", out);
      logf(1, "[API] /mesh/state -> live payload sent len=%u states=%d keys=%d\n", (unsigned)out.length(), stateCount, keyCount);
    }
    else if (lastGoodStateJson.length())
    {
      logf(1, "[API] /mesh/state fallback to lastGood len=%u\n", (unsigned)lastGoodStateJson.length());
      req->send(200, "application/json", lastGoodStateJson);
      logf(1, "[API] /mesh/state -> fallback payload sent len=%u\n", (unsigned)lastGoodStateJson.length());
    }
    else
    {
      logf(1, "[API] /mesh/state returning empty payload len=%u\n", (unsigned)out.length());
      req->send(200, "application/json", out);
      logf(1, "[API] /mesh/state -> empty payload sent\n");
    } });

  // salam: endpoint sabok faghat keys cache shode ra bar mi-gardand (bedone nodes/events/states)
  server.on("/api/mesh/keys", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    if (!authenticateWeb(req)) { req->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    String target = "";
    String targetCompact = "";
    if (req->hasParam("id"))
    {
      target = req->getParam("id")->value();
      target.trim();
      target.toUpperCase();
      targetCompact.reserve(target.length());
      for (char c : target) if (c != ':' && c != '-') targetCompact += c;
    }
    DynamicJsonDocument doc(16384);
    JsonArray keysArr = doc.createNestedArray("keys");
    for (const auto &k : g_remoteKeys)
    {
      if (target.length())
      {
        String cid = k.id; cid.toUpperCase();
        String csid = k.sid; csid.toUpperCase();
        String cf = k.friendly; cf.toUpperCase();
        String cNoColonId; cNoColonId.reserve(cid.length());
        for (char c : cid) if (c != ':' && c != '-') cNoColonId += c;
        String cNoColonSid; cNoColonSid.reserve(csid.length());
        for (char c : csid) if (c != ':' && c != '-') cNoColonSid += c;
        bool match =
          cid.equalsIgnoreCase(target) || csid.equalsIgnoreCase(target) || cf.equalsIgnoreCase(target) ||
          (targetCompact.length() && (cNoColonId.equalsIgnoreCase(targetCompact) || cNoColonSid.equalsIgnoreCase(targetCompact)));
        if (!match) continue;
      }
      JsonObject obj = keysArr.createNestedObject();
      obj["id"] = k.id;
      obj["sid"] = k.sid;
      obj["friendly"] = k.friendly;
      obj["ts"] = (uint64_t)k.ts;
      if (k.rawJson.length())
      {
        DynamicJsonDocument tmp(8192);
        if (deserializeJson(tmp, k.rawJson) == DeserializationError::Ok)
          obj["raw"] = tmp.as<JsonVariant>();
        else
          obj["rawJson"] = k.rawJson;
      }
    }
    doc["success"] = true;
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out); });

  server.on("/api/mem", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    if (!authenticateWeb(req)) { req->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    uint32_t total = ESP.getHeapSize();
    uint32_t free  = ESP.getFreeHeap();
    uint32_t used  = (total > free) ? (total - free) : 0;
    uint32_t pct   = total ? (used * 100 / total) : 0;
    DynamicJsonDocument doc(256);
    doc["success"] = true;
    doc["totalKB"] = (uint32_t)(total / 1024);
    doc["freeKB"]  = (uint32_t)(free  / 1024);
    doc["usedKB"]  = (uint32_t)(used  / 1024);
    doc["pct"]     = pct;
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out); });

  server.addHandler(new AsyncCallbackJsonWebHandler("/api/login", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
    if (!json.is<JsonObject>()) { request->send(400,"application/json","{\"error\":\"         JSON                      \"}"); return; }
    JsonObject obj = json.as<JsonObject>();
    String user = obj["user"];
    String pass = obj["pass"];
    logf(1, "[API] login attempt user=%s\n", user.c_str());
    if (user == username && pass == userPassword) {
      sessionToken = String((uint32_t)esp_random(), HEX);
      DynamicJsonDocument resp(128);
      resp["success"] = true;
      resp["token"]   = sessionToken;
      String out; serializeJson(resp, out);
      request->send(200, "application/json", out);
      logf(1, "[API] login success user=%s\n", user.c_str());
    } else {
      request->send(401, "application/json", "{\"error\":\"                                            \"}");
      logf(1, "[API] login failed user=%s\n", user.c_str());
    } }));

  // salam: in endpoint az UI locale yek STATE_REQ be node target mifereste ta state jadid ro bekhunim
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/mesh/requestState", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
    if (!authenticateWeb(request)) { request->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    if (!json.is<JsonObject>()) { request->send(400,"application/json","{\"error\":\"invalid json\"}"); return; }
    String nodeId = json["nodeId"] | "";
    nodeId.trim();
    if (!nodeId.length()) { request->send(400,"application/json","{\"error\":\"nodeId required\"}"); return; }
    DynamicJsonDocument doc(128);
    doc["t"] = nodeId;
    doc["req"] = MeshNet_GetLocalMacStr();
    String payload; serializeJson(doc, payload);
    logf(1, "[API] requestState start nodeId=%s payload=%s\n", nodeId.c_str(), payload.c_str());
    MeshNet_RecordNetworkEvent("STATE_REQ", payload, false, false, 8, DeviceNowMs());
    request->send(200, "application/json", "{\"success\":true}");
    logf(1, "[API] requestState done nodeId=%s -> HTTP200\n", nodeId.c_str()); }));

  // salam: in endpoint az UI locale yek KEYS_REQ be node target mifereste ta config ha ro begirim
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/mesh/requestKeys", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
    if (!authenticateWeb(request)) { request->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    if (!json.is<JsonObject>()) { request->send(400,"application/json","{\"error\":\"invalid json\"}"); return; }
    String nodeId = json["nodeId"] | "";
    nodeId.trim();
    if (!nodeId.length()) { request->send(400,"application/json","{\"error\":\"nodeId required\"}"); return; }
    DynamicJsonDocument doc(128);
    doc["t"] = nodeId;
    doc["req"] = MeshNet_GetLocalMacStr();
    String payload; serializeJson(doc, payload);
    logf(1, "[API] requestKeys start nodeId=%s payload=%s\n", nodeId.c_str(), payload.c_str());
    MeshNet_RecordNetworkEvent("KEYS_REQ", payload, false, false, 8, DeviceNowMs());
    request->send(200, "application/json", "{\"success\":true}");
    logf(1, "[API] requestKeys done nodeId=%s -> HTTP200\n", nodeId.c_str()); }));

  // salam: in endpoint key/value jadid ro be soorate KEY_IDX be node remote broadcast mikone ta setting avaz she
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/mesh/saveRemote", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
    if (!authenticateWeb(request)) { request->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    if (!json.is<JsonObject>()) { request->send(400,"application/json","{\"error\":\"invalid json\"}"); return; }
    String nodeId = json["nodeId"] | "";
    String key = json["key"] | "";
    String value = json["value"] | "";
    nodeId.trim(); key.trim();
    int idx = keyIndexByName(key);
    if (!nodeId.length() || idx < 0) { request->send(400,"application/json","{\"error\":\"nodeId or key invalid\"}"); return; }
    DynamicJsonDocument doc(256);
    doc["t"] = nodeId;
    doc["p"] = idx;
    doc["v"] = value;
    String payload;
    serializeJson(doc, payload);
    logf(1, "[API] saveRemote start nodeId=%s key=%s val=%s payload=%s\n", nodeId.c_str(), key.c_str(), value.c_str(), payload.c_str());
    MeshNet_RecordNetworkEvent("KEY_IDX", payload, false, false, 8, DeviceNowMs());
    request->send(200, "application/json", "{\"success\":true}"); }));

  server.addHandler(new AsyncCallbackJsonWebHandler("/api/save", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
    if (!authenticateWeb(request)) { request->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    if (!json.is<JsonObject>()) { request->send(400, "application/json", "{\"error\":\"         JSON                      \"}"); return; }
    JsonObject obj = json.as<JsonObject>();
    String key = obj["key"];
    String value = obj["value"];
    String ValidationResult = validateKey(key, value);
    if (ValidationResult != "1") {
      request->send(400, "application/json", "{\"error\":\"                         : " + ValidationResult + "\"}");
      return;
    }
    prefs.putString(key.c_str(), value);
    SetPublicVariablesFromPrefs();
    if (key == "wifi_Ssid_Name" || key == "Ssid_Password")
      StartSoftAP();
    request->send(200, "application/json", "{\"success\":true}"); }));

  server.addHandler(new AsyncCallbackJsonWebHandler("/api/changepass", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
    if (!authenticateWeb(request)) { request->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    if (!json.is<JsonObject>()) { request->send(400,"application/json","{\"error\":\"         JSON                      \"}"); return; }
    JsonObject obj = json.as<JsonObject>();
    String oldpass = obj["old"];
    String newpass = obj["new"];
    if (oldpass != userPassword) { request->send(403,"application/json","{\"error\":\"                                   \"}"); return; }
    if (newpass.length() < 4)    { request->send(400,"application/json","{\"error\":\"                                                      \"}"); return; }
    userPassword = newpass;
    prefs.putString("userPassword", newpass);
    request->send(200, "application/json", "{\"success\":true}"); }));

  server.addHandler(new AsyncCallbackJsonWebHandler("/api/reset", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
    if (!authenticateWeb(request)) { request->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    prefs.clear();
    request->send(200, "application/json", "{\"success\":true}"); }));

  server.on("/api/resetEsp", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!authenticateWeb(request)) { request->send(401,"application/json","{\"error\":\"unauthorized\"}"); return; }
    request->send(200, "application/json", "{\"success\":true}");
    delay(500);
    ESP.restart(); });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req)
            { req->send(200, "text/html; charset=utf-8", index_html); });

  server.begin();
}

// ===================== Public Funcs =====================

// Forward declarations needed by TaskDelay
void monitorInputSmsStateMachine();
void processIncomingSmsQueue();
void PollSensorsAndDecide();
void SetAllarmState();
void SmsSender();

// NOTE: extended TaskDelay with service ticks
void TaskDelay(int ms, bool serviceTick, bool serviceSms)
{
  if (ms <= 0) return;
  uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < (uint32_t)ms)
  {
    if (serviceTick)
    {
      UpdateDeviceClockIfNeeded();
      // Check incoming SMS RX and queue
      monitorInputSmsStateMachine();
      processIncomingSmsQueue();
      // Sensor logic (non-blocking)
      PollSensorsAndDecide();
      // Legacy tick
      SetAllarmState();
      MeshNet_Tick();
    }

    if (serviceSms)
    {
      // Avoid re-entrancy while SMS state machine is busy
      if (smsState == SMS_IDLE)
      {
        SmsSender();
      }
    }

    vTaskDelay(1);
  }
}

// Backward-compatible wrapper (default: service sensors + RX, no SMS send)
void TaskDelay(int delay)
{
  String t = String(delay);
  Serial.println("delay for :  " + t + "  ms");
  TaskDelay((int)delay, true, false);
}

//      SetAllarmState           NO-OP                                                                                                     .
void SetAllarmState()
{
  //                                                         
  BlinkTick(ledBlink);
  BlinkTick(buzzBlink);
}
void SplitMobiles();

void stopAP()
{
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  MeshNet_SetLocalSsid("");
  Serial.println("[WIFI] SoftAP stopped.");
}

void StartSoftAP()
{
  if (!public_WifiEnabled)
  {
    stopAP();
    return;
  }
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPsetHostname(deviceName.c_str());
  bool ap_started = WiFi.softAP(ssidName.c_str(), ssidPassword.c_str(), MeshNet_GetMeshChannel(), false, 4);
  if (ap_started)
  {
    Serial.println("AP Started Successfully!");
    Serial.print("IP Address: http://");
    Serial.println(WiFi.softAPIP());
  }
  else
  {
    Serial.println("AP Failed to Start!");
  }
}

// helper: restart after small delay (to allow HTTP/mesh response to flush)
void restartLocalNodeDelayed(int ms)
{
  delay(ms);
  ESP.restart();
}

void applyActuatorsPolicy()
{
  if (!public_LedEnabled)
  {
    digitalWrite(AllarmLedPin, LOW);
  }
  if (!public_AlertEnabled_Buzzer)
  {
    digitalWrite(AllarmBuzzerPin, LOW);
  }
}

// ===================== SMS / GPS =====================

bool isGpsOn();
bool getGpsLocation();
void compileSms(String smsText, String num);
String ReportSmsTextGenerator(String senderNumber, String msg);
bool SenAtCommanSim808(String command, String expectedResponse, int timeout);
void SetupSim();
bool isAuthorizedNumber(String num);
void monitorInputSmsStateMachine();
void processIncomingSmsQueue();
void SmsSender();
String normalizeMobile(const String &in);

bool getSignalCSQ(int &csq, int &rssiDbm)
{
  String resp;
  if (!sendAtWait("AT+CSQ", "OK", 1200, &resp))
    return false;
  int p = resp.indexOf("+CSQ:");
  if (p < 0)
    return false;
  int comma = resp.indexOf(',', p);
  if (comma < 0)
    return false;
  String s = resp.substring(p + 5, comma);
  s.trim();
  csq = s.toInt();
  if (csq == 99)
  {
    rssiDbm = -1;
    return true;
  }
  // RSSI dBm approx: rssi = -113 + 2*csq
  rssiDbm = -113 + 2 * csq;
  return true;
}

//                                               
String boolStr(bool b);
int csqToPercent(int csq);

//                                                                          check
String buildCheckReport()
{
  //                               CSQ                       (              )
  int csq = -1, rssi = -1;
  if (getSignalCSQ(csq, rssi))
    g_lastCSQ = csq;

  String rep = "SYS:" + boolStr(public_SystemStatus)
              + " PIR:" + boolStr(public_PirEnabled)
              + " VIB:" + boolStr(public_VibEnabled)
              + " LED:" + boolStr(public_LedEnabled)
              + " BUZ:" + boolStr(public_AlertEnabled_Buzzer)
              + " ALR:" + boolStr(public_AlertEnabled_Sms)
              + " SMS:" + boolStr(public_SmsTxEnabled)
              + " WIFI:" + boolStr(public_WifiEnabled);
  if (g_lastCSQ >= 0)
  {
    int pct = csqToPercent(g_lastCSQ);
    if (pct >= 0) rep += " CSQ:" + String(pct) + "%";
  }
  //                           
  rep += " GAS:" + String(public_GasValue);
  //                                            
  rep += " TIME:" + nowTimeReadable();
  return rep;
}

void SplitMobiles()
{
  public_List_AllAlternetMobiles.clear();
  if (public_OwnerMobileNumber.length() >= 10)
    public_List_AllAlternetMobiles.push_back(public_OwnerMobileNumber);
  if (public_AlternetMobile.length() >= 10)
    public_List_AllAlternetMobiles.push_back(public_AlternetMobile);

  int start = 0;
  int index = public_AllAlternetMobiles.indexOf('*');
  while (index != -1)
  {
    String mobile = public_AllAlternetMobiles.substring(start, index);
    mobile.trim();
    if (mobile.length() > 0)
      public_List_AllAlternetMobiles.push_back(mobile);
    start = index + 1;
    index = public_AllAlternetMobiles.indexOf('*', start);
  }
  String lastMobile = public_AllAlternetMobiles.substring(start);
  lastMobile.trim();
  if (lastMobile.length() > 0)
    public_List_AllAlternetMobiles.push_back(lastMobile);
}

//                                                  (              number+code)
struct LastSend
{
  String key; // number|code
  uint32_t lastMs;
};
std::vector<LastSend> g_lastSends;

bool shouldSendThrottled(const String &number, const String &code, uint32_t windowMs = 30000)
{
  String k = number + "|" + code;
  uint32_t now = millis();
  for (auto &x : g_lastSends)
  {
    if (x.key == k)
    {
      if ((uint32_t)(now - x.lastMs) < windowMs)
        return false;
      x.lastMs = now;
      return true;
    }
  }
  g_lastSends.push_back({k, now});
  return true;
}

// ======            +                 ======
bool enqueueSms(String number, String text, int priority)
{
  auto deviceNowMs64 = []() -> uint64_t {
    uint64_t nowMs = g_lastSyncUnixMs + (uint64_t)(millis() - g_lastSyncMillis);
    return nowMs;
  };
  //                          SMS                                                                "blocked"                          
  if (!public_SmsTxEnabled)
  {
    uint32_t idb = smsLogAppend(number, detectReasonFromText(text), deviceNowMs64(), text, "blocked", priority);
    logf(1, "[SMS ] blocked by policy (SmsTxEnabled=false) id=%lu\n", (unsigned long)idb);
    return false;
  }

  if (priority < 0 || priority > 2)
    return false;
  int head = smsQueueHead[priority];
  int tail = smsQueueTail[priority];
  int nextTail = (tail + 1) % SMS_QUEUE_SIZE;
  if (nextTail == head)
  {
    logf(1, "[SMS ] queue full for priority %d\n", priority);
    uint32_t id = smsLogAppend(number, detectReasonFromText(text), deviceNowMs64(), text, "failed", priority);
    logf(1, "[LOG ] archived (failed due full queue) id=%lu\n", (unsigned long)id);
    return false;
  }

  uint32_t archId = smsLogAppend(number, detectReasonFromText(text), deviceNowMs64(), text, "queued", priority);

  smsQueues[priority][tail] = {number, text, priority, millis(), archId};
  smsQueueTail[priority] = nextTail;

  logf(1, "[ENQ ] number=%s pr=%d archId=%lu text=%s\n",
       number.c_str(), priority, (unsigned long)archId, text.c_str());
  return true;
}

void monitorInputSmsStateMachine()
{
  switch (smsRxState)
  {
  case SMS_RX_IDLE:
    if (SIM808.available())
    {
      smsRxBuffer = "";
      smsRxStart = millis();
      smsRxLastReceive = millis();
      smsRxState = SMS_RX_READING;
    }
    break;

  case SMS_RX_READING:
    while (SIM808.available())
    {
      char c = SIM808.read();
      smsRxBuffer += c;
      smsRxLastReceive = millis();
    }
    if (millis() - smsRxLastReceive > 500 && smsRxBuffer.length() > 0)
    {
      smsRxState = SMS_RX_WAITING;
    }
    break;

  case SMS_RX_WAITING:
    Serial.println("SMS_RX_READING     WAITING");
    if (smsRxBuffer.indexOf("+CDS:") != -1)
    {
      Serial.println("     Delivery report received.");
      //                   PDU                       sent/sending      delivered                
      smsLogMarkLatestDelivered();
    }
    else if (smsRxBuffer.indexOf("+CMT:") != -1)
    {
      String sender = "";
      int q1 = smsRxBuffer.indexOf("\"");
      int q2 = smsRxBuffer.indexOf("\"", q1 + 1);
      if (q1 != -1 && q2 != -1)
        sender = smsRxBuffer.substring(q1 + 1, q2);

      String text = "";
      int lastQuote = smsRxBuffer.lastIndexOf("\"");
      if (lastQuote != -1 && lastQuote + 1 < smsRxBuffer.length())
        text = smsRxBuffer.substring(lastQuote + 1);

      text.replace("\r", "");
      text.replace("\n", "");
      text.trim();
      for (int i = 0; i < text.length(); i++)
        if (text[i] < 32 || text[i] > 126)
          text[i] = ' ';

      //                           (inbox)
      uint64_t nowMs = g_lastSyncUnixMs + (uint64_t)(millis() - g_lastSyncMillis);
      inboxAppend(sender, text, nowMs);

      int nextTail = (incomingSmsTail + 1) % INCOMING_SMS_QUEUE_SIZE;
      if (nextTail != incomingSmsHead)
      {
        incomingSmsQueue[incomingSmsTail] = {sender, text};
        incomingSmsTail = nextTail;
        Serial.println("     SMS queued from " + sender + ": " + text);
      }
      else
      {
        Serial.println("       Incoming SMS queue full. Message dropped.");
      }
    }
    Serial.println("     Raw SMS Buffer: " + smsRxBuffer);
    smsRxState = SMS_RX_IDLE;
    break;
  }
}

void processIncomingSmsQueue()
{
  if (incomingSmsHead != incomingSmsTail)
  {
    IncomingSms msg = incomingSmsQueue[incomingSmsHead];
    incomingSmsHead = (incomingSmsHead + 1) % INCOMING_SMS_QUEUE_SIZE;
    String senderNorm = normalizeMobile(msg.sender);
    String ownerNorm = normalizeMobile(public_OwnerMobileNumber);
    if (senderNorm != ownerNorm)
    {
      enqueueSms(public_OwnerMobileNumber, ReportSmsTextGenerator(msg.sender, msg.text), 0);
    }
    compileSms(msg.text, msg.sender);
  }
}

//                     CSQ (0..31, 99=unknown)                                        
int csqToPercent(int csq)
{
  if (csq < 0 || csq > 31) return -1; // 99                    
  //                   0..31 -> 0..100                     
  // 31 ~ 100%
  int pct = (csq * 100 + 15) / 31; //                        
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  return pct;
}

bool sendAtWait(const String &cmd, const String &expect, int timeoutMs, String *out)
{
  // Avoid interleaving with SMS sending over the same UART
  uint32_t t0wait = millis();
  while (smsState != SMS_IDLE && (uint32_t)(millis() - t0wait) < 8000) {
    vTaskDelay(10);
  }
  SIM808.flush();
  SIM808.println(cmd);
  uint32_t t0 = millis();
  String resp;
  while (millis() - t0 < (uint32_t)timeoutMs)
  {
    while (SIM808.available())
      resp += (char)SIM808.read();
    if (resp.indexOf(expect) != -1)
      break;
    vTaskDelay(1);
  }
  if (out)
    *out = resp;
  return (resp.indexOf(expect) != -1);
}

bool isGpsOn()
{
  String resp;
  if (!sendAtWait("AT+CGNSPWR?", "OK", 1000, &resp))
    return false;
  int idx = resp.indexOf("+CGNSPWR:");
  if (idx >= 0)
  {
    int nl = resp.indexOf('\n', idx);
    String line = (nl > idx ? resp.substring(idx, nl) : resp.substring(idx));
    if (line.indexOf(": 1") != -1)
    {
      Serial.println("gps power on success... resp : " + line);
      return true;
    }
  }
  Serial.println("gps power on failed... resp : " + resp);
  return false;
}

bool getGpsLocation()
{
  if (!isGpsOn())
  {
    SenAtCommanSim808("AT+CGNSPWR=1", "OK", 1000);
    TaskDelay(1000);
  }

  String fullResp;
  if (!sendAtWait("AT+CGNSINF", "OK", 2000, &fullResp))
  {
    Serial.println("       AT+CGNSINF failed");
    return false;
  }
  Serial.println("fullResp>>>> " + fullResp);

  int p = fullResp.indexOf("+CGNSINF:");
  if (p == -1)
    return false;
  int col = fullResp.indexOf(':', p);
  if (col == -1)
    return false;
  String payload = fullResp.substring(col + 1);
  payload.trim();

  auto getToken = [&](int n) -> String
  {
    int startIdx = 0;
    for (int i = 0; i < n; i++)
    {
      int nextComma = payload.indexOf(',', startIdx);
      if (nextComma == -1)
        return String("");
      startIdx = nextComma + 1;
    }
    int endIdx = payload.indexOf(',', startIdx);
    if (endIdx == -1)
      endIdx = payload.length();
    String t = payload.substring(startIdx, endIdx);
    t.trim();
    return t;
  };

  String fixStatus = getToken(1);
  String latStr = getToken(3);
  String lonStr = getToken(4);
  Serial.println(" fixStatus     " + fixStatus + "   latStr    " + latStr + "    lonStr     " + lonStr);

  if (fixStatus == "1" && latStr.length() > 2 && lonStr.length() > 2 && latStr != "0.000000" && lonStr != "0.000000")
  {
    latitude = latStr;
    longitude = lonStr;
    Serial.println("Google Maps: https://maps.google.com/?q=" + latitude + "," + longitude);
    return true;
  }

  Serial.println("       GPS not fixed yet!");
  return false;
}

//                                                                      E164          + (                   : 98xxxxxxxxxx)
String normalizeMobile(const String &in)
{
  String d;
  d.reserve(in.length());
  for (size_t i = 0; i < in.length(); ++i)
  {
    char c = in[i];
    if (c >= '0' && c <= '9') d += c;
  }
  //        00                                   
  if (d.startsWith("00")) d.remove(0, 2);
  //             98                                             
  if (d.startsWith("98")) return d;
  //                            11               0                 98                
  if (d.length() == 11 && d.startsWith("0")) return String("98") + d.substring(1);
  //        10                 (         0)          98                
  if (d.length() == 10) return String("98") + d;
  //                                            >= 12                       10                         98                     
  if (d.length() >= 12) return String("98") + d.substring(d.length() - 10);
  return d; //                               (fallback)
}

bool isAuthorizedNumber(String num)
{
  String target = normalizeMobile(num);
  int allowedCount = public_List_AllAlternetMobiles.size();
  for (int i = 0; i < allowedCount; i++)
  {
    if (normalizeMobile(public_List_AllAlternetMobiles[i]) == target)
      return true;
  }
  return false;
}

//                                  SMS: trim + lowercase +                                           
String normalizeCommand(const String &in)
{
  String t = in;
  t.trim();
  t.toLowerCase();
  String out;
  out.reserve(t.length());
  for (size_t i = 0; i < t.length(); ++i)
  {
    char c = t[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
    //                                                                     (       '>','\r','\n',               ...)
  }
  return out;
}

String ReportSmsTextGenerator(String senderNumber, String msg)
{
  return "sender number : " + senderNumber + " ---msg--- " + msg;
}

bool SenAtCommanSim808(String command, String expectedResponse, int timeout)
{
  // Ensure we don't collide with SMS sending state-machine
  auto waitForSmsIdle = [](uint32_t timeoutMs) -> bool {
    uint32_t t0 = millis();
    while (smsState != SMS_IDLE && (uint32_t)(millis() - t0) < timeoutMs) {
      vTaskDelay(10);
    }
    return (smsState == SMS_IDLE);
  };
  waitForSmsIdle(8000);
  while (millis() - lastCommandTime < 1000)
  {
    vTaskDelay(10);
  }
  lastCommandTime = millis();

  Serial.print("Sending command: ");
  Serial.println(command);

  SIM808.flush();
  SIM808.println(command);
  unsigned long startTime = millis();
  String fullResponse = "";

  while (millis() - startTime < (uint32_t)timeout)
  {
    if (SIM808.available())
    {
      String line = SIM808.readStringUntil('\n');
      line.trim();
      if (line.length() > 0)
      {
        Serial.println("Received line: " + line);
        fullResponse += line;
        if (fullResponse.indexOf(expectedResponse) != -1)
        {
          Serial.println("    Command executed successfully.");
          TaskDelay(100);
          return true;
        }
      }
    }
  }
  Serial.println("    Failed to execute command: " + command);
  return false;
}

// =======                           (<=5               ) =======
// System: "arm", "darm" (              : "disarm"                                  )
// PIR: "piron"/"pirof"
// VIB: "vibon"/"vibof"
// LED: "ledon"/"ledof"
// BUZ: "buzon"/"buzof"
// ALR (alerts): "alron"/"alrof"
// SMS TX (      ): "smson"/"smsof"
// WIFI: "wifon"/"wifof"
// GPS: "gps"
// CHECK: "check"

String boolStr(bool b) { return b ? "on" : "off"; }

void applyWifiSetting()
{
  if (public_WifiEnabled)
    StartSoftAP();
  else
    stopAP();
}

void applyLedPolicy()
{
  if (!public_LedEnabled)
  {
    digitalWrite(AllarmLedPin, LOW);
  }
}

void applyBuzzerPolicy()
{
  if (!public_AlertEnabled_Buzzer)
  {
    digitalWrite(AllarmBuzzerPin, LOW);
  }
}

void compileSms(String smsText, String num)
{
  //                                                                                                            
  String cmd = normalizeCommand(smsText);
  //                                                       YYYYMMDD:HHmm (        : 14040816:1221)
  auto trySetTimeFromJalali = [&](const String &raw) -> bool {
    if (raw.length() != 13) return false;
    if (raw.charAt(8) != ':') return false;
    for (int i=0;i<8;i++) if (!isDigit(raw.charAt(i))) return false;
    for (int i=9;i<13;i++) if (!isDigit(raw.charAt(i))) return false;
    int jy = raw.substring(0,4).toInt();
    int jm = raw.substring(4,6).toInt();
    int jd = raw.substring(6,8).toInt();
    int hh = raw.substring(9,11).toInt();
    int mm = raw.substring(11,13).toInt();
    if (jm<1||jm>12||jd<1||jd>31||hh<0||hh>23||mm<0||mm>59) return false;

    auto jalaliToGregorian = [&](int jy,int jm,int jd,int &gy,int &gm,int &gd){
      int jy2 = jy - 979;
      int jm2 = jm - 1;
      int jd2 = jd - 1;
      long j_day_no = 365L*jy2 + (jy2/33)*8 + ((jy2%33)+3)/4;
      for (int i=0;i<jm2;i++) j_day_no += (i<6)?31:30;
      j_day_no += jd2;
      long g_day_no = j_day_no + 79;
      int gy2 = 1600 + 400*(g_day_no/146097);
      g_day_no %= 146097;
      bool leap = true;
      if (g_day_no >= 36525) {
        g_day_no--; gy2 += 100*(g_day_no/36524); g_day_no %= 36524;
        if (g_day_no >= 365) g_day_no++; else leap = false;
      }
      gy2 += 4*(g_day_no/1461);
      g_day_no %= 1461;
      if (g_day_no >= 366) { leap = false; g_day_no--; gy2 += g_day_no/365; g_day_no %= 365; }
      int gd2 = (int)g_day_no + 1;
      int dim[12] = {31, leap?29:28,31,30,31,30,31,31,30,31,30,31};
      int gm2=0; while (gm2<12 && gd2>dim[gm2]) { gd2 -= dim[gm2]; gm2++; }
      gy=gy2; gm=gm2+1; gd=gd2;
    };
    auto gregorianToJdn = [&](int y,int m,int d)->long{
      int a = (14 - m)/12; y = y + 4800 - a; m = m + 12*a - 3;
      return d + (153*m + 2)/5 + 365L*y + y/4 - y/100 + y/400 - 32045;
    };

    int gy,gm,gd; jalaliToGregorian(jy,jm,jd,gy,gm,gd);
    long jdn = gregorianToJdn(gy,gm,gd);
    const long JDN_UNIX_EPOCH = 2440588;
    long days = jdn - JDN_UNIX_EPOCH;
    long secsLocal = days*86400L + hh*3600L + mm*60L;
    long secsUtc = secsLocal - 12600L; // UTC = IRST - 3:30
    uint64_t ms = (uint64_t)secsUtc * 1000ULL;

    // Set device clock + broadcast to mesh
    ApplyClockFromMs(ms, true);

    //                    
    enqueueSms(num, String("time set @ ") + formatTimestampReadable(ms), 1);
    return true;
  };
  //                                                                                   
  if (trySetTimeFromJalali(smsText)) return;
  if (!isAuthorizedNumber(num))
  {
    if (num.length() > 0)
    {
      String msg = String("access denied for ") + cmd + " command";
      enqueueSms(num, msg, 0);
      Serial.println("    access denied " + num + " cmd=" + cmd);
      return;
    }
  }

  smsText = cmd; //                                                                                     

  //                                         (                              "disarm"                   gas)
  if (smsText != "disarm" &&
      !(smsText.startsWith("gmin") || smsText.startsWith("gmax") || smsText.startsWith("gset") || smsText.startsWith("gason") || smsText.startsWith("gasof")) &&
      smsText.length() > 5)
  {
    enqueueSms(num, "cmdlen>5", 2);
    return;
  }

  if (smsText == "gps")
  {
    SenAtCommanSim808("AT+CGNSPWR=1", "OK", 1000);
    TaskDelay(500);
    SenAtCommanSim808("AT+CGNSSEQ=RMC", "OK", 1000);
    TaskDelay(500);

    bool gpsOk = false;
    for (int attempt = 0; attempt < 3 && !gpsOk; ++attempt)
    {
      if (getGpsLocation())
      {
        gpsOk = true;
        break;
      }
      TaskDelay(1000);
    }

    if (gpsOk)
    {
      String link = "https://maps.google.com/?q=" + latitude + "," + longitude;
      if (shouldSendThrottled(num, "GPS", 30000))
        enqueueSms(num, link, 1);
    }
    else
    {
      if (shouldSendThrottled(num, "GPSNR", 30000))
        enqueueSms(num, "GPS not ready", 1);
    }
    return;
  }

  if (smsText == "arm")
  {
    public_SystemStatus = 1;
    prefs.putString("SystemEnabled", "true");
    if (shouldSendThrottled(num, "ARM", 15000))
      enqueueSms(num, "system is arm", 1);
    return;
  }
  if (smsText == "darm" || smsText == "disarm")
  {
    public_SystemStatus = 0;
    prefs.putString("SystemEnabled", "false");
    if (shouldSendThrottled(num, "DISARM", 15000))
      enqueueSms(num, "system is disarm", 1);
    return;
  }

  //               
  if (smsText == "piron")
  {
    public_PirEnabled = true;
    prefs.putString("PirEnabled", "true");
    enqueueSms(num, "pir:on", 2);
    return;
  }
  if (smsText == "pirof")
  {
    public_PirEnabled = false;
    prefs.putString("PirEnabled", "false");
    enqueueSms(num, "pir:off", 2);
    return;
  }
  if (smsText == "vibon")
  {
    public_VibEnabled = true;
    prefs.putString("VibEnabled", "true");
    enqueueSms(num, "vib:on", 2);
    return;
  }
  if (smsText == "vibof")
  {
    public_VibEnabled = false;
    prefs.putString("VibEnabled", "false");
    enqueueSms(num, "vib:off", 2);
    return;
  }

  //                  
  if (smsText == "ledon")
  {
    public_LedEnabled = true;
    prefs.putString("LedEnabled", "true");
    applyLedPolicy();
    enqueueSms(num, "led:on", 2);
    return;
  }
  if (smsText == "ledof")
  {
    public_LedEnabled = false;
    prefs.putString("LedEnabled", "false");
    applyLedPolicy();
    enqueueSms(num, "led:off", 2);
    return;
  }
  if (smsText == "buzon")
  {
    public_AlertEnabled_Buzzer = true;
    prefs.putString("BuzzerEnabled", "true");
    applyBuzzerPolicy();
    enqueueSms(num, "buz:on", 2);
    return;
  }
  if (smsText == "buzof")
  {
    public_AlertEnabled_Buzzer = false;
    prefs.putString("BuzzerEnabled", "false");
    applyBuzzerPolicy();
    enqueueSms(num, "buz:off", 2);
    return;
  }

  //                                  (              )
  if (smsText == "alron")
  {
    public_AlertEnabled_Sms = true;
    prefs.putString("SmsAlertEnabled", "true");
    enqueueSms(num, "alerts:on", 2);
    return;
  }
  if (smsText == "alrof")
  {
    public_AlertEnabled_Sms = false;
    prefs.putString("SmsAlertEnabled", "false");
    enqueueSms(num, "alerts:off", 2);
    return;
  }

  //                   SMS
  if (smsText == "smson")
  {
    public_SmsTxEnabled = true;
    prefs.putString("SmsTxEnabled", "true");
    enqueueSms(num, "sms:on", 2);
    return;
  }
  if (smsText == "smsof")
  {
    public_SmsTxEnabled = false;
    prefs.putString("SmsTxEnabled", "false"); /*                                                        */
    return;
  }

  // WiFi
  if (smsText == "wifon")
  {
    public_WifiEnabled = true;
    prefs.putString("WifiEnabled", "true");
    StartSoftAP();
    enqueueSms(num, "wifi:on", 2);
    return;
  }
  if (smsText == "wifof")
  {
    public_WifiEnabled = false;
    prefs.putString("WifiEnabled", "false");
    stopAP();
    enqueueSms(num, "wifi:off", 2);
    return;
  }

  // Gas sensor controls
  if (smsText == "gason")
  {
    public_GasEnabled = true;
    prefs.putString("GasEnabled", "true");
    enqueueSms(num, "gas:on", 2);
    return;
  }
  if (smsText == "gasof")
  {
    public_GasEnabled = false;
    prefs.putString("GasEnabled", "false");
    enqueueSms(num, "gas:off", 2);
    return;
  }
  if (smsText.startsWith("gmin"))
  {
    int v = smsText.substring(4).toInt();
    if (v >= 0 && v <= 4095)
    {
      public_GasMin = v;
      prefs.putString("GasMin", String(v));
      enqueueSms(num, String("gas:min=") + String(v), 2);
    }
    else
    {
      enqueueSms(num, "gas:min invalid", 2);
    }
    return;
  }
  if (smsText.startsWith("gmax"))
  {
    int v = smsText.substring(4).toInt();
    if (v >= 0 && v <= 4095)
    {
      public_GasMax = v;
      prefs.putString("GasMax", String(v));
      enqueueSms(num, String("gas:max=") + String(v), 2);
    }
    else
    {
      enqueueSms(num, "gas:max invalid", 2);
    }
    return;
  }

  if (smsText == "help")
  {
    String msg =
        "cmds: gps, arm, darm, piron, pirof, vibon, vibof, "
        "ledon, ledof, buzon, buzof, alron, alrof, smson, smsof, "
        "wifon, wifof, check, help, YYYYMMDD:HHmm";
    enqueueSms(num, msg, 2);
    return;
  }
  if (smsText == "check")
  {
    String rep = buildCheckReport();
    enqueueSms(num, rep, 1);
    return;
  }

  //                 
  enqueueSms(num, "Unknown command: " + smsText, 2);
}

void SetupSim()
{
  SIM808.begin(9600, SERIAL_8N1, SIM808_RX, SIM808_TX);
  TaskDelay(5000);

  Serial.println("Initializing SIM808...");

  if (!SenAtCommanSim808("AT", "OK", 1000))
    return;
  TaskDelay(2000);
  if (!SenAtCommanSim808("AT+CGNSPWR=1", "OK", 2000))
    return;
  TaskDelay(2000);
  if (!SenAtCommanSim808("AT+CGNSSEQ=RMC", "OK", 2000))
    return;
  if (!SenAtCommanSim808("AT+CGNSINF", "OK", 2000))
    return;
  if (!SenAtCommanSim808("ATE0", "OK", 1000))
    return;
  if (!SenAtCommanSim808("AT+CPIN?", "READY", 2000))
    return;
  if (!SenAtCommanSim808("AT+CREG?", "0,1", 3000))
    return;
  if (!SenAtCommanSim808("AT+CSQ", "OK", 1000))
    return;
  if (!SenAtCommanSim808("AT+CMGF=1", "OK", 1000))
    return;

  // Request SMS Status Reports via Text mode
  // fo=49 enables SR request bit (TP-SRR), rest values keep common settings.
  if (!SenAtCommanSim808("AT+CSMP=49,167,0,0", "OK", 1000))
    return;

  // Push incoming SMS to TE and also push status reports (+CDS) to TE (ds=1)
  if (!SenAtCommanSim808("AT+CNMI=2,2,0,1,0", "OK", 1000))
    return;

  Serial.println("    SIM808 Initialized Successfully!");
  public_SimIsOnline = true;
  //                                           +                                 
  enqueueSms(public_OwnerMobileNumber, String("device setup is down! | ") + buildCheckReport(), 2);
}

// ===================== Sensors & State Machines =====================

struct PirSensor
{
  int pin;
  String name;
  bool isAnalog;
  unsigned long lastDebounceMs; //                           
  PirSensor() : pin(-1), name(""), isAnalog(false), lastDebounceMs(0) {}
  PirSensor(int p, const char *n, bool analog = false) : pin(p), name(String(n)), isAnalog(analog), lastDebounceMs(0) {}
};

//               : 15 (        )   18 (PIR)
PirSensor sensors[] = {PirSensor(VIB_PIN, "sensor_vibration"), PirSensor(PIR_PIN, "sensor_pir")};
const int sensorCount = sizeof(sensors) / sizeof(sensors[0]);

//                                                                                            
const uint32_t WINDOW_MS = 2000;
const uint8_t MAX_EVENTS_PER_SENSOR = 16;
const uint8_t ALARM_THRESHOLD = 7;
uint32_t g_eventTimes[2][MAX_EVENTS_PER_SENSOR]; // [sensorIndex][slot]
uint8_t g_eventCount[2] = {0, 0};
bool g_eventCapped[2] = {false, false};
const uint32_t DEBOUNCE_MS = 80; //                  

// Forward declarations for window helpers
void pruneWindow(uint8_t idx);
uint8_t windowCount(uint8_t idx);

//                         LED/Buzzer (                 )
struct BlinkSM
{
  int pin;
  bool active;
  bool level;
  uint32_t lastToggle;
  uint32_t onMs;
  uint32_t offMs;
  uint32_t stopAt; //        0                                                            
};

BlinkSM ledSM = {AllarmLedPin, false, LOW, 0, 500, 500, 0};
BlinkSM buzzSM = {AllarmBuzzerPin, false, LOW, 0, 500, 500, 0};

void smStart(BlinkSM &sm, uint32_t onMs, uint32_t offMs, uint32_t durationMs)
{
  sm.onMs = onMs;
  sm.offMs = offMs;
  sm.active = true;
  sm.level = HIGH;
  sm.lastToggle = millis();
  sm.stopAt = (durationMs > 0) ? (millis() + durationMs) : 0;
  digitalWrite(sm.pin, sm.level);
}

void smStop(BlinkSM &sm)
{
  sm.active = false;
  sm.level = LOW;
  sm.lastToggle = millis();
  sm.stopAt = 0;
  digitalWrite(sm.pin, LOW);
}

void smTick(BlinkSM &sm)
{
  if (!sm.active)
    return;
  uint32_t now = millis();
  if (sm.stopAt && (int32_t)(sm.stopAt - now) <= 0)
  {
    smStop(sm);
    return;
  }
  uint32_t span = sm.level ? sm.onMs : sm.offMs;
  if ((uint32_t)(now - sm.lastToggle) >= span)
  {
    sm.level = !sm.level;
    sm.lastToggle = now;
    digitalWrite(sm.pin, sm.level ? HIGH : LOW);
  }
}

//                                                     
void recordEvent(uint8_t idx)
{
  uint32_t now = millis();
  if ((uint32_t)(now - sensors[idx].lastDebounceMs) < DEBOUNCE_MS)
    return;
  sensors[idx].lastDebounceMs = now;

  if (g_eventCount[idx] < MAX_EVENTS_PER_SENSOR)
    g_eventTimes[idx][g_eventCount[idx]++] = now;
  else
  {
    for (uint8_t i = 1; i < MAX_EVENTS_PER_SENSOR; ++i)
      g_eventTimes[idx][i - 1] = g_eventTimes[idx][i];
    g_eventTimes[idx][MAX_EVENTS_PER_SENSOR - 1] = now;
  }

  // Trace: which sensor fired and how many times inside the active window.
  uint8_t inWindow = windowCount(idx);
  bool capped = inWindow >= MAX_EVENTS_PER_SENSOR;
  if (capped)
  {
    inWindow = MAX_EVENTS_PER_SENSOR;
    g_eventCount[idx] = MAX_EVENTS_PER_SENSOR;
  }
  if (!capped || !g_eventCapped[idx])
  {
    logf(1, "[SENS] %s trigger recorded -> %u in last %u ms%s\n",
         sensors[idx].name.c_str(),
         inWindow,
         (unsigned)WINDOW_MS,
         capped ? " (capped)" : "");
  }
  g_eventCapped[idx] = capped;
}

//                                                 
void pruneWindow(uint8_t idx)
{
  uint32_t now = millis();
  uint8_t w = 0;
  for (uint8_t i = 0; i < g_eventCount[idx]; ++i)
    if ((uint32_t)(now - g_eventTimes[idx][i]) <= WINDOW_MS)
      g_eventTimes[idx][w++] = g_eventTimes[idx][i];
  g_eventCount[idx] = w;
}

uint8_t windowCount(uint8_t idx)
{
  pruneWindow(idx);
  return g_eventCount[idx];
}

//                    :
//         :                 >=4        s                
//         :            >=5                                                                      4               
bool shouldAlarm(uint8_t vCount, uint8_t pCount)
{
  return (vCount + pCount) >= ALARM_THRESHOLD;
}

//                                            LED                                                               .
uint32_t ledExtendUntilMs = 0;
const uint32_t LED_EXTEND_MS = 2000;
const uint32_t BUZZER_ALARM_MS = 4000;

uint32_t alarmCooldownUntilMs = 0;

void PollSensorsAndDecide()
{
  bool gasAlarmNow = false;
bool tempAlarmNow = false;
bool humAlarmNow  = false;
bool dhtReadingValid = false;
#if HAS_GAS
  int gasRaw = analogRead(GasAnalogPin);
  public_GasValue = (public_GasValue * 7 + gasRaw) / 8;
  bool gasReadingValid = public_GasEnabled && gasRaw > 50; // avoid phantom 0 when sensor not attached
  if (gasReadingValid) {
    gasAlarmNow = (public_GasValue < public_GasMin) || (public_GasValue > public_GasMax);
  }
#endif

#if HAS_DHT
  // Periodic DHT read + log (independent of Arm state)
  static uint32_t _lastDhtRead = 0;
  if (public_DhtEnabled && (millis() - _lastDhtRead > 2000)) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) { public_TempValue = t; dhtReadingValid = true; }
    if (!isnan(h)) { public_HumValue  = h; dhtReadingValid = true; }
    _lastDhtRead = millis();
  }
  if (public_DhtEnabled && dhtReadingValid) {
    tempAlarmNow = (public_TempValue < public_TempMin) || (public_TempValue > public_TempMax);
    humAlarmNow  = (public_HumValue  < public_HumMin)  || (public_HumValue  > public_HumMax);
  }
#endif

  uint32_t now = millis();

  bool remoteAlarmActive = false;
  String remoteCause;
  bool remoteRequiresSms = false;
  bool remoteRequiresSiren = false;
  if (g_remoteAlarm.pending)
  {
    if ((int32_t)(now - g_remoteAlarm.expireAtMs) < 0)
    {
      remoteAlarmActive = true;
      remoteCause = g_remoteAlarm.description;
      remoteRequiresSms = g_remoteAlarm.requiresSms;
      remoteRequiresSiren = g_remoteAlarm.requiresSiren;
    }
    else
    {
      g_remoteAlarm.pending = false;
    }
  }

  if (!public_SystemStatus && !remoteAlarmActive)
    return;

  //                           /                                      
  int vibState = public_VibEnabled ? digitalRead(sensors[0].pin) : LOW;
  int pirState = public_PirEnabled ? digitalRead(sensors[1].pin) : LOW;

  if (vibState == HIGH)
    recordEvent(0);
  if (pirState == HIGH)
    recordEvent(1);

  uint8_t V = windowCount(0);
  uint8_t P = windowCount(1);

  // gasAlarmNow computed at function start

  bool localAlarmNow = shouldAlarm(V, P) || gasAlarmNow || tempAlarmNow || humAlarmNow;
  bool alarmNow = localAlarmNow || remoteAlarmActive;
  String localCauseDetail;
  String localCauseKey = "SENS";
#if HAS_GAS
  if (gasAlarmNow)
  {
    localCauseKey = "GAS";
    localCauseDetail = String("GAS ") + String(public_GasValue) + " in[" + String(public_GasMin) + "," + String(public_GasMax) + "]";
  }
  else
#endif
#if HAS_DHT
  if (tempAlarmNow)
  {
    localCauseKey = "TEMP";
    localCauseDetail = String("TEMP ") + String(public_TempValue, 1) + "C in[" + String(public_TempMin) + "," + String(public_TempMax) + "]";
  }
  else if (humAlarmNow)
  {
    localCauseKey = "HUM";
    localCauseDetail = String("HUM ") + String((int)public_HumValue) + "% in[" + String(public_HumMin) + "," + String(public_HumMax) + "]";
  }
  else
#endif
  {
    if ((V >= 4 && P < 4))
    {
      localCauseKey = "VIB";
      localCauseDetail = "VIB";
    }
    else if ((P >= 4 && V < 4))
    {
      localCauseKey = "PIR";
      localCauseDetail = "PIR";
    }
    else
    {
      localCauseKey = "BOTH";
      localCauseDetail = "BOTH";
    }
    localCauseDetail += String(" V=") + String(V) + " P=" + String(P);
  }
  static bool prevLocalAlarm = false;
  if (localAlarmNow && !prevLocalAlarm)
  {
    String combo;
    if (gasAlarmNow || tempAlarmNow || humAlarmNow)
    {
      combo = localCauseDetail;
    }
    else if (V > 0 && P == 0)
    {
      combo = String("Vibration only x") + String(V);
    }
    else if (P > 0 && V == 0)
    {
      combo = String("PIR only x") + String(P);
    }
    else
    {
      combo = String("Vibration x") + String(V) + " + PIR x" + String(P);
    }
    logf(1, "[ALRM] Local trigger: %s | total=%u (threshold=%u, window=%u ms)\n",
         combo.c_str(), (unsigned)(V + P), (unsigned)ALARM_THRESHOLD, (unsigned)WINDOW_MS);
  }
  prevLocalAlarm = localAlarmNow;

  // ---                       ---

  if (alarmNow)
  {
    bool driveBuzzer = (localAlarmNow && public_AlertEnabled_Buzzer) ||
                       (remoteAlarmActive && remoteRequiresSiren && public_AlertEnabled_Buzzer);
    bool sendSmsNow = (localAlarmNow && public_AlertEnabled_Sms) ||
                      (remoteAlarmActive && remoteRequiresSms && public_AlertEnabled_Sms);

    if (driveBuzzer && !buzzSM.active && (int32_t)(now - alarmCooldownUntilMs) >= 0)
    {
      logf(1, "[ALRM] START buzzer (on/off=500/500ms, duration=%ums). Reset LED & window. Set cooldown.\n",
           (unsigned)BUZZER_ALARM_MS);

      smStart(buzzSM, 500, 500, BUZZER_ALARM_MS);
      smStop(ledSM);

      alarmCooldownUntilMs = now + BUZZER_ALARM_MS + 1000;

      if (localAlarmNow)
      {
        g_eventCount[0] = 0;
        g_eventCount[1] = 0;
        g_eventCapped[0] = false;
        g_eventCapped[1] = false;
        logf(1, "[ALRM] Alarm handled, window counts reset (V=0, P=0) at %lu ms\n", (unsigned long)now);
      }
    }

    if (sendSmsNow)
    {
      String throttleKey = remoteAlarmActive ? "REMOTE" : localCauseKey;
      for (auto &num : public_List_AllAlternetMobiles)
      {
        if (shouldSendThrottled(num, throttleKey, 30000))
        {
          String msg;
          if (localAlarmNow)
          {
            msg = (gasAlarmNow || tempAlarmNow || humAlarmNow)
                      ? (String("ALARM:") + localCauseDetail + " @ " + nowTimeReadable())
                      : (String("ALARM:") + localCauseDetail + " @ " + nowTimeReadable());
          }
          else
          {
            msg = String("NET:") + remoteCause + " @ " + nowTimeReadable();
          }
          if (remoteAlarmActive && localAlarmNow)
            msg = String("ALARM:") + localCauseDetail + " + NET:" + remoteCause + " @ " + nowTimeReadable();

          logf(1, "[SMS ] enqueue to %s: %s\n", num.c_str(), msg.c_str());
          enqueueSms(num, msg, 0);
        }
        else
        {
          logf(1, "[SMS ] throttled (skip) for %s (ALARM)\n", num.c_str());
        }
      }
    }

    if (localAlarmNow && (uint32_t)(now - g_lastNetworkAlarmMs) > kNetworkAlarmMinIntervalMs)
    {
      uint64_t ts = g_deviceNowMs ? g_deviceNowMs : (uint64_t)now;
      MeshNet_RecordNetworkEvent("ALARM", localCauseDetail, public_AlertEnabled_Sms, public_AlertEnabled_Buzzer, 8, ts);
      g_lastNetworkAlarmMs = now;
    }

    if (remoteAlarmActive)
      g_remoteAlarm.pending = false;
  }
  else
  {
    //                                    LED                                                        LED                    
    if ((vibState == HIGH || pirState == HIGH || V > 0 || P > 0) && !buzzSM.active && public_LedEnabled)
    {
      if (!ledSM.active)
      {
        smStart(ledSM, 500, 500, 0);
      }
      ledExtendUntilMs = now + LED_EXTEND_MS;
    }
  }

  //        LED                                        
  if (!public_LedEnabled && ledSM.active)
  {
    smStop(ledSM);
  }

  //                     LED                        s (                                 )
  if (ledSM.active && (int32_t)(now - ledExtendUntilMs) > 0 && buzzSM.active == false)
  {
    smStop(ledSM);
  }
}

// ======                SMS                                                    ======
void SmsSender()
{
  if (smsState == SMS_IDLE)
  {
    int selectedPriority = -1;
    int selectedIndex = -1;
    unsigned long oldestTime = ULONG_MAX;
    for (int p = 0; p < 3; p++)
    {
      int head = smsQueueHead[p];
      int tail = smsQueueTail[p];
      while (head != tail)
      {
        SmsMessage &msg = smsQueues[p][head];
        if (msg.timestamp < oldestTime)
        {
          oldestTime = msg.timestamp;
          selectedPriority = p;
          selectedIndex = head;
        }
        head = (head + 1) % SMS_QUEUE_SIZE;
      }
    }
    if (selectedIndex != -1)
    {
      currentPriority = selectedPriority;
      currentMessage = smsQueues[selectedPriority][selectedIndex];
      smsQueueHead[selectedPriority] = (selectedIndex + 1) % SMS_QUEUE_SIZE;

      smsLogUpdateStatus(currentMessage.archiveId, "sending");

      smsState = SMS_INIT;
      smsTimer = millis();
      smsSendRespBuf = "";
      return;
    }
    return;
  }

  switch (smsState)
  {
  case SMS_INIT:
    SIM808.println("AT+CMGF=1");
    smsTimer = millis();
    TaskDelay(100);
    smsState = SMS_SEND_HEADER;
    break;

  case SMS_SEND_HEADER:
    if (millis() - smsTimer > 500)
    {
      SIM808.print("AT+CMGS=\"");
      SIM808.print(currentMessage.number);
      SIM808.println("\"");
      smsTimer = millis();
      TaskDelay(100);
      smsState = SMS_SEND_BODY;
    }
    break;

  case SMS_SEND_BODY:
    if (millis() - smsTimer > 500)
    {
      SIM808.print(currentMessage.text);
      smsTimer = millis();
      TaskDelay(100);
      smsState = SMS_SEND_CTRLZ;
    }
    break;

  case SMS_SEND_CTRLZ:
    if (millis() - smsTimer > 500)
    {
      TaskDelay(1000);
      SIM808.write(26);
      smsTimer = millis();
      TaskDelay(100);
      smsSendRespBuf = "";
      smsState = SMS_WAIT_RESPONSE;
    }
    break;

  case SMS_WAIT_RESPONSE:
    // Collect response from modem, detect success/failure earlier than timeout
    while (SIM808.available())
    {
      char c = (char)SIM808.read();
      smsSendRespBuf += c;
    }

    if (smsSendRespBuf.indexOf("ERROR") != -1 || smsSendRespBuf.indexOf("+CMS ERROR") != -1)
    {
      Serial.printf("    SMS failed to %s: %s\n", currentMessage.number.c_str(), currentMessage.text.c_str());
      smsLogUpdateStatus(currentMessage.archiveId, "failed");
      currentPriority = -1;
      smsState = SMS_IDLE;
      break;
    }

    if (smsSendRespBuf.indexOf("+CMGS:") != -1 && smsSendRespBuf.indexOf("OK") != -1)
    {
      Serial.printf("     SMS accepted by modem (CMGS) to %s: %s\n", currentMessage.number.c_str(), currentMessage.text.c_str());
      smsLogOnSent(currentMessage.archiveId);
      currentPriority = -1;
      smsState = SMS_IDLE;
      break;
    }

    if (millis() - smsTimer > 15000)
    {
      // Fallback: consider as sent after timeout
      Serial.printf("     SMS sent (timeout fallback) to %s: %s\n", currentMessage.number.c_str(), currentMessage.text.c_str());
      smsLogOnSent(currentMessage.archiveId);
      currentPriority = -1;
      smsState = SMS_IDLE;
    }
    break;
  }
}

// ===================== Setup / Loop =====================

NodeCapabilities buildLocalCaps()
{
  NodeCapabilities caps;
  caps.hasSim = true;
  caps.smsAlertEnabled = public_AlertEnabled_Sms;
  caps.smsTxEnabled = public_SmsTxEnabled;
  caps.hasBuzzer = public_AlertEnabled_Buzzer;
  caps.hasLed = public_LedEnabled;
  caps.hasPir = public_PirEnabled;
  caps.hasVib = public_VibEnabled;
  caps.hasGas = public_GasEnabled;
#if HAS_DHT
  caps.hasDht = public_DhtEnabled;
#else
  caps.hasDht = false;
#endif
  caps.wifiEnabled = public_WifiEnabled;
  uint8_t sensors = 0;
  if (public_PirEnabled) sensors++;
  if (public_VibEnabled) sensors++;
  if (public_GasEnabled) sensors++;
#if HAS_DHT
  if (public_DhtEnabled) sensors++;
#endif
  caps.sensorCount = sensors;
  return caps;
}

void EnsurePreferencesFresh()
{
  String currentHash = ESP.getSketchMD5();
  if (!currentHash.length())
    currentHash = "nohash";
  const String buildMarker = String(__DATE__) + " " + String(__TIME__);
  String storedHash = prefs.isKey("fw_hash") ? prefs.getString("fw_hash", "") : "";
  String storedBuild = prefs.isKey("fw_build") ? prefs.getString("fw_build", "") : "";
  bool firmwareChanged = (storedHash != currentHash) || (storedBuild != buildMarker);
  if (firmwareChanged || FORCE_CLEAR_PREFS)
  {
    logf(1, "[CFG] Firmware/upload detected (hash %s -> %s, build %s -> %s). Clearing prefs.\n",
         storedHash.c_str(), currentHash.c_str(),
         storedBuild.c_str(), buildMarker.c_str());
    prefs.clear();
    prefsClock.clear();
    prefs.putString("fw_hash", currentHash);
    prefs.putString("fw_build", buildMarker);

    if (FORCE_CLEAR_PREFS)
    {
      uint64_t mac = ESP.getEfuseMac();
      String freshSsid = String("ElixMesh_") + formatMacCompact(mac);
      prefs.putString("wifi_Ssid_Name", freshSsid);
      prefs.putString("Ssid_Password", ssidPasswordDefault);
      prefs.putString("deviceName", String("Node_") + formatMacCompact(mac));
    }
  }
}

void SetPublicVariablesFromPrefs()
{
  //                                                  
  for (int i = 0; i < numKeys; i++)
  {
    if (!prefs.isKey(defaultKeys[i].key.c_str()))
      prefs.putString(defaultKeys[i].key.c_str(), defaultKeys[i].defaultVal);
  }

  //             
  if (!prefs.isKey("username"))
    prefs.putString("username", "admin");
  if (!prefs.isKey("password"))
    prefs.putString("password", "1234");

  username = prefs.getString("username", "admin");
  userPassword = prefs.getString("password", "1234");

  // WiFi creds + identity
  uint64_t mac = ESP.getEfuseMac();
  ssidName = prefs.getString("wifi_Ssid_Name", String("elix_Node_") + formatMacShort(mac));
  ssidPassword = prefs.getString("Ssid_Password", ssidPasswordDefault);
  meshName = prefs.getString("mesh_name", meshNameDefault);
  meshPassword = prefs.getString("mesh_pass", meshPasswordDefault);
  deviceName = prefs.getString("deviceName", "");

  String trimmedDevice = deviceName;
  trimmedDevice.trim();
  if (trimmedDevice.length() < 3)
  {
    trimmedDevice = String("Node_") + formatMacCompact(mac);
    prefs.putString("deviceName", trimmedDevice);
  }
  deviceName = trimmedDevice;

  // SoftAP SSID per node
  String trimmedSsid = ssidName;
  trimmedSsid.trim();
  // اگر خالی یا همان مقدار پیش‌فرض قدیمی بود، با مک مقداردهی کن
  if (trimmedSsid.length() < 4 || trimmedSsid == ssidNameDefault)
  {
    trimmedSsid = String("ElixMesh_") + formatMacCompact(mac);
    prefs.putString("wifi_Ssid_Name", trimmedSsid);
  }
  ssidName = trimmedSsid;

  if (ssidPassword.length() < 8)
  {
    ssidPassword = ssidPasswordDefault;
    prefs.putString("Ssid_Password", ssidPassword);
  }
  MeshNet_SetFriendlyName(deviceName);
  MeshNet_SetLocalSsid(meshName);
  MeshNet_SetAuth(meshName, meshPassword);

  public_SystemStatus = prefs.getString("SystemEnabled", "true") == "true";
  public_PirEnabled = false;
#if HAS_PIR
  public_PirEnabled = prefs.getString("PirEnabled", "true") == "true";
#endif
  public_VibEnabled = false;
#if HAS_VIB
  public_VibEnabled = prefs.getString("VibEnabled", "true") == "true";
#endif
  public_LedEnabled = prefs.getString("LedEnabled", "true") == "true";
  public_AlertEnabled_Buzzer = prefs.getString("BuzzerEnabled", "false") == "true";
  public_AlertEnabled_Sms = prefs.getString("SmsAlertEnabled", "true") == "true";
  public_SmsTxEnabled = prefs.getString("SmsTxEnabled", "true") == "true";
  public_WifiEnabled = prefs.getString("WifiEnabled", "true") == "true";

  // Gas sensor prefs
  public_GasEnabled = false;
#if HAS_GAS
  public_GasEnabled = prefs.getString("GasEnabled", "true") == "true";
#endif
  public_GasMin = 0;
#if HAS_GAS
  public_GasMin = prefs.getString("GasMin", "300").toInt();
#endif
  public_GasMax = 0;
#if HAS_GAS
  public_GasMax = prefs.getString("GasMax", "2500").toInt();
#endif

#if HAS_DHT
  public_DhtEnabled = prefs.getString("DhtEnabled", "true") == "true";
  public_TempMin = prefs.getString("TempMin", "10").toInt();
  public_TempMax = prefs.getString("TempMax", "40").toInt();
  public_HumMin  = prefs.getString("HumMin",  "20").toInt();
  public_HumMax  = prefs.getString("HumMax",  "80").toInt();
#endif

  public_OwnerMobileNumber = prefs.getString("OwnerMobile", "09127917347");
  public_AlternetMobile = prefs.getString("alternetMobile", "");
  public_AllAlternetMobiles = prefs.getString("AlternetMobiles", "");

  Serial.println("ssidName :  " + ssidName + "    password    :    " + ssidPassword);
  Serial.println("public_OwnerMobileNumber :  " + public_OwnerMobileNumber);
  SplitMobiles();

  applyActuatorsPolicy();
  MeshNet_UpdateLocalCapabilities(buildLocalCaps());
}

// removed stray JS block that broke C++ compilation
void setup()
{
  Serial.begin(9600);
  setLogEnabled(1); // default: enable serial tracing, can be toggled with 0/1
  Serial.println("Starting ESP32 AP...");

  prefs.begin("config", false);
  prefs.clear();
  prefsClock.begin("clock", false);
  EnsurePreferencesFresh();

  //              
  pinMode(AllarmLedPin, OUTPUT);
  pinMode(AllarmBuzzerPin, OUTPUT);

  for (int i = 0; i < sensorCount; i++)
    if (sensors[i].pin > 0) pinMode(sensors[i].pin, INPUT_PULLDOWN);
#if HAS_GAS
  pinMode(GasAnalogPin, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(GasAnalogPin, ADC_11db);
#endif

#if HAS_DHT
  dht.begin();
#endif

  SetPublicVariablesFromPrefs();
  uint64_t mac = ESP.getEfuseMac();
  Serial.printf("[MESH] Local node name=%s (me) short=%s mac=%s SSID=%s\n",
                deviceName.c_str(),
                formatMacShort(mac).c_str(),
                formatMacFull(mac).c_str(),
                ssidName.c_str());
  StartSoftAP();
  MeshNet_Init(handleMeshEvent);
  MeshNet_SetTimeProvider(DeviceNowMs);
  MeshNet_SetClockSetter(ApplyClockFromMs);
  MeshNet_UpdateLocalCapabilities(buildLocalCaps());
  HtmlFunctions();
  SetupSim();

  //                         sync         
  epochStartTime = prefs.getULong("epochStartTime", 0);
  if (epochStartTime != 0)
  {
    g_lastSyncUnixMs = (uint64_t)epochStartTime;
    g_lastSyncMillis = millis();
    g_deviceNowMs = g_lastSyncUnixMs;
    printTimestampReadable(g_deviceNowMs);
  }
}

void loop()
{
  UpdateDeviceClockIfNeeded();
  MeshNet_Tick();
  static uint32_t lastCapsPush = 0;
  if ((uint32_t)(millis() - lastCapsPush) > 5000)
  {
    MeshNet_UpdateLocalCapabilities(buildLocalCaps());
    lastCapsPush = millis();
  }

  // SMS RX/TX state machines
  monitorInputSmsStateMachine();
  processIncomingSmsQueue();
  SmsSender();

  // LED/Buzzer state machines tick
  smTick(ledSM);
  smTick(buzzSM);

  // Sensor logic + decision (non-blocking)
  PollSensorsAndDecide();

  //                                                                                
  SmsRetryTick();

  //                                           (                     )
  SetAllarmState();
}




















