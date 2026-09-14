#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MemoryBudget.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {

struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};

constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};

enum class FontFileSelection : uint8_t { Closest, Exact };

// This is a cold setup path, not a render loop. The 320-byte stack footprint
// (path and filename) replace a heap-allocated whole-font catalog
// during every dictionary swap, avoiding persistent fragmentation on the C3.
bool findInstalledFontFile(const char* familyName, const uint8_t targetPointSize, const FontFileSelection selection,
                           char* path, const size_t pathSize, uint8_t& selectedPointSize) {
  if (!familyName || familyName[0] == '\0' || !path || pathSize == 0) return false;

  const char* root = SdCardFontRegistry::findFamilyRoot(familyName);
  if (!root) return false;
  const int directoryLength = std::snprintf(path, pathSize, "%s/%s", root, familyName);
  if (directoryLength <= 0 || static_cast<size_t>(directoryLength) >= pathSize) return false;

  HalFile dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) return false;

  uint8_t closestSize = 0;
  uint8_t closestDiff = UINT8_MAX;
  char filename[128] = {};
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (!isDirectory) entry.getName(filename, sizeof(filename));
    entry.close();
    if (isDirectory) continue;

    uint8_t pointSize = 0;
    uint8_t style = 0;
    if (!SdCardFontRegistry::parseFilename(filename, pointSize, style) || style != 0) continue;

    if (selection == FontFileSelection::Closest) {
      const uint8_t diff = pointSize > targetPointSize ? pointSize - targetPointSize : targetPointSize - pointSize;
      if (closestDiff == UINT8_MAX || diff < closestDiff || (diff == closestDiff && pointSize < closestSize)) {
        closestSize = pointSize;
        closestDiff = diff;
      }
    }
  }
  dir.close();

  if (selection == FontFileSelection::Closest) {
    selectedPointSize = closestSize;
  } else if (selection == FontFileSelection::Exact) {
    selectedPointSize = targetPointSize;
  }

  if (selectedPointSize == 0) return false;
  // Scan once more to preserve the exact file name rather than assuming the
  // file base name matches the directory name.
  dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) return false;
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (!isDirectory) entry.getName(filename, sizeof(filename));
    entry.close();
    if (isDirectory) continue;

    uint8_t pointSize = 0;
    uint8_t style = 0;
    if (!SdCardFontRegistry::parseFilename(filename, pointSize, style) || style != 0 ||
        pointSize != selectedPointSize) {
      continue;
    }
    const int pathLength = std::snprintf(path, pathSize, "%s/%s/%s", root, familyName, filename);
    dir.close();
    return pathLength > 0 && static_cast<size_t>(pathLength) < pathSize;
  }
  dir.close();
  return false;
}

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  if (SETTINGS.sdFontFamilyName[0] == '\0') {
    LOG_DBG("SDFS", "SD font resolver ready; discovery deferred until requested");
    return;
  }

  ensureLoaded(renderer);
  releaseRegistry();
}

