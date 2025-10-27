#include <AsyncJson.h>
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>

bool Public_MotionDetected = false;
unsigned long epochStartTime = 0;
char lastOktime[32];

void SetPublicVariablesFromPrefs();
bool enqueueSms(String number, String text, int priority);

void printTimestampReadable(uint64_t timestampMs)
{
  time_t timestampSec = (timestampMs / 1000) + 12600; // تبدیل به ثانیه
  struct tm *timeinfo = localtime(&timestampSec);     // یا gmtime برای UTC

  strftime(lastOktime, sizeof(lastOktime), "%Y-%m-%d %H:%M:%S", timeinfo);

  Serial.print(" ok time is :       ");
  Serial.println(lastOktime);
}

#pragma region Sim808_Variables

struct SmsMessage
{
  String number;
  String text;
  int priority; // 0 = بالا، 1 = متوسط، 2 = پایین
  unsigned long timestamp;
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
int allowedCount = 0;
int smsQueueHead[3] = {0, 0, 0};
int smsQueueTail[3] = {0, 0, 0};
SmsMessage smsQueues[3][SMS_QUEUE_SIZE]; // سه صف برای سه اولویت
IncomingSms incomingSmsQueue[INCOMING_SMS_QUEUE_SIZE];
SmsReceiveState smsRxState = SMS_RX_IDLE;
String smsRxBuffer = "";
SmsState smsState = SMS_IDLE;
unsigned long lastCommandTime = 0;

#pragma endregion

#pragma region webServer
// سرور
AsyncWebServer server(80);
Preferences prefs;

// --- امنیت ---
String ssidNameDefault = "ElixHome";
String ssidPasswordDefault = "12345678";
String ssidName;
String ssidPassword;
String username = "admin";
String userPassword = "1234";
#pragma endregion

#pragma region webpageVaiables

// --- ساختار کلید ---
struct ConfigKey
{
  String key;
  String type; // string, int, bool, dropdown, mobile
  String defaultVal;
  String min;
  String max;
  String options; // برای dropdown: "on,off"
  bool isSystem;
};

// کلیدهای پیش‌فرض
ConfigKey defaultKeys[] = {
    {"wifi_Ssid_Name", "string", ssidNameDefault, "2", "10", "", false},
    {"wifi_Ssid_Password", "string", ssidPasswordDefault, "3", "20", "", false},
    {"systemStatus", "dropdown", "on", "", "", "on,off", false},
    {"deviceName", "string", "دزدگیر", "3", "20", "", false},
    {"SmsAlertEnabled", "bool", "true", "", "", "true,false", false},
    {"BuzzerAlertEnabled", "bool", "true", "", "", "true,false", false},
    {"OwnerMobile", "mobile", "", "10", "12", "", false},
    {"alternetMobile", "mobile", "", "10", "10", "", false},
    {"AllAlternetMobiles", "string", "", "10", "100", "", false},
    // {"theme", "dropdown", "dark", "", "", "light,dark,oled", false}
    // {"wifiTimeout", "int", "30", "10", "300", "", false},
};

const int numKeys = sizeof(defaultKeys) / sizeof(defaultKeys[0]);
#pragma endregion

#pragma region Html
// --- HTML ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fa" dir="rtl">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0"/>
  <title>تنظیمات دستگاه </title>
  <style>
    :root { --p: #3498db; --d: #e74c3c; --s: #2ecc71; --bg: #1a1a1a; --card: #2d2d2d; --text: #eee; }
    body { font-family: Tahoma; background: var(--bg); color: var(--text); margin:0; }
    .container { max-width: 900px; margin: 20px auto; padding: 20px; }
    .card { background: var(--card); border-radius: 12px; padding: 20px; margin-bottom: 20px; box-shadow: 0 4px 12px rgba(0,0,0,0.3); }
    .card2 {  background: #047445ff; border-radius: 12px; padding: 20px; margin-bottom: 20px; box-shadow: 0 4px 12px hsla(0, 100%, 50%, 1.00); }
    h1, h2 { text-align: center; color: var(--p); }
    input, select, button { padding: 10px; margin: 5px 0; border-radius: 8px; width: 100%; border: 1px solid #555; background: rgba(255,255,255,0.1); color: var(--text); }
    button { background: var(--p); color: white; border: none; cursor: pointer; font-weight: bold; }
    .btn-d { background: var(--d); }
    .btn-s { background: var(--s); }
    .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 15px; }
    .key-card { border: 1px solid #444; border-radius: 8px; padding: 15px; }
    .system { border-left: 5px solid var(--p); }
    .hidden { display: none; }
    .error { color: var(--d); font-size: 0.9em; }
    .mobile-input { direction: ltr; text-align: left; font-family: monospace; }
  </style>
</head>
<body>

  <!-- صفحه ورود -->
  <div id="login" class="container">
    <div class="card" style="max-width:400px;margin:auto;">
      <h2>ورود به سیستم</h2>
      <input type="text" id="user" placeholder="نام کاربری" />
      <input type="password" id="pass" placeholder="رمز عبور" />
      <button type="button" onclick="login()">ورود</button>
      <p class="error" id="err"></p>
    </div>
  </div>

  <!-- صفحه اصلی -->
  <div id="main" class="container hidden">
    <div class="card">
      <h1>مدیریت تنظیمات</h1>
      <div class="card2">
  <h2>تنظیم ساعت دستگاه</h2>
  <input type="datetime-local" id="deviceTime"  step="1" />
 <input type="button" id="sendTestSms" value="ارسال تست SMS" onclick="sendTestSms()" />
  <button onclick="syncTime()">همگام‌سازی با دستگاه</button>
</div>

      <button class="btn-s" onclick="changePass()">تغییر رمز</button>
      <button onclick="logout()">خروج</button> 
    </div>
    <div id="grid" class="grid"></div>

    <div class="card"> 
      
      <button class="btn-d" onclick="factoryReset()">ریست فکتوری</button>
      <button class="btn-d" onclick="ResetEsp()">راه اندازی مجدد </button>

    </div>
  </div>

  <!-- اسکریپت بعد از HTML -->
  <script> 
function syncTime() {
  const input = document.getElementById("deviceTime");
  const selected = new Date(input.value);
  if (isNaN(selected.getTime())) {
    alert("زمان نامعتبر است");
    return;
  }

  api('/setTime', {
    timestamp: selected.getTime()
  }).then(res => {
    alert(res.success ? "⏱ ساعت دستگاه تنظیم شد" : res.error);
  });
}
  function updateTimeBoxFromSystemClock() {
  const input = document.getElementById("deviceTime");
  if (!input) return;

  const now = new Date();
  const local = new Date(now.getTime() - now.getTimezoneOffset() * 60000)
    .toISOString()
    .slice(0, 19); // YYYY-MM-DDTHH:MM:SS

  input.value = local;

  console.log('local=>>>>>>>>>>>>>>>    '+ local);
}

// هر ثانیه ساعت سیستم رو بخون و داخل input بذار
setInterval(updateTimeBoxFromSystemClock, 1000);
    function $(id) { return document.getElementById(id); }

    const keys = [];
async function ResetEsp() {
  if (!confirm("آیا مطمئن هستید که می‌خواهید دستگاه را مجددا راه اندازی کنید؟")) return;

  const res = await api('/resetEsp', {});
  if (res.success) {
    alert(" با موفقیت انجام شد. دستگاه در حال راه‌اندازی مجدد است.");
    location.reload(); // یا می‌تونی دستور خاصی برای ریبوت بزنی
  } else {
    alert("خطا در ریست : " + res.error);
  }
}

    async function api(path, data = null) {
    if (document.getElementById('err'))
  document.getElementById('err').textContent = 'اتصال به سرور قطع است!';


      try {
        const res = await fetch('/api' + path, {
          method: data ? 'POST' : 'GET',
          headers: {'Content-Type': 'application/json'},
          body: data ? JSON.stringify(data) : null
        });
        return await res.json();
      } catch (e) {
        $('#err').textContent = 'اتصال به سرور قطع است!';
        console.error(e);
        return { success: false, error: 'Network Error' };
      }
    }

      async function login() {
      alert("در حال ورود به سیستم"); // ✅ این خط رو اضافه کن

      const userEl = document.getElementById("user");
      const passEl = document.getElementById("pass");

      if (!userEl || !passEl) {
        alert("ورود ناموفق: عناصر ورودی پیدا نشدند!");
        return;
      }

      const user = userEl.value.trim();
      const pass = passEl.value;

      if (!user || !pass) {
        document.getElementById("err").textContent = "همه فیلدها الزامی است";
        return;
      }

      const res = await api("/login", { user, pass });
      if (res.success) {
      document.getElementById("login").classList.add("hidden");
      document.getElementById("main").classList.remove("hidden");
      console.log("user " + user + "    pass " + pass);
      const now = new Date();
      console.log(now.toLocaleString());
      await loadKeys();
     }
     else {
        document.getElementById("err").textContent = res.error || "نام کاربری یا رمز اشتباه";
      }
      }

    async function loadKeys() {
        const data = await api('/keys');
        if (data.keys) {
          keys.length = 0;
          data.keys.forEach(k => keys.push(k)); // حالا هر کلید شامل value هم هست
          render();
        }
    }

    function render() {
     const g = document.getElementById('grid');
  if (!g) {
    console.warn("عنصر #grid پیدا نشد!");
    return;
  }
  g.innerHTML = ''; // یا هر محتوایی

      keys.forEach(k => {
        const div = document.createElement('div');
        div.className = 'key-card' + (k.isSystem ? ' system' : '');
        div.innerHTML = `
          <strong>${k.key}</strong>${k.isSystem ? ' (سیستمی)' : ''}
          <div>${inputFor(k)}</div>
          <button onclick="save('${k.key}')">ذخیره</button>
          <p class="error" id="e-${k.key}"></p>
        `;
        g.appendChild(div);
      });
    }
    function inputFor(k) {
      const val = k.value || '';
      if (k.type === 'dropdown' || k.type === 'bool') {
        return `<select id="i-${k.key}">${k.options.split(',').map(o => `<option value="${o}" ${val===o?'selected':''}>${o}</option>`).join('')}</select>`;
      } else if (k.type === 'mobile') {
        return `<input class="mobile-input" id="i-${k.key}" value="${val}" maxlength="11" placeholder="09121234567"/>`;
      } else if (k.type === 'int') {
        return `<input type="number" id="i-${k.key}" value="${val}" min="${k.min}" max="${k.max}"/>`;
      } else {
        return `<input type="text" id="i-${k.key}" value="${val}" maxlength="${k.max||''}" placeholder="حداقل ${k.min} کاراکتر"/>`;
      }
    }

function getVal(key) {
  const k = keys.find(x => x.key === key);
  return k && k.value !== undefined ? k.value : '';
}


    async function save(key) {
    const val = document.getElementById('i-' + key).value;
      const res = await api('/save', {key, value: val});
      if (res.success) {
        $('#e-'+key).textContent = '';
      } else {
        $('#e-'+key).textContent = res.error;
      }
    }

    async function factoryReset() {
      if (confirm('ریست فکتوری؟')) {
        await api('/reset');
        loadKeys();
      }
    }

    async function changePass() {
      const oldp = prompt('رمز فعلی:');
      if (!oldp) return;
      const newp = prompt('رمز جدید (حداقل 4 کاراکتر):');
      if (!newp || newp.length < 4) return alert('رمز کوتاه است');
      const res = await api('/changepass', {old: oldp, new: newp});
      alert(res.success ? 'رمز تغییر کرد' : res.error);
    }

    function logout() {
      $('#main').classList.add('hidden');
      $('#login').classList.remove('hidden');
      $('#user').value = $('#pass').value = '';
      $('#err').textContent = '';
    }

    function sendTestSms() {
  fetch("/api/sendTestSms", { method: "POST" })
    .then(res => res.json())
    .then(data => {
      alert(data.success ? "📤 SMS تست ارسال شد" : "❌ خطا در ارسال SMS");
    });
}
  </script>

</body>
</html>
)rawliteral";
#pragma endregion

#pragma region publicVariables
bool public_SystemStatus;
bool public_AlertEnabled_Sms;
bool public_AlertEnabled_Buzzer;
String public_OwnerMobileNumber;
String public_AlternetMobile;
String public_AllAlternetMobiles;
bool public_SimIsOnline;
std::vector<String> public_List_AllAlternetMobiles;
const int AllarmLedPin = 2;
const int AllarmBuzzerPin = 2;
unsigned long public_DeviceTime;

#pragma endregion

#pragma region WebPageFuncs

bool authenticateWeb(AsyncWebServerRequest *request)
{
  return true; // برای سادگی. در پروژه واقعی از session استفاده کن
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
          return "عدد باید بین " + defaultKeys[i].min + " و " + defaultKeys[i].max + " باشد";
      }
      else if (t == "string")
      {
        int len = value.length();
        int mn = defaultKeys[i].min.toInt();
        int mx = defaultKeys[i].max.toInt();
        if (len < mn || (mx > 0 && len > mx))
          return "طول باید بین " + String(mn) + " و " + String(mx) + " باشد";
      }
      else if (t == "mobile")
      {
        if (value.length() != 11 || value[0] != '0')
          return "فرمت: 09121234567";
        for (int j = 1; j < 10; j++)
          if (!isDigit(value[j]))
            return "فقط عدد";
      }
      else if (t == "dropdown" || t == "bool")
      {
        if (defaultKeys[i].options.indexOf(value) == -1 && defaultKeys[i].options.indexOf("," + value) == -1)
        {
          return "مقدار مجاز نیست";
        }
      }
      return "1";
    }
  }
  return "کلید نامعتبر";
}

