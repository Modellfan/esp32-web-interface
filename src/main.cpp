#include "main.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ctype.h>
#include <esp_system.h>

#include "can_task.h"
#include "config.h"
#include "event_processor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "http_handlers.h"
#include "oi_can.h"
#include "websocket_handlers.h"
#include "wifi_setup.h"

#include "managers/device_cache.h"
#include "managers/device_connection.h"
#include "managers/device_discovery.h"
#include "models/can_event.h"
#include "utils/can_hardware.h"
#include "utils/string_utils.h"

// ============================================================================
// Global Variables
// ============================================================================

// FreeRTOS Queue handles
QueueHandle_t canCommandQueue = nullptr;
QueueHandle_t canEventQueue = nullptr;

const char* host = "inverter";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Config config;

namespace {
#ifndef DEBUG_WDT_RUNTIME_LOGS
#define DEBUG_WDT_RUNTIME_LOGS 0
#endif

const char* resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:
      return "unknown";
    case ESP_RST_POWERON:
      return "power_on";
    case ESP_RST_EXT:
      return "external_pin";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
      return "task_watchdog";
    case ESP_RST_WDT:
      return "other_watchdog";
    case ESP_RST_DEEPSLEEP:
      return "deepsleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    case ESP_RST_USB:
      return "usb";
    case ESP_RST_JTAG:
      return "jtag";
    case ESP_RST_EFUSE:
      return "efuse";
    case ESP_RST_PWR_GLITCH:
      return "power_glitch";
    case ESP_RST_CPU_LOCKUP:
      return "cpu_lockup";
    default:
      return "unhandled";
  }
}

#ifdef DEBUG
void printRuntimeStatus(const char* sourceTag) {
  const UBaseType_t cmdDepth = canCommandQueue ? uxQueueMessagesWaiting(canCommandQueue) : 0;
  const UBaseType_t evtDepth = canEventQueue ? uxQueueMessagesWaiting(canEventQueue) : 0;
  const UBaseType_t txDepth = canTxQueue ? uxQueueMessagesWaiting(canTxQueue) : 0;
  const UBaseType_t sdoDepth = sdoResponseQueue ? uxQueueMessagesWaiting(sdoResponseQueue) : 0;

  DBG_OUTPUT_PORT.printf(
      "[WDTDBG][%s] ms=%lu core=%d connState=%d scan=%d heap=%u minHeap=%u stackHW=%u queues(cmd=%u evt=%u tx=%u sdo=%u)\n",
      sourceTag, (unsigned long)millis(), (int)xPortGetCoreID(), (int)DeviceConnection::instance().getState(),
      DeviceDiscovery::instance().isScanActive() ? 1 : 0, (unsigned int)ESP.getFreeHeap(),
      (unsigned int)ESP.getMinFreeHeap(), (unsigned int)uxTaskGetStackHighWaterMark(nullptr), (unsigned int)cmdDepth,
      (unsigned int)evtDepth, (unsigned int)txDepth, (unsigned int)sdoDepth);
}

void handleSerialDebugCommands() {
  static char lineBuf[40];
  static size_t lineLen = 0;

  while (DBG_OUTPUT_PORT.available() > 0) {
    char ch = (char)DBG_OUTPUT_PORT.read();

    if (ch == '\r' || ch == '\n') {
      if (lineLen == 0) {
        continue;
      }

      lineBuf[lineLen] = '\0';
      for (size_t i = 0; i < lineLen; i++) {
        lineBuf[i] = (char)tolower((unsigned char)lineBuf[i]);
      }

      if (strcmp(lineBuf, "status") == 0 || strcmp(lineBuf, "s") == 0) {
        printRuntimeStatus("serial");
      } else if (strcmp(lineBuf, "help") == 0 || strcmp(lineBuf, "h") == 0 || strcmp(lineBuf, "?") == 0) {
        DBG_OUTPUT_PORT.println("[WDTDBG] serial commands: status|s, help|h|?");
      } else {
        DBG_OUTPUT_PORT.printf("[WDTDBG] unknown serial command '%s' (try 'help')\n", lineBuf);
      }

      lineLen = 0;
      continue;
    }

    if (lineLen < (sizeof(lineBuf) - 1)) {
      lineBuf[lineLen++] = ch;
    } else {
      lineLen = 0;
      DBG_OUTPUT_PORT.println("[WDTDBG] serial command too long, input cleared");
    }
  }
}
#endif
}  // namespace

