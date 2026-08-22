# SlipStream Firmware — Phase 2 Spec (OTA, WiFi, Images, Versioning)

## For a fresh Claude Code session

This is a continuation of the completed `SlipStream` firmware repo
(milestones 1–10, all done and committed). Before starting here, read:

- `thermal-printer-iot-spec.md` — the original spec, including confirmed
  hardware facts (which USB port is which, the VBUS jumper fix) and the
  established architectural patterns (pure-logic/hardware-glue split,
  concurrency model, workflow policy).
- `slipstream-handoff-summary.md` — a condensed summary of what's already
  built, including exact component APIs (`usb_printer_host_enqueue_print`,
  etc.), confirmed hardware facts (GPIO pins, printer VID/PID), and things
  flagged as relevant to this phase.

Don't re-derive anything already established in those two documents —
treat them as ground truth. This doc only covers **new** work.

Workflow policy is unchanged from Phase 1: one milestone at a time, stop
after each, don't commit, flag wrong assumptions rather than working
around them.

## Confirmed Architecture Decisions for Phase 2

- **BLE stays a standing local fallback.** Once WiFi/polling is working,
  the existing BLE GATT write characteristic remains active — you can
  still BLE-write a message directly at any time, not just during
  provisioning.
- **Firmware talks to the backend via HTTPS polling, not MQTT directly.**
  The device periodically checks in with the server (see the
  `slipstream-web` repo's API spec for the exact contract). MQTT is an
  internal-only implementation detail of the backend; the firmware never
  speaks it.
- **API credentials (server URL, API key) live in a gitignored
  `secrets.h`, compiled in at build time.** A checked-in
  `secrets.h.example` documents the required fields without exposing real
  values. This is the one class of secret that's genuinely fine to
  "hardcode" — it's never committed, and it removes an entire BLE
  provisioning flow that would otherwise be needed just to deliver it.
  Changing the API key or server URL requires a rebuild + wired reflash —
  an accepted tradeoff, piggybacking on the wired flash already needed for
  the OTA partition migration (M15).
- **WiFi credentials (SSID, password) are still delivered over BLE**,
  using the same generic BLE tool (nRF Connect/LightBlue) already proven
  out for message testing — no custom app. Kept intentionally simple: two
  separate plain-write characteristics, one for SSID and one for
  password, no delimiter parsing required. Stored in NVS.
- **The WiFi-credential BLE characteristics do not require
  bonding/encryption for v1**, per an explicit risk-acceptance decision —
  unlike an API key, a leaked WiFi password during a deliberate
  button-hold pairing window is a low-consequence exposure. Bonding/LE
  Secure Connections remains a cheap future hardening option, not a
  blocker.
- **OTA integrity:** SHA-256 checksum (corruption check) + application-
  level cryptographic signature verification (tamper check) — not full
  Secure Boot v2/eFuse-based secure boot. Rollback-safety is required: a
  new image must self-validate (successful first poll) before being
  marked permanently bootable.
- **OTA signature format (confirmed in `slipstream-web` M8 — firmware M22
  must match exactly):** Ed25519, chosen over ECDSA/RSA for a fixed-size
  (64-byte), DER-free signature — nothing to get wrong parsing ASN.1 on
  the firmware side. The signed image the server hosts/serves is
  `[original firmware bytes][64-byte Ed25519 signature]` — a single
  trailing 64-byte footer, no length prefix needed since the signature
  size is fixed for this algorithm. Firmware M22 must: strip the last 64
  bytes as the signature, treat everything before that as the actual
  image to flash, and verify the signature against that image using the
  Ed25519 public key corresponding to the maintainer's private signing
  key (`slipstream-web`'s `scripts/sign_firmware.py generate-key`) —
  that public key needs to be embedded in firmware, e.g. compiled into
  `secrets.h` or a dedicated constant, as a trusted-key decision this
  spec doesn't currently cover and M22 should account for. mbedTLS as
  shipped with ESP-IDF v5.5.x supports Ed25519 verification; no hardware
  crypto acceleration needed since this is a one-time per-OTA-event
  check, not a per-boot hot path.
- **OTA requires a one-time wired flash.** The current partition table
  (from Phase 1) doesn't have the dual-app-slot layout OTA needs — that
  layout can only be established via a wired flash, not OTA'd into
  existence. This is an accepted, planned exception to "no more cables,"
  done once early in this phase, not a surprise to hit later.
- **Device clock must be synced via SNTP before HTTPS/TLS is attempted.**
  Some TLS stacks reject certificates as invalid if the clock is still at
  its power-on default — an easy thing to lose an afternoon to if it's
  not planned for explicitly.
- **WiFi and BLE run concurrently, not exclusively.** WiFi takes priority
  on boot (auto-connects with stored credentials if present). The pairing
  button remains independently functional regardless of WiFi state — a
  3s hold opens BLE advertising whether or not WiFi is connected. This
  relies on the ESP32-S3's built-in software coexistence arbitration
  (enabled by default when both radios are active) rather than anything
  custom-built; given this device's light traffic (occasional BLE writes,
  a poll every 30–60s), contention isn't expected. Exclusivity (pausing
  one radio while the other is active) is a fallback only if real-world
  testing shows a problem — not something to build preemptively.
