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

// Future sensors can be added here similarly.
