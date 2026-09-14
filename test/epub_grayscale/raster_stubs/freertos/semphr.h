#pragma once
#include "FreeRTOS.h"
inline void* xSemaphoreCreateMutex() { return reinterpret_cast<void*>(1); }
inline int xSemaphoreTake(void*, unsigned) { return pdTRUE; }
inline void xSemaphoreGive(void*) {}
inline void* xSemaphoreGetMutexHolder(void*) { return reinterpret_cast<void*>(1); }