void HtmlFunctions()
{

  server.addHandler(new AsyncCallbackJsonWebHandler("/api/setTime", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
  if (!json.is<JsonObject>()) {
    request->send(400, "application/json", "{\"error\":\"فرمت JSON نامعتبر است\"}");
    return;
  }

  JsonObject obj = json.as<JsonObject>();
  unsigned long long timestamp = obj["timestamp"];

  Serial.print("browser time ");
  Serial.println((unsigned long long)timestamp);
  
  printTimestampReadable(timestamp);
  
  prefs.putULong("epochStartTime", timestamp);
  epochStartTime = timestamp;

  request->send(200, "application/json", "{\"success\":true}"); }));

  server.on("/api/time", HTTP_GET, [](AsyncWebServerRequest *request)
            {
  unsigned long now = millis();
  unsigned long currentTime = epochStartTime + now;
  request->send(200, "application/json", "{\"timestamp\":" + String(currentTime) + "}"); });

  server.on("/api/sendTestSms", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    Serial.println("send test message ");
  enqueueSms(public_OwnerMobileNumber , " test message  at   " + String(lastOktime),2); // ✅ اجرای تابع SMS
  request->send(200, "application/json", "{\"success\":true}"); });

  server.on("/api/keys", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    if (!authenticateWeb(req)) return req->send(401);
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.createNestedArray("keys");
    for (int i = 0; i < numKeys; i++) {
      JsonObject obj = arr.createNestedObject();
      obj["key"] = defaultKeys[i].key;
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

  server.addHandler(new AsyncCallbackJsonWebHandler("/api/login", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
  if (!json.is<JsonObject>()) {
    request->send(400, "application/json", "{\"error\":\"فرمت JSON نامعتبر است\"}");
    return;
  }


  JsonObject obj = json.as<JsonObject>();
  String user = obj["user"];
  String pass = obj["pass"];

  if (user == username && pass == userPassword) {
    request->send(200, "application/json", "{\"success\":true}");
  } else {
    request->send(401, "application/json", "{\"error\":\"نام کاربری یا رمز اشتباه\"}");
  } }));

  server.addHandler(new AsyncCallbackJsonWebHandler("/api/save", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
  if (!json.is<JsonObject>()) {
    request->send(400, "application/json", "{\"error\":\"فرمت JSON نامعتبر است\"}");
    return;
  }

  JsonObject obj = json.as<JsonObject>();
  String key = obj["key"];
  String value = obj["value"];

  String ValidationResult = validateKey(key, value);

  // اعتبارسنجی و ذخیره‌سازی
 if (ValidationResult != "1") {
  request->send(400, "application/json", "{\"error\":\"مقدار نامعتبر: " + ValidationResult + "\"}");
  return;
}

  prefs.putString(key.c_str(), value);

  Serial.println(" after save ValidationResult :    "+ ValidationResult +"     validateKey   =>>>  key :  " + key + "  value :  " + value);
 
  SetPublicVariablesFromPrefs();
 
  request->send(200, "application/json", "{\"success\":true}"); }));

  server.addHandler(new AsyncCallbackJsonWebHandler("/api/changepass", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
  JsonObject obj = json.as<JsonObject>();
  String oldpass = obj["old"];
  String newpass = obj["new"];

  if (oldpass != userPassword) {
    request->send(403, "application/json", "{\"error\":\"رمز فعلی اشتباه است\"}");
    return;
  }

  if (newpass.length() < 4) {
    request->send(400, "application/json", "{\"error\":\"رمز جدید باید حداقل ۴ حرف باشد\"}");
    return;
  }

  userPassword = newpass;
  prefs.putString("userPassword", newpass);
  request->send(200, "application/json", "{\"success\":true}"); }));

  server.addHandler(new AsyncCallbackJsonWebHandler("/api/reset", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                    {
  // عملیات ریست فکتوری
  prefs.clear();
  request->send(200, "application/json", "{\"success\":true}"); }));

  server.on("/api/ResetEsp", HTTP_POST, [](AsyncWebServerRequest *request)
            {
   request->send(200, "application/json", "{\"success\":true}");

  delay(500);
  ESP.restart(); });

  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest *req)
            {
  Serial.println("[LOGIN] Request received!");  // اضافه شد
  if (req->hasParam("body", true)) {
    String body = req->getParam("body", true)->value();
    Serial.println("Body: " + body);  // اضافه شد
      DynamicJsonDocument doc(256);
      deserializeJson(doc, body);
      if (doc["user"] == username && doc["pass"] == userPassword) {
        req->send(200, "application/json", "{\"success\":true}");
      } else {
        req->send(401, "application/json", "{\"error\":\"اشتباه\"}");
      }
    } });

  server.on("/api/changepass", HTTP_POST, [](AsyncWebServerRequest *req)
            {
  if (!authenticateWeb(req)) return req->send(401);
  if (req->hasParam("body", true)) {
    String body = req->getParam("body", true)->value();
    DynamicJsonDocument doc(256);
    deserializeJson(doc, body);
    
    if (doc["old"] == userPassword && doc["new"].as<String>().length() >= 4) {
      userPassword = doc["new"].as<String>();
      prefs.putString("password", userPassword);
      req->send(200, "application/json", "{\"success\":true}");
    } else {
      req->send(400, "application/json", "{\"error\":\"خطا\"}");
    }
  } });

  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *req)
            {
    if (!authenticateWeb(req)) return req->send(401);
    for (int i = 0; i < numKeys; i++) {
      prefs.putString(defaultKeys[i].key.c_str(), defaultKeys[i].defaultVal);
    }
    req->send(200, "application/json", "{\"success\":true}"); });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req)
            { req->send(200, "text/html", index_html); });

  server.begin();
}