- **Image job payloads don't fit the existing fixed-size queue slot.**
  The print job queue (Phase 1, M3) uses a 256-byte payload per slot,
  sized for short text messages — an image is many kilobytes. Rather than
  growing that slot size (wastes RAM on the common text-job case), image
  jobs store a reference/handle in the queue slot; the hardware-glue layer
  fetches/streams the actual image bytes when that job is dequeued for
  printing. The pure-logic queue's slot size and structure stay unchanged
  for text jobs.
- **Super Text Mode needs no dedicated firmware work.** It's a web-side
  renderer (text in, banner bitmap out) that rides the same raster image
  print path as any other image — the device can't tell the difference
  between a photo and a rendered banner.
- **Image/banner rendering happens server-side.** The firmware never
  renders bitmaps or does dithering — it receives ready-to-print raster
  bytes and sends them via `GS v 0`, extending the existing
  `escpos_formatter` pure-logic component.
- **Text size uses the printer's native ESC/POS command** (`GS !`) —
  formatter extension only, no rendering involved.

## Open Item — confirm before scoping M17

Is the office WiFi a simple WPA2-PSK network, or enterprise auth
(WPA2/3-Enterprise)? The latter is a meaningfully larger firmware feature.
Don't assume PSK without confirming.

## Open Item — confirm before scoping M24/M25

The TM-H2000's actual print width in dots is unconfirmed. Server-side
image rendering (in the `slipstream-web` repo) needs this to know what
size bitmap to produce — check the printer's ESC/POS command reference or
test empirically rather than assuming a value.

## Milestones (continuing from Phase 1's M1–M10; M11–M14 are the
QR/image stretch milestones already scoped in the original spec)

15. **OTA-capable partition table migration** — replace the default
    single-app partition table with a dual-app-slot + `otadata` layout.
    Requires a wired flash — the last one expected for the lifetime of
    this device barring emergencies.
    - *Acceptance:* device boots normally post-migration on the new
      partition table; `idf.py partition-table` output confirms two app
      slots plus `otadata`; existing Phase 1 functionality (BLE print,
      USB host printing) still works unchanged.

16. **Firmware version embedding** — a semver constant, ideally
    CMake-injected from a git tag/short-hash at build time. Exposed via a
    new read-only BLE characteristic and logged at boot.
    - *Acceptance:* console banner and BLE read both report the same
      version string; changing the git tag and rebuilding changes the
      reported version without manual editing.

