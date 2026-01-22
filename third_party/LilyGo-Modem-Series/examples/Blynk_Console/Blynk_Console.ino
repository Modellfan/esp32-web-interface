/*************************************************************
  Download latest Blynk library here:
    https://github.com/blynkkk/blynk-library/releases/latest

  Blynk is a platform with iOS and Android apps to control
  Arduino, Raspberry Pi and the likes over the Internet.
  You can easily build graphic interfaces for all your
  projects by simply dragging and dropping widgets.

    Downloads, docs, tutorials: http://www.blynk.cc
    Sketch generator:           http://examples.blynk.cc
    Blynk community:            http://community.blynk.cc
    Follow us:                  http://www.fb.com/blynkapp
                                http://twitter.com/blynk_app

  Blynk library is licensed under MIT license
  This example code is in public domain.

 *************************************************************
  Attention! Please check out TinyGSM guide:
    https://tiny.cc/tinygsm-readme

  Change GPRS apm, user, pass, and Blynk auth token to run :)
  Feel free to apply it to any other example. It's simple!

 *************************************************************/
// See all AT commands, if wanted
// #define DUMP_AT_COMMANDS

// Set serial for debug console (to the Serial Monitor, default speed 115200)
#define SerialMon Serial

// Set serial for AT commands (to the module)
// Use Hardware Serial on Mega, Leonardo, Micro
#define SerialAT Serial1

#define BLYNK_PRINT SerialMon

/* Fill in information from Blynk Device Info here */
#define BLYNK_TEMPLATE_ID           "TMPxxxxxx"
#define BLYNK_TEMPLATE_NAME         "Device"
#define BLYNK_AUTH_TOKEN            "YourAuthToken"

// It depends on the operator whether to set up an APN. If some operators do not set up an APN,
// they will be rejected when registering for the network. You need to ask the local operator for the specific APN.
// APNs from other operators are welcome to submit PRs for filling.
//#define NETWORK_APN     "ctlte"             //ctlte: China Telecom

// When using an IPv6 access point, the correct IPv6 APN must be configured.
bool use_ipv6_access_point = false; // Whether to use IPv6 to set the access point

#ifdef NETWORK_APN
String apn = NETWORK_APN;
#else
String apn = "";
#endif


// Default heartbeat interval for GSM is 60
// If you want override this value, uncomment and set this option:
//#define BLYNK_HEARTBEAT 30

#include "utilities.h"
#include <Arduino.h>
#include <TinyGsmClient.h>
#include <BlynkSimpleTinyGSM.h>

BlynkTimer timer;

#ifdef DUMP_AT_COMMANDS
#include <StreamDebugger.h>
StreamDebugger debugger(SerialAT, SerialMon);
TinyGsm modem(debugger);
#else
TinyGsm modem(SerialAT);
#endif

// Your GPRS credentials
// Leave empty, if missing user or pass
char user[] = "";
char pass[] = "";


BLYNK_WRITE(V0)
{
    if (param.asInt() == 1) {
        Serial.println("OFF");
        Blynk.logEvent("LED STATE", "OFF");
    } else {
        Serial.println("ON");
        Blynk.logEvent("LED STATE", "ON");
    }
}

void sendRandomData()
{
    //Send randomly generated fake data
    float h = random(60, 100);
    float t = random(60, 100);
    Blynk.virtualWrite(V1, h);
    Blynk.virtualWrite(V2, t);
}

void setup()
{
    Serial.begin(115200);

    // Set modem baud
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

#ifdef BOARD_POWERON_PIN
    /* Set Power control pin output
    * * @note      Known issues, ESP32 (V1.2) version of T-A7670, T-A7608,
    *            when using battery power supply mode, BOARD_POWERON_PIN (IO12) must be set to high level after esp32 starts, otherwise a reset will occur.
    * */
    pinMode(BOARD_POWERON_PIN, OUTPUT);
    digitalWrite(BOARD_POWERON_PIN, HIGH);
#endif

    // Set modem reset pin ,reset modem
#ifdef MODEM_RESET_PIN
    pinMode(MODEM_RESET_PIN, OUTPUT);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL); delay(100);
    digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL); delay(2600);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
#endif