#pragma endregion

#pragma region PublicFuncs

void TaskDelay(int delay)
{
  String t = String(delay);
  Serial.println("delay for :  " + t + "  ms");
  vTaskDelay(delay);
}

void SetAllarmState()
{
  if (Public_MotionDetected)
  {
    Serial.println("***********************************************       allarm on");
    // آژیر روشن
    digitalWrite(AllarmBuzzerPin, !digitalRead(AllarmBuzzerPin));
    digitalWrite(AllarmLedPin, !digitalRead(AllarmBuzzerPin));
  }
  else
  {
    // آژیر خاموش
    digitalWrite(AllarmBuzzerPin, LOW);
    digitalWrite(AllarmLedPin, LOW);
  }
}

void SplitMobiles();

void SetPublicVariablesFromPrefs()
{
  public_SystemStatus = prefs.getBool("systemStatus", false);

  if (!prefs.isKey("username"))
    prefs.putString("username", "admin");
  if (!prefs.isKey("password"))
    prefs.putString("password", "1234");

  username = prefs.getString("username", "admin");
  userPassword = prefs.getString("password", "1234");
  // --- تنظیم کلیدهای پیش‌فرض ---
  for (int i = 0; i < numKeys; i++)
  {
    if (!prefs.isKey(defaultKeys[i].key.c_str()))
    {
      prefs.putString(defaultKeys[i].key.c_str(), defaultKeys[i].defaultVal);
    }
  }

  ssidName = prefs.getString("wifi_Ssid_Name", ssidNameDefault);
  ssidPassword = prefs.getString("wifi_Ssid_Password", ssidPasswordDefault);

  public_AlertEnabled_Sms = prefs.getBool("SmsAlertEnabled", 1);
  public_AlertEnabled_Buzzer = prefs.getBool("BuzzerAlertEnabled", 1);
  public_OwnerMobileNumber = prefs.getString("OwnerMobile", "09127917347");
  public_AlternetMobile = prefs.getString("alternetMobile", "");
  public_AllAlternetMobiles = prefs.getString("AllAlternetMobiles", "");

  Serial.println("ssidName :  " + ssidName + "    password    :    " + ssidPassword);
  Serial.println("public_OwnerMobileNumber :  " + public_OwnerMobileNumber);
  SplitMobiles();
}

