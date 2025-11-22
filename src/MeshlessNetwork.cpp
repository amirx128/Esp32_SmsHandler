#include "MeshlessNetwork.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <vector>
#include <deque>
#include <algorithm>
#include <cstring>

namespace
{
constexpr uint8_t  kMeshChannel        = 6;
constexpr uint8_t  kProtoVersion       = 1;
constexpr uint32_t kHelloIntervalMs    = 4000;
constexpr uint32_t kNodeOfflineMs      = 100000;
constexpr uint32_t kEventResendMs      = 2000;
constexpr uint32_t kEventLifetimeMs    = 60000;
constexpr size_t   kMaxHistory         = 32;
constexpr uint8_t  kAckTtl             = 5;
constexpr uint8_t  kFlagsRequiresSms   = 0x01;
constexpr uint8_t  kFlagsRequiresSiren = 0x02;

enum PacketKind : uint8_t
{
  PKT_HELLO = 1,
  PKT_EVENT = 2,
  PKT_ACK   = 3
};

struct MeshPacketHeader
{
  uint8_t  version;
  uint8_t  kind;
  uint8_t  ttl;
  uint8_t  reserved;
  uint32_t messageId;
  uint64_t originMac;
  uint64_t senderMac;
} __attribute__((packed));

struct HelloPayload
{
  char     nodeId[8];
  char     friendly[16];
  char     ssid[32];
  uint16_t capsMask;
  uint8_t  sensorCount;
} __attribute__((packed));

struct EventPayload
{
  uint8_t  flags;
  uint8_t  initialTtl;
  uint16_t reserved;
  uint64_t timestampMs;
  char     type[16];
  char     payload[64];
} __attribute__((packed));

struct AckPayload
{
  uint32_t messageId;
  uint64_t eventOrigin;
} __attribute__((packed));

struct NodeEntry
{
  uint64_t         mac = 0;
  String           nodeId;
  String           friendlyName;
  String           ssid;
  NodeCapabilities caps;
  uint32_t         lastSeenMs = 0;
  bool             online = false;
  bool             isLocal = false;
};

struct SeenEvent
{
  uint32_t id = 0;
  uint64_t origin = 0;
};

struct SeenAck
{
  uint32_t id = 0;
  uint64_t eventOrigin = 0;
  uint64_t ackNode = 0;
};

struct HistoryEntry
{
  uint32_t             messageId = 0;
  uint64_t             originMac = 0;
  String               originNode;
  String               type;
  String               payload;
  bool                 requiresSms = false;
  bool                 requiresSiren = false;
  uint64_t             timestampMs = 0;
  uint8_t              initialTtl = 0;
  uint8_t              expectedAckCount = 0;
  bool                 delivered = false;
  std::vector<String>  ackedBy;
};

struct PendingEvent
{
  uint32_t             messageId = 0;
  uint64_t             originMac = 0;
  String               type;
  String               payload;
  bool                 requiresSms = false;
  bool                 requiresSiren = false;
  uint8_t              ttl = 0;
  uint8_t              initialTtl = 0;
  uint32_t             lastSendMs = 0;
  uint32_t             createdMs = 0;
  uint64_t             timestampMs = 0;
  std::vector<uint64_t> expected;
  std::vector<uint64_t> acked;
  bool                 delivered = false;
};

MeshEventHandler           g_handler = nullptr;
std::vector<NodeEntry>     g_nodes;
std::vector<SeenEvent>     g_seenEvents;
std::vector<SeenAck>       g_seenAcks;
std::deque<HistoryEntry>   g_history;
std::vector<PendingEvent>  g_pendingEvents;
NodeCapabilities           g_localCaps;
String                     g_localSsid;
String                     g_friendlyName;
String                     g_localNodeId;
uint64_t                   g_localMac = 0;
bool                       g_initialized = false;
bool                       g_espNowReady = false;
uint32_t                   g_lastHelloMs = 0;
uint32_t                   g_eventCounter = 0;
bool                       g_capsDirty = true;
const uint8_t              kBroadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

String shortMac(uint64_t mac)
{
  char buf[5];
  snprintf(buf, sizeof(buf), "%04X", static_cast<uint16_t>(mac & 0xFFFF));
  return String(buf);
}

NodeEntry *findNode(uint64_t mac)
{
  for (auto &n : g_nodes)
    if (n.mac == mac)
      return &n;
  return nullptr;
}

NodeEntry &ensureNode(uint64_t mac)
{
  NodeEntry *existing = findNode(mac);
  if (existing)
    return *existing;
  NodeEntry entry;
  entry.mac = mac;
  entry.nodeId = shortMac(mac);
  entry.friendlyName = entry.nodeId;
  entry.online = false;
  g_nodes.push_back(entry);
  return g_nodes.back();
}

void updateLocalNodeEntry()
{
  NodeEntry &me = ensureNode(g_localMac);
  me.isLocal = true;
  me.online = true;
  me.lastSeenMs = millis();
  me.nodeId = g_localNodeId.length() ? g_localNodeId : shortMac(g_localMac);
  me.friendlyName = g_friendlyName.length() ? g_friendlyName : String("Node_") + me.nodeId;
  me.ssid = g_localSsid;
  me.caps = g_localCaps;
}

uint16_t encodeCaps(const NodeCapabilities &caps)
{
  uint16_t mask = 0;
  if (caps.hasSim) mask |= 1 << 0;
  if (caps.smsAlertEnabled) mask |= 1 << 1;
  if (caps.smsTxEnabled) mask |= 1 << 2;
  if (caps.hasBuzzer) mask |= 1 << 3;
  if (caps.hasLed) mask |= 1 << 4;
  if (caps.hasPir) mask |= 1 << 5;
  if (caps.hasVib) mask |= 1 << 6;
  if (caps.hasGas) mask |= 1 << 7;
  if (caps.hasDht) mask |= 1 << 8;
  if (caps.wifiEnabled) mask |= 1 << 9;
  return mask;
}

NodeCapabilities decodeCaps(uint16_t mask, uint8_t sensorCount)
{
  NodeCapabilities caps;
  caps.hasSim = mask & (1 << 0);
  caps.smsAlertEnabled = mask & (1 << 1);
  caps.smsTxEnabled = mask & (1 << 2);
  caps.hasBuzzer = mask & (1 << 3);
  caps.hasLed = mask & (1 << 4);
  caps.hasPir = mask & (1 << 5);
  caps.hasVib = mask & (1 << 6);
  caps.hasGas = mask & (1 << 7);
  caps.hasDht = mask & (1 << 8);
  caps.wifiEnabled = mask & (1 << 9);
  caps.sensorCount = sensorCount;
  return caps;
}

void copyString(char *dst, size_t len, const String &src)
{
  if (len == 0)
    return;
  size_t toCopy = std::min(len - 1, static_cast<size_t>(src.length()));
  memcpy(dst, src.c_str(), toCopy);
  dst[toCopy] = '\0';
  if (toCopy + 1 < len)
    memset(dst + toCopy + 1, 0, len - toCopy - 1);
}

String describeNode(uint64_t mac)
{
  NodeEntry *n = findNode(mac);
  if (!n)
    return shortMac(mac);
  if (n->friendlyName.length())
    return n->friendlyName;
  return n->nodeId;
}

bool eventSeen(uint32_t id, uint64_t origin)
{
  for (const auto &e : g_seenEvents)
    if (e.id == id && e.origin == origin)
      return true;
  return false;
}

void markEventSeen(uint32_t id, uint64_t origin)
{
  if (eventSeen(id, origin))
    return;
  if (g_seenEvents.size() >= 64)
    g_seenEvents.erase(g_seenEvents.begin());
  SeenEvent entry;
  entry.id = id;
  entry.origin = origin;
  g_seenEvents.push_back(entry);
}

bool ackSeen(uint32_t id, uint64_t origin, uint64_t ackNode)
{
  for (const auto &a : g_seenAcks)
    if (a.id == id && a.eventOrigin == origin && a.ackNode == ackNode)
      return true;
  return false;
}

void markAckSeen(uint32_t id, uint64_t origin, uint64_t ackNode)
{
  if (ackSeen(id, origin, ackNode))
    return;
  if (g_seenAcks.size() >= 64)
    g_seenAcks.erase(g_seenAcks.begin());
  SeenAck ack;
  ack.id = id;
  ack.eventOrigin = origin;
  ack.ackNode = ackNode;
  g_seenAcks.push_back(ack);
}

HistoryEntry *findHistory(uint64_t origin, uint32_t messageId)
{
  for (auto &entry : g_history)
    if (entry.originMac == origin && entry.messageId == messageId)
      return &entry;
  return nullptr;
}

PendingEvent *findPending(uint32_t messageId)
{
  for (auto &evt : g_pendingEvents)
  {
    if (evt.messageId == messageId && evt.originMac == g_localMac)
      return &evt;
  }
  return nullptr;
}

void pruneHistory()
{
  while (g_history.size() > kMaxHistory)
    g_history.pop_front();
}

void ensureBroadcastPeer()
{
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kBroadcastAddr, 6);
  peer.channel = kMeshChannel;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  esp_now_add_peer(&peer);
}

