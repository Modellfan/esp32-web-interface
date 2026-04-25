#include "debug_console.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include <cstring>

#include "oi_can_task.h"

namespace DebugConsole {

namespace {

constexpr size_t kMaxBatchRequests = 20;
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

const char* commandToString(OICanTask::Command command) {
  switch (command) {
    case OICanTask::Command::Reconfigure:
      return "Reconfigure";
    case OICanTask::Command::GetStatus:
      return "GetStatus";
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
  Serial.println("  names [count]");
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
      Serial.printf(" name=%s param=0x%04X value=%.3f sample=%u",
                    response.data.value.name,
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
  else if (verb == "names") {
    const int limit = args.isEmpty() ? 5 : args.toInt();
    printNames(limit > 0 ? limit : 5);
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
