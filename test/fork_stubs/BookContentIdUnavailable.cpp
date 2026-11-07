// This fork keys bookmark, clipping and progress stores on the book's content hash
// (util/BookContentId.h). Upstream's tests check the path-keyed stores, so the hash is
// reported as unavailable here, and the stores fall back to the names upstream gives them.
#include "util/BookContentId.h"

std::string BookContentId::contentHash(const std::string&) { return ""; }
std::string BookContentId::bookStateDirName(const std::string&) { return ""; }
std::string BookContentId::bookStateDir(const std::string&) { return ""; }
