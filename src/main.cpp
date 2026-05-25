/*
 * detector_tobe.cpp  –  v12, + DFPlayer 2 pe PD6/PD7
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

SoftwareSerial btSerial(2, 3);

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

#define BTN_PIN  8   // PB0 = pin 8 - pauza/resume
#define BTN2_PIN 9   // PB1 = pin 9 - schimba stil

//  LED RGB  ar anod comun  
#define LED_R 10  // PB2 - PWM
#define LED_G 11  // PB3 - PWM
#define LED_B 12  // PB4 - fara PWM

// ROCK:  DF1 fis 1,2,3  |  DF2 fis 1,2,3
// METAL: DF1 fis 4,5,6  |  DF2 fis 5,6,7
enum Style { ROCK, METAL, BILLIE_JEAN };
static Style currentStyle = ROCK;

//  var LED RGB 
static uint8_t r_target=0, g_target=0, b_target=0;
static uint8_t r_cur=0,    g_cur=0,    b_cur=0;
static uint32_t ledOffMs = 0;

static uint8_t stepTo(uint8_t cur, uint8_t target, uint8_t step) {
    if (cur < target) return (target-cur <= step) ? target : cur+step;
    if (cur > target) return (cur-target <= step) ? target : cur-step;
    return cur;
}

static void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
    r_target=r; g_target=g; b_target=b;
}

static void updateLed() {
    if (millis() > ledOffMs) setLedColor(0,0,0);
    r_cur = stepTo(r_cur, r_target, 8);
    g_cur = stepTo(g_cur, g_target, 8);
    b_cur = stepTo(b_cur, b_target, 8);
    // anod comun: invert (0=aprins, 255=stins)
    analogWrite(LED_R, 255 - r_cur);
    analogWrite(LED_G, 255 - g_cur);
    digitalWrite(LED_B, b_cur > 127 ? LOW : HIGH);
}

enum State { READY, DETECTING, COOLING };

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

//  bata1 foloseste dfPlayer, bata2 foloseste dfPlayer2 
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

    uint8_t base = (currentStyle == ROCK)  ? 1 :
                (currentStyle == METAL) ? (b.df == &dfPlayer ? 4 : 5) :
                /* BILLIE_JEAN */         8;

    if (b.peak_pos > abs(b.peak_neg) && b.peak_pos > AXIS_DY_ZONE) {
        toba="DREAPTA"; simbol=" >>> ";
        sound = (currentStyle == BILLIE_JEAN) ? base+1 : base+2;
    } else if (abs(b.peak_neg) > b.peak_pos && abs(b.peak_neg) > AXIS_DY_ZONE) {
        toba="STANGA";  simbol=" <<< ";
        sound = (currentStyle == BILLIE_JEAN) ? base+2 : base+1;
    } else {
        toba="MIJLOC";  simbol=" [*] "; sound=base;
}

    //  fiecare bata reda pe propriul DFPlayer 
    b.df->play(sound);

    //  LED RGB: culoare per toba
    // STANGA=rosu, MIJLOC=galben, DREAPTA=mov
    if (sound == base+1)      setLedColor(255, 0,   0);   // rosu
    else if (sound == base)   setLedColor(255, 200, 0);   // galben
    else                      setLedColor(255, 0,   255); // mov
    ledOffMs = millis() + 600;

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
    pinMode(BTN_PIN, INPUT);
    pinMode(BTN2_PIN, INPUT);
    pinMode(LED_R, OUTPUT);
    pinMode(LED_G, OUTPUT);
    pinMode(LED_B, OUTPUT);
    // Stinge LED initial (anod comun: HIGH=stins)
    digitalWrite(LED_R, HIGH);
    digitalWrite(LED_G, HIGH);
    digitalWrite(LED_B, HIGH);

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

    //  5. Initializare DFPlayer 2 
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

    //  Bluetooth ─
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
                // Prima apasare  PAUZA
                paused = true;
                dfPlayer.stop();
                dfPlayer2.stop();
                bat1.state = READY; bat1.calmN = 0;
                bat2.state = READY; bat2.calmN = 0;

                // 0004.mp3 pe cardul SD al DF1
                dfPlayer2.loop(4);

                lcd.setCursor(0, 0);
                lcd.print(F("  ** PAUSED **  "));
                lcd.setCursor(0, 1);
                lcd.print(F("Apasa pt resume "));

            } else {
                // A doua apasare  RESUME + recalibrare
                paused = false;
                dfPlayer.stop();

                lcd.setCursor(0, 0);
                if      (currentStyle == ROCK)        lcd.print(F("  Stil: ROCK    "));
                else if (currentStyle == METAL)       lcd.print(F("  Stil: METAL   "));
                else                                  lcd.print(F(" Billie Jean    "));
                lcd.setCursor(0, 1);
                lcd.print(F("Calibrare...    "));

                calibrate(bat1);
                calibrate(bat2);

                lcd.setCursor(0, 1);
                lcd.print(F("Ready!          "));
            }
        }
    }

    // Buton stil: ROCK  METAL
    if (digitalRead(BTN2_PIN) == HIGH) {
        delay(50);
        if (digitalRead(BTN2_PIN) == HIGH) {
            while (digitalRead(BTN2_PIN) == HIGH);
            delay(50);
            currentStyle = (currentStyle == ROCK)  ? METAL :
                        (currentStyle == METAL) ? BILLIE_JEAN : ROCK;

            lcd.setCursor(0, 0);
            if      (currentStyle == ROCK)        lcd.print(F("  Stil: ROCK    "));
            else if (currentStyle == METAL)       lcd.print(F("  Stil: METAL   "));
            else                                  lcd.print(F(" Billie Jean    "));
        }
    }

    if (!paused) {
        process(bat1);
        process(bat2);
    }

    updateLed();
    delay(5);
}