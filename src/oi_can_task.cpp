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
#include <algorithm>
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
constexpr size_t DEFAULT_REQUEST_QUEUE_LENGTH = 24;
constexpr size_t DEFAULT_RESPONSE_QUEUE_LENGTH = 96;
constexpr size_t DEFAULT_TWAI_TX_QUEUE_LENGTH = 30;
constexpr size_t DEFAULT_TWAI_RX_QUEUE_LENGTH = 30;
constexpr size_t MAX_SIMPLE_REQUESTS = 20;
constexpr size_t DEFAULT_MAX_ACTIVE_REQUESTS = 4;
constexpr uint16_t DEFAULT_TASK_STACK_WORDS = 12288;
constexpr uint8_t DEFAULT_TASK_PRIORITY = 2;
constexpr size_t SCAN_MAX_INFLIGHT = 8;
constexpr TickType_t DEFAULT_READ_TIMEOUT_TICKS = pdMS_TO_TICKS(40);
constexpr TickType_t DEFAULT_WRITE_TIMEOUT_TICKS = pdMS_TO_TICKS(150);
constexpr TickType_t DEFAULT_CONTROL_TIMEOUT_TICKS = pdMS_TO_TICKS(200);
constexpr TickType_t SCAN_PROBE_TIMEOUT_TICKS = pdMS_TO_TICKS(20);
constexpr TickType_t SCAN_SERIAL_TIMEOUT_TICKS = pdMS_TO_TICKS(25);
constexpr TickType_t STARTUP_RETRY_INTERVAL_TICKS = pdMS_TO_TICKS(1000);
constexpr TickType_t SIMPLE_RETRY_BACKOFF_TICKS = pdMS_TO_TICKS(15);
constexpr uint8_t MAX_REQUEST_RETRIES = 1;
constexpr uint32_t MIN_INTERFRAME_GAP_US = 1500;
constexpr uint32_t TASK_IDLE_WAIT_MS = 5;
constexpr TickType_t STATS_REFRESH_INTERVAL_TICKS = pdMS_TO_TICKS(250);
constexpr uint32_t DOWNLOAD_PROGRESS_STEP = 100;
constexpr uint8_t SCAN_PROGRESS_STEP_NODES = 8;

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

struct ScanProbeEntry {
  bool inUse;
  uint8_t nodeId;
  TickType_t sentAt;
};

struct RuntimeCounters {
  uint32_t canFramesTx;
  uint32_t canFramesRx;
  uint32_t canBytesTx;
  uint32_t canBytesRx;
  uint32_t canRepliesReceived;
  uint32_t requestsAccepted;
  uint32_t requestsCompleted;
  uint32_t requestsFailed;
  uint32_t requestTimeouts;
  uint32_t responsesEmitted;
};

QueueHandle_t g_requestQueue = nullptr;
QueueHandle_t g_responseQueue = nullptr;
SemaphoreHandle_t g_apiMutex = nullptr;
SemaphoreHandle_t g_taskStoppedSemaphore = nullptr;
TaskHandle_t g_taskHandle = nullptr;
portMUX_TYPE g_stateLock = portMUX_INITIALIZER_UNLOCKED;

TaskState g_taskState = TaskState::Stopped;
StatusPayload g_cachedStatus = {};
sdo_task_config g_taskConfig = {};
sdo_task_stats g_cachedTaskStats = {};
uint32_t g_nextSequence = 1;
bool g_stopRequested = false;
TickType_t g_taskStartedAt = 0;
TickType_t g_lastStatsRefreshAt = 0;
TickType_t g_busLoadWindowStartedAt = 0;
uint32_t g_busLoadWindowBits = 0;
float g_currentBusLoadKbps = 0.0f;
float g_currentBusLoadPercent = 0.0f;
RuntimeCounters g_runtimeCounters = {};
twai_status_info_t g_lastTwaiStatus = {};

uint8_t g_nodeId = 1;
uint8_t g_baudRate = 2;
int g_txPin = -1;
int g_rxPin = -1;
bool g_driverInstalled = false;
bool g_traceEnabled = false;
TickType_t g_lastStartupAttempt = 0;
uint32_t g_serial[4] = {};
char g_jsonFileName[kPathLength] = "";
uint8_t g_activeRequestLimit = static_cast<uint8_t>(DEFAULT_MAX_ACTIVE_REQUESTS);

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

sdo_task_config defaultTaskConfig() {
  sdo_task_config config;
  config.nodeId = 1U;
  config.baudRate = 2U;
  config.txPin = -1;
  config.rxPin = -1;
  config.requestQueueLength = DEFAULT_REQUEST_QUEUE_LENGTH;
  config.responseQueueLength = DEFAULT_RESPONSE_QUEUE_LENGTH;
  config.twaiTxQueueLength = DEFAULT_TWAI_TX_QUEUE_LENGTH;
  config.twaiRxQueueLength = DEFAULT_TWAI_RX_QUEUE_LENGTH;
  config.taskStackWords = DEFAULT_TASK_STACK_WORDS;
  config.taskPriority = DEFAULT_TASK_PRIORITY;
  config.maxActiveRequests = DEFAULT_MAX_ACTIVE_REQUESTS;
  return config;
}

uint16_t sanitizeQueueLength(uint16_t value, uint16_t fallbackValue) {
  return value == 0U ? fallbackValue : value;
}

uint8_t sanitizeMaxActiveRequests(uint8_t value) {
  if (value == 0U) {
    return static_cast<uint8_t>(DEFAULT_MAX_ACTIVE_REQUESTS);
  }

  if (value > MAX_SIMPLE_REQUESTS) {
    return static_cast<uint8_t>(MAX_SIMPLE_REQUESTS);
  }

  return value;
}