void StartSoftAP()
{
  // --- راه‌اندازی Access Point ---
  WiFi.mode(WIFI_AP);

  bool ap_started = WiFi.softAP(ssidName, ssidPassword); // نام و رمز دلخواه

  if (ap_started)
  {
    Serial.println("AP Started Successfully!");
    Serial.print("IP Address: http://");
    Serial.println(WiFi.softAPIP()); // معمولاً: 192.168.4.1
  }
  else
  {
    Serial.println("AP Failed to Start!");
  }
}

#pragma endregion

#pragma region SmsFuncs

bool isGpsOn();
bool getGpsLocation();
void compileSms(String smsText, String num);
String ReportSmsTextGenerator(String senderNumber, String msg);
void monitorInputSms();
bool SenAtCommanSim808(String command, String expectedResponse, int timeout);
void SetupSim();
bool isAuthorizedNumber(String num);
void monitorInputSmsStateMachine();
void processIncomingSmsQueue();
void SmsSender();

void SplitMobiles()
{
  public_List_AllAlternetMobiles.clear(); // پاک‌سازی لیست قبلی

  // ابتدا عنصر اول لیست رو با شماره اصلی تنظیم می‌کنیم
  public_List_AllAlternetMobiles.push_back(public_AlternetMobile);

  int start = 0;
  int index = public_AllAlternetMobiles.indexOf('*');

  while (index != -1)
  {
    String mobile = public_AllAlternetMobiles.substring(start, index);
    if (mobile.length() > 0)
    {
      public_List_AllAlternetMobiles.push_back(mobile);
    }
    start = index + 1;
    index = public_AllAlternetMobiles.indexOf('*', start);
  }

  // افزودن آخرین شماره بعد از آخرین '*'
  String lastMobile = public_AllAlternetMobiles.substring(start);
  if (lastMobile.length() > 0)
  {
    public_List_AllAlternetMobiles.push_back(lastMobile);
  }
}

