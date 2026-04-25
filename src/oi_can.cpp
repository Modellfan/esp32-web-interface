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
#include "oi_can.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include <cstring>

#include "oi_can_task.h"

namespace OICan {

namespace {

uint8_t toTaskBaudRate(BaudRate baudRate) {
  return static_cast<uint8_t>(baudRate);
}

SetResult mapTaskResult(OICanTask::Result result) {
  switch (result) {
    case OICanTask::Result::Ok:
      return Ok;
    case OICanTask::Result::UnknownIndex:
    case OICanTask::Result::InvalidRequest:
      return UnknownIndex;
    case OICanTask::Result::ValueOutOfRange:
      return ValueOutOfRange;
    default:
      return CommError;
  }
}

struct ValueContext {
  double value = 0.0;
  bool hasValue = false;
};

struct SnapshotContext {
  JsonDocument* doc = nullptr;
  bool anyValue = false;
};

struct CanMapContext {
  JsonArray array;
};

struct StreamContext {
  String result;
  int currentSample = -1;
  bool firstInSample = true;
};

bool captureValueCallback(const OICanTask::Response& response, void* context) {
  if (response.kind == OICanTask::ResponseKind::Value) {
    auto* valueContext = static_cast<ValueContext*>(context);
    valueContext->value = response.data.value.value;
    valueContext->hasValue = true;
  }
  return true;
}

bool snapshotCallback(const OICanTask::Response& response, void* context) {
  if (response.kind == OICanTask::ResponseKind::Value) {
    auto* snapshotContext = static_cast<SnapshotContext*>(context);
    if (snapshotContext->doc != nullptr) {
      (*snapshotContext->doc)[response.data.value.name]["value"] = response.data.value.value;
      snapshotContext->anyValue = true;
    }
  }
  return true;
}

bool canMapCallback(const OICanTask::Response& response, void* context) {
  if (response.kind == OICanTask::ResponseKind::MappingItem) {
    auto* mapContext = static_cast<CanMapContext*>(context);
    JsonObject object = mapContext->array.add<JsonObject>();
    object["isrx"] = response.data.mappingItem.isRx;
    object["id"] = response.data.mappingItem.canId;
    object["paramid"] = response.data.mappingItem.paramId;
    object["position"] = response.data.mappingItem.position;
    object["length"] = response.data.mappingItem.length;
    object["gain"] = response.data.mappingItem.gain;
    object["offset"] = response.data.mappingItem.offset;
    object["index"] = response.data.mappingItem.index;
    object["subindex"] = response.data.mappingItem.subIndex;
  }
  return true;
}

bool streamCallback(const OICanTask::Response& response, void* context) {
  if (response.kind == OICanTask::ResponseKind::Value) {
    auto* streamContext = static_cast<StreamContext*>(context);
    const int sampleIndex = response.data.value.sampleIndex;

    if (streamContext->currentSample != sampleIndex) {
      if (streamContext->currentSample >= 0) {
        streamContext->result += "\r\n";
      }
      streamContext->currentSample = sampleIndex;
      streamContext->firstInSample = true;
    }

    if (!streamContext->firstInSample) {
      streamContext->result += ",";
    }

    streamContext->result += String(response.data.value.value, 2);
    streamContext->firstInSample = false;
  }
  return true;
}

bool copyStringArg(char* dest, size_t destSize, const String& source) {
  if ((source.length() + 1U) > destSize) {
    return false;
  }

  source.toCharArray(dest, destSize);
  return true;
}

bool requestSchemaRefresh() {
  OICanTask::Request request = {};
  request.command = OICanTask::Command::DownloadSchemaJson;
  request.timeoutTicks = pdMS_TO_TICKS(5000);
  request.data.downloadSchemaJson.forceRedownload = true;
  return OICanTask::SubmitAndDrain(request, nullptr, nullptr) == OICanTask::Result::Ok;
}

} // namespace

void Init(uint8_t nodeId, BaudRate baud, int txPin, int rxPin) {
  if (!OICanTask::StartTask()) {
    return;
  }

  OICanTask::Reconfigure(nodeId, toTaskBaudRate(baud), txPin, rxPin);
}

void Loop() {
  delay(0);
}

bool SendJson(WebServer& server) {
  char schemaPath[OICanTask::kPathLength] = {};
  if (!OICanTask::GetSchemaFileName(schemaPath, sizeof(schemaPath))) {
    return false;
  }

  File file = SPIFFS.open(schemaPath, "r");
  if (!file) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error != DeserializationError::Ok) {
    requestSchemaRefresh();
    return false;
  }

  SnapshotContext context;
  context.doc = &doc;
  context.anyValue = false;
  OICanTask::Request request = {};
  request.command = OICanTask::Command::ReadLiveSnapshot;
  request.timeoutTicks = pdMS_TO_TICKS(15000);

  const OICanTask::Result result = OICanTask::SubmitAndDrain(request, snapshotCallback, &context);
  if ((result != OICanTask::Result::Ok) || !context.anyValue) {
    return false;
  }

  auto client = server.client();
  server.setContentLength(measureJson(doc));
  server.send(200, "application/json", "");
  serializeJson(doc, client);
  return true;
}

