# SlipStream — TM-H2000 IoT Receipt Printer Firmware Repo Spec

## Overview

Firmware for an ESP32-S3 that turns an Epson TM-H2000 thermal receipt printer
into a device people can send short physical messages to. This spec covers
the **first firmware repo**: proving out USB-host printing and BLE message
receipt, with a print job queue in between. Remote/MQTT delivery is
explicitly out of scope for this repo — it's a fast-follow, not part of this
build.

## Target Environment

- **ESP-IDF: 5.5.1** (assume already installed/activated in the dev
  environment — do not attempt to install IDF itself)
- **Chip: ESP32-S3**, native USB-OTG used for USB host mode
- **Printer interface:** Epson TM-H2000 over USB, ESC/POS command set
- **Build system:** standard `idf.py` / CMake, no custom build tooling

### Open hardware item (do not assume — verify against the physical board)

The dev board has two USB-C ports. On most ESP32-S3 boards, only **one** is
wired to the native USB-OTG peripheral (GPIO19/GPIO20) — the other is a
USB-UART bridge chip used for flashing and console logs. Confirm which port
is OTG (check board silkscreen/datasheet, usually labeled "USB" vs
"UART"/"COM") before wiring the printer. Also confirm the board can source
5V VBUS in host mode to power the printer's USB interface, or note that an
externally-injected 5V may be required.

## Design Principle: Pure Logic vs. Hardware I/O

Split business logic from ESP-IDF/hardware dependencies wherever
possible, specifically:

- **Pure-logic components** (no `esp_*`/FreeRTOS includes): state machines,
  the ESC/POS byte formatter. These compile and run as plain C, testable
  with Unity on the host machine (gcc), no ESP32 attached, no docker.
- **Hardware components**: USB host driver glue, BLE peripheral glue, GPIO
  button handling. These call into the pure-logic components but aren't
  themselves unit tested on host — they get validated on-device per
  milestone.

Suggested repo layout:

```
/main                      — app_main, wiring components together
/components
  /print_job_fsm           — pure logic
  /ble_session_fsm          — pure logic
  /escpos_formatter          — pure logic
  /usb_printer_host          — hardware glue (USB host + printer I/O)
  /ble_peripheral            — hardware glue (BLE GATT server, button)
/test
  /host                      — Unity tests for pure-logic components, run via gcc
.github/workflows
  build.yml                  — idf.py build (targets esp32s3)
  test.yml                   — host unit tests (no IDF/docker needed)
```

## State Machines (pure logic, unit test these thoroughly)

### 1. Print Job State Machine
States: `IDLE → FORMATTING → SENDING → PRINTING → COMPLETE`, with an
`ERROR` state reachable from any active state. Sits behind a small FIFO
queue so multiple incoming messages don't get dropped while one is
printing.

**Confirmed in M3 review:** a single, persistent FSM instance is reused
across jobs — not a fresh instance per job — via an explicit `RESET`
transition legal only from `COMPLETE` or `ERROR` (not from any active
state). This gives whatever drives the FSM a defined moment to observe a
finished/failed job (update the LED, log it, decide on a retry) before the
instance cycles back to `IDLE` for the next queued job. There is currently
no mid-flight cancellation path — a job runs to a terminal state before the
FSM can advance. Revisit if hardware-fault-triggered abort becomes
necessary later.

### 2. BLE Session State Machine
States: `IDLE → ADVERTISING (button press) → CONNECTED → RECEIVING → IDLE`,
with a timeout path back to `IDLE` from `ADVERTISING` or `CONNECTED`.

**Confirmed in M4 review:** a single `TIMEOUT` event covers both triggers
that land on that timeout path — an elapsed advertising/connection window,
and a peer connecting then disconnecting before sending anything. The FSM
intentionally doesn't distinguish them (both are "give up, return to
`IDLE`"). Hardware glue (M9) is expected to log which real-world trigger
(timer vs. BLE disconnect callback) actually fired before calling
`ble_session_fsm_handle_event` — that context is useful for debugging but
doesn't belong in the state machine itself.

Write exhaustive unit tests for both: every legal transition, and confirm
illegal transitions are rejected/no-op rather than crashing.

## Workflow & Commit Policy

- Implement **one milestone at a time**, then stop and hand control back —
  do not chain into the next milestone automatically.
- **Do not commit.** Leave changes unstaged/uncommitted in the working
  tree. Kyle reviews the diff, runs any hardware-dependent validation
  himself, and makes the commit.
- Keep each milestone's diff self-contained and revertable on its own —
  don't fold unrelated cleanup or "while I was in there" changes into a
  milestone's changes.
- If a milestone surfaces a wrong earlier assumption (cut-command support,
  which USB port is OTG, etc.), stop and flag it rather than quietly
  working around it.

## Concurrency Model (applies from M8 onward)

