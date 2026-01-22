// Fixed GSM/LTE test sketch for LilyGo T-Call A7670E-FASE (SIMCom A7670).
// Key fixes for Kaufland Mobil / Telekom reseller SIMs:
//  - Do NOT use CFUN=0/1 (it causes SIMCARD: NOT AVAILABLE + "Flight Mode" stuck states on some firmwares)
//  - Force LTE selection via SIMCom commands (+CNMP/+CMNB) AFTER CPIN is READY
//  - Force PDP context to IPv4 + correct APN (internet.telekom)
//  - Do NOT set APN username/password (leave empty). Your previous "telekom/tm" caused trouble.
//  - Make registration checks LTE-aware by also looking at CEREG output

#include <Arduino.h>

#define TINY_GSM_RX_BUFFER 1024
#define SerialMon Serial
#define TINY_GSM_DEBUG SerialMon
#define TINY_GSM_MODEM_A7670

#include <TinyGsmClient.h>

#define SerialAT Serial1

// T-Call A7670 v1.0 pinout
static const int MODEM_DTR_PIN = 14;
static const int MODEM_TX_PIN = 26;
static const int MODEM_RX_PIN = 25;
static const int BOARD_PWRKEY_PIN = 4;
static const int BOARD_LED_PIN = 12;
static const int MODEM_RESET_PIN = 27;
static const int MODEM_RESET_LEVEL = LOW;

static const int MODEM_POWERON_PULSE_WIDTH_MS = 100;
static const int MODEM_START_WAIT_MS = 3000;

// Kaufland Mobil (Telekom) APN settings
static const char APN[] = "internet.telekom";
static const char APN_USER[] = "";   // MUST be empty for Kaufland Mobil
static const char APN_PASS[] = "";   // MUST be empty for Kaufland Mobil
static const char SIM_PIN[] = "4804";

// Force LTE selection (recommended)
static const bool FORCE_LTE_ONLY = true;  // true = LTE-only, false = LTE+GSM fallback

static const char HTTP_HOST[] = "example.com";
static const int HTTP_PORT = 80;
static const char HTTP_PATH[] = "/";

TinyGsm modem(SerialAT);
TinyGsmClient client(modem);

static const char* simStatusToString(SimStatus status)
{
  switch (status) {
    case SIM_READY: return "READY";
    case SIM_LOCKED: return "LOCKED";
    case SIM_ANTITHEFT_LOCKED: return "ANTITHEFT_LOCKED";
    default: return "ERROR";
  }
}

static const char* regStatusToString(RegStatus status)
{
  switch (status) {
    case REG_OK_HOME: return "OK_HOME";
    case REG_OK_ROAMING: return "OK_ROAMING";
    case REG_DENIED: return "DENIED";
    case REG_SEARCHING: return "SEARCHING";
    case REG_UNREGISTERED: return "UNREGISTERED";
    case REG_SMS_ONLY: return "SMS_ONLY";
    case REG_EMERGENCY_ONLY: return "EMERGENCY_ONLY";
    default: return "UNKNOWN";
  }
}

static void printAtResponse(const char* cmd, uint32_t timeoutMs = 3000UL)
{
  String res;
  modem.sendAT(cmd);
  modem.waitResponse(timeoutMs, res);
  SerialMon.print("AT");
  SerialMon.print(cmd);
  SerialMon.print(" -> ");
  SerialMon.println(res);
}

static void dumpDiagnostics(const char* phase)
{
  SerialMon.println();
  SerialMon.print("=== Modem Diagnostics (");
  SerialMon.print(phase);
  SerialMon.println(") ===");
  SerialMon.print("Modem name: ");
  SerialMon.println(modem.getModemName());
  SerialMon.print("Modem info: ");
  SerialMon.println(modem.getModemInfo());
  SerialMon.print("IMEI: ");
  SerialMon.println(modem.getIMEI());
  SerialMon.print("IMSI: ");
  SerialMon.println(modem.getIMSI());
  SerialMon.print("ICCID: ");
  SerialMon.println(modem.getSimCCID());
  SerialMon.print("SIM status: ");
  SerialMon.println(simStatusToString(modem.getSimStatus()));
  SerialMon.print("Operator: ");
  SerialMon.println(modem.getOperator());
  SerialMon.print("Signal quality: ");
  SerialMon.println(modem.getSignalQuality());
  SerialMon.print("Network mode: ");
  SerialMon.println(modem.getNetworkModeString());
  RegStatus reg = modem.getRegistrationStatus();
  SerialMon.print("Registration: ");
  SerialMon.println(regStatusToString(reg));

  String ueInfo;
  if (modem.getSystemInformation(ueInfo)) {
    SerialMon.print("CPSI: ");
    SerialMon.println(ueInfo);
  }

  printAtResponse("+CPIN?");
  printAtResponse("+CEREG?");
  printAtResponse("+CGREG?");
  printAtResponse("+CREG?");
  printAtResponse("+COPS?");
  printAtResponse("+CNMP?");
  printAtResponse("+CMNB?");
  printAtResponse("+CGDCONT?");
  printAtResponse("+CFUN?");
}

