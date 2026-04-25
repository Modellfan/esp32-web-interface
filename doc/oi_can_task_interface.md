# `oi_can_task` Interface Proposal

## Goal

Prepare the current `src/oi_can.cpp` code for a FreeRTOS task-based split where:

- one task owns all TWAI receive/transmit access
- SDO request/reply handling is serialized in one place
- startup JSON download stays inside the CAN task
- the web layer stops calling blocking CAN code directly
- the task boundary is a request queue and a response queue
- one request may emit multiple response events before a final completion/error event
- the task maintains an internal table for queued/active requests, per-request deadlines, and bus-load limits

## What The Current Code Is Doing

The current `OICan` module mixes three different responsibilities:

1. Background CAN state handling
   - `Init()` starts TWAI and kicks off serial discovery
   - `Loop()` receives frames, retries startup requests, and drives the update state machine
   - `handleSdoResponse()` and `handleUpdate()` are long-lived background state handlers

2. Synchronous RPC-style calls
   - `SetValue()`, `GetValue()`, `SaveToFlash()`, `StartStop()`
   - `SendJson()`, `SendCanMapping()`, `AddCanMapping()`, `RemoveCanMapping()`
   - these functions directly call `drainReceiveQueue()` and `waitForSdoReply()`

3. HTTP and SPIFFS-facing work
   - `SendJson()` and `SendCanMapping()` talk to `WebServer`
   - parameter name lookup repeatedly opens the downloaded JSON file
   - JSON download is started from the CAN state machine and stored in SPIFFS

That is workable in the current single-threaded loop, but it does not scale cleanly to multiple tasks because `twai_receive()` and `twai_transmit()` are being used from more than one execution path.

## Main Constraints From The Existing Code

These points should shape the new interface:

- `drainReceiveQueue()` and `waitForSdoReply()` assume exclusive ownership of the CAN RX queue.
- `Loop()` is required for startup, JSON download retry, and firmware update progress.
- firmware update shares the same CAN receive path as normal SDO traffic.
- `SendJson()` and `StreamValues()` can produce payloads much larger than a queue item.
- `getId()` re-parses SPIFFS on every value read/write, so the task should keep a RAM cache once JSON is available.
- current reply matching is based on node id plus index/subindex, not on a transaction id, so true multi-request pipelining on one default SDO channel must be treated as capability-limited.

## Proposed Split

### `oi_can_task` owns

- TWAI driver install/start/stop/reconfigure
- all `twai_receive()` and `twai_transmit()` calls
- SDO frame formatting and reply matching
- startup serial discovery
- JSON definition download and validation
- SPIFFS storage for:
  - downloaded schema file (`/<serial>.json`)
  - firmware update source files
  - optional compatibility output files only if a legacy HTTP endpoint still requires one prebuilt artifact
- firmware update state machine
- an in-RAM cache of `name -> id` loaded from the downloaded JSON
- task status (`Starting`, `DownloadingJson`, `Ready`, `Updating`, `Error`)

### The web/application layer owns

- `WebServer`
- HTTP argument parsing and HTTP status codes
- config persistence in `Config`
- aggregating streamed queue events into HTTP responses where needed
- streaming persisted files returned by a final `FileReady` event where needed
- user-visible text formatting

The key rule is:

> no code outside `oi_can_task` should call `twai_receive()` or `twai_transmit()`

## Queue Contract

Use exactly one request queue and one response queue.

The response queue should be treated as an event stream, not as a one-request-one-response mailbox.

The task should internally separate:

- queued requests: accepted from callers but not yet sent on CAN
- active requests: already put on the bus and waiting for a reply or timeout
- completed requests: finished and already reported through the response queue

That means the task can monitor many request deadlines at once, even if the dispatch window onto the CAN bus is smaller than the request queue depth.

Recommended mechanics:

- `g_oiCanRequestQueue`: caller -> CAN task
- `g_oiCanResponseQueue`: CAN task -> caller
- `g_oiCanApiMutex`: first-cut option if one caller submits a burst and drains the shared response queue

If true multi-caller concurrency is needed later, add one of these:

- `clientTag` inside `Request` and `Response` so callers can demultiplex events on the shared response queue
- a registration layer that maps requests to caller-owned mailboxes while keeping one internal CAN task

This is still a queue-based interface, but it avoids response mix-ups between tasks and allows the task to report incremental progress.

Response lifecycle:

1. caller sends one `Request` with a unique `sequence`
2. CAN task emits zero or more non-terminal events for that `sequence`
3. CAN task emits exactly one terminal event for that `sequence`
4. caller keeps draining the response queue until that terminal event arrives

