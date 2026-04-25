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
#ifndef OI_CAN_TASK_H
#define OI_CAN_TASK_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

namespace OICanTask {

constexpr size_t kNameLength = 32;
constexpr size_t kNamesCsvLength = 192;
constexpr size_t kMapJsonLength = 256;
constexpr size_t kFileNameLength = 64;
constexpr size_t kPathLength = 32;
constexpr size_t kProgressStageLength = 24;

enum class Command : uint8_t {
  Reconfigure,
  GetStatus,
  DownloadSchemaJson,
  ReadLiveSnapshot,
  ReadCanMap,
  StreamValues,
  GetValue,
  SetValue,
  AddCanMapping,
  RemoveCanMapping,
  SaveToFlash,
  StartStop,
  StartFirmwareUpdate,
  GetUpdateStatus
};

enum class Result : uint8_t {
  Ok,
  Busy,
  NotReady,
  Timeout,
  CommError,
  UnknownIndex,
  ValueOutOfRange,
  InvalidRequest,
  FileError
};

enum class TaskState : uint8_t {
  Stopped,
  Starting,
  DownloadingJson,
  Ready,
  Updating,
  Error
};

enum class ResponseKind : uint8_t {
  Accepted,
  Progress,
  Value,
  MappingItem,
  FileReady,
  Completed,
  Failed
};

struct StatusPayload {
  TaskState state;
  uint8_t nodeId;
  uint8_t baudRate;
  uint8_t updateCurrentPage;
  uint8_t updateTotalPages;
  bool schemaAvailable;
  uint32_t serialCrc;
  uint8_t queuedRequests;
  uint8_t activeRequests;
  uint8_t maxActiveRequests;
};

struct ProgressPayload {
  char stage[kProgressStageLength];
  uint32_t current;
  uint32_t total;
};

struct ValuePayload {
  char name[kNameLength];
  uint16_t paramId;
  float value;
  uint16_t sampleIndex;
};

struct MappingItemPayload {
  bool isRx;
  uint32_t canId;
  uint16_t paramId;
  uint8_t position;
  int8_t length;
  float gain;
  int8_t offset;
  uint16_t index;
  uint8_t subIndex;
};

struct FilePayload {
  char path[kPathLength];
  uint32_t size;
};

struct Request {
  uint32_t sequence;
  Command command;
  TickType_t timeoutTicks;

  union {
    struct {
      uint8_t nodeId;
      uint8_t baudRate;
      int txPin;
      int rxPin;
    } reconfigure;

    struct {
      bool forceRedownload;
    } downloadSchemaJson;

    struct {
      bool includeMetadata;
    } readLiveSnapshot;

    struct {
      char name[kNameLength];
    } getValue;

    struct {
      char name[kNameLength];
      float value;
    } setValue;

    struct {
      uint16_t samples;
      char namesCsv[kNamesCsvLength];
    } streamValues;

    struct {
      char json[kMapJsonLength];
    } mapJson;

    struct {
      uint16_t index;
      uint8_t subIndex;
    } removeMap;

    struct {
      int opmode;
    } startStop;

    struct {
      char fileName[kFileNameLength];
    } startFirmwareUpdate;
  } data;
};

struct Response {
  uint32_t sequence;
  Command command;
  ResponseKind kind;
  Result result;
  bool terminal;

  union {
    StatusPayload status;
    ProgressPayload progress;
    ValuePayload value;
    MappingItemPayload mappingItem;
    FilePayload file;
  } data;
};

using ResponseCallback = bool (*)(const Response& event, void* context);

uint32_t AllocateSequence();
bool StartTask();
bool LockApi(TickType_t timeoutTicks = pdMS_TO_TICKS(50));
void UnlockApi();
bool Submit(const Request& request, TickType_t sendTimeoutTicks = pdMS_TO_TICKS(50));
bool Receive(Response& response, TickType_t waitTicks);
Result SubmitAndDrain(Request request,
                      ResponseCallback callback,
                      void* context,
                      TickType_t sendTimeoutTicks = pdMS_TO_TICKS(50));
void SetTraceEnabled(bool enabled);
bool GetTraceEnabled();

bool Reconfigure(uint8_t nodeId, uint8_t baudRate, int txPin, int rxPin);
Result GetStatus(StatusPayload& status);
bool GetCachedStatus(StatusPayload& status);
bool GetSchemaFileName(char* path, size_t pathSize);
uint8_t GetNodeId();
uint8_t GetBaudRate();
int GetCurrentUpdatePage();
int GetUpdateTotalPages();

} // namespace OICanTask

#endif
