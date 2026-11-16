#pragma once

/**
 * Recognises BookOrbit books that reached the SD card by other means than this catalog
 * (copied over USB, sent by another app, downloaded by another reader), files them where
 * a catalog download would have put them, and adds them to the download index, so the
 * catalog marks them, lists them under "On device" and the BookOrbit Home counts them.
 *
 * The candidates come from the Library index, which already lists every book on the card:
 * the EPUBs it saw arrive since the last pass (its records' `firstSeen`, kept across
 * renames) that the download index does not know. Each is hashed the way KOReader and
 * BookOrbit do (partial MD5) and the server is asked which book the hash belongs to,
 * through the KOReader plugin's match check. That also recognises files the server has
 * since rewritten or delivered converted, and hashes linked by hand. Files the server does
 * not recognise are listed in its "KOReader Books to Review", named after the Library's
 * title and author, and remembered here with their hash: when the server's library token
 * changes (a book added, a file linked by hand), they are asked about again.
 *
 * A recognised book moves to the path the server's naming template gives it under the
 * download folder. A second copy of a book already filed (at that path, or wherever the
 * download index records it) is deleted instead; its reading position and highlights are
 * keyed by the book's content, so the copy kept has them. Books in /Read stay put, as does
 * a book whose destination holds a different file.
 *
 * Runs in two halves around the network, from the BookOrbit theme's syncs (the sleep sync
 * and the Home tile's refresh). Both are blocking and poll `cancelled` between steps.
 */
namespace BookOrbitDeviceScan {

// Offline half, before Wi-Fi: rebuilds the Library index when the card changed since it was
// built, then hashes the books that arrived since the last pass into a work file for run().
void prepare(bool (*cancelled)());

// Online half, with Wi-Fi up: identifies what prepare() found (and, when the server's
// library changed, the files it did not recognise before), then files the recognised books.
void run(bool (*cancelled)());

}  // namespace BookOrbitDeviceScan
