/*
 *  v12, + DFPlayer 2 pe PD6/PD7
 */

#include <Arduino.h>
#include <Wire.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

SoftwareSerial dfSerial(5, 4);
DFRobotDFPlayerMini dfPlayer;

SoftwareSerial dfSerial2(7, 6);   // RX=PD7, TX=PD6
DFRobotDFPlayerMini dfPlayer2;

#define SND_MIJLOC   1
#define SND_STANGA   2
#define SND_DREAPTA  3

SoftwareSerial btSerial(3, 2);

#define MPU_ADDR_STG  0x68
#define MPU_ADDR_DR   0x69
#define REG_PWR_MGMT  0x6B
#define REG_ACCEL_CFG 0x1C
#define REG_GYRO_CFG  0x1B
#define REG_CONFIG    0x1A

#define HIT_THRESHOLD   6000L
#define DETECT_WINDOW   6
#define AXIS_DY_ZONE    4000
#define REST_THRESHOLD  1500L
#define CALM_SAMPLES    2
#define MAX_COOLING_MS  250UL

#define USER_LED 13

#define BTN_PIN 8  // PB0 

enum State { READY, DETECTING, COOLING };

// pointer df in struct
struct Bat {
    uint8_t               addr;
    const char*           nume;
    DFRobotDFPlayerMini*  df;
    int16_t               ax_off, ay_off, az_off;
    State                 state;
    uint8_t               detectN;
    int32_t               peak_pos;
    int32_t               peak_neg;
    uint8_t               calmN;
    uint32_t              coolingStart;
};

// bat1 foloseste dfPlayer, bat2 foloseste dfPlayer2 
static Bat bat1 = { MPU_ADDR_STG, "STG", &dfPlayer,  0,0,0, READY,0,0,0,0,0 };
static Bat bat2 = { MPU_ADDR_DR,  "DR",  &dfPlayer2, 0,0,0, READY,0,0,0,0,0 };
static uint32_t hitCount = 0;

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
    mpu_write_reg(addr, REG_ACCEL_CFG, 0x10);
    mpu_write_reg(addr, REG_GYRO_CFG,  0x08);
    mpu_write_reg(addr, REG_CONFIG,    0x03);
}

static void calibrate(Bat &b) {
    Serial.print(F("Calibrare ")); Serial.print(b.nume);
    Serial.println(F("..."));
    int32_t sx=0, sy=0, sz=0;
    int16_t ax, ay, az;
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
    uint8_t     sound;
    const char* toba;
    const char* simbol;

    if (b.peak_pos > abs(b.peak_neg) && b.peak_pos > AXIS_DY_ZONE) {
        toba="DREAPTA"; simbol=" >>> "; sound=SND_DREAPTA;
    } else if (abs(b.peak_neg) > b.peak_pos && abs(b.peak_neg) > AXIS_DY_ZONE) {
        toba="STANGA";  simbol=" <<< "; sound=SND_STANGA;
    } else {
        toba="MIJLOC";  simbol=" [*] "; sound=SND_MIJLOC;
    }

    // f iecare bata reda pe propriul DFPlayer
    b.df->play(sound);

    btSerial.print(b.nume);
    btSerial.print(',');
    btSerial.println(toba);

    lcd.setCursor(0, 1);
    lcd.print(b.nume);
    lcd.print(F(": "));
    lcd.print(toba);
    lcd.print(F("        "));

    Serial.print(F("#")); Serial.print(hitCount);
    Serial.print(F(" [")); Serial.print(b.nume); Serial.print(F("]"));
    Serial.print(simbol); Serial.println(toba);

    digitalWrite(USER_LED, HIGH);
    // delay(15);
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

void setup() {
    Serial.begin(57600);
    pinMode(USER_LED, OUTPUT);

    Wire.begin();
    Wire.setClock(400000);

    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print(F("  Stil: ROCK    "));
    lcd.setCursor(0, 1);
    lcd.print(F("                "));

    //  DFPlayer 1 
    dfSerial.begin(9600);
    delay(1000);
    if (!dfPlayer.begin(dfSerial, false)) {
        Serial.println(F("DF1 ERR!"));
        lcd.setCursor(0, 1);
        lcd.print(F("DF1 ERR!        "));
    } else {
        Serial.println(F("DF1 OK"));
        dfPlayer.volume(25);
        dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
        delay(3000);
        Serial.print(F("DF1 fisiere: "));
        Serial.println(dfPlayer.readFileCounts());
    }
    Serial.print(F("DF1 state: "));
    Serial.println(dfPlayer.readState());

    // Initializare DFPlayer 2 
    dfSerial2.begin(9600);
    delay(1000);
    if (!dfPlayer2.begin(dfSerial2, false)) {
        Serial.println(F("DF2 ERR!"));
        lcd.setCursor(0, 1);
        lcd.print(F("DF2 ERR!        "));
    } else {
        Serial.println(F("DF2 OK"));
        dfPlayer2.volume(30);
        dfPlayer2.EQ(DFPLAYER_EQ_NORMAL);
        delay(3000);
        Serial.print(F("DF2 fisiere: "));
        Serial.println(dfPlayer2.readFileCounts());
    }
    Serial.print(F("DF2 state: "));
    Serial.println(dfPlayer2.readState());

    //  Bluetooth 
    btSerial.begin(9600);
    btSerial.println(F("AEROBEAT_READY"));

    //  MPU-6050 
    mpu_init(MPU_ADDR_STG);
    mpu_init(MPU_ADDR_DR);
    delay(100);

    lcd.setCursor(0, 1);
    lcd.print(F("Calibrare...    "));
    calibrate(bat1);
    calibrate(bat2);

    lcd.setCursor(0, 1);
    lcd.print(F("Ready!          "));
    Serial.println(F("AERO BEAT v12 ready"));

    for (int i=0; i<3; i++) {
        digitalWrite(USER_LED, HIGH); delay(80);
        digitalWrite(USER_LED, LOW);  delay(80);
    }

    delay(1000);
    lcd.setCursor(0, 1);
    lcd.print(F("                "));
}

static bool paused = false;

void loop() {
    if (digitalRead(BTN_PIN) == HIGH) {
        delay(50);
        if (digitalRead(BTN_PIN) == HIGH) {
            while (digitalRead(BTN_PIN) == HIGH);
            delay(50);

            if (!paused) {
                // Prima apasare ->PAUZA
                paused = true;
                dfPlayer.stop();
                dfPlayer2.stop();
                bat1.state = READY; bat1.calmN = 0;
                bat2.state = READY; bat2.calmN = 0;

                // 0004.mp3 pe cardul SD al DF1
                dfPlayer.loop(4);

                lcd.setCursor(0, 0);
                lcd.print(F("  ** PAUSED **  "));
                lcd.setCursor(0, 1);
                lcd.print(F("Apasa pt resume "));

            } else {
                // A doua apasare ->RESUME + recalibrare
                paused = false;
                dfPlayer.stop();

                lcd.setCursor(0, 0);
                lcd.print(F("  Stil: ROCK    "));
                lcd.setCursor(0, 1);
                lcd.print(F("Calibrare...    "));

                calibrate(bat1);
                calibrate(bat2);

                lcd.setCursor(0, 1);
                lcd.print(F("Ready!          "));
            }
        }
    }

    if (!paused) {
        process(bat1);
        process(bat2);
    }

    delay(5);
}