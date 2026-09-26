// Unhinged rig firmware: live tilt/pan control over USB serial.
//
// Protocol v1 (one ASCII line each way, newline terminated, 115200 baud):
//   in:  hello | move <tilt> <pan> | tilt <deg> | pan <deg> | center | stop | status
//   out: hello fw=unhinged proto=1 tilt=<min>..<max> pan=<min>..<max>
//        pos tilt=<deg> pan=<deg> moving=<0|1>   (10 Hz while moving, once at rest, on status)
//        hb up=<s> boot=<n> reset=<reason>        (1 Hz)
//        err <reason>
// Calibration, typed by hand (the app ignores these lines):
//   trim                 print each servo's trim
//   trim <servo> <deg>   nudge one servo so linked servos agree (index into SERVOS)
//   trim save            keep the trims across reboots
// Angles are degrees from center. Targets are latest-wins; motion is speed and
// acceleration limited so the rig never jumps.
//
// Boards (no libraries needed on either):
//   Raspberry Pi Pico / Pico 2: arduino-pico core (rp2040:rp2040:rpipico or rpipico2).
//     Servo power from VBUS (pin 40), never 3V3.
//   XIAO ESP32-S3: USB CDC On Boot: Enabled. Servo power from 5V, never 3V3.
// Servos are driven with hardware PWM directly: ESP32Servo 3.2.1 on arduino-esp32
// 3.3 only outputs on one pin when several servos are attached.

#if defined(ARDUINO_ARCH_RP2040)
#include <EEPROM.h>
#include <hardware/clocks.h>
#include <hardware/pwm.h>
#include <hardware/watchdog.h>
#if PICO_RP2350
#include <hardware/structs/powman.h>
#else
#include <hardware/structs/vreg_and_chip_reset.h>
#endif
#elif defined(ARDUINO_ARCH_ESP32)
#include <Preferences.h>
#include <esp_system.h>
#else
#error "Unsupported board: use a Raspberry Pi Pico or an ESP32"
#endif

// ---- Axes ----
enum AxisId { TILT, PAN, NUM_AXES };
const char* const AXIS_NAMES[NUM_AXES] = {"tilt", "pan"};
const float AXIS_MIN[NUM_AXES] = {-85, -85};
const float AXIS_MAX[NUM_AXES] = {85, 85};

// ---- Servos. Several servos can drive one axis. ----
struct ServoConfig {
  int pin;
  AxisId axis;
  bool invert;   // mirrored mounting
};

#if defined(ARDUINO_ARCH_RP2040)
const ServoConfig SERVOS[] = {
  {3, TILT, false},  // GP3, physical pin 5
  {4, TILT, true},   // GP4, physical pin 6
  {2, PAN, false},   // GP2, physical pin 4
};
#else
const ServoConfig SERVOS[] = {
  {D0, TILT, false},
  {D1, TILT, true},
  {D2, PAN, false},
};
#endif
const int NUM_SERVOS = sizeof(SERVOS) / sizeof(SERVOS[0]);

// SG90: 500..2400 us covers ~180 degrees.
const int PULSE_MIN_US = 500;
const int PULSE_MAX_US = 2400;
const float SERVO_MIN_DEG = 2, SERVO_MAX_DEG = 178;  // never drive into the end stops
const uint32_t PWM_HZ = 50, PWM_PERIOD_US = 1000000 / PWM_HZ;

const float MAX_SPEED = 180;  // deg/s
const float MAX_ACCEL = 720;  // deg/s^2

const uint32_t TICK_MS = 20;  // 50 Hz, the servo frame rate
const uint32_t POS_MS = 100;  // 10 Hz position reports while moving
const uint32_t HEARTBEAT_MS = 1000;
const uint32_t ATTACH_STAGGER_MS = 300;  // between axes; servos on one axis start together
const float MAX_TRIM = 20;