// ============================================================================
// Setup
// ============================================================================

void setup(void) {
  DBG_OUTPUT_PORT.begin(115200);
  delay(50);
  const esp_reset_reason_t resetReason = esp_reset_reason();
  DBG_OUTPUT_PORT.printf("[BOOT] Reset reason: %s (%d)\n", resetReasonToString(resetReason), (int)resetReason);

#ifdef DEBUG
  DBG_OUTPUT_PORT.println("[WDTDBG][setup] start");
  DBG_OUTPUT_PORT.printf("[WDTDBG][setup] STATUS_LED_PIN=%d STATUS_LED_COUNT=%d\n", STATUS_LED_PIN, STATUS_LED_COUNT);
#endif

  // Initialize status LED (NeoPixel)
#ifdef DEBUG
  DBG_OUTPUT_PORT.println("[WDTDBG][setup] status led begin");
#endif
  StatusLED::instance().begin();
  statusLEDOff();
#ifdef DEBUG
  DBG_OUTPUT_PORT.println("[WDTDBG][setup] status led done");
#endif

  // Start SPI Flash file system
#ifdef DEBUG
  DBG_OUTPUT_PORT.println("[WDTDBG][setup] LittleFS begin");
#endif
  LittleFS.begin(false, "/littlefs", 10, "littlefs");
#ifdef DEBUG
  DBG_OUTPUT_PORT.println("[WDTDBG][setup] LittleFS done");
#endif

  // WiFi initialization
#ifdef DEBUG
  DBG_OUTPUT_PORT.println("[WDTDBG][setup] WiFi init");
#endif
  WiFiSetup::initialize();
#ifdef DEBUG
  DBG_OUTPUT_PORT.println("[WDTDBG][setup] WiFi init done");
#endif

  MDNS.begin(host);
#ifdef DEBUG
  DBG_OUTPUT_PORT.println("[WDTDBG][setup] mDNS begin done");
#endif

  config.load();
#ifdef DEBUG
  DBG_OUTPUT_PORT.printf("[WDTDBG][setup] config loaded canTx=%d canRx=%d baud=%d\n", config.getCanTXPin(),
                         config.getCanRXPin(), config.getCanSpeed());
#endif

  // Initialize CAN enable pin if configured
  if (config.getCanEnablePin() > 0) {
    pinMode(config.getCanEnablePin(), OUTPUT);
    digitalWrite(config.getCanEnablePin(), LOW);
  }

  // Initialize CAN transceiver shutdown and standby pins (platform-specific)
  CanHardware::initAllTransceiverPins();

  // Initialize CAN bus at startup
  DBG_OUTPUT_PORT.println("Initializing CAN bus...");
  OICan::InitCAN(config.getBaudRateEnum(), config.getCanTXPin(), config.getCanRXPin());

  // Create FreeRTOS queues
  canCommandQueue = xQueueCreate(10, sizeof(CANCommand));
  canEventQueue = xQueueCreate(20, sizeof(CANEvent));

  if (canCommandQueue == nullptr || canEventQueue == nullptr) {
    DBG_OUTPUT_PORT.println("ERROR: Failed to create queues!");
    return;
  }

  DBG_OUTPUT_PORT.println("Queues created successfully");

  // Setup device discovery callback to post events
  DeviceDiscovery::instance().setDiscoveryCallback([](uint8_t nodeId, const char* serial, uint32_t lastSeen) {
    CANEvent evt;
    evt.type = EVT_DEVICE_DISCOVERED;
    evt.data.deviceDiscovered.nodeId = nodeId;
    safeCopyString(evt.data.deviceDiscovered.serial, serial);
    evt.data.deviceDiscovered.lastSeen = lastSeen;
    evt.data.deviceDiscovered.name[0] = '\0';

    // Look up name from cache
    std::string name = DeviceCache::instance().getDeviceName(serial);
    if (!name.empty()) {
      safeCopyString(evt.data.deviceDiscovered.name, name.c_str());
    }

    xQueueSend(canEventQueue, &evt, 0);
  });

  // Setup scan progress callback to post events
  DeviceDiscovery::instance().setProgressCallback([](uint8_t currentNode, uint8_t startNode, uint8_t endNode) {
    CANEvent evt;
    evt.type = EVT_SCAN_PROGRESS;
    evt.data.scanProgress.currentNode = currentNode;
    evt.data.scanProgress.startNode = startNode;
    evt.data.scanProgress.endNode = endNode;
    xQueueSend(canEventQueue, &evt, 0);
  });

  // Setup connection ready callback to post events when device is truly connected
  DeviceConnection::instance().setConnectionReadyCallback([](uint8_t nodeId, const char* serial) {
    DBG_OUTPUT_PORT.printf("[Callback] Connection ready - node %d, serial %s\n", nodeId, serial);
    CANEvent evt;
    evt.type = EVT_CONNECTED;
    evt.data.connected.nodeId = nodeId;
    safeCopyString(evt.data.connected.serial, serial);
    xQueueSend(canEventQueue, &evt, 0);
  });

  // Note: JSON download progress callback removed - async download now uses
  // EVT_JSON_READY event for completion notification to specific client

  // Initialize CAN queues and spawn CAN task
  initCanQueues();
#if CONFIG_FREERTOS_UNICORE
  xTaskCreate(canTask, "CAN_Task", 8192, nullptr, 1, nullptr);
  DBG_OUTPUT_PORT.println("CAN task spawned (single-core mode)");
#else
  xTaskCreatePinnedToCore(canTask, "CAN_Task", 8192, nullptr, 1, nullptr, 0);
  DBG_OUTPUT_PORT.println("CAN task spawned on Core 0 (dual-core mode)");
#endif

  // WebSocket setup
  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);

  // Server initialization
  ArduinoOTA.setHostname(host);
  ArduinoOTA.begin();

  // Register all HTTP routes
  registerHttpRoutes(server);

  server.begin();

  MDNS.addService("http", "tcp", 80);

