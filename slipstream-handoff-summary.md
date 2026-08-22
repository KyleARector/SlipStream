# SlipStream Firmware Repo — Handoff Summary

Context for whoever drafts the spec for the "web portion" (remote/MQTT delivery) follow-on repo. This repo (`SlipStream`) is done: all ten milestones from `thermal-printer-iot-spec.md` are complete, committed, and validated end-to-end on real hardware. Nothing here needs finishing — this is background so the new spec stays consistent with what's already built and decided.

## What this repo does

ESP32-S3 firmware that turns an Epson TM-H2000 thermal receipt printer into a device people can BLE-write short text messages to, and it prints them. Full pipeline, working: phone (nRF Connect/LightBlue) → BLE GATT write → print job queue → USB-host bulk transfer → physical receipt with cut. No WiFi, no MQTT, no remote delivery, no custom mobile app — all explicitly out of scope for *this* repo, which is presumably exactly what the web/remote portion is meant to add.

## Architectural patterns established (worth matching for consistency)

**Pure logic vs. hardware I/O split.** Business logic components (state machines, byte formatters) have zero `esp_*`/FreeRTOS includes and compile as plain C, unit-testable on the host machine via gcc — no ESP32, no docker. Hardware-glue components (USB host driver, BLE peripheral, GPIO) call into the pure-logic components but aren't themselves unit tested; they're validated on-device. This split is why the FSMs stayed fully unit-tested through the whole build.

**Repo layout convention:**
```
/main                    — app_main, wiring components together
/components
  /print_job_fsm         — pure logic
  /ble_session_fsm       — pure logic
  /escpos_formatter      — pure logic
  /usb_printer_host      — hardware glue
  /ble_peripheral        — hardware glue
/test/host               — Unity tests, run via gcc + a plain Makefile (no CMake — stayed proportional to actual test-suite size; only 4 small binaries)
.github/workflows        — build.yml (idf.py build) + test.yml (host tests, no IDF/docker)
tools/                   — standalone Mac-side test scripts (e.g. a direct-USB printer test using pyusb), separate from firmware milestones
```

**Concurrency model — this is the pattern most likely to matter for the web/remote repo too:** pure-logic FSM/queue components have zero internal locking by design (plain structs, single-threaded by construction). The rule: exactly ONE FreeRTOS task owns each FSM/queue pair directly; every other task or callback (BLE write callback, a future MQTT callback, etc.) hands off via a FreeRTOS message queue rather than touching the struct directly. No mutexes bolted onto the pure-logic components — task ownership is the idiomatic ESP-IDF approach and keeps those components genuinely thread-naive (which is what keeps them host-testable). Expect this exact pattern to recur for any new "queue driven by an async network callback" work.

**Workflow policy** (drove the entire build, worth adopting for the next repo too):
- One milestone at a time; stop and hand control back after each, don't chain automatically.
- Don't commit — the repo owner reviews, tests on hardware, and commits himself.
- Keep diffs self-contained per milestone; no unrelated cleanup folded in.
- If a milestone surfaces a wrong earlier assumption, stop and flag it rather than quietly working around it. This happened multiple times and each confirmed decision got written back into the spec doc itself (see "Confirmed in M3/M4/M9 review" notes) so the spec stayed the single source of truth.

## Components already built (reusable reference, not to be duplicated)

- **`print_job_fsm`** — `IDLE → FORMATTING → SENDING → PRINTING → COMPLETE`, `ERROR` from any active state, `RESET` legal only from `COMPLETE`/`ERROR` (single persistent instance reused across jobs, not one per job). Sits behind a small FIFO queue (capacity 8, 256-byte text payload per job).
- **`ble_session_fsm`** — `IDLE → ADVERTISING → CONNECTED → RECEIVING`, with `RECEIVING → CONNECTED` (not `IDLE` — this was a real bug found and fixed via hardware testing; `IDLE` means strictly "no BLE link at all," so a session can receive multiple messages in a row without dropping). `TIMEOUT` covers both an elapsed advertising window and an actual peer disconnect — hardware glue decides which fired and whether to auto-resume advertising (it does, on a genuine disconnect; a from-scratch timeout requires a fresh physical button hold).
- **`escpos_formatter`** — pure-logic, `text in → ESC/POS bytes out` (init + text + one line feed). No cut command in the pure-logic formatter — cut bytes are appended separately in the hardware-glue layer as a deliberate, flagged addition, since cut-command support was unconfirmed against the physical printer until real-hardware testing proved it out.
- **`usb_printer_host`** — owns the USB Host Library lifecycle, enumerates the printer, and exposes `usb_printer_host_enqueue_print(text, text_len)` as the one public entry point for "print this text" — safe to call from any task/context, hands off via its own FreeRTOS queue. This is the exact integration point any future message source (BLE now, MQTT later) should call.
- **`ble_peripheral`** — pairing button (debounce + 3s hold) + onboard RGB LED status (WS2812B) + NimBLE GATT server (one write characteristic, no security/bonding — intentionally open, since there's no server/identity to protect against yet).

## Confirmed hardware facts (Kyle's specific board — may not transfer to different hardware)

- Board: unbranded ESP32-S3-WROOM-1 devkit, electrically matching a "YD-ESP32-S3 / AI-S3" reference design (schematic/pinout/photo are in the SlipStream repo root).
- Two USB-C ports: one native USB-OTG (GPIO19/20), one CH343 UART bridge (flashing/console). The OTG port needed a solder-bridged jumper to source VBUS for host mode — not populated by default on this board.
- Onboard WS2812B RGB LED on **GPIO48** (confirmed via schematic; a separate pinout image had this wrong).
- Pairing button on GPIO4, active-low, internal pull-up.
- Printer: Epson TM-H2000, `VID=0x04B8 PID=0x0202`, vendor-specific interface (`0xFF`), bulk OUT `0x01` / bulk IN `0x82`, 64-byte packets, Full Speed.

## Things flagged during this build that are directly relevant to the web/remote spec

- **Secrets/local config pattern (discussed, not yet built — nothing in this repo needed it).** Recommended approach for whenever WiFi/MQTT credentials enter the picture: a gitignored header (e.g. `secrets.h`) with a checked-in `secrets.h.example` template, rather than putting secrets in Kconfig/`sdkconfig` (which this repo deliberately tracks in git for build reproducibility — secrets can't live there).
- **Device identity / anti-spoofing.** Explicitly out of scope here since there's no server yet. This BLE peripheral has zero pairing security (no bonding, no encryption) — deliberate, since physical button-press proximity is the only "trust" mechanism needed for a local-only BLE demo repo. Once a server exists for the web/remote portion, "how does the server know this is really Kyle's device" becomes a real question that this repo never had to answer.
- **Public-repo hygiene** was a stated concern (this is/will be a public repo) — worth carrying that awareness into the new spec too.

## Non-goals from this repo's spec (candidates for the new one to actually own)

WiFi, MQTT, remote delivery, OTA firmware updates, custom mobile app/PWA. Also noted but unrelated to the web portion: QR/image printing is a separate stretch milestone set (M11–M14) parked in this repo's own spec, not part of the web/remote work.
