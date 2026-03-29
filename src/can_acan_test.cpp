#ifndef ARDUINO_ARCH_ESP32
#error "Select an ESP32 board"
#endif

#include <Arduino.h>
#include <inttypes.h>

#include <ACAN_ESP32.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <core_version.h>

#ifndef CAN_ACAN_TEST_BIT_RATE
#define CAN_ACAN_TEST_BIT_RATE 1000000UL
#endif

#ifndef CAN_ACAN_TEST_TX_PIN
#define CAN_ACAN_TEST_TX_PIN 17
#endif

#ifndef CAN_ACAN_TEST_RX_PIN
#define CAN_ACAN_TEST_RX_PIN 16
#endif

#ifndef CAN_ACAN_TEST_SEND_INTERVAL_MS
#define CAN_ACAN_TEST_SEND_INTERVAL_MS 500
#endif

#ifndef LED_BUILTIN
#define LED_BUILTIN 5
#endif

static uint32_t gBlinkLedDate = 0;
static uint32_t gReceivedFrameCount = 0;
static uint32_t gSentFrameCount = 0;

static void logCanFrame(const char* direction, const CANMessage& frame) {
  Serial.printf("CAN %s id=0x%08" PRIX32 " dlc=%u%s%s data",
                direction,
                frame.id,
                frame.len,
                frame.ext ? " ext" : "",
                frame.rtr ? " rtr" : "");

  if (!frame.rtr) {
    for (uint8_t i = 0; i < frame.len; i++) {
      Serial.printf(" %02X", frame.data[i]);
    }
  }

  Serial.print("\r\n");
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  Serial.begin(115200);
  delay(100);

  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  Serial.print("ESP32 Arduino Release: ");
  Serial.println(ARDUINO_ESP32_RELEASE);
  Serial.print("ESP32 Chip Revision: ");
  Serial.println(chip_info.revision);
  Serial.print("ESP32 SDK: ");
  Serial.println(ESP.getSdkVersion());
  Serial.print("ESP32 Flash: ");
  uint32_t size_flash_chip = 0;
  esp_flash_get_size(NULL, &size_flash_chip);
  Serial.print(size_flash_chip / (1024 * 1024));
  Serial.print(" MB ");
  Serial.println(((chip_info.features & CHIP_FEATURE_EMB_FLASH) != 0) ? "(embeded)" : "(external)");
  Serial.print("APB CLOCK: ");
  Serial.print(APB_CLK_FREQ);
  Serial.println(" Hz");

  Serial.println("Configure ESP32 CAN");
  Serial.printf("ACAN normal mode tx=%d rx=%d baud=%lu\r\n",
                CAN_ACAN_TEST_TX_PIN,
                CAN_ACAN_TEST_RX_PIN,
                static_cast<unsigned long>(CAN_ACAN_TEST_BIT_RATE));

  ACAN_ESP32_Settings settings(CAN_ACAN_TEST_BIT_RATE);
  settings.mRequestedCANMode = ACAN_ESP32_Settings::NormalMode;
  settings.mRxPin = static_cast<gpio_num_t>(CAN_ACAN_TEST_RX_PIN);
  settings.mTxPin = static_cast<gpio_num_t>(CAN_ACAN_TEST_TX_PIN);

  const uint32_t errorCode = ACAN_ESP32::can.begin(settings);
  if (errorCode == 0) {
    Serial.print("Bit Rate prescaler: ");
    Serial.println(settings.mBitRatePrescaler);
    Serial.print("Time Segment 1:     ");
    Serial.println(settings.mTimeSegment1);
    Serial.print("Time Segment 2:     ");
    Serial.println(settings.mTimeSegment2);
    Serial.print("RJW:                ");
    Serial.println(settings.mRJW);
    Serial.print("Triple Sampling:    ");
    Serial.println(settings.mTripleSampling ? "yes" : "no");
    Serial.print("Actual bit rate:    ");
    Serial.print(settings.actualBitRate());
    Serial.println(" bit/s");
    Serial.print("Exact bit rate ?    ");
    Serial.println(settings.exactBitRate() ? "yes" : "no");
    Serial.print("Distance            ");
    Serial.print(settings.ppmFromDesiredBitRate());
    Serial.println(" ppm");
    Serial.print("Sample point:       ");
    Serial.print(settings.samplePointFromBitStart());
    Serial.println("%");
    Serial.println("Configuration OK!");
  } else {
    Serial.print("Configuration error 0x");
    Serial.println(errorCode, HEX);
  }
}

void loop() {
  CANMessage frame;

  if (gBlinkLedDate < millis()) {
    gBlinkLedDate += CAN_ACAN_TEST_SEND_INTERVAL_MS;
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));

    Serial.print("Sent: ");
    Serial.print(gSentFrameCount);
    Serial.print(" ");
    Serial.print("Receive: ");
    Serial.print(gReceivedFrameCount);
    Serial.print(" ");
    Serial.print("STATUS 0x");
    Serial.print(ACAN_ESP32::can.TWAI_STATUS_REG(), HEX);
    Serial.print(" RXERR ");
    Serial.print(ACAN_ESP32::can.TWAI_RX_ERR_CNT_REG());
    Serial.print(" TXERR ");
    Serial.println(ACAN_ESP32::can.TWAI_TX_ERR_CNT_REG());

    frame.id = 0x123;
    frame.len = 8;
    frame.ext = false;
    frame.rtr = false;
    for (uint8_t i = 0; i < frame.len; i++) {
      frame.data[i] = i;
    }

    const bool ok = ACAN_ESP32::can.tryToSend(frame);
    if (ok) {
      gSentFrameCount += 1;
      logCanFrame("TX", frame);
    } else {
      Serial.println("CAN TX failed");
    }
  }

  while (ACAN_ESP32::can.receive(frame)) {
    gReceivedFrameCount += 1;
    logCanFrame("RX", frame);
  }
}
