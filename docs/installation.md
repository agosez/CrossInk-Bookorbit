---
title: Installation
nav_order: 2
---

# Installation

## Supported Devices

- Xteink X3, X4
- Xteink X4 Pro
- Xteink X4 Classic
- Seeed Studio Sticky
- BOOX Picco (USB installation only, see [BOOX Picco](#boox-picco))

## Firmware Files

Every path below starts from a `firmware-*.bin` downloaded from **this fork's**
[releases page](https://github.com/agosez/CrossInk-Bookorbit/releases) — one file per
device model. CrossInk's own web installer and update feeds only carry upstream builds,
which have no BookOrbit support. Once this fork is installed,
`Settings > System > Check for Updates` follows this fork's releases, so later versions
arrive over the air. Turn on `Settings > System > Include Release Candidates` to be
offered the test builds published ahead of each release as well.

## Web Installation via USB

#### For new installs and updates.

1. Download the `firmware-*.bin` for your device from the releases page.
2. Open the [CrossPoint flash tools](https://crosspointreader.com/#flash-tools), select
   your device model and choose the custom-firmware option.
3. Give it the downloaded `firmware-*.bin` and start the flash, keeping the reader
   connected through the download-mode and flashing steps.

## BOOX Picco

#### For new installs and every update.

Picco keeps its factory recovery software next to a single application slot, so it
has no Wi-Fi or SD card firmware update: each version is installed over USB, and
only into that slot.

1. Download `firmware-picco-v<version>.bin` from the releases page.
2. Connect Picco over USB. From the stock firmware, hold Power and Volume Up (+)
   together for more than 10 seconds to enter serial flash mode; a Picco already
   running CrossInk usually does not need this.
3. Open the [CrossPoint flash tools](https://crosspointreader.com/#flash-tools),
   select **BOOX Picco**, choose the custom-firmware option and give it the
   downloaded file. For Picco it reads the partition table and writes the
   application slot only, leaving the bootloader, partition table and recovery
   software untouched.
4. Press and hold the power button to restart Picco.

Never erase the flash, write at address `0x0`, or use a firmware made for another
model: either would remove the recovery software.

#### With esptool instead

From a computer (macOS or Linux; see [Command Line](#command-line) to install
`esptool` and find the port), after steps 1 and 2:

1. Check that the reader has the stock layout before writing anything:

   ```sh
   esptool.py --chip esp32s3 --port /dev/cu.usbmodem2101 read_flash 0x8000 0xc00 picco-partitions.bin
   python3 -c "import struct; d = open('picco-partitions.bin', 'rb').read(); [print(e[12:28].rstrip(b'\0').decode(), hex(struct.unpack('<I', e[4:8])[0])) for e in (d[i:i + 32] for i in range(0, len(d), 32)) if e[:2] == b'\xaa\x50']"
   ```

   The list must include `safeboot 0x20000` and `app 0xe0000`. If it does not, stop:
   the address below would overwrite something else.
2. Write the firmware into the `app` slot only, leaving the bootloader, settings and
   `safeboot` recovery untouched:

   ```sh
   esptool.py --chip esp32s3 --port /dev/cu.usbmodem2101 --baud 921600 write_flash 0xe0000 /path/to/firmware-picco-v<version>.bin
   ```

3. Press and hold the power button to restart Picco.

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
books and reading progress live on the SD card and survive the swap in both directions.
Highlights and bookmarks stay stored too, but upstream does not read the content-keyed
files this fork keeps them in, so they are invisible there — everything returns when the
fork is flashed back.

## Command Line

These instructions are for macOS and Linux, and for the ESP32-C3 devices (X3 and X4);
for Picco, follow [BOOX Picco](#boox-picco) above; for the other devices, use the web
installer.

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
