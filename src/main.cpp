#include <AsyncJson.h>
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <vector>
#include "DeviceConfig.h"
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

// ---- Time formatting (UNCHANGED: +12600 and localtime) ----
void printTimestampReadable(uint64_t timestampMs)
{
  time_t timestampSec = (timestampMs / 1000) + 12600; // UTC+3:30         
  struct tm *timeinfo = localtime(&timestampSec);
  strftime(lastOktime, sizeof(lastOktime), "%Y-%m-%d %H:%M:%S", timeinfo);
  Serial.print(" ok time is : ");
  Serial.println(lastOktime);
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
Preferences prefs;
Preferences prefsClock;

// ---            /                       ---
String ssidNameDefault = "ElixHome";
String ssidPasswordDefault = "12345678";
String ssidName;
String ssidPassword;
String username = "admin";
String userPassword = "1234";

// ===================== Config Keys =====================

struct ConfigKey
{
  String key;
  String title; // caption (fa-IR)
  String type; // string, int, bool, dropdown, mobile
  String defaultVal;
  String min;
  String max;
  String options; //          dropdown: "on,off"
  bool isSystem;
};

//                           :                                                    +                                    
ConfigKey defaultKeys[] = {
    {"wifi_Ssid_Name",   "                WiFi (SSID)",     "string", ssidNameDefault,      "2",  "10",  "",            false},
    {"Ssid_Password",    "       WiFi",                  "string", ssidPasswordDefault,  "3",  "20",  "",            false},

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

// ===================== HTML =====================

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en" dir="ltr">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0"/>
  <title>Device Settings</title>
  <style>
    :root { --p:#3498db; --d:#e74c3c; --s:#2ecc71; --bg:#1a1a1a; --card:#2d2d2d; --text:#eee; }
    body { font-family: Tahoma; background:var(--bg); color:var(--text); margin:0; }
    .container { max-width: 1100px; margin: 20px auto; padding: 20px; }
    .card { background:var(--card); border-radius:12px; padding:20px; margin-bottom:20px; box-shadow:0 4px 12px rgba(0,0,0,0.3); }
    .card2 { background:#047445ff; border-radius:12px; padding:20px; margin-bottom:20px; box-shadow:0 4px 12px rgba(0,0,0,1); }
    h1,h2 { text-align:center; color:var(--p); }
    input, select, button { padding:10px; margin:5px 0; border-radius:8px; width:100%; border:1px solid #555; background:rgba(255,255,255,0.1); color:var(--text); }
    button { background:var(--p); color:#fff; border:none; cursor:pointer; font-weight:bold; }
    .btn-d { background:var(--d); }
    .btn-s { background:var(--s); }
    .grid { display:grid; grid-template-columns: repeat(auto-fill, minmax(280px,1fr)); gap:15px; }
    .key-card { border:1px solid #444; border-radius:8px; padding:15px; }
    .system { border-left:5px solid var(--p); }
    .hidden { display:none; }
    .error { color:var(--d); font-size:0.9em; }
    .mobile-input { direction:ltr; text-align:left; font-family:monospace; }
    table { width:100%; border-collapse: collapse; font-size: 0.9rem; }
    th, td { border-bottom:1px solid #444; padding:8px 6px; text-align:right; }
    th { position: sticky; top: 0; background:#222; }
    .table-wrap { max-height: 420px; overflow:auto; border:1px solid #444; border-radius:8px; }
    .mono { direction:ltr; text-align:left; font-family:monospace; }
    .badge { padding:3px 8px; border-radius:8px; font-size:0.8rem; }
    .q { background:#555; }
    .s { background:#2c7; }
    .f { background:#c44; }
    /* Hide any stray inbox block outside #main */
    body > .container:not(#main) #inbox { display: none; }
  </style>
</head>
<body>

<div id="login" class="container">
  <div class="card" style="max-width:400px;margin:auto;">
    <h2>System Login</h2>
    <input type="text" id="user" placeholder="Username" />
    <input type="password" id="pass" placeholder="Password" />
    <button type="button" onclick="login()">Login</button>
    <p class="error" id="err"></p>
  </div>
</div>

<div id="main" class="container hidden">
  <div class="card">
    <h1>Device Settings</h1>
    <div class="card2">
      <h2>Device Time</h2>
      <input type="datetime-local" id="deviceTime" step="1" />
      <input type="button" id="sendTestSms" value="Send Test SMS" onclick="sendTestSms()" />
      <button onclick="syncTime()">Sync Time</button>
    </div>
    <button class="btn-s" onclick="changePass()">Change Password</button>
    <button class="btn-s" onclick="toggleHelp()">Help</button>
    <button onclick="logout()">Logout</button> 
  </div>
  <div id="grid" class="grid"></div>

)rawliteral"
#if HAS_GAS
R"rawliteral(
  <div class="card">
    <h2>Gas Status (MQ)</h2>
    <div><strong>Current:</strong> <span id="gasVal">-</span> <small>(Range: <span id="gasRange">-</span>)</small></div>
  </div>
)rawliteral"
#endif
#if HAS_DHT
R"rawliteral(
  <div class="card">
    <h2>Temperature & Humidity (DHT)</h2>
    <div>
      <strong>Current:</strong>
      <span id="dhtTemp">-</span> °C,
      <small>Humidity: <span id="dhtHum">-</span> %</small>
      <small style="margin-left:8px;">(Temp range: <span id="dhtRange">-</span>)</small>
    </div>
  </div>
)rawliteral"
#endif
R"rawliteral(


  <div id="help-card" class="card hidden">
    <h2>Commands Help</h2>
    <div class="table-wrap">
      <table>
        <thead>
          <tr>
            <th>Command</th>
            <th>Description</th>
          </tr>
        </thead>
        <tbody id="help-tbody">
          <tr><td colspan="2" style="text-align:center;color:#999;">Loading...</td></tr>
        </tbody>
      </table>
    </div>
  </div>
  <div class="card">
    <h3>SMS Log (last 200)</h3>
    <div class="table-wrap">
      <table>
        <thead>
          <tr>
            <th>ID</th>
            <th>Number</th>
            <th>Reason</th>
            <th>Sent At</th>
            <th>Text</th>
            <th>Status</th>
          </tr>
        </thead>
        <tbody id="sms-tbody">
          <tr><td colspan="6" style="text-align:center;color:#999;">Loading...</td></tr>
        </tbody>
      </table>
    </div>
  </div>

  <div id="inbox" class="card">
    <h3>Inbox (last 200)</h3>
    <div class="table-wrap">
      <table>
        <thead>
          <tr>
            <th>ID</th>
            <th>From</th>
            <th>Received At</th>
            <th>Text</th>
          </tr>
        </thead>
        <tbody id="inbox-tbody">
          <tr><td colspan="4" style="text-align:center;color:#999;">Loading...</td></tr>
        </tbody>
      </table>
    </div>
  </div>

  <div class="card"> 
    <button class="btn-d" onclick="factoryReset()">Factory Reset</button>
    <button class="btn-d" onclick="ResetEsp()">Restart Device</button>
  </div>
</div>

<script>
let TOKEN = null;

function $(id){ return document.getElementById(id); }

async function api(path, data=null){
  try{
    if (!TOKEN) TOKEN = sessionStorage.getItem("TOKEN");
    const res = await fetch('/api'+path, {
      method: data ? 'POST' : 'GET',
      headers: { 'Content-Type': 'application/json', ...(TOKEN?{'X-Auth':TOKEN}:{}) },
      body: data ? JSON.stringify(data) : null
    });
    return await res.json();
  }catch(e){
    const errEl = document.getElementById('err');
    if (errEl) errEl.textContent = 'Connection error';
    return { success:false, error:'Network Error' };
  }
}

async function login(){
  const userEl = $('user'), passEl = $('pass');
  if (!userEl || !passEl){ alert('Login error: input elements not found'); return; }
  const user = userEl.value.trim();
  const pass = passEl.value;
  if (!user || !pass){ $('err').textContent = '                                       '; return; }

  const res = await api('/login', { user, pass });
  if (res.success){
    TOKEN = res.token;
    sessionStorage.setItem('TOKEN', TOKEN);
    $('login').classList.add('hidden');
    $('main').classList.remove('hidden');

    // Auto-sync device clock with client immediately and periodically
    await autoSyncClock();
    setInterval(autoSyncClock, 5 * 60 * 1000); //                   

    await loadKeys();
    await loadSmsLog();
    await loadInboxLog();
  )rawliteral"
#if HAS_GAS
R"rawliteral(
    await loadGas();
)rawliteral"
#endif
R"rawliteral(
    setInterval(loadSmsLog, 3000);
    setInterval(loadInboxLog, 5000);
  )rawliteral"
#if HAS_GAS
R"rawliteral(
    setInterval(loadGas, 2000);
)rawliteral"
#endif
#if HAS_DHT
R"rawliteral(
    await loadDht();
    setInterval(loadDht, 2000);
    /*
)rawliteral"
#endif
#if HAS_DHT
R"rawliteral(
  <div class="card">
    <h2>Temperature (DHT)</h2>
    <div><strong>Current:</strong> <span id="dhtTemp">-</span> °C <small>(Range: <span id="dhtRange">-</span>)</small></div>
  </div>
)rawliteral"
#endif
R"rawliteral(
    */
  }else{
    $('err').textContent = res.error || 'Invalid username or password';
  }
}

async function autoSyncClock(){
  try{
    const now = Date.now();
    await api('/setTime', { timestamp: now });
  }catch(e){
    // ignore
  }
}

async function loadKeys(){
  const data = await api('/keys');
  if (data.keys){
    window.keys = [];
    data.keys.forEach(k => window.keys.push(k));
    render();
  }
}

function inputFor(k){
  const val = k.value || '';
  if (k.type==='dropdown' || k.type==='bool'){
    return `<select id="i-${k.key}">${k.options.split(',').map(o=>`<option value="${o}" ${val===o?'selected':''}>${o}</option>`).join('')}</select>`;
  }else if (k.type==='mobile'){
    return `<input class="mobile-input" id="i-${k.key}" value="${val}" maxlength="11" placeholder="09121234567"/>`;
  }else if (k.type==='int'){
    return `<input type="number" id="i-${k.key}" value="${val}" min="${k.min}" max="${k.max}"/>`;
  }else{
    return `<input type="text" id="i-${k.key}" value="${val}" maxlength="${k.max||''}" placeholder="min ${k.min} chars"/>`;
  }
}

function render(){
  const g = $('grid');
  if (!g){ console.warn('         #grid                !'); return; }
  g.innerHTML = '';
  window.keys.forEach(k => {
    const div = document.createElement('div');
    div.className = 'key-card' + (k.isSystem?' system':'');
    div.innerHTML = `
      <strong>${k.key}</strong>${k.isSystem?' (system)':''}
      <div>${inputFor(k)}</div>
      <button onclick="save('${k.key}')">Save</button>
      <p class="error" id="e-${k.key}"></p>
    `;
    g.appendChild(div);
  });
}

async function save(key){
  const val = document.getElementById('i-'+key).value;
  const res = await api('/save', {key, value: val});
  if (res.success) { $('e-'+key).textContent = ''; }
  else { $('e-'+key).textContent = res.error; }
}

async function factoryReset(){
  if (confirm('                       ')){
    await api('/reset', {});
    loadKeys();
    loadSmsLog();
  }
}

async function changePass(){
  const oldp = prompt('Current password:');
  if (!oldp) return;
  const newp = prompt('New password (min 4 chars):');
  if (!newp || newp.length < 4) return alert('Password too short');
  const res = await api('/changepass', {old: oldp, new: newp});
  alert(res.success ? 'Password changed' : res.error);
}

function logout(){
  $('main').classList.add('hidden');
  $('login').classList.remove('hidden');
  $('user').value = $('pass').value = '';
  $('err').textContent = '';
  TOKEN = null;
  sessionStorage.removeItem('TOKEN');
}

function syncTime(){
  const input = document.getElementById('deviceTime');
  const selected = new Date(input.value);
  if (isNaN(selected.getTime())) return alert('Invalid time');
  api('/setTime', { timestamp: selected.getTime() }).then(res => {
    alert(res.success ? 'Device time updated' : res.error);
  });
}

function fmtTs(ms){
  try{
    const d = new Date(ms);
    return d.toLocaleString('en-US');
  }catch(e){ return ms; }
}

async function loadSmsLog(){
  const data = await api('/smsLog');
  const tb = $('sms-tbody');
  if (!tb) return;
  tb.innerHTML = '';
  if (!data.success || !data.items || !data.items.length){
    tb.innerHTML = `<tr><td colspan="6" style="text-align:center;color:#999;">                        </td></tr>`;
    return;
  }
  data.items.forEach(row=>{
    const tr = document.createElement('tr');
    const st = (row.status === 'delivered' || row.status === 'sent') ? 's'
               : ((row.status === 'queued' || row.status === 'sending') ? 'q' : 'f');
    tr.innerHTML = `
      <td class="mono">${row.id}</td>
      <td class="mono">${row.number}</td>
      <td>${row.reason||''}</td>
      <td class="mono">${fmtTs(row.timestamp)}</td>
      <td style="white-space:nowrap; max-width:420px; overflow:hidden; text-overflow:ellipsis;" title="${row.text}">${row.text}</td>
      <td><span class="badge ${st}">${row.status}</span></td>
    `;
    tb.appendChild(tr);
  });
}

async function loadInboxLog(){
  const data = await api('/inboxLog');
  const tb = $('inbox-tbody');
  if (!tb) return;
  tb.innerHTML = '';
  if (!data.success || !data.items || !data.items.length){
    tb.innerHTML = `<tr><td colspan="4" style="text-align:center;color:#999;">                        </td></tr>`;
    return;
  }
  data.items.forEach(row=>{
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td class="mono">${row.id}</td>
      <td class="mono">${row.from}</td>
      <td class="mono">${fmtTs(row.timestamp)}</td>
      <td style="white-space:nowrap; max-width:420px; overflow:hidden; text-overflow:ellipsis;" title="${row.text}">${row.text}</td>
    `;
    tb.appendChild(tr);
  });
}

function sendTestSms(){
  api('/sendTestSms', {}).then(data => {
    alert(data.success ? 'Test SMS sent' : 'SMS send error');
  });
}

async function ResetEsp(){
  if (!confirm('                                                                                                                 ')) return;
  const res = await api('/resetEsp', {});
  if (res.success){ alert('Device is restarting'); location.reload(); }
  else { alert('Reset error: '+res.error); }
}

function updateTimeBoxFromSystemClock(){
  const input = document.getElementById('deviceTime');
  if (!input) return;
  const now = new Date();
  const local = new Date(now.getTime() - now.getTimezoneOffset()*60000).toISOString().slice(0,19);
  input.value = local;
}
setInterval(updateTimeBoxFromSystemClock, 1000);

// Help data and UI
const HELP = [
  {cmd:'gps', desc:'Send GPS link (if ready)'},
  {cmd:'arm', desc:'Arm system'},
  {cmd:'darm/disarm', desc:'Disarm system'},
  {cmd:'piron/pirof', desc:'Enable/Disable PIR'},
  {cmd:'vibon/vibof', desc:'Enable/Disable Vibration'},
  {cmd:'ledon/ledof', desc:'LED on/off'},
  {cmd:'buzon/buzof', desc:'Buzzer on/off'},
  {cmd:'alron/alrof', desc:'SMS alerts on/off'},
  {cmd:'smson/smsof', desc:'Global SMS TX on/off'},
  {cmd:'wifon/wifof', desc:'WiFi AP on/off'},
  {cmd:'check', desc:'System status report'},
  {cmd:'help', desc:'Show commands'},
  {cmd:'YYYYMMDD:HHmm', desc:'Set device time (Jalali syntax, e.g. 14040816:1221)'}
];

function renderHelp(){
  const tb = document.getElementById('help-tbody');
  if (!tb) return;
  tb.innerHTML = '';
  HELP.forEach(row => {
    const tr = document.createElement('tr');
    tr.innerHTML = '<td class="mono">' + row.cmd + '</td><td>' + row.desc + '</td>';
    tb.appendChild(tr);
  });
}

function toggleHelp(){
  const card = document.getElementById('help-card');
  if (!card) return;
  card.classList.toggle('hidden');
  if (!card.classList.contains('hidden')) renderHelp();
}

/*
async function loadGas(){
  try{
    const d = await api('/gas');
    const v = document.getElementById('gasVal');
    const r = document.getElementById('gasRange');
    if (!v || !r) return;
    if (!d || !d.success){ v.textContent = '   '; r.textContent = '   '; return; }
async function loadDht(){
  try{
    const d = await api('/dht');
    const t = document.getElementById('dhtTemp');
    const r = document.getElementById('dhtRange');
    if (!t || !r) return;
    if (!d || !d.success){ t.textContent = '—'; r.textContent = '—'; return; }
    const tempStr = (typeof d.t === 'number') ? d.t.toFixed(1) : d.t;
    t.textContent = tempStr;
    r.textContent = d.min + '..' + d.max;
    const updLabel = (key) => {
      const inp = document.getElementById('i-'+key);
      if (!inp) return;
      const container = inp.parentElement;
      if (!container) return;
      let small = container.querySelector('small.temp-live');
      if (!small) {
        small = document.createElement('small');
        small.className = 'temp-live';
        small.style.marginRight = '8px';
        container.appendChild(small);
      }
      small.textContent = 'Now: ' + tempStr + ' °C';
    };
    updLabel('TempMin');
    updLabel('TempMax');
  }catch(e){}
}
    v.textContent = d.value;
    r.textContent = d.min + '..' + d.max;

    // Update live labels next to GasMin/GasMax inputs
    const updLabel = (key) => {
      const inp = document.getElementById('i-'+key);
      if (!inp) return;
      const container = inp.parentElement;
      if (!container) return;
      let small = container.querySelector('small.gas-live');
      if (!small) {
        small = document.createElement('small');
        small.className = 'gas-live';
        small.style.marginRight = '8px';
        container.appendChild(small);
      }
      small.textContent = 'Now: ' + d.value;
    };
    updLabel('GasMin');
    updLabel('GasMax');
  }catch(e){}
}
*/

 //                                                    (                                                HTML)
  function fixTitles(){
    try{
      var outTb = document.getElementById('sms-tbody');
      if (outTb){
        var h2 = outTb.closest('.card').querySelector('h2');
        if (h2) h2.textContent = 'SMS Log (last 200)';
      }
      var inTb = document.getElementById('inbox-tbody');
      if (inTb){
        var h2i = inTb.closest('.card').querySelector('h2');
        if (h2i) h2i.textContent = 'Inbox (last 200)';
      }
    }catch(e){}
  }
  fixTitles();
  
  // Re-declared clean functions (override commented broken block above)
  async function loadGas(){
    try{
      const d = await api('/gas');
      const v = document.getElementById('gasVal');
      const r = document.getElementById('gasRange');
      if (!v || !r) return;
      if (!d || !d.success){ v.textContent='-'; r.textContent='-'; return; }
      v.textContent = d.value;
      r.textContent = d.min + '..' + d.max;
      const updLabel = (key) => {
        const inp = document.getElementById('i-'+key);
        if (!inp) return;
        const container = inp.parentElement;
        if (!container) return;
        let small = container.querySelector('small.gas-live');
        if (!small) { small = document.createElement('small'); small.className='gas-live'; small.style.marginRight='8px'; container.appendChild(small); }
        small.textContent = 'Now: ' + d.value;
      };
      updLabel('GasMin'); updLabel('GasMax');
    }catch(e){}
  }

  async function loadDht(){
    try{
      const d = await api('/dht');
      const tEl = document.getElementById('dhtTemp');
      const hEl = document.getElementById('dhtHum');
      const trEl = document.getElementById('dhtTempRange');
      const hrEl = document.getElementById('dhtHumRange');
      const legacyRange = document.getElementById('dhtRange');
      if (!tEl || !hEl) return;
      if (!d || !d.success){
        tEl.textContent='-'; hEl.textContent='-';
        if (trEl) trEl.textContent='-'; if (hrEl) hrEl.textContent='-'; if (legacyRange) legacyRange.textContent='-';
        return;
      }
      const tempStr = (typeof d.t === 'number') ? d.t.toFixed(1) : d.t;
      const humStr  = (typeof d.h === 'number') ? d.h.toFixed(0) : d.h;
      tEl.textContent = tempStr;
      hEl.textContent = humStr;
      if (trEl) trEl.textContent = d.min + '..' + d.max;
      if (hrEl) hrEl.textContent = d.hmin + '..' + d.hmax;
      if (legacyRange) legacyRange.textContent = 'T: ' + (d.min + '..' + d.max) + ', H: ' + (d.hmin + '..' + d.hmax);

      const updTempLabel = (key) => {
        const inp = document.getElementById('i-'+key);
        if (!inp) return;
        const container = inp.parentElement;
        if (!container) return;
        let small = container.querySelector('small.temp-live');
        if (!small) { small = document.createElement('small'); small.className='temp-live'; small.style.marginRight='8px'; container.appendChild(small); }
        small.textContent = 'Now: ' + tempStr + ' C';
      };
      const updHumLabel = (key) => {
        const inp = document.getElementById('i-'+key);
        if (!inp) return;
        const container = inp.parentElement;
        if (!container) return;
        let small = container.querySelector('small.hum-live');
        if (!small) { small = document.createElement('small'); small.className='hum-live'; small.style.marginRight='8px'; container.appendChild(small); }
        small.textContent = 'Now: ' + humStr + ' %';
      };
      updTempLabel('TempMin'); updTempLabel('TempMax');
      updHumLabel('HumMin');  updHumLabel('HumMax');
    }catch(e){}
  }
</script>

</body>
</html>
)rawliteral";

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

    prefs.putULong("epochStartTime", (unsigned long)timestamp);
    epochStartTime = (unsigned long)timestamp;

    g_lastSyncUnixMs = timestamp;
    g_lastSyncMillis = millis();
    g_deviceNowMs    = g_lastSyncUnixMs;

    printTimestampReadable(timestamp);
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

  server.addHandler(new AsyncCallbackJsonWebHandler("/api/login", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
    if (!json.is<JsonObject>()) { request->send(400,"application/json","{\"error\":\"         JSON                      \"}"); return; }
    JsonObject obj = json.as<JsonObject>();
    String user = obj["user"];
    String pass = obj["pass"];
    if (user == username && pass == userPassword) {
      sessionToken = String((uint32_t)esp_random(), HEX);
      DynamicJsonDocument resp(128);
      resp["success"] = true;
      resp["token"]   = sessionToken;
      String out; serializeJson(resp, out);
      request->send(200, "application/json", out);
    } else {
      request->send(401, "application/json", "{\"error\":\"                                            \"}");
    } }));

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
  WiFi.mode(WIFI_OFF);
  Serial.println("[WIFI] SoftAP stopped.");
}

void StartSoftAP()
{
  if (!public_WifiEnabled)
  {
    stopAP();
    return;
  }
  WiFi.mode(WIFI_AP);
  bool ap_started = WiFi.softAP(ssidName, ssidPassword);
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
    Serial.printf("[SMS ] blocked by policy (SmsTxEnabled=false) id=%lu\n", (unsigned long)idb);
    return false;
  }

  if (priority < 0 || priority > 2)
    return false;
  int head = smsQueueHead[priority];
  int tail = smsQueueTail[priority];
  int nextTail = (tail + 1) % SMS_QUEUE_SIZE;
  if (nextTail == head)
  {
    Serial.println("SMS queue full for priority " + String(priority));
    uint32_t id = smsLogAppend(number, detectReasonFromText(text), deviceNowMs64(), text, "failed", priority);
    Serial.printf("[LOG ] archived (failed due full queue) id=%lu\n", (unsigned long)id);
    return false;
  }

  uint32_t archId = smsLogAppend(number, detectReasonFromText(text), deviceNowMs64(), text, "queued", priority);

  smsQueues[priority][tail] = {number, text, priority, millis(), archId};
  smsQueueTail[priority] = nextTail;

  Serial.printf("[ENQ ] number=%s pr=%d archId=%lu text=%s\n",
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

    // Set device clock
    prefs.putULong("epochStartTime", (unsigned long)ms);
    epochStartTime = (unsigned long)ms;
    g_lastSyncUnixMs = ms;
    g_lastSyncMillis = millis();
    g_deviceNowMs    = g_lastSyncUnixMs;
    printTimestampReadable(ms);

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
uint32_t g_eventTimes[2][MAX_EVENTS_PER_SENSOR]; // [sensorIndex][slot]
uint8_t g_eventCount[2] = {0, 0};
const uint32_t DEBOUNCE_MS = 80; //                  

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
  bool c1 = (vCount >= 4) || (pCount >= 4);
  bool bothHave = (vCount >= 1 && pCount >= 1);
  bool bothLe4 = (vCount <= 4 && pCount <= 4);
  bool c2 = ((vCount + pCount) >= 5) && bothHave && bothLe4;
  return c1 || c2;
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
#if HAS_GAS
  static uint32_t _lastGasLog = 0;
  int gasRaw = analogRead(GasAnalogPin);
  public_GasValue = (public_GasValue * 7 + gasRaw) / 8;
  if (public_GasEnabled) {
    gasAlarmNow = (public_GasValue < public_GasMin) || (public_GasValue > public_GasMax);
  }
  if (millis() - _lastGasLog > 1000) {
    _lastGasLog = millis();
    Serial.printf("[GAS/TICK] pin=%d raw=%d filtered=%d range=[%d,%d] enabled=%d\n",
                  GasAnalogPin, gasRaw, public_GasValue, public_GasMin, public_GasMax, (int)public_GasEnabled);
  }
#endif

#if HAS_DHT
  // Periodic DHT read + log (independent of Arm state)
  static uint32_t _lastDhtRead = 0;
  static uint32_t _lastDhtLog  = 0;
  if (public_DhtEnabled && (millis() - _lastDhtRead > 2000)) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) public_TempValue = t;
    if (!isnan(h)) public_HumValue  = h;
    _lastDhtRead = millis();
  }
  if (millis() - _lastDhtLog > 2000) {
    _lastDhtLog = millis();
    Serial.printf("[DHT/TICK] pin=%d t=%.1fC h=%.0f%% range=[%d,%d] enabled=%d\n",
                  DHT_PIN, public_TempValue, public_HumValue, public_TempMin, public_TempMax, (int)public_DhtEnabled);
  }
  if (public_DhtEnabled) {
    tempAlarmNow = (public_TempValue < public_TempMin) || (public_TempValue > public_TempMax);
    humAlarmNow  = (public_HumValue  < public_HumMin)  || (public_HumValue  > public_HumMax);
  }
#endif

  if (!public_SystemStatus)
    return;

  uint32_t now = millis();

  static uint8_t prevV = 0, prevP = 0;
  static bool prevLedActive = false, prevBuzzActive = false;

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

  int32_t cooldownLeft = (alarmCooldownUntilMs > now) ? (int32_t)(alarmCooldownUntilMs - now) : 0;

  Serial.printf("[SENS] t=%lu ms | vibPin=%d pirPin=%d | V=%u P=%u | LED=%d BUZZ=%d | cooldownLeft=%ld ms\n",
                (unsigned long)now, vibState, pirState, V, P,
                (int)ledSM.active, (int)buzzSM.active, (long)cooldownLeft);

  if (V != prevV || P != prevP)
  {
    Serial.printf("[WIN ] counts changed: V: %u -> %u , P: %u -> %u (window=%ums)\n",
                  prevV, V, prevP, P, (unsigned)WINDOW_MS);
    prevV = V;
    prevP = P;
  }

  bool alarmNow = shouldAlarm(V, P) || gasAlarmNow || tempAlarmNow || humAlarmNow;
  if (alarmNow)
  {
    bool c1 = (V >= 4) || (P >= 4);
    bool bothHave = (V >= 1 && P >= 1);
    bool bothLe4 = (V <= 4 && P <= 4);
    bool c2 = ((V + P) >= 5) && bothHave && bothLe4;

    Serial.printf("[ALRM] condition met: c1=%d, c2=%d gas=%d temp=%d hum=%d\n", (int)c1, (int)c2, (int)gasAlarmNow, (int)tempAlarmNow, (int)humAlarmNow);
  }

  if (prevBuzzActive != buzzSM.active)
  {
    Serial.printf("[BUZZ] state change: %s -> %s\n",
                  prevBuzzActive ? "ACTIVE" : "INACTIVE",
                  buzzSM.active ? "ACTIVE" : "INACTIVE");
    prevBuzzActive = buzzSM.active;
  }
  if (prevLedActive != ledSM.active)
  {
    Serial.printf("[LED ] state change: %s -> %s\n",
                  prevLedActive ? "ACTIVE" : "INACTIVE",
                  ledSM.active ? "ACTIVE" : "INACTIVE");
    prevLedActive = ledSM.active;
  }

  // ---                       ---

  //                                                                                                                                                               
  if (alarmNow && !buzzSM.active && (int32_t)(now - alarmCooldownUntilMs) >= 0)
  {
    if (public_AlertEnabled_Buzzer)
    {
      Serial.printf("[ALRM] START buzzer (on/off=500/500ms, duration=%ums). Reset LED & window. Set cooldown.\n",
                    (unsigned)BUZZER_ALARM_MS);

      smStart(buzzSM, 500, 500, BUZZER_ALARM_MS);
      smStop(ledSM);

      alarmCooldownUntilMs = now + BUZZER_ALARM_MS + 1000;

      g_eventCount[0] = 0;
      g_eventCount[1] = 0;
      Serial.println("[ALRM] window counts reset (V=0, P=0).");
    }
    else
    {
      Serial.println("[ALRM] Buzzer disabled -> no buzzer start.");
    }

    //            SMS                                           
    if (public_AlertEnabled_Sms)
    {
      for (auto &num : public_List_AllAlternetMobiles)
      {
        if (shouldSendThrottled(num, gasAlarmNow?"GAS":(tempAlarmNow?"TEMP":(humAlarmNow?"HUM":"ALARM")), 30000))
        {
          String cause;
          #if HAS_GAS
          if (gasAlarmNow)
            cause = String("GAS ") + String(public_GasValue) + " in[" + String(public_GasMin) + "," + String(public_GasMax) + "]";
          else
          #endif
          #if HAS_DHT
          if (tempAlarmNow)
            cause = String("TEMP ") + String(public_TempValue, 1) + "C in[" + String(public_TempMin) + "," + String(public_TempMax) + "]";
          else if (humAlarmNow)
            cause = String("HUM ") + String((int)public_HumValue) + "% in[" + String(public_HumMin) + "," + String(public_HumMax) + "]";
          else
          #endif
            cause = (V >= 4 && P < 4)   ? "PIR"
                   : (P >= 4 && V < 4) ? "VIB"
                                       : "BOTH";
          // Include device time in message
          String msg = (gasAlarmNow || tempAlarmNow || humAlarmNow)
                        ? (String("ALARM:") + cause + " @ " + nowTimeReadable())
                        : (String("ALARM:") + cause + " V=" + String(V) + " P=" + String(P) + " @ " + nowTimeReadable());

          Serial.printf("[SMS ] enqueue to %s: %s\n", num.c_str(), msg.c_str());
          enqueueSms(num, msg, 0);
        }
        else
        {
          Serial.printf("[SMS ] throttled (skip) for %s (ALARM)\n", num.c_str());
        }
      }
    }
  }
  else if (!alarmNow)
  {
    //                                    LED                                                        LED                    
    if ((vibState == HIGH || pirState == HIGH || V > 0 || P > 0) && !buzzSM.active && public_LedEnabled)
    {
      if (!ledSM.active)
      {
        Serial.println("[LED ] START soft-blink (on/off=500/500, no fixed duration; auto-stop on silence).");
        smStart(ledSM, 500, 500, 0);
      }
      ledExtendUntilMs = now + LED_EXTEND_MS;
      Serial.printf("[LED ] extend-until set to t=%lu (in %u ms)\n",
                    (unsigned long)ledExtendUntilMs, (unsigned)LED_EXTEND_MS);
    }
  }
  else
  {
    if (buzzSM.active)
    {
      Serial.println("[ALRM] condition true but buzzer already ACTIVE     no restart.");
    }
    else if ((int32_t)(now - alarmCooldownUntilMs) < 0)
    {
      Serial.printf("[ALRM] condition true but still in cooldown (%ld ms left)     no start.\n",
                    (long)((int32_t)(alarmCooldownUntilMs - now)));
    }
  }

  //        LED                                        
  if (!public_LedEnabled && ledSM.active)
  {
    Serial.println("[LED ] policy disabled -> STOP.");
    smStop(ledSM);
  }

  //                     LED                        s (                                 )
  if (ledSM.active && (int32_t)(now - ledExtendUntilMs) > 0 && buzzSM.active == false)
  {
    Serial.println("[LED ] STOP due to silence (no events in the last 2s).");
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

  // WiFi creds
  ssidName = prefs.getString("wifi_Ssid_Name", ssidNameDefault);
  ssidPassword = prefs.getString("Ssid_Password", ssidPasswordDefault);


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
}

// removed stray JS block that broke C++ compilation
void setup()
{
  Serial.begin(9600);
  Serial.println("Starting ESP32 AP...");

  prefs.begin("config", false);
  prefsClock.begin("clock", false);

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
  StartSoftAP();
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












