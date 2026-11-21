#pragma once

#include <cstddef>
#include <string>
#include <vector>

/**
 * The books the BookOrbit catalog lists under "On device": the catalog downloads the
 * download index knows, wherever the server's file naming template filed them, then every
 * EPUB directly in the download folder, the SD root and /Read, where books copied over USB
 * and older downloads land.
 *
 * The list is sorted on the SD card and read back a page at a time, so its length costs
 * card space rather than RAM: build() writes one fixed-size record per book, and the order
 * a bounded-memory sort (library::ExternalSorter) put them in. Both files stay in
 * /.crosspoint while the listing is open; clear() removes them. Main task only.
 */
namespace BookOrbitLocalBooks {

// How many books the list holds, without building it.
size_t count();

// Builds the sorted list; `total` receives its row count. False, with nothing left behind,
// when the card or the heap could not hold it.
bool build(int& total);

// The paths of rows [firstRow, firstRow + count) of the last build, in list order.
bool readPage(int firstRow, size_t count, std::vector<std::string>& paths);

// Removes the files of the last build.
void clear();

}  // namespace BookOrbitLocalBooks
