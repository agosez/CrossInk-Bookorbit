#pragma once

// Joins a saved Wi-Fi network with no UI, for the BookOrbit sync that runs behind the
// sleep screen. WifiSelectionActivity does the same with its screens: the last network
// joined first, then any other saved network a scan finds, strongest first.
namespace SavedWifiConnect {

// Polled while waiting on the radio; returning true abandons the attempt at once.
using StopCheck = bool (*)();

bool connect(StopCheck shouldStop);

}  // namespace SavedWifiConnect
