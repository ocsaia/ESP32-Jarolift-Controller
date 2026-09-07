# Improvement Plan

Working backlog for this fork. Every item was verified against the code at
`067f0ad` (upstream `dewenni/main`, v1.9.0) — line references point there.

Legend:
- **status** `open` = present in upstream and not fixed anywhere
- **status** `port` = already fixed in `Banabas/ESP32-Jarolift-PositionController`,
  must be **re-implemented by hand** (that fork has no common git history with
  upstream, so `cherry-pick` / `rebase` / `merge` are not possible)
- **status** `done` = landed on this fork, on the branch named below

## Progress

Phase 1a (build hygiene + ported fixes) is complete. Every commit builds clean
for `esp32` with zero warnings from project sources.

| Branch | Items |
|--------|-------|
| `chore/dependency-pins` | E1 |
| `chore/release-artifacts-opt-in` | E3 |
| `fix/rx-at-boot` | C1 |
| `fix/learn-mode-switch` | C2 |
| `fix/format-string-vulnerability` | C3, E2 |
| `fix/timer-minmax-clamp` | C4 |
| `fix/config-load-robustness` | C5, C6, C7 |
| `fix/dusk2dawn-include-case` | C8 |
| `docs/claude-md-and-plan` | E4 |

The two `chore/` branches are the shared base for the rest: nothing builds
without E1, and E3 stops development builds from destroying the committed
release artifacts.

Phase 1b landed: **A1, A2, A3, B1-B5, B7, D1-D7, F3, F5** plus the Minor list, on
these branches - `fix/telnet-command-validation`, `fix/webui-cleanups`,
`fix/minor-correctness`, `fix/radio-hardening`, `fix/network-resilience`,
`feat/small-features`, `fix/ha-discovery`, `fix/cross-task-command-queue`, `fix/mqtt-robustness`,
`fix/ha-retained-discovery`. Every one builds warning-free for
esp32; the 16 MB target builds too.

`fix/cross-task-command-queue` (A2, A3, B4) carries a caveat the others do
not: its three adversarial reviewers were lost to a spend limit and the retries
were cancelled to stay inside it, so it was reviewed by hand instead - one pair
of eyes rather than four. The commit message lists exactly what was and was not
verified. It is the change to re-review first if anything on this branch
misbehaves.

### Still open, and why

| Item | Why it did not land |
|------|---------------------|
| **F4** | Partly done, and the rest is not possible. The abort risk it was blocked on is fixed - the dump is bounded now - but the ring cannot grow: 320 lines overflows the esp32s2's `dram0_0_seg` by 1128 bytes. Raising it would mean dropping that target or making the size target-dependent, and it would spend the S2's entire remaining static headroom on log history. True chunking, which would let the browser show a longer history, still needs an append command in EspWebUI on both the firmware and the client side - the client clears its output on every `add_log` - so it means forking or contributing to that library. |

### New findings from review, not yet fixed

- ~~`AsyncMqttClient::setWill()` stores the bare topic pointer~~ - fixed on
  `fix/mqtt-robustness`.
- `lib_deps` pulls AsyncTCP twice (3.5.0 transitively via ESPAsyncWebServer,
  plus the pinned 3.3.6). **Checked: not a defect.** Only one object is ever
  compiled - `.pio/build/esp32/libc09/AsyncTCP@3.3.6/AsyncTCP.cpp.o` - so the
  pinned version is the one that links and the 3.5.0 copy is an unused
  download.
- `__detachInterrupt()` in the Arduino core has no pin bounds check - already
  guarded on this branch, but worth knowing before any other detach is added.

Phase 2, in progress.

**F1 core landed** on `feat/position-tracking`: the tracker, the immediate stop
path, the notify hooks including physical remotes, the MQTT `set_position`
contract and the Home Assistant position model - which also resolved **B6**.
Breaking: the position convention is inverted to 0 = closed, 100 = open.

