---
title: Installation
nav_order: 2
---

# Installation

## Supported Devices

- Xteink X3, X4
- Xteink X4 Pro
- Seeed Studio Sticky

## Firmware Files

Every path below starts from a `firmware-*.bin` downloaded from **this fork's**
[releases page](https://github.com/agosez/CrossInk-Bookorbit/releases) — one file per
device model. CrossInk's own web installer and update feeds only carry upstream builds,
which have no BookOrbit support. Once this fork is installed,
`Settings > System > Check for Updates` follows this fork's releases, so later versions
arrive over the air.

## Web Installation via USB

#### For new installs and updates.

1. Download the `firmware-*.bin` for your device from the releases page.
2. Open the [CrossPoint flash tools](https://crosspointreader.com/#flash-tools), select
   your device model and choose the custom-firmware option.
3. Give it the downloaded `firmware-*.bin` and start the flash, keeping the reader
   connected through the download-mode and flashing steps.

X4 Pro uses the ESP32-S3 firmware option. Keep the reader connected during the
download-mode and flashing steps shown by Inky.

## USB Drive

On X4 Pro, choose `Home > File Transfer > USB Drive` to expose the SD card to
your computer. Eject the drive from the computer before disconnecting it; the
reader restarts to Home when the drive is safely ejected or the cable is
removed.

## SD Card Firmware Update

#### For a device that already runs CrossInk (upstream or this fork). Works on USB-locked devices.

1. Download the `firmware-*.bin` for your device from the releases page.
2. Place it anywhere on the SD card.
3. Go to `Settings > System > SD Card Firmware Update`, navigate to the `.bin` file and
   update.

## Reverting to upstream CrossInk

Flash an upstream build with CrossInk's own
[web installer](https://inky.crossink.dev/#flash-tools), or place an upstream
`firmware-*.bin` on the SD card and use the SD Card Firmware Update above. Settings,
books, reading progress, highlights and bookmarks live on the SD card and survive the
swap in both directions.

## Command Line

These instructions are for macOS and Linux, and for the ESP32-C3 devices (X3 and X4);
for the other devices, use the web installer.

Install `esptool`:

```sh
pip3 install esptool
```

Download the `firmware-*.bin` file from the
[releases page](https://github.com/agosez/CrossInk-Bookorbit/releases), then connect
your device with USB-C.

Find the device port:

```sh
# Linux
dmesg | grep tty

# macOS
ls /dev/cu.*
```

Flash the firmware:

```sh
# Linux
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin

# macOS
esptool.py --chip esp32c3 --port /dev/cu.usbmodem2101 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

Replace the port and firmware path with your actual values.
