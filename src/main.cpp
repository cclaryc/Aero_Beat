/*
 * detector_tobe.cpp  –  v8, 2 bete + DFPlayer Mini
 *
 * Hardware:
 *   Bat STANGA  : MPU-6050 AD0=GND  → 0x68
 *   Bat DREAPTA : MPU-6050 AD0=3.3V → 0x69
 *   DFPlayer    : RX ← PD4 (pin 4) prin 1kΩ
 *                 TX → PD5 (pin 5) direct
 *                 DAC_R / DAC_L → modul jack → amplificator
 *
 * Fisiere pe SD card (folder /mp3/ sau root, 0001.mp3 etc.):
 *   1 → Snare  (toba mijloc)
 *   2 → Hi-hat (toba stanga)
 *   3 → Bass   (toba dreapta)
 */

#include <Arduino.h>
#include <Wire.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>

// ── DFPlayer ──────────────────────────────────────────────
// PD4 = pin 4 = TX spre DFPlayer RX (prin 1kΩ)
// PD5 = pin 5 = RX de la DFPlayer TX
SoftwareSerial dfSerial(5, 4);   // (RX_arduino, TX_arduino)
DFRobotDFPlayerMini dfPlayer;

// Numere de fisiere pe SD card
#define SND_MIJLOC   1   // snare
#define SND_STANGA   2   // hi-hat
#define SND_DREAPTA  3   // bass drum

// ── I2C senzori ───────────────────────────────────────────
#define MPU_ADDR_STG  0x68
#define MPU_ADDR_DR   0x69

#define REG_PWR_MGMT  0x6B
#define REG_ACCEL_CFG 0x1C
#define REG_GYRO_CFG  0x1B
#define REG_CONFIG    0x1A

// ── Parametri detectie ────────────────────────────────────
#define HIT_THRESHOLD   6000L
#define DETECT_WINDOW   10
#define AXIS_DY_ZONE    4000
#define REST_THRESHOLD  1500L
#define CALM_SAMPLES    4
#define MAX_COOLING_MS  400UL

#define USER_LED 13

// ── Struct bat ────────────────────────────────────────────
enum State { READY, DETECTING, COOLING };

struct Bat {
    uint8_t     addr;
    const char* nume;
    int16_t     ax_off, ay_off, az_off;
    State       state;
    uint8_t     detectN;
    int32_t     peak_pos;
    int32_t     peak_neg;
    uint8_t     calmN;
    uint32_t    coolingStart;
};

static Bat bat1 = { MPU_ADDR_STG, "STG", 0,0,0, READY,0,0,0,0,0 };
static Bat bat2 = { MPU_ADDR_DR,  "DR",  0,0,0, READY,0,0,0,0,0 };
static uint32_t hitCount = 0;

// ─────────────────────────────────────────────────────────

static void mpu_write_reg(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg); Wire.write(val);
    Wire.endTransmission();
}

static void mpu_read(uint8_t addr, int16_t &ax, int16_t &ay, int16_t &az) {
    Wire.beginTransmission(addr);
    Wire.write(0x3B);
    Wire.endTransmission(false);
    Wire.requestFrom(addr, (uint8_t)6, (uint8_t)true);
    ax = (Wire.read() << 8) | Wire.read();
    ay = (Wire.read() << 8) | Wire.read();
    az = (Wire.read() << 8) | Wire.read();
}

static void mpu_init(uint8_t addr) {
    mpu_write_reg(addr, REG_PWR_MGMT,  0x00);
    delay(50);
    mpu_write_reg(addr, REG_ACCEL_CFG, 0x10); // +-8g
    mpu_write_reg(addr, REG_GYRO_CFG,  0x08); // +-500°/s
    mpu_write_reg(addr, REG_CONFIG,    0x03); // DLPF 44Hz
}

static void calibrate(Bat &b) {
    Serial.print(F("Calibrare bat "));
    Serial.print(b.nume); Serial.println(F("... nemiscat!"));
    int32_t sx=0,sy=0,sz=0;
    int16_t ax,ay,az;
    for (uint16_t i=0; i<200; i++) {
        mpu_read(b.addr, ax, ay, az);
        sx+=ax; sy+=ay; sz+=az;
        delay(5);
    }
    b.ax_off=sx/200; b.ay_off=sy/200; b.az_off=sz/200;
    Serial.print(F("  OK ay_off=")); Serial.println(b.ay_off);
}