**Calibration landed** on `feat/position-calibration`, with MQTT commands so a
channel can be measured without the WebUI:

    <base>/cmd/shutter/<n>/calibrate  <-  down | up | finish | abort

**F1 is complete.** The WebUI landed on `feat/position-webui`: a position slider
per channel on the control page, and a calibration card on the service page that
acts on the shutter already selected there.

Correction to an earlier note in this file: WebUI work does **not** regenerate
`include/gzip_*.h`. EspWebUI's build script writes its output into the library's
own include directory under `.pio/`, which is not tracked. The tracked artefacts
that do change are `web/output/index.html` and `web/output/user.js`.

Those dead `include/gzip_*.h` files have been deleted. Two of them shared a name
with a header EspWebUI generates, so it was worth checking which one the build
actually used: deleting all five leaves the firmware byte-identical after a
clean rebuild, which proves the library's copies were always the ones compiled
and no stale page was ever served.

The tracker and the calibration are covered by 27 native unit tests (see
CLAUDE.md for how to run them). The first run of the tracker suite found two
real defects; the calibration suite passed first time, having been written
against the lessons of that round.

**F2 in progress.**

- Native tests for the existing timer logic landed on `test/timer-logic`, and
  verified three fixes that until then were only compile-checked: the polar
  day/night handling, the min/max clamp and the DST reasoning.
- **Twilight modes** landed on `feat/twilight-modes`: civil, nautical and
  astronomical, plus a custom horizon angle for a view blocked by a hill or a
  building. Config version 3 -> 4; a missing key reads as 0, which is the
  previous behaviour, so no migration step is needed.

- **Timer count 6 -> 24** landed on `feat/more-timers`, with the page and the
  twilight selector. Per-channel scheduling did not need a schema change: every
  timer already carries a 16 bit channel mask, so the capability was there and
  only the capacity was missing. The per-channel/per-group schema the other fork
  uses was considered and rejected - it buys no capability, spends ~2.5 kB of RAM
  on slots that stay empty, and needs a migration.

**F2 is complete.** The weekend override was considered and deliberately skipped:
every timer already has independent per-day flags with checkboxes in the UI, so
a different weekend schedule is two timers - weekdays on one, Saturday and
Sunday on the other - and there are twenty-four slots. Building it in would cost
about 336 bytes of RAM and a second, near-identical set of controls in each of
the twenty-four blocks, for convenience rather than capability. The real
downside of the two-timer approach is that the channel mask and command have to
be set in both places; if that becomes annoying in practice the decision is
worth revisiting.

`scripts/gen_timer_blocks.py` regenerates the timer page from its first block.
It verifies the substitution against every block that already exists before
writing anything, so a token that is still hard-coded aborts the run instead of
producing twenty-four subtly broken copies. Edit block 0, then run it.

---

## A. Crash / memory corruption (P0)

None of these are fixed in the Banabas fork — verified byte-identical there.
These are the likely causes of unexplained reboots.

### A1 — RX ISR overruns the pulse buffer · `open`

`lib/JaroliftController/JaroliftController.cpp:368-402`, buffer declared in
`JaroliftController.h:94-97` (`kPulseBufferSize = 216`).

`pbWrite_` is incremented with no bound check in both branches:

```cpp
lowBuf_[pbWrite_] = lowVal;  pbWrite_++;   // HIGH edge
hiBuf_[pbWrite_]  = highVal;               // LOW edge
```

The only reset is `currentMicros - timeout > 3500`, but every accepted pulse
refreshes `timeout`. Any 433 MHz burst with >216 edges in the 300–1000 µs range
spaced closer than 3.5 ms writes past both buffers into `pbWrite_`, `rxSerial_`,
`rxFunction_`, `initOK_`, `rxDataReady_` and `cc1101_`. A valid Jarolift frame is
~73 pulses, so the headroom is small for a shared ISM band.

Fix: clamp `pbWrite_` (`if (pbWrite_ >= kPulseBufferSize) { pbWrite_ = 0; return; }`)
in both branches, and snapshot the buffers under `portENTER_CRITICAL` before
decoding in `processRxData()`.

