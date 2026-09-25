#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Shared request policy for both HTTP transports. URLs are parsed once per
// request hop, so the existing cold-path string allocations remain bounded.
namespace HttpRedirectPolicy {
struct Url {
  bool https = false;
  std::string host;
  std::string path;
  uint16_t port = 80;
};

inline bool equalsIgnoreCase(const std::string_view a, const std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const char left = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
    const char right = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
    if (left != right) return false;
  }
  return true;
}

inline bool parseUrl(const std::string_view url, Url& out) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return false;

  const std::string_view scheme = url.substr(0, schemeEnd);
  out.https = equalsIgnoreCase(scheme, "https");
  if (!out.https && !equalsIgnoreCase(scheme, "http")) return false;

  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = url.find_first_of("/?#", hostStart);
  const std::string hostPort{
      url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart)};
  if (pathStart == std::string::npos) {
    out.path = "/";
  } else {
    out.path.assign(url.substr(pathStart));
  }
  if (!out.path.empty() && (out.path[0] == '?' || out.path[0] == '#')) out.path.insert(out.path.begin(), '/');
  out.port = out.https ? 443 : 80;

  const size_t portSep = hostPort.rfind(':');
  if (portSep != std::string::npos) {
    out.host = hostPort.substr(0, portSep);
    const std::string portText = hostPort.substr(portSep + 1);
    if (portText.empty()) return false;
    uint32_t parsedPort = 0;
    for (const char c : portText) {
      if (c < '0' || c > '9') return false;
      parsedPort = parsedPort * 10 + static_cast<uint32_t>(c - '0');
      if (parsedPort > UINT16_MAX) return false;
    }
    if (parsedPort == 0) return false;
    out.port = static_cast<uint16_t>(parsedPort);
  } else {
    out.host = hostPort;
  }

  return !out.host.empty() && !out.path.empty();
}

inline bool sameOrigin(const Url& a, const Url& b) {
  return a.https == b.https && a.port == b.port && equalsIgnoreCase(a.host, b.host);
}

inline bool shouldSendAuthorization(const Url& request, const Url& credentialOrigin, const bool hasCredentials) {
  return hasCredentials && sameOrigin(request, credentialOrigin);
}

inline bool isAllowedRedirect(const Url& current, const Url& redirect) { return !current.https || redirect.https; }

inline std::string buildRedirectUrl(const std::string_view baseUrl, const std::string_view location) {
  const size_t fragmentStart = location.find('#');
  const std::string_view requestLocation = location.substr(0, fragmentStart);

  Url absolute;
  if (parseUrl(requestLocation, absolute)) return std::string(requestLocation);
  if (requestLocation.find("://") != std::string::npos) return "";

  Url base;
  if (!parseUrl(baseUrl, base)) return std::string(location);

  const size_t baseFragmentStart = baseUrl.find('#');
  const std::string_view requestBase = baseUrl.substr(0, baseFragmentStart);
  std::string origin = base.https ? "https://" : "http://";
  origin += base.host;
  if ((base.https && base.port != 443) || (!base.https && base.port != 80)) {
    origin += ":";
    origin += std::to_string(base.port);
  }

  if (requestLocation.starts_with("//")) return (base.https ? "https:" : "http:") + std::string(requestLocation);
  if (requestLocation.empty()) return std::string(requestBase);
  if (requestLocation[0] == '?') {
    const size_t queryStart = requestBase.find('?');
    return std::string(requestBase.substr(0, queryStart)) + std::string(requestLocation);
  }
  if (requestLocation[0] == '/') return origin + std::string(requestLocation);

  std::string basePath = base.path;
  const size_t queryStart = basePath.find('?');
  if (queryStart != std::string::npos) basePath.resize(queryStart);
  const size_t lastSlash = basePath.rfind('/');
  const std::string parent = lastSlash == std::string::npos ? "/" : basePath.substr(0, lastSlash + 1);
  return origin + parent + std::string(requestLocation);
}
}  // namespace HttpRedirectPolicy
