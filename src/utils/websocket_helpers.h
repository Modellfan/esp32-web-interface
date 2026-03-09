#ifndef WEBSOCKET_HELPERS_H
#define WEBSOCKET_HELPERS_H

#include <ArduinoJson.h>

#include <AsyncWebSocket.h>
#include <cstring>

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
 * @return true if queued successfully
 */
inline bool sendParamValuesData(AsyncWebSocket& ws, AsyncWebSocketClient* client, uint32_t nodeId,
                                const String& rawParams) {
  if (client == nullptr || !client->canSend() || client->queueIsFull()) {
    return false;
  }

  char prefix[80];
  const int prefixLen = snprintf(prefix, sizeof(prefix), "{\"event\":\"paramValuesData\",\"data\":{\"nodeId\":%lu,\"rawParams\":",
                                 (unsigned long)nodeId);
  if (prefixLen <= 0 || prefixLen >= (int)sizeof(prefix)) {
    return false;
  }

  static constexpr char suffix[] = "}}";
  const size_t suffixLen = sizeof(suffix) - 1;
  const size_t paramsLen = rawParams.length();
  const size_t totalLen = (size_t)prefixLen + paramsLen + suffixLen;

  AsyncWebSocketMessageBuffer* buffer = nullptr;
  try {
    buffer = ws.makeBuffer(totalLen);
  } catch (...) {
    return false;
  }

  if (buffer == nullptr) {
    return false;
  }

  uint8_t* out = buffer->get();
  memcpy(out, prefix, (size_t)prefixLen);
  memcpy(out + prefixLen, rawParams.c_str(), paramsLen);
  memcpy(out + prefixLen + paramsLen, suffix, suffixLen);

  // Ownership of buffer is transferred; AsyncWebSocketClient::text deletes it.
  return client->text(buffer);
}

#endif  // WEBSOCKET_HELPERS_H
