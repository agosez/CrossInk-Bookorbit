// CrossPointSettings' in-class initializer calls this; tests that build the settings
// header without CrossPointSettings.cpp need the definition (X3/X4 default).
#include "CrossPointSettings.h"

uint8_t CrossPointSettings::defaultIndexingMethod() { return INDEXING_FULL_SECTION; }