// ---- State ----
struct AxisState {
  float pos = 0, vel = 0, target = 0;
};
AxisState axes[NUM_AXES];
float trims[NUM_SERVOS];  // degrees added per servo, loaded from flash
uint32_t bootCount;
const char* resetReason;
const uint32_t BOOT_MAGIC = 0x0DD0F01D;

// ---- Platform ----
// Everything board-specific: PWM, reset reason, boot counter, trim storage.
#if defined(ARDUINO_ARCH_RP2040)

// Hardware PWM set up directly: analogWriteFreq() won't go below 100 Hz, which
// halves every servo pulse. One count = 1 us, wrap at 20000 = 50 Hz.
void pwmBegin() {}
void pwmAttach(int pin) {
  gpio_set_function(pin, GPIO_FUNC_PWM);
  uint slice = pwm_gpio_to_slice_num(pin);
  pwm_set_clkdiv(slice, clock_get_hz(clk_sys) / 1000000.0f);
  pwm_set_wrap(slice, PWM_PERIOD_US - 1);
  pwm_set_enabled(slice, true);
}
void pwmWriteUs(int pin, int us) { pwm_set_gpio_level(pin, us); }

// Watchdog scratch registers survive every reset except power-on and RUN.
// The RP2350 flags brownouts; on the RP2040 a brownout shows as POWERON.
void readResetInfo() {
#if PICO_RP2350
  uint32_t cr = powman_hw->chip_reset;
  bool cold = cr & (POWMAN_CHIP_RESET_HAD_POR_BITS | POWMAN_CHIP_RESET_HAD_BOR_BITS |
                    POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS);
  if (cr & POWMAN_CHIP_RESET_HAD_BOR_BITS) resetReason = "BROWNOUT";
  else if (cr & POWMAN_CHIP_RESET_HAD_POR_BITS) resetReason = "POWERON";
  else if (cr & POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS) resetReason = "EXT";
#else
  uint32_t cr = vreg_and_chip_reset_hw->chip_reset;
  bool cold = cr & (VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS | VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS);
  if (cr & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS) resetReason = "POWERON";
  else if (cr & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS) resetReason = "EXT";
#endif
  else if (watchdog_caused_reboot()) resetReason = "SW";
  else resetReason = "OTHER";
  if (cold || watchdog_hw->scratch[0] != BOOT_MAGIC) {
    watchdog_hw->scratch[0] = BOOT_MAGIC;
    watchdog_hw->scratch[1] = 0;
  }
  bootCount = ++watchdog_hw->scratch[1];
}

struct TrimStore { uint32_t magic; float trims[NUM_SERVOS]; };
void loadTrims() {
  TrimStore t;
  EEPROM.begin(256);
  EEPROM.get(0, t);
  for (int s = 0; s < NUM_SERVOS; s++) {
    trims[s] = (t.magic == BOOT_MAGIC && isfinite(t.trims[s])) ? t.trims[s] : 0;
  }
}
void saveTrims() {
  TrimStore t = {BOOT_MAGIC, {}};
  for (int s = 0; s < NUM_SERVOS; s++) t.trims[s] = trims[s];
  EEPROM.put(0, t);
  EEPROM.commit();
}

#else  // ESP32

const uint32_t PWM_BITS = 14;
void pwmBegin() {}
void pwmAttach(int pin) { ledcAttach(pin, PWM_HZ, PWM_BITS); }
void pwmWriteUs(int pin, int us) {
  ledcWrite(pin, (uint32_t)us * ((1 << PWM_BITS) - 1) / PWM_PERIOD_US);
}

