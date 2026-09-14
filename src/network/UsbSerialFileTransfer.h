#pragma once

namespace UsbSerialFileTransfer {

enum class ProcessResult {
  None,
  ScreenshotRequested,
};

ProcessResult process(bool fileTransferAllowed);

// Register the native USB CDC overflow callback when the X4 Pro transport
// supports it. Other transports intentionally do nothing.
void registerUsbCdcOverflowHandler();

}  // namespace UsbSerialFileTransfer