void sendPacket(PacketKind kind,
                uint8_t ttl,
                uint32_t messageId,
                uint64_t originMac,
                const uint8_t *payload,
                size_t payloadLen)
{
  if (!g_espNowReady)
    return;

  MeshPacketHeader header = {};
  header.version = kProtoVersion;
  header.kind = kind;
  header.ttl = ttl;
  header.reserved = 0;
  header.messageId = messageId;
  header.originMac = originMac;
  header.senderMac = g_localMac;

  std::vector<uint8_t> buffer(sizeof(header) + payloadLen);
  memcpy(buffer.data(), &header, sizeof(header));
  if (payloadLen > 0 && payload)
    memcpy(buffer.data() + sizeof(header), payload, payloadLen);

  esp_now_send(kBroadcastAddr, buffer.data(), buffer.size());
}

void broadcastHello()
{
  HelloPayload payload = {};
  copyString(payload.nodeId, sizeof(payload.nodeId), g_localNodeId);
  copyString(payload.friendly, sizeof(payload.friendly), g_friendlyName.length() ? g_friendlyName : g_localNodeId);
  copyString(payload.ssid, sizeof(payload.ssid), g_localSsid);
  payload.capsMask = encodeCaps(g_localCaps);
  payload.sensorCount = g_localCaps.sensorCount;
  sendPacket(PKT_HELLO, 0, 0, g_localMac, reinterpret_cast<uint8_t *>(&payload), sizeof(payload));
  g_capsDirty = false;
  g_lastHelloMs = millis();
}

