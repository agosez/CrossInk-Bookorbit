#pragma once

// Releases the ESP32-S3 USB-OTG PHY and reconnects the hardware USB
// Serial/JTAG peripheral before a normal application restart. No-op on targets
// that do not enable the X4 Pro USB Drive capability.
void handoffUsbOtgToSerialJtag();
