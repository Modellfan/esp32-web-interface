#include "debug_console.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include <cstdlib>
#include <cstring>

#include "oi_can_task.h"

namespace DebugConsole {

namespace {

constexpr size_t kMaxBatchRequests = 20;
constexpr uint16_t kDefaultStreamSampleCount = 2;
constexpr TickType_t kConsoleLockTimeout = pdMS_TO_TICKS(1000);
constexpr TickType_t kConsoleReceivePoll = pdMS_TO_TICKS(100);
constexpr TickType_t kConsoleBatchSlack = pdMS_TO_TICKS(2000);

String g_lineBuffer;
bool g_started = false;

struct BatchStats {
  size_t submitted = 0;
  size_t accepted = 0;
  size_t values = 0;
  size_t completed = 0;
  size_t failed = 0;
  size_t timeouts = 0;
  size_t busy = 0;
};

struct SerialCommandResult {
  bool pass = false;
  bool hasSerial = false;
  bool terminalReceived = false;
  OICanTask::Result result = OICanTask::Result::Busy;
  uint32_t words[4] = {};
};

struct ScanCommandResult {
  bool terminalReceived = false;
  OICanTask::Result result = OICanTask::Result::Busy;
  uint8_t foundNodes[127] = {};
  size_t foundCount = 0;
};

struct ValueCommandResult {
  bool terminalReceived = false;
  bool hasValue = false;
  OICanTask::Result result = OICanTask::Result::Busy;
  uint8_t nodeId = 0;
  uint16_t paramId = 0;
  float value = 0.0f;
};

struct DownloadCommandResult {
  bool terminalReceived = false;
  bool fileReady = false;
  OICanTask::Result result = OICanTask::Result::Busy;
  uint8_t nodeId = 0;
  uint32_t size = 0;
  char path[OICanTask::kPathLength] = {};
};

struct MapCommandResult {
  bool terminalReceived = false;
  OICanTask::Result result = OICanTask::Result::Busy;
  uint8_t nodeId = 0;
  size_t itemCount = 0;
};

struct StreamValuesCommandResult {
  bool terminalReceived = false;
  OICanTask::Result result = OICanTask::Result::Busy;
  uint16_t sampleRateHz = 0;
  uint16_t samplesRequested = 0;
  size_t valueCount = 0;
  uint16_t samplesSeen = 0;
};

const char* resultToString(OICanTask::Result result) {
  switch (result) {
    case OICanTask::Result::Ok:
      return "Ok";
    case OICanTask::Result::Busy:
      return "Busy";
    case OICanTask::Result::NotReady:
      return "NotReady";
    case OICanTask::Result::Timeout:
      return "Timeout";
    case OICanTask::Result::CommError:
      return "CommError";
    case OICanTask::Result::UnknownIndex:
      return "UnknownIndex";
    case OICanTask::Result::ValueOutOfRange:
      return "ValueOutOfRange";
    case OICanTask::Result::InvalidRequest:
      return "InvalidRequest";
    case OICanTask::Result::FileError:
      return "FileError";
  }

  return "Unknown";
}

const char* stateToString(OICanTask::TaskState state) {
  switch (state) {
    case OICanTask::TaskState::Stopped:
      return "Stopped";
    case OICanTask::TaskState::Starting:
      return "Starting";
    case OICanTask::TaskState::DownloadingJson:
      return "DownloadingJson";
    case OICanTask::TaskState::Ready:
      return "Ready";
    case OICanTask::TaskState::Updating:
      return "Updating";
    case OICanTask::TaskState::Error:
      return "Error";
  }

  return "Unknown";
}

const char* canStateToString(OICanTask::CanBusState state) {
  switch (state) {
    case OICanTask::CanBusState::Stopped:
      return "Stopped";
    case OICanTask::CanBusState::Running:
      return "Running";
    case OICanTask::CanBusState::BusOff:
      return "BusOff";
    case OICanTask::CanBusState::Recovering:
      return "Recovering";
    case OICanTask::CanBusState::Unknown:
      return "Unknown";
  }

  return "Unknown";
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

const char* commandToString(OICanTask::Command command) {
  switch (command) {
    case OICanTask::Command::Reconfigure:
      return "Reconfigure";
    case OICanTask::Command::GetStatus:
      return "GetStatus";
    case OICanTask::Command::GetSerial:
      return "GetSerial";
    case OICanTask::Command::ScanNodes:
      return "ScanNodes";
    case OICanTask::Command::GetValueById:
      return "GetValueById";
    case OICanTask::Command::DownloadSchemaJson:
      return "DownloadSchemaJson";
    case OICanTask::Command::ReadLiveSnapshot:
      return "ReadLiveSnapshot";
    case OICanTask::Command::ReadCanMap:
      return "ReadCanMap";
    case OICanTask::Command::StreamValues:
      return "StreamValues";
    case OICanTask::Command::GetValue:
      return "GetValue";
    case OICanTask::Command::SetValue:
      return "SetValue";
    case OICanTask::Command::AddCanMapping:
      return "AddCanMapping";
    case OICanTask::Command::RemoveCanMapping:
      return "RemoveCanMapping";
    case OICanTask::Command::SaveToFlash:
      return "SaveToFlash";
    case OICanTask::Command::StartStop:
      return "StartStop";
    case OICanTask::Command::StartFirmwareUpdate:
      return "StartFirmwareUpdate";
    case OICanTask::Command::GetUpdateStatus:
      return "GetUpdateStatus";
  }

  return "Unknown";
}

const char* responseKindToString(OICanTask::ResponseKind kind) {
  switch (kind) {
    case OICanTask::ResponseKind::Accepted:
      return "Accepted";
    case OICanTask::ResponseKind::Progress:
      return "Progress";
    case OICanTask::ResponseKind::Value:
      return "Value";
    case OICanTask::ResponseKind::MappingItem:
      return "MappingItem";
    case OICanTask::ResponseKind::FileReady:
      return "FileReady";
    case OICanTask::ResponseKind::Serial:
      return "Serial";
    case OICanTask::ResponseKind::Completed:
      return "Completed";
    case OICanTask::ResponseKind::Failed:
      return "Failed";
  }

  return "Unknown";
}

bool copyNameArg(char* dest, size_t destSize, const String& source) {
  if ((source.length() + 1U) > destSize) {
    return false;
  }

  source.toCharArray(dest, destSize);
  return true;
}

void printPrompt() {
  Serial.print("dbg> ");
}

void printHelp() {
  Serial.println("Debug console commands:");
  Serial.println("  help");
  Serial.println("  trace on|off");
  Serial.println("  status");
  Serial.println("  taskstatus");
  Serial.println("  names [count]");
  Serial.println("  getserial <nodeid>");
  Serial.println("  getvalue <nodeid> <paramid>");
  Serial.println("  getmap <nodeid>");
  Serial.println("  download <nodeid>");
  Serial.println("  scan <lastnode>");
  Serial.println("  streamvalues <hz> [samples]");
  Serial.println("  taskget <name>");
  Serial.println("  taskgetmany <name1,name2,...>");
}

void printStatus() {
  OICanTask::StatusPayload status = {};
  OICanTask::GetCachedStatus(status);
  Serial.printf("STATUS state=%s node=%u baud=%u queued=%u active=%u maxActive=%u schema=%s update=%u/%u serial=0x%08" PRIX32 "\r\n",
                stateToString(status.state),
                status.nodeId,
                status.baudRate,
                status.queuedRequests,
                status.activeRequests,
                status.maxActiveRequests,
                status.schemaAvailable ? "yes" : "no",
                status.updateCurrentPage,
                status.updateTotalPages,
                status.serialCrc);
}

void printTaskStatus() {
  OICanTask::sdo_task_config config = {};
  OICanTask::sdo_task_stats stats = {};
  OICanTask::GetTaskConfig(config);
  OICanTask::GetTaskStats(stats);

  Serial.printf("TASKSTATUS cfg running=%s driver=%s task=%s can=%s node=%u baud=%" PRIu32 " tx=%d rx=%d schema=%s trace=%s maxActive=%u qReq=%u/%u qResp=%u/%u\r\n",
                stats.taskRunning ? "yes" : "no",
                stats.driverInstalled ? "yes" : "no",
                stateToString(stats.taskState),
                canStateToString(stats.canState),
                config.nodeId,
                baudRateToBitsPerSecond(config.baudRate),
                config.txPin,
                config.rxPin,
                stats.schemaAvailable ? "yes" : "no",
                stats.traceEnabled ? "on" : "off",
                stats.maxActiveRequests,
                stats.requestQueueUsed,
                stats.requestQueueCapacity,
                stats.responseQueueUsed,
                stats.responseQueueCapacity);

  Serial.printf("TASKSTATUS load kbps=%.2f pct=%.2f uptimeMs=%" PRIu32 " canTx=%" PRIu32 " canRx=%" PRIu32 " bytesTx=%" PRIu32 " bytesRx=%" PRIu32 " replies=%" PRIu32 "\r\n",
                stats.currentBusLoadKbps,
                stats.currentBusLoadPercent,
                stats.uptimeMs,
                stats.canFramesTx,
                stats.canFramesRx,
                stats.canBytesTx,
                stats.canBytesRx,
                stats.canRepliesReceived);

  Serial.printf("TASKSTATUS req accepted=%" PRIu32 " completed=%" PRIu32 " failed=%" PRIu32 " timeouts=%" PRIu32 " emitted=%" PRIu32 " queued=%u active=%u\r\n",
                stats.requestsAccepted,
                stats.requestsCompleted,
                stats.requestsFailed,
                stats.requestTimeouts,
                stats.responsesEmitted,
                stats.queuedRequests,
                stats.activeRequests);

  Serial.printf("TASKSTATUS twai txErr=%" PRIu32 " rxErr=%" PRIu32 " errPassive=%s msgsToTx=%" PRIu32 " msgsToRx=%" PRIu32 " txFailed=%" PRIu32 " rxMissed=%" PRIu32 " rxOverrun=%" PRIu32 " arbLost=%" PRIu32 " busError=%" PRIu32 "\r\n",
                stats.txErrorCounter,
                stats.rxErrorCounter,
                stats.errorPassive ? "yes" : "no",
                stats.msgsToTx,
                stats.msgsToRx,
                stats.txFailedCount,
                stats.rxMissedCount,
                stats.rxOverrunCount,
                stats.arbLostCount,
                stats.busErrorCount);
}

void printNames(int limit) {
  char schemaPath[OICanTask::kPathLength] = {};
  if (!OICanTask::GetSchemaFileName(schemaPath, sizeof(schemaPath))) {
    Serial.println("NAMES error=no schema path");
    return;
  }

  File file = SPIFFS.open(schemaPath, "r");
  if (!file) {
    Serial.println("NAMES error=failed to open schema");
    return;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error != DeserializationError::Ok) {
    Serial.println("NAMES error=invalid schema json");
    return;
  }

  JsonObject root = doc.as<JsonObject>();
  int index = 0;
  for (JsonPair kv : root) {
    const int id = kv.value()["id"].as<int>();
    if (id <= 0) {
      continue;
    }

    Serial.printf("NAME[%d] %s id=0x%04X\r\n", index, kv.key().c_str(), static_cast<unsigned>(id));
    index++;
    if (index >= limit) {
      break;
    }
  }

  if (index == 0) {
    Serial.println("NAMES empty");
  }
}

void printResponse(const OICanTask::Response& response) {
  Serial.printf("RESP seq=%" PRIu32 " cmd=%s kind=%s result=%s terminal=%u",
                response.sequence,
                commandToString(response.command),
                responseKindToString(response.kind),
                resultToString(response.result),
                response.terminal ? 1U : 0U);

  switch (response.kind) {
    case OICanTask::ResponseKind::Value:
      Serial.printf(" node=%u",
                    response.data.value.nodeId);
      if (response.data.value.name[0] != '\0') {
        Serial.printf(" name=%s", response.data.value.name);
      }
      Serial.printf(" param=0x%04X value=%.3f sample=%u",
                    response.data.value.paramId,
                    response.data.value.value,
                    response.data.value.sampleIndex);
      break;
    case OICanTask::ResponseKind::Progress:
      Serial.printf(" stage=%s current=%" PRIu32 " total=%" PRIu32,
                    response.data.progress.stage,
                    response.data.progress.current,
                    response.data.progress.total);
      break;
    case OICanTask::ResponseKind::FileReady:
      Serial.printf(" path=%s size=%" PRIu32,
                    response.data.file.path,
                    response.data.file.size);
      break;
    case OICanTask::ResponseKind::Serial:
      Serial.printf(" node=%u serial=%08" PRIX32 "-%08" PRIX32 "-%08" PRIX32 "-%08" PRIX32,
                    response.data.serial.nodeId,
                    response.data.serial.words[0],
                    response.data.serial.words[1],
                    response.data.serial.words[2],
                    response.data.serial.words[3]);
      break;
    case OICanTask::ResponseKind::MappingItem:
      Serial.printf(" isrx=%u id=%" PRIu32 " param=0x%04X pos=%u len=%d gain=%.3f offset=%d index=0x%04X sub=%u",
                    response.data.mappingItem.isRx ? 1U : 0U,
                    response.data.mappingItem.canId,
                    response.data.mappingItem.paramId,
                    response.data.mappingItem.position,
                    static_cast<int>(response.data.mappingItem.length),
                    response.data.mappingItem.gain,
                    static_cast<int>(response.data.mappingItem.offset),
                    response.data.mappingItem.index,
                    response.data.mappingItem.subIndex);
      break;
    default:
      break;
  }

  Serial.print("\r\n");
}

void drainStaleResponses() {
  OICanTask::Response response = {};
  while (OICanTask::Receive(response, 0)) {
    Serial.printf("STALE seq=%" PRIu32 " kind=%s result=%s\r\n",
                  response.sequence,
                  responseKindToString(response.kind),
                  resultToString(response.result));
  }
}

bool parseNextCsvItem(const char*& cursor, String& item) {
  item = "";
  if (cursor == nullptr) {
    return false;
  }

  while ((*cursor == ',') || (*cursor == ' ')) {
    cursor++;
  }

  if (*cursor == '\0') {
    return false;
  }

  while ((*cursor != '\0') && (*cursor != ',')) {
    item += *cursor;
    cursor++;
  }

  item.trim();
  if (*cursor == ',') {
    cursor++;
  }

  return item.length() > 0;
}

void printTargetSummary(const BatchStats& stats, size_t expectedValues) {
  const bool pass = (stats.submitted > 0U) &&
                    (stats.accepted == stats.submitted) &&
                    (stats.completed == stats.submitted) &&
                    (stats.failed == 0U) &&
                    (stats.timeouts == 0U) &&
                    (stats.values == expectedValues);

  Serial.printf("TARGET submitted=%u accepted=%u values=%u completed=%u failed=%u timeouts=%u busy=%u result=%s\r\n",
                static_cast<unsigned>(stats.submitted),
                static_cast<unsigned>(stats.accepted),
                static_cast<unsigned>(stats.values),
                static_cast<unsigned>(stats.completed),
                static_cast<unsigned>(stats.failed),
                static_cast<unsigned>(stats.timeouts),
                static_cast<unsigned>(stats.busy),
                pass ? "PASS" : "FAIL");
}

bool parseNodeId(const String& text, uint8_t& nodeIdOut) {
  if (text.isEmpty()) {
    return false;
  }

  const long parsed = text.toInt();
  if (parsed < 1L || parsed > 127L) {
    return false;
  }

  nodeIdOut = static_cast<uint8_t>(parsed);
  return true;
}

bool parseParamId(const String& text, uint16_t& paramIdOut) {
  if (text.isEmpty()) {
    return false;
  }

  char* end = nullptr;
  const unsigned long parsed = strtoul(text.c_str(), &end, 0);
  if ((end == nullptr) || (*end != '\0') || (parsed == 0UL) || (parsed > 0xFFFFUL)) {
    return false;
  }

  paramIdOut = static_cast<uint16_t>(parsed);
  return true;
}

bool parsePositiveU16(const String& text, uint16_t& valueOut) {
  if (text.isEmpty()) {
    return false;
  }

  char* end = nullptr;
  const unsigned long parsed = strtoul(text.c_str(), &end, 10);
  if ((end == nullptr) || (*end != '\0') || (parsed == 0UL) || (parsed > 0xFFFFUL)) {
    return false;
  }

  valueOut = static_cast<uint16_t>(parsed);
  return true;
}

void printSerialTarget(uint8_t nodeId, const SerialCommandResult& result) {
  if (result.hasSerial) {
    Serial.printf("TARGET getserial node=%u result=%s serial=%08" PRIX32 "-%08" PRIX32 "-%08" PRIX32 "-%08" PRIX32 "\r\n",
                  nodeId,
                  result.pass ? "PASS" : "FAIL",
                  result.words[0],
                  result.words[1],
                  result.words[2],
                  result.words[3]);
    return;
  }

  Serial.printf("TARGET getserial node=%u result=FAIL reason=%s\r\n",
                nodeId,
                resultToString(result.result));
}

void printScanTarget(uint8_t lastNode, const ScanCommandResult& result) {
  const bool pass = result.terminalReceived &&
                    (result.result == OICanTask::Result::Ok) &&
                    (result.foundCount > 0U);

  Serial.printf("TARGET scan last=%u found=%u nodes=",
                lastNode,
                static_cast<unsigned>(result.foundCount));

  for (size_t i = 0; i < result.foundCount; i++) {
    if (i > 0U) {
      Serial.print(",");
    }
    Serial.print(result.foundNodes[i]);
  }

  if (result.foundCount == 0U) {
    Serial.print("-");
  }

  Serial.printf(" result=%s reason=%s\r\n",
                pass ? "PASS" : "FAIL",
                resultToString(result.result));
}

void printGetValueTarget(const ValueCommandResult& result) {
  if (result.hasValue) {
    Serial.printf("TARGET getvalue node=%u param=%u value=%.3f result=%s reason=%s\r\n",
                  result.nodeId,
                  result.paramId,
                  result.value,
                  (result.terminalReceived && result.result == OICanTask::Result::Ok) ? "PASS" : "FAIL",
                  resultToString(result.result));
    return;
  }

  Serial.printf("TARGET getvalue node=%u param=%u result=FAIL reason=%s\r\n",
                result.nodeId,
                result.paramId,
                resultToString(result.result));
}

void printDownloadTarget(const DownloadCommandResult& result) {
  if (result.fileReady) {
    Serial.printf("TARGET download node=%u path=%s size=%" PRIu32 " result=%s reason=%s\r\n",
                  result.nodeId,
                  result.path,
                  result.size,
                  (result.terminalReceived && result.result == OICanTask::Result::Ok) ? "PASS" : "FAIL",
                  resultToString(result.result));
    return;
  }

  Serial.printf("TARGET download node=%u result=FAIL reason=%s\r\n",
                result.nodeId,
                resultToString(result.result));
}

void printGetMapTarget(const MapCommandResult& result) {
  Serial.printf("TARGET getmap node=%u items=%u result=%s reason=%s\r\n",
                result.nodeId,
                static_cast<unsigned>(result.itemCount),
                (result.terminalReceived && result.result == OICanTask::Result::Ok) ? "PASS" : "FAIL",
                resultToString(result.result));
}

void printStreamValuesTarget(const StreamValuesCommandResult& result) {
  const bool pass = result.terminalReceived &&
                    (result.result == OICanTask::Result::Ok) &&
                    (result.valueCount > 0U) &&
                    (result.samplesSeen >= result.samplesRequested);

  Serial.printf("TARGET streamvalues rate=%u samples=%u seen=%u values=%u result=%s reason=%s\r\n",
                result.sampleRateHz,
                result.samplesRequested,
                result.samplesSeen,
                static_cast<unsigned>(result.valueCount),
                pass ? "PASS" : "FAIL",
                resultToString(result.result));
}

void runGetBatch(const String names[], size_t nameCount) {
  if ((nameCount == 0U) || (nameCount > kMaxBatchRequests)) {
    Serial.println("TASKGET error=invalid batch size");
    return;
  }

  if (!OICanTask::LockApi(kConsoleLockTimeout)) {
    Serial.println("TASKGET error=api busy");
    return;
  }

  BatchStats stats;
  uint32_t sequences[kMaxBatchRequests] = {};
  size_t terminalsPending = 0;
  drainStaleResponses();

  for (size_t i = 0; i < nameCount; i++) {
    OICanTask::Request request = {};
    request.sequence = OICanTask::AllocateSequence();
    request.command = OICanTask::Command::GetValue;
    request.timeoutTicks = pdMS_TO_TICKS(1000);
    if (!copyNameArg(request.data.getValue.name, sizeof(request.data.getValue.name), names[i])) {
      Serial.printf("TASKGET submit[%u] error=name too long\r\n", static_cast<unsigned>(i));
      stats.busy++;
      continue;
    }

    if (!OICanTask::Submit(request, pdMS_TO_TICKS(100))) {
      Serial.printf("TASKGET submit[%u] error=queue full name=%s\r\n",
                    static_cast<unsigned>(i),
                    names[i].c_str());
      stats.busy++;
      continue;
    }

    sequences[stats.submitted] = request.sequence;
    terminalsPending++;
    stats.submitted++;
    Serial.printf("TASKGET submit[%u] seq=%" PRIu32 " name=%s\r\n",
                  static_cast<unsigned>(i),
                  request.sequence,
                  names[i].c_str());
  }

  const TickType_t startedAt = xTaskGetTickCount();
  const TickType_t overallTimeout =
      (pdMS_TO_TICKS(static_cast<uint32_t>(stats.submitted) * 1200U)) + kConsoleBatchSlack;

  while ((terminalsPending > 0U) && ((xTaskGetTickCount() - startedAt) < overallTimeout)) {
    OICanTask::Response response = {};
    if (!OICanTask::Receive(response, kConsoleReceivePoll)) {
      continue;
    }

    bool knownSequence = false;
    for (size_t i = 0; i < stats.submitted; i++) {
      if (sequences[i] == response.sequence) {
        knownSequence = true;
        break;
      }
    }

    if (!knownSequence) {
      Serial.printf("RESP other seq=%" PRIu32 " kind=%s result=%s\r\n",
                    response.sequence,
                    responseKindToString(response.kind),
                    resultToString(response.result));
      continue;
    }

    printResponse(response);

    switch (response.kind) {
      case OICanTask::ResponseKind::Accepted:
        stats.accepted++;
        break;
      case OICanTask::ResponseKind::Value:
        stats.values++;
        break;
      case OICanTask::ResponseKind::Completed:
        stats.completed++;
        break;
      case OICanTask::ResponseKind::Failed:
        stats.failed++;
        if (response.result == OICanTask::Result::Timeout) {
          stats.timeouts++;
        }
        break;
      default:
        break;
    }

    if (response.terminal) {
      terminalsPending--;
    }
  }

  if (terminalsPending > 0U) {
    stats.failed += terminalsPending;
    stats.timeouts += terminalsPending;
    Serial.printf("TASKGET timeout waiting_for_terminals=%u\r\n", static_cast<unsigned>(terminalsPending));
  }

  OICanTask::UnlockApi();
  printTargetSummary(stats, stats.submitted);
}

void handleTaskGet(const String& args) {
  String name = args;
  name.trim();
  if (name.isEmpty()) {
    Serial.println("TASKGET error=missing name");
    return;
  }

  String names[1];
  names[0] = name;
  runGetBatch(names, 1);
}

void handleGetSerial(const String& args) {
  String value = args;
  value.trim();

  uint8_t nodeId = 0;
  if (!parseNodeId(value, nodeId)) {
    Serial.println("GETSERIAL error=invalid nodeid");
    return;
  }

  if (!OICanTask::LockApi(kConsoleLockTimeout)) {
    Serial.println("GETSERIAL error=api busy");
    return;
  }

  drainStaleResponses();

  OICanTask::Request request = {};
  request.sequence = OICanTask::AllocateSequence();
  request.command = OICanTask::Command::GetSerial;
  request.timeoutTicks = pdMS_TO_TICKS(250);
  request.data.getSerial.nodeId = nodeId;

  if (!OICanTask::Submit(request, pdMS_TO_TICKS(100))) {
    OICanTask::UnlockApi();
    Serial.printf("GETSERIAL submit error=node=%u\r\n", nodeId);
    return;
  }

  Serial.printf("GETSERIAL submit seq=%" PRIu32 " node=%u\r\n", request.sequence, nodeId);

  SerialCommandResult serialResult = {};
  const TickType_t startedAt = xTaskGetTickCount();
  const TickType_t overallTimeout = pdMS_TO_TICKS(3000);

  while ((xTaskGetTickCount() - startedAt) < overallTimeout) {
    OICanTask::Response response = {};
    if (!OICanTask::Receive(response, kConsoleReceivePoll)) {
      continue;
    }

    if (response.sequence != request.sequence) {
      Serial.printf("RESP other seq=%" PRIu32 " kind=%s result=%s\r\n",
                    response.sequence,
                    responseKindToString(response.kind),
                    resultToString(response.result));
      continue;
    }

    printResponse(response);

    if (response.kind == OICanTask::ResponseKind::Serial) {
      serialResult.hasSerial = true;
      memcpy(serialResult.words, response.data.serial.words, sizeof(serialResult.words));
    }

    if (response.terminal) {
      serialResult.terminalReceived = true;
      serialResult.result = response.result;
      serialResult.pass = (response.result == OICanTask::Result::Ok) && serialResult.hasSerial;
      break;
    }
  }

  if (!serialResult.terminalReceived) {
    serialResult.result = OICanTask::Result::Timeout;
  }

  OICanTask::UnlockApi();
  printSerialTarget(nodeId, serialResult);
}

void handleGetValue(const String& args) {
  const int spacePos = args.indexOf(' ');
  if (spacePos < 0) {
    Serial.println("GETVALUE error=usage getvalue <nodeid> <paramid>");
    return;
  }

  String nodeText = args.substring(0, spacePos);
  String paramText = args.substring(spacePos + 1);
  nodeText.trim();
  paramText.trim();

  uint8_t nodeId = 0;
  uint16_t paramId = 0;
  if (!parseNodeId(nodeText, nodeId) || !parseParamId(paramText, paramId)) {
    Serial.println("GETVALUE error=invalid nodeid or paramid");
    return;
  }

  if (!OICanTask::LockApi(kConsoleLockTimeout)) {
    Serial.println("GETVALUE error=api busy");
    return;
  }

  drainStaleResponses();

  OICanTask::Request request = {};
  request.sequence = OICanTask::AllocateSequence();
  request.command = OICanTask::Command::GetValueById;
  request.timeoutTicks = pdMS_TO_TICKS(120);
  request.data.getValueById.nodeId = nodeId;
  request.data.getValueById.paramId = paramId;

  if (!OICanTask::Submit(request, pdMS_TO_TICKS(100))) {
    OICanTask::UnlockApi();
    Serial.printf("GETVALUE submit error=node=%u param=%u\r\n", nodeId, paramId);
    return;
  }

  Serial.printf("GETVALUE submit seq=%" PRIu32 " node=%u param=%u\r\n",
                request.sequence,
                nodeId,
                paramId);

  ValueCommandResult valueResult = {};
  valueResult.nodeId = nodeId;
  valueResult.paramId = paramId;
  const TickType_t startedAt = xTaskGetTickCount();
  const TickType_t overallTimeout = pdMS_TO_TICKS(2000);

  while ((xTaskGetTickCount() - startedAt) < overallTimeout) {
    OICanTask::Response response = {};
    if (!OICanTask::Receive(response, kConsoleReceivePoll)) {
      continue;
    }

    if (response.sequence != request.sequence) {
      Serial.printf("RESP other seq=%" PRIu32 " kind=%s result=%s\r\n",
                    response.sequence,
                    responseKindToString(response.kind),
                    resultToString(response.result));
      continue;
    }

    printResponse(response);

    if (response.kind == OICanTask::ResponseKind::Value) {
      valueResult.hasValue = true;
      valueResult.nodeId = response.data.value.nodeId;
      valueResult.paramId = response.data.value.paramId;
      valueResult.value = response.data.value.value;
    }

    if (response.terminal) {
      valueResult.terminalReceived = true;
      valueResult.result = response.result;
      break;
    }
  }

  if (!valueResult.terminalReceived) {
    valueResult.result = OICanTask::Result::Timeout;
  }

  OICanTask::UnlockApi();
  printGetValueTarget(valueResult);
}

void handleDownload(const String& args) {
  String value = args;
  value.trim();

  uint8_t nodeId = 0;
  if (!parseNodeId(value, nodeId)) {
    Serial.println("DOWNLOAD error=invalid nodeid");
    return;
  }

  if (!OICanTask::LockApi(kConsoleLockTimeout)) {
    Serial.println("DOWNLOAD error=api busy");
    return;
  }

  drainStaleResponses();

  OICanTask::Request request = {};
  request.sequence = OICanTask::AllocateSequence();
  request.command = OICanTask::Command::DownloadSchemaJson;
  request.timeoutTicks = pdMS_TO_TICKS(5000);
  request.data.downloadSchemaJson.forceRedownload = true;
  request.data.downloadSchemaJson.nodeId = nodeId;

  if (!OICanTask::Submit(request, pdMS_TO_TICKS(100))) {
    OICanTask::UnlockApi();
    Serial.printf("DOWNLOAD submit error=node=%u\r\n", nodeId);
    return;
  }

  Serial.printf("DOWNLOAD submit seq=%" PRIu32 " node=%u\r\n", request.sequence, nodeId);

  DownloadCommandResult downloadResult = {};
  downloadResult.nodeId = nodeId;
  const TickType_t startedAt = xTaskGetTickCount();
  const TickType_t overallTimeout = pdMS_TO_TICKS(60000);

  while ((xTaskGetTickCount() - startedAt) < overallTimeout) {
    OICanTask::Response response = {};
    if (!OICanTask::Receive(response, kConsoleReceivePoll)) {
      continue;
    }

    if (response.sequence != request.sequence) {
      Serial.printf("RESP other seq=%" PRIu32 " kind=%s result=%s\r\n",
                    response.sequence,
                    responseKindToString(response.kind),
                    resultToString(response.result));
      continue;
    }

    printResponse(response);

    if (response.kind == OICanTask::ResponseKind::FileReady) {
      downloadResult.fileReady = true;
      downloadResult.size = response.data.file.size;
      strncpy(downloadResult.path, response.data.file.path, sizeof(downloadResult.path) - 1U);
      downloadResult.path[sizeof(downloadResult.path) - 1U] = '\0';
    }

    if (response.terminal) {
      downloadResult.terminalReceived = true;
      downloadResult.result = response.result;
      break;
    }
  }

  if (!downloadResult.terminalReceived) {
    downloadResult.result = OICanTask::Result::Timeout;
  }

  OICanTask::UnlockApi();
  printDownloadTarget(downloadResult);
}

void handleGetMap(const String& args) {
  String value = args;
  value.trim();

  uint8_t nodeId = 0;
  if (!parseNodeId(value, nodeId)) {
    Serial.println("GETMAP error=invalid nodeid");
    return;
  }

  if (!OICanTask::LockApi(kConsoleLockTimeout)) {
    Serial.println("GETMAP error=api busy");
    return;
  }

  drainStaleResponses();

  OICanTask::Request request = {};
  request.sequence = OICanTask::AllocateSequence();
  request.command = OICanTask::Command::ReadCanMap;
  request.timeoutTicks = pdMS_TO_TICKS(5000);
  request.data.readCanMap.nodeId = nodeId;

  if (!OICanTask::Submit(request, pdMS_TO_TICKS(100))) {
    OICanTask::UnlockApi();
    Serial.printf("GETMAP submit error=node=%u\r\n", nodeId);
    return;
  }

  Serial.printf("GETMAP submit seq=%" PRIu32 " node=%u\r\n", request.sequence, nodeId);

  MapCommandResult mapResult = {};
  mapResult.nodeId = nodeId;
  JsonDocument mapDoc;
  JsonArray mapArray = mapDoc.to<JsonArray>();
  const TickType_t startedAt = xTaskGetTickCount();
  const TickType_t overallTimeout = pdMS_TO_TICKS(12000);

  while ((xTaskGetTickCount() - startedAt) < overallTimeout) {
    OICanTask::Response response = {};
    if (!OICanTask::Receive(response, kConsoleReceivePoll)) {
      continue;
    }

    if (response.sequence != request.sequence) {
      Serial.printf("RESP other seq=%" PRIu32 " kind=%s result=%s\r\n",
                    response.sequence,
                    responseKindToString(response.kind),
                    resultToString(response.result));
      continue;
    }

    printResponse(response);

    if (response.kind == OICanTask::ResponseKind::MappingItem) {
      JsonObject object = mapArray.add<JsonObject>();
      object["isrx"] = response.data.mappingItem.isRx;
      object["id"] = response.data.mappingItem.canId;
      object["paramid"] = response.data.mappingItem.paramId;
      object["position"] = response.data.mappingItem.position;
      object["length"] = response.data.mappingItem.length;
      object["gain"] = response.data.mappingItem.gain;
      object["offset"] = response.data.mappingItem.offset;
      object["index"] = response.data.mappingItem.index;
      object["subindex"] = response.data.mappingItem.subIndex;
      mapResult.itemCount++;
    }

    if (response.terminal) {
      mapResult.terminalReceived = true;
      mapResult.result = response.result;
      break;
    }
  }

  if (!mapResult.terminalReceived) {
    mapResult.result = OICanTask::Result::Timeout;
  }

  OICanTask::UnlockApi();

  if (mapResult.terminalReceived && mapResult.result == OICanTask::Result::Ok) {
    String json;
    serializeJson(mapDoc, json);
    Serial.printf("CANMAP %s\r\n", json.c_str());
  }

  printGetMapTarget(mapResult);
}

void handleScan(const String& args) {
  String value = args;
  value.trim();

  uint8_t lastNode = 0;
  if (!parseNodeId(value, lastNode)) {
    Serial.println("SCAN error=invalid last node");
    return;
  }

  if (!OICanTask::LockApi(kConsoleLockTimeout)) {
    Serial.println("SCAN error=api busy");
    return;
  }

  drainStaleResponses();

  OICanTask::Request request = {};
  request.sequence = OICanTask::AllocateSequence();
  request.command = OICanTask::Command::ScanNodes;
  request.timeoutTicks = pdMS_TO_TICKS(20);
  request.data.scanNodes.lastNode = lastNode;

  if (!OICanTask::Submit(request, pdMS_TO_TICKS(100))) {
    OICanTask::UnlockApi();
    Serial.printf("SCAN submit error=last=%u\r\n", lastNode);
    return;
  }

  Serial.printf("SCAN submit seq=%" PRIu32 " last=%u\r\n", request.sequence, lastNode);

  ScanCommandResult scanResult = {};
  const TickType_t startedAt = xTaskGetTickCount();
  const TickType_t overallTimeout =
      pdMS_TO_TICKS(static_cast<uint32_t>(lastNode) * 25U + 3000U);

  while ((xTaskGetTickCount() - startedAt) < overallTimeout) {
    OICanTask::Response response = {};
    if (!OICanTask::Receive(response, kConsoleReceivePoll)) {
      continue;
    }

    if (response.sequence != request.sequence) {
      Serial.printf("RESP other seq=%" PRIu32 " kind=%s result=%s\r\n",
                    response.sequence,
                    responseKindToString(response.kind),
                    resultToString(response.result));
      continue;
    }

    printResponse(response);

    if ((response.kind == OICanTask::ResponseKind::Serial) &&
        (scanResult.foundCount < (sizeof(scanResult.foundNodes) / sizeof(scanResult.foundNodes[0])))) {
      scanResult.foundNodes[scanResult.foundCount++] = response.data.serial.nodeId;
    }

    if (response.terminal) {
      scanResult.terminalReceived = true;
      scanResult.result = response.result;
      break;
    }
  }

  if (!scanResult.terminalReceived) {
    scanResult.result = OICanTask::Result::Timeout;
  }

  OICanTask::UnlockApi();
  printScanTarget(lastNode, scanResult);
}

void handleStreamValues(const String& args) {
  const int spacePos = args.indexOf(' ');
  String rateText = (spacePos >= 0) ? args.substring(0, spacePos) : args;
  String sampleText = (spacePos >= 0) ? args.substring(spacePos + 1) : "";
  rateText.trim();
  sampleText.trim();

  uint16_t sampleRateHz = 0;
  uint16_t samples = kDefaultStreamSampleCount;
  if (!parsePositiveU16(rateText, sampleRateHz)) {
    Serial.println("STREAMVALUES error=usage streamvalues <hz> [samples]");
    return;
  }

  if (!sampleText.isEmpty() && !parsePositiveU16(sampleText, samples)) {
    Serial.println("STREAMVALUES error=invalid samples");
    return;
  }

  if (!OICanTask::LockApi(kConsoleLockTimeout)) {
    Serial.println("STREAMVALUES error=api busy");
    return;
  }

  drainStaleResponses();

  OICanTask::Request request = {};
  request.sequence = OICanTask::AllocateSequence();
  request.command = OICanTask::Command::StreamValues;
  request.timeoutTicks = pdMS_TO_TICKS(60000);
  request.data.streamValues.samples = samples;
  request.data.streamValues.sampleRateHz = sampleRateHz;
  request.data.streamValues.allValues = true;
  request.data.streamValues.namesCsv[0] = '\0';

  if (!OICanTask::Submit(request, pdMS_TO_TICKS(100))) {
    OICanTask::UnlockApi();
    Serial.printf("STREAMVALUES submit error=rate=%u samples=%u\r\n", sampleRateHz, samples);
    return;
  }

  Serial.printf("STREAMVALUES submit seq=%" PRIu32 " rate=%u samples=%u mode=all\r\n",
                request.sequence,
                sampleRateHz,
                samples);

  StreamValuesCommandResult streamResult = {};
  streamResult.sampleRateHz = sampleRateHz;
  streamResult.samplesRequested = samples;
  const TickType_t startedAt = xTaskGetTickCount();
  const TickType_t samplePeriodMs = (sampleRateHz == 0U) ? 0U : (1000U / sampleRateHz);
  const TickType_t overallTimeout =
      pdMS_TO_TICKS(static_cast<uint32_t>(samples) * (samplePeriodMs + 12000U) + 5000U);

  while ((xTaskGetTickCount() - startedAt) < overallTimeout) {
    OICanTask::Response response = {};
    if (!OICanTask::Receive(response, kConsoleReceivePoll)) {
      continue;
    }

    if (response.sequence != request.sequence) {
      Serial.printf("RESP other seq=%" PRIu32 " kind=%s result=%s\r\n",
                    response.sequence,
                    responseKindToString(response.kind),
                    resultToString(response.result));
      continue;
    }

    printResponse(response);

    if (response.kind == OICanTask::ResponseKind::Value) {
      streamResult.valueCount++;
      const uint16_t seen = static_cast<uint16_t>(response.data.value.sampleIndex + 1U);
      if (seen > streamResult.samplesSeen) {
        streamResult.samplesSeen = seen;
      }
    }

    if (response.terminal) {
      streamResult.terminalReceived = true;
      streamResult.result = response.result;
      break;
    }
  }

  if (!streamResult.terminalReceived) {
    streamResult.result = OICanTask::Result::Timeout;
  }

  OICanTask::UnlockApi();
  printStreamValuesTarget(streamResult);
}

void handleTaskGetMany(const String& args) {
  const char* cursor = args.c_str();
  String names[kMaxBatchRequests];
  size_t count = 0;
  while (count < kMaxBatchRequests) {
    String item;
    if (!parseNextCsvItem(cursor, item)) {
      break;
    }
    names[count++] = item;
  }

  if (count == 0U) {
    Serial.println("TASKGETMANY error=no names");
    return;
  }

  runGetBatch(names, count);
}

void handleTrace(const String& args) {
  String value = args;
  value.trim();
  value.toLowerCase();

  if (value == "on") {
    OICanTask::SetTraceEnabled(true);
    Serial.println("TRACE on");
  }
  else if (value == "off") {
    OICanTask::SetTraceEnabled(false);
    Serial.println("TRACE off");
  }
  else {
    Serial.printf("TRACE %s\r\n", OICanTask::GetTraceEnabled() ? "on" : "off");
  }
}

void handleCommand(const String& line) {
  String command = line;
  command.trim();
  if (command.isEmpty()) {
    return;
  }

  const int spacePos = command.indexOf(' ');
  String verb = (spacePos >= 0) ? command.substring(0, spacePos) : command;
  String args = (spacePos >= 0) ? command.substring(spacePos + 1) : "";
  verb.toLowerCase();
  args.trim();

  if (verb == "help") {
    printHelp();
  }
  else if (verb == "trace") {
    handleTrace(args);
  }
  else if (verb == "status") {
    printStatus();
  }
  else if (verb == "taskstatus") {
    printTaskStatus();
  }
  else if (verb == "names") {
    const int limit = args.isEmpty() ? 5 : args.toInt();
    printNames(limit > 0 ? limit : 5);
  }
  else if (verb == "getserial") {
    handleGetSerial(args);
  }
  else if (verb == "getvalue") {
    handleGetValue(args);
  }
  else if (verb == "getmap") {
    handleGetMap(args);
  }
  else if (verb == "download") {
    handleDownload(args);
  }
  else if (verb == "scan") {
    handleScan(args);
  }
  else if (verb == "streamvalues") {
    handleStreamValues(args);
  }
  else if (verb == "taskget") {
    handleTaskGet(args);
  }
  else if (verb == "taskgetmany") {
    handleTaskGetMany(args);
  }
  else {
    Serial.printf("ERROR unknown command=%s\r\n", verb.c_str());
    printHelp();
  }
}

} // namespace

void Begin() {
  if (g_started) {
    return;
  }

  g_started = true;
  Serial.println();
  Serial.println("OI debug console ready");
  printHelp();
  printPrompt();
}

void Loop() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());

    if ((c == '\r') || (c == '\n')) {
      if (!g_lineBuffer.isEmpty()) {
        Serial.println();
        handleCommand(g_lineBuffer);
        g_lineBuffer = "";
        printPrompt();
      }
      continue;
    }

    if ((c == '\b') || (c == 127)) {
      if (!g_lineBuffer.isEmpty()) {
        g_lineBuffer.remove(g_lineBuffer.length() - 1);
      }
      continue;
    }

    if (isPrintable(static_cast<unsigned char>(c))) {
      if (g_lineBuffer.isEmpty()) {
        Serial.print(c);
      }
      else {
        Serial.print(c);
      }
      g_lineBuffer += c;
    }
  }
}

} // namespace DebugConsole