void sendAck(uint32_t messageId, uint64_t eventOrigin, uint8_t ttl)
{
  AckPayload payload = {};
  payload.messageId = messageId;
  payload.eventOrigin = eventOrigin;
  sendPacket(PKT_ACK, ttl, messageId, g_localMac, reinterpret_cast<uint8_t *>(&payload), sizeof(payload));
}

void broadcastEvent(const PendingEvent &evt)
{
  EventPayload payload = {};
  payload.flags = 0;
  if (evt.requiresSms)
    payload.flags |= kFlagsRequiresSms;
  if (evt.requiresSiren)
    payload.flags |= kFlagsRequiresSiren;
  payload.initialTtl = evt.initialTtl;
  payload.timestampMs = evt.timestampMs;
  copyString(payload.type, sizeof(payload.type), evt.type);
  copyString(payload.payload, sizeof(payload.payload), evt.payload);
  sendPacket(PKT_EVENT, evt.ttl, evt.messageId, evt.originMac, reinterpret_cast<uint8_t *>(&payload), sizeof(payload));
}

void rebroadcastEvent(const MeshPacketHeader &header, const EventPayload &payload)
{
  if (header.ttl == 0)
    return;
  sendPacket(PKT_EVENT, header.ttl - 1, header.messageId, header.originMac,
             reinterpret_cast<const uint8_t *>(&payload), sizeof(payload));
}

void rebroadcastAck(const MeshPacketHeader &header, const AckPayload &payload)
{
  if (header.ttl == 0)
    return;
  sendPacket(PKT_ACK, header.ttl - 1, payload.messageId, header.originMac,
             reinterpret_cast<const uint8_t *>(&payload), sizeof(payload));
}