#ifdef MODEM_FLIGHT_PIN
    // If there is an airplane mode control, you need to exit airplane mode
    pinMode(MODEM_FLIGHT_PIN, OUTPUT);
    digitalWrite(MODEM_FLIGHT_PIN, HIGH);
#endif

#ifdef MODEM_DTR_PIN
    // Pull down DTR to ensure the modem is not in sleep state
    Serial.printf("Set DTR pin %d LOW\n", MODEM_DTR_PIN);
    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, LOW);
#endif

    // Turn on modem
    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH);
    delay(MODEM_POWERON_PULSE_WIDTH_MS);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);

    // Check if the modem is online
    Serial.println("Start modem...");

    int retry = 0;
    while (!modem.testAT(1000)) {
        Serial.println(".");
        if (retry++ > 30) {
            digitalWrite(BOARD_PWRKEY_PIN, LOW);
            delay(100);
            digitalWrite(BOARD_PWRKEY_PIN, HIGH);
            delay(MODEM_POWERON_PULSE_WIDTH_MS);
            digitalWrite(BOARD_PWRKEY_PIN, LOW);
            retry = 0;
        }
    }
    Serial.println();

    // Check if SIM card is online
    SimStatus sim = SIM_ERROR;
    while (sim != SIM_READY) {
        sim = modem.getSimStatus();
        switch (sim) {
        case SIM_READY:
            Serial.println("SIM card online");
            break;
        case SIM_LOCKED:
            Serial.println("The SIM card is locked. Please unlock the SIM card first.");
            // const char *SIMCARD_PIN_CODE = "123456";
            // modem.simUnlock(SIMCARD_PIN_CODE);
            break;
        default:
            break;
        }
        delay(1000);
    }

#ifdef TINY_GSM_MODEM_HAS_NETWORK_MODE
    if (!modem.setNetworkMode(MODEM_NETWORK_AUTO)) {
        Serial.println("Set network mode failed!");
    }
    String mode = modem.getNetworkModeString();
    Serial.print("Current network mode : ");
    Serial.println(mode);
#endif

#ifdef TINY_GSM_MODEM_HAS_PREFERRED_MODE
    if (!modem.setPreferredMode(MODEM_PREFERRED_CATM_NBIOT)) {
        Serial.println("Set network preferred failed!");
    }
    String prefMode = modem.getPreferredModeString();
    Serial.print("Current preferred mode : ");
    Serial.println(prefMode);
#endif



#ifdef NETWORK_APN
    Serial.printf("Set network apn : %s\n", NETWORK_APN);
    if (!modem.setNetworkAPN(NETWORK_APN)) {
        Serial.println("Set network apn error !");
    }
#endif


    // Check network registration status and network signal status
    int16_t sq ;
    Serial.print("Wait for the modem to register with the network.");
    RegStatus status = REG_NO_RESULT;
    while (status == REG_NO_RESULT || status == REG_SEARCHING || status == REG_UNREGISTERED) {
        status = modem.getRegistrationStatus();
        switch (status) {
        case REG_UNREGISTERED:
        case REG_SEARCHING:
            sq = modem.getSignalQuality();
            Serial.printf("[%lu] Signal Quality:%d\n", millis() / 1000, sq);
            delay(1000);
            break;
        case REG_DENIED:
            Serial.println("Network registration was rejected, please check if the APN is correct");
            return ;
        case REG_OK_HOME:
            Serial.println("Online registration successful");
            break;
        case REG_OK_ROAMING:
            Serial.println("Network registration successful, currently in roaming mode");
            break;
        default:
            Serial.printf("Registration Status:%d\n", status);
            delay(1000);
            break;
        }
    }
    Serial.println();

#ifdef MODEM_REG_SMS_ONLY
    while (status == REG_SMS_ONLY) {
        Serial.println("Registered for \"SMS only\", home network (applicable only when E-UTRAN), this type of registration cannot access the network. Please check the APN settings and ask the operator for the correct APN information and the balance and package of the SIM card. If you still cannot connect, please replace the SIM card and test again. Related ISSUE: https://github.com/Xinyuan-LilyGO/LilyGO-T-A76XX/issues/307#issuecomment-3034800353");
        delay(5000);
    }
