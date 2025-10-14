#include <Arduino.h>

#pragma region define variables
HardwareSerial SIM808(1);
const int SIM808_RX = 16;
const int SIM808_TX = 17;
const int AllarmLedPin = 2;
const int AllarmBuzzerPin = 2;
int systemStatus = 1; // 1=>arm , 2 disarm , 3 halfArm
String latitude = "";
String longitude = "";
String adminMobileNumber = "+989368054055";
String allowedNumbers[] = {
    "+989121234567",
    "+989368054055", adminMobileNumber};
bool simIsOnline = false;
const int allowedCount = sizeof(allowedNumbers) / sizeof(allowedNumbers[0]);
String smsBuffer = "";
unsigned long lastSmsCheck = 0;
const unsigned long smsTimeout = 3000;           // زمان بررسی پیامک‌ها
const unsigned long PIR_DETECTION_WINDOW = 1000; // بازه‌ی ۱ ثانیه
const int PIR_REQUIRED_COUNT = 3;                // تعداد تحریک لازم در بازه
const int PIR_LOOP_DELAY = 100;                  // صبر در انتهای هر دور (میلی‌ثانیه)
struct PirSensor
{
  int pin;
  String name;
  unsigned long windowStart;
  int motionCount;
  bool motionDetected;

  PirSensor() : pin(-1), name(""), windowStart(0), motionCount(0), motionDetected(false) {}
  PirSensor(int p, const char *n)
      : pin(p), name(String(n)), windowStart(0), motionCount(0), motionDetected(false) {}
};

PirSensor sensors[] = {
    PirSensor(22, "single pir "),
    PirSensor(23, "box pir")};
const int sensorCount = sizeof(sensors) / sizeof(sensors[0]);
#pragma endregion

#pragma region functions signature
bool initModuleSim808(String command, String expectedResponse, int timeout);
void SetupSim();
String sendSms(String number, String msg);
void monitorInputSms();
bool getGpsLocation();
void compileSms(String smsText, String num);
bool PirIsTrige(PirSensor &sensor);
void Allarm(int blinkCountPerLoop, int blinkDelayMs, int loopCount, int loopPauseMs);
void AlarmTask(void *param);
void CheckSensorsTask(void *param);

bool isAuthorizedNumber(String num);
volatile bool alarmTriggered = false;

#pragma endregion

void Allarm(int blinkCountPerLoop, int blinkDelayMs, int loopCount, int loopPauseMs)
{
  for (int i = 0; i < loopCount; i++)
  {
    for (int j = 0; j < blinkCountPerLoop; j++)
    {
      digitalWrite(AllarmLedPin, HIGH);
      digitalWrite(AllarmBuzzerPin, HIGH);
      vTaskDelay(blinkDelayMs / portTICK_PERIOD_MS);
      digitalWrite(AllarmLedPin, LOW);
      digitalWrite(AllarmBuzzerPin, LOW);
      vTaskDelay(blinkDelayMs / portTICK_PERIOD_MS);
    }
    vTaskDelay(loopPauseMs / portTICK_PERIOD_MS); // وقفه بین حلقه‌ها
  }
}

void AlarmTask(void *param)
{
  // پارامتر ورودی رو بازیابی می‌کنیم
  int *args = (int *)param;
  int blinkCountPerLoop = args[0];
  int blinkDelayMs = args[1];
  int loopCount = args[2];
  int loopPauseMs = args[3];
  free(args); // حافظه آزاد شود

  Serial.println("🚨 Alarm started!");

  Allarm(blinkCountPerLoop, blinkDelayMs, loopCount, loopPauseMs);

  Serial.println("✅ Alarm finished. Resetting flag...");
  alarmTriggered = false;

  vTaskDelete(NULL); // حذف Task
}

void CheckSensorsTask(void *param)
{
  while (systemStatus == 1)
  {
    bool anyTriggered = false;

    for (int i = 0; i < sensorCount; i++)
    {
      bool triggered = PirIsTrige(sensors[i]);
      if (triggered)
      {
        anyTriggered = true;
        break; // فقط یکی کافی است
      }
    }

    // اگر حرکتی دید و هنوز آلارم فعال نیست
    if (anyTriggered && !alarmTriggered)
    {
      alarmTriggered = true;

      Serial.println("⚠️ Motion detected! Creating alarm thread...");

      // آرگومان‌های ورودی تابع آلارم
      int *args = (int *)malloc(4 * sizeof(int));
      args[0] = 3;    // blinkCountPerLoop
      args[1] = 300;  // blinkDelayMs
      args[2] = 2;    // loopCount
      args[3] = 1000; // loopPauseMs

      xTaskCreate(AlarmTask, "AlarmTask", 4096, args, 1, NULL);
    }

    vTaskDelay(100 / portTICK_PERIOD_MS); // هر 100 میلی‌ثانیه چک کنه
  }
}

bool isAuthorizedNumber(String num){
  for (int i = 0; i < allowedCount; i++)
  {
    if (allowedNumbers[i] == num)
    {
      return true;
    }
  }
  return false;
}

bool PirIsTrige(PirSensor &sensor){
  unsigned long now = millis();
  int pirState = digitalRead(sensor.pin);

  if (pirState == HIGH)
  {
    Serial.println("sensor >>> " + sensor.name + " <<< is triged , count : " + sensor.motionCount);
    if (now - sensor.windowStart > PIR_DETECTION_WINDOW)
    {
      sensor.windowStart = now;
      sensor.motionCount = 1;
      sensor.motionDetected = false;
    }
    else
    {
      sensor.motionCount++;
    }

    if (sensor.motionCount >= PIR_REQUIRED_COUNT && !sensor.motionDetected)
    {
      sensor.motionDetected = true;
      return true;
    }
  }

  if (now - sensor.windowStart > PIR_DETECTION_WINDOW)
  {
    sensor.motionCount = 0;
    sensor.motionDetected = false;
  }

  return false;
}

