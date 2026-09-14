#pragma once

using StrId = int;

constexpr StrId STR_BACK = 1;
constexpr StrId STR_CANCEL = 2;
constexpr StrId STR_SAVE = 3;
constexpr StrId STR_SELECT = 4;
constexpr StrId STR_DIR_UP = 5;
constexpr StrId STR_DIR_DOWN = 6;

class I18nStub {
 public:
  const char* get(StrId) const { return "translated"; }
};

inline I18nStub I18N;

inline const char* tr(const StrId) { return "translated"; }
