// Standalone power test: sweeps the D2 servo forever, no USB needed.
// Built-in LED: solid for 2 s at boot, then blinks once per sweep.
// If the LED goes solid again mid-sweep, the board restarted (brownout).

const int PIN = D2;
const float LO = 30, HI = 150, SPEED = 60;  // degrees, deg/s

void writeDeg(float deg) {
  uint32_t us = 500 + (uint32_t)(1900 * deg / 180);
  ledcWrite(PIN, us * 16383 / 20000);
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);  // XIAO LED is active low: on
  ledcAttach(PIN, 50, 14);
  writeDeg(90);
  delay(2000);
  digitalWrite(LED_BUILTIN, HIGH);
}

void loop() {
  static float pos = 90;
  static int dir = 1;
  pos += dir * SPEED * 0.02f;
  if (pos >= HI) { pos = HI; dir = -1; }
  if (pos <= LO) {
    pos = LO; dir = 1;
    digitalWrite(LED_BUILTIN, LOW); delay(100); digitalWrite(LED_BUILTIN, HIGH);
  }
  writeDeg(pos);
  delay(20);
}
