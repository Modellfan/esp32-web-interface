// T-Call A7670E (pinout v1.0) GPS showcase based on LilyGo GPS examples.
// Copy this file to a .ino sketch if you want to compile it directly.

#include <Arduino.h>

#define TINY_GSM_RX_BUFFER 1024
#define SerialMon Serial
#define TINY_GSM_DEBUG SerialMon
#define TINY_GSM_MODEM_A7670

#include <TinyGsmClient.h>

#define SerialAT Serial1

// T-Call A7670 v1.0 pinout (from LilyGo utilities.h)
static const int MODEM_DTR_PIN = 14;
static const int MODEM_TX_PIN = 26;
static const int MODEM_RX_PIN = 25;
static const int BOARD_PWRKEY_PIN = 4;
static const int BOARD_LED_PIN = 12;
static const int MODEM_RING_PIN = 13;
static const int MODEM_RESET_PIN = 27;
static const int MODEM_RESET_LEVEL = LOW;

static const int MODEM_GPS_ENABLE_GPIO = -1;
static const int MODEM_GPS_ENABLE_LEVEL = -1;

static const int MODEM_POWERON_PULSE_WIDTH_MS = 100;
static const int MODEM_START_WAIT_MS = 3000;

TinyGsm modem(SerialAT);

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

static void ensureModemResponsive()
{
  int retry = 0;
  while (!modem.testAT(1000)) {
    SerialMon.println(".");
    if (retry++ > 30) {
      powerOnModem();
      retry = 0;
    }
  }
  SerialMon.println();
}

static bool modemHasBuiltInGps(const String& modemName)
{
  if (modemName.startsWith("A7670E-FASE") || modemName.startsWith("A7670SA-FASE")) {
    return true;
  }
  if (modemName.startsWith("A7670E-LNXY-UBL")
      || modemName.startsWith("A7670E-LNMV")
      || modemName.startsWith("A7670SA-LASE")
      || modemName.startsWith("A7670SA-LASC")
      || modemName.startsWith("A7670G-LLSE")
      || modemName.startsWith("A7670G-LABE")
      || modemName.startsWith("A7670E-LASE ")) {
    return false;
  }
  return true;
}

static bool enableGpsWithRetry()
{
  int retry = 0;
  while (!modem.enableGPS(MODEM_GPS_ENABLE_GPIO, MODEM_GPS_ENABLE_LEVEL)) {
    SerialMon.print(".");
    if (retry++ > 30) {
      SerialMon.println(" GPS start failed, retrying...");
      retry = 0;
    }
    delay(1000);
  }
  SerialMon.println();
  return true;
}

static void printGpsBasic()
{
  float lat = 0;
  float lon = 0;
  float speed = 0;
  float alt = 0;
  int vsat = 0;
  int usat = 0;
  float acc = 0;
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  uint8_t fixMode = 0;

  if (modem.getGPS(&fixMode, &lat, &lon, &speed, &alt, &vsat, &usat, &acc,
                   &year, &month, &day, &hour, &minute, &second)) {
    SerialMon.print("FixMode: "); SerialMon.println(fixMode);
    SerialMon.print("Latitude: "); SerialMon.print(lat, 6);
    SerialMon.print(" Longitude: "); SerialMon.println(lon, 6);
    SerialMon.print("Speed: "); SerialMon.print(speed);
    SerialMon.print(" Altitude: "); SerialMon.println(alt);
    SerialMon.print("Visible Satellites: "); SerialMon.print(vsat);
    SerialMon.print(" Used Satellites: "); SerialMon.println(usat);
    SerialMon.print("Accuracy: "); SerialMon.println(acc);
    SerialMon.print("Date: "); SerialMon.print(year);
    SerialMon.print("-"); SerialMon.print(month);
    SerialMon.print("-"); SerialMon.println(day);
    SerialMon.print("Time: "); SerialMon.print(hour);
    SerialMon.print(":"); SerialMon.print(minute);
    SerialMon.print(":"); SerialMon.println(second);
  } else {
    SerialMon.println("No GPS fix yet.");
  }
}

