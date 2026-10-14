/*
  MASALA PACKER - Arduino Mega 2560 main controller
  ---------------------------------------------------------------
  6 dispensers (DC gear motor + auger / vane / vibration tray) fill ONE
  weigh hopper (load cell + HX711) one after another. After all 6 are
  done the hopper trap door (servo) opens, the spices fall into the bag
  under the funnel, then an optional impulse sealer closes the pouch.
  An ESP32 on Serial2 sends data to the cloud (MQTT) and receives commands.

  CHANNELS (edit names/targets below)
   0 Tejpat (bay leaf)   1 Dalchini (cinnamon)   2 Long (clove)
   3 Sukumel             4 Timur                 5 Alainchi (cardamom)

  PIN MAP (Mega)
   Motor MOSFET gates : D2 D3 D4 D5 D6 D7   (logic-level MOSFET, e.g. IRLZ44N)
   Hopper door servo  : D9   (power the servo from its own 5-6V 3A supply)
   HX711              : DT = D30, SCK = D31
   Start button       : D32 -> GND (INPUT_PULLUP)
   E-stop (NC switch) : D33 -> GND (INPUT_PULLUP)  open circuit = STOP
   Bag IR sensor      : D34 (LOW = bag present)
   Sealer relay       : D35 (active-LOW relay module)
   Buzzer / LED       : D36 / D37
   LCD 16x2 I2C       : SDA 20, SCL 21 (address 0x27, change if needed)
   ESP32 link         : Mega TX2(16) -> divider -> ESP32 RX2(16)
                        Mega RX2(17) <- ESP32 TX2(17)   GND common

  SERIAL / MQTT COMMANDS (USB serial or via ESP32)
   START | STOP | TARE | CAL <grams> | SET <ch 0-5> <grams> | STATUS

  Libraries: HX711 (bogde), LiquidCrystal_I2C, Servo, EEPROM
  FIRST TESTS: use rice/lentils, no heat sealer, keep fingers away.
  Hardware E-stop MUST also cut motor power - not only this software.
*/

#include <HX711.h>
#include <Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

// ---------------- pins ----------------
#define N_CH 6
const uint8_t MOTOR_PIN[N_CH] = {2, 3, 4, 5, 6, 7};
const uint8_t PIN_DOOR  = 9;
const uint8_t PIN_HX_DT = 30, PIN_HX_SCK = 31;
const uint8_t PIN_START = 32, PIN_ESTOP = 33, PIN_BAG = 34;
const uint8_t PIN_SEAL  = 35, PIN_BUZZ = 36, PIN_LED = 37;

// ---------------- machine settings ----------------
const char* NAME[N_CH] = {"Tejpat", "Dalchini", "Long", "Sukumel", "Timur", "Alainchi"};
const uint8_t FULL_PWM[N_CH] = {200, 255, 180, 200, 180, 200};  // fast feed
const uint8_t SLOW_PWM[N_CH] = { 90, 120,  80,  90,  80,  90};  // fine pulses
const float   FINE_ZONE_G    = 2.0;     // switch to pulses when this close
const uint32_t STALL_MS      = 8000;    // no weight gain -> empty / jam
const int   DOOR_CLOSED = 5, DOOR_OPEN = 95;  // servo angles
const bool  SEAL_ENABLED = false;       // true only when impulse sealer wired
const uint32_t SEAL_MS = 1200, COOL_MS = 1500;
const uint8_t RELAY_ON = LOW, RELAY_OFF = HIGH;

// ---------------- saved config ----------------
struct Config {
  uint16_t magic;
  float calib;                 // HX711 scale factor
  float target[N_CH];          // grams per bag
  float tol[N_CH];             // +/- allowed grams
  float inflight[N_CH];        // learned "falling in air" grams
  uint32_t packs;
};
Config cfg;
const uint16_t MAGIC = 0xA55A;

HX711 scale;
Servo door;
LiquidCrystal_I2C lcd(0x27, 16, 2);

enum State { ST_IDLE, ST_RUN, ST_FAULT };
State state = ST_IDLE;
bool startReq = false, abortReq = false;
String lineUsb, lineEsp;
uint32_t lastLcd = 0, lastStatus = 0;

// ---------------- helpers ----------------
void tx(const String &s) { Serial.println(s); Serial2.println(s); }

void beep(int n, int ms = 80) {
  for (int i = 0; i < n; i++) {
    digitalWrite(PIN_BUZZ, HIGH); delay(ms);
    digitalWrite(PIN_BUZZ, LOW);  delay(ms);
  }
}

void lcdShow(const String &a, const String &b = "") {
  lcd.clear(); lcd.setCursor(0, 0); lcd.print(a.substring(0, 16));
  lcd.setCursor(0, 1); lcd.print(b.substring(0, 16));
}

void saveCfg() { EEPROM.put(0, cfg); }

