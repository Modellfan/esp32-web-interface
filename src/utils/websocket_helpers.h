#ifndef WEBSOCKET_HELPERS_H
#define WEBSOCKET_HELPERS_H

#include <Arduino.h>
#include <ArduinoJson.h>

#include <AsyncWebSocket.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#ifdef DEBUG
struct WebSocketDebugState {
  char remoteIp[16] = "";
  uint16_t remotePort = 0;
  uint32_t connectedAtMs = 0;
  uint32_t lastInboundAtMs = 0;
  uint32_t lastOutboundAtMs = 0;
  uint32_t lastSnapshotAtMs = 0;
  size_t lastInboundLen = 0;
  size_t lastOutboundLen = 0;
  size_t lastQueueLen = 0;
  bool lastCanSend = false;
  bool lastQueueFull = false;
  uint16_t lastCloseCode = 0;
  char lastAction[32] = "";
  char lastOutboundEvent[32] = "";
  char lastCloseReason[64] = "";
};

extern std::map<uint32_t, WebSocketDebugState> g_webSocketDebugStates;
extern std::recursive_mutex g_webSocketDebugStateMutex;

inline WebSocketDebugState* getWebSocketDebugStateUnlocked(uint32_t clientId) {
  auto it = g_webSocketDebugStates.find(clientId);
  if (it == g_webSocketDebugStates.end()) {
    return nullptr;
  }
  return &it->second;
}

inline WebSocketDebugState* ensureWebSocketDebugStateUnlocked(AsyncWebSocketClient* client) {
  if (client == nullptr) {
    return nullptr;
  }

  return &g_webSocketDebugStates[client->id()];
}