void SdCardFontSystem::persistSettingsChange() const {
  if (settingsPersistenceCallback_) {
    settingsPersistenceCallback_(settingsPersistenceContext_);
  } else {
    SETTINGS.saveToFile();
  }
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.load(std::memory_order_acquire);

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  uint8_t targetPointSize = SETTINGS.getSdFontTargetPointSize();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
      loadedFontPointSize_ = 0;
    }
    return;
  }

  if (!registryWasDirty && currentFamily == wantedFamily && loadedFontPointSize_ == targetPointSize &&
      SETTINGS.legacySdFontSizeStep == UINT8_MAX) {
    return;
  }

  ensureRegistry();

  const auto* family = registry_.findFamily(wantedFamily);
  if (family && SETTINGS.legacySdFontSizeStep != UINT8_MAX) {
    const auto sizes = family->availableSizes();
    if (!sizes.empty()) {
      const uint8_t step = std::min<uint8_t>(SETTINGS.legacySdFontSizeStep, sizes.size() - 1);
      targetPointSize = sizes[step];
      SETTINGS.readerFontPointSize = targetPointSize;
      SETTINGS.legacySdFontSizeStep = UINT8_MAX;
      persistSettingsChange();
      LOG_INF("SDFS", "Migrated SD font size to %u pt", targetPointSize);
    }
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      persistSettingsChange();
      return;
    }
    const auto* wantedFile = family->findClosestFile(targetPointSize);
    uint8_t wantedPt = wantedFile ? wantedFile->pointSize : 0;
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (target %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            targetPointSize, registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  if (family) {
    if (manager_.loadFamilyClosest(*family, renderer, targetPointSize)) {
      loadedFontPointSize_ = targetPointSize;
      setupUiFallbacks(renderer);
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
      persistSettingsChange();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
    persistSettingsChange();
  }
}

void SdCardFontSystem::releaseLoadedFont(GfxRenderer& renderer) {
  if (manager_.currentFamilyName().empty()) return;

  const std::string familyName = manager_.currentFamilyName();
  (void)familyName;
  manager_.unloadAll(renderer);
  loadedFontPointSize_ = 0;
  LOG_DBG("SDFS", "Released SD card font before low-memory operation: %s", familyName.c_str());
}

void SdCardFontSystem::ensureRegistry() {
  const bool dirty = registryDirty_.exchange(false, std::memory_order_acq_rel);
  if (registryLoaded_ && !dirty) return;
  if (dirty) LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
  registry_.discover();
  if (registry_.lastDiscoveryFailed()) {
    LOG_ERR("SDFS", "SD font registry scan ran out of memory (free=%u maxAlloc=%u)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    registryDirty_.store(true, std::memory_order_release);
    return;
  }
  registryLoaded_ = true;
}

void SdCardFontSystem::releaseRegistry() {
  if (!registryLoaded_) return;
  LOG_DBG("SDFS", "Releasing SD font catalog (%d families)", registry_.getFamilyCount());
  registry_.clear();
  registryLoaded_ = false;
}

void SdCardFontSystem::releaseForNetwork(GfxRenderer& renderer) {
  releaseLoadedFont(renderer);

  releaseRegistry();
  registryDirty_.store(true, std::memory_order_release);
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;

  static constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};
  bool hasCjk = false;
  for (const uint32_t cp : kCjkProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasCjk = true;
      break;
    }
  }
  if (!hasCjk) {
    LOG_DBG("SDFS", "%s has no CJK coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (const auto& ui : kUiFontSizes) {
    const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, ui.pointSize);
    if (sdFontId != 0) {
      renderer.setFallbackFont(ui.fontId, sdFontId);
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
}

void SdCardFontSystem::setupUiFallbacksDirect(GfxRenderer& renderer, const char* familyName) {
  if (!familyName || familyName[0] == '\0') return;

  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(manager_.currentFamilyName()));
  if (readerIt == renderer.getFontMap().end()) return;

  static constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};
  bool hasCjk = false;
  for (const uint32_t cp : kCjkProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasCjk = true;
      break;
    }
  }
  if (!hasCjk) return;

  for (const auto& ui : kUiFontSizes) {
    char path[160] = {};
    uint8_t pointSize = 0;
    if (!findInstalledFontFile(familyName, ui.pointSize, FontFileSelection::Exact, path, sizeof(path), pointSize)) {
      continue;
    }
    const int sdFontId = manager_.loadFamilyExtraFile(path, familyName, pointSize, renderer);
    if (sdFontId != 0) renderer.setFallbackFont(ui.fontId, sdFontId);
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
  // The manager loads exactly one size (closest to the selected point size), so the
  // enum is implicit — always return the single loaded font ID for this family.
  // ensureLoaded() must have been called with the current settings before this.
  return manager_.getFontId(familyName);
}