void SmsSender()
{
  Serial.println("start sms sender...... ");
  int selectedPriority = -1;
  int selectedIndex = -1;
  unsigned long oldestTime = ULONG_MAX;

  if (smsState == SMS_IDLE)
  {
    // پیدا کردن قدیمی‌ترین پیام با اولویت بالا
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

    // اگر پیام پیدا شد، آماده‌سازی برای ارسال
    if (selectedIndex != -1)
    {
      currentPriority = selectedPriority;
      currentMessage = smsQueues[selectedPriority][selectedIndex];
      smsQueueHead[selectedPriority] = (selectedIndex + 1) % SMS_QUEUE_SIZE;
      smsState = SMS_INIT;
      smsTimer = millis();
      return;
    }

    return; // هیچ پیامکی در صف نیست
  }

  Serial.println("state   :   " + String(smsState) + "   number :  " + currentMessage.number + "   text :  " + currentMessage.text);

  switch (smsState)
  {
  case SMS_INIT:
    SIM808.println("AT+CMGF=1");
    smsState = SMS_SEND_HEADER;
    smsTimer = millis();
    TaskDelay(100); // کمی صبر برای دریافت پاسخ
    while (SIM808.available())
    {
      Serial.write(SIM808.read()); // چاپ پاسخ روی مانیتور سریال
    }
    Serial.println("       command   :   AT+CMGF=1");
    break;

  case SMS_SEND_HEADER:
    if (millis() - smsTimer > 500)
    {
      SIM808.print("AT+CMGS=\"");
      SIM808.print(currentMessage.number);
      SIM808.println("\"");
      smsState = SMS_SEND_BODY;
      smsTimer = millis();
    }
    TaskDelay(100); // کمی صبر برای دریافت پاسخ
    while (SIM808.available())
    {
      Serial.write(SIM808.read()); // چاپ پاسخ روی مانیتور سریال
    }
    Serial.println("       command   :   AT+CMGS=\"");
    break;

  case SMS_SEND_BODY:
    if (millis() - smsTimer > 500)
    {
      SIM808.print(currentMessage.text);
      smsState = SMS_SEND_CTRLZ;
      smsTimer = millis();
    }
    TaskDelay(100); // کمی صبر برای دریافت پاسخ
    while (SIM808.available())
    {
      Serial.write(SIM808.read()); // چاپ پاسخ روی مانیتور سریال
    }
    Serial.println("       command   :   " + currentMessage.text);
    break;

  case SMS_SEND_CTRLZ:
    if (millis() - smsTimer > 500)
    {
      SIM808.write(26); // Ctrl+Z
      smsState = SMS_WAIT_RESPONSE;
      smsTimer = millis();
    }
    TaskDelay(100); // کمی صبر برای دریافت پاسخ
    while (SIM808.available())
    {
      Serial.write(SIM808.read()); // چاپ پاسخ روی مانیتور سریال
    }
    Serial.println("       command   :  26");
    break;

  case SMS_WAIT_RESPONSE:
    if (millis() - smsTimer > 3000)
    {
      Serial.printf("📩 SMS sent to %s: %s\n",
                    currentMessage.number.c_str(),
                    currentMessage.text.c_str());

      smsState = SMS_IDLE;
      currentPriority = -1;
    }

    Serial.println("       command   :   finish");
    break;
  }
}

