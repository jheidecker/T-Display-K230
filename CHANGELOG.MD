# Changelog

[English](./CHANGELOG.MD) | [中文](./CHANGELOG_CN.MD)

## Unreleased

- Fixed nRF9151 GNSS not getting a first fix while LTE stayed registered
  without power saving. GNSS now pauses LTE until the first fix or for up to
  5 minutes, then brings LTE back, and pauses it again after 30 minutes
  without a fix. Until the first fix, GNSS keeps the modem UART and streams
  NMEA even for background requests, so the fix is seen; background polling
  takes over after it. Set `nrf9151.gnss.lte_pause=0` in settings.conf to
  keep LTE on.
- Fixed the Cellular app's LTE status going stale while GNSS held the modem
  UART. The GNSS thread now refreshes it with NMEA output paused.
- Added nRF Cloud A-GNSS for the nRF9151. When LTE is registered and the
  modem is onboarded to nRF Cloud, GNSS connects to nRF Cloud and downloads
  satellite data before pausing LTE, which gives a fix in seconds instead of
  minutes and works with weaker signals. Without LTE or nRF Cloud, GNSS
  starts as before. Set `nrf9151.gnss.agnss=0` in settings.conf to turn it
  off. The modem needs Serial Modem firmware with the nRF Cloud provisioning
  client. `nrf9151.gnss.cloud_assistance` alone doesn't give assistance: the
  modem refuses it until nRF Cloud is connected.
- Fixed the Meshtastic ACK setting switching itself off for channel
  messages. With ACK on, a channel message now waits for a neighbor to relay
  it and shows "Relayed", as in stock Meshtastic, and is re-sent up to 3 times
  if no relay is heard. Channel messages from the phone app work the same way
  when the app asks for ACK. Position, telemetry and other broadcasts never
  ask for ACK.
- Fixed Meshtastic settings such as ACK, Rebroadcast, hop limit and region
  reverting to their old values when changed while Meshtastic was running.
  The daemon now restarts with the new settings.
- Added a start/stop page for the official Meshtastic firmware. When
  `meshtasticd` is installed, the Meshtastic app and hotkey open a page that
  starts and stops it, shows its log, and shows the pairing PIN when a phone
  pairs over Bluetooth. A new `k230_meshtastic_ble_bridge` serves meshtasticd
  to the Meshtastic phone app through the nRF52840's Meshtastic BLE service.
  `/etc/init.d/S90meshtasticd` starts both at boot; set
  `meshtasticd.autostart=0` in settings.conf to keep it stopped. Without
  `meshtasticd` the launcher's own Meshtastic app works as before. The
  startup LoRa probe is skipped while meshtasticd runs or starts at boot, so
  it doesn't reset the radio under it. meshtasticd reads the system clock only
  at startup, and at boot that's still 1970, so the launcher restarts it once
  the clock is set, and again if the clock jumps by more than a minute.
- Fixed battery capacity calibration not reaching the BQ27220 gauge, which
  kept reporting its 3000 mAh default. The launcher now writes the gauge in
  the sequence it accepts, reads the value back before reporting success, and
  re-applies the saved capacity at startup when the gauge has lost it.
- Fixed the clock staying at 1970, or taking minutes to set, after boot. The
  K230 has no RTC, and ntpd starts before Wi-Fi is up. The launcher now sets
  the clock from the first source that knows the time: NTP as soon as Wi-Fi
  has a route, LTE network time from the nRF9151 (`AT+CCLK?`), or a GNSS fix.
  Later readings only correct it. Changes are logged to
  `/tmp/k230_clock_sync.log`.
- Fixed Wi-Fi dropping off the network after its DHCP lease ran out. udhcpc
  exited right after getting the lease, so nothing renewed it; it now stays
  running and renews.
- Fixed the Wi-Fi address changing on every reconnect. udhcpc now asks for
  the last address it held (kept in `wifi_last_ip` next to
  `settings.conf`) and sends the hostname.
- Fixed Wi-Fi sometimes not connecting at boot until it was toggled by hand.
  Autoconnect made one attempt at launcher start; if it failed (no `wlan0`
  yet, no association within 20 s, or no DHCP answer), nothing retried. It
  now retries with backoff while Wi-Fi is on and the link is down, cycling
  through saved networks. Attempts are kept in `/tmp/k230_wifi_connect.log`
  instead of being overwritten, and wpa_supplicant logs to syslog
  (`/tmp/messages`).
- Fixed Wi-Fi worker threads redrawing the status bar off the UI thread,
  which could crash the launcher in LVGL (seen when a Wi-Fi retry
  finished). They now leave the redraw to the main loop.
- Fixed slow, lossy Wi-Fi. The RTL8189FS driver starts in its deepest power
  save, where pings to the device averaged 1.4 s against 0.18 s with power
  save off. Power save is now turned off after connecting; set
  `wifi.power_save=1` to keep it on.
- Fixed Wi-Fi choosing a weaker access point over a stronger one with the
  same SSID. The RTL8189FS driver reported 40 MHz support to wpa_supplicant
  even when limited to 20 MHz, so 40 MHz APs were ranked as twice as fast.
  It now reports 40 MHz only when `rtw_bw_mode` allows it, and the driver
  loads with `rtw_bw_mode=0` (20 MHz on 2.4 GHz), so the stronger AP wins.

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