CanBusState mapTwaiState(twai_state_t state) {
  switch (state) {
    case TWAI_STATE_STOPPED:
      return CanBusState::Stopped;
    case TWAI_STATE_RUNNING:
      return CanBusState::Running;
    case TWAI_STATE_BUS_OFF:
      return CanBusState::BusOff;
    case TWAI_STATE_RECOVERING:
      return CanBusState::Recovering;
    default:
      return CanBusState::Unknown;
  }
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

bool isValidNodeId(uint8_t nodeId) {
  return nodeId >= 1U && nodeId <= 127U;
}

uint32_t sdoRequestId(uint8_t nodeId) {
  return 0x600U | nodeId;
}

uint32_t sdoReplyId(uint8_t nodeId) {
  return 0x580U | nodeId;
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

uint32_t estimateFrameBits(const twai_message_t& frame) {
  const uint32_t dataBits = static_cast<uint32_t>(frame.rtr ? 0U : frame.data_length_code * 8U);
  const uint32_t baseBits = frame.extd ? 67U : 47U;
  const uint32_t crcAckIntermission = 13U;
  return baseBits + dataBits + crcAckIntermission;
}

void freeParamCache();
bool schemaFileExists();

void refreshBusLoadSample(TickType_t now) {
  if (g_busLoadWindowStartedAt == 0) {
    g_busLoadWindowStartedAt = now;
  }

  const TickType_t elapsedTicks = now - g_busLoadWindowStartedAt;
  if (elapsedTicks < pdMS_TO_TICKS(1000)) {
    return;
  }

  const float elapsedSeconds = static_cast<float>(pdTICKS_TO_MS(elapsedTicks)) / 1000.0f;
  if (elapsedSeconds <= 0.0f) {
    return;
  }

  g_currentBusLoadKbps = static_cast<float>(g_busLoadWindowBits) / elapsedSeconds / 1000.0f;
  const uint32_t baudRateBps = baudRateToBitsPerSecond(g_baudRate);
  g_currentBusLoadPercent =
      (baudRateBps == 0U) ? 0.0f : ((g_currentBusLoadKbps * 1000.0f) * 100.0f / static_cast<float>(baudRateBps));
  g_busLoadWindowBits = 0U;
  g_busLoadWindowStartedAt = now;
}

void noteCanTraffic(const twai_message_t& frame, bool transmitted) {
  const uint32_t bytes = frame.rtr ? 0U : static_cast<uint32_t>(frame.data_length_code);
  const uint32_t bits = estimateFrameBits(frame);

  if (transmitted) {
    g_runtimeCounters.canFramesTx++;
    g_runtimeCounters.canBytesTx += bytes;
  }
  else {
    g_runtimeCounters.canFramesRx++;
    g_runtimeCounters.canBytesRx += bytes;
  }

  g_busLoadWindowBits += bits;
  refreshBusLoadSample(xTaskGetTickCount());
}

void resetRuntimeState() {
  memset(&g_runtimeCounters, 0, sizeof(g_runtimeCounters));
  memset(&g_lastTwaiStatus, 0, sizeof(g_lastTwaiStatus));
  memset(g_simpleRequests, 0, sizeof(g_simpleRequests));
  g_hasPendingComplex = false;
  g_pendingComplexRequest = {};
  g_lastTxMicros = 0U;
  g_taskStartedAt = xTaskGetTickCount();
  g_lastStatsRefreshAt = 0;
  g_busLoadWindowStartedAt = g_taskStartedAt;
  g_busLoadWindowBits = 0U;
  g_currentBusLoadKbps = 0.0f;
  g_currentBusLoadPercent = 0.0f;
  g_lastStartupAttempt = 0;
  memset(g_serial, 0, sizeof(g_serial));
  g_jsonFileName[0] = '\0';
  g_updateState = UpdateState::Idle;
  g_updateCurrentByte = 0U;
  g_updateCurrentPage = 0U;
  g_updateTotalPages = 0U;
  g_updateCrc = 0xFFFFFFFFUL;
  if (g_updateFile) {
    g_updateFile.close();
  }
  freeParamCache();
}

void updateTaskStatsSnapshot() {
  refreshBusLoadSample(xTaskGetTickCount());

  twai_status_info_t twaiStatus = {};
  if (g_driverInstalled) {
    twai_get_status_info(&twaiStatus);
    g_lastTwaiStatus = twaiStatus;
  }
  else {
    memset(&g_lastTwaiStatus, 0, sizeof(g_lastTwaiStatus));
  }

  sdo_task_stats stats = {};
  stats.taskRunning = (g_taskHandle != nullptr);
  stats.driverInstalled = g_driverInstalled;
  stats.traceEnabled = g_traceEnabled;
  stats.schemaAvailable = schemaFileExists();
  stats.errorPassive = (g_lastTwaiStatus.tx_error_counter >= 128U) || (g_lastTwaiStatus.rx_error_counter >= 128U);
  stats.taskState = g_taskState;
  stats.canState = g_driverInstalled ? mapTwaiState(g_lastTwaiStatus.state) : CanBusState::Stopped;
  stats.nodeId = g_nodeId;
  stats.baudRate = g_baudRate;
  stats.txPin = g_txPin;
  stats.rxPin = g_rxPin;
  stats.maxActiveRequests = g_activeRequestLimit;
  stats.requestQueueUsed =
      (g_requestQueue != nullptr) ? static_cast<uint16_t>(uxQueueMessagesWaiting(g_requestQueue)) : 0U;
  stats.requestQueueCapacity = g_taskConfig.requestQueueLength;
  stats.responseQueueUsed =
      (g_responseQueue != nullptr) ? static_cast<uint16_t>(uxQueueMessagesWaiting(g_responseQueue)) : 0U;
  stats.responseQueueCapacity = g_taskConfig.responseQueueLength;
  stats.queuedRequests = requestCountQueued();
  stats.activeRequests = requestCountActive();
  stats.uptimeMs = (g_taskStartedAt == 0) ? 0U : static_cast<uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount() - g_taskStartedAt));
  stats.canFramesTx = g_runtimeCounters.canFramesTx;
  stats.canFramesRx = g_runtimeCounters.canFramesRx;
  stats.canBytesTx = g_runtimeCounters.canBytesTx;
  stats.canBytesRx = g_runtimeCounters.canBytesRx;
  stats.canRepliesReceived = g_runtimeCounters.canRepliesReceived;
  stats.currentBusLoadKbps = g_currentBusLoadKbps;
  stats.currentBusLoadPercent = g_currentBusLoadPercent;
  stats.requestsAccepted = g_runtimeCounters.requestsAccepted;
  stats.requestsCompleted = g_runtimeCounters.requestsCompleted;
  stats.requestsFailed = g_runtimeCounters.requestsFailed;
  stats.requestTimeouts = g_runtimeCounters.requestTimeouts;
  stats.responsesEmitted = g_runtimeCounters.responsesEmitted;
  stats.txErrorCounter = g_lastTwaiStatus.tx_error_counter;
  stats.rxErrorCounter = g_lastTwaiStatus.rx_error_counter;
  stats.msgsToTx = g_lastTwaiStatus.msgs_to_tx;
  stats.msgsToRx = g_lastTwaiStatus.msgs_to_rx;
  stats.txFailedCount = g_lastTwaiStatus.tx_failed_count;
  stats.rxMissedCount = g_lastTwaiStatus.rx_missed_count;
  stats.rxOverrunCount = g_lastTwaiStatus.rx_overrun_count;
  stats.arbLostCount = g_lastTwaiStatus.arb_lost_count;
  stats.busErrorCount = g_lastTwaiStatus.bus_error_count;

  portENTER_CRITICAL(&g_stateLock);
  g_cachedTaskStats = stats;
  portEXIT_CRITICAL(&g_stateLock);
}

