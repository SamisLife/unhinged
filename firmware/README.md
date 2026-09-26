# Phase 0: servo power bring-up

Goal: find out if the SG90s can run from the XIAO's 5V pin (laptop USB power).

## Wire (one servo)

| SG90 wire | XIAO pin |
|-----------|----------|
| brown     | GND      |
| red       | 5V (never 3V3) |
| orange    | D0       |

## Flash

Arduino IDE: Board **XIAO_ESP32S3**, **Tools > USB CDC On Boot: Enabled**, library **ESP32Servo**. Open `servo_bringup/servo_bringup.ino`, upload.

Or from the terminal (ESP32 core and ESP32Servo are already installed for arduino-cli):

```sh
firmware/flash.sh
```

If the IDE fails with `ctags: bad CPU type`, this Mac has no Rosetta. Use `flash.sh`, or install Rosetta once with `softwareupdate --install-rosetta --agree-to-license`.

If upload hangs on "Connecting...": hold **BOOT**, tap **RESET**, release BOOT, upload again, then tap RESET after.

## Watch

Close the Arduino serial monitor (only one program can hold the port), then:

```sh
.venv/bin/python tools/monitor.py
```

It timestamps every line, reconnects by itself if USB drops, shouts on `BROWNOUT` or a reboot, and saves a log to `tools/logs/`. Type commands and press Enter.

A healthy heartbeat, once a second:

```
hb up=42.0s boot=1 reset=POWERON sel=all s0=90(idle)
```

- `reset=POWERON`, `SW`, `EXT` or `OTHER` right after flashing: normal.
- `reset=BROWNOUT`: power sag. The 5V pin is not enough.
- `boot=` goes above 1 without you unplugging: the board rebooted.
- `!!! serial dropped`: USB fell off the bus, usually a power sag too.

## Commands

| Command | Does |
|---------|------|
| `a 90` | ramp to 90 degrees |
| `slow` | slow sweep 20..160 |
| `fast` | sweep as fast as an SG90 can |
| `shake` | hard jumps 60 / 120 every 150 ms (worst case) |
| `stop` | hold where it is |
| `off` | go limp (no current) |
| `sel 0` / `sel all` | pick which servos the commands drive |
| `status` | print a heartbeat now |

## Checklist (per step, about 30 s each)

1. `slow`: smooth, no buzz, no reset.
2. `fast`: no reset, no serial drop.
3. `shake`: no reset, no serial drop.
4. `slow`, then lightly pinch the horn with two fingers so it has to push. Don't stall it hard for more than a second. Then `shake` while pinching.
5. `stop`. Heartbeat should still show `boot=1`.

Write down for each: any reset, any serial drop, any jitter or twitch when it should be still.

## Adding a servo

In `servo_bringup.ino`, uncomment the next line in `AXES` (`s1` on D1, `s2` on D2), reflash. With `sel all` (the default) every command moves all servos at the same instant, which is the worst case for current. Rerun the checklist.

## Decision

- **Stable with two servos moving together, including when pinched:** stay on the 5V pin. Add a few hundred µF capacitor across 5V and GND near the servos (stripe side to GND).
- **Any brownout, serial drop, or reboot:** move servo power to a separate 5V supply: sacrificed USB cable from a 2A+ wall brick into the breadboard rails. Check polarity with a multimeter first. Connect the brick's GND to the XIAO's GND. Never connect the brick's 5V to the XIAO's 5V pin while the XIAO is on laptop USB.