inline void freeWebSocketDebugState(AsyncWebSocketClient* client) {
  if (client == nullptr) {
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(g_webSocketDebugStateMutex);
  g_webSocketDebugStates.erase(client->id());
}

inline void snapshotWebSocketDebugStateUnlocked(AsyncWebSocketClient* client, WebSocketDebugState* state) {
  if (client == nullptr || state == nullptr) {
    return;
  }

  const uint32_t now = millis();
  if (state->connectedAtMs == 0) {
    state->connectedAtMs = now;
  }
  state->lastSnapshotAtMs = now;
  state->lastQueueLen = client->queueLen();
  state->lastCanSend = client->canSend();
  state->lastQueueFull = client->queueIsFull();
}

inline void snapshotWebSocketDebugState(AsyncWebSocketClient* client) {
  std::lock_guard<std::recursive_mutex> lock(g_webSocketDebugStateMutex);
  WebSocketDebugState* state = ensureWebSocketDebugStateUnlocked(client);
  if (state == nullptr) {
    return;
  }
  snapshotWebSocketDebugStateUnlocked(client, state);
}

inline void setWebSocketDebugPeer(AsyncWebSocketClient* client) {
  std::lock_guard<std::recursive_mutex> lock(g_webSocketDebugStateMutex);
  WebSocketDebugState* state = ensureWebSocketDebugStateUnlocked(client);
  if (state == nullptr) {
    return;
  }

  snprintf(state->remoteIp, sizeof(state->remoteIp), "%s", client->remoteIP().toString().c_str());
  state->remotePort = client->remotePort();
  state->connectedAtMs = millis();
  snapshotWebSocketDebugStateUnlocked(client, state);
}

inline void recordWebSocketInbound(AsyncWebSocketClient* client, const char* action, size_t payloadLen) {
  std::lock_guard<std::recursive_mutex> lock(g_webSocketDebugStateMutex);
  WebSocketDebugState* state = ensureWebSocketDebugStateUnlocked(client);
  if (state == nullptr) {
    return;
  }

  snapshotWebSocketDebugStateUnlocked(client, state);
  state->lastInboundAtMs = millis();
  state->lastInboundLen = payloadLen;
  if (action != nullptr) {
    snprintf(state->lastAction, sizeof(state->lastAction), "%s", action);
  }
}

inline void recordWebSocketOutbound(AsyncWebSocketClient* client, const char* eventName, size_t payloadLen) {
  std::lock_guard<std::recursive_mutex> lock(g_webSocketDebugStateMutex);
  WebSocketDebugState* state = ensureWebSocketDebugStateUnlocked(client);
  if (state == nullptr) {
    return;
  }

  snapshotWebSocketDebugStateUnlocked(client, state);
  state->lastOutboundAtMs = millis();
  state->lastOutboundLen = payloadLen;
  if (eventName != nullptr) {
    snprintf(state->lastOutboundEvent, sizeof(state->lastOutboundEvent), "%s", eventName);
  }
}

inline void recordWebSocketCloseReason(AsyncWebSocketClient* client, uint16_t closeCode, const char* reason, size_t len) {
  std::lock_guard<std::recursive_mutex> lock(g_webSocketDebugStateMutex);
  WebSocketDebugState* state = ensureWebSocketDebugStateUnlocked(client);
  if (state == nullptr) {
    return;
  }

  state->lastCloseCode = closeCode;
  if (reason == nullptr || len == 0) {
    state->lastCloseReason[0] = '\0';
    return;
  }

  const size_t copyLen = (len < (sizeof(state->lastCloseReason) - 1)) ? len : (sizeof(state->lastCloseReason) - 1);
  memcpy(state->lastCloseReason, reason, copyLen);
  state->lastCloseReason[copyLen] = '\0';
}

inline void logWebSocketDebugSummary(const char* prefix, AsyncWebSocketClient* client) {
  if (client == nullptr) {
    Serial.printf("%s client=<null>\n", prefix);
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(g_webSocketDebugStateMutex);
  WebSocketDebugState* state = getWebSocketDebugStateUnlocked(client->id());
  if (state == nullptr) {
    Serial.printf("%s client#%lu state=<missing>\n", prefix, (unsigned long)client->id());
    return;
  }

  const uint32_t now = millis();
  const unsigned long connectedAgeMs = (state->connectedAtMs > 0) ? (now - state->connectedAtMs) : 0;
  const unsigned long inboundAgeMs = (state->lastInboundAtMs > 0) ? (now - state->lastInboundAtMs) : 0;
  const unsigned long outboundAgeMs = (state->lastOutboundAtMs > 0) ? (now - state->lastOutboundAtMs) : 0;

  Serial.printf(
      "%s client#%lu ip=%s:%u age=%lums queue=%u canSend=%d queueFull=%d lastAction=%s lastIn=%uB/%lums lastEvent=%s lastOut=%uB/%lums close=%u reason=%s\n",
      prefix, (unsigned long)client->id(), state->remoteIp[0] ? state->remoteIp : "?", state->remotePort,
      connectedAgeMs, (unsigned int)state->lastQueueLen, state->lastCanSend ? 1 : 0, state->lastQueueFull ? 1 : 0,
      state->lastAction[0] ? state->lastAction : "-", (unsigned int)state->lastInboundLen, inboundAgeMs,
      state->lastOutboundEvent[0] ? state->lastOutboundEvent : "-", (unsigned int)state->lastOutboundLen,
      outboundAgeMs, state->lastCloseCode, state->lastCloseReason[0] ? state->lastCloseReason : "-");
}
#else
inline void freeWebSocketDebugState(AsyncWebSocketClient* client) {
  (void)client;
}

inline void setWebSocketDebugPeer(AsyncWebSocketClient* client) {
  (void)client;
}

inline void recordWebSocketInbound(AsyncWebSocketClient* client, const char* action, size_t payloadLen) {
  (void)client;
  (void)action;
  (void)payloadLen;
}

inline void recordWebSocketOutbound(AsyncWebSocketClient* client, const char* eventName, size_t payloadLen) {
  (void)client;
  (void)eventName;
  (void)payloadLen;
}

inline void recordWebSocketCloseReason(AsyncWebSocketClient* client, uint16_t closeCode, const char* reason, size_t len) {
  (void)client;
  (void)closeCode;
  (void)reason;
  (void)len;
}

inline void logWebSocketDebugSummary(const char* prefix, AsyncWebSocketClient* client) {
  (void)prefix;
  (void)client;
}
#endif

enum class ParamValuesSendResult : uint8_t {
  Success,
  ClientNull,
  ClientNotConnected,
  QueueBusy,
  PrefixBuildFailed,
  BufferAllocFailed,
  EnqueueFailed,
};

inline const char* paramValuesSendResultToString(ParamValuesSendResult result) {
  switch (result) {
    case ParamValuesSendResult::Success:
      return "success";
    case ParamValuesSendResult::ClientNull:
      return "client_null";
    case ParamValuesSendResult::ClientNotConnected:
      return "client_not_connected";
    case ParamValuesSendResult::QueueBusy:
      return "queue_busy";
    case ParamValuesSendResult::PrefixBuildFailed:
      return "prefix_build_failed";
    case ParamValuesSendResult::BufferAllocFailed:
      return "buffer_alloc_failed";
    case ParamValuesSendResult::EnqueueFailed:
      return "enqueue_failed";
    default:
      return "unknown";
  }
}

/**
 * Sends a general error message via WebSocket
 *
 * @param client WebSocket client to send the error to
 * @param eventName The event name for the error (e.g., "canMappingError")
 * @param errorMessage The error message to send
 */
inline void sendWebSocketError(AsyncWebSocketClient* client, const char* eventName, const char* errorMessage) {
  JsonDocument errorDoc;
  errorDoc["event"] = eventName;
  errorDoc["data"]["error"] = errorMessage;
  String errorOutput;
  serializeJson(errorDoc, errorOutput);
  recordWebSocketOutbound(client, eventName, errorOutput.length());
  client->text(errorOutput);
}

/**
 * Sends a "Device is busy" error message via WebSocket
 *
 * @param client WebSocket client to send the error to
 * @param eventName The event name for the error (e.g., "canMappingError", "startDeviceError")
 */
inline void sendDeviceBusyError(AsyncWebSocketClient* client, const char* eventName) {
  sendWebSocketError(client, eventName, "Device is busy");
}

/**
 * Sends paramValuesData without building an extra large temporary String.
 * This reduces peak heap usage for large parameter JSON payloads.
 *
 * @param ws WebSocket server instance (for makeBuffer)
 * @param client Target WebSocket client
 * @param nodeId Connected node ID
 * @param rawParams JSON object string with parameter data
 * @return result code describing the queue/alloc outcome
 */
inline ParamValuesSendResult sendParamValuesData(AsyncWebSocket& ws, AsyncWebSocketClient* client, uint32_t nodeId,
                                                 const String& rawParams) {
  if (client == nullptr) {
#ifdef DEBUG
    Serial.println("[WebSocket][DEBUG] paramValuesData blocked: client=<null>");
#endif
    return ParamValuesSendResult::ClientNull;
  }

  if (client->status() != WS_CONNECTED) {
#ifdef DEBUG
    recordWebSocketOutbound(client, "paramValuesData", rawParams.length());
    logWebSocketDebugSummary("[WebSocket][DEBUG] paramValuesData blocked", client);
#endif
    return ParamValuesSendResult::ClientNotConnected;
  }

  if (!client->canSend() || client->queueIsFull()) {
#ifdef DEBUG
    recordWebSocketOutbound(client, "paramValuesData", rawParams.length());
    logWebSocketDebugSummary("[WebSocket][DEBUG] paramValuesData blocked", client);
#endif
    return ParamValuesSendResult::QueueBusy;
  }

  char prefix[80];
  const int prefixLen = snprintf(prefix, sizeof(prefix), "{\"event\":\"paramValuesData\",\"data\":{\"nodeId\":%lu,\"rawParams\":",
                                 (unsigned long)nodeId);
  if (prefixLen <= 0 || prefixLen >= (int)sizeof(prefix)) {
    return ParamValuesSendResult::PrefixBuildFailed;
  }

  static constexpr char suffix[] = "}}";
  const size_t suffixLen = sizeof(suffix) - 1;
  const size_t paramsLen = rawParams.length();
  const size_t totalLen = (size_t)prefixLen + paramsLen + suffixLen;
  recordWebSocketOutbound(client, "paramValuesData", totalLen);

  AsyncWebSocketMessageBuffer* buffer = nullptr;
  try {
    buffer = ws.makeBuffer(totalLen);
  } catch (...) {
    return ParamValuesSendResult::BufferAllocFailed;
  }

  if (buffer == nullptr) {
    return ParamValuesSendResult::BufferAllocFailed;
  }

  uint8_t* out = buffer->get();
  memcpy(out, prefix, (size_t)prefixLen);
  memcpy(out + prefixLen, rawParams.c_str(), paramsLen);
  memcpy(out + prefixLen + paramsLen, suffix, suffixLen);

  // Ownership of buffer is transferred; AsyncWebSocketClient::text deletes it.
  if (!client->text(buffer)) {
    return ParamValuesSendResult::EnqueueFailed;
  }

  return ParamValuesSendResult::Success;
}

#endif  // WEBSOCKET_HELPERS_H
