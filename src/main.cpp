#include <Arduino.h>

#pragma region define testCodes
// #define LED_PIN 16

// void setup()
// {
//   pinMode(LED_PIN, OUTPUT);
// }

// void loop()
// {
//   for (int i = 0; i < 3; i++)
//   {
//     digitalWrite(LED_PIN, HIGH);
//     delay(1000);
//     digitalWrite(LED_PIN, LOW);
//     delay(1000);
//   }
//   delay(2000);
// }
#pragma endregion

#pragma region define variables
HardwareSerial SIM808(1);
const int SIM808_RX = 16;
const int SIM808_TX = 17;
String latitude = "";
String longitude = "";

String smsBuffer = "";
unsigned long lastSmsCheck = 0;
const unsigned long smsTimeout = 3000; // زمان بررسی پیامک‌ها


#pragma endregion

#pragma region functions signature
bool initModuleSim808(String command, String expectedResponse, int timeout);
void SetupSim();
String sendSms(String function_number, String function_msg);
void monitorInputSms();
bool getGpsLocation();
void compileSms(String smsText , String num);
#pragma endregion

bool getGpsLocation()
{
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

  // فعال کردن GPS
  if (!initModuleSim808("AT+CGNSPWR=1", "OK", 2000))
    return; // روشن کردن GPS
  delay(2000);
  if (!initModuleSim808("AT+CGNSSEQ=RMC", "OK", 2000))
    return; // مشخص کردن نوع داده GPS
  delay(1000);
  if (!initModuleSim808("AT+CGNSINF", "OK", 2000))
    return; // بررسی وضعیت GPS

  Serial.println("✅ SIM808 Initialized Successfully!");
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

String sendSms(String function_number, String function_msg)
{
  String txt = "Sending SMS... to " + function_number + "msg :    " + function_msg;
  Serial.println();

  // اطمینان از تنظیم مجدد حالت متن
  if (!initModuleSim808("AT+CMGF=1", "OK", 1000))
  {
    Serial.println("init failed");
    return "false";
  }

  // ارسال دستور شروع پیامک
  SIM808.print("AT+CMGS=\"");
  SIM808.print(function_number); // شماره مقصد
  SIM808.println("\"");
  delay(2000); // زمان انتظار برای پاسخ

  // ارسال متن پیامک
  SIM808.print(function_msg);
  delay(100);

  // ارسال Ctrl+Z برای ارسال پیام
  SIM808.write(26);
  delay(3000); // زمان کافی برای ارسال پیامک

  Serial.print(" SMS sent.... : : :     ");
  Serial.println(txt);
  return "true";
}

void compileSms(String smsText , String num)
{
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
    sendSms("+989368054055", "senderNumber: " + senderNumber + " msg: " + smsText);
    compileSms(smsText,senderNumber);
  }
  else
  {
    Serial.println("⚠️ Failed to extract sender or message text.");
  }
}

void setup()
{
  Serial.begin(9600);

  SetupSim();
 
  String response = sendSms("+989368054055", "device setup done ");
}

void loop()
{
  monitorInputSms();
  getGpsLocation();
}
