# CLAUDE.md

ESP32 + CC1101 controller for Jarolift TDEF roller shutters (433 MHz KeeLoq),
with a WebUI, MQTT/Home Assistant, telnet and timers. Arduino framework on
PlatformIO.

This is a fork of `dewenni/ESP32-Jarolift-Controller`. `upstream` points at the
original, `origin` at this fork. Work items are tracked in
[IMPROVEMENT-PLAN.md](IMPROVEMENT-PLAN.md) — read it before starting a change,
and keep it in sync when an item lands.

## Build

```bash
pio run -e esp32          # also: esp32s2, esp32s3, esp32c3
pio run                   # all four targets
pio pkg install -e esp32  # resolve dependencies only, no compile
```

There are no unit tests and no emulator — **compiling is the verification step**.
Always build after a change and check that project sources stay warning-free:

```bash
pio run -e esp32 2>&1 | grep -E '^(src|lib)/.*warning:'
```

Flashing and runtime behaviour can only be checked on real hardware, which the
agent does not have. Do not claim runtime behaviour was verified.

## Things that will bite you

**Never write `release/*.bin` from a normal build.** Those binaries are tracked
in git and the release workflow uploads `./release/*` as-is without building.
`scripts/build_release.py` also deletes the whole folder when the `esp32`
environment builds, so a single-target build used to wipe the other targets'
artifacts. Writing there is now opt-in via `JAROLIFT_RELEASE=1`; when producing
a real release, set it and build all targets in one run.

**Git dependencies must be pinned by tag (`#vX.Y.Z`) or commit SHA.** The
`@ X.Y.Z` form is not a version constraint for a plain git URL — PlatformIO only
compares it against whatever `library.json` declares at HEAD, so the build breaks
the moment upstream publishes a new version. This already happened with EspWebUI.

**`include/gzip_*.h` are generated, not written by hand.** EspWebUI's build
script compresses `web/html`, `web/js` and `web/css` into those headers on every
build, and they are committed. Edit the sources under `web/`; expect large
generated diffs in `include/` whenever you do.

**Config changes need a `CFG_VERSION` bump.** `src/config.cpp` holds
`CFG_VERSION` and a comment block describing each version. Adding a field means:
extend the struct in `include/config.h`, write it in `configSaveToFile()`, read
it in `configLoadFromFile()`, set a default in `configInitValue()`, bump
`CFG_VERSION` and document it. A missing JSON key silently reads as 0/false, so
defaults matter.

**Do not use 0 as an "unset" sentinel for radio values.** `config.jaro.serial`
is a legal base serial when it is 0 (channel serials become `0x00…0x0F`), and a
live device runs that way. Changing the base serial re-derives every channel's
KeeLoq device key and forces re-learning all receivers.

## Task context — the rule most bugs here come from

Two contexts run concurrently:

- **`loop()`** (Arduino main task) — `jaroliftCyclic()`, `mqttCyclic()`,
  `webUICyclic()`, `configCyclic()`, `cyclicTelnet()`.
- **AsyncTCP task** — every ESPAsyncWebServer, WebSocket and AsyncMqttClient
  callback, i.e. `onMqttMessage()` and the EspWebUI element callback.

Anything touched from both sides needs a FreeRTOS primitive. Plain `std::queue`,
`bool` flags and shared buffers are **not** safe here, and several open items in
IMPROVEMENT-PLAN.md exist precisely because this was ignored. `webUI.cpp` shows
the intended shape — the WebSocket callback hands work to `loop()` rather than
acting directly — but it currently does so through a single unsynchronised slot.

Radio commands block `loop()` for a long time: a `CMD_SHADE` transmits for
~2.3 s and `cmdUnlearn()` runs ~5 s, against a 10 s watchdog that is only fed
from `loop()`. Do not add blocking work to a command path.

## Style

- `.clang-format` is authoritative (LLVM-based, 150 column limit, 2-space
  indent). Run `clang-format -i` on files you touch.
- Functions carry a banner comment block (`@brief` / `@param` / `@return`) —
  match the surrounding style.
- Log through `ESP_LOGx` with a file-local `static const char *TAG`.
- Comments explain *why*, in English. Some older comments are in German; leave
  them unless you are rewriting the code around them.

## Commits

One logical change per commit and per branch, branched off `upstream/main` (or
off the build-hygiene base when a build is needed), so each stays independently
PR-able upstream. Explain in the commit body what was wrong and how it failed in
practice, not just what changed.
