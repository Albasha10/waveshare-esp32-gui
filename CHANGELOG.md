# Changelog — Modbus TCP setup (Ethernet → PLC)

Adds a working Modbus TCP master to the web server, configured from **IoT Config → Modbus → Modbus TCP (Ethernet)**.
Compiled with Arduino-ESP32 core 3.3.12 (ESP32S3 Dev Module, 4MB, Huge APP): 45 % of program space.
Modbus functions tested against a simulated NX1P2 station; the page was tested in a browser against a mock of the endpoints.
**Not yet flash-tested on the board.**

## Firmware (ESP32_GUI.ino)
- **Ethernet now actually starts.** The W5500 on this board needs its SPI pins (INT 12, MOSI 13, MISO 14, SCLK 15, CS 16, RST 39). The old `ETH.begin()` with no arguments never came up on this board.
  - Default: fixed IP **192.168.250.10 / 255.255.255.0**, no router. Internet stays on WiFi.
  - Changeable from the page; DHCP is also an option. The settings are saved in config.json under `"eth"`.
- **Modbus TCP client rewritten:**
  - checks MBAP transaction IDs;
  - reports PLC exception codes ("PLC exception 2 (fc 3 addr 200)");
  - 1 s timeout;
  - reconnects automatically;
  - keeps ok/error counters and the last error.