uint8_t snapshotExpectedNodes(std::vector<uint64_t> &out)
{
  out.clear();
  for (const auto &n : g_nodes)
  {
    if (!n.online)
      continue;
    out.push_back(n.mac);
  }
  if (std::find(out.begin(), out.end(), g_localMac) == out.end())
    out.push_back(g_localMac);
  return static_cast<uint8_t>(out.size());
}

void applyAck(uint32_t messageId, uint64_t eventOrigin, uint64_t ackNode)
{
  if (eventOrigin == 0)
    return;

  // Update pending list for local-origin events
  PendingEvent *pending = (eventOrigin == g_localMac) ? findPending(messageId) : nullptr;
  if (pending)
  {
    if (std::find(pending->acked.begin(), pending->acked.end(), ackNode) == pending->acked.end())
      pending->acked.push_back(ackNode);
    if (!pending->expected.empty() && pending->acked.size() >= pending->expected.size())
      pending->delivered = true;
  }

  // Update history entry
  HistoryEntry *history = findHistory(eventOrigin, messageId);
  if (history)
  {
    String name = describeNode(ackNode);
    bool already = std::find(history->ackedBy.begin(), history->ackedBy.end(), name) != history->ackedBy.end();
    if (!already)
      history->ackedBy.push_back(name);

    if (pending)
    {
      history->expectedAckCount = pending->expected.size();
      history->delivered = pending->delivered;
    }
  }
}

void handleHello(const MeshPacketHeader &header, const HelloPayload &payload)
{
  NodeEntry &node = ensureNode(header.originMac);
  bool wasOnline = node.online;
  node.nodeId = payload.nodeId[0] ? String(payload.nodeId) : shortMac(header.originMac);
  node.friendlyName = payload.friendly[0] ? String(payload.friendly) : node.nodeId;
  node.ssid = payload.ssid;
  node.caps = decodeCaps(payload.capsMask, payload.sensorCount);
  node.online = true;
  node.lastSeenMs = millis();
  if (!wasOnline)
  {
    Serial.printf("[MESH] Node %s (%s) online.\n", node.friendlyName.c_str(), node.nodeId.c_str());
  }
}

void handleEvent(const MeshPacketHeader &header, const EventPayload &payload)
{
  if (eventSeen(header.messageId, header.originMac))
    return;
  markEventSeen(header.messageId, header.originMac);

  NodeEntry &node = ensureNode(header.originMac);
  node.online = true;
  node.lastSeenMs = millis();

  HistoryEntry history;
  history.messageId = header.messageId;
  history.originMac = header.originMac;
  history.originNode = node.friendlyName.length() ? node.friendlyName : node.nodeId;
  history.type = payload.type;
  history.payload = payload.payload;
  history.requiresSms = (payload.flags & kFlagsRequiresSms) != 0;
  history.requiresSiren = (payload.flags & kFlagsRequiresSiren) != 0;
  history.timestampMs = payload.timestampMs;
  history.initialTtl = payload.initialTtl;
  history.delivered = false;
  g_history.push_back(history);
  pruneHistory();

  MeshEventInfo info;
  info.messageId = String(history.originNode) + "-" + String(history.messageId, HEX);
  info.originNode = history.originNode;
  info.originMac = header.originMac;
  info.type = history.type;
  info.payload = history.payload;
  info.requiresSms = history.requiresSms;
  info.requiresSiren = history.requiresSiren;
  info.ttl = payload.initialTtl;
  info.timestampMs = history.timestampMs;

  if (g_handler)
    g_handler(info);

  sendAck(header.messageId, header.originMac, kAckTtl);

  if (header.ttl > 0)
    rebroadcastEvent(header, payload);
}

void handleAck(const MeshPacketHeader &header, const AckPayload &payload)
{
  if (ackSeen(payload.messageId, payload.eventOrigin, header.originMac))
    return;
  markAckSeen(payload.messageId, payload.eventOrigin, header.originMac);

  applyAck(payload.messageId, payload.eventOrigin, header.originMac);

  if (header.ttl > 0)
    rebroadcastAck(header, payload);
}

