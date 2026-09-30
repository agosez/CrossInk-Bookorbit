#include "PowerButtonWakeWatcher.h"

#include <HalGPIO.h>
#include <Logging.h>

#ifndef SIMULATOR
namespace {
// Only a GPIO read and a delay run on this stack; nothing logs from it.
constexpr uint32_t STACK_BYTES = 2048;
// Above the loop task's priority 1: sampling has to go on while the main task
// computes, and a TLS handshake keeps the C3's single core busy for a second or more.
constexpr UBaseType_t TASK_PRIORITY = 2;
constexpr uint32_t SAMPLE_PERIOD_MS = 10;
}  // namespace
#endif

bool PowerButtonWakeWatcher::start(const unsigned long holdMs, const bool heldAtStart) {
  stop();
  detector = SleepWakePolicy::WakePressDetector(holdMs, heldAtStart);
  requested.store(false);
#ifdef SIMULATOR
  return true;
#else
  // A heap stack rather than a static one: the task only exists for the sleep sync, and a
  // static buffer would keep 2 KB of the C3's DRAM reserved through every other boot.
  if (xTaskCreate(&taskEntry, "PowerWakeWatch", STACK_BYTES, this, TASK_PRIORITY, &taskHandle) != pdPASS) {
    taskHandle = nullptr;
    LOG_ERR("PWR", "Could not start the power-button watcher (free=%u maxAlloc=%u)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }
  return true;
#endif
}

void PowerButtonWakeWatcher::stop() {
  if (taskHandle == nullptr) return;
  // The task never returns on its own, so its handle stays valid until this delete.
  vTaskDelete(taskHandle);
  taskHandle = nullptr;
}

bool PowerButtonWakeWatcher::wakeRequested() {
#ifdef SIMULATOR
  // The simulator has no raw pin a task could sample. Its network calls return quickly,
  // so polling the key state whenever the sync asks is enough there.
  gpio.update();
  const bool pressed = gpio.isPressed(HalGPIO::BTN_POWER) || gpio.wasPressed(HalGPIO::BTN_POWER);
  if (detector.update(pressed, millis())) requested.store(true);
#endif
  return requested.load();
}

#ifndef SIMULATOR
void PowerButtonWakeWatcher::taskEntry(void* context) {
  auto* self = static_cast<PowerButtonWakeWatcher*>(context);
  while (true) {
    if (self->detector.update(gpio.isPowerButtonPhysicallyPressed(), millis())) {
      self->requested.store(true);
    }
    vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
  }
}
#endif
