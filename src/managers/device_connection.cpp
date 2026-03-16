/*
 * This file is part of the openinverter web interface
 *
 * Copyright (C) 2025 Nishanth Samala <contact@outlandnish.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include "device_connection.h"

#include <Arduino.h>

#include "can_task.h"
#include "device_discovery.h"

#include "models/can_event.h"
#include "protocols/sdo_protocol.h"

// External queue for events
extern QueueHandle_t canEventQueue;

#define DBG_OUTPUT_PORT Serial

namespace {
bool isExpectedSdoResponseForNode(const twai_message_t& frame, uint8_t nodeId) {
  return !frame.extd && frame.identifier == (SDO_RESPONSE_BASE_ID | nodeId);
}

bool isExpectedJsonAbort(const twai_message_t& frame) {
  if (frame.data[0] != SDOProtocol::ABORT) {
    return false;
  }

  const uint16_t index = frame.data[1] | (frame.data[2] << 8);
  const uint8_t subIndex = frame.data[3];
  return index == SDOProtocol::INDEX_STRINGS && subIndex == 0;
}

bool hasExpectedSegmentToggle(const twai_message_t& frame, bool toggleBit) {
  const uint8_t expectedToggle = toggleBit ? SDOProtocol::TOGGLE_BIT : 0;
  return (frame.data[0] & SDOProtocol::TOGGLE_BIT) == expectedToggle;
}
}  // namespace

DeviceConnection::DeviceConnection() {
  // Initialize arrays
  for (int i = 0; i < 4; i++) {
    serial_[i] = 0;
  }
  jsonFileName_[0] = '\0';

  // Create mutex for JSON buffer protection
  jsonBufferMutex_ = xSemaphoreCreateMutex();
}

DeviceConnection& DeviceConnection::instance() {
  static DeviceConnection instance;
  return instance;
}

void DeviceConnection::setState(State newState) {
  const bool wasDownloadingJson = isDownloadingJson();
  state_ = newState;
  resetStateStartTime();

  if (newState == ERROR && wasDownloadingJson && jsonRequestClientId_ != 0 && canEventQueue != nullptr) {
    CANEvent evt;
    evt.type = EVT_JSON_READY;
    evt.data.jsonReady.clientId = jsonRequestClientId_;
    evt.data.jsonReady.nodeId = nodeId_;
    evt.data.jsonReady.success = false;
    xQueueSend(canEventQueue, &evt, 0);
    DBG_OUTPUT_PORT.printf("[DeviceConnection] Sent JSON ready error event for client %lu\n",
                           (unsigned long)jsonRequestClientId_);
    jsonRequestClientId_ = 0;
  }
}

void DeviceConnection::setSerialPart(uint8_t index, uint32_t value) {
  if (index < 4) {
    serial_[index] = value;
  }
}

uint32_t DeviceConnection::getSerialPart(uint8_t index) const {
  return (index < 4) ? serial_[index] : 0;
}

String DeviceConnection::getSerial() const {
  char buf[40];
  snprintf(buf, sizeof(buf), "%" PRIX32 ":%" PRIX32 ":%" PRIX32 ":%" PRIX32, serial_[0], serial_[1], serial_[2],
           serial_[3]);
  return String(buf);
}

void DeviceConnection::generateJsonFileName() {
  snprintf(jsonFileName_, sizeof(jsonFileName_), "/%" PRIx32 ".json", serial_[3]);
}

void DeviceConnection::resetStateStartTime() {
  stateStartTime_ = millis();
}

unsigned long DeviceConnection::getStateElapsedTime() const {
  return millis() - stateStartTime_;
}

bool DeviceConnection::hasStateTimedOut(unsigned long timeoutMs) const {
  return getStateElapsedTime() > timeoutMs;
}

void DeviceConnection::clearJsonCache() {
  if (xSemaphoreTake(jsonBufferMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    cachedParamJson_.clear();
    jsonReceiveBuffer_ = "";
    jsonTotalSize_ = 0;
    lastJsonParseFailed_ = false;
    xSemaphoreGive(jsonBufferMutex_);
  }
}

// Thread-safe JSON buffer accessors
String DeviceConnection::getJsonReceiveBufferCopy() {
  String copy;
  if (xSemaphoreTake(jsonBufferMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    copy = jsonReceiveBuffer_;
    xSemaphoreGive(jsonBufferMutex_);
  }
  return copy;
}

int DeviceConnection::getJsonReceiveBufferLength() {
  int len = 0;
  if (xSemaphoreTake(jsonBufferMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    len = jsonReceiveBuffer_.length();
    xSemaphoreGive(jsonBufferMutex_);
  }
  return len;
}

bool DeviceConnection::isJsonBufferEmpty() {
  bool empty = true;
  if (xSemaphoreTake(jsonBufferMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    empty = jsonReceiveBuffer_.isEmpty();
    xSemaphoreGive(jsonBufferMutex_);
  }
  return empty;
}

bool DeviceConnection::canSendParameterRequest() {
  unsigned long currentTime = micros();
  unsigned long timeSinceLastRequest = currentTime - lastParamRequestTime_;
  return timeSinceLastRequest >= minParamRequestIntervalUs_;
}

void DeviceConnection::markParameterRequestSent() {
  lastParamRequestTime_ = micros();
}

// Start JSON download (called when browser requests JSON)
void DeviceConnection::startJsonDownload() {
  if (state_ != IDLE) {
    DBG_OUTPUT_PORT.println("[DeviceConnection] Cannot start JSON download - not in IDLE state");
    return;
  }

  lastJsonParseFailed_ = false;
  jsonReceiveBuffer_ = "";
  jsonTotalSize_ = 0;
  toggleBit_ = false;
  setState(JSON_INIT_SENDING);
}

// Start JSON download for a specific client (non-blocking)
bool DeviceConnection::startJsonDownloadAsync(uint32_t clientId) {
  if (state_ != IDLE) {
    DBG_OUTPUT_PORT.println("[DeviceConnection] Cannot start JSON download - not in IDLE state");
    return false;
  }

  jsonRequestClientId_ = clientId;
  clearJsonCache();
  lastJsonParseFailed_ = false;
  jsonReceiveBuffer_ = "";
  jsonTotalSize_ = 0;
  toggleBit_ = false;
  setState(JSON_INIT_SENDING);
  DBG_OUTPUT_PORT.printf("[DeviceConnection] Started async JSON download for client %lu\n", (unsigned long)clientId);
  return true;
}

// Start serial acquisition (used after device reset)
void DeviceConnection::startSerialAcquisition() {
  if (state_ != IDLE) {
    DBG_OUTPUT_PORT.println("[DeviceConnection] Cannot start serial acquisition - not in IDLE state");
    return;
  }

  currentSerialPart_ = 0;
  toggleBit_ = false;
  setState(SERIAL_SENDING);
  DBG_OUTPUT_PORT.printf("[DeviceConnection] Starting serial acquisition for node %d\n", nodeId_);
}

// Non-blocking state machine processing (called from can_task loop)
void DeviceConnection::processConnection() {
  unsigned long currentTime = millis();
  twai_message_t rxframe;

  switch (state_) {
    case IDLE:
    case ERROR:
      // Nothing to do
      break;

    // =====================================================================
    // Serial number acquisition states
    // =====================================================================
    case SERIAL_SENDING:
      // Clear any stale responses
      SDOProtocol::clearPendingResponses();

      // Send request for current serial part
      SDOProtocol::requestElement(nodeId_, SDOProtocol::INDEX_SERIAL, currentSerialPart_);
      requestSentTime_ = currentTime;
      state_ = SERIAL_WAITING;
      break;

    case SERIAL_WAITING:
      // Non-blocking check for response
      if (SDOProtocol::waitForResponse(&rxframe, 0)) {
        // Check for abort
        if (rxframe.data[0] == SDOProtocol::ABORT) {
          DBG_OUTPUT_PORT.println("[DeviceConnection] SDO abort - error obtaining serial");
          setState(ERROR);
          break;
        }

        // Validate response is for serial index
        uint16_t rxIndex = rxframe.data[1] | (rxframe.data[2] << 8);
        if (rxIndex == SDOProtocol::INDEX_SERIAL && rxframe.data[3] == currentSerialPart_) {
          setSerialPart(currentSerialPart_, *(uint32_t*)&rxframe.data[4]);
          currentSerialPart_++;

          if (currentSerialPart_ < 4) {
            // More parts to fetch
            state_ = SERIAL_SENDING;
          } else {
            // Got all 4 parts
            generateJsonFileName();
            DBG_OUTPUT_PORT.printf("Got Serial Number %" PRIX32 ":%" PRIX32 ":%" PRIX32 ":%" PRIX32 "\r\n", serial_[0],
                                   serial_[1], serial_[2], serial_[3]);

            setState(IDLE);
            DBG_OUTPUT_PORT.println("Connection established. Parameter JSON available on request.");

            // Notify that connection is ready
            if (connectionReadyCallback_) {
              char serialStr[64];
              sprintf(serialStr, "%" PRIX32 ":%" PRIX32 ":%" PRIX32 ":%" PRIX32, serial_[0], serial_[1], serial_[2],
                      serial_[3]);
              connectionReadyCallback_(nodeId_, serialStr);
            }
          }
        }
      } else if ((currentTime - requestSentTime_) >= SDO_TIMEOUT_MS) {
        // Timeout - retry or error
        if (hasStateTimedOut(CONNECTION_TIMEOUT_MS)) {
          DBG_OUTPUT_PORT.println("[DeviceConnection] Connection timeout");
          setState(ERROR);
        } else {
          // Retry current part
          state_ = SERIAL_SENDING;
        }
      }
      break;

    // =====================================================================
    // JSON download states
    // =====================================================================
    case JSON_INIT_SENDING:
      // Clear any stale responses
      SDOProtocol::clearPendingResponses();

      // Send initiate upload request for strings index
      SDOProtocol::requestElement(nodeId_, SDOProtocol::INDEX_STRINGS, 0);
      requestSentTime_ = currentTime;
      state_ = JSON_INIT_WAITING;
      break;

    case JSON_INIT_WAITING:
      {
      bool handledInitResponse = false;
      while (SDOProtocol::waitForResponse(&rxframe, 0)) {
        if (!isExpectedSdoResponseForNode(rxframe, nodeId_)) {
          DBG_OUTPUT_PORT.printf("[OBTAIN_JSON] Ignoring init response for unexpected node 0x%03" PRIX32 "\r\n",
                                 rxframe.identifier);
          continue;
        }

        if (isExpectedJsonAbort(rxframe)) {
          DBG_OUTPUT_PORT.println("[DeviceConnection] SDO abort during JSON init");
          setState(ERROR);
          handledInitResponse = true;
          break;
        }

        const uint16_t rxIndex = rxframe.data[1] | (rxframe.data[2] << 8);
        const uint8_t rxSubIndex = rxframe.data[3];
        if (rxIndex != SDOProtocol::INDEX_STRINGS || rxSubIndex != 0) {
          DBG_OUTPUT_PORT.printf("[OBTAIN_JSON] Ignoring init response for index 0x%04X/%u\r\n", rxIndex,
                                 rxSubIndex);
          continue;
        }

        // Check for initiate upload response
        if ((rxframe.data[0] & SDOProtocol::READ) == SDOProtocol::READ) {
          DBG_OUTPUT_PORT.println("[OBTAIN_JSON] Initiate upload response received");

          if (rxframe.data[0] & SDOProtocol::SIZE_SPECIFIED) {
            const uint32_t reportedTotalSize = *(uint32_t*)&rxframe.data[4];
            if (reportedTotalSize == 0xFFFFUL || reportedTotalSize == 0xFFFFFFFFUL) {
              jsonTotalSize_ = 0;
              DBG_OUTPUT_PORT.printf(
                  "[OBTAIN_JSON] Reported total size: %lu bytes (device placeholder, treating as unknown)\r\n",
                  (unsigned long)reportedTotalSize);
            } else {
              jsonTotalSize_ = (int)reportedTotalSize;
              DBG_OUTPUT_PORT.printf("[OBTAIN_JSON] Reported total size: %lu bytes\r\n",
                                     (unsigned long)reportedTotalSize);
            }

            if (jsonProgressCallback_) {
              jsonProgressCallback_(0);
            }
          } else {
            jsonTotalSize_ = 0;
          }

          // Request first segment
          state_ = JSON_SEGMENT_SENDING;
          handledInitResponse = true;
          break;
        }
      }
      if (!handledInitResponse && (currentTime - requestSentTime_) >= SDO_TIMEOUT_MS) {
        DBG_OUTPUT_PORT.println("[DeviceConnection] JSON init timeout");
        setState(ERROR);
      }
      }
      break;

    case JSON_SEGMENT_SENDING:
      // Drop any late segment responses before asking for the next chunk.
      SDOProtocol::clearPendingResponses();
      SDOProtocol::requestNextSegment(nodeId_, toggleBit_);
      requestSentTime_ = currentTime;
      state_ = JSON_SEGMENT_WAITING;
      break;

    case JSON_SEGMENT_WAITING:
      {
      bool handledSegmentResponse = false;
      while (SDOProtocol::waitForResponse(&rxframe, 0)) {
        if (!isExpectedSdoResponseForNode(rxframe, nodeId_)) {
          DBG_OUTPUT_PORT.printf("[OBTAIN_JSON] Ignoring segment response for unexpected node 0x%03" PRIX32 "\r\n",
                                 rxframe.identifier);
          continue;
        }

        if (isExpectedJsonAbort(rxframe)) {
          DBG_OUTPUT_PORT.println("[DeviceConnection] SDO abort during JSON download");
          setState(ERROR);
          handledSegmentResponse = true;
          break;
        }

        const uint8_t command = rxframe.data[0];
        if ((command & 0xE0) != 0) {
          DBG_OUTPUT_PORT.printf("[OBTAIN_JSON] Ignoring non-segment response cmd=0x%02X\r\n", command);
          continue;
        }

        if (!hasExpectedSegmentToggle(rxframe, toggleBit_)) {
          DBG_OUTPUT_PORT.printf("[OBTAIN_JSON] Ignoring stale segment with toggle=%d expected=%d\r\n",
                                 (command & SDOProtocol::TOGGLE_BIT) ? 1 : 0, toggleBit_ ? 1 : 0);
          continue;
        }

        const bool isLastSegment = (command & SDOProtocol::SIZE_SPECIFIED) != 0;
        if (isLastSegment) {
          // Last segment - protect buffer access
          bool parseSuccess = false;
          if (xSemaphoreTake(jsonBufferMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            const int size = 7 - ((command >> 1) & 0x7);
            if (size < 0 || size > 7) {
              lastJsonParseFailed_ = true;
              DBG_OUTPUT_PORT.printf("[OBTAIN_JSON] Invalid final segment size=%d\r\n", size);
              cachedParamJson_.clear();
              jsonReceiveBuffer_ = "";
              jsonTotalSize_ = 0;
            } else {
              for (int i = 0; i < size; i++) {
                jsonReceiveBuffer_ += (char)rxframe.data[1 + i];
              }

              DBG_OUTPUT_PORT.println("[OBTAIN_JSON] Download complete");
              DBG_OUTPUT_PORT.printf("[OBTAIN_JSON] JSON size: %d bytes\r\n", jsonReceiveBuffer_.length());

              // Parse JSON
              cachedParamJson_.clear();
              DeserializationError error = deserializeJson(cachedParamJson_, jsonReceiveBuffer_);
              if (error) {
                lastJsonParseFailed_ = true;
                DBG_OUTPUT_PORT.printf("[OBTAIN_JSON] Parse error: %s\r\n", error.c_str());
                cachedParamJson_.clear();
                jsonReceiveBuffer_ = "";
                jsonTotalSize_ = 0;
              } else {
                lastJsonParseFailed_ = false;
                DBG_OUTPUT_PORT.println("[OBTAIN_JSON] Parsed successfully");
                parseSuccess = true;
              }
            }
            xSemaphoreGive(jsonBufferMutex_);
          }

          // Send JSON ready event if a client requested it
          if (jsonRequestClientId_ != 0 && canEventQueue != nullptr) {
            CANEvent evt;
            evt.type = EVT_JSON_READY;
            evt.data.jsonReady.clientId = jsonRequestClientId_;
            evt.data.jsonReady.nodeId = nodeId_;
            evt.data.jsonReady.success = parseSuccess;
            xQueueSend(canEventQueue, &evt, 0);
            DBG_OUTPUT_PORT.printf("[OBTAIN_JSON] Sent JSON ready event for client %lu\n",
                                   (unsigned long)jsonRequestClientId_);
            jsonRequestClientId_ = 0;  // Clear after sending
          }

          setState(IDLE);
          handledSegmentResponse = true;
          break;
        }

        if ((command & 0x0F) != 0) {
          DBG_OUTPUT_PORT.printf("[OBTAIN_JSON] Ignoring malformed segment cmd=0x%02X\r\n", command);
          continue;
        }

        // Normal segment
        if (xSemaphoreTake(jsonBufferMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
          for (int i = 0; i < 7; i++) {
            jsonReceiveBuffer_ += (char)rxframe.data[1 + i];
          }
          xSemaphoreGive(jsonBufferMutex_);
        }
        toggleBit_ = !toggleBit_;
        state_ = JSON_SEGMENT_SENDING;
        handledSegmentResponse = true;
        break;
      }
      if (!handledSegmentResponse && (currentTime - requestSentTime_) >= SDO_TIMEOUT_MS) {
        // Timeout - retry
        DBG_OUTPUT_PORT.println("[DeviceConnection] JSON segment timeout, retrying");
        SDOProtocol::clearPendingResponses();
        state_ = JSON_SEGMENT_SENDING;
      }
      }
      break;
  }
}

bool DeviceConnection::connectToDevice(uint8_t nodeId, BaudRate baud, int txPin, int rxPin) {
  setCanPins(txPin, rxPin);
  setBaudRate(baud);

  // If a JSON download was in progress, send error to waiting client
  if (jsonRequestClientId_ != 0 && isDownloadingJson()) {
    DBG_OUTPUT_PORT.printf("[DeviceConnection] Interrupting download for client %lu due to reconnect\n",
                           (unsigned long)jsonRequestClientId_);
    // Send error event for interrupted download
    if (canEventQueue != nullptr) {
      CANEvent evt;
      evt.type = EVT_JSON_READY;
      evt.data.jsonReady.clientId = jsonRequestClientId_;
      evt.data.jsonReady.nodeId = nodeId_;
      evt.data.jsonReady.success = false;
      xQueueSend(canEventQueue, &evt, 0);
    }
    jsonRequestClientId_ = 0;
  }

  if (!initCanBusForDevice(nodeId, baud, txPin, rxPin)) {
    DBG_OUTPUT_PORT.println("Failed to initialize CAN bus");
    return false;
  }

  // Clear cached JSON when switching to a different device
  if (nodeId_ != nodeId) {
    clearJsonCache();
    DBG_OUTPUT_PORT.println("Cleared cached JSON (switching devices)");
  }

  nodeId_ = nodeId;
  currentSerialPart_ = 0;
  toggleBit_ = false;
  setState(SERIAL_SENDING);  // Start the serial acquisition state machine
  DBG_OUTPUT_PORT.printf("Connecting to node %d...\n", nodeId);
  return true;
}

bool DeviceConnection::initializeForScanning(BaudRate baud, int txPin, int rxPin) {
  setCanPins(txPin, rxPin);
  setBaudRate(baud);

  if (!initCanBusScanning(baud, txPin, rxPin)) {
    DBG_OUTPUT_PORT.println("Failed to initialize CAN bus for scanning");
    return false;
  }

  nodeId_ = 0;  // No specific device connected yet
  setState(IDLE);
  DBG_OUTPUT_PORT.println("CAN bus initialized (no device connected)");

  // Load saved devices into memory
  DeviceDiscovery::instance().loadDevices();
  return true;
}

bool DeviceConnection::resetToScanningMode() {
  if (canTxPin_ < 0 || canRxPin_ < 0) {
    DBG_OUTPUT_PORT.println("[DeviceConnection] Cannot reset to scanning - CAN pins not configured");
    return false;
  }

  if (!initCanBusScanning(baudRate_, canTxPin_, canRxPin_)) {
    DBG_OUTPUT_PORT.println("[DeviceConnection] Failed to reset CAN filter to scanning mode");
    return false;
  }

  nodeId_ = 0;
  setState(IDLE);
  clearJsonCache();
  DBG_OUTPUT_PORT.println("[DeviceConnection] Reset to scanning mode");
  return true;
}