The pure-logic FSM/queue components (print job FSM, BLE session FSM) have
zero internal locking by design — they're plain structs, single-threaded
by construction, matching the "pure logic" split above. That's fine in
isolation, but M8/M9 introduce real concurrency: BLE characteristic writes
land on a BLE callback/task while a separate task drains the print queue.
Concurrent push/pop on the same struct without protection is a real data
race that host unit tests cannot catch (they're single-threaded).

**Required pattern:** exactly one FreeRTOS task owns the print job FSM and
its queue. Every other task or callback (BLE write callback, later an MQTT
callback) communicates with it by sending messages through a FreeRTOS
queue, not by calling into the FSM/queue struct directly. Do not add a
mutex to the pure-logic components as a substitute for this — task
ownership is the idiomatic ESP-IDF approach and keeps the pure-logic
components genuinely thread-naive, which is what keeps them host-testable.

## Milestones

Each milestone should be a small, independently reviewable chunk of work
with a clear, concrete acceptance criteria. Don't bundle milestones —
implement one, stop (per Workflow above), and let it be validated before
moving to the next.

1. **Environment sanity check** — Repo skeleton builds and flashes against
   IDF 5.5.1, targeting esp32s3. App logs a version/build banner over the
   console. Include the `.gitignore` below as part of this milestone.
   - *Acceptance:* `idf.py set-target esp32s3 && idf.py build flash monitor`
     completes with no errors; banner appears in monitor output; `git
     status` after a build shows no build artifacts as untracked/dirty.

2. **CI skeleton** — GitHub Actions with two jobs: (a) firmware build via
   `idf.py build` against IDF 5.5.1, (b) host unit test job that compiles
   and runs a single trivial passing Unity test with plain gcc, no IDF
   dependency.
   - *Acceptance:* both jobs green on a fresh PR; job (b) finishes in well
     under a minute since it never touches IDF/docker.

3. **Print job FSM (pure logic)** — Implement states/transitions and the
   FIFO queue in front of it.
   - *Acceptance:* `test/host` builds and runs standalone (gcc + Unity, no
     `idf.py`) and passes; suite exercises every legal transition and
     confirms illegal transitions are rejected/no-op, not crashing.

4. **BLE session FSM (pure logic)** — Same treatment as M3.
   - *Acceptance:* standalone host-test pass with full transition
     coverage, including the timeout paths back to `IDLE`.

5. **ESC/POS formatter (pure logic)** — Given a plain-text string, produce
   the byte buffer (init sequence + text + line feed, cut command flagged
   TBD until printer capability is confirmed).
   - *Acceptance:* host tests assert exact byte-for-byte output against a
     handful of known-good sample strings, including at least one edge
     case (empty string, embedded newline).

6. **USB host bring-up (hardware)** — Initialize USB-OTG in host mode,
   enumerate the printer, log its device descriptor (VID/PID, interface
   count) — no printing yet.
   - *Acceptance:* console log shows the printer's descriptor on connect;
     unplug/replug is logged cleanly too (no crash or hang on disconnect).

7. **First physical print (hardware)** — Wire the ESC/POS formatter (M5)
   output through the USB host connection (M6) to print one hardcoded
   string.
   - *Acceptance:* physical receipt prints the exact expected text,
     correctly formatted, no garbled characters.

8. **Print queue integration (hardware + logic)** — Wire the print job FSM
   (M3) in as the actual driver of the print path from M7, so multiple
   incoming jobs queue and print in order.
   - *Acceptance:* enqueue 3 hardcoded messages back-to-back (rapid-fire,
     not spaced apart); all 3 print in enqueued order, none dropped or
     duplicated.

9. **BLE peripheral + pairing button (hardware)** — GPIO button triggers
   BLE advertising per the BLE session FSM (M4); expose one GATT write
   characteristic.
   - Debounce the raw button signal, and additionally require a
     continuous **3-second hold** before treating it as an intentional
     "enter pairing mode" event (debounce filters electrical noise;
     the hold requirement guards against accidental bumps/brief catches).
   - Advertising times out after **30 seconds** with no connection,
     returning the FSM to `IDLE`. Keep this as a named constant, easy to
     tune later.
   - Reuse the onboard RGB LED to reflect BLE session state: e.g. blinking
     while `ADVERTISING`, solid while `CONNECTED`, off at `IDLE`.
   - Default behavior: button press is only actioned from `IDLE` (starts
     advertising); presses while `ADVERTISING` or `CONNECTED` are ignored.
     Revisit if a cancel/restart-on-repress behavior is wanted later.
   - *Acceptance:* holding the button for 3s from `IDLE` starts
     advertising (LED reflects it); a brief/bouncy press does nothing;
     connecting with nRF Connect/LightBlue and writing arbitrary text to
     the characteristic logs it verbatim over console; advertising times
     out and returns to `IDLE` (LED off) after 30s with no connection.

10. **End-to-end: BLE message → physical print** — Wire BLE characteristic
    writes (M9) into the print job queue (M8).
    - *Acceptance:* send text via nRF Connect/LightBlue, confirm a
      matching physical receipt prints; send two messages back-to-back and
      confirm both print, in order.

## Explicit Non-Goals for This Repo

- No custom mobile app or PWA — use a generic BLE tool (nRF Connect /
  LightBlue) for all BLE testing in this repo
- No WiFi, no MQTT, no remote delivery — future repo/milestone set
- No OTA firmware updates
- No power-sharing/buck-converter firmware concerns — that's hardware, not
  in scope here

## .gitignore

Standard ESP-IDF ignores — use this verbatim as the repo's `.gitignore`:

```
# Build output
build/
sdkconfig.old

# IDF component manager
managed_components/
dependencies.lock

# VS Code / CLion IDE cruft (only if not intentionally tracked)
.vscode/
.idea/
compile_commands.json

# macOS
.DS_Store

# Host unit test build artifacts
test/host/build/
*.o
*.gcno
*.gcda
```

Note: `sdkconfig` itself (without `.old`) is intentionally **not** ignored —
it's the project's actual configuration and should be tracked so the build
is reproducible for anyone who clones the repo.

## Notes for Implementation

- Prefer IDF's built-in `usb_host` component; there's no ready-made USB
  Printer Class driver in IDF, so bulk-transfer handling for the printer
  will need to be implemented directly (bulk OUT endpoint, no complex
  negotiation required for basic ESC/POS printing).
- Keep the ESC/POS command set minimal for M5–M7 (init, text, line feed);
  treat cut-command support as unconfirmed until verified against the
  physical printer.