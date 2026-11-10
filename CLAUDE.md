# CLAUDE.md — Smart Home Control Panel

ESPHome + LVGL firmware for the Waveshare **ESP32-S3-Touch-LCD-7B** (1024×600 RGB,
GT911 touch, CH32V003 I2C IO expander) with a web editor served by the device.
User-facing docs: `README.md`. The user writes in Russian; on-screen and editor
UI texts are Ukrainian.

## Map

| Path | What |
|---|---|
| `panel-s3.yaml` | **Public** firmware (built by CI, published in releases). No secrets, ever. |
| `panel-local.yaml` | Git-ignored personal build: public firmware + API key + LAN OTA password from `secrets.yaml`. Template: `panel-local.example.yaml`. |
| `simulator.yaml` | Host/SDL build; no web server. Shares `base_logic.yaml`, `gui.yaml`, `config/panel/*`. |
| `config/panel/panel_cards.h` | Runtime pages/cards: parsing, rendering, HA subscriptions/actions, alarm, charts. |
| `config/panel/configurator.js/.css` | Web editor (`web_server.js_include`). Runs standalone because `js_url: ""`. |
| `config/panel/system.yaml` | Brightness, sleep, status bar (battery bar `bat_fill`), 1 s interval (`panel_cards::tick()`, alarm wake). |
| `config/panel/top_layer.yaml` | Status bar + bottom nav (LVGL top layer). |
| `config/panel/screenshot.{h,yaml}` | `GET /screenshot.bmp` (panel-s3 only). |
| `config/panel/icons.txt` | Source of truth for icons → `python3 tools/gen_icons.py` (needs `fonttools`). |
| `components/gt911/` | Local copy of the ESPHome 2026.9 GT911 driver (reset timing + retries). |
| `tools/configurator_preview.html` | Editor preview with fake device + fake HA (`#icons`, `#search`, `#chart`, `#settings`). |
| `.github/workflows/` | `ci.yml` (push/PR), `release.yml` (tag `v*` → release + OTA manifest). |

## Working with the device

* Address: `192.168.1.178` (DHCP since the secret-free firmware; mDNS `esp32-s3-touch-lcd-7b.local`). A USB cable is often attached (`/dev/cu.usbmodem…`).
* **Always pass `--device`**: with USB attached `esphome run` stops at an interactive "USB or OTA?" prompt and hangs.
  * build + flash: `esphome run panel-local.yaml --device 192.168.1.178 --no-logs`
  * logs: `esphome logs panel-local.yaml --device 192.168.1.178` (run in background, kill after a few seconds; boot-time lines are not visible over Wi-Fi — the config dump is)
* See the screen: `curl -s -o s.bmp http://192.168.1.178/screenshot.bmp && sips -s format png s.bmp --out s.png`, then Read the PNG. Verify UI changes this way after flashing.
* Every flash/reboot resets chart history (RAM only, by design).
* Web server POSTs need a body: `curl -d "" "http://IP/text/Panel%20Card%201/set?value=..."` (otherwise 411).
* Check the touchscreen after flashing: config dump line `GT911 … Setup: OK after N attempt(s)`.

## Secrets model (repo is PUBLIC)

* Never put keys/passwords/SSIDs into `panel-s3.yaml`. The old API key leaked in git history and was rotated.
* Wi-Fi and the API key are stored on the device (fallback AP / Improv; HA provisions the key). `provisioning: timeout: 15min` — only right after power-up.
* LAN OTA password = API key (base64), set at runtime from the saved key (`#ifdef USE_OTA_ENCRYPTION_PROVISIONED` interval in `panel-s3.yaml`); `panel-local.yaml` compiles it in via `lan_ota_password`.
* `secrets.yaml` keys: `wifi_ssid`, `wifi_password`, `ota_password` (old firmware only), `api_key`. Don't print them; use `pbcopy` if the user needs one.
* Docs screenshots of the editor use demo data from the preview, not the real device (real entity IDs reveal the user's location).

## Firmware conventions

* Card text entity: `type;entity;label;icon;page;size[;period]`. Types: `toggle | sensor | action | subpage | alarm | chart | clock` (alarm/clock use a placeholder entity). Sizes 1–5 (5 = whole page). Adding a type touches: `panel_cards.h` (`set_config` whitelist, `icon_glyph`, `render_slot`, `on_click`, subscriptions) and `configurator.js` (`TYPES`, `TYPE_SHORT`, `TYPE_DOMAINS`, form `sync`, tile subtitle) + README.
* Clock fonts `clock_56/110/200` contain only `0123456789:-`; date uses `roboto_22`/`roboto_36`.
* Fonts contain only listed glyphs: `mdi_36` from `icons.txt`; Roboto has Latin/Cyrillic core only (no `→` etc. — check text on a screenshot).
* LVGL: in flex containers use `flex_align_track: center` to centre vertically; the status block in the header is `floating` + `RIGHT_MID` (flex pushed it below the 50 px bar). Snapshots of the top layer are taken per child (a full-screen ARGB buffer fails on fragmented PSRAM).
* Air alarm (Ukraine Alarm): one setting `Panel Alarm Sensor` = `binary_sensor.<region>_air`; siblings by suffix; siblings with another entity_id (HA may prefix an area, e.g. `binary_sensor.<area>_<region>_air_yellow`) are found by the editor via HA (friendly name) and stored after `;` — the firmware maps them by suffix. Max 255 chars. `Air` is on together with `Air (red)`/`Air (yellow)`; the typed one decides the colour (yellow = drones, red = ballistic/other). `Air` alone = red.
* Charts: `LV_USE_CHART` via build flag, 120 buckets per period, chart drawn as card background, min visible range 2 % of the value.
* **Internal RAM is tight** (~20 KB free). When it drops to ~10 KB, Wi-Fi silently loses every outgoing packet over ~1200 bytes: pages, `/0.js`, screenshots and the log dump stall while small requests and OTA still work (`ping -s 1400 <ip>` fails). Put buffers in PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, see chart `History`), no big static arrays. To measure: temporarily add `debug:` + `sensor: platform: debug (free, block, psram)` to `panel-local.yaml`.
* After editing `icons.txt` run `tools/gen_icons.py`; output must be reproducible (CI diffs it).

## Releases

* `git tag vX.Y.Z && git push origin vX.Y.Z` → CI builds with `-s firmware_version vX.Y.Z`, uploads `panel-s3.{ota,factory}.bin` + `panel-s3.manifest.json`; the panel's `Firmware` update entity (checks every 6 h) offers it in HA. Dash tags = pre-release (not offered).
* ESPHome version is pinned to **2026.9.1** in both workflows; keep it equal to the local one. When upgrading, diff `components/gt911` against upstream.
* Commit/push/tag only when the user asks.