void loadCfg() {
  EEPROM.get(0, cfg);
  if (cfg.magic != MAGIC) {
    cfg.magic = MAGIC;
    cfg.calib = 1.0;  // NOT calibrated yet
    float t[N_CH]  = {1.5, 6.0, 2.0, 2.0, 1.5, 3.0};   // ~16 g pack, edit!
    float tl[N_CH] = {0.6, 1.2, 0.4, 0.6, 0.4, 0.6};
    for (int i = 0; i < N_CH; i++) { cfg.target[i] = t[i]; cfg.tol[i] = tl[i]; cfg.inflight[i] = 0.2; }
    cfg.packs = 0;
    saveCfg();
  }
}

void allMotorsOff() { for (int i = 0; i < N_CH; i++) analogWrite(MOTOR_PIN[i], 0); }
bool bagPresent()   { return digitalRead(PIN_BAG) == LOW; }
bool estopActive()  { return digitalRead(PIN_ESTOP) == HIGH; }
float readW(uint8_t n) { return scale.get_units(n); }

void sendStatus() {
  String s = "{\"ev\":\"status\",\"state\":";
  s += (state == ST_IDLE ? "\"idle\"" : state == ST_RUN ? "\"run\"" : "\"fault\"");
  s += ",\"packs\":" + String(cfg.packs);
  s += ",\"w\":" + String(readW(1), 1);
  s += ",\"bag\":" + String(bagPresent() ? 1 : 0) + "}";
  tx(s);
}

void handleLine(String s) {
  s.trim(); s.toUpperCase();
  if (s == "START") startReq = true;
  else if (s == "STOP") abortReq = true;
  else if (s == "TARE") { scale.tare(10); tx("{\"ev\":\"tare\"}"); }
  else if (s == "STATUS") sendStatus();
  else if (s.startsWith("CAL ")) {
    float known = s.substring(4).toFloat();
    if (known > 0) {
      scale.set_scale();                       // factor = 1
      float f = scale.get_value(15) / known;   // raw units per gram
      scale.set_scale(f); cfg.calib = f; saveCfg();
      tx("{\"ev\":\"cal\",\"factor\":" + String(f, 2) + "}");
    }
  } else if (s.startsWith("SET ")) {
    int ch; float g;
    if (sscanf(s.c_str(), "SET %d %f", &ch, &g) == 2 && ch >= 0 && ch < N_CH && g > 0 && g < 50) {
      cfg.target[ch] = g; saveCfg();
      tx("{\"ev\":\"set\",\"ch\":" + String(ch) + ",\"g\":" + String(g, 1) + "}");
    }
  }
}

void pollComms() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { handleLine(lineUsb); lineUsb = ""; } else if (c != '\r' && lineUsb.length() < 40) lineUsb += c;
  }
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') { handleLine(lineEsp); lineEsp = ""; } else if (c != '\r' && lineEsp.length() < 40) lineEsp += c;
  }
}

bool stopped() { pollComms(); return abortReq || estopActive(); }

bool waitMs(uint32_t ms) {            // delay that can be interrupted
  uint32_t t = millis();
  while (millis() - t < ms) { if (stopped()) return false; delay(5); }
  return true;
}

void fault(const String &msg) {
  allMotorsOff();
  door.write(DOOR_CLOSED);
  digitalWrite(PIN_SEAL, RELAY_OFF);
  state = ST_FAULT;
  lcdShow("FAULT", msg);
  tx("{\"ev\":\"fault\",\"msg\":\"" + msg + "\"}");
  beep(5, 150);
}

// ---------------- dispensing one ingredient ----------------
// Weighs cumulatively: cumT = total grams that should be in hopper after this ingredient.
bool dispense(uint8_t ch, float cumT, float &finalW) {
  uint8_t pin = MOTOR_PIN[ch];
  for (uint8_t attempt = 0; attempt < 4; attempt++) {
    float lead = (attempt == 0) ? cfg.inflight[ch] : 0;   // top-ups use no lead
    float best = readW(1);
    uint32_t lastProg = millis();
    while (true) {
      if (stopped()) { allMotorsOff(); return false; }
      float w = readW(1);
      if (w > best + 0.05) { best = w; lastProg = millis(); }
      if (millis() - lastProg > STALL_MS) { fault(String(NAME[ch]) + " empty/jam"); return false; }
      float remain = cumT - lead - w;
      if (remain <= 0) break;
      if (remain > FINE_ZONE_G) analogWrite(pin, FULL_PWM[ch]);
      else { analogWrite(pin, SLOW_PWM[ch]); delay(120); analogWrite(pin, 0); delay(350); }
    }
    analogWrite(pin, 0);
    if (!waitMs(700)) return false;           // let the material settle
    finalW = readW(6);
    if (attempt == 0) {                        // learn in-air amount
      float err = finalW - cumT;
      cfg.inflight[ch] = constrain(cfg.inflight[ch] + 0.3f * err, 0.0f, 3.0f);
    }
    if (cumT - finalW <= cfg.tol[ch]) return true;   // not under-filled
  }
  return true;
}