bool isGpsOn()
{
  SenAtCommanSim808("AT+CGNSPWR?", "OK", 1000);
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < 1000)
  {
    while (SIM808.available())
    {
      char c = SIM808.read();
      resp += c;
    }
  }
  resp.trim();
  // اگر مقدار بعد از +CGNSPWR: 1 بود یعنی روشن است
  int idx = resp.indexOf("+CGNSPWR:");
  if (idx != -1)
  {
    String val = resp.substring(idx + 9); // بعد از +CGNSPWR:
    val.trim();
    if (val.startsWith("1"))
    {
      Serial.println("gps power on success... resp : " + resp + "-------  idx  " + idx);
      return true;
    }
  }
  Serial.println("gps power on failed... resp : " + resp + "-------  idx  " + idx);

  return false;
}

bool getGpsLocation()
{
  if (!isGpsOn())
  {
    SenAtCommanSim808("AT+CGNSPWR=1", "OK", 1000);
    TaskDelay(1000); // اجازه بده روشن شود
  }

  Serial.println("getGpsLocation()");
  SenAtCommanSim808("AT+CGNSINF", "OK", 1000);

  TaskDelay(150); // اجازه برای پاسخ

  unsigned long start = millis();
  String fullResp = "";

  // خواندن کل پاسخ (تا timeout)
  while (millis() - start < 3000)
  {
    while (SIM808.available())
    {
      char c = SIM808.read();
      fullResp += c;
    }
  }
  Serial.println("fullResp>>>>    " + fullResp);

  int p = fullResp.indexOf("+CGNSINF:");
  if (p == -1)
    return false;

  Serial.println("getGpsLocation1>>>>    " + p);
  // payload بعد از :
  int col = fullResp.indexOf(':', p);
  if (col == -1)
    return false;
  String payload = fullResp.substring(col + 1);
  payload.trim();

  // helper برای گرفتن n-امین توکن (بعد از جداکننده ,)
  auto getToken = [&](int n) -> String
  {
    int startIdx = 0;
    for (int i = 0; i < n; ++i)
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

  String runStatus = getToken(0); // 0
  String fixStatus = getToken(1); // 1 -> 1 means fixed
  String utcTime = getToken(2);   // 2
  String latStr = getToken(3);    // 3 -> latitude
  String lonStr = getToken(4);    // 4 -> longitude

  if (fixStatus == "1" && latStr.length() > 2 && lonStr.length() > 2 && latStr != "0.000000" && lonStr != "0.000000")
  {
    latitude = latStr;
    longitude = lonStr;
    Serial.println("Google Maps: https://maps.google.com/?q=" + latitude + "," + longitude);
    return true;
  }

  Serial.println("⚠️ GPS not fixed yet!");
  return false;
}

bool isAuthorizedNumber(String num)
{
  for (int i = 0; i < allowedCount; i++)
  {
    if (public_List_AllAlternetMobiles[i] == num)
    {
      return true;
    }
  }
  return false;
}

String ReportSmsTextGenerator(String senderNumber, String msg)
{
  String result = "sender number : " + senderNumber + " ---msg--- " + msg;
  return result;
}

bool enqueueSms(String number, String text, int priority)
{

  Serial.println(" enqueueSms ====>>>>>  number :  " + number + "  text:  " + text + "  priority:  " + priority);
  if (priority < 0 || priority > 2)
  {
    Serial.println(" bad priority ......   priority:  " + priority);
    return false;
  }

  int nextTail = (smsQueueTail[priority] + 1) % SMS_QUEUE_SIZE;

  smsQueues[priority][smsQueueTail[priority]] = {number, text, priority, millis()};
  smsQueueTail[priority] = nextTail;
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

    // اگر 500ms از آخرین دریافت گذشته، فرض کن پیامک کامل شده
    if (millis() - smsRxLastReceive > 500 && smsRxBuffer.length() > 0)
    {
      smsRxState = SMS_RX_WAITING;
    }
    break;

  case SMS_RX_WAITING:
    if (smsRxBuffer.indexOf("+CMT:") != -1)
    {
      // استخراج شماره فرستنده
      String sender = "";
      int q1 = smsRxBuffer.indexOf("\"");
      int q2 = smsRxBuffer.indexOf("\"", q1 + 1);
      if (q1 != -1 && q2 != -1)
      {
        sender = smsRxBuffer.substring(q1 + 1, q2);
      }

      // استخراج متن پیامک
      String text = "";
      int lastQuote = smsRxBuffer.lastIndexOf("\"");
      if (lastQuote != -1 && lastQuote + 1 < smsRxBuffer.length())
      {
        text = smsRxBuffer.substring(lastQuote + 1);
      }

      text.replace("\r", "");
      text.replace("\n", "");
      text.trim();

      // پاکسازی کاراکترهای خراب
      for (int i = 0; i < text.length(); i++)
      {
        if (text[i] < 32 || text[i] > 126)
        {
          text[i] = ' ';
        }
      }

      // اضافه به صف
      int nextTail = (incomingSmsTail + 1) % INCOMING_SMS_QUEUE_SIZE;
      if (nextTail != incomingSmsHead)
      {
        incomingSmsQueue[incomingSmsTail] = {sender, text};
        incomingSmsTail = nextTail;
        Serial.println("📥 SMS queued from " + sender + ": " + text);
      }
      else
      {
        Serial.println("⚠️ Incoming SMS queue full. Message dropped.");
      }
    }

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

    // پردازش پیامک
    enqueueSms(public_OwnerMobileNumber, ReportSmsTextGenerator(msg.sender, msg.text), 0);
    compileSms(msg.text, msg.sender);
  }
}

