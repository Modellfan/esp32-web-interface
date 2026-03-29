#include <Arduino.h>
#include <inttypes.h>

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "driver/twai.h"

#ifndef CAN_TEST_RX_PIN
#define CAN_TEST_RX_PIN 21
#endif

#ifndef CAN_TEST_TX_PIN
#define CAN_TEST_TX_PIN 22
#endif

#ifndef CAN_TEST_BAUD
#define CAN_TEST_BAUD 500000
#endif

#ifndef CAN_TEST_POLLING_RATE_MS
#define CAN_TEST_POLLING_RATE_MS 1000
#endif

static bool driver_installed = false;

static twai_timing_config_t getTimingConfig() {
#if CAN_TEST_BAUD == 125000
  return TWAI_TIMING_CONFIG_125KBITS();
#elif CAN_TEST_BAUD == 250000
  return TWAI_TIMING_CONFIG_250KBITS();
#elif CAN_TEST_BAUD == 500000
  return TWAI_TIMING_CONFIG_500KBITS();
#else
#error "Unsupported CAN_TEST_BAUD. Use 125000, 250000, or 500000."
#endif
}

static void logCanFrame(const char* direction, const twai_message_t& message) {
  Serial.printf("CAN %s id=0x%08" PRIX32 " dlc=%u%s%s data",
                direction,
                message.identifier,
                message.data_length_code,
                message.extd ? " ext" : "",
                message.rtr ? " rtr" : "");

  if (!message.rtr) {
    for (int i = 0; i < message.data_length_code; i++) {
      Serial.printf(" %02X", message.data[i]);
    }
  }

  Serial.print("\r\n");
}

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.printf("Starting CAN test tx=%d rx=%d baud=%d poll=%dms\r\n",
                CAN_TEST_TX_PIN,
                CAN_TEST_RX_PIN,
                CAN_TEST_BAUD,
                CAN_TEST_POLLING_RATE_MS);

  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
      static_cast<gpio_num_t>(CAN_TEST_TX_PIN),
      static_cast<gpio_num_t>(CAN_TEST_RX_PIN),
      TWAI_MODE_LISTEN_ONLY);
  twai_timing_config_t t_config = getTimingConfig();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
    Serial.println("Driver installed");
  } else {
    Serial.println("Failed to install driver");
    return;
  }

  if (twai_start() == ESP_OK) {
    Serial.println("Driver started");
  } else {
    Serial.println("Failed to start driver");
    return;
  }

  uint32_t alerts_to_enable =
      TWAI_ALERT_RX_DATA | TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_ERROR | TWAI_ALERT_RX_QUEUE_FULL;
  if (twai_reconfigure_alerts(alerts_to_enable, NULL) == ESP_OK) {
    Serial.println("CAN Alerts reconfigured");
  } else {
    Serial.println("Failed to reconfigure alerts");
    return;
  }

  driver_installed = true;
}

void loop() {
  if (!driver_installed) {
    delay(1000);
    return;
  }

  uint32_t alerts_triggered = 0;
  esp_err_t alert_result = twai_read_alerts(&alerts_triggered, pdMS_TO_TICKS(CAN_TEST_POLLING_RATE_MS));
  if (alert_result != ESP_OK) {
    return;
  }

  twai_status_info_t twai_status = {};
  twai_get_status_info(&twai_status);

  if (alerts_triggered & TWAI_ALERT_ERR_PASS) {
    Serial.println("Alert: TWAI controller has become error passive.");
  }

  if (alerts_triggered & TWAI_ALERT_BUS_ERROR) {
    Serial.println("Alert: A (Bit, Stuff, CRC, Form, ACK) error has occurred on the bus.");
    Serial.printf("Bus error count: %" PRIu32 "\r\n", twai_status.bus_error_count);
  }

  if (alerts_triggered & TWAI_ALERT_RX_QUEUE_FULL) {
    Serial.println("Alert: The RX queue is full causing a received frame to be lost.");
    Serial.printf("RX buffered: %" PRIu32 "\t", twai_status.msgs_to_rx);
    Serial.printf("RX missed: %" PRIu32 "\t", twai_status.rx_missed_count);
    Serial.printf("RX overrun %" PRIu32 "\r\n", twai_status.rx_overrun_count);
  }

  if (alerts_triggered & TWAI_ALERT_RX_DATA) {
    twai_message_t message;
    while (twai_receive(&message, 0) == ESP_OK) {
      logCanFrame("RX", message);
    }
  }
}
