# Architecture

This document explains how the project is structured, how the firmware and web application interact, and why responsibilities are split across the current files.

## Table of Contents

- [1. System Overview](#1-system-overview)
- [2. Repository Structure](#2-repository-structure)
- [3. Runtime Model](#3-runtime-model)
- [4. Firmware Architecture](#4-firmware-architecture)
- [5. Frontend Architecture](#5-frontend-architecture)
- [6. End-to-End Data Flows](#6-end-to-end-data-flows)
- [7. Persistence and Generated Artifacts](#7-persistence-and-generated-artifacts)
- [8. File Responsibility Map](#8-file-responsibility-map)
- [9. Separation Principles Used in This Project](#9-separation-principles-used-in-this-project)
- [10. Where to Change Code for Common Tasks](#10-where-to-change-code-for-common-tasks)

## 1. System Overview

The project is a two-part system:

1. ESP32 firmware in `src/`
2. A Preact single-page web app in `web/`

At runtime the ESP32 does four jobs:

- joins WiFi or starts its own access point
- hosts the compiled web app from LittleFS
- exposes a REST surface for a small set of configuration and file endpoints
- exposes a WebSocket control channel for live CAN operations and status updates

The web app does three jobs:

- renders the UI for device discovery, monitoring, parameter editing, CAN mapping, CAN message sending, and OTA
- keeps a persistent WebSocket connection open to the ESP32
- treats the ESP32 as the gateway to OpenInverter devices on the CAN bus

The most important architectural idea is that the firmware is split by execution context:

- Async web callbacks accept HTTP and WebSocket requests
- a dedicated CAN FreeRTOS task owns the CAN bus, command execution, and most state machines
- the main Arduino loop converts queued backend events into outbound WebSocket messages

That split keeps CAN activity out of the web callback stack and makes the browser-facing layer mostly a translation layer.

## 2. Repository Structure

High-level layout:

```text
.
|- src/                 ESP32 firmware
|  |- managers/         long-lived stateful services
|  |- models/           shared command/event/data structures
|  |- protocols/        SDO protocol helpers
|  |- firmware/         remote device firmware update logic
|  |- utils/            stateless transport and formatting helpers
|  |- main.cpp          firmware bootstrap and top-level orchestration
|  |- can_task.cpp      dedicated CAN task
|  |- websocket_handlers.cpp
|  |- http_handlers.cpp
|  `- event_processor.cpp
|- web/                 Preact SPA
|  |- src/
|  |  |- api/           browser-side API wrapper
|  |  |- contexts/      app-wide state containers
|  |  |- hooks/         async and stateful UI workflows
|  |  |- pages/         route-level screens
|  |  |- components/    reusable UI units
|  |  |- utils/         browser-side helpers and local cache helpers
|  |  `- styles/        theme and shared CSS
|  `- vite.config.js    dev proxy and production build output to data/dist
|- data/                LittleFS content uploaded to the ESP32
|- platformio.ini       firmware build configuration
|- partitions.csv       flash partition layout
`- README.md            project entry documentation
```

The built frontend is written into `data/dist`, then uploaded as the ESP32 filesystem image with `pio run -t uploadfs`.

## 3. Runtime Model

### 3.1 Execution contexts

There are three main execution contexts in the firmware:

| Context | File(s) | Responsibility |
| --- | --- | --- |
| Arduino setup and main loop | `src/main.cpp` | boot, initialize services, run periodic top-level processing |
| Dedicated CAN task | `src/can_task.cpp` | own CAN TX/RX, connection state machine, scanning, spot values, interval messages |
| Async network callbacks | `src/http_handlers.cpp`, `src/websocket_handlers.cpp` | receive browser requests without directly owning CAN state |

### 3.2 Queues between contexts

The key queues are:

| Queue | Defined in | Used by | Purpose |
| --- | --- | --- | --- |
| `canCommandQueue` | `src/main.cpp` | WebSocket handlers -> CAN task | browser requests become backend work items |
| `canEventQueue` | `src/main.cpp` | CAN task/managers -> main loop | backend events become browser-facing messages |
| `canTxQueue` | `src/can_task.cpp` | protocol/helpers -> CAN task | decouples SDO/request code from direct TWAI transmission |
| `sdoResponseQueue` | `src/can_task.cpp` | CAN task -> protocol/state machines | filtered SDO responses are handed to higher-level logic |

This queue split is the backbone of the firmware design:

- Web callbacks do not directly drive TWAI.
- CAN logic does not directly serialize browser payloads.
- The main loop only publishes already-formed backend events.

### 3.3 Protocol split

Communication is intentionally divided by purpose:

- HTTP is used for coarse-grained operations like `/settings`, `/devices`, `/version`, static file serving, and OTA upload start.
- WebSocket is used for interactive operations like scanning, connecting, streaming spot values, parameter fetches, parameter writes, CAN mappings, and live status.
- CAN/SDO is used underneath the ESP32 to talk to OpenInverter devices.

### 3.4 Browser-to-device request path

A typical command path looks like this:

1. A component calls a hook or context action.
2. The hook/context sends a WebSocket action.
3. `src/websocket_handlers.cpp` validates the JSON and either:
   - queues a `CANCommand`, or
   - calls a synchronous `OICan` helper for direct SDO-backed operations.
4. `src/can_task.cpp` processes queued commands and advances state machines.
5. Managers and protocol helpers emit `CANEvent` objects.
6. `src/event_processor.cpp` serializes those events.
7. The browser receives WebSocket events and updates context or hook state.

## 4. Firmware Architecture

### 4.1 Boot and top-level orchestration

`src/main.cpp` is the firmware composition root. It does not own business logic; it wires the subsystems together.

Startup responsibilities in `main.cpp`:

- initialize serial logging
- initialize the status LED
- mount LittleFS
- configure WiFi and mDNS
- load EEPROM-backed settings
- configure CAN transceiver control pins
- initialize the CAN bus
- create the command and event queues
- register discovery and connection callbacks
- create the CAN task
- attach WebSocket and HTTP handlers
- start OTA and the web server

The `loop()` function stays intentionally small:

- `ws.cleanupClients()`
- `ArduinoOTA.handle()`
- `EventProcessor::processEvents(ws)`
- `EventProcessor::processFirmwareProgress(ws)`

This is an important separation rule in the project: `main.cpp` is orchestration, not application logic.

### 4.2 Web entry layer

#### `src/http_handlers.cpp`

This file owns traditional request/response HTTP handling:

- `handleVersion()` exposes a firmware version string
- `handleDevices()` returns the saved/in-memory device list
- `handleSettings()` reads and updates EEPROM-backed configuration
- `handleOtaUpload()` stores the uploaded firmware binary in LittleFS and starts the CAN-side firmware update
- `handleFileRequest()` serves the built SPA and other filesystem assets
- `registerHttpRoutes()` binds routes to the server

Why it is separate:

- HTTP endpoints are mostly stateless and request/response oriented.
- Serving files and settings does not belong in the CAN task.

#### `src/websocket_handlers.cpp`

This file is the command gateway for live operations. It translates browser actions into backend work.

Main responsibilities:

- track client connection and disconnect lifecycle
- send initial state snapshots when a WebSocket client connects
- dispatch inbound JSON by `action`
- enforce client/device locking
- queue `CANCommand` work for the CAN task
- call `OICan` helpers for direct synchronous SDO operations
- send targeted success/error responses when an action does not fit the general event queue flow

Representative handlers:

- discovery and connection: `handleStartScan()`, `handleStopScan()`, `handleConnect()`, `handleDisconnect()`
- device metadata: `handleSetDeviceName()`, `handleRenameDevice()`, `handleDeleteDevice()`
- node and parameter operations: `handleGetNodeId()`, `handleSetNodeId()`, `handleGetParamSchema()`, `handleGetParamValues()`, `handleGetParamValuesOnly()`, `handleUpdateParam()`, `handleReloadParams()`
- monitoring and control: `handleStartSpotValues()`, `handleStopSpotValues()`, `handleSendCanMessage()`, `handleStartCanInterval()`, `handleStopCanInterval()`, `handleStartCanIoInterval()`, `handleStopCanIoInterval()`, `handleUpdateCanIoFlags()`
- device commands: `handleSaveToFlash()`, `handleLoadFromFlash()`, `handleLoadDefaults()`, `handleStartDevice()`, `handleStopDevice()`, `handleResetDevice()`, `handleListErrors()`
- mapping management: `handleGetCanMappings()`, `handleAddCanMapping()`, `handleRemoveCanMapping()`

Why it is separate:

- it keeps JSON parsing and browser protocol concerns away from the CAN task
- it is the right place for multi-client policy such as `ClientLockManager`
- it gives the web API a single translation layer

#### `src/event_processor.cpp`

This file is the outbound mirror of `websocket_handlers.cpp`.

Main responsibilities:

- pull `CANEvent` items from `canEventQueue`
- serialize event payloads into JSON
- broadcast normal events to all WebSocket clients
- send `EVT_JSON_READY` responses to the specific requesting client
- merge the latest spot values into a full parameter payload before sending large parameter data
- publish firmware update progress and completion events

Why it is separate:

- the CAN task emits strongly typed backend events
- browser JSON serialization stays in one file
- targeted delivery rules stay out of the managers

### 4.3 CAN execution layer

#### `src/can_task.cpp`

This file owns the dedicated FreeRTOS CAN task and is the heart of backend execution.

Responsibilities:

- initialize `canTxQueue` and `sdoResponseQueue`
- read and dispatch `CANCommand` items
- send queued CAN frames to TWAI
- receive TWAI frames and route them
- run periodic services every task iteration
- drive the `DeviceConnection` and `DeviceDiscovery` state machines
- check pending async parameter write timeouts
- coordinate firmware update state transitions

Its internal functions are grouped by concern:

- command handlers such as `handleConnectCommand()` and `handleStartSpotValuesCommand()`
- periodic helpers such as `processSpotValuesSequence()`
- dispatch logic in `dispatchCommand()`
- TWAI filter setup in `initCanBusScanning()` and `initCanBusForDevice()`
- transport helpers such as `processTxQueue()` and `receiveAndProcessCanMessages()`

This split matters because `can_task.cpp` is the only place that continuously touches:

- TWAI receive/transmit
- command queue consumption
- background manager state machines

### 4.4 Manager layer

The `src/managers/` directory contains long-lived stateful services. These files hold policy and state, not HTTP/WebSocket parsing and not low-level frame packing.

#### `src/managers/device_connection.*`

Purpose:

- own the active device connection state
- keep the currently selected node ID and baud settings
- run the non-blocking serial acquisition and JSON download state machines
- cache parameter JSON in memory
- remember which WebSocket client requested a parameter download

Important behaviors:

- `connectToDevice()` switches CAN filtering to a specific node and starts serial acquisition
- `processConnection()` advances the serial and JSON download state machine
- `startJsonDownloadAsync()` begins a targeted async parameter fetch for one browser client
- `resetToScanningMode()` restores accept-all scanning filters after disconnect

Why it is separate:

- connection state is cross-cutting and long-lived
- JSON download is not just transport, it is a state machine with retries, timeouts, mutex-protected buffers, and client targeting

#### `src/managers/device_discovery.*`

Purpose:

- own active scan state
- probe node IDs by requesting serial number parts
- maintain the in-memory device list
- publish discovery and scan progress callbacks
- persist names and known node IDs through `devices.json`

Important behaviors:

- `startContinuousScan()` / `stopContinuousScan()`
- `processScan()` non-blocking scan state machine
- `addOrUpdateDevice()` and `updateLastSeenByNodeId()`
- `saveDeviceName()` and `deleteDevice()`
- `getSavedDevices()` for browser snapshots

Why it is separate:

- scanning and passive heartbeat tracking have different timing and persistence rules than connection handling

#### `src/managers/spot_values_manager.*`

Purpose:

- manage the list of monitored parameter IDs
- reload and drain a request queue at the configured interval
- batch spot-value responses into a single event payload
- keep the latest value map for later parameter payload merging

Important behaviors:

- `start()`, `stop()`, `pause()`, `resume()`
- `reloadQueue()` and `processQueue()`
- `isWaitingForParam()` and `handleResponse()`
- `flushBatch()`

Why it is separate:

- spot-value streaming has its own cadence and batching policy
- it must survive independently of parameter page rendering logic

#### `src/managers/can_interval_manager.*`

Purpose:

- manage periodic generic CAN frames
- manage the specialized CAN IO control message stream
- track interval configuration and last-sent timestamps

Important behaviors:

- `startInterval()` / `stopInterval()` / `clearAllIntervals()`
- `sendPendingMessages()`
- `startCanIoInterval()` / `stopCanIoInterval()` / `updateCanIoFlags()`
- `sendCanIoMessage()`

Why it is separate:

- periodic message scheduling is a service, not a one-off request

#### `src/managers/client_lock_manager.*`

Purpose:

- ensure only one WebSocket client controls a device at a time

Important behaviors:

- `tryAcquireLock()`
- `releaseLock()`
- `releaseClientLocks()`
- query helpers for lock ownership

Why it is separate:

- it is a pure multi-client policy service and should stay independent from transport or device logic

#### `src/managers/device_storage.*`

Purpose:

- isolate filesystem operations for `devices.json` and per-device JSON cache files

Important behaviors:

- `loadDevices()`
- `saveDevices()`
- `updateDeviceInJson()`
- `hasJsonCache()`, `removeJsonCache()`, `getJsonFileName()`

Why it is separate:

- storage access should not be duplicated in discovery or connection code

#### `src/managers/device_cache.*`

Purpose:

- cache `devices.json` in memory
- provide efficient name lookup by serial during event serialization and discovery callbacks

Why it is separate:

- name lookup is common and should not re-open LittleFS on every event

### 4.5 Protocol and command layer

#### `src/oi_can.*`

This namespace is the higher-level OpenInverter CAN API. It exposes operations in terms the rest of the firmware understands.

Examples:

- bus setup: `InitCAN()`, `Init()`
- parameter access: `RequestValue()`, `SetValue()`, `TryGetValueResponse()`
- JSON and schema related access: `GetRawJson()`, `ReloadJson()`
- device commands: `SaveToFlash()`, `LoadFromFlash()`, `LoadDefaults()`, `StartDevice()`, `StopDevice()`, `ResetDevice()`
- CAN mapping: `GetCanMapping()`, `AddCanMapping()`, `RemoveCanMapping()`, `ClearCanMap()`
- logging and streaming: `ListErrors()`, `StreamValues()`
- firmware update: `StartUpdate()`
- discovery helpers: `ScanDevices()`, `StartContinuousScan()`

Why it is separate:

- it hides raw SDO details from handlers and managers
- it is the domain API for OpenInverter-specific operations

#### `src/protocols/sdo_protocol.*`

This is the lower-level SDO transport helper layer.

Responsibilities:

- build SDO requests and writes
- wait on `sdoResponseQueue`
- expose request/write helper combinations
- track pending async writes and match later responses

Why it is separate:

- protocol framing rules should not be mixed into business logic or web handlers

#### `src/firmware/update_handler.*`

Purpose:

- implement the bootloader/CAN firmware update state machine for the remote device

Responsibilities:

- segment the uploaded firmware file into pages
- send bootloader protocol frames
- verify progress and completion
- surface percent-complete updates back to `EventProcessor`

### 4.6 Shared data and utility layer

#### `src/models/`

The model files define the strongly typed contract between layers.

- `can_types.h`: constants, enums, CAN IDs, baud rates, result enums
- `can_command.h`: payloads for all queued commands sent to the CAN task
- `can_event.h`: payloads for all queued events emitted back to the main loop
- `interval_messages.h`: in-memory data structures for scheduled CAN traffic

Why they are separate:

- queue producers and consumers need a shared schema
- these files should stay free of runtime policy

#### `src/utils/`

These files hold stateless helpers or very narrow transport helpers:

- `can_queue.h`: inline queue wrappers for CAN TX and SDO response handling
- `can_utils.*`: CAN frame debug logging and response validation helpers
- `can_io_utils.*`: bit packing for CAN IO control frames
- `can_hardware.*`: board-specific transceiver control pin initialization
- `string_utils.h`: safe string copy helpers
- `request_id.h`: request ID helper definitions
- `websocket_helpers.h`: targeted WebSocket error helpers, large payload send helpers, and debug instrumentation

#### Other firmware support files

- `config.*`: EEPROM-backed CAN pins, CAN speed, and scan range settings
- `wifi_setup.*`: station/AP setup and `/wifi.txt` loading
- `status_led.*`: NeoPixel-based status feedback

## 5. Frontend Architecture

### 5.1 Frontend composition model

The frontend follows this split:

- `pages/` own route-level orchestration
- `contexts/` own long-lived app state
- `hooks/` own asynchronous protocol-heavy workflows
- `components/` render reusable UI pieces
- `utils/` hold stateless browser helpers
- `*.content.ts` files hold localized text definitions
- CSS is split between global theme files and component-local style files

This keeps rendering separate from protocol handling and browser storage concerns.

### 5.2 App bootstrap and routing

#### `web/src/main.tsx`

This is the frontend composition root. It wraps the app in:

- `IntlayerProvider`
- `ToastProvider`
- `WebSocketProvider`
- `DeviceProvider`

It also registers the PWA service worker.

#### `web/src/App.tsx`

Responsibilities:

- configure route switching with `wouter`
- connect the global API helper to the active WebSocket sender
- route to:
  - `/` -> `SystemOverview`
  - `/settings` -> `Settings`
  - `/devices/:serial` -> `DeviceDetails`

### 5.3 Shared state containers

#### `web/src/contexts/WebSocketContext.tsx`

Purpose:

- own the browser WebSocket connection
- reconnect automatically
- expose `sendMessage()` and `subscribe()`
- translate raw socket messages into a subscription bus

Why it is separate:

- there should be exactly one shared socket, not one per page/component

#### `web/src/contexts/DeviceContext.tsx`

Purpose:

- own the system-level device list and scan state
- merge saved device records with live discovery data
- expose scanning and device management actions to pages
- remember the last connected device in browser storage

Why it is separate:

- system overview, sidebar, and page navigation all need the same device state

#### `web/src/contexts/DeviceDetailsContext.tsx`

Purpose:

- own device-detail session state that should survive tab switches inside the device page

State groups inside this context:

- cached parameter payload
- live monitoring and chart history
- CAN message form state and scheduled message UI state
- CAN IO control state

Why it is separate:

- the device page has many tabs that should share one session state model

### 5.4 Hooks and async workflow layer

#### `web/src/hooks/useParamSchema.ts`

Purpose:

- fetch only parameter schema metadata, preferably from local browser cache first
- ask the ESP32 for cached schema
- fall back to a background full parameter download if needed

This hook exists so the UI can begin rendering parameter labels before a full values fetch completes.

#### `web/src/hooks/useParams.ts`

Purpose:

- fetch full parameter values, preferably using schema-aware optimization
- merge values-only payloads into a cached schema
- track pending requests, progress, timeouts, and cancellation
- keep the detail page cache synchronized

This is one of the highest-value separation points in the frontend because it keeps complex WebSocket request bookkeeping out of the UI components.

#### `web/src/hooks/useCanMappings.ts`

Purpose:

- wrap the CAN mapping request/response cycle behind a simple hook API

#### Other hooks

- `useToast.ts`: toast access helper
- `useSwipeGesture.ts`: mobile interaction helper

### 5.5 Pages

#### `web/src/pages/SystemOverview/index.tsx`

Responsibilities:

- auto-start scanning after connection and settings load
- show the device scanner and device list
- launch naming/renaming flows
- navigate into a selected device

#### `web/src/pages/DeviceDetails/index.tsx`

Responsibilities:

- connect to the selected device
- coordinate the detail-level contexts and hooks
- assemble tabs for monitoring, parameters, CAN mappings, CAN messages, and OTA
- stop streaming, CAN IO, and device locks when leaving the page

#### `web/src/pages/Settings/index.tsx`

Responsibilities:

- load settings through REST
- edit CAN pins, speed, and scan range
- persist them back to `/settings`

### 5.6 Reusable components

The components directory is large, but the split is consistent:

- shell/navigation: `Layout`, `Sidebar`, `Tabs`, `LanguageSelector`
- status/feedback: `ConnectionStatus`, `DisconnectedState`, `LoadingSpinner`, `ProgressBar`, `Toast`
- discovery/system overview: `DeviceScanner`, `DeviceListItem`, `DeviceNaming`
- monitoring and charts: `SpotValuesMonitor`, `LineChart`, `MultiLineChart`
- parameter editing: `DeviceParameters`, `ParameterCategory`, `ParameterInput`
- CAN-specific tools: `CanMappingEditor`, `CanMessageSender`, `CanIoControl`
- update flow: `OTAUpdate`

A common local pattern is:

- `index.tsx` contains the component implementation
- `styles.css` contains local styling if needed
- `*.content.ts` contains localized text for that component or page

### 5.7 Browser-side utility layer

Important files:

- `api/inverter.ts`: typed REST and WebSocket-trigger helper surface
- `utils/paramStorage.ts`: localStorage schema cache
- `utils/lastDevice.ts`: remember last connected device
- `utils/parameterDisplay.ts`: formatting and enum label helpers
- `utils/spotValueConversions.ts`: display conversions for live values
- `utils/deviceSort.ts`: presentation sorting

### 5.8 Build and delivery path

`web/vite.config.js` is an important architecture file because it defines:

- dev-server proxying of HTTP and WebSocket traffic to `inverter.local`
- the production build output path `../data/dist`
- PWA registration and caching behavior
- path aliases like `@components`, `@hooks`, `@contexts`, and `@api`

`web/package.json` defines the browser build and typecheck commands.

## 6. End-to-End Data Flows

### 6.1 Device discovery flow

1. `SystemOverview` triggers `DeviceContext.startScan()`.
2. `DeviceContext` sends `startScan` over WebSocket.
3. `websocket_handlers.cpp` queues `CMD_START_SCAN`.
4. `can_task.cpp` calls `OICan::StartContinuousScan()`.
5. `DeviceDiscovery` begins probing node IDs.
6. Discovery callbacks push `EVT_DEVICE_DISCOVERED` and `EVT_SCAN_PROGRESS`.
7. `EventProcessor` broadcasts those events.
8. `DeviceContext` merges them into the UI state.

### 6.2 Device connect and parameter load flow

1. `DeviceDetails` sends `connect`.
2. `websocket_handlers.cpp` acquires a client lock and queues `CMD_CONNECT`.
3. `can_task.cpp` stops scanning/spot-values if needed and reinitializes CAN for the selected node.
4. `DeviceConnection` acquires the device serial through its non-blocking state machine.
5. `DeviceConnection` emits a ready callback which becomes `EVT_CONNECTED`.
6. The page then requests parameter schema or parameter values.
7. `DeviceConnection::startJsonDownloadAsync()` runs the segmented SDO JSON download.
8. `EventProcessor` sends `paramValuesData` only to the requesting client.
9. `useParamSchema()` or `useParams()` resolves the waiting request and updates context state.

### 6.3 Spot values flow

1. `SpotValuesMonitor` requests `startSpotValues`.
2. `websocket_handlers.cpp` queues `CMD_START_SPOT_VALUES`.
3. `SpotValuesManager` stores the parameter list and interval.
4. On each cycle the CAN task reloads the request queue.
5. CAN responses are routed back to `SpotValuesManager::handleResponse()`.
6. Batched values become `EVT_SPOT_VALUES`.
7. `EventProcessor` broadcasts `spotValues`.
8. `SpotValuesMonitor` merges the latest values and updates historical charts.

### 6.4 Parameter write flow

1. `DeviceParameters` requests a parameter update through `api.setParamById()`.
2. `updateParam` reaches `websocket_handlers.cpp`.
3. The handler checks that the device is idle and that no async write is already pending.
4. Spot values are paused to avoid write/read collisions.
5. `SDOProtocol::setValueAsync()` records the pending write.
6. CAN RX routing in `can_task.cpp` matches the later response.
7. A `EVT_VALUE_SET` event is emitted.
8. `EventProcessor` serializes `paramUpdateResult`.
9. Frontend hooks/components update the displayed parameter state and can restart monitoring.

### 6.5 CAN mapping flow

1. `CanMappingEditor` uses `useCanMappings()`.
2. The hook sends `getCanMappings`, `addCanMapping`, or `removeCanMapping`.
3. `websocket_handlers.cpp` calls the relevant `OICan` helper directly.
4. `OICan` performs the SDO sequence against the current device.
5. A targeted WebSocket response is returned.
6. The hook refreshes local mapping state after successful writes.

### 6.6 OTA flow

1. The browser uploads a firmware binary to `/ota/upload`.
2. `http_handlers.cpp` stores it in LittleFS and calls `OICan::StartUpdate()`.
3. `FirmwareUpdateHandler` takes over remote flashing over CAN bootloader frames.
4. `EventProcessor::processFirmwareProgress()` emits `otaProgress` and `otaSuccess`.
5. `OTAUpdate` updates the UI progress bar.

## 7. Persistence and Generated Artifacts

### 7.1 On the ESP32

| Storage | File or medium | Used for |
| --- | --- | --- |
| EEPROM | `Config` struct | CAN pins, speed, scan range |
| LittleFS | `/wifi.txt` | station-mode credentials |
| LittleFS | `/devices.json` | saved devices, names, last known node IDs |
| LittleFS | `/firmware_update.bin` | temporary OTA upload staging file |
| LittleFS | `/dist/*` | compiled web app assets |
| LittleFS | per-device JSON cache filename | cached parameter JSON keyed from device serial |

### 7.2 In the browser

| Storage | File responsible | Used for |
| --- | --- | --- |
| `localStorage` | `web/src/utils/paramStorage.ts` | cached parameter schema |
| `localStorage` | `web/src/utils/lastDevice.ts` | last connected device |

### 7.3 Generated artifacts

| Artifact | Produced by | Consumed by |
| --- | --- | --- |
| firmware binary | PlatformIO | ESP32 flash |
| LittleFS image | PlatformIO + `data/` | ESP32 filesystem partition |
| `data/dist/app.js`, `app.css`, assets | Vite | `handleFileRequest()` |

## 8. File Responsibility Map

This section is the most direct answer to "how functions are separated into different files".

### 8.1 Firmware core files

| File | What belongs here | What should not belong here |
| --- | --- | --- |
| `src/main.cpp` | boot, composition, callback wiring, top-level loop calls | feature logic, protocol details, UI JSON shaping |
| `src/main.h` | global declarations and small inline helpers used across firmware | complex logic |
| `src/http_handlers.cpp` | REST endpoints, static file serving, OTA upload entry | CAN polling loops, long-lived state machines |
| `src/websocket_handlers.cpp` | browser action parsing, validation, lock checks, command creation | continuous CAN processing |
| `src/event_processor.cpp` | event-to-JSON serialization and WebSocket outbound delivery | CAN request generation |
| `src/can_task.cpp` | continuous CAN-side execution and command dispatch | browser routing, HTML/static responses |

### 8.2 Firmware managers

| File | State it owns | Functions separated into it |
| --- | --- | --- |
| `src/managers/device_connection.cpp` | current node, serial acquisition state, JSON download state, JSON buffers | `connectToDevice()`, `processConnection()`, `startJsonDownloadAsync()`, `resetToScanningMode()` |
| `src/managers/device_discovery.cpp` | scan progress, known device map, passive heartbeat throttling | `startContinuousScan()`, `processScan()`, `saveDeviceName()`, `deleteDevice()` |
| `src/managers/spot_values_manager.cpp` | monitored parameter IDs, request queue, current batch, latest values | `start()`, `processQueue()`, `handleResponse()`, `flushBatch()` |
| `src/managers/can_interval_manager.cpp` | scheduled generic CAN messages and CAN IO control interval state | `startInterval()`, `sendPendingMessages()`, `startCanIoInterval()`, `sendCanIoMessage()` |
| `src/managers/client_lock_manager.cpp` | node-to-client and client-to-node lock maps | `tryAcquireLock()`, `releaseLock()`, `releaseClientLocks()` |
| `src/managers/device_storage.cpp` | no long-lived in-memory state, only file access rules | `loadDevices()`, `saveDevices()`, `removeJsonCache()` |
| `src/managers/device_cache.cpp` | in-memory cached `devices.json` | `getDevices()`, `getDeviceName()`, `invalidate()` |

### 8.3 Firmware protocol and helper files

| File | Responsibility |
| --- | --- |
| `src/oi_can.cpp` | domain-level OpenInverter operations built on top of SDO |
| `src/protocols/sdo_protocol.cpp` | raw SDO request/response helpers and async write tracking |
| `src/firmware/update_handler.cpp` | remote bootloader flashing state machine |
| `src/config.cpp` | EEPROM-backed settings read/write |
| `src/wifi_setup.cpp` | WiFi STA/AP startup |
| `src/status_led.cpp` | LED state indication |
| `src/utils/can_queue.h` | queue wrappers for CAN TX and SDO RX |
| `src/utils/can_utils.cpp` | CAN debug logging and response checks |
| `src/utils/can_io_utils.cpp` | CAN IO frame bit packing |
| `src/utils/can_hardware.cpp` | transceiver enable/standby pin setup |
| `src/utils/websocket_helpers.h` | WebSocket error helpers, debug helpers, large payload send helper |

### 8.4 Frontend core files

| File | What belongs here |
| --- | --- |
| `web/src/main.tsx` | app bootstrap and providers |
| `web/src/App.tsx` | route table and top-level app setup |
| `web/src/api/inverter.ts` | typed browser-side gateway to HTTP endpoints and WebSocket-triggered operations |

### 8.5 Frontend contexts

| File | State it owns | Why it is separate |
| --- | --- | --- |
| `web/src/contexts/WebSocketContext.tsx` | connection lifecycle and socket subscription bus | only one socket should exist |
| `web/src/contexts/DeviceContext.tsx` | saved devices, scan state, connection navigation helpers | system-wide device state is shared by multiple pages/components |
| `web/src/contexts/DeviceDetailsContext.tsx` | detail-page session state for params, monitoring, CAN tools | tab content should share one device session |

### 8.6 Frontend hooks

| File | Workflow it owns |
| --- | --- |
| `web/src/hooks/useParamSchema.ts` | schema-first loading path with cache fallback |
| `web/src/hooks/useParams.ts` | full parameter download, values-only optimization, request timeout/cancel handling |
| `web/src/hooks/useCanMappings.ts` | CAN mapping request/response cycle |
| `web/src/hooks/useToast.ts` | toast API access |
| `web/src/hooks/useSwipeGesture.ts` | mobile swipe behavior |

### 8.7 Frontend pages

| File | Route-level responsibility |
| --- | --- |
| `web/src/pages/SystemOverview/index.tsx` | discovery dashboard and entry into device pages |
| `web/src/pages/DeviceDetails/index.tsx` | connected-device workspace and tab assembly |
| `web/src/pages/Settings/index.tsx` | board settings editor |
| `web/src/pages/*/*.content.ts` | localized page copy |

### 8.8 Frontend component groups

| Group | Files | Responsibility |
| --- | --- | --- |
| App shell | `Layout`, `Sidebar`, `Tabs`, `LanguageSelector` | navigation, shell, responsive layout |
| Connectivity feedback | `ConnectionStatus`, `DisconnectedState`, `Toast`, `LoadingSpinner`, `ProgressBar` | communicate connection and operation state |
| Discovery UI | `DeviceScanner`, `DeviceListItem`, `DeviceNaming` | scan progress and device selection/name flows |
| Monitoring UI | `SpotValuesMonitor`, `LineChart`, `MultiLineChart` | live data visualization |
| Parameter UI | `DeviceParameters`, `ParameterCategory`, `ParameterInput` | parameter browsing and editing |
| CAN tooling | `CanMappingEditor`, `CanMessageSender`, `CanIoControl` | CAN-specific control features |
| Update UI | `OTAUpdate` | firmware upload UX |

### 8.9 Frontend content and style files

| Pattern | Meaning |
| --- | --- |
| `index.tsx` | actual component/page implementation |
| `*.content.ts` | text/content definitions for `preact-intlayer` |
| `styles.css` in a component folder | component-local styling |
| `web/src/styles/theme/*.css` | shared design tokens such as colors, spacing, typography |
| `web/src/style.css` and `web/src/styles/theme.css` | global app styling entry points |

## 9. Separation Principles Used in This Project

The project uses a few clear separation rules.

### 9.1 On the firmware side

- Entry points compose services, they do not own business rules.
- Web handlers translate transport payloads, they do not own continuous execution.
- The CAN task owns polling loops, queue draining, and hardware-facing state machines.
- Managers own long-lived state and feature policy.
- Protocol helpers own SDO frame-level details.
- Models define data contracts shared across layers.
- Utils stay narrow and mostly stateless.

### 9.2 On the frontend side

- Pages orchestrate, but do not own the global socket.
- Contexts own cross-component state.
- Hooks own async request/response workflows and caching.
- Components focus on rendering and user interaction.
- Utils stay stateless.
- Localized text is split into `*.content.ts` instead of being embedded into components.

### 9.3 Why this split works well here

This project has two hard problems:

- concurrency on the firmware side
- asynchronous request/result coordination on the frontend side

The file layout reflects those problems directly:

- queue and task boundaries are explicit in firmware files
- request-state hooks and shared contexts are explicit in frontend files

## 10. Where to Change Code for Common Tasks

| Task | Primary files to change |
| --- | --- |
| Add a new WebSocket action | `web/src/...` caller, `src/websocket_handlers.h/.cpp`, possibly `src/models/can_command.h`, `src/can_task.cpp`, and `src/event_processor.cpp` |
| Add a new long-running CAN feature | `src/managers/`, `src/can_task.cpp`, possibly `src/models/can_event.h` |
| Add a new synchronous OpenInverter command | `src/oi_can.cpp`, `src/websocket_handlers.cpp`, frontend hook/component |
| Add a new persistent board setting | `src/config.h/.cpp`, `src/http_handlers.cpp`, `web/src/pages/Settings/index.tsx`, `web/src/api/inverter.ts` |
| Add a new detail-page tab | `web/src/pages/DeviceDetails/index.tsx`, a new component in `web/src/components/` |
| Add a new translated UI string | matching `*.content.ts` file |
| Change how spot values are batched or scheduled | `src/managers/spot_values_manager.cpp`, maybe `src/can_task.cpp` and `web/src/components/SpotValuesMonitor/index.tsx` |
| Change CAN scan behavior | `src/managers/device_discovery.cpp`, maybe `src/websocket_handlers.cpp` and `web/src/contexts/DeviceContext.tsx` |
| Change CAN mapping workflow | `src/oi_can.cpp`, `src/websocket_handlers.cpp`, `web/src/hooks/useCanMappings.ts`, `web/src/components/CanMappingEditor/index.tsx` |
| Change OTA behavior | `src/http_handlers.cpp`, `src/firmware/update_handler.cpp`, `src/event_processor.cpp`, `web/src/components/OTAUpdate/index.tsx` |

If you are adding new code, the quickest rule of thumb is:

- browser protocol translation -> `websocket_handlers.cpp`
- background CAN execution -> `can_task.cpp`
- stateful feature logic -> a manager
- frame/protocol details -> `oi_can.cpp` or `sdo_protocol.cpp`
- browser request coordination -> a hook
- shared browser state -> a context
- rendering -> a component or page
