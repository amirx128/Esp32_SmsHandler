#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

struct NodeCapabilities
{
  bool hasSim = false;
  bool smsAlertEnabled = false;
  bool smsTxEnabled = false;
  bool hasBuzzer = false;
  bool hasLed = false;
  bool hasPir = false;
  bool hasVib = false;
  bool hasGas = false;
  bool hasDht = false;
  bool wifiEnabled = false;
  uint8_t sensorCount = 0;
};

struct MeshEventInfo
{
  String messageId;
  String originNode;
  uint64_t originMac = 0;
  String type;
  String payload;
  bool requiresSms = false;
  bool requiresSiren = false;
  uint8_t ttl = 0;
  uint64_t timestampMs = 0;
};

using MeshEventHandler = void (*)(const MeshEventInfo &info);
using MeshTimeProvider = uint64_t (*)();
using MeshClockSetter  = void (*)(uint64_t timestampMs, bool broadcastMesh);

void    MeshNet_SetFriendlyName(const String &name);
void    MeshNet_SetLocalSsid(const String &ssid);
void    MeshNet_SetAuth(const String &ssid, const String &password);
void    MeshNet_UpdateLocalCapabilities(const NodeCapabilities &caps);
void    MeshNet_SetTimeProvider(MeshTimeProvider provider);
void    MeshNet_SetClockSetter(MeshClockSetter setter);
void    MeshNet_Init(MeshEventHandler handler);
void    MeshNet_Tick();
String  MeshNet_GetShortMac();
String  MeshNet_GetLocalNodeId();
uint8_t MeshNet_GetMeshChannel();
String  MeshNet_RecordNetworkEvent(const String &type,
                                   const String &payload,
                                   bool requiresSms,
                                   bool requiresSiren,
                                   uint8_t ttl,
                                   uint64_t timestampMs);
String  MeshNet_GetLocalFriendly();
void    MeshNet_SerializeNodes(JsonArray arr);
void    MeshNet_SerializeEvents(JsonArray arr);