void compileSms(String smsText, String num)
{
  if (!isAuthorizedNumber(num))
  {
    if (num.startsWith("989"))
    {
      enqueueSms(num, "access denied", 0);

      Serial.println("⛔ access denied " + num);
      return;
    }
  }

  smsText.toLowerCase();
  smsText.trim();

  if (smsText == "gps")
  {
    Serial.println("📨 Command received: GPS");

    if (getGpsLocation())
    {
      String link = "https://maps.google.com/?q=" + latitude + "," + longitude;
      enqueueSms(num, link, 1); // شماره دلخواه
    }
    else
    {
      enqueueSms(num, "GPS not ready", 1); // شماره دلخواه
    }
  }
  else if (smsText == "disarm")
  {
    public_SystemStatus = 2;
    enqueueSms(num, "system is disarm", 1);
    enqueueSms(public_OwnerMobileNumber, "system is disarm for user : " + num, 1);
  }
  else if (smsText == "arm")
  {
    public_SystemStatus = 1;
    enqueueSms(num, "system is arm", 1);
    enqueueSms(public_OwnerMobileNumber, "system is arm for user : " + num, 1);
  }
  else if (smsText == "smson")
  {
    public_SystemStatus = 1;
    enqueueSms(num, "system sms is on", 1);
    enqueueSms(public_OwnerMobileNumber, "system sms is on ,  user : " + num, 1);
  }
  else if (smsText == "smsoff")
  {
    public_SystemStatus = 0;
    enqueueSms(num, "system sms is off", 1);
    enqueueSms(public_OwnerMobileNumber, "system sms is off ,  user : " + num, 1);
  }
  else
  {
    Serial.println("📨 Unknown command: " + smsText);
  }
}

bool SenAtCommanSim808(String command, String expectedResponse, int timeout)
{
  // صبر تا زمانی که 1 ثانیه از آخرین اجرا گذشته باشه
  while (millis() - lastCommandTime < 1000)
  {
    vTaskDelay(10); // تاخیر کوتاه برای جلوگیری از بلاک شدن کامل
  }

  lastCommandTime = millis(); // ثبت زمان اجرای فعلی

  Serial.print("Sending command: ");
  Serial.println(command);

  SIM808.flush();          // پاک‌سازی بافر
  SIM808.println(command); // ارسال دستور

  unsigned long startTime = millis();
  String fullResponse = "";

  while (millis() - startTime < timeout)
  {
    if (SIM808.available())
    {
      String line = SIM808.readStringUntil('\n');
      line.trim(); // حذف فاصله‌ها و کاراکترهای اضافی

      if (line.length() > 0)
      {
        Serial.println("Received line: " + line);
        fullResponse += line;

        if (fullResponse.indexOf(expectedResponse) != -1)
        {
          Serial.println("✅ Command executed successfully.");
          TaskDelay(100);
          return true;
        }
      }
    }
  }

  Serial.println("❌ Failed to execute command: " + command);
  return false;
}

void SetupSim()
{
  SIM808.begin(9600, SERIAL_8N1, SIM808_RX, SIM808_TX);
  TaskDelay(5000);

  Serial.println("Initializing SIM808...");

  if (!SenAtCommanSim808("AT", "OK", 1000))
    return;
  // فعال کردن GPS
  if (!SenAtCommanSim808("AT+CGNSPWR=1", "OK", 2000))
    return; // روشن کردن GPS
  TaskDelay(2000);
  if (!SenAtCommanSim808("AT+CGNSSEQ=RMC", "OK", 2000))
    return; // مشخص کردن نوع داده GPS

  if (!SenAtCommanSim808("AT+CGNSINF", "OK", 2000))
    return; // بررسی وضعیت GPS

  if (!SenAtCommanSim808("ATE0", "OK", 1000))
    return; // خاموش کردن Echo برای تمیز بودن خروجی

  if (!SenAtCommanSim808("AT+CPIN?", "READY", 2000))
    return; // بررسی سیم‌کارت

  if (!SenAtCommanSim808("AT+CREG?", "0,1", 3000))
    return; // بررسی ثبت در شبکه
  if (!SenAtCommanSim808("AT+CSQ", "OK", 1000))
    return; // بررسی قدرت سیگنال (اختیاری ولی مفید)

  // تنظیمات SMS
  if (!SenAtCommanSim808("AT+CMGF=1", "OK", 1000))
    return; // حالت TEXT
  if (!SenAtCommanSim808("AT+CNMI=2,2,0,0,0", "OK", 1000))
    return; // نمایش مستقیم پیام‌ها

  Serial.println("✅ SIM808 Initialized Successfully!");
  public_SimIsOnline = true;
  enqueueSms(public_OwnerMobileNumber, "device setup is down!", 2);
}

