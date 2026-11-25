#include "BookContentIdStub.h"

#include "util/BookContentId.h"

namespace BookContentIdStub {
std::string stateDir;
}

std::string BookContentId::bookStateDirName(const std::string&) { return BookContentIdStub::stateDir; }
std::string BookContentId::bookStateDir(const std::string&) { return BookContentIdStub::stateDir; }