static void ensureModemResponsive()
{
  int retry = 0;
  while (!modem.testAT(1000)) {
    SerialMon.print(".");
    if (retry++ > 30) {
      // try power key pulse again if modem is not responding
      pinMode(BOARD_PWRKEY_PIN, OUTPUT);
      digitalWrite(BOARD_PWRKEY_PIN, LOW);
      delay(100);
      digitalWrite(BOARD_PWRKEY_PIN, HIGH);
      delay(MODEM_POWERON_PULSE_WIDTH_MS);
      digitalWrite(BOARD_PWRKEY_PIN, LOW);
      retry = 0;
    }
  }
  SerialMon.println();
}

static void powerOnModem()
{
  pinMode(BOARD_PWRKEY_PIN, OUTPUT);
  digitalWrite(BOARD_PWRKEY_PIN, LOW);
  delay(100);
  digitalWrite(BOARD_PWRKEY_PIN, HIGH);
  delay(MODEM_POWERON_PULSE_WIDTH_MS);
  digitalWrite(BOARD_PWRKEY_PIN, LOW);
}

static void resetModem()
{
  pinMode(MODEM_RESET_PIN, OUTPUT);
  digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
  delay(100);
  digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL);
  delay(2600);
  digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
}

static bool waitForSimReady(uint32_t timeoutMs)
{
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    SimStatus sim = modem.getSimStatus();
    if (sim == SIM_READY) {
      return true;
    }
    if (sim == SIM_LOCKED) {
      SerialMon.println("SIM locked, attempting unlock...");
      if (strlen(SIM_PIN) > 0 && modem.simUnlock(SIM_PIN)) {
        SerialMon.println("SIM unlocked.");
        delay(1000);
        continue;
      }
      SerialMon.println("SIM unlock failed, retrying...");
    }
    delay(1000);
  }
  SerialMon.println("SIM not ready (timeout).");
  return false;
}

static void configureRatAndApn()
{
  SerialMon.println("Configuring RAT + PDP...");

  printAtResponse("+CMEE=2");

  // Force LTE-only (or LTE+GSM fallback)
  if (FORCE_LTE_ONLY) {
    printAtResponse("+CNMP=13", 5000UL);   // LTE only
  } else {
    printAtResponse("+CNMP=38", 5000UL);   // LTE + GSM
  }

  // DON'T send CMNB on your firmware (it returns ERROR and can detach)
  // printAtResponse("+CMNB=1", 5000UL);

  printAtResponse("+COPS=0", 10000UL);
  printAtResponse("+CEREG=2");

  // PDP profile: IPv4 + correct APN
  printAtResponse("+CGDCONT=1,\"IP\",\"internet.telekom\"");
  printAtResponse("+CGPIAF=1,0,0,0");

  printAtResponse("+CNMP?");
  printAtResponse("+CGDCONT?");
  printAtResponse("+CEREG?");
}


// Parse "+CEREG: <n>,<stat>[,...]" and return stat, or -1 on parse failure.
static int ceregStat()
{
  String res;
  modem.sendAT("+CEREG?");
  if (modem.waitResponse(2000UL, res) != 1) return -1;

  // Example: "\r\n+CEREG: 2,0\r\n\r\nOK\r\n"
  int idx = res.indexOf("+CEREG:");
  if (idx < 0) return -1;

  // Find first comma after "+CEREG: "
  int comma1 = res.indexOf(',', idx);
  if (comma1 < 0) return -1;

  // stat starts after comma1
  int start = comma1 + 1;
  while (start < (int)res.length() && res[start] == ' ') start++;

  // stat ends at next comma or line break
  int end = res.indexOf(',', start);
  if (end < 0) end = res.indexOf('\r', start);
  if (end < 0) end = res.indexOf('\n', start);
  if (end < 0) end = res.length();

  String statStr = res.substring(start, end);
  statStr.trim();
  return statStr.toInt(); // 0,1,2,3,5,8,9,10,11...
}