void monitorInputSms()
{
  String response = "";
  unsigned long start = millis();
  while (millis() - start < 3000)
  {
    while (SIM808.available())
    {
      response += (char)SIM808.read();
    }
  }

  if (response.length() == 0 || response.indexOf("+CMT:") == -1)
    return;

  Serial.println("Raw Response from SIM808:");
  Serial.println(response);

  // استخراج شماره فرستنده
  String senderNumber = "";
  int quoteStart = response.indexOf("\"");
  int quoteEnd = response.indexOf("\"", quoteStart + 1);
  if (quoteStart != -1 && quoteEnd != -1)
  {
    senderNumber = response.substring(quoteStart + 1, quoteEnd);
  }

  // استخراج متن پیامک
  String smsText = "";
  int lastQuote = response.lastIndexOf("\"");
  if (lastQuote != -1 && lastQuote + 1 < response.length())
  {
    smsText = response.substring(lastQuote + 1);
  }

  // پاک‌سازی کاراکترهای خراب یا غیرقابل چاپ
  smsText.replace("\r", "");
  smsText.replace("\n", "");
  smsText.trim();
  for (int i = 0; i < smsText.length(); i++)
  {
    if (smsText[i] < 32 || smsText[i] > 126)
    {
      smsText[i] = ' ';
    }
  }

  Serial.println("------------ SMS Details ------------");
  Serial.print("Sender Number: ");
  Serial.println(senderNumber);
  Serial.print("Message Text: ");
  Serial.println(smsText);
  Serial.println("------------------------------------- send  SMS");

  if (senderNumber.length() > 3 && smsText.length() > 0)
  {
    enqueueSms(public_OwnerMobileNumber, ReportSmsTextGenerator(senderNumber, smsText), 0);
    compileSms(smsText, senderNumber);
  }
  else
  {
    Serial.println("⚠️ Failed to extract sender or message text.");
  }
}

#pragma endregion

#pragma region SensorVariables

struct PirSensor
{
  int pin;
  String name;
  bool isAnalog;
  unsigned long windowStart;
  int motionCount;
  bool motionDetected;

  PirSensor()
      : pin(-1), name(""), isAnalog(false),
        windowStart(0), motionCount(0), motionDetected(false) {}

  PirSensor(int p, const char *n, bool analog = false)
      : pin(p), name(String(n)), isAnalog(analog),
        windowStart(0), motionCount(0), motionDetected(false) {}
};

PirSensor sensors[] = {
    PirSensor(23, " sensor ofoghi "), PirSensor(22, " pir sensor ")};
const int sensorCount = sizeof(sensors) / sizeof(sensors[0]);

const unsigned long PIR_DETECTION_WINDOW = 2000; // بازه‌ی ۱ ثانیه
const int PIR_REQUIRED_COUNT = 2;                // تعداد تحریک لازم در بازه
#pragma endregion

#pragma region SensorFuncs

void CheckSensors();
bool PirIsTrige(PirSensor &sensor);

bool PirIsTrige(PirSensor &sensor)
{
  unsigned long now = millis();
  int pirState = digitalRead(sensor.pin);

  if (sensor.motionCount == 0)
  {
    sensor.windowStart = now;
  }
  // اگر تحریک شد
  if (pirState == HIGH)
  {
    Serial.println("PirIsTrige ===>>>   Sensor " + sensor.name + " triggered | Count: " + String(sensor.motionCount));

    // اگر پنجره‌ی زمانی تموم شده باشه، شمارنده صفر بشه و پنجره جدید شروع بشه
    if (now - sensor.windowStart > PIR_DETECTION_WINDOW)
    {
      sensor.windowStart = now;
      sensor.motionCount = 1;
    }
    else
    {
      sensor.motionCount++;
    }

    sensor.motionDetected = true;

    // اگر تعداد تحریک‌ها کافی بود
    if (sensor.motionCount >= PIR_REQUIRED_COUNT)
    {
      Serial.println("✅ Motion confirmed for sensor: " + sensor.name +
                     " | Count: " + String(sensor.motionCount) +
                     " / Required: " + String(PIR_REQUIRED_COUNT));

      // ریست شمارنده و شروع پنجره جدید
      sensor.motionCount = 0;
      sensor.windowStart = now;
      sensor.motionDetected = false;

      return true;
    }
  }
  else
  {
    Serial.println("PirIsTrige ===>>>   Sensor " + sensor.name + "        in else block.....................");
    // اگر تحریک نبود
    sensor.motionDetected = false;

    // اگر پنجره‌ی زمانی تموم شده باشه، شمارنده صفر بشه
    if (now - sensor.windowStart > PIR_DETECTION_WINDOW)
    {
      sensor.windowStart = now;
      sensor.motionCount = 0;
    }
  }

  return false;
}

void CheckSensors()
{
  if (!public_SystemStatus)
    return; // اگر سیستم فعال نیست، خروج

  for (int i = 0; i < sensorCount; i++)
  {
    PirSensor &sensor = sensors[i];

    if (PirIsTrige(sensor))
    {
      Serial.println("✅ Triggered: " + sensor.name + " → Sending SMS");

      Public_MotionDetected = true;

      for (int j = 0; j < allowedCount; j++)
      {
        String message = "Motion detected on sensor: " + sensor.name;
        Serial.println("Sending SMS to: " + public_List_AllAlternetMobiles[j] + " | Text: " + message);
        enqueueSms(public_List_AllAlternetMobiles[j], message, 2);
      }
    }
    else
    {
      Public_MotionDetected = false;
    }
  }
}
#pragma endregion

void setup()
{
#pragma region begins

  Serial.begin(9600);
  Serial.println("Starting ESP32 AP...");
  prefs.begin("config", false);

#pragma endregion

  SetPublicVariablesFromPrefs();

  StartSoftAP();

  HtmlFunctions();

  SetupSim();
  prefs.begin("clock", false);
  epochStartTime = prefs.getULong("epochStartTime", 0);
}

void loop()
{
  monitorInputSms();
  monitorInputSmsStateMachine(); // دریافت پیامک بدون بلاک شدن
  processIncomingSmsQueue();     // پردازش پیامک‌ها با تأخیر
  SmsSender();
  SetAllarmState();
  CheckSensors();
}