bool SdCardFontSystem::changeReaderFontSize(const bool larger, const FontSizeStepMode mode) {
  refreshIfDirty();

  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      const auto sizes = family->availableSizes();
      if (changeReaderFontSizeStep(sizes.data(), sizes.size(), SETTINGS.readerFontPointSize, larger, mode)) return true;
      if (sizes.size() > 0) return false;
    }
  }

  return SETTINGS.changeReaderFontSize(larger, mode);
}

uint8_t SdCardFontSystem::resolveLegacySizeStep(const char* familyName, const uint8_t sizeStep) {
  ensureRegistry();
  const auto* family = familyName ? registry_.findFamily(familyName) : nullptr;
  if (family) {
    const auto sizes = family->availableSizes();
    if (!sizes.empty()) return sizes[std::min<uint8_t>(sizeStep, sizes.size() - 1)];
  }
  return CrossPointSettings::getSdFontRangePointSize(SETTINGS.sdFontSizeRange, sizeStep);
}

DictionaryFontActivation SdCardFontSystem::activateDictionaryFont(GfxRenderer& renderer, const char* familyName,
                                                                  uint8_t targetPointSize) {
  // A non-zero size with no dedicated family means "use the reader's installed
  // family at this size". This keeps the setting useful when the same custom
  // family is wanted for reading and definitions without keeping two families
  // resident.
  if ((!familyName || familyName[0] == '\0') && targetPointSize != 0 && SETTINGS.sdFontFamilyName[0] != '\0') {
    familyName = SETTINGS.sdFontFamilyName;
  }
  if (!familyName || familyName[0] == '\0') {
    return {restoreReaderFont(renderer), false};
  }

  MemoryBudget::logHeapShape("dict.font_before_activate");
  char path[160] = {};
  uint8_t selectedPointSize = 0;
  // Prefer the actual loaded reader-file size. Built-in readers have no SD
  // file, so use their effective physical point size instead.
  if (targetPointSize == 0) {
    targetPointSize = manager_.currentPointSize() != 0
                          ? manager_.currentPointSize()
                          : CrossPointSettings::getReaderFontPointSize(SETTINGS.getEffectiveReaderFontSize());
  }
  if (!findInstalledFontFile(familyName, targetPointSize, FontFileSelection::Closest, path, sizeof(path),
                             selectedPointSize)) {
    LOG_DBG("SDFS", "Dictionary font not found on card: %s", familyName);
    const char* globalFamilyName = SETTINGS.dictionarySdFontFamilyName;
    if (globalFamilyName[0] != '\0' && std::strcmp(familyName, globalFamilyName) != 0) {
      LOG_DBG("SDFS", "Using global dictionary font while per-book font is unavailable: %s", globalFamilyName);
      return activateDictionaryFont(renderer, globalFamilyName, SETTINGS.dictionaryFontPointSize);
    }
    const int readerFontId = restoreReaderFont(renderer);
    MemoryBudget::logHeapShape("dict.font_reader_fallback");
    return {readerFontId, false};
  }

  if (manager_.currentFamilyName() == familyName && manager_.currentPointSize() == selectedPointSize) {
    const int fontId = manager_.getFontId(manager_.currentFamilyName());
    MemoryBudget::logHeapShape("dict.font_reused_reader");
    return {fontId, true};
  }

  // A reader SD font can retain page glyphs, kerning, and advance tables after
  // a long reading session. They are disposable at this handoff: keeping them
  // through the headroom check makes a dictionary font appear unavailable until
  // its book cache is deleted or the heap happens to be less fragmented.
  const int activeReaderFontId = SETTINGS.getReaderFontId();
  const auto beforeCacheRelease = MemoryBudget::snapshot();
  if (renderer.releaseSdCardFontForLowMemory(activeReaderFontId)) {
    const auto afterCacheRelease = MemoryBudget::snapshot();
    LOG_DBG("SDFS", "Released reader SD-font caches before dictionary swap: free=%u->%u maxAlloc=%u->%u",
            beforeCacheRelease.freeHeap, afterCacheRelease.freeHeap, beforeCacheRelease.maxAllocHeap,
            afterCacheRelease.maxAllocHeap);
  }

  auto heap = MemoryBudget::snapshot();
  if (!MemoryBudget::hasHeapForDictionarySdFont(heap)) {
    // The reader family itself is also disposable for a dictionary swap. Retry
    // after releasing it so its font data does not cause a false low-memory
    // fallback.
    const auto beforeReaderUnload = heap;
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    heap = MemoryBudget::snapshot();
    LOG_DBG("SDFS", "Released reader font before dictionary swap retry: free=%u->%u maxAlloc=%u->%u",
            beforeReaderUnload.freeHeap, heap.freeHeap, beforeReaderUnload.maxAllocHeap, heap.maxAllocHeap);
  }
  if (!MemoryBudget::hasHeapForDictionarySdFont(heap)) {
    LOG_ERR("SDFS", "Low heap for dictionary font swap (%u free, %u max alloc, need %u/%u); using reader font",
            heap.freeHeap, heap.maxAllocHeap, MemoryBudget::DICTIONARY_SD_FONT_MIN_FREE,
            MemoryBudget::DICTIONARY_SD_FONT_MIN_MAX_ALLOC);
    const int readerFontId = restoreReaderFont(renderer);
    MemoryBudget::logHeapShape("dict.font_heap_fallback");
    return {readerFontId, false};
  }

  // unloadAll() also drops optional CJK UI sizes before the dictionary file is
  // allocated, so both families are never resident at once.
  if (!manager_.currentFamilyName().empty()) {
    manager_.unloadAll(renderer);
  }
  loadedFontPointSize_ = 0;

  if (manager_.loadFamilyFile(path, familyName, selectedPointSize, renderer)) {
    const int fontId = manager_.getFontId(manager_.currentFamilyName());
    LOG_DBG("SDFS", "Activated dictionary font %s at %u pt", familyName, manager_.currentPointSize());
    MemoryBudget::logHeapShape("dict.font_after_activate");
    return {fontId, true};
  }

  LOG_ERR("SDFS", "Failed to load dictionary font %s; restoring reader font", familyName);
  const int readerFontId = restoreReaderFont(renderer);
  MemoryBudget::logHeapShape("dict.font_reader_fallback");
  return {readerFontId, false};
}