bool getGpsLocation()
{
  Serial.println("getGpsLocation()");
  SIM808.println("AT+CGNSINF");
  delay(150); // اجازه برای پاسخ

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

  int p = fullResp.indexOf("+CGNSINF:");
  if (p == -1)
    return false;

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

void SetupSim()
{
  SIM808.begin(9600, SERIAL_8N1, SIM808_RX, SIM808_TX);
  delay(5000);

  Serial.println("Initializing SIM808...");

  if (!initModuleSim808("AT", "OK", 1000))
    return;

  // فعال کردن GPS
  if (!initModuleSim808("AT+CGNSPWR=1", "OK", 2000))
    return; // روشن کردن GPS
  delay(2000);
  if (!initModuleSim808("AT+CGNSSEQ=RMC", "OK", 2000))
    return; // مشخص کردن نوع داده GPS
  delay(1000);
  if (!initModuleSim808("AT+CGNSINF", "OK", 2000))
    return; // بررسی وضعیت GPS

  if (!initModuleSim808("ATE0", "OK", 1000))
    return; // خاموش کردن Echo برای تمیز بودن خروجی
  if (!initModuleSim808("AT+CPIN?", "READY", 2000))
    return; // بررسی سیم‌کارت
  if (!initModuleSim808("AT+CREG?", "0,1", 3000))
    return; // بررسی ثبت در شبکه
  if (!initModuleSim808("AT+CSQ", "OK", 1000))
    return; // بررسی قدرت سیگنال (اختیاری ولی مفید)

  // تنظیمات SMS
  if (!initModuleSim808("AT+CMGF=1", "OK", 1000))
    return; // حالت TEXT
  if (!initModuleSim808("AT+CNMI=2,2,0,0,0", "OK", 1000))
    return; // نمایش مستقیم پیام‌ها

  Serial.println("✅ SIM808 Initialized Successfully!");
  simIsOnline = true;
}

String sendSms(String number, String msg)
{
  if (!simIsOnline)
  {
    SetupSim();
  }
  String txt = "Sending SMS... to " + number + "msg :    " + msg;
  Serial.println();

  if (!initModuleSim808("AT+CMGF=1", "OK", 1000))
  {
    Serial.println("init failed, reinitializing SIM...");
    simIsOnline = false;
    SetupSim();
    if (!simIsOnline || !initModuleSim808("AT+CMGF=1", "OK", 1000))
      return "false";
  }

  // ارسال دستور شروع پیامک
  SIM808.print("AT+CMGS=\"");
  SIM808.print(number); // شماره مقصد
  SIM808.println("\"");
  delay(2000); // زمان انتظار برای پاسخ

  // ارسال متن پیامک
  SIM808.print(msg);
  delay(100);

  // ارسال Ctrl+Z برای ارسال پیام
  SIM808.write(26);
  delay(3000); // زمان کافی برای ارسال پیامک

  Serial.print(" SMS sent.... : : :     ");
  Serial.println(txt);
  return "true";
}

bool initModuleSim808(String command, String expectedResponse, int timeout)
{
  Serial.print("Sending command: ");
  Serial.println(command);
  SIM808.println(command); // ارسال دستور AT به ماژول

  long int time = millis();
  while ((millis() - time) < timeout)
  {
    if (SIM808.available())
    { // بررسی داده‌های دریافتی
      String response = SIM808.readString();
      Serial.print("Response: ");
      Serial.println(response);

      if (response.indexOf(expectedResponse) != -1)
      {
        Serial.println("Command executed successfully.");
        return true; // اگر پاسخ مورد نظر دریافت شد
      }
    }
  }

  Serial.print("Failed to execute command: ");
  Serial.println(command); // نمایش دستور ناموفق در سریال مانیتور
  return false;            // اگر پاسخ موردنظر در زمان مشخص دریافت نشود
}

void compileSms(String smsText, String num)
{
  if (!isAuthorizedNumber(num))
  {
    if (num.startsWith("989"))
    {
      sendSms(num, "access denied");
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
      sendSms(num, link); // شماره دلخواه
    }
    else
    {
      sendSms(num, "GPS not ready");
    }
  }
  else if (smsText == "disarm")
  {
    systemStatus = 2;
    sendSms(num, "system is disarm");
    sendSms(adminMobileNumber, "system is disarm for user : " + num);
  }
  else if (smsText == "arm")
  {
    systemStatus = 1;
    sendSms(num, "system is arm");
    sendSms(adminMobileNumber, "system is arm for user : " + num);
  }
  else
  {
    Serial.println("📨 Unknown command: " + smsText);
  }
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
    sendSms(adminMobileNumber, "senderNumber: " + senderNumber + " msg: " + smsText);
    compileSms(smsText, senderNumber);
  }
  else
  {
    Serial.println("⚠️ Failed to extract sender or message text.");
  }
}

void setup()
{
  Serial.begin(9600);
  xTaskCreate(CheckSensorsTask, "CheckSensorsTask", 4096, NULL, 1, NULL);

  for (int i = 0; i < sensorCount; i++)
  {
    pinMode(sensors[i].pin, INPUT);
  }
  pinMode(AllarmBuzzerPin, OUTPUT);
  pinMode(AllarmLedPin, OUTPUT);

  SetupSim();

  String response = sendSms("+989368054055", "device setup done ");
}

void loop()
{
  monitorInputSms();
  getGpsLocation();
  vTaskDelay(1000 / portTICK_PERIOD_MS); // هر 100 میلی‌ثانیه چک کنه
}