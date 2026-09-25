#pragma once

#include <string>

class BookOrbitCredentialStore {
 public:
  const std::string& getServerUrl() const { return serverUrl; }
  std::string serverUrl = "https://books.example.test";
};

inline BookOrbitCredentialStore BOOKORBIT_STORE;
