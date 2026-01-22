/**
 * @file      LoRaWAN.ino
 * LMIC library only support SX1276 Radio
 */

#include <Arduino.h>
#include "loramac.h"
#include <SPI.h>

#define LORA_MISO           19
#define LORA_MOSI           23
#define LORA_SCK            18

void setup()
{
    Serial.begin(115200);

    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI);

    // When the power is turned on, a delay is required.
    delay(1500);
    Serial.println("LMIC-Arduino library only support SX1276 Radio ...");
    Serial.println("LMIC-Arduino library only support SX1276 Radio ...");
    Serial.println("LMIC-Arduino library only support SX1276 Radio ...");
    Serial.println("LMIC-Arduino library only support SX1276 Radio ...");
    setupLMIC();
}

void loop()
{
    loopLMIC();
}