static void printGpsExtended()
{
#if defined(TINY_GSM_MODEM_HAS_GPS_EX)
  GPSInfo info;
  if (modem.getGPS_Ex(info)) {
    SerialMon.print("FixMode: "); SerialMon.println(info.isFix);
    SerialMon.print("Latitude: "); SerialMon.println(info.latitude, 6);
    SerialMon.print("Longitude: "); SerialMon.println(info.longitude, 6);
    SerialMon.print("Speed: "); SerialMon.println(info.speed);
    SerialMon.print("Altitude: "); SerialMon.println(info.altitude);
    SerialMon.print("GPS Satellites: "); SerialMon.println(info.gps_satellite_num);
    SerialMon.print("BEIDOU Satellites: "); SerialMon.println(info.beidou_satellite_num);
    SerialMon.print("GLONASS Satellites: "); SerialMon.println(info.glonass_satellite_num);
    SerialMon.print("GALILEO Satellites: "); SerialMon.println(info.galileo_satellite_num);
    SerialMon.print("Date: "); SerialMon.print(info.year);
    SerialMon.print("-"); SerialMon.print(info.month);
    SerialMon.print("-"); SerialMon.println(info.day);
    SerialMon.print("Time: "); SerialMon.print(info.hour);
    SerialMon.print(":"); SerialMon.print(info.minute);
    SerialMon.print(":"); SerialMon.println(info.second);
    SerialMon.print("Course: "); SerialMon.println(info.course);
    SerialMon.print("PDOP: "); SerialMon.println(info.PDOP);
    SerialMon.print("HDOP: "); SerialMon.println(info.HDOP);
    SerialMon.print("VDOP: "); SerialMon.println(info.VDOP);
  } else {
    SerialMon.println("No extended GPS fix yet.");
  }
#endif
}

static bool parseGnssInfo(const String& raw, int* gps, int* bds, int* glo, int* gal,
                          String* date, String* time)
{
  if (raw.length() == 0) {
    return false;
  }
  String parts[16];
  int partCount = 0;
  String current;
  for (size_t i = 0; i < raw.length(); ++i) {
    char c = raw[i];
    if (c == ',') {
      if (partCount < 16) {
        parts[partCount++] = current;
      }
      current = "";
    } else {
      current += c;
    }
  }
  if (partCount < 16) {
    parts[partCount++] = current;
  }
  if (partCount < 11) {
    return false;
  }
  if (gps) *gps = parts[1].length() ? parts[1].toInt() : 0;
  if (bds) *bds = parts[2].length() ? parts[2].toInt() : 0;
  if (glo) *glo = parts[3].length() ? parts[3].toInt() : 0;
  if (gal) *gal = parts[4].length() ? parts[4].toInt() : 0;
  if (date) *date = parts[9];
  if (time) *time = parts[10];
  return true;
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

  String modemName = modem.getModemName();
  SerialMon.print("Modem: ");
  SerialMon.println(modemName);
  if (!modemHasBuiltInGps(modemName)) {
    SerialMon.println("This modem variant does not have built-in GPS.");
    while (true) {
      delay(1000);
    }
  }

  String info;
  modem.sendAT("+SIMCOMATI");
  modem.waitResponse(10000UL, info);
  SerialMon.println(info);

  SerialMon.println("CGNSSINFO format sample:");
  SerialMon.println("+CGNSSINFO: 2,04,00,21.xxxxxx,N,114.xxxxxxxx,E,020924,094145.00,-34.0,1.403,,6.9,6.8,1.0,03");

  SerialMon.println("Enabling GPS...");
  enableGpsWithRetry();

  SerialMon.print("GPS enabled: ");
  SerialMon.println(modem.isEnableGPS() ? "yes" : "no");

  modem.setGPSBaud(115200);
  modem.setGPSMode(GNSS_MODE_GPS_BDS_GALILEO_SBAS_QZSS);

  SerialMon.print("AGPS: ");
  SerialMon.println(modem.enableAGPS() ? "enabled" : "failed");

  SerialMon.print("Hot start: ");
  SerialMon.println(modem.gpsHotStart() ? "ok" : "failed");

  const uint32_t waitMs = 600000;
  const uint32_t intervalMs = 15000;
  uint32_t start = millis();
  while (millis() - start < waitMs) {
    if (!modem.isEnableGPS()) {
      SerialMon.println("GPS not enabled, restarting...");
      enableGpsWithRetry();
    }
    int gps = 0;
    int bds = 0;
    int glo = 0;
    int gal = 0;
    String date;
    String time;
    String raw = modem.getGPSraw();
    SerialMon.print("GNSS raw: ");
    SerialMon.println(raw);
    if (parseGnssInfo(raw, &gps, &bds, &glo, &gal, &date, &time)) {
      SerialMon.print("Satellites GPS: "); SerialMon.print(gps);
      SerialMon.print(" BDS: "); SerialMon.print(bds);
      SerialMon.print(" GLONASS: "); SerialMon.print(glo);
      SerialMon.print(" GALILEO: "); SerialMon.println(gal);
      if (date.length() >= 6 && time.length() >= 6) {
        SerialMon.print("UTC date/time: "); SerialMon.print(date);
        SerialMon.print(" "); SerialMon.println(time);
      } else {
        SerialMon.println("UTC date/time: not available yet");
      }
    } else {
      SerialMon.println("GNSS info not available yet");
    }

    SerialMon.println("Basic GPS fix:");
    printGpsBasic();
    SerialMon.println("Extended GPS fix:");
    printGpsExtended();

    delay(intervalMs);
  }

  SerialMon.println("Disabling GPS...");
  modem.disableGPS(MODEM_GPS_ENABLE_GPIO, !MODEM_GPS_ENABLE_LEVEL);
}

void loop()
{
  delay(1000);
}
