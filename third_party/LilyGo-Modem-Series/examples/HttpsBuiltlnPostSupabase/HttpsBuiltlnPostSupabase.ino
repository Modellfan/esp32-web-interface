/**
 * @file      HttpsBuiltlnPostSupabase.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-11-17
 * @note      Example for posting GPS data to Supabase using HTTPS with Modem-BuiltIn commands
 */

// See all AT commands, if wanted
#define DUMP_AT_COMMANDS

#include "utilities.h"
#include <TinyGsmClient.h>
#include <cJSON.h>

#ifdef DUMP_AT_COMMANDS  // if enabled it requires the streamDebugger lib
#include <StreamDebugger.h>
StreamDebugger debugger(SerialAT, Serial);
TinyGsm modem(debugger);
#else
TinyGsm modem(SerialAT);
#endif

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

// Supabase configuration
#define SUPABASE_SERVER_URL "YOUR_SUPABASE_PROJECT_URL" // Supabase RESTful API endpoint URL

// Simplify testing by inserting data using a server role.
#define SUPABASE_API_KEY "YOUR_SUPABASE_API_KEY"        // Supabase API Key

/**
 * @brief Generate JSON string with custom parent key, latitude and longitude
 * @param coord_key Custom parent key (e.g., "coordinates", "location", "gps_data")
 * @param latitude Latitude value (double type, e.g., 39.9042)
 * @param longitude Longitude value (double type, e.g., 116.4074)
 * @param is_formatted 1 = pretty-printed JSON, 0 = compact JSON
 * @return Dynamically allocated JSON string (must be freed by caller), NULL on failure
 */
char *generate_gps_json(const char *coord_key, double latitude, double longitude, int is_formatted)
{
    cJSON *root = NULL;
    cJSON *gps_obj = NULL;
    char *json_str = NULL;

    // 1. Validate input parameters (English error messages)
    if (coord_key == NULL || strlen(coord_key) == 0) {
        Serial.println("[ERROR] Custom key (coord_key) cannot be empty!");
        goto cleanup;
    }
    if (isnan(latitude) || isnan(longitude)) {
        Serial.println("[ERROR] Latitude/longitude cannot be NaN (not a number)!");
        goto cleanup;
    }

    // 2. Create root JSON object
    root = cJSON_CreateObject();
    if (root == NULL) {
        Serial.println("[ERROR] Failed to create root JSON object!");
        goto cleanup;
    }

    // 3. Create GPS sub-object (contains latitude/longitude)
    gps_obj = cJSON_CreateObject();
    if (gps_obj == NULL) {
        Serial.println("[ERROR] Failed to create GPS sub-object!");
        goto cleanup;
    }

    // 4. Add latitude/longitude to sub-object (preserve precision)
    cJSON_AddNumberToObject(gps_obj, "latitude", latitude);
    cJSON_AddNumberToObject(gps_obj, "longitude", longitude);

    // 5. Attach sub-object to root with custom key
    cJSON_AddItemToObject(root, coord_key, gps_obj);
    gps_obj = NULL;  // Transfer ownership to root

    // 6. Generate JSON string (formatted/compact)
    json_str = is_formatted ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        Serial.println("[ERROR] Failed to generate JSON string!");
    }

cleanup:
    // Free cJSON objects (recursive release)
    cJSON_Delete(root);
    cJSON_Delete(gps_obj);
    return json_str;
}


void setup()
{
    Serial.begin(115200); // Set console baud rate

    Serial.println("Start Sketch");

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

    // Turn on the modem
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


    // Print modem software version
    String res;
    modem.sendAT("+SIMCOMATI");
    modem.waitResponse(10000UL, res);
    Serial.println(res);

    Serial.println("Please make sure you are using the latest released version of the firmware. Find the latest version here: https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series#modem-firmware-upgrade-guide");
    Serial.println("If you still have problems with the latest firmware, please open an issue. Otherwise, please do not create meaningless issues.");

    // Initialize HTTPS
    while (!modem.https_begin()) {
        Serial.println("Failed to init HTTPS, retry after 3s...");
        delay(3000);
    }
    Serial.println("HTTPS initialized successfully");

    // Set GET URL
    if (!modem.https_set_url(SUPABASE_SERVER_URL)) {
        Serial.println("Failed to set the URL. Please check the validity of the URL!");
        return;
    }

    modem.https_add_header("apikey", SUPABASE_API_KEY);
    modem.https_add_header("Authorization", "Bearer " SUPABASE_API_KEY);
    modem.https_add_header("Prefer", "return=representation");
    modem.https_set_content_type("application/json");
    modem.https_set_user_agent("TinyGSM/LilyGo-Sim-Series");


    // ---------------- Test Case: Default key "coordinates" (pretty-printed) ----------------
    char *json = generate_gps_json("coordinates", 39.9042, 116.4074, 1);
    if (json == NULL) {
        Serial.println("Failed to generate JSON");
        return;
    }

    Serial.printf("Posting HTTPS data to %s ...\n", SUPABASE_SERVER_URL);
    int httpCode = modem.https_post(json);
    if (httpCode != 200 && httpCode != 201) {
        Serial.print("HTTP post failed ! error code = ");
        Serial.println(httpCode);
        return;
    }

    Serial.printf("HTTP Response code: %d\n", httpCode);

    free(json);  // Must free to avoid memory leak (ESP has limited RAM!)

    // Get HTTPS header information
    String header = modem.https_header();
    Serial.print("HTTP Header : ");
    Serial.println(header);

    // Get HTTPS response
    String body = modem.https_body();
    Serial.print("HTTP body : ");
    Serial.println(body);

    // Disconnect http server
    modem.https_end();
}

void loop()
{
    // Debug AT
    if (SerialAT.available()) {
        Serial.write(SerialAT.read());
    }
    if (Serial.available()) {
        SerialAT.write(Serial.read());
    }
    delay(1);
}

#ifndef TINY_GSM_FORK_LIBRARY
#error "No correct definition detected, Please copy all the [lib directories](https://github.com/Xinyuan-LilyGO/LilyGO-T-A76XX/tree/main/lib) to the arduino libraries directory , See README"
#endif

/*

-------------------------------
20251113
Manufacturer: SIMCOM INCORPORATED
Model: SIMCOM_SIM7600G-H
Revision: LE20B05SIM7600G22
+GCAP: +CGSM
DeviceInfo: 173,170

-------------------------------
20251117:

Manufacturer: INCORPORATED
Model: A7670E-FASE
Revision: A011B09A7670M7_F
A7670M7_B09V01_250619
+GCAP: +CGSM,+FCLASS,+DS
DeviceInfo:

*/
