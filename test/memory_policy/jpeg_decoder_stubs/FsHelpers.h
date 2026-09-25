#pragma once

#include <string>

namespace FsHelpers {
inline bool hasJpgExtension(const std::string& extension) { return extension == ".jpg" || extension == ".jpeg"; }
}  // namespace FsHelpers