Verify: feed a long noise burst (or a second 433 MHz transmitter) and confirm no
reset; assert `pbWrite_ < kPulseBufferSize` in a debug build.

### A2 — `mqttCmdQueue` is used from two tasks without synchronisation · `open`

Producer `addMqttCmd()` `src/mqtt.cpp:34-49` runs in the **AsyncTCP task**
(via `onMqttMessage`); consumer `processMqttMessage()` `src/mqtt.cpp:325,433`
runs in `loop()`. `std::queue`/`std::deque` is not thread-safe — concurrent
push/pop corrupts the heap. Reproducible by a retained-message burst on
HA restart.

Fix: replace with a FreeRTOS queue (preferred), or guard every access with a
mutex. See **B4** — the same primitive should serve the WebUI path.

### A3 — Uninitialised payload buffer · `port`

`src/mqtt.cpp:100-105`:

```cpp
if (payload == NULL)                      { msgCpy.payload[0] = '\0'; }
else if (len > 0 && len < PAYLOAD_LEN)    { memcpy(...); msgCpy.payload[len] = '\0'; }
// no else -> len == 0 or len >= 512 leaves the stack buffer uninitialised
```

`len == 0` is the normal way to clear a retained topic, so this is hit in normal
operation. Add the missing `else`. Fragmented messages (`index`/`total`) are also
unhandled.

---

## B. Functional bugs (P1)

### B1 — WiFi disconnect event is never registered · `open`

`onWiFiStationDisconnected()` exists at `src/basics.cpp:60` but only
`STA_CONNECTED` and `GOT_IP` are bound at `src/basics.cpp:125-126`. So
`wifi.connected` stays `true` forever after the first connect: the UI reports
"connected" with no link, `checkWiFi()` never reconnects, and `mqttCyclic()`
keeps seeing a live link — which drives **B2**.

### B2 — WiFi/MQTT outage reboots the device in a loop · `open`

`src/basics.cpp:99-104` (5 × 30 s) and `src/mqtt.cpp:231-236` (5 × 10 s) call
`ESP.restart()`. `mqtt_retry` only resets on a successful connect, so a broker
outage reboots the controller roughly every 50 s indefinitely, and the shutters
are uncontrollable while it boots. Replace with unbounded retry and exponential
backoff (10 s → 5 min cap); never reboot a working device because a network peer
is down.

### B3 — Telnet passes channel `-1` into array indexing · `open`

`src/telnet.cpp:193-214` and `222-244`: `int channel = -1;` stays `-1` when no
number is given, then `jaroCmd(CMD_UP, channel - 1)` → `uint8_t` 254 →
`discLowArr_[254]` out of bounds; `cmdGroup` does `config.jaro.grp_mask[-1]`.
Reachable by typing `shutter up` / `group up`.

Fix in both places, **and** add the missing bounds check to
`JaroliftController::cmdChannel()` / `cmdGroup()` so the library defends itself.

### B4 — WebUI callback is a single slot shared across tasks · `open`

`src/webUI.cpp:26-28, 86-90, 119-122`. The AsyncTCP task writes
`webCallbackElementID` / `webCallbackValue` / `webCallbackAvailable`; `loop()`
processes **one** event per iteration. A settings form that sends several changed
fields in one burst loses all but the last — worse while `loop()` sits inside a
multi-second radio command. The flags are not `volatile`/atomic either.

Fix: FreeRTOS queue (same primitive as **A2**).

Note: this is the likely mechanism behind the `remote_serial[0]` field that never
stuck on the live device.

### B5 — SHADE position feedback is inverted · `open`

`src/jarolift.cpp:216-243`: `CMD_SET_SHADE` (teach the shade position) publishes
`POS_SHADE`, while `CMD_SHADE` (drive to it) publishes nothing. The group path
(`CMD_GRP_SHADE`) and the remote path (`function == 0x3`) both publish, so single
SHADE is the outlier.

