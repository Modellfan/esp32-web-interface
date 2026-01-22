/**
 * @file      QWIIC_T_ICM20948.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-11-28
 * @note      Example demonstrates configuring the QWIIC I2C/UART port as I2C using the ICM20948 sensor.
 *            The UART port does not have a pull-up resistor connected by default;  please add an external 10K pull-up resistor.
 */
#include <Arduino.h>
#include <Wire.h>
#include <ICM20948_WE.h>

#define QWIIC_SDA_PIN           (3)
#define QWIIC_SCL_PIN           (2)

// The default is the UART port, which is used to configure the second I2C port
#define QWIIC_UART_TX_PIN       (43)
#define QWIIC_UART_RX_PIN       (44)

#define ICM20948_ADDR           (0x68)

ICM20948_WE icm1 = ICM20948_WE(&Wire, ICM20948_ADDR);
ICM20948_WE icm2 = ICM20948_WE(&Wire1, ICM20948_ADDR);

bool found_icm1 = false;
bool found_icm2 = false;

uint32_t deviceScan(TwoWire &_port)
{
    uint8_t err, addr;
    int nDevices = 0;
    for (addr = 1; addr < 127; addr++) {
        _port.beginTransmission(addr);
        err = _port.endTransmission();
        if (err == 0) {
            Serial.print("I2C device found at address 0x");
            if (addr < 16)
                Serial.print("0");
            Serial.print(addr, HEX);
            Serial.println(" !");
            nDevices++;
        } else if (err == 4) {
            Serial.print("Unknow error at address 0x");
            if (addr < 16)
                Serial.print("0");
            Serial.println(addr, HEX);
        }
    }
    if (nDevices == 0)
        Serial.println("No I2C devices found\n");
    else
        Serial.println("Done\n");
    return nDevices;
}



bool initICM(ICM20948_WE &icm)
{
    if (!icm.init()) {
        Serial.println("ICM20948 does not respond");
        return false;
    } else {
        icm.setAccRange(ICM20948_ACC_RANGE_2G);
        icm.setAccDLPF(ICM20948_DLPF_1);
        icm.setAccSampleRateDivider(4095);
        icm.setGyrDLPF(ICM20948_DLPF_1);
        icm.setGyrSampleRateDivider(255);
        // icm.enableIntLatch(true);
        icm.enableGyr(true);
        icm.enableAcc(true);
        icm.enableInterrupt(ICM20948_DATA_READY_INT);
        if (!icm.initMagnetometer()) {
            Serial.println("Magnetometer does not respond");
        } else {
            Serial.println("Magnetometer is connected");
        }
    }
    return true;
}


void printIMU(const char * label, ICM20948_WE &icm)
{
    xyzFloat accRaw;
    xyzFloat corrAccRaw;
    xyzFloat gVal;
    icm.readSensor();
    icm.getAccRawValues(&accRaw);
    icm.getCorrectedAccRawValues(&corrAccRaw);
    icm.getGValues(&gVal);
    float resultantG = icm.getResultantG(&gVal);

    Serial.println("*************************************");
    Serial.println(label);
    Serial.println("Raw acceleration values (x,y,z):");
    Serial.print(accRaw.x);
    Serial.print("   ");
    Serial.print(accRaw.y);
    Serial.print("   ");
    Serial.println(accRaw.z);

    Serial.println("Corrected raw acceleration values (x,y,z):");
    Serial.print(corrAccRaw.x);
    Serial.print("   ");
    Serial.print(corrAccRaw.y);
    Serial.print("   ");
    Serial.println(corrAccRaw.z);

    Serial.println("g-values (x,y,z):");
    Serial.print(gVal.x);
    Serial.print("   ");
    Serial.print(gVal.y);
    Serial.print("   ");
    Serial.println(gVal.z);

    Serial.print("Resultant g: ");
    Serial.println(resultantG);
    Serial.println("*************************************");
}

void setup()
{
    Serial.begin(115200);
    while (!Serial) {}

    Serial.println("QWIIC I2C Scanner");

    // Change the Qwiic UART port to an I2C port
    Wire.begin(QWIIC_UART_TX_PIN, QWIIC_UART_RX_PIN);
    deviceScan(Wire);
    // Initialize the Qwiic I2C port
    Wire1.begin(QWIIC_SDA_PIN, QWIIC_SCL_PIN);
    deviceScan(Wire1);

    Serial.println("Please note that the UART port does not have an I2C pull-up resistor and is configured for I2C function.");

    Wire.beginTransmission(ICM20948_ADDR);
    if (Wire.endTransmission() == 0) {
        Serial.println("Found ICM20948 on UART port");
        found_icm1 = true;
        bool rlst = initICM(icm1);
        while (!rlst) {
            Serial.println("Init Wire port ICM20948 failed"); delay(1000);
        }
    }
    Wire1.beginTransmission(ICM20948_ADDR);
    if (Wire1.endTransmission() == 0) {
        Serial.println("Found ICM20948 on I2C port");
        found_icm2 = true;
        bool rlst = initICM(icm2);
        while (!rlst) {
            Serial.println("Init Wire port ICM20948 failed"); delay(1000);
        }
    }

}

void loop()
{
    if (found_icm1) {
        printIMU("ICM20948 on UART port:", icm1);
    }
    if (found_icm2) {
        printIMU("ICM20948 on I2C port:", icm2);
    }
    delay(100);
}