RTC_NOINIT_ATTR uint32_t bootMagic;
RTC_NOINIT_ATTR uint32_t rtcBootCount;
void readResetInfo() {
  esp_reset_reason_t r = esp_reset_reason();
  switch (r) {
    case ESP_RST_POWERON:   resetReason = "POWERON"; break;
    case ESP_RST_EXT:       resetReason = "EXT"; break;
    case ESP_RST_SW:        resetReason = "SW"; break;
    case ESP_RST_PANIC:     resetReason = "PANIC"; break;
    case ESP_RST_INT_WDT:   resetReason = "INT_WDT"; break;
    case ESP_RST_TASK_WDT:  resetReason = "TASK_WDT"; break;
    case ESP_RST_WDT:       resetReason = "WDT"; break;
    case ESP_RST_DEEPSLEEP: resetReason = "DEEPSLEEP"; break;
    case ESP_RST_BROWNOUT:  resetReason = "BROWNOUT"; break;
    case ESP_RST_SDIO:      resetReason = "SDIO"; break;
    case ESP_RST_USB:       resetReason = "USB"; break;
    case ESP_RST_JTAG:      resetReason = "JTAG"; break;
    default:                resetReason = "OTHER"; break;
  }
  if (r == ESP_RST_POWERON || bootMagic != BOOT_MAGIC) {
    bootMagic = BOOT_MAGIC;
    rtcBootCount = 0;
  }
  bootCount = ++rtcBootCount;
}

Preferences prefs;
void loadTrims() {
  prefs.begin("unhinged", true);
  for (int s = 0; s < NUM_SERVOS; s++) trims[s] = prefs.getFloat(("trim" + String(s)).c_str(), 0);
  prefs.end();
}
void saveTrims() {
  prefs.begin("unhinged", false);
  for (int s = 0; s < NUM_SERVOS; s++) prefs.putFloat(("trim" + String(s)).c_str(), trims[s]);
  prefs.end();
}

#endif

// ---- Output ----
void sendHello() {
  Serial.printf("hello fw=unhinged proto=1 tilt=%g..%g pan=%g..%g\n",
                AXIS_MIN[TILT], AXIS_MAX[TILT], AXIS_MIN[PAN], AXIS_MAX[PAN]);
}

bool isMoving() {
  for (int a = 0; a < NUM_AXES; a++) {
    if (axes[a].vel != 0 || axes[a].pos != axes[a].target) return true;
  }
  return false;
}

void sendPos() {
  Serial.printf("pos tilt=%.1f pan=%.1f moving=%d\n", axes[TILT].pos, axes[PAN].pos,
                isMoving() ? 1 : 0);
}

void sendHeartbeat() {
  Serial.printf("hb up=%.1f boot=%lu reset=%s\n", millis() / 1000.0f,
                (unsigned long)bootCount, resetReason);
}

// ---- Motion ----
void writeServo(int s) {
  const ServoConfig& c = SERVOS[s];
  float v = axes[c.axis].pos;
  float deg = constrain(90 + (c.invert ? -v : v) + trims[s], SERVO_MIN_DEG, SERVO_MAX_DEG);
  int us = PULSE_MIN_US + (int)((PULSE_MAX_US - PULSE_MIN_US) * deg / 180.0f + 0.5f);
  pwmWriteUs(c.pin, us);
}

// Accelerate toward the fastest speed that can still stop exactly at the target.
void stepAxis(AxisState& a, float dt) {
  float d = a.target - a.pos;
  if (fabsf(d) < 0.05f && fabsf(a.vel) < MAX_ACCEL * dt) {
    a.pos = a.target;
    a.vel = 0;
    return;
  }
  float want = copysignf(fminf(MAX_SPEED, sqrtf(2 * MAX_ACCEL * fabsf(d))), d);
  float dv = constrain(want - a.vel, -MAX_ACCEL * dt, MAX_ACCEL * dt);
  a.vel += dv;
  float step = a.vel * dt;
  if (fabsf(step) >= fabsf(d) && (step > 0) == (d > 0)) {
    a.pos = a.target;
    a.vel = 0;
  } else {
    a.pos += step;
  }
}

void setTarget(AxisId axis, float deg) {
  axes[axis].target = constrain(deg, AXIS_MIN[axis], AXIS_MAX[axis]);
}

// ---- Trim ----
void sendTrims() {
  Serial.print("trim");
  for (int s = 0; s < NUM_SERVOS; s++) Serial.printf(" s%d=%.1f", s, trims[s]);
  Serial.println();
}