Examples:

- `StreamValues` request:
  - `Accepted`
  - `Value`
  - `Value`
  - `Value`
  - `Completed`
- `DownloadSchemaJson` request:
  - `Accepted`
  - `Progress("started", 0, totalBytes)`
  - `Progress("downloading", 100, totalBytes)`
  - `Progress("downloading", 200, totalBytes)`
  - `FileReady("/12345678.json")`
  - `Completed`

## Scheduler, Active Table, And Bus Limits

Inside `oi_can_task`, add a scheduler that drains the request queue, maintains a bounded active table, and decides when another CAN request may be sent.

Recommended internal limits:

- `MAX_QUEUED_REQUESTS`: maximum requests accepted into the task backlog
- `MAX_ACTIVE_REQUESTS`: maximum requests tracked as on-bus and awaiting reply
- `MAX_TX_PER_CYCLE`: upper bound for new CAN requests sent in one scheduler pass
- `MIN_INTERFRAME_GAP_US`: minimum spacing between request frames
- `MAX_REQUEST_RETRIES`: retries before terminal failure
- `MAX_CAN_UTILIZATION_PERCENT`: software throttle target for request traffic

Recommended internal request states:

- `Queued`
- `Dispatched`
- `WaitingReply`
- `RetryDelay`
- `Completed`
- `TimedOut`
- `Aborted`

Suggested internal active-table shape:

```cpp
struct ActiveRequestEntry {
  bool inUse;
  uint32_t sequence;
  Command command;
  uint8_t nodeId;
  uint16_t index;
  uint8_t subIndex;
  TickType_t queuedAt;
  TickType_t sentAt;
  TickType_t deadlineAt;
  TickType_t retryAt;
  uint8_t retryCount;
  bool responseStarted;
};
```

Scheduler loop responsibilities:

1. dequeue newly arrived API requests into an internal backlog
2. move dispatchable requests from backlog to the active table while limits allow
3. transmit CAN frames subject to spacing and utilization limits
4. match incoming replies to active entries
5. emit response events immediately on match
6. mark deadlines and retries for entries that do not respond in time
7. emit terminal `Failed` events for timed-out entries

### Important Protocol Caveat

The active table may track many requests, but the number of requests that should be placed on the CAN bus at once depends on protocol capability.

For this project, the safe default should be:

- request queue depth may be large, for example 20 or 32
- timeout monitoring may cover all queued/active entries
- on-bus SDO dispatch window should default to `1` per node on the standard SDO channel

Reason:

- current SDO matching has no independent transaction id
- request and response COB-IDs are shared per node
- many CANopen SDO servers assume one active transfer per channel

So the task should not blindly blast 20 `GetValue` requests onto the default SDO channel unless the target protocol is explicitly verified to support that behavior.

If later the target supports:

- multiple SDO channels, or
- a custom request/reply protocol with reliable correlation,

then `MAX_ACTIVE_REQUESTS` and the dispatch window may be increased above `1`.

### Practical Interpretation Of "20 GetValue Requests"

If 20 `GetValue` requests are submitted together, the task should:

1. accept all 20 into its internal backlog
2. track timeout metadata for every accepted request
3. dispatch as many as the protocol-safe active window allows
4. emit each value as soon as its reply arrives
5. backfill the next queued request when one active slot completes or times out

This still gives overlapping queue management and deadline monitoring, while keeping CAN bus use bounded and protocol-safe.

## Queue Payload Rules

Queue items should be plain fixed-size structs only.

Do not put these into queue payloads:

- `String`
- `JsonDocument`
- `WebServer&`
- `File`
- raw heap pointers that transfer ownership implicitly

Use:

- fixed-size `char[]`
- integers
- enums
- small POD structs
- explicit event kinds and terminal markers

## Suggested Public Types

```cpp
namespace OICanTask {

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
  char stage[24];
  uint32_t current;
  uint32_t total;
};

struct ValuePayload {
  char name[32];
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
  char path[32];
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
      char name[32];
    } getValue;

    struct {
      char name[32];
      float value;
    } setValue;

    struct {
      uint16_t samples;
      char namesCsv[192];
    } streamValues;

    struct {
      char json[256];
    } mapJson;

    struct {
      uint16_t index;
      uint8_t subIndex;
    } removeMap;

    struct {
      int opmode;
    } startStop;

    struct {
      char fileName[64];
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
    struct {
      float value;
    } scalar;
  } data;
};

} // namespace OICanTask
```