17. **`secrets.h` build integration** — gitignored header with
    `API_SERVER_URL` and `API_KEY` as compile-time constants; a checked-in
    `secrets.h.example` documents the required fields; add `secrets.h` to
    `.gitignore`.
    - *Acceptance:* a fresh clone with only `secrets.h.example` present
      fails to build with a clear error pointing at the missing file, not
      a cryptic compile error; `git status` never shows `secrets.h` as
      trackable.

18. **BLE-delivered WiFi credentials** — two simple plain-write GATT
    characteristics, one for SSID and one for password, written via a
    generic BLE tool (nRF Connect/LightBlue) — no custom app, no
    delimiter parsing. Stored in NVS. The existing plain-text-message
    characteristic keeps working unchanged (Phase 1 fallback path).
    Bonding/encryption is not required for v1 (see decision above).
    - *Acceptance:* SSID and password written via BLE persist across a
      reboot (read back from NVS); no WiFi credential ever appears as a
      compiled-in constant anywhere in the source.

19. **WiFi station mode bring-up** — connect using NVS-stored credentials,
    with retry/backoff. Falls back to BLE-only operation (no crash, no
    hang) if WiFi is unreachable or credentials are missing.
    - *Acceptance:* connects successfully with valid stored credentials;
      with WiFi unavailable, device remains fully responsive over BLE and
      retries WiFi on a sane backoff schedule.

20. **SNTP time sync** — sync the device clock before any HTTPS/TLS
    attempt.
    - *Acceptance:* console log confirms a valid current-date/time after
      sync completes, before the polling client (M21) makes its first
      request.

21. **HTTPS polling client** — periodic authenticated check-in to the
    server (interval configurable, sane default e.g. 30–60s), using the
    `secrets.h`-compiled server URL and API key. Parses the response for:
    queued print jobs, latest firmware version.
    - *Acceptance:* poll cycle visible in console logs; a manually-queued
      test job on the server side is retrieved and enqueued into the
      print job FSM within one poll interval.

22. **OTA update flow** — compare server-reported latest version to
    running version; if newer, download via `esp_https_ota`, verify
    SHA-256 then signature, flash to the inactive partition, reboot.
    Includes rollback-safety (mark-valid after first successful
    post-update poll; auto-revert otherwise). The print job queue lives
    in RAM — an OTA reboot with jobs still queued loses them, not just
    delays them. Wait until the on-device queue is empty before
    proceeding with an update, not just avoid interrupting an active
    print.
    - *Acceptance:* a deliberately-corrupted or unsigned test image is
      rejected without bricking the device; a valid signed image updates
      successfully and the new version is reported on the next poll; a
      simulated post-update failure (e.g. blocking the first poll)
      triggers rollback to the previous working image; an OTA check that
      arrives while jobs are queued defers the update until the queue
      drains, rather than proceeding immediately.

23. **ESC/POS text size formatter extension** — `GS !` width/height
    multiplier support in `escpos_formatter`.
    - *Acceptance:* host tests assert correct byte output for each
      supported size; physical print confirms visually correct scaling.

24. **Raster image print path** — extend the print job payload to carry
    either text or a bitmap reference; wire into the existing print job
    FSM/queue (builds on the M11–M14 stretch scope from the original
    spec, now promoted to real work rather than stretch).
    - *Acceptance:* a hardcoded test bitmap prints correctly via the same
      queue path as text jobs, no special-casing required at the queue
      level.

25. **End-to-end: server-queued image job → physical print** — full
    integration: server queues an image, device polls, downloads,
    prints.
    - *Acceptance:* an image queued via the backend's API prints
      correctly on physical hardware, matching the source bitmap.

## Non-Goals for Phase 2

- Full Secure Boot v2 / flash encryption (deliberately deferred — see
  security rationale above)
- WPA2/3-Enterprise WiFi support, unless confirmed necessary (see Open
  Item above)
- Multi-device fleet management (this spec assumes one hand-provisioned
  device)
- MQTT client code in the firmware — that stays server-side only
