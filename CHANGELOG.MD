# Changelog

[English](./CHANGELOG.MD) | [中文](./CHANGELOG_CN.MD)

## Unreleased

- Fixed Wi-Fi worker threads redrawing the status bar off the UI thread,
  which could crash the launcher in LVGL, for example when Wi-Fi was turned
  off. The main loop now does the redraw.
- Fixed the Wi-Fi DHCP lease never being renewed. udhcpc exited right after
  getting the lease, so once it expired the router could give the address to
  another device. udhcpc now stays running and renews it.
- Fixed Wi-Fi sometimes not connecting at boot until it was toggled by hand.
  Autoconnect made one attempt at launcher start, and if it failed (no
  `wlan0` yet, no association within 20 s, or no DHCP answer), nothing
  retried. While Wi-Fi is on and the link is down, the launcher now retries
  with backoff, cycling through saved networks. Attempts are kept in
  `/tmp/k230_wifi_connect.log` instead of being overwritten.
- Fixed Wi-Fi associating but getting no address when the access point was
  slow to accept it. The connect script gave up after 20 s, which an access
  point that refuses the first attempt can exceed, and then asked for no
  lease until the next retry. It now waits up to 45 s for the access point
  and keeps asking for a lease in the background until one comes.
- Turned Wi-Fi power save off after connecting. The RTL8189FS driver
  starts in its deepest power save, where pings to the device averaged
  1.4 s against 0.18 s with it off. Set `wifi.power_save=1` to keep it on.
  wpa_supplicant now also logs to syslog (`/tmp/messages`).
- Fixed the Wi-Fi address changing on every reconnect. udhcpc now asks for
  the last address it held (kept in `wifi_last_ip` next to
  `settings.conf`) and sends the hostname.
- Fixed Wi-Fi staying on a weak access point when a much stronger one with
  the same SSID was in range. wpa_supplicant won't roam while the signal
  is above about -64 dBm, which on the RTL8189FS still meant up to 30%
  packet loss. While the signal is below -55 dBm, the launcher now scans
  every 3 minutes and moves to an access point at least 8 dB stronger,
  then waits 10 minutes before moving again. It moves by pinning
  wpa_supplicant to that access point for up to 20 s, because mesh access
  points can refuse the first attempt. Moves are logged to
  `/tmp/k230_wifi_connect.log`.

## v0.2.9 - 2026-11-02

- Improved nRF9151 LTE/GNSS coexistence by prioritizing LTE registration,
  retrying automatic operator search, and scheduling background GNSS polling
  without holding the GNSS UART continuously.
- Improved modem status reporting with registration reasons, signal validity,
  request throttling, cached background results, and stable complete snapshots
  during LTE refreshes.
- Improved GNSS startup recovery and cold-start handling after modem power
  cycles, including LTE-before-GNSS scheduling.
- Fixed audio output switching while playback is active. The launcher now
  releases the PCM stream, applies the internal or external route immediately,
  and persists the preference only after the route change succeeds.
- Added startup LoRa hardware detection and moved the probe to the background
  so the launcher can reach the home screen without waiting for the radio probe.
- LoRa, FLRC, LoRaWAN, and Meshtastic apps, together with the Meshtastic
  autostart setting, are hidden when no supported LoRa hardware is detected.
- Fixed a stability issue where the background LoRa probe could update LVGL
  objects from its worker thread and crash the UI while opening Settings.

## v0.2.8 - 2026-10-20

- Updated the K230 Linux SDK submodule to upstream `dev` commit
  `fc6cc2151d54cb307be47f81815023f7ffb0eae6` and adapted the BSP image layout
  while preserving the T-Display boot partition offsets.
- Aligned the RM69A10 U-Boot DSI D-PHY timing with the Linux 594 Mbps lane
  configuration so the boot logo and Linux display use consistent timing.
- Added K230 VPU hardware decoding and scaling for H.264 and HEVC video to
  improve playback smoothness and reduce CPU load.
- Added automatic software-decoder fallback for video streams unsupported by
  the hardware decoder. Set `video.hw_decode=0` in `settings.conf` to force
  software decoding.

## v0.2.7 - 2026-10-08

- Fixed the RM69A10 display intermittently shifting the whole image a few
  pixels sideways after a warm boot. The DSI D-PHY frequency range is now
  derived from the 594 Mbps lane rate instead of a fixed value meant for
  about 450 Mbps.

## v0.2.6 - 2026-10-07

- Added NES mapper 163 support and improved unsupported/invalid ROM handling
  before launch.
- Updated LR2021 LILYGO RF switch handling for DIO6/DIO7/DIO8/DIO10 routing.
- Limited LR2021 LILYGO 2.4 GHz HF profile power to 4 dBm and documented
  LR2021 module RF-switch differences.

## v0.2.5 - 2026-09-29

- Added optional DRV2605 haptics support and related settings entries.
- Added camera focus support for DW9714-based modules, including tap focus and
  autofocus stability improvements.
- Added QR code scanning through the camera app flow.
- Improved camera startup reliability and focus settling behavior.
- Improved music and NES list navigation, long-title handling, and portrait
  layout bounds.
- Added invalid NES ROM protection to avoid crashes when a ROM file is empty or
  incomplete.
- Improved MTP storage-space reporting and write-error handling so failed file
  copies are easier to diagnose.

## v0.2.4 - 2026-09-03

- Added PicoClaw chat app with model profiles, API-key storage, SSH profile
  provisioning, and Weixin QR login.
- Added on-device Meshtastic map tile download with built-in tile sources,
  custom source persistence, network checks, and download confirmation.
- Added storage expansion settings for SD cards larger than the flashed image.
- Improved I2C Scan with asynchronous scanning across board and extension buses.
- Improved LoRa tools with radio model persistence and cleaner RF test profile
  controls.
- Updated default hardware F-key actions: F3/F4 keyboard backlight down/up,
  F7 screenshot.
- Refined portrait and landscape layouts for MTP, Motion, LoRa, PicoClaw, and
  related settings pages.

## v0.2.3

- Refined MTP landscape layout bounds.
- Improved Motion app layout in landscape mode.
- Set the default low-battery shutdown threshold.

## v0.2.2

- Added Xiaozhi voice assistant with wake-word entry, push-to-talk, robot face
  mode, and automatic session recovery.
- Improved nRF52840 ownership coordination between Meshtastic BLE and UART DFU.
- Improved first-run charger, battery, and low-voltage shutdown defaults.
- Improved repeatable SD-card image generation.