void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len)
{
  if (len < static_cast<int>(sizeof(MeshPacketHeader)))
    return;
  MeshPacketHeader header;
  memcpy(&header, data, sizeof(header));
  if (header.version != kProtoVersion)
    return;

  const uint8_t *payload = data + sizeof(header);
  int payloadLen = len - sizeof(header);

  switch (header.kind)
  {
  case PKT_HELLO:
    if (payloadLen == static_cast<int>(sizeof(HelloPayload)))
    {
      HelloPayload hello;
      memcpy(&hello, payload, sizeof(hello));
      handleHello(header, hello);
    }
    break;
  case PKT_EVENT:
    if (payloadLen == static_cast<int>(sizeof(EventPayload)))
    {
      EventPayload evt;
      memcpy(&evt, payload, sizeof(evt));
      handleEvent(header, evt);
    }
    break;
  case PKT_ACK:
    if (payloadLen == static_cast<int>(sizeof(AckPayload)))
    {
      AckPayload ack;
      memcpy(&ack, payload, sizeof(ack));
      handleAck(header, ack);
    }
    break;
  default:
    break;
  }
}

void onEspNowSent(const uint8_t *mac, esp_now_send_status_t status)
{
  (void)mac;
  (void)status;
}

} // namespace

void MeshNet_SetFriendlyName(const String &name)
{
  g_friendlyName = name;
  if (g_initialized)
    updateLocalNodeEntry();
  g_capsDirty = true;
}

void MeshNet_SetLocalSsid(const String &ssid)
{
  g_localSsid = ssid;
  if (g_initialized)
    updateLocalNodeEntry();
  g_capsDirty = true;
}

void MeshNet_UpdateLocalCapabilities(const NodeCapabilities &caps)
{
  g_localCaps = caps;
  if (g_initialized)
    updateLocalNodeEntry();
  g_capsDirty = true;
}

void MeshNet_Init(MeshEventHandler handler)
{
  g_handler = handler;
  if (g_initialized)
    return;

  g_localMac = ESP.getEfuseMac();
  g_localNodeId = MeshNet_GetShortMac();
  updateLocalNodeEntry();

  if (WiFi.getMode() == WIFI_MODE_NULL)
    WiFi.mode(WIFI_AP_STA);

  esp_wifi_set_promiscuous(false);
  esp_wifi_set_channel(kMeshChannel, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() == ESP_OK)
  {
    g_espNowReady = true;
    esp_now_register_recv_cb(onEspNowRecv);
    esp_now_register_send_cb(onEspNowSent);
    ensureBroadcastPeer();
    Serial.println("[MESH] ESP-NOW initialized.");
  }
  else
  {
    Serial.println("[MESH] Failed to init ESP-NOW.");
  }

  g_initialized = true;
}

void MeshNet_Tick()
{
  if (!g_initialized || !g_espNowReady)
    return;

  uint32_t now = millis();
  if (g_capsDirty || (uint32_t)(now - g_lastHelloMs) >= kHelloIntervalMs)
    broadcastHello();

  for (auto &node : g_nodes)
  {
    if (!node.isLocal && node.online && node.lastSeenMs > 0)
    {
      if ((uint32_t)(now - node.lastSeenMs) > kNodeOfflineMs)
      {
        node.online = false;
        Serial.printf("[MESH] Node %s (%s) offline (timeout).\n",
                      node.friendlyName.c_str(), node.nodeId.c_str());
      }
    }
  }

  for (auto &evt : g_pendingEvents)
  {
    if (evt.delivered)
      continue;
    if ((uint32_t)(now - evt.createdMs) > kEventLifetimeMs)
    {
      evt.delivered = true;
      continue;
    }
    if ((uint32_t)(now - evt.lastSendMs) >= kEventResendMs)
    {
      broadcastEvent(evt);
      evt.lastSendMs = now;
    }
  }

  g_pendingEvents.erase(std::remove_if(g_pendingEvents.begin(),
                                       g_pendingEvents.end(),
                                       [](const PendingEvent &evt)
                                       { return evt.delivered; }),
                        g_pendingEvents.end());
}