#endif

    Serial.printf("Registration Status:%d\n", status);
    delay(1000);

    String ueInfo;
    if (modem.getSystemInformation(ueInfo)) {
        Serial.print("Inquiring UE system information:");
        Serial.println(ueInfo);
    }

    /**
     *  Configure the network APN and specify whether to access the network using IPv6. If unsure, please consult your SIM card provider.
     */
    Serial.print("Connecting to network with APN:"); Serial.println(apn);
    Serial.print("Use IPv6 access point:"); Serial.println(use_ipv6_access_point ? "true" : "false");
    retry = 3;
    while (retry--) {
        if (modem.setNetworkActive(apn, use_ipv6_access_point)) {
            break;
        }
        Serial.println("Enable network failed, retry after 3s...");
        delay(3000);
    }
    if (retry < 0) {
        Serial.println("Failed to enable network!");
        return;
    }

    delay(5000);

    String ipAddress = modem.getLocalIP();
    Serial.print("Network IP:"); Serial.println(ipAddress);

    delay(1000);

    String version;

    // Flush Rx buffer
    while (SerialAT.available() > 0) {
        SerialAT.read();
    }

    // Check firmware version
    modem.sendAT("+SIMCOMATI");
    Serial.println("Check firmware version");
    modem.waitResponse(10000, version);
    Serial.println(version);


    Serial.println("---------------Blynk-------------------");
    delay(3000);

    // Restart takes quite some time
    // To skip it, call init() instead of restart()
    Serial.println("Initializing modem...");
    if (!modem.init()) {
        Serial.println("Failed to restart modem, delaying 10s and retrying");
        return;
    }

    String name = modem.getModemName();
    Serial.println("Modem Name: " + name);

    Blynk.begin(BLYNK_AUTH_TOKEN, modem, apn.c_str(), user, pass);
    // Setup a function to be called every two second
    timer.setInterval(2000L, sendRandomData);

    Serial.println("loop...");
}

void loop()
{
    Blynk.run();
    timer.run();
}

#ifndef TINY_GSM_FORK_LIBRARY
#error "No correct definition detected, Please copy all the [lib directories](https://github.com/Xinyuan-LilyGO/LilyGO-T-A76XX/tree/main/lib) to the arduino libraries directory , See README"
#endif


