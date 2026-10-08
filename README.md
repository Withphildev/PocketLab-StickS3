# PocketLab for M5StickS3

> **Public beta — v0.16.0.** PocketLab is ready for real-world testing, but additional reliability, compatibility, and usability updates are expected.

PocketLab is a small, understandable ESP32-S3 multi-tool built around safe diagnostics and local device management.

It runs entirely on an M5StickS3, creates its own private Wi-Fi access point, and serves an offline WebUI at `http://pocketlab`. No cloud account, telemetry service, or internet connection is required.

## Supported hardware

- M5Stack StickS3
- A phone, tablet, or computer with Wi-Fi and a modern browser
- Optional BLE HID keyboard for keyboard mode

PocketLab has not been validated on the M5StickC, M5StickC Plus, M5StickC Plus2, Cardputer, or other ESP32 boards. Do not flash the factory image to a different device.

## Install with M5Burner

1. Select the **M5StickS3** device.
2. Choose PocketLab `v0.16.0`.
3. Connect the StickS3 over USB.
4. Erase and burn the complete factory image at offset `0x0000`.
5. Restart the StickS3.

Installing the factory image erases existing firmware, PocketLab files, saved baselines, and settings on the device.

## First start

1. Read the Wi-Fi name and password on the StickS3 screen, or press the blue Face button to show the join QR code.
2. Join the displayed PocketLab Wi-Fi network.
3. Open `http://pocketlab` if the dashboard does not appear automatically.
4. Use the M5 button to stop the portal. Use the blue Face button to start it again.

## Safety and privacy

Use PocketLab only with equipment and networks you own or have clear permission to examine.

- Cyber Academy does not crack passwords, disconnect clients, impersonate networks, or attack access points.
- Academy signal history and checklist progress stay in browser memory and clear when the page is refreshed or closed.
- Practice-password examples are evaluated locally in the browser and are never transmitted to PocketLab. Use a made-up example, not a real password.
- The main Wi-Fi and BLE survey tools save a report only when the user explicitly presses a save button.
- PocketLab is an offline learning and diagnostic tool, not a substitute for a professional security assessment.

## Current features

- Creates a private Wi-Fi access point with a unique SSID and password.
- Opens with an embedded PocketLab splash screen and a firmware-version overlay.
- Includes Nova as an offline WebUI assistant with idle, scanning, success, and warning states tied to dashboard activity.
- Adds responsive Nova illustrations to the Device, Wi-Fi Survey, BLE Inventory, Bluetooth Keyboard, and File Vault sections.
- Adds a separate, all-ages Cyber Academy view with clear permission and home-use guidance.
- Guides learners through a six-step, session-only home-network checklist with reversible completion buttons, progress display, and a completed-item filter.
- Includes a no-logging Wi-Fi signal explorer for locating weak coverage and dead spots, with a four-level meter and a temporary 20-reading browser chart.
- Shows the selected Wi-Fi name, 2.4 GHz band, and channel prominently above the signal explorer.
- Smooths signal strength with a rolling five-reading median while retaining latest, strongest, and weakest room measurements.
- Reduces live scan frequency to nine seconds, stops walk tests after three minutes, and pauses them whenever the browser tab is hidden to conserve battery.
- Resets temporary readings with one button when moving to a new room.
- Provides a browser-only password-strength lesson using made-up examples; the typed example is never transmitted to PocketLab or saved.
- Explains Wi-Fi security modes, router placement, updates, and the limits of WPS detection without claiming to test or crack a password.
- Includes a separate expandable glossary with plain-language networking definitions and expand-all/collapse-all controls.
- Shows a standard Wi-Fi join QR code when the blue Face button is pressed on the portal screen; press it again to return to the connection details.
- Hosts a local browser dashboard at the friendly address `http://pocketlab`.
- Shows heap, PSRAM, and LittleFS storage status.
- Performs Wi-Fi surveys without joining the discovered networks.
- Runs an on-demand 10-second BLE advertisement inventory, then completely shuts down Bluetooth to conserve memory and power.
- Shows BLE names, addresses and address types, RSSI, connectability, advertised services, manufacturer data, and observation timing/counts.
- Saves BLE inventories as CSV and JSON, supports sortable WebUI results, and flags addresses first seen after a saved BLE baseline.
- Warns that randomized BLE addresses can change and should not be treated as proof of a new physical device.
- Names and saves survey sessions as both CSV and JSON.
- Records SSID, BSSID, channel, security, RSSI, and browser-provided timestamps.
- Creates a trusted baseline and flags BSSIDs first seen after that baseline.
- Sorts results by signal strength, channel, or network name.
- Displays a 2.4 GHz channel-usage chart.
- Lists, downloads, and deletes survey reports stored in LittleFS.
- Includes a WebUI file vault for uploading, downloading, and deleting small field files in internal flash.
- Limits individual uploads to 1 MB, prevents accidental overwrites, sanitizes filenames, and preserves a 64 KB filesystem safety reserve.
- Separates uploaded vault files from generated reports and shows total internal-flash usage in the device dashboard.
- Provides a dedicated BLE HID keyboard mode that shuts down Wi-Fi first to preserve RAM.
- Scans for standard HID keyboards, displays the six-digit pairing passkey on the StickS3, bonds securely, and subscribes to keyboard input reports.
- Includes an on-device key tester for text, arrows, Enter, Backspace, and Escape; the M5 button or Escape returns to the Wi-Fi portal.
- Accepts both seven-byte and eight-byte keyboard arrays, restores HID Report Protocol after pairing, and shows a live raw-report counter for troubleshooting device-specific layouts.
- Retries HID notification subscriptions with both acknowledged and unacknowledged CCCD writes, reports the active subscription count on screen, and explicitly sends Exit Suspend to the keyboard.
- Coalesces rapid HID reports into 40 ms display updates and preserves redraw requests that arrive while the LCD is being painted, preventing received keys from appearing one press late.
- Prefers the standardized HID Boot Keyboard Input report over Logitech-specific Report Protocol data, falls back automatically when Boot Protocol is unavailable, and latches the last non-zero raw packet for diagnosis.
- Shows a filtered battery estimate, voltage, charging state, minimum observed voltage, and low-battery warnings on the StickS3 and WebUI.
- Uses median and moving-average filtering plus charge-aware hysteresis to prevent Wi-Fi load spikes from making the percentage jump.
- Runs at reduced display brightness, automatically dims after 30 seconds, disables unused external power, and allows the screen to be turned off from the WebUI while the portal remains active.
- Reports the last ESP32 reset reason in the WebUI to distinguish normal restarts, watchdog resets, crashes, and brownouts.
- Provides Eco and Performance profiles. Eco uses AP-only mode while idle, lower Wi-Fi transmit power, and a 160 MHz CPU; the station radio is enabled only for surveys.
- Automatically stops an unattended portal after a configurable no-client timeout.
- Includes a low-power sleep control that turns off Wi-Fi and the screen; the blue Face button wakes the device.
- Shows an animated charging battery, connected-device count, CPU speed, and voltage trend in the WebUI.
- Uses DNS redirection so common captive-portal checks arrive at the dashboard.

The generated network name and password are displayed on the StickS3 in large text. The M5 button stops the portal; the blue Face button starts it again.

## Build

```sh
platformio run
```

## Planned modules

1. Turn the BLE keyboard provider into a navigable on-device tool menu.
2. Add IR learning and remote profiles for equipment you own.
3. Add more passive, permission-first Cyber Academy lessons.

PocketLab is intended for devices and networks you own or have permission to test.

## License

PocketLab is released under the [MIT License](LICENSE).