String MeshNet_GetShortMac()
{
  return shortMac(ESP.getEfuseMac());
}

String MeshNet_GetLocalNodeId()
{
  if (g_localNodeId.length() == 0)
    g_localNodeId = MeshNet_GetShortMac();
  return g_localNodeId;
}

uint8_t MeshNet_GetMeshChannel()
{
  return kMeshChannel;
}

String MeshNet_RecordNetworkEvent(const String &type,
                                  const String &payload,
                                  bool requiresSms,
                                  bool requiresSiren,
                                  uint8_t ttl,
                                  uint64_t timestampMs)
{
  if (!g_initialized || !g_espNowReady)
    return "";

  PendingEvent evt;
  evt.messageId = ++g_eventCounter;
  evt.originMac = g_localMac;
  evt.type = type;
  evt.payload = payload;
  evt.requiresSms = requiresSms;
  evt.requiresSiren = requiresSiren;
  evt.ttl = ttl;
  evt.initialTtl = ttl;
  evt.lastSendMs = 0;
  evt.createdMs = millis();
  evt.timestampMs = timestampMs;
  evt.delivered = false;
  snapshotExpectedNodes(evt.expected);
  evt.acked.push_back(g_localMac);

  g_pendingEvents.push_back(evt);

  HistoryEntry history;
  history.messageId = evt.messageId;
  history.originMac = g_localMac;
  history.originNode = g_friendlyName.length() ? g_friendlyName : g_localNodeId;
  history.type = type;
  history.payload = payload;
  history.requiresSms = requiresSms;
  history.requiresSiren = requiresSiren;
  history.timestampMs = timestampMs;
  history.initialTtl = ttl;
  history.expectedAckCount = evt.expected.size();
  history.delivered = false;
  history.ackedBy.push_back(describeNode(g_localMac));
  g_history.push_back(history);
  pruneHistory();

  broadcastEvent(evt);
  auto *pending = findPending(evt.messageId);
  if (pending)
    pending->lastSendMs = millis();

  return String(history.originNode) + "-" + String(history.messageId, HEX);
}

void MeshNet_SerializeNodes(JsonArray arr)
{
  uint32_t now = millis();
  for (const auto &node : g_nodes)
  {
    JsonObject obj = arr.createNestedObject();
    obj["id"] = node.nodeId;
    obj["friendly"] = node.friendlyName;
    obj["ssid"] = node.ssid;
    obj["local"] = node.isLocal;
    obj["online"] = node.online;
    obj["ageMs"] = node.lastSeenMs ? (uint32_t)(now - node.lastSeenMs) : 0;
    JsonObject caps = obj.createNestedObject("caps");
    caps["sim"] = node.caps.hasSim;
    caps["smsAlert"] = node.caps.smsAlertEnabled;
    caps["smsTx"] = node.caps.smsTxEnabled;
    caps["buzzer"] = node.caps.hasBuzzer;
    caps["led"] = node.caps.hasLed;
    caps["pir"] = node.caps.hasPir;
    caps["vib"] = node.caps.hasVib;
    caps["gas"] = node.caps.hasGas;
    caps["dht"] = node.caps.hasDht;
    caps["wifi"] = node.caps.wifiEnabled;
    caps["sensors"] = node.caps.sensorCount;
  }
}

void MeshNet_SerializeEvents(JsonArray arr)
{
  for (const auto &entry : g_history)
  {
    JsonObject obj = arr.createNestedObject();
    obj["id"] = entry.originNode + "-" + String(entry.messageId, HEX);
    obj["origin"] = entry.originNode;
    obj["type"] = entry.type;
    obj["payload"] = entry.payload;
    obj["ts"] = (uint64_t)entry.timestampMs;
    obj["requiresSms"] = entry.requiresSms;
    obj["requiresSiren"] = entry.requiresSiren;
    obj["ttl"] = entry.initialTtl;
    obj["expectedAcks"] = entry.expectedAckCount;
    obj["delivered"] = entry.delivered;
    JsonArray ackArr = obj.createNestedArray("acks");
    for (const auto &name : entry.ackedBy)
      ackArr.add(name);
  }
}