#ifdef DEBUG
  DBG_OUTPUT_PORT.println("[WDTDBG][setup] complete");
#endif
}

// ============================================================================
// Main Loop
// ============================================================================

void loop(void) {
#ifdef DEBUG
  handleSerialDebugCommands();
#if DEBUG_WDT_RUNTIME_LOGS
  const uint32_t loopStartUs = micros();
  uint32_t opStartUs = micros();
#endif
#endif

  ws.cleanupClients();
#ifdef DEBUG
#if DEBUG_WDT_RUNTIME_LOGS
  const uint32_t wsCleanupUs = micros() - opStartUs;
  opStartUs = micros();
#endif
#endif

  ArduinoOTA.handle();
#ifdef DEBUG
#if DEBUG_WDT_RUNTIME_LOGS
  const uint32_t otaHandleUs = micros() - opStartUs;
  opStartUs = micros();
#endif
#endif

  // Process events from CAN task and firmware progress
  EventProcessor::processEvents(ws);
#ifdef DEBUG
#if DEBUG_WDT_RUNTIME_LOGS
  const uint32_t eventProcessUs = micros() - opStartUs;
  opStartUs = micros();
#endif
#endif

  EventProcessor::processFirmwareProgress(ws);
#ifdef DEBUG
#if DEBUG_WDT_RUNTIME_LOGS
  const uint32_t fwProcessUs = micros() - opStartUs;
  const uint32_t loopElapsedUs = micros() - loopStartUs;

  if (wsCleanupUs > 20000 || otaHandleUs > 20000 || eventProcessUs > 40000 || fwProcessUs > 20000 ||
      loopElapsedUs > 100000) {
    DBG_OUTPUT_PORT.printf("[WDTDBG][loop] slow us total=%lu ws=%lu ota=%lu events=%lu fw=%lu\n",
                           (unsigned long)loopElapsedUs, (unsigned long)wsCleanupUs, (unsigned long)otaHandleUs,
                           (unsigned long)eventProcessUs, (unsigned long)fwProcessUs);
  }

  static uint32_t lastHeartbeatMs = 0;
  const uint32_t nowMs = millis();
  if ((nowMs - lastHeartbeatMs) >= 2000) {
    printRuntimeStatus("loop");
    lastHeartbeatMs = nowMs;
  }
#endif
#endif
}
