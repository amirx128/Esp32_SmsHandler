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

// -------------------------------------------------------------
// Global build-time toggles (extend via PlatformIO build_flags)
// -------------------------------------------------------------

#ifndef HAS_SIM_MODULE
#define HAS_SIM_MODULE 1   // 1 = SIM/SMS modem present on this board
#endif

#ifndef HAS_SIREN_ACTUATOR
#define HAS_SIREN_ACTUATOR 1   // 1 = buzzer/siren hardware is assembled
#endif

#ifndef DEFAULT_MESH_CHANNEL
#define DEFAULT_MESH_CHANNEL 6
#endif

#ifndef IS_MONITORING
#define IS_MONITORING 1   // 1 = enable Serial logging, 0 = mute completely
#endif

constexpr bool IsMonitoring = (IS_MONITORING != 0);
constexpr bool DEVICE_HAS_SIM = (HAS_SIM_MODULE != 0);
constexpr bool DEVICE_HAS_SIREN = (HAS_SIREN_ACTUATOR != 0);
constexpr uint8_t DEVICE_DEFAULT_MESH_CHANNEL = DEFAULT_MESH_CHANNEL;