void SendCanMapping(WebServer& server) {
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  CanMapContext context;
  context.array = array;

  OICanTask::Request request = {};
  request.command = OICanTask::Command::ReadCanMap;
  request.timeoutTicks = pdMS_TO_TICKS(5000);

  const OICanTask::Result result = OICanTask::SubmitAndDrain(request, canMapCallback, &context);
  if (result != OICanTask::Result::Ok) {
    server.send(500, "text/plain", "CAN communication error");
    return;
  }

  auto client = server.client();
  server.setContentLength(measureJson(doc));
  server.send(200, "application/json", "");
  serializeJson(doc, client);
}

SetResult AddCanMapping(String json) {
  OICanTask::Request request = {};
  request.command = OICanTask::Command::AddCanMapping;
  request.timeoutTicks = pdMS_TO_TICKS(1000);
  if (!copyStringArg(request.data.mapJson.json, sizeof(request.data.mapJson.json), json)) {
    return CommError;
  }

  return mapTaskResult(OICanTask::SubmitAndDrain(request, nullptr, nullptr));
}

SetResult RemoveCanMapping(String json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    return UnknownIndex;
  }

  if (doc["index"].isNull() || doc["subindex"].isNull()) {
    return UnknownIndex;
  }

  OICanTask::Request request = {};
  request.command = OICanTask::Command::RemoveCanMapping;
  request.timeoutTicks = pdMS_TO_TICKS(500);
  request.data.removeMap.index = doc["index"].as<uint16_t>();
  request.data.removeMap.subIndex = doc["subindex"].as<uint8_t>();
  return mapTaskResult(OICanTask::SubmitAndDrain(request, nullptr, nullptr));
}

SetResult SetValue(String name, double value) {
  OICanTask::Request request = {};
  request.command = OICanTask::Command::SetValue;
  request.timeoutTicks = pdMS_TO_TICKS(500);
  if (!copyStringArg(request.data.setValue.name, sizeof(request.data.setValue.name), name)) {
    return UnknownIndex;
  }
  request.data.setValue.value = static_cast<float>(value);
  return mapTaskResult(OICanTask::SubmitAndDrain(request, nullptr, nullptr));
}

double GetValue(String name) {
  OICanTask::Request request = {};
  request.command = OICanTask::Command::GetValue;
  request.timeoutTicks = pdMS_TO_TICKS(500);
  if (!copyStringArg(request.data.getValue.name, sizeof(request.data.getValue.name), name)) {
    return 0;
  }

  ValueContext context;
  const OICanTask::Result result = OICanTask::SubmitAndDrain(request, captureValueCallback, &context);
  if ((result != OICanTask::Result::Ok) || !context.hasValue) {
    return 0;
  }

  return context.value;
}

bool StartStop(int opmode) {
  OICanTask::Request request = {};
  request.command = OICanTask::Command::StartStop;
  request.timeoutTicks = pdMS_TO_TICKS(500);
  request.data.startStop.opmode = opmode;
  return OICanTask::SubmitAndDrain(request, nullptr, nullptr) == OICanTask::Result::Ok;
}

bool SaveToFlash() {
  OICanTask::Request request = {};
  request.command = OICanTask::Command::SaveToFlash;
  request.timeoutTicks = pdMS_TO_TICKS(500);
  return OICanTask::SubmitAndDrain(request, nullptr, nullptr) == OICanTask::Result::Ok;
}

String StreamValues(String names, int samples) {
  OICanTask::Request request = {};
  request.command = OICanTask::Command::StreamValues;
  request.timeoutTicks = pdMS_TO_TICKS(static_cast<uint32_t>(samples > 0 ? samples : 1) * 500U + 1000U);
  request.data.streamValues.samples = static_cast<uint16_t>(samples > 0 ? samples : 0);
  if (!copyStringArg(request.data.streamValues.namesCsv, sizeof(request.data.streamValues.namesCsv), names)) {
    return "";
  }

  StreamContext context;
  const OICanTask::Result result = OICanTask::SubmitAndDrain(request, streamCallback, &context);
  if (result != OICanTask::Result::Ok) {
    return "";
  }

  if (context.currentSample >= 0) {
    context.result += "\r\n";
  }

  return context.result;
}

int StartUpdate(String fileName) {
  OICanTask::Request request = {};
  request.command = OICanTask::Command::StartFirmwareUpdate;
  request.timeoutTicks = pdMS_TO_TICKS(1000);
  if (!copyStringArg(request.data.startFirmwareUpdate.fileName, sizeof(request.data.startFirmwareUpdate.fileName), fileName)) {
    return 0;
  }

  if (OICanTask::SubmitAndDrain(request, nullptr, nullptr) != OICanTask::Result::Ok) {
    return 0;
  }

  return OICanTask::GetUpdateTotalPages();
}

int GetCurrentUpdatePage() {
  return OICanTask::GetCurrentUpdatePage();
}

int GetNodeId() {
  return OICanTask::GetNodeId();
}

BaudRate GetBaudRate() {
  return static_cast<BaudRate>(OICanTask::GetBaudRate());
}

} // namespace OICan