// ---- Commands ----
// Strict parse: the whole token must be a number, so "12,5" is rejected.
bool parseDeg(const char* tok, float& out) {
  if (!tok) return false;
  char* end;
  out = strtof(tok, &end);
  return end != tok && *end == 0 && isfinite(out);
}

void handleCommand(char* line) {
  char* cmd = strtok(line, " \t");
  if (!cmd) return;
  char* a1 = strtok(nullptr, " \t");
  char* a2 = strtok(nullptr, " \t");
  float t, p;

  if (!strcmp(cmd, "move")) {
    if (!parseDeg(a1, t) || !parseDeg(a2, p)) { Serial.println("err usage: move <tilt> <pan>"); return; }
    setTarget(TILT, t);
    setTarget(PAN, p);
  } else if (!strcmp(cmd, "tilt") || !strcmp(cmd, "pan")) {
    if (!parseDeg(a1, t)) { Serial.printf("err usage: %s <deg>\n", cmd); return; }
    setTarget(cmd[0] == 't' ? TILT : PAN, t);
  } else if (!strcmp(cmd, "center")) {
    setTarget(TILT, 0);
    setTarget(PAN, 0);
  } else if (!strcmp(cmd, "stop")) {
    for (int a = 0; a < NUM_AXES; a++) {
      axes[a].target = axes[a].pos;
      axes[a].vel = 0;
    }
    sendPos();
  } else if (!strcmp(cmd, "status")) {
    sendPos();
  } else if (!strcmp(cmd, "trim")) {
    if (a1 && !strcmp(a1, "save")) {
      saveTrims();
      Serial.print("saved ");
    } else if (a1) {
      char* end;
      long s = strtol(a1, &end, 10);
      if (*end || s < 0 || s >= NUM_SERVOS || !parseDeg(a2, t)) {
        Serial.printf("err usage: trim <0..%d> <deg> | trim save\n", NUM_SERVOS - 1);
        return;
      }
      trims[s] = constrain(t, -MAX_TRIM, MAX_TRIM);
    }
    sendTrims();
  } else if (!strcmp(cmd, "hello")) {
    sendHello();
  } else {
    Serial.printf("err unknown command '%s'\n", cmd);
  }
}

void pollSerial() {
  static char buf[64];
  static size_t len = 0;
  static bool overflow = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (overflow) {
        Serial.println("err line too long");
      } else {
        buf[len] = 0;
        handleCommand(buf);
      }
      len = 0;
      overflow = false;
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    } else {
      overflow = true;
    }
  }
}

// ---- Main ----
void setup() {
  readResetInfo();
  Serial.begin(115200);
#if ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(0);  // never stall motion if nobody is reading the port
#endif

  loadTrims();
  pwmBegin();
  // One axis at a time to spread the startup current, but every servo on an
  // axis starts in the same instant so linked servos never fight.
  for (int a = 0; a < NUM_AXES; a++) {
    for (int s = 0; s < NUM_SERVOS; s++) {
      if (SERVOS[s].axis != a) continue;
      pwmAttach(SERVOS[s].pin);
      writeServo(s);
    }
    delay(ATTACH_STAGGER_MS);
  }
  sendHello();
}

void loop() {
  static uint32_t lastTick = millis(), lastPos = 0, lastBeat = 0;
  static bool wasMoving = false;
  pollSerial();

  uint32_t now = millis();
  if (now - lastTick >= TICK_MS) {
    float dt = (now - lastTick) / 1000.0f;
    lastTick = now;
    for (int a = 0; a < NUM_AXES; a++) stepAxis(axes[a], dt);
    for (int s = 0; s < NUM_SERVOS; s++) writeServo(s);

    bool moving = isMoving();
    if (moving && now - lastPos >= POS_MS) {
      lastPos = now;
      sendPos();
    } else if (!moving && wasMoving) {
      sendPos();  // final resting position
    }
    wasMoving = moving;
  }
  if (now - lastBeat >= HEARTBEAT_MS) {
    lastBeat = now;
    sendHeartbeat();
  }
}