static bool waitForEpsRegistration(uint32_t timeoutMs)
{
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    int stat = ceregStat();

    // 1 = registered (home), 5 = registered (roaming)
    if (stat == 1 || stat == 5) {
      SerialMon.println("EPS (LTE) registration OK.");
      return true;
    }

    // 3 = denied (fail fast)
    if (stat == 3) {
      SerialMon.println("EPS (LTE) registration DENIED.");
      dumpDiagnostics("eps registration denied");
      return false;
    }

    SerialMon.print("Waiting for EPS registration, CEREG stat=");
    SerialMon.print(stat);
    SerialMon.print("  CSQ=");
    SerialMon.println(modem.getSignalQuality());

    delay(2000);
  }

  SerialMon.println("EPS registration timeout.");
  dumpDiagnostics("eps registration timeout");
  return false;
}


static bool activateNetwork()
{
  SerialMon.print("Activating data with APN: ");
  SerialMon.println(APN);

  // Ensure we are attached to packet service before PDP
  // (Some firmwares need an explicit CGATT=1)
  printAtResponse("+CGATT=1", 10000UL);
  printAtResponse("+CGATT?", 3000UL);

  // Clear any lingering auth (Kaufland Mobil: none)
  printAtResponse("+CGAUTH=1,0", 3000UL);

  // Use TinyGSM's canonical PDP bring-up
  int retry = 3;
  while (retry-- > 0) {
    if (modem.gprsConnect(APN, APN_USER, APN_PASS)) {
      SerialMon.println("GPRS/LTE PDP connected.");
      SerialMon.print("Local IP: ");
      SerialMon.println(modem.localIP());
      return true;
    }
    SerialMon.println("PDP activation failed, retrying...");
    printAtResponse("+CGATT?", 3000UL);
    printAtResponse("+CGACT?", 3000UL);
    delay(3000);
  }

  return false;
}


static bool httpGetTest()
{
  SerialMon.print("HTTP GET ");
  SerialMon.print(HTTP_HOST);
  SerialMon.print(HTTP_PATH);
  SerialMon.println();

  if (!client.connect(HTTP_HOST, HTTP_PORT)) {
    SerialMon.println("HTTP connect failed.");
    return false;
  }

  client.print("GET ");
  client.print(HTTP_PATH);
  client.print(" HTTP/1.1\r\nHost: ");
  client.print(HTTP_HOST);
  client.print("\r\nConnection: close\r\n\r\n");

  uint32_t start = millis();
  size_t total = 0;
  while (client.connected() && millis() - start < 10000) {
    while (client.available()) {
      char c = static_cast<char>(client.read());
      if (total < 512) {
        SerialMon.write(c);
      }
      total++;
      start = millis();
    }
    delay(1);
  }
  client.stop();
  SerialMon.println();
  SerialMon.print("HTTP bytes read: ");
  SerialMon.println(total);
  return total > 0;
}



void setup()
{
  SerialMon.begin(115200);
  delay(100);

  pinMode(BOARD_LED_PIN, OUTPUT);
  digitalWrite(BOARD_LED_PIN, HIGH);

  resetModem();
  pinMode(MODEM_DTR_PIN, OUTPUT);
  digitalWrite(MODEM_DTR_PIN, LOW);

  powerOnModem();

  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  SerialMon.println("Starting modem...");
  delay(MODEM_START_WAIT_MS);
  ensureModemResponsive();

  SerialMon.print("Modem: ");
  SerialMon.println(modem.getModemName());

  String info;
  modem.sendAT("+SIMCOMATI");
  modem.waitResponse(10000UL, info);
  SerialMon.println(info);

  dumpDiagnostics("startup");

  if (!waitForSimReady(30000)) {
    dumpDiagnostics("sim not ready");
    while (true) delay(1000);
  }

  // Configure RAT + APN AFTER CPIN READY (no CFUN 0/1!)
  configureRatAndApn();

  // Now register
if (!waitForEpsRegistration(120000)) {
  while (true) delay(1000);
}

  // Activate data
  if (!activateNetwork()) {
    SerialMon.println("Data activation failed.");
    dumpDiagnostics("data activation failed");
    while (true) delay(1000);
  }

  SerialMon.print("Local IP: ");
  SerialMon.println(modem.getLocalIP());
}

void loop()
{
  static uint32_t lastStatus = 0;
  static uint32_t lastHttp = 0;

  if (millis() - lastStatus > 10000) {
    SerialMon.print("Signal quality: ");
    SerialMon.println(modem.getSignalQuality());
    SerialMon.print("Network connected: ");
    SerialMon.println(modem.isNetworkConnected() ? "yes" : "no");
    lastStatus = millis();
  }

  if (millis() - lastHttp > 60000) {
    httpGetTest();
    lastHttp = millis();
  }
}
