#include "I18n.h"

#include <cstddef>
#include <cstring>

#include "I18nStrings.h"

using namespace i18n_strings;

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  // Use generated helper function - no hardcoded switch needed!
  LangStrings lang = getLanguageStrings(_language);

  // Bit 15 means "my base language spells this the same, ask it instead": the
  // string is stored once, in the deepest language of the chain that differs.
  // Most languages base on English directly, so that is one hop; a variant
  // (Valencian -> Catalan -> English) takes two. Chains always end at English,
  // whose table never sets the bit; the counter only stops a malformed table
  // from looping forever.
  uint16_t off = lang.offsets[index];
  for (uint8_t hops = 0; (off & 0x8000) != 0 && hops < static_cast<uint8_t>(Language::_COUNT); ++hops) {
    lang = getLanguageStrings(lang.base);
    off = lang.offsets[index];
  }
  if ((off & 0x8000) != 0) {
    return "???";
  }
  return lang.data + off;
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;
}

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return "???";
  }
  return LANGUAGE_NAMES[index];
}

Language I18n::languageFromCode(const char* code) {
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    if (strcmp(code, LANGUAGE_CODES[i]) == 0) return static_cast<Language>(i);
  }
  return Language::EN;
}

// Generate character set for a specific language
const char* I18n::getCharacterSet(Language lang) {
  const auto langIndex = static_cast<size_t>(lang);
  if (langIndex >= static_cast<size_t>(Language::_COUNT)) {
    lang = Language::EN;  // Fallback to first language
  }

  return CHARACTER_SETS[static_cast<size_t>(lang)];
}
