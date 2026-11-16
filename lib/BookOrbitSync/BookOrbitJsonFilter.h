#pragma once

#include <ArduinoJson.h>

// Fills `filter` from JSON text, the way the BookOrbit clients describe what to keep of a
// reply: parsing the text costs less flash than building the same filter node by node,
// about 40 bytes for each field it names. Defined once, in BookOrbitSyncClient.cpp, so the
// firmware holds a single copy of ArduinoJson's text parser for it.
void bookOrbitSetFilter(JsonDocument& filter, const char* json);
