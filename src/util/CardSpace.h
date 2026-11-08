#pragma once

// Storage.usedBytes() counts the card's free clusters by reading its whole allocation
// table, with the SD card and the display's bus held. exFAT keeps one bit per cluster
// there, a fraction of a second even for a 128 GB card. FAT32 keeps four bytes per
// cluster, which takes seconds on a typical card and minutes on a slow one (#110).
namespace CardSpace {

// True when the card is exFAT, so usedBytes() is quick enough to call where the user
// waits. False on FAT, and when the format cannot be told.
bool isQuickToCount();

}  // namespace CardSpace