static void classify(Bat &b) {
    hitCount++;

    const char *toba;
    const char *simbol;
    uint8_t     sound;

    if (b.peak_pos > abs(b.peak_neg) && b.peak_pos > AXIS_DY_ZONE) {
        toba   = "TOBA DREAPTA";
        simbol = " >>> ";
        sound  = SND_DREAPTA;
    } else if (abs(b.peak_neg) > b.peak_pos && abs(b.peak_neg) > AXIS_DY_ZONE) {
        toba   = "TOBA STANGA";
        simbol = " <<< ";
        sound  = SND_STANGA;
    } else {
        toba   = "TOBA MIJLOC";
        simbol = " [*] ";
        sound  = SND_MIJLOC;
    }

    // ── Reda sunetul ──────────────────────────────────────
    dfPlayer.play(sound);

    // ── Serial debug ──────────────────────────────────────
    Serial.print(F("#")); Serial.print(hitCount);
    Serial.print(F(" [")); Serial.print(b.nume); Serial.print(F("]"));
    Serial.print(simbol); Serial.println(toba);

    digitalWrite(USER_LED, HIGH);
    delay(15);
    digitalWrite(USER_LED, LOW);
}

static void process(Bat &b) {
    int16_t ax, ay, az;
    mpu_read(b.addr, ax, ay, az);

    int32_t dx = (int32_t)ax - b.ax_off;
    int32_t dy = (int32_t)ay - b.ay_off;
    int32_t dz = (int32_t)az - b.az_off;
    int32_t mag = abs(dx) + abs(dy) + abs(dz);

    switch (b.state) {
        case READY:
            if (mag > HIT_THRESHOLD) {
                b.detectN  = 1;
                b.peak_pos = (dy > 0) ? dy : 0;
                b.peak_neg = (dy < 0) ? dy : 0;
                b.state    = DETECTING;
            }
            break;

        case DETECTING:
            if (dy > b.peak_pos) b.peak_pos = dy;
            if (dy < b.peak_neg) b.peak_neg = dy;
            b.detectN++;
            if (b.detectN >= DETECT_WINDOW) {
                classify(b);
                b.coolingStart = millis();
                b.calmN = 0;
                b.state = COOLING;
            }
            break;

        case COOLING:
            if (mag < REST_THRESHOLD) {
                b.calmN++;
                if (b.calmN >= CALM_SAMPLES) {
                    b.state = READY; b.calmN = 0;
                }
            } else if ((millis() - b.coolingStart) > MAX_COOLING_MS) {
                b.state = READY; b.calmN = 0;
            } else {
                b.calmN = 0;
            }
            break;
    }
}

// ─────────────────────────────────────────────────────────

void setup() {
    Serial.begin(57600);
    pinMode(USER_LED, OUTPUT);

    // ── DFPlayer ──────────────────────────────────────────
    dfSerial.begin(9600);
    delay(200);
if (!dfPlayer.begin(dfSerial, false)) {
        Serial.println(F("DFPlayer ERR! Verifica SD si conexiunile."));
    } else {
        Serial.println(F("DFPlayer OK"));
        dfPlayer.volume(25);   // volum 0-30
        dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
    }
Serial.print(F("DFPlayer state: "));
Serial.println(dfPlayer.readState());
Serial.print(F("Fisiere pe card: "));
Serial.println(dfPlayer.readFileCounts());
    // ── MPU-6050 ──────────────────────────────────────────
    Wire.begin();
    Wire.setClock(400000);
    mpu_init(MPU_ADDR_STG);
    mpu_init(MPU_ADDR_DR);
    delay(100);

    calibrate(bat1);
    calibrate(bat2);

    Serial.println(F("\r\n=============================="));
    Serial.println(F("  AERO BEAT v8  –  2 bete"));
    Serial.println(F("=============================="));

    for (int i=0; i<3; i++) {
        digitalWrite(USER_LED, HIGH); delay(80);
        digitalWrite(USER_LED, LOW);  delay(80);
    }
}

void loop() {
    process(bat1);
    process(bat2);
    delay(5);
}