/*
SIM7670G 20250709 OK
Manufacturer: SIMCOM INCORPORATED
Model: SIM7670G-MNGV
Revision: 2374B04SIM767XM5A_M
SIM767XM5_B04V01_241010
IMEI: XXXXXXXXX

!OK
Manufacturer: SIMCOM INCORPORATED
Model: SIM7670G-MNGV
Revision: 2374B05SIM767XM5A_M
SIM767XM5_B05V01_241206
IMEI: XXXXXXXXXXXX


17:32:15.074 > Start modem...
17:32:15.607 >
17:32:17.615 > SIM card online
17:32:18.616 > Wait for the modem to register with the network.Online registration successful
17:32:18.622 >
17:32:18.623 > Registration Status:1
17:32:19.640 > Inquiring UE system information:LTE,Online,460-11,0x775C,117004605,399,EUTRAN-BAND3,1506,3,3,-13,-94,-81,3
+IPADDR: 10.25.155.217 IP:AT+IPADDR
17:32:24.657 > Check firmware version
17:32:24.677 > AT+SIMCOMATI
17:32:24.678 > Manufacturer: SIMCOM INCORPORATED
17:32:24.678 > Model: SIM7670G-MNGV
17:32:24.678 > Revision: 2374B05SIM767XM5A_M
17:32:24.678 > SIM767XM5_B05V01_241206
17:32:24.678 > IMEI: 864643060031749
17:32:24.678 >
17:32:24.678 > OK
17:32:24.678 >
17:32:27.677 > Initializing modem...
17:32:27.708 > Modem Name: SIM7670G-MNGV
17:32:27.708 > [16538]
17:32:27.709 >     ___  __          __
17:32:27.709 >    / _ )/ /_ _____  / /__
17:32:27.709 >   / _  / / // / _ \/  '_/
17:32:27.709 >  /____/_/\_, /_//_/_/\_\
17:32:27.709 >         /___/ v1.3.2 on ESP32-S3
17:32:27.709 >
17:32:27.709 >  #StandWithUkraine    https://bit.ly/swua
17:32:27.709 >
17:32:27.709 >
17:32:27.709 > [16538] Modem init...
17:32:27.738 > [16568] Connecting to network...
17:32:27.751 > [16581] Network: 46011
17:32:27.751 > [16581] Connecting to YourAPN ...
17:32:27.839 > [16669] Connected to GPRS
17:32:27.903 > [16733] Connecting to blynk.cloud:80
17:32:29.039 > [17868] Ready (ping: 311ms).
17:32:29.131 > loop...

------------------------------SIM7600G B04 G22------------------------------------------
! 2025/10/17 OK https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/issues/380#issuecomment-3412977696
Manufacturer: SIMCOM INCORPORATED
Model: SIMCOM_SIM7600G-H
Revision: LE20B04SIM7600G22
QCN:
IMEI: xxxxxxxxxxxxxxxx
MEID:
+GCAP: +CGSM
DeviceInfo: 173,170

SIM card online
Current network mode : AUTO
Wait for the modem to register with the network.[24] Signal Quality:99
[25] Signal Quality:28
Registration Status:-1
[29] Signal Quality:99
[30] Signal Quality:99
[31] Signal Quality:99
[32] Signal Quality:29
Online registration successful

Registration Status:1
Inquiring UE system information:LTE,Online,460-11,0x775C,117004595,399,EUTRAN-BAND3,1650,5,5,-72,-844,-573,17
Network IP:10.9.25.34

Check firmware version
AT+SIMCOMATI

Manufacturer: SIMCOM INCORPORATED
Model: SIMCOM_SIM7600G-H
Revision: LE20B04SIM7600G22
QCN:
IMEI: 862636054508609
MEID:
+GCAP: +CGSM
DeviceInfo: 173,170

OK

---------------Blynk-------------------
Initializing modem...
Modem Name: SIM7600G-H

[43754]
    ___  __          __
   / _ )/ /_ _____  / /__
  / _  / / // / _ \/  '_/
 /____/_/\_, /_//_/_/\_\
        /___/ v1.3.2 on ESP32

 #StandWithUkraine    https://bit.ly/swua


[43764] Modem init...
[43832] Connecting to network...
[43859] Network: CHN-CT
[43859] Connecting to YourAPN ...
[43979] Connected to GPRS
[44054] Connecting to blynk.cloud:80
[45040] Ready (ping: 334ms).
loop...

------------------------------SIM7600G B05 G22------------------------------------------
! 2025/10/17 OK https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/issues/380#issuecomment-3412977696

Manufacturer: SIMCOM INCORPORATED
Model: SIMCOM_SIM7600G-H
Revision: LE20B05SIM7600G22
QCN:
IMEI: xxxxxxxxxxx
MEID:
+GCAP: +CGSM
DeviceInfo: 173,170

SIM card online
Current network mode : AUTO
Wait for the modem to register with the network.[24] Signal Quality:99
[25] Signal Quality:25
Registration Status:-1
[29] Signal Quality:99
[30] Signal Quality:26
[31] Signal Quality:26
Online registration successful

Registration Status:1
Inquiring UE system information:LTE,Online,460-11,0x775C,117004605,399,EUTRAN-BAND3,1506,3,3,-104,-899,-626,18
Network IP:10.27.112.145

Check firmware version
AT+SIMCOMATI

Manufacturer: SIMCOM INCORPORATED
Model: SIMCOM_SIM7600G-H
Revision: LE20B05SIM7600G22
QCN:
IMEI: xxxxxxxxxxx
MEID:
+GCAP: +CGSM
DeviceInfo: 173,170

OK

---------------Blynk-------------------
Initializing modem...
Modem Name: SIM7600G-H

[42846]
    ___  __          __
   / _ )/ /_ _____  / /__
  / _  / / // / _ \/  '_/
 /____/_/\_, /_//_/_/\_\
        /___/ v1.3.2 on ESP32

 #StandWithUkraine    https://bit.ly/swua


[42856] Modem init...
[42929] Connecting to network...
[42958] Network: CHN-CT
[42958] Connecting to YourAPN ...
[43090] Connected to GPRS
[43165] Connecting to blynk.cloud:80
[44145] Ready (ping: 317ms).
loop...

*/