// ---------------- full packing cycle ----------------
void runCycle() {
  state = ST_RUN; abortReq = false;
  door.write(DOOR_CLOSED);
  lcdShow("Taring...");
  scale.tare(10);
  float cum = 0, prev = 0, got[N_CH];
  bool ok = true;
  uint32_t t0 = millis();

  for (uint8_t ch = 0; ch < N_CH; ch++) {
    cum += cfg.target[ch];
    lcdShow(String(NAME[ch]), String(cfg.target[ch], 1) + "g target");
    float w = 0;
    if (!dispense(ch, cum, w)) {
      if (state != ST_FAULT) { allMotorsOff(); state = ST_IDLE; lcdShow("Stopped"); tx("{\"ev\":\"stopped\"}"); }
      return;
    }
    got[ch] = w - prev; prev = w;
    if (fabs(got[ch] - cfg.target[ch]) > cfg.tol[ch]) ok = false;
  }

  if (!bagPresent()) { fault("No bag"); return; }
  lcdShow("Filling bag...");
  for (uint8_t k = 0; k < 2; k++) {            // open/close; repeat if residue
    door.write(DOOR_OPEN);
    if (!waitMs(1800)) { fault("Stopped at drop"); return; }
    door.write(DOOR_CLOSED);
    if (!waitMs(600)) { fault("Stopped at drop"); return; }
    if (readW(5) < 0.7) break;
  }

  if (SEAL_ENABLED) {
    lcdShow("Sealing...");
    digitalWrite(PIN_SEAL, RELAY_ON);  delay(SEAL_MS);
    digitalWrite(PIN_SEAL, RELAY_OFF); delay(COOL_MS);
  }

  cfg.packs++; saveCfg();
  String s = "{\"ev\":\"pack\",\"n\":" + String(cfg.packs) + ",\"ok\":" + String(ok ? 1 : 0);
  s += ",\"sec\":" + String((millis() - t0) / 1000) + ",\"g\":[";
  for (uint8_t i = 0; i < N_CH; i++) { s += String(got[i], 1); if (i < N_CH - 1) s += ","; }
  s += "]}";
  tx(s);
  lcdShow(ok ? "Pack OK" : "Pack OUT OF TOL", "Total " + String(cum, 1) + "g");
  beep(ok ? 1 : 3);
  state = ST_IDLE;
  delay(1200);
}

// ---------------- setup / loop ----------------
bool startPressed() {
  if (digitalRead(PIN_START) == LOW) {
    delay(30);
    if (digitalRead(PIN_START) == LOW) { while (digitalRead(PIN_START) == LOW) delay(5); return true; }
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200);
  for (int i = 0; i < N_CH; i++) { pinMode(MOTOR_PIN[i], OUTPUT); analogWrite(MOTOR_PIN[i], 0); }
  pinMode(PIN_START, INPUT_PULLUP); pinMode(PIN_ESTOP, INPUT_PULLUP); pinMode(PIN_BAG, INPUT_PULLUP);
  pinMode(PIN_SEAL, OUTPUT); digitalWrite(PIN_SEAL, RELAY_OFF);
  pinMode(PIN_BUZZ, OUTPUT); pinMode(PIN_LED, OUTPUT);

  lcd.init(); lcd.backlight();
  door.attach(PIN_DOOR); door.write(DOOR_CLOSED);

  loadCfg();
  scale.begin(PIN_HX_DT, PIN_HX_SCK);
  scale.set_scale(cfg.calib);
  scale.tare(10);

  lcdShow("Masala Packer", cfg.calib == 1.0 ? "NOT CALIBRATED" : "Ready");
  tx("{\"ev\":\"boot\",\"calibrated\":" + String(cfg.calib != 1.0 ? 1 : 0) + "}");
  beep(2);
  delay(1000);
}

void loop() {
  pollComms();

  if (state == ST_FAULT) {
    digitalWrite(PIN_LED, (millis() / 300) % 2);
    if ((startPressed() || startReq) && !estopActive()) { startReq = false; abortReq = false; state = ST_IDLE; }
    return;
  }
  digitalWrite(PIN_LED, HIGH);

  if (estopActive()) { lcdShow("E-STOP pressed"); delay(300); return; }

  if (millis() - lastLcd > 500) {
    lastLcd = millis();
    lcd.setCursor(0, 0); lcd.print("Ready Pk#" + String(cfg.packs) + "      ");
    lcd.setCursor(0, 1); lcd.print(String(readW(1), 1) + "g Bag:" + (bagPresent() ? "YES " : "NO  "));
  }
  if (millis() - lastStatus > 10000) { lastStatus = millis(); sendStatus(); }

  if (startPressed() || startReq) {
    startReq = false;
    if (!bagPresent()) { lcdShow("Place bag first"); beep(2); delay(800); }
    else runCycle();
  }
}