> Superseded if **F1** (position control) is taken — that replaces the fixed
> 0/90/100 scheme entirely. Do not fix twice.

### B6 — HA discovery sets `optimistic: true` while also publishing state · `open`

`src/mqttDiscovery.cpp:129-145`. With `optimistic` on, Home Assistant ignores
`stat_t`, so everything `mqttSendPosition()` publishes is discarded. Also
`POS_SHADE = 90` matches neither `state_open="0"` nor `state_closed="100"`.
Either drop `optimistic` and use the state properly, or stop publishing position.

> Same supersession note as **B5**.

### B7 — Discovery configs are published without `retain` · `open`

`src/mqttDiscovery.cpp:177`. If HA restarts while the ESP is offline, every
entity disappears. The `homeassistant/status` subscription only partly
compensates. HA convention is retained discovery configs.

---

## C. Fixes to port from the Banabas fork

Already written and shipped there; small enough to re-implement by hand.

| # | Item | Upstream location | Fork release | status |
|---|------|-------------------|--------------|--------|
| C1 | `enterRx()` missing at end of `begin()` — receiver stays in IDLE, so remote reception is dead until the first transmission. Identical to upstream **PR #61**. | `JaroliftController.cpp:879` | v1.21.4 | `done` |
| C2 | `learn_mode` forced back to `true` by a copy/paste leftover from the log-level code (`== 0` → `= 4` on a `bool`), **and** `setLegacyLearnMode()` never called, so the WebUI switch reached nothing. | `src/config.cpp:346,638`; `src/jarolift.cpp:193` | v1.21.4 | `done` |
| C3 | Format-string vulnerability: 12 WebUI fields pass user text as the `printf` format (a `%` in a WiFi password corrupts/crashes), plus one in `basics.cpp`. | `src/webUIcallback.cpp:53,56,59,65,68,71,74,82,106,109,112,115`; `src/basics.cpp:276` | v1.21.0 | `done` |
| C4 | Timer min/max clamp: `uint8_t` holding `getHour()`'s `-1` becomes 255 and `>= 0` is always true; the minute clamp is applied separately from the hour, so 05:30 with min 07:15 yields 07:30. | `src/timer.cpp:171-191` | v1.20/1.21 | `done` |
| C5 | MQTT password can stay as raw ciphertext when decryption fails after a partial config read → permanent auth failure → reconnect/reboot loop (feeds **B2**). | `src/config.cpp:512` area | v1.21.0 | `done` |
| C6 | GPIO duplicate-pin check off-by-one: `usedCount < MAX_GPIO - 1` never records the 20th pin. | `src/config.cpp:70` | v1.21.0 | `done` |
| C7 | Duplicate `eth.ipaddress` read in `configLoadFromFile()`. | `src/config.cpp:~500` | v1.20.0 | `done` |
| C8 | `Dusk2Dawn` includes `<Math.h>` instead of `<math.h>` — build fails on case-sensitive filesystems (Linux/CI). Hidden on Windows. | `lib/Dusk2Dawn/Dusk2Dawn.cpp:8`, `.h:11` | v1.21.4 | `done` |

---

## D. Robustness (P2)

