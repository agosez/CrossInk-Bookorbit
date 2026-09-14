#include "HalSystem.h"

#include <string>

#include "AppVersion.h"
#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include "esp_debug_helpers.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"

#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include "esp_cpu_utils.h"
#include "esp_memory_utils.h"
#include "xtensa/corebits.h"
#endif

#define MAX_PANIC_STACK_DEPTH 32
#define MAX_PANIC_BACKTRACE_DEPTH 32
#define PANIC_CAPTURE_MAGIC 0x50414E49u

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];
RTC_NOINIT_ATTR uint32_t panicBacktrace[MAX_PANIC_BACKTRACE_DEPTH];
RTC_NOINIT_ATTR volatile size_t panicBacktraceDepth;

#if CONFIG_IDF_TARGET_ARCH_RISCV
// Preserve the exception-frame values that identify a C3 panic. Unlike the raw
// stack words below, MEPC is the actual faulting instruction and MTVAL is the
// fault address/value reported by the CPU.
struct RiscvPanicRegisters {
  uint32_t mepc;
  uint32_t ra;
  uint32_t sp;
  uint32_t fp;
  uint32_t mcause;
  uint32_t mtval;
  uint32_t mstatus;
  uint32_t captured;
};
RTC_NOINIT_ATTR RiscvPanicRegisters panicRiscvRegisters;
#endif
#if CONFIG_IDF_TARGET_ARCH_XTENSA
// Preserve the X4 Pro/Sticky exception details alongside their existing
// symbolizable backtrace. EXCVADDR identifies the bad address for memory
// access faults, which the backtrace alone cannot show.
struct XtensaPanicRegisters {
  uint32_t pc;
  uint32_t a0;
  uint32_t a1;
  uint32_t ps;
  uint32_t exccause;
  uint32_t excvaddr;
  uint32_t captured;
};
RTC_NOINIT_ATTR XtensaPanicRegisters panicXtensaRegisters;
#endif
// RTC_NOINIT is uninitialized on cold boot, so only this exact marker proves a
// panic diagnostic was captured before the reset.
RTC_NOINIT_ATTR volatile uint32_t panicCaptureMarker;

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";

#if CONFIG_IDF_TARGET_ARCH_XTENSA
void IRAM_ATTR captureXtensaPanicBacktrace(const void* frame) {
  const auto* exceptionFrame = static_cast<const XtExcFrame*>(frame);
  panicXtensaRegisters.pc = exceptionFrame->pc;
  panicXtensaRegisters.a0 = exceptionFrame->a0;
  panicXtensaRegisters.a1 = exceptionFrame->a1;
  panicXtensaRegisters.ps = exceptionFrame->ps;
  panicXtensaRegisters.exccause = exceptionFrame->exccause;
  panicXtensaRegisters.excvaddr = exceptionFrame->excvaddr;
  panicXtensaRegisters.captured = PANIC_CAPTURE_MAGIC;

  esp_backtrace_frame_t backtraceFrame = {
      .pc = static_cast<uint32_t>(exceptionFrame->pc),
      .sp = static_cast<uint32_t>(exceptionFrame->a1),
      .next_pc = static_cast<uint32_t>(exceptionFrame->a0),
      .exc_frame = exceptionFrame,
  };

  size_t depth = 0;
  uint32_t pc = esp_cpu_process_stack_pc(backtraceFrame.pc);
  panicBacktrace[depth++] = pc;

  bool corrupted =
      !esp_stack_ptr_is_sane(backtraceFrame.sp) ||
      (!esp_ptr_executable(reinterpret_cast<const void*>(pc)) && exceptionFrame->exccause != EXCCAUSE_INSTR_PROHIBITED);
  while (depth < MAX_PANIC_BACKTRACE_DEPTH && backtraceFrame.next_pc != 0 && !corrupted) {
    if (!esp_backtrace_get_next_frame(&backtraceFrame)) {
      break;
    }

    pc = esp_cpu_process_stack_pc(backtraceFrame.pc);
    if (esp_ptr_executable(reinterpret_cast<const void*>(pc))) {
      panicBacktrace[depth++] = pc;
    }
  }

  panicBacktraceDepth = depth;
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;
}
#endif

#if CONFIG_IDF_TARGET_ARCH_RISCV
void IRAM_ATTR captureRiscvPanicRegisters(const void* frame) {
  const auto* exceptionFrame = static_cast<const esp_cpu_frame_t*>(frame);
  panicRiscvRegisters.mepc = exceptionFrame->mepc;
  panicRiscvRegisters.ra = exceptionFrame->ra;
  panicRiscvRegisters.sp = exceptionFrame->sp;
  panicRiscvRegisters.fp = exceptionFrame->s0;
  panicRiscvRegisters.mcause = exceptionFrame->mcause;
  panicRiscvRegisters.mtval = exceptionFrame->mtval;
  panicRiscvRegisters.mstatus = exceptionFrame->mstatus;
  panicRiscvRegisters.captured = PANIC_CAPTURE_MAGIC;
}
#endif

void IRAM_ATTR __wrap_panic_abort(const char* message) {
  if (!message) message = PANIC_REASON_UNKNOWN;
  // IRAM-safe bounded copy (strncpy is not IRAM-safe in panic context)
  int i = 0;
  for (; i < (int)sizeof(panicMessage) - 1 && message[i]; i++) {
    panicMessage[i] = message[i];
  }
  panicMessage[i] = '\0';
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;

  __real_panic_abort(message);
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }

#if CONFIG_IDF_TARGET_ARCH_XTENSA
  captureXtensaPanicBacktrace(frame);
  __real_panic_print_backtrace(frame, core);
  return;
#elif !__riscv
  __real_panic_print_backtrace(frame, core);
  return;
#else
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  panicBacktraceDepth = 0;

  captureRiscvPanicRegisters(frame);

  // Copied from components/esp_system/port/arch/riscv/panic_arch.c
  uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < 1024; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    // panic_print_hex(sp + x);
    // panic_print_str(": ");
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      // panic_print_str("0x");
      // panic_print_hex(spp[y]);
      // panic_print_str(y == per_line - 1 ? "\r\n" : " ");
      panicStack[depth].spp[y] = spp[y];
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;

  __real_panic_print_backtrace(frame, core);
#endif
}
}

namespace HalSystem {

void begin() {
  // On a panic reboot, preserve diagnostics until checkPanic() has tried to write them to the SD card.
  // Ordinary boots clear any stale retained diagnostics.
  if (!isRebootFromPanic()) {
    clearPanic();
  } else {
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
  if (isRebootFromPanic()) {
    auto panicInfo = getPanicInfo(true);
    auto file = Storage.open("/crash_report.txt", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
      const size_t written = file.write(panicInfo.c_str(), panicInfo.size());
      file.close();
      if (written == panicInfo.size()) {
        // Keep the crash data for CrashActivity, but mark it consumed so a
        // later watchdog reset cannot be mistaken for this panic.
        panicCaptureMarker = 0;
        LOG_INF("SYS", "Dumped panic info to SD card");
      } else {
        LOG_ERR("SYS", "Failed to write complete crash report (%zu of %zu bytes)", written, panicInfo.size());
      }
    } else {
      LOG_ERR("SYS", "Failed to open crash_report.txt for writing");
    }
  }
}

void clearPanic() {
  panicCaptureMarker = 0;
  panicMessage[0] = '\0';
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  panicBacktraceDepth = 0;
#if CONFIG_IDF_TARGET_ARCH_RISCV
  panicRiscvRegisters.captured = 0;
#endif
#if CONFIG_IDF_TARGET_ARCH_XTENSA
  panicXtensaRegisters.captured = 0;
#endif
  clearLastLogs();
}

std::string getPanicInfo(bool full) {
  if (!full) {
    return panicMessage;
  } else {
    std::string info;

    info += "CrossInk version: " CROSSINK_VERSION;
    info += "\nCrossInk device type: " CROSSINK_FIRMWARE_DEVICE_TYPE;
    info += "\n\nPanic reason: " + std::string(panicMessage);
    info += "\n\nLast logs:\n" + getLastLogs();
    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08X", value);
      return std::string(buffer);
    };
#if CONFIG_IDF_TARGET_ARCH_RISCV
    if (panicRiscvRegisters.captured == PANIC_CAPTURE_MAGIC) {
      info += "\n\nRISC-V exception registers:\n";
      info += "MEPC (faulting instruction): 0x" + toHex(panicRiscvRegisters.mepc);
      info += "\nRA (caller): 0x" + toHex(panicRiscvRegisters.ra);
      info += "\nSP (stack pointer): 0x" + toHex(panicRiscvRegisters.sp);
      info += "\nS0/FP (frame pointer): 0x" + toHex(panicRiscvRegisters.fp);
      info += "\nMCAUSE: 0x" + toHex(panicRiscvRegisters.mcause);
      info += "\nMTVAL (fault address/value): 0x" + toHex(panicRiscvRegisters.mtval);
      info += "\nMSTATUS: 0x" + toHex(panicRiscvRegisters.mstatus);
    }
#endif
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    if (panicXtensaRegisters.captured == PANIC_CAPTURE_MAGIC) {
      info += "\n\nXtensa exception registers:\n";
      info += "PC (faulting instruction): 0x" + toHex(panicXtensaRegisters.pc);
      info += "A0 (return address): 0x" + toHex(panicXtensaRegisters.a0);
      info += "A1 (stack pointer): 0x" + toHex(panicXtensaRegisters.a1);
      info += "PS (processor state): 0x" + toHex(panicXtensaRegisters.ps);
      info += "EXCCAUSE: 0x" + toHex(panicXtensaRegisters.exccause);
      info += "\nEXCVADDR (fault address): 0x" + toHex(panicXtensaRegisters.excvaddr);
    }
#endif
    if (panicStack[0].sp != 0) {
      info += "\n\nStack memory:\n";
      for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
        if (panicStack[i].sp == 0) {
          break;
        }
        info += "0x" + toHex(panicStack[i].sp) + ": ";
        for (size_t j = 0; j < 8; j++) {
          info += "0x" + toHex(panicStack[i].spp[j]) + " ";
        }
        info += "\n";
      }
    }

    const size_t backtraceDepth = panicBacktraceDepth <= MAX_PANIC_BACKTRACE_DEPTH ? panicBacktraceDepth : 0;
    if (backtraceDepth > 0) {
      info += "\nStack trace:\n";
      for (size_t i = 0; i < backtraceDepth; i++) {
        info += "0x" + toHex(panicBacktrace[i]) + "\n";
      }
    }

    return info;
  }
}

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  if (resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP) {
    return true;
  }

  const bool watchdogReset =
      resetReason == ESP_RST_INT_WDT || resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT;
  return watchdogReset && panicCaptureMarker == PANIC_CAPTURE_MAGIC;
}

}  // namespace HalSystem
