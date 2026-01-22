/**
 * @file      loramac.cpp
 * LMIC library only support SX1276 Radio
 */


#include <Arduino.h>
#include <lmic.h>
#include <hal/hal.h>
#include "shield.h"

// Chose LSB mode on the console and then copy it here.
static const u1_t PROGMEM APPEUI[8] = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xBB, 0xAA};
// LSB mode
static const u1_t PROGMEM DEVEUI[8] = {0x8D, 0x6F, 0x06, 0xD0, 0x7E, 0xD5, 0xB3, 0x70};
// MSB mode
static const u1_t PROGMEM APPKEY[16] = {0xF4, 0x84, 0x2F, 0xE1, 0x08, 0x77, 0xCC, 0xAF, 0x31, 0x90, 0x9D, 0xB3, 0x45, 0x04, 0x90, 0xFD};



// Pin mapping
const lmic_pinmap lmic_pins = {
    .nss =  LORA_SS,
    .rxtx = LMIC_UNUSED_PIN,
    .rst =  LORA_RST,
    .dio = {LORA_DI0, RADIO_DIO_1, RADIO_DIO_2}
};

static osjob_t sendjob;
static int spreadFactor = DR_SF7;
static int joinStatus = EV_JOINING;
static const unsigned TX_INTERVAL = 30;
static String lora_msg = "";

void os_getArtEui (u1_t *buf)
{
    memcpy_P(buf, APPEUI, 8);
}

void os_getDevEui (u1_t *buf)
{
    memcpy_P(buf, DEVEUI, 8);
}

void os_getDevKey (u1_t *buf)
{
    memcpy_P(buf, APPKEY, 16);
}


void do_send(osjob_t *j)
{
    if (joinStatus == EV_JOINING) {
        Serial.println(F("Not joined yet"));
        // Check if there is not a current TX/RX job running
        os_setTimedCallback(&sendjob, os_getTime() + sec2osticks(TX_INTERVAL), do_send);

    } else if (LMIC.opmode & OP_TXRXPEND) {
        Serial.println(F("OP_TXRXPEND, not sending"));
    } else {
        Serial.println(F("OP_TXRXPEND,sending ..."));
        static uint8_t mydata[] = "Hello, world!";
        // Prepare upstream data transmission at the next possible time.
        LMIC_setTxData2(1, mydata, sizeof(mydata) - 1, 0);
        os_setTimedCallback(&sendjob, os_getTime() + sec2osticks(TX_INTERVAL), do_send);
    }
}

void onEvent (ev_t ev)
{
    Serial.print(os_getTime());
    Serial.print(": ");
    switch (ev) {
    case EV_TXCOMPLETE:
        Serial.println(F("EV_TXCOMPLETE (includes waiting for RX windows)"));

        if (LMIC.txrxFlags & TXRX_ACK) {
            Serial.println(F("Received ack"));
            lora_msg =  "Received ACK.";
        }

        lora_msg = "rssi:" + String(LMIC.rssi) + " snr: " + String(LMIC.snr);

        if (LMIC.dataLen) {
            // data received in rx slot after tx
            Serial.print(F("Data Received: "));
            // Serial.write(LMIC.frame + LMIC.dataBeg, LMIC.dataLen);
            // Serial.println();
            Serial.println(LMIC.dataLen);
            Serial.println(F(" bytes of payload"));
        }
        // Schedule next transmission
        os_setTimedCallback(&sendjob, os_getTime() + sec2osticks(TX_INTERVAL), do_send);
        break;
    case EV_JOINING:
        Serial.println(F("EV_JOINING: -> Joining..."));
        lora_msg = "OTAA joining....";
        joinStatus = EV_JOINING;
        break;
    case EV_JOIN_FAILED:
        Serial.println(F("EV_JOIN_FAILED: -> Joining failed"));
        lora_msg = "OTAA Joining failed";
        break;
    case EV_JOINED:
        Serial.println(F("EV_JOINED"));
        lora_msg = "Joined!";
        joinStatus = EV_JOINED;
        delay(3);
        // Disable link check validation (automatically enabled
        // during join, but not supported by TTN at this time).
        LMIC_setLinkCheckMode(0);

        break;
    case EV_RXCOMPLETE:
        // data received in ping slot
        Serial.println(F("EV_RXCOMPLETE"));
        break;
    case EV_LINK_DEAD:
        Serial.println(F("EV_LINK_DEAD"));
        break;
    case EV_LINK_ALIVE:
        Serial.println(F("EV_LINK_ALIVE"));
        break;
    default:
        Serial.println(F("Unknown event"));
        break;
    }
}

u1_t readReg (u1_t addr)
{
    hal_pin_nss(0);
    hal_spi(addr & 0x7F);
    u1_t val = hal_spi(0x00);
    hal_pin_nss(1);
    return val;
}

void setupLMIC(void)
{

#ifdef  RADIO_TCXO_ENABLE
    pinMode(RADIO_TCXO_ENABLE, OUTPUT);
    digitalWrite(RADIO_TCXO_ENABLE, HIGH);
#endif

    // LMIC init
    os_init();

    // Reset the MAC state. Session and pending data transfers will be discarded.
    LMIC_reset();

    LMIC_setClockError(MAX_CLOCK_ERROR * 1 / 100);
    // Set up the channels used by the Things Network, which corresponds
    // to the defaults of most gateways. Without this, only three base
    // channels from the LoRaWAN specification are used, which certainly
    // works, so it is good for debugging, but can overload those
    // frequencies, so be sure to configure the full frequency range of
    // your network here (unless your network autoconfigures them).
    // Setting up channels should happen after LMIC_setSession, as that
    // configures the minimal channel set.

    LMIC_setupChannel(0, 868100000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(1, 868300000, DR_RANGE_MAP(DR_SF12, DR_SF7B), BAND_CENTI);      // g-band
    LMIC_setupChannel(2, 868500000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(3, 867100000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(4, 867300000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(5, 867500000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(6, 867700000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(7, 867900000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(8, 868800000, DR_RANGE_MAP(DR_FSK,  DR_FSK),  BAND_MILLI);      // g2-band
    // TTN defines an additional channel at 869.525Mhz using SF9 for class B
    // devices' ping slots. LMIC does not have an easy way to define set this
    // frequency and support for class B is spotty and untested, so this
    // frequency is not configured here.

    // Disable link check validation
    LMIC_setLinkCheckMode(0);

    // TTN uses SF9 for its RX2 window.
    LMIC.dn2Dr = DR_SF9;

    // Set data rate and transmit power for uplink (note: txpow seems to be ignored by the library)
    LMIC_setDrTxpow(spreadFactor, 14);


    Serial.println("LMIC_startJoining");
    // Start job
    LMIC_startJoining();


    do_send(&sendjob);     // Will fire up also the join
}

void loopLMIC(void)
{
    os_runloop_once();
}


