#if defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO && !ARDUINO_USB_MODE && !defined(SIMULATOR)

#include <stdint.h>

#include "class/dfu/dfu_device.h"
#include "class/dfu/dfu_rt_device.h"
#include "class/hid/hid.h"
#include "class/net/net_device.h"

// The Arduino S3 framework ships one TinyUSB archive containing optional
// classes even when the X4 Pro descriptor exposes only CDC and MSC. Keep the
// archive's optional callback references weak and inert when those classes are
// disabled in the composite device.
extern "C" {

__attribute__((weak)) uint8_t const* tud_hid_descriptor_report_cb(uint8_t) { return nullptr; }

__attribute__((weak)) uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t*, uint16_t) {
  return 0;
}

__attribute__((weak)) void tud_hid_set_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t const*, uint16_t) {}

__attribute__((weak)) void tud_dfu_runtime_reboot_to_dfu_cb(void) {}

__attribute__((weak)) uint32_t tud_dfu_get_timeout_cb(uint8_t, uint8_t) { return 0; }

__attribute__((weak)) void tud_dfu_download_cb(uint8_t, uint16_t, uint8_t const*, uint16_t) {}

__attribute__((weak)) void tud_dfu_manifest_cb(uint8_t) {}

__attribute__((weak)) bool tud_network_recv_cb(uint8_t const*, uint16_t) { return false; }
}

#endif
