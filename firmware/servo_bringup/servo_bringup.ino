// Unhinged, Phase 0: servo power bring-up on a XIAO ESP32-S3.
//
// Question this answers: can the SG90s run from the XIAO's 5V pin (laptop USB)?
// Watch for: reset=BROWNOUT, boot count climbing, the serial port dropping, jitter.
//
// Arduino IDE: Tools > Board: XIAO_ESP32S3, Tools > USB CDC On Boot: Enabled.
// Library: ESP32Servo.
//
// Commands (newline terminated, 115200 baud):
//   a <deg>        ramp selected servos to an angle
//   slow           slow sweep (30 deg/s)
//   fast           fast sweep (as fast as an SG90 goes)
//   shake          rapid back-and-forth, hard jumps (worst case)
//   stop           hold where they are
//   off            detach (servos go limp, draw ~nothing)
//   sel <i|all>    choose which servos the commands drive (default: all)
//   status         print one heartbeat now
//   help

#include <ESP32Servo.h>
#include <esp_system.h>

// ---- Servo list. Adding a servo = adding a line here. ----
struct AxisConfig {
  const char* name;
  int pin;
  float minDeg;  // hard limits, never exceeded
  float maxDeg;
};

const AxisConfig AXES[] = {
  {"s0", D0, 5, 175},
  {"s1", D1, 5, 175},
  {"s2", D2, 5, 175},
};
const int NUM_AXES = sizeof(AXES) / sizeof(AXES[0]);

// SG90 pulse range. 500..2400 us covers ~180 degrees on most units.
const int PULSE_MIN_US = 500;
const int PULSE_MAX_US = 2400;

const float SLOW_DEG_S = 30;
const float FAST_DEG_S = 600;   // ~SG90 no-load max (0.1 s / 60 deg)
const float RAMP_DEG_S = 120;   // speed for "a <deg>"
const float SWEEP_LO = 20, SWEEP_HI = 160;
const float SHAKE_LO = 60, SHAKE_HI = 120;
const uint32_t SHAKE_HALF_PERIOD_MS = 150;

const uint32_t TICK_MS = 20;        // 50 Hz, matches the servo frame rate
const uint32_t HEARTBEAT_MS = 1000;
const uint32_t ATTACH_STAGGER_MS = 300;  // don't let every servo jump at once on boot

// ---- State ----
enum Mode { IDLE, SWEEP, SHAKE, OFF };
const char* modeName(Mode m) {
  switch (m) {
    case IDLE:  return "idle";
    case SWEEP: return "sweep";
    case SHAKE: return "shake";
    case OFF:   return "off";
  }
  return "?";
}

struct Axis {
  Servo servo;
  bool attached = false;
  Mode mode = IDLE;
  float pos = 90;     // last commanded angle (no feedback on an SG90)
  float target = 90;
  float speed = RAMP_DEG_S;
  int dir = 1;
};
Axis axes[NUM_AXES];
bool selected[NUM_AXES];

// Survives brownout and software resets, cleared on real power-on or a fresh flash.
RTC_NOINIT_ATTR uint32_t bootMagic;
RTC_NOINIT_ATTR uint32_t bootCount;
const uint32_t BOOT_MAGIC = 0x0DD0F01D;
esp_reset_reason_t resetReason;

const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "OTHER";
  }
}

// ---- Motion ----
float clampAxis(int i, float deg) {
  return constrain(deg, AXES[i].minDeg, AXES[i].maxDeg);
}

void writeAxis(int i, float deg) {
  deg = clampAxis(i, deg);
  axes[i].pos = deg;
  int us = PULSE_MIN_US + (int)((PULSE_MAX_US - PULSE_MIN_US) * deg / 180.0f + 0.5f);
  axes[i].servo.writeMicroseconds(us);
}

void ensureAttached(int i) {
  if (axes[i].attached) return;
  axes[i].servo.setPeriodHertz(50);
  axes[i].servo.attach(AXES[i].pin, PULSE_MIN_US, PULSE_MAX_US);
  axes[i].attached = true;
  writeAxis(i, axes[i].pos);
}

void updateAxis(int i, float dt) {
  Axis& a = axes[i];
  switch (a.mode) {
    case OFF:
      return;
    case IDLE: {
      float step = a.speed * dt;
      float d = a.target - a.pos;
      writeAxis(i, fabsf(d) <= step ? a.target : a.pos + (d > 0 ? step : -step));
      break;
    }
    case SWEEP: {
      float lo = clampAxis(i, SWEEP_LO), hi = clampAxis(i, SWEEP_HI);
      float next = a.pos + a.dir * a.speed * dt;
      if (next >= hi) { next = hi; a.dir = -1; }
      if (next <= lo) { next = lo; a.dir = 1; }
      writeAxis(i, next);
      break;
    }
    case SHAKE: {
      // Shared clock so every selected servo jumps on the same frame (worst-case current).
      bool high = (millis() / SHAKE_HALF_PERIOD_MS) % 2;
      writeAxis(i, high ? SHAKE_HI : SHAKE_LO);
      break;
    }
  }
}

