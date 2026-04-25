/*
 * This file is part of the esp32 web interface
 *
 * Copyright (C) 2024
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
#include "oi_can_task.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include <cstdarg>
#include <cstring>
#include <inttypes.h>
#include <new>

#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "driver/gpio.h"
#include "driver/twai.h"

#define DBG_OUTPUT_PORT Serial

namespace OICanTask {

namespace {

constexpr uint8_t SDO_REQUEST_DOWNLOAD = (1U << 5);
constexpr uint8_t SDO_REQUEST_UPLOAD = (2U << 5);
constexpr uint8_t SDO_REQUEST_SEGMENT = (3U << 5);
constexpr uint8_t SDO_TOGGLE_BIT = (1U << 4);
constexpr uint8_t SDO_RESPONSE_UPLOAD = (2U << 5);
constexpr uint8_t SDO_RESPONSE_DOWNLOAD = (3U << 5);
constexpr uint8_t SDO_EXPEDITED = (1U << 1);
constexpr uint8_t SDO_SIZE_SPECIFIED = 1U;
constexpr uint8_t SDO_WRITE = SDO_REQUEST_DOWNLOAD | SDO_EXPEDITED | SDO_SIZE_SPECIFIED;
constexpr uint8_t SDO_READ = SDO_REQUEST_UPLOAD;
constexpr uint8_t SDO_ABORT = 0x80;
constexpr uint8_t SDO_WRITE_REPLY = SDO_RESPONSE_DOWNLOAD;
constexpr uint8_t SDO_READ_REPLY = SDO_RESPONSE_UPLOAD | SDO_EXPEDITED | SDO_SIZE_SPECIFIED;

constexpr uint32_t SDO_ERR_RANGE = 0x06090030UL;

constexpr uint16_t SDO_INDEX_PARAM_UID = 0x2100;
constexpr uint16_t SDO_INDEX_MAP_TX = 0x3000;
constexpr uint16_t SDO_INDEX_MAP_RX = 0x3001;
constexpr uint16_t SDO_INDEX_MAP_RD = 0x3100;
constexpr uint16_t SDO_INDEX_SERIAL = 0x5000;
constexpr uint16_t SDO_INDEX_STRINGS = 0x5001;
constexpr uint16_t SDO_INDEX_COMMANDS = 0x5002;

constexpr uint8_t SDO_CMD_SAVE = 0;
constexpr uint8_t SDO_CMD_RESET = 2;
constexpr uint8_t SDO_CMD_START = 4;
constexpr uint8_t SDO_CMD_STOP = 5;

constexpr size_t PAGE_SIZE_BYTES = 1024;
constexpr size_t REQUEST_QUEUE_LENGTH = 24;
constexpr size_t RESPONSE_QUEUE_LENGTH = 96;
constexpr size_t MAX_SIMPLE_REQUESTS = 20;
constexpr size_t MAX_ACTIVE_REQUESTS = 4;
constexpr TickType_t DEFAULT_READ_TIMEOUT_TICKS = pdMS_TO_TICKS(40);
constexpr TickType_t DEFAULT_WRITE_TIMEOUT_TICKS = pdMS_TO_TICKS(150);
constexpr TickType_t DEFAULT_CONTROL_TIMEOUT_TICKS = pdMS_TO_TICKS(200);
constexpr TickType_t STARTUP_RETRY_INTERVAL_TICKS = pdMS_TO_TICKS(1000);
constexpr TickType_t SIMPLE_RETRY_BACKOFF_TICKS = pdMS_TO_TICKS(15);
constexpr uint8_t MAX_REQUEST_RETRIES = 1;
constexpr uint32_t MIN_INTERFRAME_GAP_US = 1500;
constexpr uint32_t TASK_IDLE_WAIT_MS = 5;
constexpr uint32_t DOWNLOAD_PROGRESS_STEP = 100;

enum class UpdateState : uint8_t {
  Idle,
  SendMagic,
  SendSize,
  SendPage,
  CheckCrc
};

struct ParamCacheEntry {
  char name[kNameLength];
  uint16_t id;
};

struct SimpleRequestEntry {
  bool inUse;
  bool waitingReply;
  Request request;
  uint16_t index;
  uint8_t subIndex;
  uint8_t expectedCommand;
  uint32_t writeValue;
  TickType_t queuedAt;
  TickType_t sentAt;
  TickType_t deadlineAt;
  TickType_t retryAt;
  uint8_t retryCount;
  char name[kNameLength];
};

QueueHandle_t g_requestQueue = nullptr;
QueueHandle_t g_responseQueue = nullptr;
SemaphoreHandle_t g_apiMutex = nullptr;
TaskHandle_t g_taskHandle = nullptr;
portMUX_TYPE g_stateLock = portMUX_INITIALIZER_UNLOCKED;

TaskState g_taskState = TaskState::Stopped;
StatusPayload g_cachedStatus = {};
uint32_t g_nextSequence = 1;

uint8_t g_nodeId = 1;
uint8_t g_baudRate = 2;
int g_txPin = -1;
int g_rxPin = -1;
bool g_driverInstalled = false;
bool g_traceEnabled = false;
TickType_t g_lastStartupAttempt = 0;
uint32_t g_serial[4] = {};
char g_jsonFileName[kPathLength] = "";

ParamCacheEntry* g_paramCache = nullptr;
size_t g_paramCacheCount = 0;

SimpleRequestEntry g_simpleRequests[MAX_SIMPLE_REQUESTS] = {};
bool g_hasPendingComplex = false;
Request g_pendingComplexRequest = {};
uint32_t g_lastTxMicros = 0;

UpdateState g_updateState = UpdateState::Idle;
File g_updateFile;
size_t g_updateCurrentByte = 0;
size_t g_updateCurrentPage = 0;
uint8_t g_updateTotalPages = 0;
uint32_t g_updateCrc = 0xFFFFFFFFUL;

uint32_t readU32LE(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

int32_t readI32LE(const uint8_t* data) {
  return static_cast<int32_t>(readU32LE(data));
}

uint16_t readU16LE(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

void writeU32LE(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value & 0xFFU);
  data[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
  data[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
  data[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

void copyCString(char* dest, size_t destSize, const char* src) {
  if (destSize == 0U) {
    return;
  }

  if (src == nullptr) {
    dest[0] = '\0';
    return;
  }

  strncpy(dest, src, destSize - 1U);
  dest[destSize - 1U] = '\0';
}

uint32_t baudRateToBitsPerSecond(uint8_t baudRate) {
  switch (baudRate) {
    case 0:
      return 125000U;
    case 1:
      return 250000U;
    default:
      return 500000U;
  }
}

bool isSimpleScheduledCommand(Command command) {
  switch (command) {
    case Command::GetValue:
    case Command::SetValue:
    case Command::RemoveCanMapping:
    case Command::SaveToFlash:
    case Command::StartStop:
      return true;
    default:
      return false;
  }
}

bool isReadyForSdoCommand() {
  return g_driverInstalled && g_taskState == TaskState::Ready && g_updateState == UpdateState::Idle;
}

uint16_t requestCountQueued() {
  uint16_t count = g_hasPendingComplex ? 1U : 0U;
  for (const SimpleRequestEntry& entry : g_simpleRequests) {
    if (entry.inUse && !entry.waitingReply) {
      count++;
    }
  }
  return count;
}

uint16_t requestCountActive() {
  uint16_t count = 0;
  for (const SimpleRequestEntry& entry : g_simpleRequests) {
    if (entry.inUse && entry.waitingReply) {
      count++;
    }
  }
  return count;
}

bool schemaFileExists() {
  return g_jsonFileName[0] != '\0' && SPIFFS.exists(g_jsonFileName);
}

void updateCachedStatus() {
  StatusPayload status = {};
  status.state = g_taskState;
  status.nodeId = g_nodeId;
  status.baudRate = g_baudRate;
  status.updateCurrentPage = static_cast<uint8_t>(g_updateCurrentPage);
  status.updateTotalPages = g_updateTotalPages;
  status.schemaAvailable = schemaFileExists();
  status.serialCrc = g_serial[3];
  status.queuedRequests = static_cast<uint8_t>(requestCountQueued());
  status.activeRequests = static_cast<uint8_t>(requestCountActive());
  status.maxActiveRequests = static_cast<uint8_t>(MAX_ACTIVE_REQUESTS);

  portENTER_CRITICAL(&g_stateLock);
  g_cachedStatus = status;
  portEXIT_CRITICAL(&g_stateLock);
}

void setTaskState(TaskState state) {
  g_taskState = state;
  updateCachedStatus();
}

void tracef(const char* format, ...) {
  if (!g_traceEnabled) {
    return;
  }

  char buffer[192];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  DBG_OUTPUT_PORT.print("[oi_can_task] ");
  DBG_OUTPUT_PORT.print(buffer);
  DBG_OUTPUT_PORT.print("\r\n");
}

#ifdef DEBUG_CAN
void logCanFrame(const char* direction, const twai_message_t& frame) {
  DBG_OUTPUT_PORT.printf("CAN %s id=0x%08" PRIX32 " dlc=%u%s%s data",
                         direction,
                         frame.identifier,
                         frame.data_length_code,
                         frame.extd ? " ext" : "",
                         frame.rtr ? " rtr" : "");

  if (!frame.rtr) {
    for (uint8_t i = 0; i < frame.data_length_code; i++) {
      DBG_OUTPUT_PORT.printf(" %02X", frame.data[i]);
    }
  }

  DBG_OUTPUT_PORT.print("\r\n");
}
#endif

esp_err_t transmitFrame(const twai_message_t& frame, TickType_t timeoutTicks) {
  const uint32_t nowUs = micros();
  const uint32_t elapsedUs = nowUs - g_lastTxMicros;
  if (elapsedUs < MIN_INTERFRAME_GAP_US) {
    delayMicroseconds(MIN_INTERFRAME_GAP_US - elapsedUs);
  }

  const esp_err_t result = twai_transmit(&frame, timeoutTicks);
  if (result == ESP_OK) {
    g_lastTxMicros = micros();
  }

#ifdef DEBUG_CAN
  if (result == ESP_OK) {
    logCanFrame("TX", frame);
  }
  else {
    DBG_OUTPUT_PORT.printf("CAN TX failed err=%d id=0x%08" PRIX32 "\r\n", result, frame.identifier);
  }
#endif

  return result;
}

esp_err_t receiveFrame(twai_message_t& frame, TickType_t timeoutTicks) {
  const esp_err_t result = twai_receive(&frame, timeoutTicks);

#ifdef DEBUG_CAN
  if (result == ESP_OK) {
    logCanFrame("RX", frame);
  }
#endif

  return result;
}

void fillSdoReadFrame(twai_message_t& frame, uint16_t index, uint8_t subIndex) {
  memset(&frame, 0, sizeof(frame));
  frame.extd = false;
  frame.identifier = 0x600 | g_nodeId;
  frame.data_length_code = 8;
  frame.data[0] = SDO_READ;
  frame.data[1] = static_cast<uint8_t>(index & 0xFFU);
  frame.data[2] = static_cast<uint8_t>((index >> 8) & 0xFFU);
  frame.data[3] = subIndex;
}

void fillSdoWriteFrame(twai_message_t& frame, uint16_t index, uint8_t subIndex, uint32_t value) {
  memset(&frame, 0, sizeof(frame));
  frame.extd = false;
  frame.identifier = 0x600 | g_nodeId;
  frame.data_length_code = 8;
  frame.data[0] = SDO_WRITE;
  frame.data[1] = static_cast<uint8_t>(index & 0xFFU);
  frame.data[2] = static_cast<uint8_t>((index >> 8) & 0xFFU);
  frame.data[3] = subIndex;
  writeU32LE(&frame.data[4], value);
}

void requestNextSegment(bool toggleBit) {
  twai_message_t frame = {};
  frame.extd = false;
  frame.identifier = 0x600 | g_nodeId;
  frame.data_length_code = 8;
  frame.data[0] = static_cast<uint8_t>(SDO_REQUEST_SEGMENT | (toggleBit ? SDO_TOGGLE_BIT : 0U));
  transmitFrame(frame, pdMS_TO_TICKS(10));
}

bool isExpectedSdoReply(const twai_message_t& frame, uint16_t index, uint8_t subIndex, uint8_t expectedCommand) {
  if (frame.identifier != (0x580 | g_nodeId)) {
    return false;
  }

  if (readU16LE(&frame.data[1]) != index || frame.data[3] != subIndex) {
    return false;
  }

  if (frame.data[0] == SDO_ABORT) {
    return true;
  }

  return frame.data[0] == expectedCommand;
}

Result abortToResult(Command command, const twai_message_t& frame) {
  const uint32_t abortCode = readU32LE(&frame.data[4]);
  switch (command) {
    case Command::SetValue:
      if (abortCode == SDO_ERR_RANGE) {
        return Result::ValueOutOfRange;
      }
      return Result::UnknownIndex;
    case Command::RemoveCanMapping:
    case Command::GetValue:
      return Result::UnknownIndex;
    default:
      return Result::CommError;
  }
}

void emitResponse(const Response& response) {
  if (g_responseQueue != nullptr) {
    xQueueSend(g_responseQueue, &response, pdMS_TO_TICKS(50));
  }
}

void emitAccepted(const Request& request) {
  tracef("accepted seq=%" PRIu32 " cmd=%u", request.sequence, static_cast<unsigned>(request.command));
  Response response = {};
  response.sequence = request.sequence;
  response.command = request.command;
  response.kind = ResponseKind::Accepted;
  response.result = Result::Ok;
  response.terminal = false;
  emitResponse(response);
}

void emitTerminal(const Request& request, Result result) {
  tracef("terminal seq=%" PRIu32 " cmd=%u result=%u",
         request.sequence,
         static_cast<unsigned>(request.command),
         static_cast<unsigned>(result));
  Response response = {};
  response.sequence = request.sequence;
  response.command = request.command;
  response.kind = (result == Result::Ok) ? ResponseKind::Completed : ResponseKind::Failed;
  response.result = result;
  response.terminal = true;
  emitResponse(response);
}

void emitStatusResponse(const Request& request) {
  Response response = {};
  response.sequence = request.sequence;
  response.command = request.command;
  response.kind = ResponseKind::Completed;
  response.result = Result::Ok;
  response.terminal = true;
  portENTER_CRITICAL(&g_stateLock);
  response.data.status = g_cachedStatus;
  portEXIT_CRITICAL(&g_stateLock);
  emitResponse(response);
}

void emitProgress(uint32_t sequence, Command command, const char* stage, uint32_t current, uint32_t total) {
  Response response = {};
  response.sequence = sequence;
  response.command = command;
  response.kind = ResponseKind::Progress;
  response.result = Result::Ok;
  response.terminal = false;
  copyCString(response.data.progress.stage, sizeof(response.data.progress.stage), stage);
  response.data.progress.current = current;
  response.data.progress.total = total;
  emitResponse(response);
}

void emitFileReady(uint32_t sequence, Command command, const char* path, uint32_t size) {
  Response response = {};
  response.sequence = sequence;
  response.command = command;
  response.kind = ResponseKind::FileReady;
  response.result = Result::Ok;
  response.terminal = false;
  copyCString(response.data.file.path, sizeof(response.data.file.path), path);
  response.data.file.size = size;
  emitResponse(response);
}

void emitValueEvent(uint32_t sequence,
                    Command command,
                    const char* name,
                    uint16_t paramId,
                    float value,
                    uint16_t sampleIndex) {
  tracef("value seq=%" PRIu32 " cmd=%u name=%s param=0x%04X value=%.3f sample=%u",
         sequence,
         static_cast<unsigned>(command),
         name,
         paramId,
         value,
         sampleIndex);
  Response response = {};
  response.sequence = sequence;
  response.command = command;
  response.kind = ResponseKind::Value;
  response.result = Result::Ok;
  response.terminal = false;
  copyCString(response.data.value.name, sizeof(response.data.value.name), name);
  response.data.value.paramId = paramId;
  response.data.value.value = value;
  response.data.value.sampleIndex = sampleIndex;
  emitResponse(response);
}

void emitMappingItemEvent(uint32_t sequence,
                          bool isRx,
                          uint32_t canId,
                          uint16_t paramId,
                          uint8_t position,
                          int8_t length,
                          float gain,
                          int8_t offset,
                          uint16_t index,
                          uint8_t subIndex) {
  Response response = {};
  response.sequence = sequence;
  response.command = Command::ReadCanMap;
  response.kind = ResponseKind::MappingItem;
  response.result = Result::Ok;
  response.terminal = false;
  response.data.mappingItem.isRx = isRx;
  response.data.mappingItem.canId = canId;
  response.data.mappingItem.paramId = paramId;
  response.data.mappingItem.position = position;
  response.data.mappingItem.length = length;
  response.data.mappingItem.gain = gain;
  response.data.mappingItem.offset = offset;
  response.data.mappingItem.index = index;
  response.data.mappingItem.subIndex = subIndex;
  emitResponse(response);
}

void freeParamCache() {
  delete[] g_paramCache;
  g_paramCache = nullptr;
  g_paramCacheCount = 0;
}

bool loadParamCache() {
  freeParamCache();

  if (!schemaFileExists()) {
    return false;
  }

  File file = SPIFFS.open(g_jsonFileName, "r");
  if (!file) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error != DeserializationError::Ok) {
    return false;
  }

  JsonObject root = doc.as<JsonObject>();
  size_t count = 0;
  for (JsonPair kv : root) {
    if (!kv.value()["id"].isNull()) {
      count++;
    }
  }

  if (count == 0U) {
    return true;
  }

  ParamCacheEntry* cache = new (std::nothrow) ParamCacheEntry[count];
  if (cache == nullptr) {
    return false;
  }

  size_t pos = 0;
  for (JsonPair kv : root) {
    const int id = kv.value()["id"].as<int>();
    if (id <= 0) {
      continue;
    }

    copyCString(cache[pos].name, sizeof(cache[pos].name), kv.key().c_str());
    cache[pos].id = static_cast<uint16_t>(id);
    pos++;
  }

  g_paramCache = cache;
  g_paramCacheCount = pos;
  return true;
}

int lookupParamId(const char* name) {
  if ((name == nullptr) || (name[0] == '\0')) {
    return -1;
  }

  if ((g_paramCache == nullptr) && !loadParamCache()) {
    return -1;
  }

  for (size_t i = 0; i < g_paramCacheCount; i++) {
    if (strncmp(g_paramCache[i].name, name, sizeof(g_paramCache[i].name)) == 0) {
      return g_paramCache[i].id;
    }
  }

  return -1;
}

uint16_t paramIdToIndex(int id) {
  return static_cast<uint16_t>(SDO_INDEX_PARAM_UID | ((static_cast<uint16_t>(id) >> 8) & 0xFFU));
}

uint8_t paramIdToSubIndex(int id) {
  return static_cast<uint8_t>(id & 0xFFU);
}

uint16_t requestParamId(uint16_t index, uint8_t subIndex) {
  return static_cast<uint16_t>(((index & 0xFFU) << 8) | subIndex);
}

bool waitForMatchingReply(twai_message_t& frame,
                          uint16_t index,
                          uint8_t subIndex,
                          uint8_t expectedCommand,
                          TickType_t timeoutTicks) {
  const TickType_t startTicks = xTaskGetTickCount();

  while ((xTaskGetTickCount() - startTicks) < timeoutTicks) {
    const TickType_t elapsedTicks = xTaskGetTickCount() - startTicks;
    const TickType_t remainingTicks = timeoutTicks - elapsedTicks;
    if (remainingTicks == 0) {
      break;
    }

    if (receiveFrame(frame, remainingTicks) != ESP_OK) {
      return false;
    }

    if (isExpectedSdoReply(frame, index, subIndex, expectedCommand)) {
      return true;
    }
  }

  return false;
}

Result readValueById(int id, float& valueOut) {
  twai_message_t txFrame;
  twai_message_t rxFrame;
  const uint16_t index = paramIdToIndex(id);
  const uint8_t subIndex = paramIdToSubIndex(id);

  fillSdoReadFrame(txFrame, index, subIndex);
  if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
    return Result::CommError;
  }

  if (!waitForMatchingReply(rxFrame, index, subIndex, SDO_READ_REPLY, DEFAULT_READ_TIMEOUT_TICKS)) {
    return Result::Timeout;
  }

  if (rxFrame.data[0] == SDO_ABORT) {
    return Result::UnknownIndex;
  }

  valueOut = static_cast<float>(readI32LE(&rxFrame.data[4])) / 32.0f;
  return Result::Ok;
}

Result writeValueAndWait(uint16_t index, uint8_t subIndex, uint32_t value, TickType_t timeoutTicks) {
  twai_message_t txFrame;
  twai_message_t rxFrame;

  fillSdoWriteFrame(txFrame, index, subIndex, value);
  if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
    return Result::CommError;
  }

  if (!waitForMatchingReply(rxFrame, index, subIndex, SDO_WRITE_REPLY, timeoutTicks)) {
    return Result::Timeout;
  }

  if (rxFrame.data[0] == SDO_ABORT) {
    return abortToResult(Command::SetValue, rxFrame);
  }

  return Result::Ok;
}

void cancelSimpleRequests(Result result) {
  for (SimpleRequestEntry& entry : g_simpleRequests) {
    if (entry.inUse) {
      emitTerminal(entry.request, result);
      entry = {};
    }
  }
  updateCachedStatus();
}

bool installDriver(uint8_t nodeId, uint8_t baudRate, int txPin, int rxPin) {
  twai_stop();
  twai_driver_uninstall();
  g_driverInstalled = false;

  twai_general_config_t gConfig = {
      .mode = TWAI_MODE_NORMAL,
      .tx_io = static_cast<gpio_num_t>(txPin),
      .rx_io = static_cast<gpio_num_t>(rxPin),
      .clkout_io = TWAI_IO_UNUSED,
      .bus_off_io = TWAI_IO_UNUSED,
      .tx_queue_len = 30,
      .rx_queue_len = 30,
      .alerts_enabled = TWAI_ALERT_NONE,
      .clkout_divider = 0,
      .intr_flags = 0};

  twai_timing_config_t tConfig;
  switch (baudRate) {
    case 0:
      tConfig = TWAI_TIMING_CONFIG_125KBITS();
      break;
    case 1:
      tConfig = TWAI_TIMING_CONFIG_250KBITS();
      break;
    default:
      tConfig = TWAI_TIMING_CONFIG_500KBITS();
      break;
  }

  const uint16_t id = static_cast<uint16_t>(0x580U + nodeId);
  const twai_filter_config_t fConfig = {
      .acceptance_code = (static_cast<uint32_t>(id) << 5) | (static_cast<uint32_t>(0x7deU) << 21),
      .acceptance_mask = 0x001F001FU,
      .single_filter = false};

  if (twai_driver_install(&gConfig, &tConfig, &fConfig) != ESP_OK) {
    return false;
  }

  if (twai_start() != ESP_OK) {
    twai_driver_uninstall();
    return false;
  }

  g_driverInstalled = true;
  g_lastTxMicros = 0;
  DBG_OUTPUT_PORT.printf("Driver installed tx=%d rx=%d baud=%" PRIu32 "\r\n",
                         txPin,
                         rxPin,
                         baudRateToBitsPerSecond(baudRate));
  return true;
}

Result obtainSerialSynchronously(uint32_t serialOut[4]) {
  twai_message_t txFrame;
  twai_message_t rxFrame;

  for (uint8_t subIndex = 0; subIndex < 4U; subIndex++) {
    fillSdoReadFrame(txFrame, SDO_INDEX_SERIAL, subIndex);
    if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
      return Result::CommError;
    }

    if (!waitForMatchingReply(rxFrame, SDO_INDEX_SERIAL, subIndex, SDO_READ_REPLY, DEFAULT_READ_TIMEOUT_TICKS)) {
      return Result::Timeout;
    }

    if (rxFrame.data[0] == SDO_ABORT) {
      return Result::CommError;
    }

    serialOut[subIndex] = readU32LE(&rxFrame.data[4]);
  }

  return Result::Ok;
}

Result downloadSchemaSynchronously(uint32_t sequence, bool emitEvents) {
  if (g_jsonFileName[0] == '\0') {
    return Result::FileError;
  }

  File file = SPIFFS.open(g_jsonFileName, "w+");
  if (!file) {
    return Result::FileError;
  }

  twai_message_t txFrame;
  twai_message_t rxFrame;
  fillSdoReadFrame(txFrame, SDO_INDEX_STRINGS, 0);
  if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
    file.close();
    SPIFFS.remove(g_jsonFileName);
    return Result::CommError;
  }

  if (emitEvents) {
    emitProgress(sequence, Command::DownloadSchemaJson, "started", 0, 0);
  }

  bool toggleBit = false;
  size_t bytesWritten = 0;
  size_t lastProgress = 0;

  while (true) {
    if (receiveFrame(rxFrame, pdMS_TO_TICKS(200)) != ESP_OK) {
      file.close();
      SPIFFS.remove(g_jsonFileName);
      return Result::Timeout;
    }

    if (rxFrame.identifier != (0x580 | g_nodeId)) {
      continue;
    }

    if (rxFrame.data[0] == SDO_ABORT) {
      file.close();
      SPIFFS.remove(g_jsonFileName);
      return Result::CommError;
    }

    if ((rxFrame.data[0] & SDO_READ) == SDO_READ) {
      requestNextSegment(toggleBit);
      continue;
    }

    if (((rxFrame.data[0] & SDO_SIZE_SPECIFIED) != 0U) && ((rxFrame.data[0] & SDO_READ) == 0U)) {
      const int size = 7 - static_cast<int>((rxFrame.data[0] >> 1) & 0x7U);
      if (size > 0) {
        file.write(&rxFrame.data[1], static_cast<size_t>(size));
        bytesWritten += static_cast<size_t>(size);
      }
      break;
    }

    if (rxFrame.data[0] == static_cast<uint8_t>(toggleBit ? SDO_TOGGLE_BIT : 0U)) {
      file.write(&rxFrame.data[1], 7);
      bytesWritten += 7U;
      toggleBit = !toggleBit;
      requestNextSegment(toggleBit);

      if (emitEvents && ((bytesWritten - lastProgress) >= DOWNLOAD_PROGRESS_STEP)) {
        lastProgress = bytesWritten;
        emitProgress(sequence, Command::DownloadSchemaJson, "downloading", bytesWritten, 0);
      }
    }
  }

  file.close();

  if (!loadParamCache()) {
    SPIFFS.remove(g_jsonFileName);
    return Result::FileError;
  }

  if (emitEvents) {
    emitProgress(sequence, Command::DownloadSchemaJson, "downloading", bytesWritten, bytesWritten);
    emitFileReady(sequence, Command::DownloadSchemaJson, g_jsonFileName, static_cast<uint32_t>(bytesWritten));
  }

  return Result::Ok;
}

Result ensureSchemaAvailable(bool forceRedownload, uint32_t sequence, bool emitEvents) {
  if (!g_driverInstalled) {
    return Result::CommError;
  }

  uint32_t serial[4] = {};
  const Result serialResult = obtainSerialSynchronously(serial);
  if (serialResult != Result::Ok) {
    return serialResult;
  }

  memcpy(g_serial, serial, sizeof(g_serial));
  snprintf(g_jsonFileName, sizeof(g_jsonFileName), "/%" PRIx32 ".json", g_serial[3]);

  if (!forceRedownload && schemaFileExists()) {
    if (!loadParamCache()) {
      SPIFFS.remove(g_jsonFileName);
    }
    else {
      setTaskState(TaskState::Ready);
      return Result::Ok;
    }
  }

  setTaskState(TaskState::DownloadingJson);
  const Result downloadResult = downloadSchemaSynchronously(sequence, emitEvents);
  if (downloadResult == Result::Ok) {
    setTaskState(TaskState::Ready);
  }
  else {
    setTaskState(TaskState::Starting);
  }
  return downloadResult;
}

bool simpleRequestKeyMatches(const SimpleRequestEntry& entry, uint16_t index, uint8_t subIndex) {
  return entry.waitingReply && entry.index == index && entry.subIndex == subIndex;
}

Result completeSimpleRequest(SimpleRequestEntry& entry, const twai_message_t& frame) {
  Result result = Result::Ok;
  if (frame.data[0] == SDO_ABORT) {
    result = abortToResult(entry.request.command, frame);
  }

  tracef("reply seq=%" PRIu32 " idx=0x%04X sub=0x%02X cmd=0x%02X result=%u",
         entry.request.sequence,
         entry.index,
         entry.subIndex,
         frame.data[0],
         static_cast<unsigned>(result));

  if ((result == Result::Ok) && (entry.request.command == Command::GetValue)) {
    const float value = static_cast<float>(readI32LE(&frame.data[4])) / 32.0f;
    emitValueEvent(entry.request.sequence,
                   entry.request.command,
                   entry.name,
                   requestParamId(entry.index, entry.subIndex),
                   value,
                   0);
  }

  emitTerminal(entry.request, result);
  entry = {};
  updateCachedStatus();
  return result;
}

bool duplicateActiveKeyExists(uint16_t index, uint8_t subIndex) {
  for (const SimpleRequestEntry& entry : g_simpleRequests) {
    if (entry.inUse && entry.waitingReply && entry.index == index && entry.subIndex == subIndex) {
      return true;
    }
  }
  return false;
}

TickType_t defaultTimeoutForCommand(Command command) {
  switch (command) {
    case Command::GetValue:
      return DEFAULT_READ_TIMEOUT_TICKS;
    case Command::SetValue:
    case Command::RemoveCanMapping:
      return DEFAULT_WRITE_TIMEOUT_TICKS;
    case Command::SaveToFlash:
    case Command::StartStop:
      return DEFAULT_CONTROL_TIMEOUT_TICKS;
    default:
      return DEFAULT_WRITE_TIMEOUT_TICKS;
  }
}

void processIncomingCanFrame(const twai_message_t& frame) {
  if ((frame.identifier == (0x580 | g_nodeId)) && (g_taskState == TaskState::Ready)) {
    const uint16_t index = readU16LE(&frame.data[1]);
    const uint8_t subIndex = frame.data[3];

    for (SimpleRequestEntry& entry : g_simpleRequests) {
      if (!entry.inUse || !simpleRequestKeyMatches(entry, index, subIndex)) {
        continue;
      }

      const bool replyMatches = (frame.data[0] == SDO_ABORT) || (frame.data[0] == entry.expectedCommand);
      if (!replyMatches) {
        continue;
      }

      completeSimpleRequest(entry, frame);
      return;
    }
  }

  if ((frame.identifier == 0x7deU) && (g_updateState != UpdateState::Idle)) {
    switch (g_updateState) {
      case UpdateState::SendMagic:
        if (frame.data[0] == 0x33) {
          twai_message_t txFrame = {};
          txFrame.identifier = 0x7dd;
          txFrame.data_length_code = 4;
          txFrame.data[0] = frame.data[4];
          txFrame.data[1] = frame.data[5];
          txFrame.data[2] = frame.data[6];
          txFrame.data[3] = frame.data[7];
          g_updateState = UpdateState::SendSize;
          transmitFrame(txFrame, pdMS_TO_TICKS(10));
          if (frame.data[1] < 1U) {
            delay(100);
          }
        }
        break;
      case UpdateState::SendSize:
        if (frame.data[0] == 'S') {
          twai_message_t txFrame = {};
          txFrame.identifier = 0x7dd;
          txFrame.data_length_code = 1;
          txFrame.data[0] = g_updateTotalPages;
          g_updateState = UpdateState::SendPage;
          g_updateCrc = 0xFFFFFFFFUL;
          g_updateCurrentByte = 0;
          g_updateCurrentPage = 0;
          updateCachedStatus();
          transmitFrame(txFrame, pdMS_TO_TICKS(10));
        }
        break;
      case UpdateState::SendPage:
        if (frame.data[0] == 'P') {
          char buffer[8] = {};
          size_t bytesRead = 0;
          if ((g_updateFile) && (g_updateCurrentByte < g_updateFile.size())) {
            g_updateFile.seek(g_updateCurrentByte);
            bytesRead = g_updateFile.readBytes(buffer, sizeof(buffer));
          }

          while (bytesRead < sizeof(buffer)) {
            buffer[bytesRead++] = static_cast<char>(0xFF);
          }

          g_updateCurrentByte += bytesRead;
          g_updateCrc = g_updateCrc ^ readU32LE(reinterpret_cast<const uint8_t*>(&buffer[0]));
          for (int i = 0; i < 32; i++) {
            g_updateCrc = (g_updateCrc & 0x80000000UL) ? ((g_updateCrc << 1) ^ 0x04C11DB7UL) : (g_updateCrc << 1);
          }
          g_updateCrc = g_updateCrc ^ readU32LE(reinterpret_cast<const uint8_t*>(&buffer[4]));
          for (int i = 0; i < 32; i++) {
            g_updateCrc = (g_updateCrc & 0x80000000UL) ? ((g_updateCrc << 1) ^ 0x04C11DB7UL) : (g_updateCrc << 1);
          }

          twai_message_t txFrame = {};
          txFrame.identifier = 0x7dd;
          txFrame.data_length_code = 8;
          memcpy(txFrame.data, buffer, sizeof(buffer));
          transmitFrame(txFrame, pdMS_TO_TICKS(10));
        }
        else if (frame.data[0] == 'C') {
          twai_message_t txFrame = {};
          txFrame.identifier = 0x7dd;
          txFrame.data_length_code = 4;
          writeU32LE(txFrame.data, g_updateCrc);
          g_updateState = UpdateState::CheckCrc;
          transmitFrame(txFrame, pdMS_TO_TICKS(10));
        }
        break;
      case UpdateState::CheckCrc:
        g_updateCrc = 0xFFFFFFFFUL;
        if (frame.data[0] == 'P') {
          g_updateState = UpdateState::SendPage;
          g_updateCurrentPage++;
          updateCachedStatus();
        }
        else if (frame.data[0] == 'E') {
          g_updateState = UpdateState::SendPage;
          g_updateCurrentByte = g_updateCurrentPage * PAGE_SIZE_BYTES;
        }
        else if (frame.data[0] == 'D') {
          g_updateState = UpdateState::Idle;
          if (g_updateFile) {
            g_updateFile.close();
          }
          freeParamCache();
          setTaskState(TaskState::Starting);
          g_lastStartupAttempt = 0;
        }
        break;
      case UpdateState::Idle:
        break;
    }
  }
}

bool receiveAndProcessFrame(TickType_t timeoutTicks) {
  twai_message_t frame = {};
  if (receiveFrame(frame, timeoutTicks) != ESP_OK) {
    return false;
  }

  processIncomingCanFrame(frame);
  return true;
}

bool queueSimpleRequest(const Request& request) {
  SimpleRequestEntry entry = {};
  entry.inUse = true;
  entry.request = request;
  entry.queuedAt = xTaskGetTickCount();
  entry.retryAt = entry.queuedAt;
  entry.retryCount = 0;

  switch (request.command) {
    case Command::GetValue: {
      const int id = lookupParamId(request.data.getValue.name);
      if (id <= 0) {
        return false;
      }
      entry.index = paramIdToIndex(id);
      entry.subIndex = paramIdToSubIndex(id);
      entry.expectedCommand = SDO_READ_REPLY;
      copyCString(entry.name, sizeof(entry.name), request.data.getValue.name);
      break;
    }
    case Command::SetValue: {
      const int id = lookupParamId(request.data.setValue.name);
      if (id <= 0) {
        return false;
      }
      entry.index = paramIdToIndex(id);
      entry.subIndex = paramIdToSubIndex(id);
      entry.expectedCommand = SDO_WRITE_REPLY;
      entry.writeValue = static_cast<uint32_t>(request.data.setValue.value * 32.0f);
      copyCString(entry.name, sizeof(entry.name), request.data.setValue.name);
      break;
    }
    case Command::RemoveCanMapping:
      entry.index = request.data.removeMap.index;
      entry.subIndex = request.data.removeMap.subIndex;
      entry.expectedCommand = SDO_WRITE_REPLY;
      entry.writeValue = 0U;
      break;
    case Command::SaveToFlash:
      entry.index = SDO_INDEX_COMMANDS;
      entry.subIndex = SDO_CMD_SAVE;
      entry.expectedCommand = SDO_WRITE_REPLY;
      entry.writeValue = 0U;
      break;
    case Command::StartStop:
      entry.index = SDO_INDEX_COMMANDS;
      entry.subIndex = static_cast<uint8_t>((request.data.startStop.opmode == 0) ? SDO_CMD_STOP : SDO_CMD_START);
      entry.expectedCommand = SDO_WRITE_REPLY;
      entry.writeValue = static_cast<uint32_t>(request.data.startStop.opmode);
      break;
    default:
      return false;
  }

  entry.request.timeoutTicks = (entry.request.timeoutTicks == 0) ? defaultTimeoutForCommand(request.command)
                                                                 : entry.request.timeoutTicks;

  for (SimpleRequestEntry& slot : g_simpleRequests) {
    if (!slot.inUse) {
      slot = entry;
      tracef("queued seq=%" PRIu32 " cmd=%u idx=0x%04X sub=0x%02X",
             request.sequence,
             static_cast<unsigned>(request.command),
             entry.index,
             entry.subIndex);
      emitAccepted(request);
      updateCachedStatus();
      return true;
    }
  }

  return false;
}

void dispatchSimpleRequests() {
  if (!isReadyForSdoCommand()) {
    return;
  }

  size_t activeCount = 0;
  for (const SimpleRequestEntry& entry : g_simpleRequests) {
    if (entry.inUse && entry.waitingReply) {
      activeCount++;
    }
  }

  for (SimpleRequestEntry& entry : g_simpleRequests) {
    if (!entry.inUse || entry.waitingReply) {
      continue;
    }

    if (activeCount >= MAX_ACTIVE_REQUESTS) {
      break;
    }

    if (xTaskGetTickCount() < entry.retryAt) {
      continue;
    }

    if (duplicateActiveKeyExists(entry.index, entry.subIndex)) {
      continue;
    }

    twai_message_t txFrame;
    if (entry.request.command == Command::GetValue) {
      fillSdoReadFrame(txFrame, entry.index, entry.subIndex);
    }
    else {
      fillSdoWriteFrame(txFrame, entry.index, entry.subIndex, entry.writeValue);
    }

    if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
      tracef("dispatch failed seq=%" PRIu32 " idx=0x%04X sub=0x%02X",
             entry.request.sequence,
             entry.index,
             entry.subIndex);
      emitTerminal(entry.request, Result::CommError);
      entry = {};
      continue;
    }

    tracef("dispatched seq=%" PRIu32 " idx=0x%04X sub=0x%02X timeout=%" PRIu32 "ms",
           entry.request.sequence,
           entry.index,
           entry.subIndex,
           pdTICKS_TO_MS(entry.request.timeoutTicks));
    entry.waitingReply = true;
    entry.sentAt = xTaskGetTickCount();
    entry.deadlineAt = entry.sentAt + entry.request.timeoutTicks;
    activeCount++;
  }

  updateCachedStatus();
}

void serviceSimpleTimeouts() {
  const TickType_t now = xTaskGetTickCount();
  for (SimpleRequestEntry& entry : g_simpleRequests) {
    if (!entry.inUse || !entry.waitingReply) {
      continue;
    }

    if (now < entry.deadlineAt) {
      continue;
    }

    entry.waitingReply = false;
    if (entry.retryCount < MAX_REQUEST_RETRIES) {
      entry.retryCount++;
      entry.retryAt = now + SIMPLE_RETRY_BACKOFF_TICKS;
      tracef("retry seq=%" PRIu32 " idx=0x%04X sub=0x%02X retry=%u",
             entry.request.sequence,
             entry.index,
             entry.subIndex,
             entry.retryCount);
      continue;
    }

    tracef("timeout seq=%" PRIu32 " idx=0x%04X sub=0x%02X",
           entry.request.sequence,
           entry.index,
           entry.subIndex);
    emitTerminal(entry.request, Result::Timeout);
    entry = {};
  }

  updateCachedStatus();
}

void serviceStartup() {
  if (g_taskState == TaskState::Ready || g_taskState == TaskState::Updating || g_updateState != UpdateState::Idle) {
    return;
  }

  if (!g_driverInstalled) {
    return;
  }

  if (requestCountActive() != 0U || requestCountQueued() != 0U) {
    return;
  }

  const TickType_t now = xTaskGetTickCount();
  if ((now - g_lastStartupAttempt) < STARTUP_RETRY_INTERVAL_TICKS) {
    return;
  }

  g_lastStartupAttempt = now;
  const Result result = ensureSchemaAvailable(false, 0, false);
  if (result != Result::Ok) {
    setTaskState(TaskState::Starting);
  }
}

Result processReadLiveSnapshot(const Request& request) {
  if (!isReadyForSdoCommand()) {
    return Result::NotReady;
  }

  if (!schemaFileExists()) {
    return Result::FileError;
  }

  File file = SPIFFS.open(g_jsonFileName, "r");
  if (!file) {
    return Result::FileError;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error != DeserializationError::Ok) {
    SPIFFS.remove(g_jsonFileName);
    freeParamCache();
    setTaskState(TaskState::Starting);
    return Result::FileError;
  }

  bool anySuccess = false;
  JsonObject root = doc.as<JsonObject>();
  for (JsonPair kv : root) {
    const int id = kv.value()["id"].as<int>();
    if (id <= 0) {
      continue;
    }

    float value = 0.0f;
    if (readValueById(id, value) == Result::Ok) {
      emitValueEvent(request.sequence,
                     request.command,
                     kv.key().c_str(),
                     static_cast<uint16_t>(id),
                     value,
                     0);
      anySuccess = true;
    }
  }

  return anySuccess ? Result::Ok : Result::CommError;
}

Result processReadCanMap(const Request& request) {
  if (!isReadyForSdoCommand()) {
    return Result::NotReady;
  }

  enum class ReqMapState : uint8_t { Start, CobId, DataPosLen, GainOfs, Done };

  ReqMapState state = ReqMapState::Start;
  uint16_t index = SDO_INDEX_MAP_RD;
  uint8_t subIndex = 0;
  int32_t cobId = 0;
  uint8_t pos = 0;
  int8_t len = 0;
  uint16_t paramId = 0;
  bool rx = false;
  twai_message_t txFrame;
  twai_message_t rxFrame;

  while (state != ReqMapState::Done) {
    switch (state) {
      case ReqMapState::Start:
        fillSdoReadFrame(txFrame, index, 0);
        if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
          return Result::CommError;
        }
        state = ReqMapState::CobId;
        cobId = 0;
        pos = 0;
        len = 0;
        paramId = 0;
        break;
      case ReqMapState::CobId:
        if (!waitForMatchingReply(rxFrame, index, 0, SDO_READ_REPLY, DEFAULT_READ_TIMEOUT_TICKS)) {
          state = ReqMapState::Done;
          break;
        }
        if (rxFrame.data[0] != SDO_ABORT) {
          cobId = readI32LE(&rxFrame.data[4]);
          subIndex = 1;
          fillSdoReadFrame(txFrame, index, subIndex);
          if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
            return Result::CommError;
          }
          state = ReqMapState::DataPosLen;
        }
        else if (!rx) {
          rx = true;
          index = SDO_INDEX_MAP_RD + 0x80;
          state = ReqMapState::Start;
        }
        else {
          state = ReqMapState::Done;
        }
        break;
      case ReqMapState::DataPosLen:
        if (!waitForMatchingReply(rxFrame, index, subIndex, SDO_READ_REPLY, DEFAULT_READ_TIMEOUT_TICKS)) {
          state = ReqMapState::Done;
          break;
        }
        if (rxFrame.data[0] != SDO_ABORT) {
          paramId = readU16LE(&rxFrame.data[4]);
          pos = rxFrame.data[6];
          len = static_cast<int8_t>(rxFrame.data[7]);
          subIndex++;
          fillSdoReadFrame(txFrame, index, subIndex);
          if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
            return Result::CommError;
          }
          state = ReqMapState::GainOfs;
        }
        else {
          index++;
          subIndex = 0;
          state = ReqMapState::Start;
        }
        break;
      case ReqMapState::GainOfs:
        if (!waitForMatchingReply(rxFrame, index, subIndex, SDO_READ_REPLY, DEFAULT_READ_TIMEOUT_TICKS)) {
          state = ReqMapState::Done;
          break;
        }
        if (rxFrame.data[0] == SDO_ABORT) {
          state = ReqMapState::Done;
          break;
        }
        else {
          int32_t gainFixedPoint = static_cast<int32_t>((readU32LE(&rxFrame.data[4]) & 0x00FFFFFFUL) << 8);
          gainFixedPoint >>= 8;
          const float gain = static_cast<float>(gainFixedPoint) / 1000.0f;
          const int8_t offset = static_cast<int8_t>(rxFrame.data[7]);
          emitMappingItemEvent(request.sequence,
                               rx,
                               static_cast<uint32_t>(cobId),
                               paramId,
                               pos,
                               len,
                               gain,
                               offset,
                               index,
                               subIndex);
          subIndex++;
          if (subIndex < 100U) {
            fillSdoReadFrame(txFrame, index, subIndex);
            if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
              return Result::CommError;
            }
            state = ReqMapState::DataPosLen;
          }
          else {
            state = ReqMapState::Done;
          }
        }
        break;
      case ReqMapState::Done:
        break;
    }
  }

  return Result::Ok;
}

void parseNamesCsv(const char* namesCsv, char* dest, size_t destSize, const char*& next, bool& done) {
  done = true;
  if ((namesCsv == nullptr) || (destSize == 0U)) {
    dest[0] = '\0';
    next = nullptr;
    return;
  }

  while (*namesCsv == ',') {
    namesCsv++;
  }

  size_t pos = 0;
  while ((namesCsv[pos] != '\0') && (namesCsv[pos] != ',') && (pos + 1U < destSize)) {
    dest[pos] = namesCsv[pos];
    pos++;
  }
  dest[pos] = '\0';

  if (namesCsv[pos] == ',') {
    next = namesCsv + pos + 1;
    done = false;
  }
  else {
    next = namesCsv + pos;
  }
}

Result processStreamValues(const Request& request) {
  if (!isReadyForSdoCommand()) {
    return Result::NotReady;
  }

  for (uint16_t sample = 0; sample < request.data.streamValues.samples; sample++) {
    const char* cursor = request.data.streamValues.namesCsv;
    bool done = false;
    while (!done) {
      char name[kNameLength];
      const char* next = nullptr;
      parseNamesCsv(cursor, name, sizeof(name), next, done);
      cursor = next;

      if (name[0] == '\0') {
        continue;
      }

      const int id = lookupParamId(name);
      float value = 0.0f;
      if (id > 0) {
        const Result valueResult = readValueById(id, value);
        if (valueResult != Result::Ok) {
          value = 0.0f;
        }
      }

      emitValueEvent(request.sequence,
                     request.command,
                     name,
                     (id > 0) ? static_cast<uint16_t>(id) : 0U,
                     value,
                     sample);
    }
  }

  return Result::Ok;
}

Result processAddCanMapping(const Request& request) {
  if (!isReadyForSdoCommand()) {
    return Result::NotReady;
  }

  JsonDocument doc;
  if (deserializeJson(doc, request.data.mapJson.json) != DeserializationError::Ok) {
    return Result::InvalidRequest;
  }

  if (doc["isrx"].isNull() || doc["id"].isNull() || doc["paramid"].isNull() || doc["position"].isNull() ||
      doc["length"].isNull() || doc["gain"].isNull() || doc["offset"].isNull()) {
    return Result::InvalidRequest;
  }

  const uint16_t index = doc["isrx"].as<bool>() ? SDO_INDEX_MAP_RX : SDO_INDEX_MAP_TX;
  Result result = writeValueAndWait(index, 0, doc["id"].as<uint32_t>(), DEFAULT_WRITE_TIMEOUT_TICKS);
  if (result != Result::Ok) {
    return result;
  }

  result = writeValueAndWait(index,
                             1,
                             doc["paramid"].as<uint32_t>() | (doc["position"].as<uint32_t>() << 16) |
                                 (static_cast<uint32_t>(doc["length"].as<int32_t>()) << 24),
                             DEFAULT_WRITE_TIMEOUT_TICKS);
  if (result != Result::Ok) {
    return result;
  }

  const int32_t gainValue = static_cast<int32_t>(doc["gain"].as<double>() * 1000.0);
  const uint32_t packedGain = static_cast<uint32_t>(gainValue) & 0x00FFFFFFUL;
  const uint32_t packedOffset = static_cast<uint32_t>(doc["offset"].as<int32_t>()) << 24;
  return writeValueAndWait(index, 2, packedGain | packedOffset, DEFAULT_WRITE_TIMEOUT_TICKS);
}

Result processRemoveCanMapping(const Request& request) {
  if (!isReadyForSdoCommand()) {
    return Result::NotReady;
  }

  return writeValueAndWait(request.data.removeMap.index, request.data.removeMap.subIndex, 0U, DEFAULT_WRITE_TIMEOUT_TICKS);
}

Result processDownloadSchemaJson(const Request& request) {
  if (!g_driverInstalled) {
    return Result::CommError;
  }

  if (request.data.downloadSchemaJson.forceRedownload && schemaFileExists()) {
    SPIFFS.remove(g_jsonFileName);
    freeParamCache();
  }

  return ensureSchemaAvailable(request.data.downloadSchemaJson.forceRedownload, request.sequence, true);
}

Result processReconfigure(const Request& request) {
  tracef("reconfigure node=%u baud=%u tx=%d rx=%d",
         request.data.reconfigure.nodeId,
         request.data.reconfigure.baudRate,
         request.data.reconfigure.txPin,
         request.data.reconfigure.rxPin);
  cancelSimpleRequests(Result::Busy);
  freeParamCache();
  g_hasPendingComplex = false;
  if (g_updateFile) {
    g_updateFile.close();
  }
  g_updateState = UpdateState::Idle;
  g_updateCurrentByte = 0;
  g_updateCurrentPage = 0;
  g_updateTotalPages = 0;

  const bool installed =
      installDriver(request.data.reconfigure.nodeId, request.data.reconfigure.baudRate, request.data.reconfigure.txPin, request.data.reconfigure.rxPin);
  if (!installed) {
    setTaskState(TaskState::Error);
    return Result::CommError;
  }

  g_nodeId = request.data.reconfigure.nodeId;
  g_baudRate = request.data.reconfigure.baudRate;
  g_txPin = request.data.reconfigure.txPin;
  g_rxPin = request.data.reconfigure.rxPin;
  memset(g_serial, 0, sizeof(g_serial));
  g_jsonFileName[0] = '\0';
  g_lastStartupAttempt = 0;
  setTaskState(TaskState::Starting);
  return Result::Ok;
}

Result processStartFirmwareUpdate(const Request& request) {
  if (!isReadyForSdoCommand()) {
    return Result::NotReady;
  }

  if (g_updateState != UpdateState::Idle) {
    return Result::Busy;
  }

  g_updateFile = SPIFFS.open(request.data.startFirmwareUpdate.fileName, "r");
  if (!g_updateFile) {
    return Result::FileError;
  }

  g_updateTotalPages = static_cast<uint8_t>((g_updateFile.size() + PAGE_SIZE_BYTES - 1U) / PAGE_SIZE_BYTES);
  g_updateCurrentPage = 0;
  g_updateCurrentByte = 0;
  g_updateCrc = 0xFFFFFFFFUL;

  twai_message_t txFrame;
  fillSdoWriteFrame(txFrame, SDO_INDEX_COMMANDS, SDO_CMD_RESET, 1U);
  if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
    g_updateFile.close();
    return Result::CommError;
  }

  g_updateState = UpdateState::SendMagic;
  setTaskState(TaskState::Updating);
  updateCachedStatus();
  return Result::Ok;
}

Result processComplexRequest(const Request& request) {
  tracef("complex start seq=%" PRIu32 " cmd=%u", request.sequence, static_cast<unsigned>(request.command));
  switch (request.command) {
    case Command::Reconfigure:
      return processReconfigure(request);
    case Command::GetStatus:
    case Command::GetUpdateStatus:
      emitStatusResponse(request);
      return Result::Ok;
    case Command::DownloadSchemaJson:
      return processDownloadSchemaJson(request);
    case Command::ReadLiveSnapshot:
      return processReadLiveSnapshot(request);
    case Command::ReadCanMap:
      return processReadCanMap(request);
    case Command::StreamValues:
      return processStreamValues(request);
    case Command::AddCanMapping:
      return processAddCanMapping(request);
    case Command::RemoveCanMapping:
      return processRemoveCanMapping(request);
    case Command::StartFirmwareUpdate:
      return processStartFirmwareUpdate(request);
    default:
      return Result::InvalidRequest;
  }
}

void acceptRequest(const Request& request) {
  if (isSimpleScheduledCommand(request.command)) {
    if (!queueSimpleRequest(request)) {
      tracef("queue full seq=%" PRIu32 " cmd=%u", request.sequence, static_cast<unsigned>(request.command));
      emitTerminal(request, Result::Busy);
    }
    return;
  }

  if (g_hasPendingComplex || (requestCountQueued() != 0U) || (requestCountActive() != 0U)) {
    g_pendingComplexRequest = request;
    g_hasPendingComplex = true;
    tracef("complex pending seq=%" PRIu32 " cmd=%u", request.sequence, static_cast<unsigned>(request.command));
    emitAccepted(request);
    updateCachedStatus();
    return;
  }

  emitAccepted(request);
  const Result result = processComplexRequest(request);
  if ((request.command != Command::GetStatus) && (request.command != Command::GetUpdateStatus)) {
    emitTerminal(request, result);
  }
}

void taskMain(void*) {
  setTaskState(TaskState::Stopped);
  updateCachedStatus();

  for (;;) {
    bool didWork = false;

    if (!g_hasPendingComplex) {
      Request request = {};
      while (xQueueReceive(g_requestQueue, &request, 0) == pdTRUE) {
        acceptRequest(request);
        didWork = true;

        if (g_hasPendingComplex) {
          break;
        }
      }
    }

    if (requestCountActive() != 0U || g_updateState != UpdateState::Idle) {
      didWork = receiveAndProcessFrame(0) || didWork;
    }

    serviceSimpleTimeouts();
    dispatchSimpleRequests();

    if (g_hasPendingComplex && (requestCountQueued() == 1U) && (requestCountActive() == 0U)) {
      const Request request = g_pendingComplexRequest;
      g_hasPendingComplex = false;
      updateCachedStatus();
      const Result result = processComplexRequest(request);
      if ((request.command != Command::GetStatus) && (request.command != Command::GetUpdateStatus)) {
        emitTerminal(request, result);
      }
      didWork = true;
    }

    serviceStartup();

    if (!didWork) {
      Request request = {};
      if (xQueueReceive(g_requestQueue, &request, pdMS_TO_TICKS(TASK_IDLE_WAIT_MS)) == pdTRUE) {
        acceptRequest(request);
        continue;
      }

      receiveAndProcessFrame(0);
    }
  }
}

} // namespace

uint32_t AllocateSequence() {
  portENTER_CRITICAL(&g_stateLock);
  const uint32_t sequence = g_nextSequence++;
  portEXIT_CRITICAL(&g_stateLock);
  return sequence;
}

bool StartTask() {
  if (g_taskHandle != nullptr) {
    return true;
  }

  g_requestQueue = xQueueCreate(REQUEST_QUEUE_LENGTH, sizeof(Request));
  g_responseQueue = xQueueCreate(RESPONSE_QUEUE_LENGTH, sizeof(Response));
  g_apiMutex = xSemaphoreCreateMutex();

  if ((g_requestQueue == nullptr) || (g_responseQueue == nullptr) || (g_apiMutex == nullptr)) {
    return false;
  }

  if (xTaskCreate(taskMain, "oi_can_task", 12288, nullptr, 2, &g_taskHandle) != pdPASS) {
    g_taskHandle = nullptr;
    return false;
  }

  return true;
}

bool LockApi(TickType_t timeoutTicks) {
  if (!StartTask()) {
    return false;
  }

  return xSemaphoreTake(g_apiMutex, timeoutTicks) == pdTRUE;
}

void UnlockApi() {
  if (g_apiMutex != nullptr) {
    xSemaphoreGive(g_apiMutex);
  }
}

bool Submit(const Request& request, TickType_t sendTimeoutTicks) {
  if ((g_requestQueue == nullptr) || (request.sequence == 0U)) {
    return false;
  }

  return xQueueSend(g_requestQueue, &request, sendTimeoutTicks) == pdTRUE;
}

bool Receive(Response& response, TickType_t waitTicks) {
  if (g_responseQueue == nullptr) {
    return false;
  }

  return xQueueReceive(g_responseQueue, &response, waitTicks) == pdTRUE;
}

Result SubmitAndDrain(Request request,
                      ResponseCallback callback,
                      void* context,
                      TickType_t sendTimeoutTicks) {
  if (!StartTask()) {
    return Result::Busy;
  }

  if (!LockApi(sendTimeoutTicks)) {
    return Result::Busy;
  }

  if (request.sequence == 0U) {
    request.sequence = AllocateSequence();
  }

  if (request.timeoutTicks == 0) {
    request.timeoutTicks = DEFAULT_CONTROL_TIMEOUT_TICKS;
  }

  Result finalResult = Result::Timeout;
  bool callbackActive = true;

  if (!Submit(request, sendTimeoutTicks)) {
    UnlockApi();
    return Result::Busy;
  }

  const TickType_t overallWaitTicks = request.timeoutTicks + pdMS_TO_TICKS(1000);
  const TickType_t startedAt = xTaskGetTickCount();

  while ((xTaskGetTickCount() - startedAt) < overallWaitTicks) {
    Response response = {};
    if (!Receive(response, pdMS_TO_TICKS(50))) {
      continue;
    }

    if (response.sequence != request.sequence) {
      continue;
    }

    if (callbackActive && (callback != nullptr)) {
      callbackActive = callback(response, context);
    }

    if (response.terminal) {
      finalResult = response.result;
      break;
    }
  }

  UnlockApi();
  return finalResult;
}

void SetTraceEnabled(bool enabled) {
  g_traceEnabled = enabled;
  tracef("trace=%s", enabled ? "on" : "off");
}

bool GetTraceEnabled() {
  return g_traceEnabled;
}

bool Reconfigure(uint8_t nodeId, uint8_t baudRate, int txPin, int rxPin) {
  Request request = {};
  request.command = Command::Reconfigure;
  request.timeoutTicks = pdMS_TO_TICKS(500);
  request.data.reconfigure.nodeId = nodeId;
  request.data.reconfigure.baudRate = baudRate;
  request.data.reconfigure.txPin = txPin;
  request.data.reconfigure.rxPin = rxPin;
  return SubmitAndDrain(request, nullptr, nullptr) == Result::Ok;
}

Result GetStatus(StatusPayload& status) {
  Request request = {};
  request.command = Command::GetStatus;
  request.timeoutTicks = pdMS_TO_TICKS(200);

  auto callback = [](const Response& response, void* context) -> bool {
    if ((response.kind == ResponseKind::Completed) && (response.result == Result::Ok)) {
      *static_cast<StatusPayload*>(context) = response.data.status;
    }
    return true;
  };

  return SubmitAndDrain(request, callback, &status);
}

bool GetCachedStatus(StatusPayload& status) {
  portENTER_CRITICAL(&g_stateLock);
  status = g_cachedStatus;
  portEXIT_CRITICAL(&g_stateLock);
  return true;
}

bool GetSchemaFileName(char* path, size_t pathSize) {
  if ((path == nullptr) || (pathSize == 0U)) {
    return false;
  }

  portENTER_CRITICAL(&g_stateLock);
  copyCString(path, pathSize, g_jsonFileName);
  portEXIT_CRITICAL(&g_stateLock);
  return path[0] != '\0';
}

uint8_t GetNodeId() {
  return g_nodeId;
}

uint8_t GetBaudRate() {
  return g_baudRate;
}

int GetCurrentUpdatePage() {
  return static_cast<int>(g_updateCurrentPage);
}

int GetUpdateTotalPages() {
  return static_cast<int>(g_updateTotalPages);
}

} // namespace OICanTask