## Streaming First, Files Where They Actually Help

Bulk and iterative commands should not stay silent until all work is complete. The task should emit progress or item events as work happens.

This is the intended behavior for the key operations:

- `ReadLiveSnapshot`: emit one `Value` event per parameter as each SDO reply arrives
- `ReadCanMap`: emit one `MappingItem` event per discovered mapping entry
- `StreamValues`: emit one `Value` event per sample/value pair
- `DownloadSchemaJson`: emit `Progress` events while the JSON file is being received and stored
- many individual `GetValue` requests may be queued and tracked together, but bus dispatch still obeys the active-window and load limits

SPIFFS is still useful, but for a narrower purpose:

- the downloaded schema JSON should be written by the task directly to SPIFFS
- firmware update input remains a SPIFFS file opened by the task
- the task may optionally emit a final `FileReady` event for persisted artifacts

So the rule should be:

> live data flows over the response queue; durable artifacts are stored in SPIFFS by the task

## Recommended Commands

### `Reconfigure`

Replaces `Init()`.

Request:

- node id
- baud rate
- tx/rx pins

Task behavior:

- stop/uninstall TWAI if needed
- install/start driver
- clear internal state
- start serial discovery
- download JSON if missing

Response:

- immediate `Ok` if the driver was started
- `GetStatus` reports later readiness

### `GetStatus`

Replaces the need to call `Loop()` from the application layer.

Response should report:

- current task state
- whether schema is available
- update progress
- current node id / baud
- a compact serial identifier

### `DownloadSchemaJson`

Replaces the startup-only hidden behavior with an explicit command.

Task behavior:

- validate existing schema file
- if missing, invalid, or forced, start SDO segmented upload
- write directly to `/<serial>.json` in SPIFFS
- emit progress while bytes are being written

Response sequence:

- `Accepted`
- `Progress("started", 0, totalBytesOrZeroIfUnknown)`
- `Progress("downloading", currentBytes, totalBytesOrZeroIfUnknown)`
- `FileReady(path, size)` when closed successfully
- `Completed`

This is the queue-safe replacement for the current "delete invalid JSON and retry startup" behavior.

### `ReadLiveSnapshot`

Replaces the core behavior behind `SendJson(WebServer&)`, but in streamed form.

Task behavior:

- require `Ready`
- walk the cached schema
- enqueue the required SDO reads into the internal scheduler
- emit one `Value` response as soon as each value is available

Response sequence:

- `Accepted`
- `Value(name="...", paramId=..., value=...)`
- `Value(...)`
- `Completed`

If the current HTTP endpoint still wants one JSON document, the web layer can aggregate these events into JSON locally. The task should not have to gather the whole snapshot before replying.

### `ReadCanMap`

Replaces `SendCanMapping(WebServer&)` in streamed form.

Task behavior:

- run the current multi-step mapping walk
- emit each mapping entry immediately when decoded

Response sequence:

- `Accepted`
- `MappingItem(...)`
- `MappingItem(...)`
- `Completed`

### `StreamValues`

Replaces `StreamValues(String names, int samples)` without building a full CSV first.

Task behavior:

- parse `namesCsv`
- resolve IDs from the in-RAM cache
- enqueue and schedule the required reads per sample
- emit one `Value` event for each sample/value pair as soon as it is received

Response sequence:

- `Accepted`
- `Value(sampleIndex=0, name="...", value=...)`
- `Value(sampleIndex=0, name="...", value=...)`
- `Value(sampleIndex=1, name="...", value=...)`
- `Completed`

### `GetValue` / `SetValue`

Replaces `GetValue()` and `SetValue()`.

Task behavior:

- resolve parameter name through cached `name -> id`
- enqueue one request into the scheduler
- allow many independent `GetValue` requests to be pending together, subject to active-window and bus-load limits

Response:

- scalar value for `GetValue`
- result code for `SetValue`

### `AddCanMapping` / `RemoveCanMapping`

Replaces the current mapping mutators.

For `RemoveCanMapping`, prefer sending structured `index/subIndex` instead of reusing JSON. For `AddCanMapping`, short JSON is still acceptable as a first step because the current web handler already has that format.

Longer term, a typed payload would be better than JSON in the queue.

### `SaveToFlash`

Replaces `SaveToFlash()`.

One SDO write request, one result.

### `StartStop`

Replaces `StartStop(int opmode)`.

Request:

- `opmode == 0` means stop
- non-zero means start

### `StartFirmwareUpdate`

