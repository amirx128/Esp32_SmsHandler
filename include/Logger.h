#pragma once

#include <Arduino.h>

// Simple serial logger with 0/1 gate per call and a global enable flag.
void setLogEnabled(uint8_t enable);
bool isLogEnabled();
void logf(uint8_t enable, const char *fmt, ...);