- **Writes over TCP:** coils use FC05; registers use FC06, or FC16 for float32. The existing "Write" button in the register table works over TCP when TCP is enabled.
- **Polling:** when TCP is enabled, every enabled register is read every poll interval. Before, it read one register per interval. MQTT publishing is unchanged and uses each register's topic.
  - Neighbouring registers with the same function code are read in **one request**. The NX1P2 station preset needs 2 requests per cycle instead of 22. If the PLC rejects a block (e.g. a gap address it doesn't have), those registers are read one by one.
  - The PLC connection is used without holding the register lock, so the web pages never wait on the PLC (`/getRS485Regs` returns 503 instead of an empty list if it is ever busy).
  - A register, or the PLC, only shows as failed after 3 bad cycles in a row, so one slow reply doesn't make the status flicker.
  - One lock guards the TCP socket, shared by the poll task, Test Connection and Write.
  - MQTT publishes of Modbus values are queued by the poll task (core 0) and sent from `loop()` (core 1). PubSubClient isn't thread-safe, and publishing from both cores could corrupt the MQTT connection.
- RS485 and Modbus TCP share the register list and are mutually exclusive: enabling one disables the other.
- `MAX_RS485_REGS` raised from 16 to 32. Register and config JSON buffers enlarged.
- **New endpoints:**
  - `POST /testModbusTCP` (ip, port, unitId): one test read, returns `{ok,msg}`;
  - `POST /saveEthernet` (dhcp, ip, mask, gw): saves, then restarts.
- `/getModbusTCP` also returns the counters, the last error, Ethernet link/IP and the WiFi IP.
- `/status` also returns `ethIp`.
- Fixed a newline inside a string literal in the Modbus TCP error print (compile error on newer cores).

## Web page (data/iot.html)
- Transport switch: **RS485 (serial) | Modbus TCP (Ethernet)**. A green dot marks the transport that is polling.
- **Modbus TCP Settings:**
  - PLC IP, port, unit ID, poll and publish intervals;
  - enable toggle and live "PLC Connected" badge;
  - **Test Connection**;
  - read/error counts and the last error.
- **Ethernet Port** card: fixed IP or DHCP, IP, mask and router, with link status and current IPs.
- **Load station preset**: fills the register list with the NX1P2 mini-station map (20 coils + AnalogIn1/2, topics `station/nx1p2/...`).
- The RS485 badge now shows "Polling Off" when RS485 is off, instead of reusing TCP results.

## Efficiency formula on the board; card editor on the KPI page; KPI next to Dashboard
- **Efficiency formula is adjustable and lives on the board.** Before, it was fixed in code as Runtime ÷ (Runtime + Downtime), and the Dashboard's "Custom Efficiency Formula" box only saved in that one browser. Now:
  - The formula is set on the **KPI page** and saved in config.json (`"effFormula"`; empty means the default `runtime / total * 100`).
  - The **firmware** works it out (`effEval()`, a small + − × ÷ ( ) parser), so the Dashboard card, the KPI page, the SD-card log and the Telegram status all show the same value.
  - Names you can use: `runtime`, `downtime`, `total`, `cycle` (seconds); `parts`, `rejects`, `good`; `[Register name]` for a PLC value.
  - Endpoint `POST /saveEffFormula` checks the formula and rejects unknown names, missing brackets and so on. `/getMetrics` returns `eff`, `effErr`, `formula`, `parts`, `good`.
  - The editor has click-to-insert name buttons, a PLC value picker, examples (Availability, Quality, A×Q, OEE = A×P×Q with an ideal cycle time) and a live "Result now" preview.
  - The old browser-only formula box was removed from the Dashboard. The calculator stays.
- **New Parts counter:** each completed cycle (Cycle start → Cycle end) counts one part. Saved with the metrics and reset with "Reset All Counters". **Good = parts − rejects.** Both are available as card sources too.
- **The Dashboard card editor moved to the KPI page** (section "Dashboard Cards", `/kpi#cards`), with a live "Now" column. The Dashboard's ⚙ Configure button now links there.
- **Nav order:** Dashboard, **KPI**, Signal Status, IoT Config, Modbus TCP, TagoIO, SD Card, Telegram.

## Dashboard: configurable Process Metrics + new Signal Status tab
- **Process Metrics** on the Dashboard are now up to **10 configurable cards**. Click **⚙ Configure**, then for each card set a **name**, what it **monitors** and an optional **unit**. Cards can be reordered (↑ ↓), removed and added. They're saved on the board (`/tiles.json`, endpoints `GET /getTiles`, `POST /saveTiles`), so every browser sees the same cards.
  - What a card can monitor:
    - KPIs: machine state, runtime, downtime, last cycle time, rejects, efficiency (the custom formula still applies);
    - any PLC (Modbus) value, with its words for on/off signals;
    - a PLC signal's time in its current state;
    - DI1–DI8 state, count and ON time;
    - DO1–DO8 state.
  - Each card's caption says where the value comes from (e.g. "PLC · AnalogIn1", "Last B1→B3 cycle").
  - Buttons: **NX1P2 station preset** (10 cards: machine, runtime, downtime, efficiency, cycle time, rejects, AnalogIn1/2, E-stop, mode) and **Original 5**. The page file ships with the station preset.
- **Signal Status** (`/signals`, data/signals.html, second in the nav) has the board's Digital I/O (DI with their names, DO with Toggle and "Rule" badges) and the PLC (Modbus) signal table with time in state. Both were moved off the Dashboard. The Dashboard keeps the DO logic rules, DI trigger counters, calculator and formula.

## KPI page: choose which signals drive the KPIs
- New **KPI** page (`/kpi`, data/kpi.html, in the nav after TagoIO). For each KPI role you pick a signal (DI1–DI8, or any on/off Modbus register by name) and whether it counts when **ON** or **OFF**:
  - **Machine running**: a level signal; RUNNING while active, STOPPED while not;
  - **Start** (→ RUNNING), **Stop** (→ STOPPED), **Toggle**;
  - **Cycle start** / **Cycle end**: the cycle time;
  - **Reject**: +1 each time it becomes active.
- The page shows each signal's live state (ACTIVE / not active / no data) and the KPIs now (machine state, runtime, downtime, last cycle, rejects, efficiency). It warns about conflicting settings and about Modbus polling being off.
- Buttons: **NX1P2 station preset** (running = TL_3_G, cycle B1 → B3, reject B4) and **Board inputs (original)**.
- **Defaults are the original fixed wiring**: DI1 turning OFF toggles, DI2 = start, DI3 = stop, DI4 → DI6 = cycle, DI7 = reject. An existing board behaves exactly as before until someone changes the settings.
- Firmware: `kpiTick()` in `loop()` replaces the hard-coded `i==3 / i==5 / i==6` and `autoUpdateMachineState()` calls in the DI scan. Events fire when a signal *becomes* active (not at start-up); a signal with no valid value (register missing, polling off, read error) keeps its last state. Saved in config.json under `"kpi"`. Endpoints: `GET /getKpi`, `POST /saveKpi`.
- Dashboard: the KPI card captions follow the settings (e.g. "Last B1→B3 cycle", "Count of B4").
- Setting the machine state over MQTT (`esp32/machine/state`) still works.

## Pages: Modbus TCP and TagoIO split out of IoT Config
- **IoT Config** (`/iot`) is back to MQTT + RS485, as it was originally. It has a note box showing whether Modbus TCP is on, with a link to its page.
- **Modbus TCP** (`/modbus`, data/modbus.html) has the PLC connection settings (IP, port, unit ID, intervals, Test Connection, status), the Ethernet port settings and the register list, with **Load station preset**.
- **TagoIO** (`/tagoio`, data/tagoio.html) has the token, API URL, a **Region** picker (US / Europe) and status, plus a live table of every variable being sent and its current value.
- The nav bar on every page now reads: Dashboard, IoT Config, Modbus TCP, TagoIO, SD Card, Telegram. The Dashboard's Modbus table "Configure" link goes to `/modbus`.
- **Per-register MQTT / TagoIO switches.** The register table has two switch columns, **MQTT** (Node-RED) and **TagoIO**, that save as soon as they're clicked. The TagoIO page has a **Send** checkbox per variable, plus **Send all** / **Send none**. They're saved in rs485_regs.json as `"mqtt"` / `"tago"` (default on). A switched-off register is still read from the PLC and still shows on the Dashboard; it just isn't published. `plc_online` is always sent.
- **TagoIO variable per register** (optional, saved as `"tv"`). Empty means it's made from the register name. Renaming a register in the form keeps its old TagoIO variable automatically, so the TagoIO widgets keep updating. Without this, renaming "ES0" to "Emergency button" switched TagoIO to a new variable, `emergency_button`. When it differs from the name, the table shows "TagoIO: es0" under the register name.
- **Time in current state** for on/off registers. The board tracks when each value last changed and sends it as `"since"` (seconds) in `/getRS485Regs`. The Dashboard's Modbus table has a **For** column ("5s", "1m 15s", "1h 02m", "2d 4h"), and the register table on the Modbus TCP page shows "for …" under each on/off value. It starts again on every change and after a reboot. It is shown on the board only; nothing extra goes to MQTT or TagoIO.
- The register list (one shared list for RS485 and Modbus TCP) is edited **only on the Modbus TCP page**. IoT Config shows how many registers are configured, with an "Edit registers" link. Turning one transport on turns the other off.

## TagoIO, direct from the board (WiFi, HTTPS)
- New **TagoIO (cloud)** card on IoT Config: device token (stored on the board, never shown again), API URL, on/off switch and live status ("OK, sent N values", "rejected (401) - check the device token", "waiting for WiFi", ...).
- The `tagoTask` (its own FreeRTOS task, so a slow upload never delays Modbus polling) sends every enabled register:
  - on/off values as soon as they change, at most every 2 s;
  - number values at most every 10 s;
  - everything every 5 minutes;
  - plus `plc_online`.
- Variable name = the register name in lowercase, e.g. `es0`, `tl_1_r`, `analog_in1`. Values are the raw PLC values (ES0: 1 = safe).
- **Text for 1 / 0** (coils and discrete inputs): optional words per register, e.g. DETECT / CLEAR. They're shown on IoT Config and on the Dashboard, and sent to TagoIO instead of 1/0. MQTT still sends 1/0. They're saved in rs485_regs.json as `"on"`/`"off"`. The station preset fills them in (ES0: OK/PRESSED, PB_RED: RELEASED/PRESSED, B1-B3: DETECT/CLEAR, B4: METAL/CLEAR, ...).
- **WiFi watchdog:** if WiFi has been down for 20 s, a fresh connection is started. The built-in auto-reconnect sometimes gives up.
- The TagoIO token box is a plain text field, so browsers don't auto-fill the login password into it. Europe-region accounts need the API URL `https://api.eu-w1.tago.io/data`.
- Endpoints: `GET /getTago`, `POST /saveTago` (enabled, token, url, clear).
- TLS without certificate check (`setInsecure`), fine for the lab; add TagoIO's root CA for a customer deployment.

## Dashboard page (data/index.html)
- New **Modbus Live Data** table under Digital I/O: every enabled register with its live value (ON/OFF for coils, numbers for registers), the active source (Modbus TCP ip:port / RS485 / off), a connection badge and a link to IoT Config. Rows briefly highlight when a value changes. Long lists split into two columns.

## How to use
1. Arduino IDE 1.8.x with the esp32 core and the ESP32 LittleFS upload tool. Upload the sketch, then **Tools → ESP32 Sketch Data Upload** (the page changed, so the data upload is needed).
2. Plug the board's RJ45 into the PLC network. The PLC (192.168.250.7) and this board (192.168.250.10) must be on the same subnet.
3. Open the web UI (WiFi IP, or http://192.168.250.10 from a laptop on 192.168.250.x) → IoT Config → Modbus TCP:
   - enter the PLC IP and press **Test Connection**;
   - press **Save TCP Settings**;
   - press **Load station preset** (or add registers);
   - switch polling **On**.

---

# Changelog — DO Reset via MQTT + Toggleable Modbus/Telegram Services

**Base file used:** `ESP32_GUI_DI_ONLY_ORIGINAL.ino` (your confirmed-working file: DI MQTT reset present, Wi-Fi credentials hard-coded, Telegram/RS485 tasks disabled)
**Cross-referenced against:** the ZIP you uploaded (`Waveshare-ESP32-Web-GUI-main`), specifically `src/data/*` (the real dashboard HTML/CSS/JSON, previously unseen) and `src/ESP32_GUI.ino` (the pre-MQTT-reset original)

This file merges both, plus adds the two features you asked for. Nothing here has been compiled or tested on hardware — please build and flash-test before relying on it.

## 1. DO reset via MQTT (and HTTP) — same design as before, reapplied cleanly

- `doTriggerCount[8]`, `doTotalOnTimeMs[8]`, `doLastOnStartMs[8]` — declared directly above `setOutput()` (not in the METRICS block further down), so they're defined before first use. This was the exact bug that broke compilation last time; fixed at the source this time.
- `setOutput()` now increments `doTriggerCount` and tracks on-time, mirroring what the DI scan already does.
- `getDoOnTime()` — new helper, mirrors `getOnTime()`.
- `resetMetricsByTarget()` — now accepts `do0`–`do7`, and `"all"` clears both DI and DO.
- `saveMetrics()`/`loadMetrics()` — DO arrays persisted through reboots.
- `/getMetrics` — now returns `doTriggers`/`doOnTimes` alongside the existing `triggers`/`onTimes`.
- MQTT (`esp32/counters/reset`) and HTTP (`/resetMetrics?target=...`) both call the same shared function, so both interfaces support DO resets identically — no duplicated logic to keep in sync.

## 2. Modbus (RS485) and Telegram — toggleable, off by default

**The core problem:** both background tasks were only ever started once, in `setup()`, via `xTaskCreatePinnedToCore(...)`. Commenting that line out (which you did, to fix instability) doesn't just disable the feature — it means there's no task alive at all for any "enable" toggle to control. Telegram already *had* an enable toggle in the dashboard (`tgConfig.enabled`, wired in `telegram.html`) that's been silently doing nothing, because there was no running task left to read it.

**The fix — idle-when-disabled tasks, not create/delete-on-demand:**

I deliberately did **not** implement start/stop by dynamically creating and destroying the FreeRTOS tasks at runtime (`xTaskCreatePinnedToCore()` / `vTaskDelete()` on toggle). That approach carries a real risk: if a toggle-off request happens to land while the task is mid-way through an I2C transaction or holding `rs485Mutex`, deleting the task externally can leave that mutex permanently locked, hanging anything that later tries to take it. Given you already had stability problems with these tasks once, I didn't want to introduce a second, different way for them to fail.

Instead, both tasks are now **always created** at boot (like the ZIP's original source), but check an enabled flag on every loop iteration and do nothing at all when it's off:

```cpp
// rs485Task — new check added:
if(!rs485Config.enabled) continue;   // idles harmlessly, no I2C/mutex activity while off

// telegramTask — this check already existed, unchanged:
if(!tgInitialized||!tgConfig.enabled) continue;
```

- **RS485:** new `rs485Config.enabled` field (default `false`), persisted in `config.json`. New `POST /toggleRS485` endpoint flips it and saves. New toggle switch added to `iot.html`, next to the existing device-connected status badge.
- **Telegram:** no backend flag needed — `tgConfig.enabled` already existed, already defaults to `false` in `telegram.json`, and the existing "Enable Bot" toggle in `telegram.html` already saves it correctly via `/saveTelegramConfig`. The only change needed was re-enabling the task creation line in `setup()` — the toggle will now actually mean something for the first time.

**Both default to off**, matching what you asked for.

**A caveat I can't resolve from code alone:** I don't know what actually caused your original instability — whether it was specifically `tgConfig.enabled`/logic issues, or simply the memory/stack overhead of these tasks existing at all (12,288 bytes for Telegram, 4,096 for RS485) on a device already running WebSockets, MQTT, a web server, SD logging, and Modbus concurrently. Idling the tasks avoids their *active* work (I2C bus traffic, TLS handshakes, HTTP polling to Telegram's servers) but their stack allocation still exists in RAM at all times now, whether enabled or not. If instability returns even with both toggled off, that would point to the memory overhead itself being the issue, not the logic — worth watching `/getHealth`'s `freeHeap` value over time as a first diagnostic if that happens.

## 3. Frontend changes

- `data/iot.html` — added an "Enable Modbus Polling" toggle (same `sd-toggle` style already used elsewhere in your UI) next to the RS485 status badge, wired to the new `/toggleRS485` endpoint.
- `data/telegram.html` — **no changes.** Already fully wired; it just needed a live task on the backend to actually control.

## 4. Not addressed — needs your input

- **WhatsApp bot:** No WhatsApp integration exists anywhere in any version of this code (ZIP source, DI-only file, or this merged version) — only Telegram. I did not build a stub or placeholder for this, since guessing at an unspecified integration risked adding more confusion rather than less. If you want this, it's a new feature to scope from scratch (WhatsApp Business API, or a third-party bridge like Twilio — different providers have very different setup requirements).
- **Item 4 from your list:** was left blank — let me know what it was.

## Verification performed (structural only — not a substitute for a real compile)

- Brace count: 300/300 balanced
- Parenthesis count: 1642/1642 balanced
- Confirmed no recurrence of the earlier known bugs (stray `}S`, missing closing paren on the RS485 task line — which was actually present but hidden while that line was commented out, and is now fixed)
- Confirmed `do`/`di` reset targets both present in `resetMetricsByTarget()`
- Confirmed both task-creation lines are active (not commented) and both tasks have their respective idle-checks in place

**Not verified:** actual compilation in Arduino IDE, and behavior on real hardware — please test both before deploying.