| # | Item | Location |
|---|------|----------|
| D1 | `steadyCount_` is `unsigned int` and starts at 0; the first non-STOP frame decrements it to `0xFFFFFFFF`, so remote SHADE detection needs 12 consecutive STOPs to recover. Also never decays and is not per-remote. | `JaroliftController.cpp:831-837` |
| D2 | `remoteCallback` invoked without a null check; `begin()` enables the interrupt before `setRemoteCallback()` runs. | `JaroliftController.cpp:845` |
| D3 | No `detachInterrupt` around TX — the RX ISR jitters the bit-banged `delayMicroseconds` timing. `loop()` re-`attachInterrupt`s repeatedly; `detachInterrupt` appears nowhere in the codebase. | `JaroliftController.cpp:879,900` |
| D4 | NVS opened/closed per command plus a pointless `delay(100)` per write; `cmdUnlearn` spends ~800 ms just waiting. Keep one `nvs_handle` and the counter in RAM. | `JaroliftController.cpp:103-146` |
| D5 | `jaroCmdReInit()` runs a full `jaroliftSetup()` (EEPROM, `cc1101.init()`, another `attachInterrupt`) on every key/serial field change — three fields means three full re-inits. | `src/webUIcallback.cpp:205-218` |
| D6 | Unbounded `sprintf` into 256-byte topic buffers; `jsonString[1024]` can truncate a discovery payload silently so the entity never appears. | `src/mqttDiscovery.cpp:72,78,112,137,177` |
| D7 | `GithubRelease` leaked on every repeated "check version" click when an update is available. | `src/webUIupdates.cpp:303-315` |

### Minor items · re-verified against this branch, not `067f0ad`

| Item | Location | status |
|------|----------|--------|
| Dead `if (position < 0)` on a `uint8_t` parameter | `src/jarolift.cpp:29` | `done` |
| 64-byte topic buffer against a 128-byte configured topic. There are **two**, not one: `mqttSendPosition()` and `mqttSendRemote()`. Both now format `config.mqtt.topic` directly, which also drops these two sites' dependence on `addTopic()`'s cross-task static | `src/jarolift.cpp:29,73` | `done` |
| `getUptime()` wrap arithmetic loses 296 ms per `millis()` wrap | `src/basics.cpp:418` | `done` |
| "Flash-usage percentage is a ratio, not a percentage" | `src/basics.cpp:398` | `rejected` |
| `Dusk2Dawn::_timezone` is `int`, so half-hour zones truncate | `lib/Dusk2Dawn/Dusk2Dawn.h:21` | `done` |
| Dead `updateDeviceCounter(false)` at the end of `cmdUnlearn` | `JaroliftController.cpp:613` | `done` |
| `deviceKeyMSB_/LSB_` should be `uint32_t` | `JaroliftController.h:81` | `done` |

`rejected` — the expression is already
`(float)ESP.getSketchSize() * 100 / ESP.getFreeSketchSpace()`; the `* 100` is
present and byte-identical to `067f0ad`, so the figure is a percentage. The
denominator is the *next* OTA partition rather than the running one, but
`min_spiffs.csv` gives app0 and app1 the same `0x1E0000`, so it is correct on
every target this project builds. It would return 0 (printing `inf %`, no crash)
on a partition table with no second app slot — worth remembering if **F3** lands
with an asymmetric table. The same expression is duplicated at
`src/telnet.cpp:289` and `src/webUIupdates.cpp:132`.

### D8 — astro timers fire at 23:59 during polar day / night · `done`

Not in the original survey; found while fixing the items above.
`Dusk2Dawn::sunriseSet()` reported "the sun does not cross the horizon on this
date" as `-1`. `getSunriseOrSunset()` added the configured offset and normalised
with `(x + 1440) % 1440`, so the sentinel became 23:59: `checkTimerTrigger()`
matched it against the wall clock and sent the group command just before
midnight every day, while the WebUI displayed the same fabricated time.

`-1` is also a *legal* result — `sunriseSet()` does not normalise, and an event
shortly before local midnight is a small negative number (at 70.4N / 31.1E,
sunrise at the polar-day boundary is minute `-2`) — so testing `< 0` or `== -1`
would have discarded real events at exactly the latitudes concerned. The library
now returns `DUSK2DAWN_NO_EVENT` (`-30000`, outside the legal range of roughly
`[-1456, 3076]`) and `getSunriseOrSunset()` returns `bool`.

Follow-up not taken: an astro timer with `use_max_time` set used to fire at its
configured limit during the polar season, because 1439 was clamped down to the
maximum, and now does not fire at all. Choosing the right bound needs polar day
and polar night told apart, which means a second sentinel out of
`hourAngleSunrise()` and an explicit UI decision about what "no sunrise" should
do.