// ---- Output ----
void printHeartbeat(const char* tag) {
  Serial.printf("%s up=%.1fs boot=%lu reset=%s sel=", tag, millis() / 1000.0f,
                (unsigned long)bootCount, resetName(resetReason));
  bool all = true;
  for (int i = 0; i < NUM_AXES; i++) all &= selected[i];
  if (all) {
    Serial.print("all");
  } else {
    bool first = true;
    for (int i = 0; i < NUM_AXES; i++) {
      if (!selected[i]) continue;
      Serial.printf("%s%s", first ? "" : ",", AXES[i].name);
      first = false;
    }
  }
  for (int i = 0; i < NUM_AXES; i++) {
    Serial.printf(" %s=%.0f(%s)", AXES[i].name, axes[i].pos, modeName(axes[i].mode));
  }
  Serial.println();
}

void printHelp() {
  Serial.println("cmds: a <deg> | slow | fast | shake | stop | off | sel <i|all> | status | help");
}

// ---- Commands ----
void forSelected(void (*fn)(int)) {
  for (int i = 0; i < NUM_AXES; i++) if (selected[i]) fn(i);
}

float argDeg;

void handleCommand(char* line) {
  char* cmd = strtok(line, " \t");
  if (!cmd) return;
  char* arg = strtok(nullptr, " \t");

  if (!strcmp(cmd, "a")) {
    if (!arg) { Serial.println("err: a <deg>"); return; }
    argDeg = atof(arg);
    forSelected([](int i) {
      ensureAttached(i);
      axes[i].mode = IDLE;
      axes[i].speed = RAMP_DEG_S;
      axes[i].target = clampAxis(i, argDeg);
    });
  } else if (!strcmp(cmd, "slow") || !strcmp(cmd, "fast")) {
    argDeg = !strcmp(cmd, "slow") ? SLOW_DEG_S : FAST_DEG_S;
    forSelected([](int i) {
      ensureAttached(i);
      axes[i].mode = SWEEP;
      axes[i].speed = argDeg;
      axes[i].dir = 1;
    });
  } else if (!strcmp(cmd, "shake")) {
    forSelected([](int i) { ensureAttached(i); axes[i].mode = SHAKE; });
  } else if (!strcmp(cmd, "stop")) {
    forSelected([](int i) {
      if (axes[i].mode == OFF) return;
      axes[i].mode = IDLE;
      axes[i].target = axes[i].pos;
    });
  } else if (!strcmp(cmd, "off")) {
    forSelected([](int i) {
      if (axes[i].attached) axes[i].servo.detach();
      axes[i].attached = false;
      axes[i].mode = OFF;
    });
  } else if (!strcmp(cmd, "sel")) {
    if (!arg) { Serial.println("err: sel <i|all>"); return; }
    if (!strcmp(arg, "all")) {
      for (int i = 0; i < NUM_AXES; i++) selected[i] = true;
    } else {
      int n = atoi(arg);
      if (n < 0 || n >= NUM_AXES || (n == 0 && arg[0] != '0')) {
        Serial.printf("err: servo %s not in list (0..%d)\n", arg, NUM_AXES - 1);
        return;
      }
      for (int i = 0; i < NUM_AXES; i++) selected[i] = (i == n);
    }
  } else if (!strcmp(cmd, "status")) {
    printHeartbeat("hb");
    return;
  } else if (!strcmp(cmd, "help")) {
    printHelp();
    return;
  } else {
    Serial.printf("err: unknown '%s'\n", cmd);
    printHelp();
    return;
  }
  Serial.printf("ok %s%s%s\n", cmd, arg ? " " : "", arg ? arg : "");
}

void pollSerial() {
  static char buf[64];
  static size_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[len] = 0;
      handleCommand(buf);
      len = 0;
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

// ---- Main ----
void setup() {
  resetReason = esp_reset_reason();
  if (resetReason == ESP_RST_POWERON || bootMagic != BOOT_MAGIC) {
    bootMagic = BOOT_MAGIC;
    bootCount = 0;
  }
  bootCount++;

  Serial.begin(115200);
#if ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(0);  // never stall motion if nobody is reading the port
#endif
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);

  Serial.printf("\nboot reset=%s boot=%lu servos=%d\n", resetName(resetReason),
                (unsigned long)bootCount, NUM_AXES);
  if (resetReason == ESP_RST_BROWNOUT) {
    Serial.println("!!! BROWNOUT: servo power is not enough on the 5V pin");
  }
  printHelp();

  for (int i = 0; i < NUM_AXES; i++) {
    selected[i] = true;
    ensureAttached(i);
    delay(ATTACH_STAGGER_MS);
  }
}

void loop() {
  static uint32_t lastTick = millis(), lastBeat = 0;
  pollSerial();

  uint32_t now = millis();
  if (now - lastTick >= TICK_MS) {
    float dt = (now - lastTick) / 1000.0f;
    lastTick = now;
    for (int i = 0; i < NUM_AXES; i++) updateAxis(i, dt);
  }
  if (now - lastBeat >= HEARTBEAT_MS) {
    lastBeat = now;
    printHeartbeat("hb");
  }
}