void maybeRefreshTaskStats() {
  const TickType_t now = xTaskGetTickCount();
  if ((now - g_lastStatsRefreshAt) < STATS_REFRESH_INTERVAL_TICKS) {
    return;
  }

  g_lastStatsRefreshAt = now;
  updateTaskStatsSnapshot();
}

bool schemaFileExists() {
  return g_jsonFileName[0] != '\0' && SPIFFS.exists(g_jsonFileName);
}

bool schemaFileExistsAtPath(const char* path) {
  return (path != nullptr) && (path[0] != '\0') && SPIFFS.exists(path);
}

uint32_t fileSizeAtPath(const char* path) {
  if (!schemaFileExistsAtPath(path)) {
    return 0U;
  }

  File file = SPIFFS.open(path, "r");
  if (!file) {
    return 0U;
  }

  const uint32_t size = static_cast<uint32_t>(file.size());
  file.close();
  return size;
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
  status.maxActiveRequests = g_activeRequestLimit;

  portENTER_CRITICAL(&g_stateLock);
  g_cachedStatus = status;
  portEXIT_CRITICAL(&g_stateLock);

  updateTaskStatsSnapshot();
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
    noteCanTraffic(frame, true);
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
  if (result == ESP_OK) {
    noteCanTraffic(frame, false);
  }

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
  frame.identifier = sdoRequestId(g_nodeId);
  frame.data_length_code = 8;
  frame.data[0] = SDO_READ;
  frame.data[1] = static_cast<uint8_t>(index & 0xFFU);
  frame.data[2] = static_cast<uint8_t>((index >> 8) & 0xFFU);
  frame.data[3] = subIndex;
}

void fillSdoReadFrameForNode(twai_message_t& frame, uint16_t index, uint8_t subIndex, uint8_t nodeId) {
  memset(&frame, 0, sizeof(frame));
  frame.extd = false;
  frame.identifier = sdoRequestId(nodeId);
  frame.data_length_code = 8;
  frame.data[0] = SDO_READ;
  frame.data[1] = static_cast<uint8_t>(index & 0xFFU);
  frame.data[2] = static_cast<uint8_t>((index >> 8) & 0xFFU);
  frame.data[3] = subIndex;
}

void fillSdoWriteFrame(twai_message_t& frame, uint16_t index, uint8_t subIndex, uint32_t value) {
  memset(&frame, 0, sizeof(frame));
  frame.extd = false;
  frame.identifier = sdoRequestId(g_nodeId);
  frame.data_length_code = 8;
  frame.data[0] = SDO_WRITE;
  frame.data[1] = static_cast<uint8_t>(index & 0xFFU);
  frame.data[2] = static_cast<uint8_t>((index >> 8) & 0xFFU);
  frame.data[3] = subIndex;
  writeU32LE(&frame.data[4], value);
}

void requestNextSegmentForNode(bool toggleBit, uint8_t nodeId) {
  twai_message_t frame = {};
  frame.extd = false;
  frame.identifier = sdoRequestId(nodeId);
  frame.data_length_code = 8;
  frame.data[0] = static_cast<uint8_t>(SDO_REQUEST_SEGMENT | (toggleBit ? SDO_TOGGLE_BIT : 0U));
  transmitFrame(frame, pdMS_TO_TICKS(10));
}