---

## E. Build & infrastructure

| # | Item |
|---|------|
| E1 | **Git dependency pins are broken.** `EspWebUI @ 0.0.4` does not resolve — a plain git URL cannot be version-pinned this way, and upstream moved to 0.0.7, so a clean checkout fails to build. Fixed locally to `#v0.0.4`. `EspStrUtil @ 1.1.0` and `EspSysUtil @ 1.1.0` work only by accident (EspSysUtil's newest *tag* is v1.0.1; 1.1.0 is untagged HEAD), and `ESP_Git_OTA` has no constraint at all. Pin all four by tag or commit SHA. |
| E2 | Add `-Wformat=2 -Wno-format-nonliteral` to `build_flags`. `-Wall` alone misses **C3** entirely; with the flag the compiler flags all 13 sites and nothing else. |
| E3 | `release/*.bin` are tracked **and** rewritten by `scripts/build_release.py` on every build — building a single target deletes the other targets' binaries. Either gitignore them on this fork or `git checkout -- release/` after each build. |
| E4 | No `CLAUDE.md`. Add one covering: build commands, the **E3** trap, the **E1** pin syntax, `.clang-format`, and the task-context rule (AsyncTCP vs `loop()`) that **A2**/**B4** exist because of. |

Baseline measurements (`pio run -e esp32`, after the E1 fix):

| Build | Flash | Static RAM |
|-------|-------|------------|
| upstream v1.9.0 | 77.0 % (1 514 752 B) | 27.5 % |
| Banabas v1.21.4 | 78.1 % (1 534 912 B) | 32.9 % |

---

## F. Features available in the Banabas fork

Scope decision pending — see the note at the top of **B5**/**B6**.

| # | Feature | Fork release | Notes |
|---|---------|--------------|-------|
| F1 | Time-based position control (0–100 %) with two-phase calibration and independent up/down travel times per channel | v1.20.0 | Large. **Breaking:** v1.21.0 inverted the convention to 0 % = closed / 100 % = open to match HA. Supersedes **B5**, **B6**. |
| F2 | Timer rework: per-channel and per-group schedules, weekend override, astro modes (real/civil/nautical/astronomical/custom horizon) | v1.20.0 | Large. Supersedes **C4**. |
| F3 | `esp32s3_16mb` build target with matching partition table | v1.20.0 | Small. |
| F4 | Web log buffer 200 → 320 entries | v1.21.2 | Trivial. |
| F5 | Remote-signal log line written even when MQTT is disconnected (`mqttSendRemote()` returned early and skipped the log) | v1.21.1 | Small, useful independently of F1. |
| F6 | Remote-triggered UP/DOWN/STOP feed the position tracker | v1.21.3 | Depends on **F1**. |

---

## Suggested order

1. **E1, E2, E3, E4** — make the build reproducible and the traps documented first.
2. **C1, C2, C3, C4, C5, C6, C7, C8** — known-good fixes, each a small independent commit.
3. **A1, A3** — self-contained P0s, no design decision needed.
4. **A2 + B4 together** — one shared thread-safe command queue; do not solve twice.
5. **B1 + B2 + C5 together** — network resilience is one coherent change.
6. **B3, B7, D1–D7** — independent, parallelisable.
7. **F-items** — only after the scope decision; **F1/F2 must land after A2/B4**,
   because position control adds more cross-task producers to the command queue.

## Branch strategy

Fork from `dewenni/ESP32-Jarolift-Controller` via the GitHub UI so the common
ancestor is preserved (the Banabas fork lost this, which is why nothing can be
cherry-picked from it).

```bash
git remote rename origin upstream
git remote add origin https://github.com/<user>/ESP32-Jarolift-Controller.git
git fetch upstream && git push -u origin main
```

One branch per item off `upstream/main` so each stays independently PR-able:

```bash
git switch -c fix/rx-at-boot upstream/main
```

Keep a separate integration branch that merges them all for the firmware that
actually runs on the device.
