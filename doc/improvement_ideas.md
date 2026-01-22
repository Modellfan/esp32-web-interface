# Improvement Ideas & Proposals

## Overview
This document captures proposed improvements for the ESP32 web interface firmware and UI. Ideas are grouped by **architecture**, **performance**, and **extra functionality**, with a short **proposal** for each.

## 1) Architecture & Structure

### 1.1 Interchangeable CAN/UART Interface Abstraction
**Idea:** Create a clear, interchangeable interface for CAN bus communication so that CAN, UART, or other transports can be swapped without changing business logic.

**Proposal:**
- Define a `TransportInterface` (or similar) with a small, stable API for `send`, `receive`, `subscribe`.
- Implement `CanTransport` and `UartTransport` classes that conform to this interface.
- Ensure higher-level modules (e.g., SDO/parameter logic) depend only on the interface, not the concrete transport.

**Benefits:**
- Portability to other chips/transports.
- Clear separation between transport and logic.
- Easier testing and simulation.

### 1.2 SDO as a Communication Layer Class (Multi-Instance)
**Idea:** The SDO should act as a communication layer class that receives data from a transport (CAN/UART) and provides parameter APIs. Make it instantiable for multiple node IDs.

**Proposal:**
- Convert SDO into a class (e.g., `SdoNode`) that takes a `TransportInterface` and `node_id` in its constructor.
- Allow multiple instances to run concurrently so multiple IDs can be served simultaneously.
- Expose APIs like `get_parameter`, `set_parameter`, `subscribe_parameter` at the instance level.

**Benefits:**
- Supports multiple nodes without global state.
- Simplifies testing and future expansion.

## 2) Performance

### 2.1 Selective Polling of Values
**Idea:** Allow users to choose which values are polled to avoid hitting the maximum parameter limit.

**Proposal:**
- Add configuration (UI + config storage) to select pollable parameters.
- Only poll the selected subset at runtime.

**Benefits:**
- Reduced bus load and CPU usage.
- Better scalability when many parameters exist.

### 2.2 Skip Polling for CAN-Mapped Values
**Idea:** Don’t poll values that are already mapped to CAN and can be read directly from the bus.

**Proposal:**
- If mapped, rely on bus updates instead of polling.

**Benefits:**
- Avoids redundant traffic.
- More responsive values from real-time bus updates.

### 2.3 One-Time Parameter Polling on Startup
**Idea:** Some parameters only need to be read once (static metadata). Poll them only at startup.

**Proposal:**
- Fetch these on boot and cache them.

**Benefits:**
- Lower ongoing bus usage.
- Faster runtime loop.

## 3) Extra Functionality

### 3.1 Node 0 “System Values” Visualization
**Idea:** Allow visualization of device/system values (e.g., signal strength, time, GPS) as parameters under node 0.

**Proposal:**
- Create a virtual node (ID 0) that exposes system metrics as parameters.
- Map internal metrics (RSSI, free heap, uptime) to parameter IDs.

**Benefits:**
- Unified display for internal and external values.
- Easy expansion for GPS or NTP time later.

### 3.2 Dynamic Values with User-Defined CAN Mappings
**Idea:** Provide dynamic slots for values where users define RX CAN mapping *and* display name. This enables visualization of OEM messages not implemented in firmware.

**Proposal:**
- Add a configurable list of dynamic value slots (e.g., N slots).
- Each slot defines `can_id`, `signal/bitfield`, scale, and label.
- Display these like normal parameters in the UI.

**Benefits:**
- Makes the system extensible for unknown OEM messages.
- Allows quick prototyping without firmware rebuilds.

### 3.3 MQTT Configuration Page
**Idea:** Add a configuration page to control MQTT publishing/subscribing. This also enables integration with a **MyOpenInverter** app.

**Proposal:**
- UI page where users define: 
  - Which values/parameters to publish.
  - Publish frequency per value.
  - Which parameters to subscribe to and write back.
  - Buffering strategy (offline queue, max size).
- Persist configuration in local storage or firmware config.

**Benefits:**
- Clear control of MQTT data flow.