bool isExpectedSdoReply(const twai_message_t& frame, uint16_t index, uint8_t subIndex, uint8_t expectedCommand) {
  if (frame.identifier != sdoReplyId(g_nodeId)) {
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

bool isExpectedSdoReplyForNode(const twai_message_t& frame,
                               uint16_t index,
                               uint8_t subIndex,
                               uint8_t expectedCommand,
                               uint8_t nodeId) {
  if (frame.identifier != sdoReplyId(nodeId)) {
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
    g_runtimeCounters.responsesEmitted++;
  }
}

void emitAccepted(const Request& request) {
  tracef("accepted seq=%" PRIu32 " cmd=%u", request.sequence, static_cast<unsigned>(request.command));
  g_runtimeCounters.requestsAccepted++;
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
  if (result == Result::Ok) {
    g_runtimeCounters.requestsCompleted++;
  }
  else {
    g_runtimeCounters.requestsFailed++;
    if (result == Result::Timeout) {
      g_runtimeCounters.requestTimeouts++;
    }
  }
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

void emitSerialEvent(uint32_t sequence, Command command, uint8_t nodeId, const uint32_t serialWords[4]) {
  Response response = {};
  response.sequence = sequence;
  response.command = command;
  response.kind = ResponseKind::Serial;
  response.result = Result::Ok;
  response.terminal = false;
  response.data.serial.nodeId = nodeId;
  memcpy(response.data.serial.words, serialWords, sizeof(response.data.serial.words));
  emitResponse(response);
}

void emitValueEvent(uint32_t sequence,
                    Command command,
                    uint8_t nodeId,
                    const char* name,
                    uint16_t paramId,
                    float value,
                    uint16_t sampleIndex) {
  tracef("value seq=%" PRIu32 " cmd=%u node=%u name=%s param=0x%04X value=%.3f sample=%u",
         sequence,
         static_cast<unsigned>(command),
         nodeId,
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
  response.data.value.nodeId = nodeId;
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

Result readValueByIdForNode(uint8_t nodeId, int id, float& valueOut, TickType_t timeoutTicks);

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
      g_runtimeCounters.canRepliesReceived++;
      return true;
    }
  }

  return false;
}

bool waitForMatchingReplyForNode(twai_message_t& frame,
                                 uint16_t index,
                                 uint8_t subIndex,
                                 uint8_t expectedCommand,
                                 uint8_t nodeId,
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

    if (isExpectedSdoReplyForNode(frame, index, subIndex, expectedCommand, nodeId)) {
      g_runtimeCounters.canRepliesReceived++;
      return true;
    }
  }

  return false;
}

Result readValueById(int id, float& valueOut) {
  return readValueByIdForNode(g_nodeId, id, valueOut, DEFAULT_READ_TIMEOUT_TICKS);
}

Result readValueByIdForNode(uint8_t nodeId, int id, float& valueOut, TickType_t timeoutTicks) {
  if (!isValidNodeId(nodeId) || id <= 0) {
    return Result::InvalidRequest;
  }

  twai_message_t txFrame;
  twai_message_t rxFrame;
  const uint16_t index = paramIdToIndex(id);
  const uint8_t subIndex = paramIdToSubIndex(id);

  fillSdoReadFrameForNode(txFrame, index, subIndex, nodeId);
  if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
    return Result::CommError;
  }

  if (!waitForMatchingReplyForNode(rxFrame, index, subIndex, SDO_READ_REPLY, nodeId, timeoutTicks)) {
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

void shutdownDriver() {
  if (!g_driverInstalled) {
    return;
  }

  twai_stop();
  twai_driver_uninstall();
  g_driverInstalled = false;
  memset(&g_lastTwaiStatus, 0, sizeof(g_lastTwaiStatus));
}

bool installDriver(uint8_t nodeId, uint8_t baudRate, int txPin, int rxPin) {
  shutdownDriver();

  twai_general_config_t gConfig = {
      .mode = TWAI_MODE_NORMAL,
      .tx_io = static_cast<gpio_num_t>(txPin),
      .rx_io = static_cast<gpio_num_t>(rxPin),
      .clkout_io = TWAI_IO_UNUSED,
      .bus_off_io = TWAI_IO_UNUSED,
      .tx_queue_len = sanitizeQueueLength(g_taskConfig.twaiTxQueueLength, DEFAULT_TWAI_TX_QUEUE_LENGTH),
      .rx_queue_len = sanitizeQueueLength(g_taskConfig.twaiRxQueueLength, DEFAULT_TWAI_RX_QUEUE_LENGTH),
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

  // Accept all standard frames and filter target nodes in software.
  // This keeps task ownership of the bus while allowing debug queries such as
  // `getserial <nodeId>` without reinstalling the driver.
  const twai_filter_config_t fConfig = TWAI_FILTER_CONFIG_ACCEPT_ALL();

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

Result obtainSerialSynchronously(uint8_t nodeId, uint32_t serialOut[4], TickType_t timeoutTicks) {
  if (!isValidNodeId(nodeId)) {
    return Result::InvalidRequest;
  }

  twai_message_t txFrame;
  twai_message_t rxFrame;

  for (uint8_t subIndex = 0; subIndex < 4U; subIndex++) {
    fillSdoReadFrameForNode(txFrame, SDO_INDEX_SERIAL, subIndex, nodeId);
    if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
      return Result::CommError;
    }

    if (!waitForMatchingReplyForNode(rxFrame, SDO_INDEX_SERIAL, subIndex, SDO_READ_REPLY, nodeId, timeoutTicks)) {
      return Result::Timeout;
    }

    if (rxFrame.data[0] == SDO_ABORT) {
      return Result::CommError;
    }

    serialOut[subIndex] = readU32LE(&rxFrame.data[4]);
  }

  return Result::Ok;
}

Result completeSerialFromFirstWord(uint8_t nodeId, uint32_t firstWord, uint32_t serialOut[4], TickType_t timeoutTicks) {
  if (!isValidNodeId(nodeId)) {
    return Result::InvalidRequest;
  }

  serialOut[0] = firstWord;

  twai_message_t txFrame;
  twai_message_t rxFrame;
  for (uint8_t subIndex = 1; subIndex < 4U; subIndex++) {
    fillSdoReadFrameForNode(txFrame, SDO_INDEX_SERIAL, subIndex, nodeId);
    if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
      return Result::CommError;
    }

    if (!waitForMatchingReplyForNode(rxFrame, SDO_INDEX_SERIAL, subIndex, SDO_READ_REPLY, nodeId, timeoutTicks)) {
      return Result::Timeout;
    }

    if (rxFrame.data[0] == SDO_ABORT) {
      return Result::CommError;
    }

    serialOut[subIndex] = readU32LE(&rxFrame.data[4]);
  }

  return Result::Ok;
}

bool validateSchemaFileAtPath(const char* path) {
  if (!schemaFileExistsAtPath(path)) {
    return false;
  }

  File file = SPIFFS.open(path, "r");
  if (!file) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, file);
  file.close();
  return error == DeserializationError::Ok;
}

Result downloadSchemaSynchronouslyForNode(uint8_t nodeId,
                                          const char* path,
                                          uint32_t sequence,
                                          Command command,
                                          bool emitEvents,
                                          bool installCurrentCache) {
  if ((path == nullptr) || (path[0] == '\0')) {
    return Result::FileError;
  }

  File file = SPIFFS.open(path, "w+");
  if (!file) {
    return Result::FileError;
  }

  twai_message_t txFrame;
  twai_message_t rxFrame;
  fillSdoReadFrameForNode(txFrame, SDO_INDEX_STRINGS, 0, nodeId);
  if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
    file.close();
    SPIFFS.remove(path);
    return Result::CommError;
  }

  if (emitEvents) {
    emitProgress(sequence, command, "started", 0, 0);
  }

  bool toggleBit = false;
  size_t bytesWritten = 0;
  size_t lastProgress = 0;

  while (true) {
    if (receiveFrame(rxFrame, pdMS_TO_TICKS(200)) != ESP_OK) {
      file.close();
      SPIFFS.remove(path);
      return Result::Timeout;
    }

    if (rxFrame.identifier != sdoReplyId(nodeId)) {
      continue;
    }

    if (rxFrame.data[0] == SDO_ABORT) {
      file.close();
      SPIFFS.remove(path);
      return Result::CommError;
    }

    if ((rxFrame.data[0] & SDO_READ) == SDO_READ) {
      requestNextSegmentForNode(toggleBit, nodeId);
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
      requestNextSegmentForNode(toggleBit, nodeId);

      if (emitEvents && ((bytesWritten - lastProgress) >= DOWNLOAD_PROGRESS_STEP)) {
        lastProgress = bytesWritten;
        emitProgress(sequence, command, "downloading", bytesWritten, 0);
      }
    }
  }

  file.close();

  if (installCurrentCache) {
    if (!loadParamCache()) {
      SPIFFS.remove(path);
      return Result::FileError;
    }
  }
  else if (!validateSchemaFileAtPath(path)) {
    SPIFFS.remove(path);
    return Result::FileError;
  }

  if (emitEvents) {
    emitProgress(sequence, command, "downloading", bytesWritten, bytesWritten);
    emitFileReady(sequence, command, path, static_cast<uint32_t>(bytesWritten));
  }

  return Result::Ok;
}