Replaces `StartUpdate()`.

Task behavior:

- open the update file from SPIFFS
- send reset command
- own the full update exchange with the bootloader
- keep update progress in task state

Response:

- immediate `Ok` plus total pages if known

### `GetUpdateStatus`

Replaces `GetCurrentUpdatePage()` plus the manual `while (...) OICan::Loop();` polling in the HTTP handler.

## Task State Machine

Suggested high-level states:

1. `Stopped`
2. `Starting`
3. `DownloadingJson`
4. `Ready`
5. `Updating`
6. `Error`

Important behavior:

- background frame reception is always inside the CAN task loop
- request processing is only allowed in `Ready`, except for `GetStatus`, `DownloadSchemaJson`, `Reconfigure`, and update-related commands
- if schema validation fails, the task returns to `DownloadingJson`
- update mode temporarily blocks normal SDO requests and returns `Busy`
- queued requests may outnumber active on-bus requests
- timeout handling is driven from the internal active table, not from a blocking wait loop

## Mapping From Current API To Future Queue Commands

| Current API | Future command |
|---|---|
| `Init()` | `Reconfigure` |
| `Loop()` | internal task loop only |
| startup JSON download | `DownloadSchemaJson` |
| `SendJson()` | `ReadLiveSnapshot` |
| `SendCanMapping()` | `ReadCanMap` |
| `AddCanMapping()` | `AddCanMapping` |
| `RemoveCanMapping()` | `RemoveCanMapping` |
| `SetValue()` | `SetValue` |
| `GetValue()` | `GetValue` |
| `SaveToFlash()` | `SaveToFlash` |
| `StartStop()` | `StartStop` |
| `StreamValues()` | `StreamValues` |
| `StartUpdate()` | `StartFirmwareUpdate` |
| `GetCurrentUpdatePage()` | `GetUpdateStatus` |

## Recommended Implementation Order

1. Create `src/oi_can_task.h` with enums, request/response structs, queue declarations, and thin synchronous wrapper helpers.
2. Create `src/oi_can_task.cpp` with a dedicated FreeRTOS task and move all TWAI RX/TX access into it.
3. Move the current startup serial discovery and JSON download state machine from `Loop()` into the task loop.
4. Add an internal request scheduler with:
   - backlog queue
   - bounded active table
   - per-entry deadline/retry handling
   - configurable dispatch window and transmit pacing
5. Add an in-RAM parameter index cache after schema download so `getId()` no longer reopens SPIFFS per request.
6. Implement multi-response delivery:
   - `ReadLiveSnapshot` emits `Value` items
   - `ReadCanMap` emits `MappingItem` items
   - `DownloadSchemaJson` emits `Progress` events
   - queued `GetValue` requests complete independently as replies arrive
7. Update `esp32-web-interface.ino` handlers to:
   - submit a queue request
   - keep draining responses until `terminal == true`
   - aggregate queue events into HTTP output only where the HTTP API still needs one payload
8. Move firmware update polling to `GetUpdateStatus` and remove the `while (...) OICan::Loop();` busy wait.

## Suggested First-Cut Wrapper API

Even with queues underneath, the rest of the application can stay simple if `oi_can_task.h` offers thin helpers like:

```cpp
using ResponseCallback = bool (*)(const Response& event, void* context);

bool StartTask();
Result SubmitAndDrain(const Request& request,
                      ResponseCallback callback,
                      void* context,
                      TickType_t waitTicks);
bool Reconfigure(uint8_t nodeId, BaudRate baud, int txPin, int rxPin);
Result GetStatus(StatusPayload& status);
Result DownloadSchemaJson(bool forceRedownload, ResponseCallback callback, void* context);
Result ReadLiveSnapshot(ResponseCallback callback, void* context);
Result ReadCanMap(ResponseCallback callback, void* context);
Result SetValue(const char* name, float value);
Result GetValue(const char* name, float& value);
```

Those helpers should do nothing except:

- take `g_oiCanApiMutex`
- assign a sequence number
- send one request queue item
- keep reading response queue items until `terminal == true`
- verify the sequence number on every event

## Summary

The most reasonable first FreeRTOS split is not "one queue per SDO frame". It is "one queue per operation", with the CAN task owning all frame-level details internally and emitting progress/data events as work happens.

That keeps the public boundary small, preserves the current behavior, supports live progressive responses, and gives the task a proper scheduler for queued requests, timeout tracking, bounded concurrency, and CAN bus load control without letting arbitrary callers touch the TWAI RX queue directly.