int SdCardFontSystem::restoreReaderFont(GfxRenderer& renderer) {
  const char* familyName = SETTINGS.sdFontFamilyName;
  if (!familyName || familyName[0] == '\0') {
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    MemoryBudget::logHeapShape("dict.font_after_restore");
    return SETTINGS.getBuiltInReaderFontId();
  }

  char path[160] = {};
  uint8_t selectedPointSize = 0;
  if (!findInstalledFontFile(familyName, SETTINGS.getSdFontTargetPointSize(), FontFileSelection::Closest, path,
                             sizeof(path), selectedPointSize)) {
    LOG_ERR("SDFS", "Reader font unavailable while restoring: %s", familyName);
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    MemoryBudget::logHeapShape("dict.font_after_restore");
    return SETTINGS.getBuiltInReaderFontId();
  }

  if (manager_.currentFamilyName() != familyName || manager_.currentPointSize() != selectedPointSize) {
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    if (!manager_.loadFamilyFile(path, familyName, selectedPointSize, renderer)) {
      LOG_ERR("SDFS", "Failed to restore reader font: %s", familyName);
      MemoryBudget::logHeapShape("dict.font_after_restore");
      return SETTINGS.getBuiltInReaderFontId();
    }
    loadedFontPointSize_ = SETTINGS.getSdFontTargetPointSize();
    setupUiFallbacksDirect(renderer, familyName);
  }

  const int fontId = manager_.getFontId(manager_.currentFamilyName());
  MemoryBudget::logHeapShape("dict.font_after_restore");
  return fontId != 0 ? fontId : SETTINGS.getBuiltInReaderFontId();
}
