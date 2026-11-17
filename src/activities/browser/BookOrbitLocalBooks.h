#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace BookOrbitLocalBooks {

size_t count();
// Builds a session snapshot on the SD card. Call clear() when the catalog exits.
bool build(int& total);
bool readPage(int firstRow, size_t count, std::vector<std::string>& paths);
void clear();

}  // namespace BookOrbitLocalBooks