Result ensureSchemaAvailable(bool forceRedownload, uint32_t sequence, bool emitEvents) {
  if (!g_driverInstalled) {
    return Result::CommError;
  }

  uint32_t serial[4] = {};
  const Result serialResult = obtainSerialSynchronously(g_nodeId, serial, DEFAULT_READ_TIMEOUT_TICKS);
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
  const Result downloadResult =
      downloadSchemaSynchronouslyForNode(g_nodeId, g_jsonFileName, sequence, Command::DownloadSchemaJson, emitEvents, true);
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
                   g_nodeId,
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

      g_runtimeCounters.canRepliesReceived++;
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

    if (activeCount >= g_activeRequestLimit) {
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
                     g_nodeId,
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

  const uint8_t targetNodeId =
      isValidNodeId(request.data.readCanMap.nodeId) ? request.data.readCanMap.nodeId : g_nodeId;

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
        fillSdoReadFrameForNode(txFrame, index, 0, targetNodeId);
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
        if (!waitForMatchingReplyForNode(rxFrame,
                                         index,
                                         0,
                                         SDO_READ_REPLY,
                                         targetNodeId,
                                         DEFAULT_READ_TIMEOUT_TICKS)) {
          state = ReqMapState::Done;
          break;
        }
        if (rxFrame.data[0] != SDO_ABORT) {
          cobId = readI32LE(&rxFrame.data[4]);
          subIndex = 1;
          fillSdoReadFrameForNode(txFrame, index, subIndex, targetNodeId);
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
        if (!waitForMatchingReplyForNode(rxFrame,
                                         index,
                                         subIndex,
                                         SDO_READ_REPLY,
                                         targetNodeId,
                                         DEFAULT_READ_TIMEOUT_TICKS)) {
          state = ReqMapState::Done;
          break;
        }
        if (rxFrame.data[0] != SDO_ABORT) {
          paramId = readU16LE(&rxFrame.data[4]);
          pos = rxFrame.data[6];
          len = static_cast<int8_t>(rxFrame.data[7]);
          subIndex++;
          fillSdoReadFrameForNode(txFrame, index, subIndex, targetNodeId);
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
        if (!waitForMatchingReplyForNode(rxFrame,
                                         index,
                                         subIndex,
                                         SDO_READ_REPLY,
                                         targetNodeId,
                                         DEFAULT_READ_TIMEOUT_TICKS)) {
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
            fillSdoReadFrameForNode(txFrame, index, subIndex, targetNodeId);
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

  if (request.data.streamValues.samples == 0U) {
    return Result::Ok;
  }

  if (request.data.streamValues.allValues && (g_paramCache == nullptr) && !loadParamCache()) {
    return Result::FileError;
  }

  const uint16_t sampleRateHz = request.data.streamValues.sampleRateHz;
  const TickType_t sampleIntervalTicks =
      (sampleRateHz == 0U) ? 0U : (std::max<TickType_t>(pdMS_TO_TICKS(1000U / sampleRateHz), 1U));
  TickType_t lastWakeTicks = xTaskGetTickCount();

  for (uint16_t sample = 0; sample < request.data.streamValues.samples; sample++) {
    if ((sample > 0U) && (sampleIntervalTicks > 0U)) {
      vTaskDelayUntil(&lastWakeTicks, sampleIntervalTicks);
    }
    else {
      lastWakeTicks = xTaskGetTickCount();
    }

    if (request.data.streamValues.allValues) {
      for (size_t i = 0; i < g_paramCacheCount; i++) {
        float value = 0.0f;
        const Result valueResult = readValueById(static_cast<int>(g_paramCache[i].id), value);
        if (valueResult != Result::Ok) {
          value = 0.0f;
        }

        emitValueEvent(request.sequence,
                       request.command,
                       g_nodeId,
                       g_paramCache[i].name,
                       g_paramCache[i].id,
                       value,
                       sample);
      }
      continue;
    }

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
                     g_nodeId,
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

  const uint8_t targetNodeId =
      isValidNodeId(request.data.downloadSchemaJson.nodeId) ? request.data.downloadSchemaJson.nodeId : g_nodeId;

  if (targetNodeId == g_nodeId) {
    if (request.data.downloadSchemaJson.forceRedownload && schemaFileExists()) {
      SPIFFS.remove(g_jsonFileName);
      freeParamCache();
    }

    return ensureSchemaAvailable(request.data.downloadSchemaJson.forceRedownload, request.sequence, true);
  }

  uint32_t serial[4] = {};
  const Result serialResult = obtainSerialSynchronously(targetNodeId, serial, DEFAULT_READ_TIMEOUT_TICKS);
  if (serialResult != Result::Ok) {
    return serialResult;
  }

  char targetPath[kPathLength] = {};
  snprintf(targetPath, sizeof(targetPath), "/%" PRIx32 ".json", serial[3]);

  if (request.data.downloadSchemaJson.forceRedownload && schemaFileExistsAtPath(targetPath)) {
    SPIFFS.remove(targetPath);
  }

  if (!request.data.downloadSchemaJson.forceRedownload && schemaFileExistsAtPath(targetPath)) {
    emitProgress(request.sequence, Command::DownloadSchemaJson, "cached", fileSizeAtPath(targetPath), fileSizeAtPath(targetPath));
    emitFileReady(request.sequence, Command::DownloadSchemaJson, targetPath, fileSizeAtPath(targetPath));
    return Result::Ok;
  }

  tracef("download seq=%" PRIu32 " node=%u path=%s",
         request.sequence,
         targetNodeId,
         targetPath);
  return downloadSchemaSynchronouslyForNode(targetNodeId,
                                            targetPath,
                                            request.sequence,
                                            Command::DownloadSchemaJson,
                                            true,
                                            false);
}

Result processGetSerial(const Request& request) {
  if (!g_driverInstalled) {
    return Result::CommError;
  }

  if (g_updateState != UpdateState::Idle) {
    return Result::Busy;
  }

  const uint8_t targetNodeId = request.data.getSerial.nodeId;
  uint32_t serial[4] = {};
  const TickType_t timeoutTicks = (request.timeoutTicks > 0) ? request.timeoutTicks : DEFAULT_READ_TIMEOUT_TICKS;
  const Result result = obtainSerialSynchronously(targetNodeId, serial, timeoutTicks);
  if (result != Result::Ok) {
    return result;
  }

  if (targetNodeId == g_nodeId) {
    memcpy(g_serial, serial, sizeof(g_serial));
    updateCachedStatus();
  }

  tracef("serial seq=%" PRIu32 " node=%u crc=0x%08" PRIX32,
         request.sequence,
         targetNodeId,
         serial[3]);
  emitSerialEvent(request.sequence, Command::GetSerial, targetNodeId, serial);
  return Result::Ok;
}

Result processGetValueById(const Request& request) {
  if (!g_driverInstalled) {
    return Result::CommError;
  }

  if (g_updateState != UpdateState::Idle) {
    return Result::Busy;
  }

  const uint8_t targetNodeId = request.data.getValueById.nodeId;
  const uint16_t paramId = request.data.getValueById.paramId;
  const TickType_t timeoutTicks = (request.timeoutTicks > 0) ? request.timeoutTicks : DEFAULT_READ_TIMEOUT_TICKS;
  float value = 0.0f;
  const Result result = readValueByIdForNode(targetNodeId, static_cast<int>(paramId), value, timeoutTicks);
  if (result != Result::Ok) {
    return result;
  }

  tracef("direct value seq=%" PRIu32 " node=%u param=0x%04X value=%.3f",
         request.sequence,
         targetNodeId,
         paramId,
         value);
  emitValueEvent(request.sequence,
                 Command::GetValueById,
                 targetNodeId,
                 "",
                 paramId,
                 value,
                 0);
  return Result::Ok;
}

Result processScanNodes(const Request& request) {
  if (!g_driverInstalled) {
    return Result::CommError;
  }

  if (g_updateState != UpdateState::Idle) {
    return Result::Busy;
  }

  const uint8_t lastNode = request.data.scanNodes.lastNode;
  if (!isValidNodeId(lastNode)) {
    return Result::InvalidRequest;
  }

  const TickType_t probeTimeoutTicks = (request.timeoutTicks > 0) ? request.timeoutTicks : SCAN_PROBE_TIMEOUT_TICKS;
  ScanProbeEntry probes[SCAN_MAX_INFLIGHT] = {};
  uint8_t hitNodes[127] = {};
  uint32_t hitFirstWords[127] = {};
  size_t hitCount = 0;
  uint8_t nextNode = 1;
  uint8_t completed = 0;
  uint8_t lastProgressReported = 0;

  emitProgress(request.sequence, Command::ScanNodes, "probing", 0, lastNode);

  auto emitProbeProgress = [&](bool force) {
    if (force || (completed >= static_cast<uint8_t>(lastProgressReported + SCAN_PROGRESS_STEP_NODES))) {
      lastProgressReported = completed;
      emitProgress(request.sequence, Command::ScanNodes, "probing", completed, lastNode);
    }
  };

  auto dispatchProbe = [&](ScanProbeEntry& slot, uint8_t nodeId) -> Result {
    twai_message_t txFrame;
    fillSdoReadFrameForNode(txFrame, SDO_INDEX_SERIAL, 0, nodeId);
    if (transmitFrame(txFrame, pdMS_TO_TICKS(10)) != ESP_OK) {
      return Result::CommError;
    }

    slot.inUse = true;
    slot.nodeId = nodeId;
    slot.sentAt = xTaskGetTickCount();
    tracef("scan probe seq=%" PRIu32 " node=%u", request.sequence, nodeId);
    return Result::Ok;
  };

  auto retireExpiredProbes = [&]() {
    const TickType_t now = xTaskGetTickCount();
    for (ScanProbeEntry& slot : probes) {
      if (!slot.inUse) {
        continue;
      }

      if ((now - slot.sentAt) >= probeTimeoutTicks) {
        tracef("scan miss seq=%" PRIu32 " node=%u", request.sequence, slot.nodeId);
        slot = {};
        completed++;
      }
    }
  };

  while (completed < lastNode) {
    for (ScanProbeEntry& slot : probes) {
      if (slot.inUse || nextNode > lastNode) {
        continue;
      }

      const Result dispatchResult = dispatchProbe(slot, nextNode);
      if (dispatchResult != Result::Ok) {
        return dispatchResult;
      }
      nextNode++;
    }

    TickType_t waitTicks = probeTimeoutTicks;
    const TickType_t now = xTaskGetTickCount();
    for (const ScanProbeEntry& slot : probes) {
      if (!slot.inUse) {
        continue;
      }

      const TickType_t elapsed = now - slot.sentAt;
      const TickType_t remaining = (elapsed < probeTimeoutTicks) ? (probeTimeoutTicks - elapsed) : 0;
      if (remaining < waitTicks) {
        waitTicks = remaining;
      }
    }

    if (waitTicks == 0) {
      waitTicks = 1;
    }

    twai_message_t frame;
    if (receiveFrame(frame, waitTicks) == ESP_OK) {
      for (ScanProbeEntry& slot : probes) {
        if (!slot.inUse || !isExpectedSdoReplyForNode(frame, SDO_INDEX_SERIAL, 0, SDO_READ_REPLY, slot.nodeId)) {
          continue;
        }

        g_runtimeCounters.canRepliesReceived++;
        if (frame.data[0] != SDO_ABORT && hitCount < sizeof(hitNodes)) {
          hitNodes[hitCount] = slot.nodeId;
          hitFirstWords[hitCount] = readU32LE(&frame.data[4]);
          tracef("scan hit seq=%" PRIu32 " node=%u word0=0x%08" PRIX32,
                 request.sequence,
                 slot.nodeId,
                 hitFirstWords[hitCount]);
          hitCount++;
        }

        slot = {};
        completed++;
        break;
      }
    }

    retireExpiredProbes();
    emitProbeProgress(false);
  }

  emitProbeProgress(true);

  if (hitCount > 0U) {
    emitProgress(request.sequence, Command::ScanNodes, "reading", 0, static_cast<uint32_t>(hitCount));
  }

  for (size_t i = 0; i < hitCount; i++) {
    uint32_t serial[4] = {};
    const Result serialResult = completeSerialFromFirstWord(hitNodes[i], hitFirstWords[i], serial, SCAN_SERIAL_TIMEOUT_TICKS);
    if (serialResult != Result::Ok) {
      tracef("scan serial failed seq=%" PRIu32 " node=%u result=%u",
             request.sequence,
             hitNodes[i],
             static_cast<unsigned>(serialResult));
      continue;
    }

    if (hitNodes[i] == g_nodeId) {
      memcpy(g_serial, serial, sizeof(g_serial));
      updateCachedStatus();
    }

    emitSerialEvent(request.sequence, Command::ScanNodes, hitNodes[i], serial);
    emitProgress(request.sequence, Command::ScanNodes, "reading", static_cast<uint32_t>(i + 1U), static_cast<uint32_t>(hitCount));
  }

  emitProgress(request.sequence, Command::ScanNodes, "done", completed, lastNode);
  return Result::Ok;
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
  g_taskConfig.nodeId = g_nodeId;
  g_taskConfig.baudRate = g_baudRate;
  g_taskConfig.txPin = g_txPin;
  g_taskConfig.rxPin = g_rxPin;
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
    case Command::GetSerial:
      return processGetSerial(request);
    case Command::GetValueById:
      return processGetValueById(request);
    case Command::ScanNodes:
      return processScanNodes(request);
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
  resetRuntimeState();
  setTaskState(TaskState::Stopped);
  updateCachedStatus();

  while (!g_stopRequested) {
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

    maybeRefreshTaskStats();
  }

  cancelSimpleRequests(Result::NotReady);
  if (g_hasPendingComplex) {
    emitTerminal(g_pendingComplexRequest, Result::NotReady);
    g_hasPendingComplex = false;
    g_pendingComplexRequest = {};
  }

  if (g_updateFile) {
    g_updateFile.close();
  }
  g_updateState = UpdateState::Idle;
  shutdownDriver();
  setTaskState(TaskState::Stopped);
  updateCachedStatus();
  g_taskHandle = nullptr;
  if (g_taskStoppedSemaphore != nullptr) {
    xSemaphoreGive(g_taskStoppedSemaphore);
  }
  vTaskDelete(nullptr);
}

} // namespace

uint32_t AllocateSequence() {
  portENTER_CRITICAL(&g_stateLock);
  const uint32_t sequence = g_nextSequence++;
  portEXIT_CRITICAL(&g_stateLock);
  return sequence;
}

bool StartTask(const sdo_task_config& requestedConfig) {
  sdo_task_config config = requestedConfig;
  const sdo_task_config defaults = defaultTaskConfig();
  config.requestQueueLength = sanitizeQueueLength(config.requestQueueLength, defaults.requestQueueLength);
  config.responseQueueLength = sanitizeQueueLength(config.responseQueueLength, defaults.responseQueueLength);
  config.twaiTxQueueLength = sanitizeQueueLength(config.twaiTxQueueLength, defaults.twaiTxQueueLength);
  config.twaiRxQueueLength = sanitizeQueueLength(config.twaiRxQueueLength, defaults.twaiRxQueueLength);
  config.taskStackWords = sanitizeQueueLength(config.taskStackWords, defaults.taskStackWords);
  config.taskPriority = (config.taskPriority == 0U) ? defaults.taskPriority : config.taskPriority;
  config.maxActiveRequests = sanitizeMaxActiveRequests(config.maxActiveRequests);

  if (g_taskHandle != nullptr) {
    config.requestQueueLength = g_taskConfig.requestQueueLength;
    config.responseQueueLength = g_taskConfig.responseQueueLength;
    config.twaiTxQueueLength = g_taskConfig.twaiTxQueueLength;
    config.twaiRxQueueLength = g_taskConfig.twaiRxQueueLength;
    config.taskStackWords = g_taskConfig.taskStackWords;
    config.taskPriority = g_taskConfig.taskPriority;
  }

  g_taskConfig = config;
  g_activeRequestLimit = sanitizeMaxActiveRequests(g_taskConfig.maxActiveRequests);
  updateCachedStatus();

  if (!StartTask()) {
    return false;
  }

  return Reconfigure(config.nodeId, config.baudRate, config.txPin, config.rxPin);
}

bool StartTask() {
  if (g_taskHandle != nullptr) {
    return true;
  }

  if (g_taskConfig.requestQueueLength == 0U) {
    g_taskConfig = defaultTaskConfig();
  }
  g_activeRequestLimit = sanitizeMaxActiveRequests(g_taskConfig.maxActiveRequests);
  g_stopRequested = false;

  g_requestQueue = xQueueCreate(g_taskConfig.requestQueueLength, sizeof(Request));
  g_responseQueue = xQueueCreate(g_taskConfig.responseQueueLength, sizeof(Response));
  g_apiMutex = xSemaphoreCreateMutex();
  g_taskStoppedSemaphore = xSemaphoreCreateBinary();

  if ((g_requestQueue == nullptr) || (g_responseQueue == nullptr) || (g_apiMutex == nullptr) || (g_taskStoppedSemaphore == nullptr)) {
    if (g_requestQueue != nullptr) {
      vQueueDelete(g_requestQueue);
      g_requestQueue = nullptr;
    }
    if (g_responseQueue != nullptr) {
      vQueueDelete(g_responseQueue);
      g_responseQueue = nullptr;
    }
    if (g_apiMutex != nullptr) {
      vSemaphoreDelete(g_apiMutex);
      g_apiMutex = nullptr;
    }
    if (g_taskStoppedSemaphore != nullptr) {
      vSemaphoreDelete(g_taskStoppedSemaphore);
      g_taskStoppedSemaphore = nullptr;
    }
    return false;
  }

  if (xTaskCreate(taskMain,
                  "oi_can_task",
                  g_taskConfig.taskStackWords,
                  nullptr,
                  static_cast<UBaseType_t>(g_taskConfig.taskPriority),
                  &g_taskHandle) != pdPASS) {
    g_taskHandle = nullptr;
    vQueueDelete(g_requestQueue);
    vQueueDelete(g_responseQueue);
    vSemaphoreDelete(g_apiMutex);
    vSemaphoreDelete(g_taskStoppedSemaphore);
    g_requestQueue = nullptr;
    g_responseQueue = nullptr;
    g_apiMutex = nullptr;
    g_taskStoppedSemaphore = nullptr;
    return false;
  }

  return true;
}

bool StopTask(TickType_t timeoutTicks) {
  if (g_taskHandle == nullptr) {
    g_stopRequested = false;
    shutdownDriver();
    setTaskState(TaskState::Stopped);
    updateCachedStatus();
    return true;
  }

  if ((g_apiMutex != nullptr) && (xSemaphoreTake(g_apiMutex, timeoutTicks) != pdTRUE)) {
    return false;
  }

  g_stopRequested = true;
  const bool stopped =
      (g_taskStoppedSemaphore != nullptr) && (xSemaphoreTake(g_taskStoppedSemaphore, timeoutTicks) == pdTRUE);

  if (!stopped) {
    if (g_apiMutex != nullptr) {
      xSemaphoreGive(g_apiMutex);
    }
    return false;
  }

  if (g_requestQueue != nullptr) {
    vQueueDelete(g_requestQueue);
    g_requestQueue = nullptr;
  }

  if (g_responseQueue != nullptr) {
    vQueueDelete(g_responseQueue);
    g_responseQueue = nullptr;
  }

  if (g_apiMutex != nullptr) {
    vSemaphoreDelete(g_apiMutex);
    g_apiMutex = nullptr;
  }

  if (g_taskStoppedSemaphore != nullptr) {
    vSemaphoreDelete(g_taskStoppedSemaphore);
    g_taskStoppedSemaphore = nullptr;
  }

  g_stopRequested = false;
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
  if ((g_requestQueue == nullptr) || (request.sequence == 0U) || g_stopRequested) {
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

bool GetTaskConfig(sdo_task_config& config) {
  portENTER_CRITICAL(&g_stateLock);
  config = g_taskConfig;
  portEXIT_CRITICAL(&g_stateLock);
  return true;
}

bool GetTaskStats(sdo_task_stats& stats) {
  portENTER_CRITICAL(&g_stateLock);
  stats = g_cachedTaskStats;
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
