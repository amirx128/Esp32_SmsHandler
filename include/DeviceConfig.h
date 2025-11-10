// DeviceConfig.h
// Central place to configure sensor pins per device before build.
// Rule: set a sensor pin to 0 to mark it as NOT PRESENT on this device.
// You can also override these via PlatformIO `build_flags` (-D VIB_PIN=...) per env.

#pragma once

// Defaults (ESP32)
#ifndef VIB_PIN
#define VIB_PIN 15   // Vibration sensor (digital)
#endif

#ifndef PIR_PIN
#define PIR_PIN 18   // PIR sensor (digital)
#endif

#ifndef GAS_PIN
#define GAS_PIN 35   // MQ gas sensor (analog input)
#endif

// Capabilities derived from pin presence (0 = not present)
#define HAS_VIB (VIB_PIN > 0)
#define HAS_PIR (PIR_PIN > 0)
#define HAS_GAS (GAS_PIN > 0)

// DHT temperature/humidity sensor (digital, bidirectional)
#ifndef DHT_PIN
#define DHT_PIN 4    // set to a valid GPIO (not 34-39). 0 = disabled
#endif

#ifndef DHT_TYPE
#define DHT_TYPE 22  // 11 for DHT11, 22 for DHT22, etc.
#endif

#define HAS_DHT (DHT_PIN > 0)

// Future sensors can be added here similarly.
