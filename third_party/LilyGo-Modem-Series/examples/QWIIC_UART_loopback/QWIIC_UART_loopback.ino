/**
 * @file      QWIIC_UART_loopback.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-09-05
 * @note      The example uses Standard Series pins by default. For other boards, please refer to Change Available GPIO.
 */

#include <Arduino.h>
#include <Wire.h>

#define TX_PIN          (43)
#define RX_PIN          (44)
#define QWIIC_UART      Serial2

void setup()
{
    Serial.begin(115200);
    // Initialize QWIIC UART
    QWIIC_UART.begin(115200, SERIAL_8N1, TX_PIN, RX_PIN);
}

void loop()
{
    if (Serial.available()) {
        char inChar = (char)Serial.read();
        QWIIC_UART.write(inChar);
    }

    if (QWIIC_UART.available()) {
        char inChar = (char)QWIIC_UART.read();
        Serial.write(inChar);
    }
}
