#include "Logger.h"
#include "DeviceConfig.h"

#include <stdarg.h>

static bool g_logEnabled = true;
uint8_t logInSerialIsOn = 1;

bool isLogEnabled()
{
  return g_logEnabled;
}

void setLogEnabled(uint8_t enable)
{
  g_logEnabled = (enable != 0);
  logInSerialIsOn = enable ? 1 : 0;
  Serial.printf("[LOG] Logging %s\n", g_logEnabled ? "ENABLED" : "DISABLED");
}

void logf(uint8_t enable, const char *fmt, ...)
{
  if (!enable || !g_logEnabled || !logInSerialIsOn)
    return;

  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  Serial.print(buf);
}
