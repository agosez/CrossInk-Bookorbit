---
title: User Guide
nav_order: 1.5
---

# CrossInk User Guide

This guide covers day-to-day device use. For focused reference material, see [Reader Features](./reader-features.md), [Controls](./controls.md), [SD Card Fonts](./sd-card-fonts.md), [File Transfer](./webserver.md), and [Troubleshooting](./troubleshooting.md). For Dashboard and Minimal Home-screen gestures, see [Touch Navigation](./touch-navigation.md).

- [CrossInk User Guide](#crossink-user-guide)
  - [1. Hardware Overview](#1-hardware-overview)
    - [Button Layout](#button-layout)
    - [Taking a Screenshot](#taking-a-screenshot)
  - [2. Power \& Startup](#2-power--startup)
    - [Power On / Off](#power-on--off)
    - [First Launch](#first-launch)
  - [3. Screens](#3-screens)
    - [3.1 Home Screen](#31-home-screen)
    - [3.2 Reading Mode](#32-reading-mode)
    - [3.3 Browse Files Screen](#33-browse-files-screen)
    - [3.4 Library Screen](#34-library-screen)
    - [3.5 File Transfer Screen](#35-file-transfer-screen)
    - [3.5.1 Calibre Wireless Transfers](#351-calibre-wireless-transfers)
    - [3.6 Settings](#36-settings)
      - [3.6.1 Display](#361-display)
      - [3.6.2 Reader](#362-reader)
      - [3.6.3 Controls](#363-controls)
      - [3.6.4 System](#364-system)
      - [3.6.5 OPDS Servers (Multiple Libraries)](#365-opds-servers-multiple-libraries)
      - [3.6.6 Web Settings (Wi-Fi + OPDS)](#366-web-settings-wi-fi--opds)
      - [3.6.7 KOReader Sync Quick Setup](#367-koreader-sync-quick-setup)
        - [Option A: CrossPoint Sync Server (`sync.crosspointreader.com`, default)](#option-a-crosspoint-sync-server-synccrosspointreadercom-default)
        - [Option B: Legacy Public KOReader Server (`sync.koreader.rocks`)](#option-b-legacy-public-koreader-server-synckoreaderrocks)
        - [Option C: Self-Hosted Server (Docker Compose)](#option-c-self-hosted-server-docker-compose)
    - [3.7 Sleep Screen](#37-sleep-screen)
      - [Cover settings](#cover-settings)
      - [Custom images](#custom-images)
    - [3.8 Boot Screen](#38-boot-screen)
    - [3.9 Custom Fonts (SD Card)](#39-custom-fonts-sd-card)
  - [4. Reading Mode](#4-reading-mode)
    - [Page Turning](#page-turning)
    - [Chapter Navigation](#chapter-navigation)
    - [Auto Page Turn](#auto-page-turn)
    - [Tilt Page Turn (X3 and Sticky)](#tilt-page-turn-x3-and-sticky)
    - [Touch Reader Controls](#touch-reader-controls)
    - [Footnote Navigation](#footnote-navigation)
    - [System Navigation](#system-navigation)
    - [Supported Languages](#supported-languages)
  - [5. Reader Menu](#5-reader-menu)
    - [5.1 Chapter Selection](#51-chapter-selection)
    - [5.2 Bookmarks](#52-bookmarks)
  - [6. Current Limitations & Roadmap](#6-current-limitations--roadmap)
  - [7. Troubleshooting Issues & Escaping Bootloop](#7-troubleshooting-issues--escaping-bootloop)

## 1. Hardware Overview

CrossInk supports many devices with various button layouts. For the X3/X4 devices with front buttons, by default they will use the following layout from left to right, and this guide will refer to them by those names.

<table>
  <thead>
    <tr>
      <th colspan="4"><center>Default Front Button Mapping</center></th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>Back</td>
      <td>Confirm</td>
      <td>Left</td>
      <td>Right</td>
    </tr>
  </tbody>
</table>

For devices with buttons on either side of the device, this guide may refer to the buttons as follows:

| Location       | Button Mapping     |
| -------------- | ------------------ |
| **Left Side**  | **Left**/**Up**    |
| **Right Side** | **Right**/**Down** |

Button layout can be customized in **Settings > Controls**.

### Taking a Screenshot

When the Power Button and Down button are pressed at the same time, it will take a screenshot and save it in the folder `screenshots/`.

Alternatively, while reading a book, press the **Confirm** button to open the reader menu and select **Take screenshot**.

---

## 2. Power & Startup

### Power On / Off

By default, **press and hold the Power button for approximately half a second** to put the device to sleep or wake it. In **Settings > Controls > Power Button**, you can configure the short-press and long-press actions separately. A short press wakes the device only when **Short-press Action** is set to **Sleep/Wake** or **Wake**; otherwise, hold Power for about half a second to wake it. See [Controls](#363-controls) for details.

To reboot the device (for example after a firmware update or if it's frozen), press and release the Reset button, and then quickly press and hold the Power button for a few seconds.

### First Launch

Upon turning the device on for the first time, you will be placed on the **[Home](#31-home-screen)** screen.

> [!NOTE]
> On subsequent restarts, the firmware will automatically reopen the last book you were reading.

---

## 3. Screens

### 3.1 Home Screen

The Home screen is the main entry point to the firmware. From here you can navigate to **[Reading Mode](#4-reading-mode)** with the most recently read book, the **[Browse Files](#33-browse-files-screen)** screen, the **[Library](#34-library-screen)** screen, the **[File Transfer](#35-file-transfer-screen)** screen, or **[Settings](#36-settings)**.

### 3.2 Reading Mode

See [Reading Mode](#4-reading-mode) below for more information.

### 3.3 Browse Files Screen

The Browse Files screen acts as a file and folder browser. The full path to the current directory is shown at the top of the screen. File extensions are displayed alongside each filename, and directories are shown with brackets (e.g. `[folder-name]`). Hidden directories can be shown from settings.

- **Navigate List:** Use **Left** (or **Up**), or **Right** (or **Down**) to move the selection cursor up and down through folders and books. You can also long-press these buttons to scroll a full page up or down.
- **Open Selection:** Press **Confirm** to open a folder or start reading a selected book. Selecting a `.bmp` file will open the image viewer.
- **Delete Files or Folders:** Hold and release **Confirm** to open the selected file or folder action menu, then choose **Delete**. You will be given an option to either confirm or cancel. Folder deletion is limited to empty folders.
- **Book Actions:** EPUB and XTC files can also show options such as **Delete Cache**, **Mark Finished**, and **Track Reading Stats** from the same action menu. Turning tracking off for a book keeps its saved stats; it only stops new tracking for that book.

### 3.4 Library Screen

**Library** replaces Recent Books and lists supported books across the SD card.
The first visit reads the card to build the list; later visits reuse it and
refresh after file changes or transfers.

- **Find a book:** Use **Search** to filter by title or author. With **Use Book
  Metadata** off, books are shown and searched by filename instead.
- **Sort:** Choose **Date Added**, **Title**, **Author (Last Name)**, **Author
  (First Name)**, **Recently Opened**, **Series**, or **Genre**. You can reverse
  the sort order. **Recently Opened** shows only your saved reading history,
  rather than every book on the card.
- **Buttons:** Use **Up/Down** to select a book and **Confirm** to open it. Hold
  **Confirm** for book actions. **Left** opens Sort; **Right** opens the menu
  containing **Search**, **Settings**, and **Refresh Library**.
- **Touch:** Tap a book to open it, hold it for book actions, and swipe to scroll.
  Use the search, settings, refresh, and sort controls at the top.
- **Library Settings:** Open Library's **Settings** to choose embedded EPUB
  metadata or filenames, compact or expanded rows, and a list or cover grid
  for **Recently Opened View**. You can also hide finished books, show series
  and genre, and filter which book formats appear.
- **Book actions:** EPUB and XTC books can offer **Reading Stats** without
  opening the book, when tracking is enabled for the device and that book.
  The action menu also includes book-specific controls such as **Mark Finished**
  and **Track Reading Stats** where supported.

If files were changed externally while the reader stayed on, use **Refresh
Library** to scan the card again. **Date Added** uses the file's creation time
when available, so a copy tool that preserves old timestamps may affect its
order.

### 3.5 File Transfer Screen

The File Transfer screen allows you to upload and manage files on the device.
Choose **Join a Network**, **Calibre Wireless**, or **Create Hotspot** to start
the web server for the selected mode.

See the [File Transfer guide](./webserver.md) for connection and upload details.

The web file manager can upload, download, rename, move, and delete files on the device.

The web interface also supports **WebDAV**, allowing you to mount the device as a network drive and manage files directly from your computer's file manager.

Download links for files already on the device are available in the web interface, so you can retrieve books or screenshots over Wi-Fi without connecting a cable.

A **Wi-Fi signal strength indicator** (dBm) is displayed on-screen during joined-network web server sessions.

The same screen also has **Receive File**, which receives a supported
book or image directly from another nearby CrossInk reader without joining a
Wi-Fi network. See [Nearby File Transfer](./nearby-file-transfer.md) for the
complete sender and receiver workflow.

On X4 Pro, the screen also includes **USB Drive**. This exposes the reader's SD
card to a computer over USB for direct file management. See the
[USB Drive instructions](./installation.md#usb-drive) for the safe-eject and
disconnect behavior.

> [!TIP]
> Advanced users can manage files programmatically with the same HTTP endpoints
> used by the web interface. The browser interface is the supported path for
> normal file management.

### 3.5.1 Calibre Wireless Transfers

CrossInk supports sending books from Calibre using the CrossPoint Reader device plugin.

1. Download the current `crosspoint_reader` plugin ZIP from the
   [CrossPoint Reader plugin releases](https://github.com/crosspoint-reader/calibre-plugins/releases).
2. In Calibre, open **Preferences > Plugins > Load plugin from file** and select
   that ZIP. Restart Calibre if it asks you to.
3. On the device, open **File Transfer > Calibre Wireless** and join the same
   2.4 GHz Wi-Fi network as the computer.
4. Keep the Calibre Wireless screen open, then use Calibre's **Send to device**
   action. The device screen shows the transfer progress and completion notice.

### 3.6 Settings

The Settings screen groups options by purpose. The exact choices can vary by
device model and build.

#### 3.6.1 Display

- **Sleep Screen**: Which sleep screen to display when the device sleeps:
  - "Dark" (default) - The default dark CrossInk logo sleep screen
  - "Light" - The same default sleep screen, on a white background
  - "Custom" - Custom images from the SD card; see [Sleep Screen](#37-sleep-screen) below for more information
  - "Cover" - The book cover image (Note: this is experimental and may not work as expected)
  - "None" - A blank screen
  - "Cover + Custom" - The book cover image while actively reading, falls back to "Custom" behavior otherwise
  - "Page Overlay" - Uses an image to overlay on top of the current page. Best used with transparent `.png` files or black and white `.bmp` files.
  - "Reading Stats" - Recent reading stats on the sleep screen; falls back to **Minimal** while tracking is off for the device or recent book
  - "Minimal" - A minimal sleep screen
  - "Minimal Stats" - A minimal stats sleep screen on supported devices; falls back to **Minimal** while tracking is off for the device or recent book
  - "Dashboard" - A dashboard-style sleep screen based on the Dashboard theme
  - "Quick Resume" - Keeps the current content visible while sleeping

- **Sleep Screen Cover Mode**: How to display the book cover when "Cover" sleep screen is selected:
  - "Fit" (default) - Scale the image down to fit centered on the screen, padding with white borders as necessary
  - "Crop" - Scale the image down and crop as necessary to try to fill the screen (Note: this is experimental and may not work as expected)

- **Sleep Screen Cover Filter**: What filter will be applied to the book cover when "Cover" sleep screen is selected:
  - "None" (default) - The cover image will be converted to a grayscale image and displayed as it is
  - "Contrast" - The image will be displayed as a black & white image without grayscale conversion
  - "Inverted" - The image will be inverted as in white & black and will be displayed without grayscale conversion

- **Quick Resume on Timeout**: Whether to enable the "Quick Resume" sleep screen when the device goes to sleep due to inactivity (System > Time to Sleep). This is useful for quickly resuming reading without waiting for the device to fully wake up and load the book. This overwrites the Sleep Screen Cover Mode when enabled.

- **Hide Battery %**: Configure where to suppress the battery percentage display in the status bar; the battery icon will still be shown:
  - "Never" (default) - Always show battery percentage
  - "In Reader" - Show battery percentage everywhere except in reading mode
  - "Always" - Always hide battery percentage

- **Hide Clock**: On devices with a real-time clock, choose whether the clock is
  shown everywhere, hidden only in the reader, or always hidden.

- **Refresh Frequency**: Set how often the screen does a full refresh while reading to reduce ghosting; options are every 1, 5, 10, 15, or 30 pages.

- **UI Theme**: Set which UI theme to use:
  - "Classic" - The original CrossInk theme
  - "Minimal" - A minimal theme with a large book cover
  - "Dashboard" - A dashboard-style home layout
  - "Lyra" - A theme with simple icons featuring your current book
  - "Lyra Extended" - Lyra, but displays 3 books instead of 1 on the **[Home Screen](#31-home-screen)**
  - "Lyra Carousel" - A carousel-based Lyra home layout
  - "RoundedRaff" - A rounded theme with additional visual styling
  - "Cover Grid" (PSRAM devices, such as Sticky and X4 Pro) - Shows the current
    book and up to six additional book covers; tap a cover to open that book
  - "BookOrbit" - Home as pages of tiles, modeled on BookOrbit's dashboard: the
    current book, streak, week and other books being read, then the catalog's
    shelves, then your reading statistics. It works from the SD card and never
    waits on the network. See **[BookOrbit Home](./reader-features.md#bookorbit-home)**.

  Themes that show reading stats hide those stats while **Track Reading Stats** is off.

- Library display and filtering options are now in **Library > Settings**; see
  [Library Screen](#34-library-screen).

- **Sunlight Fading Fix**: Configure whether to enable a software-fix for the issue where white X4 models may fade when used in direct sunlight:
  - "OFF" (default) - Disable the fix
  - "ON" - Enable the fix

- **Frontlight and wake schedule** (on supported devices with a frontlight and
  real-time clock): Open **Settings > Display > Frontlight**.
  - **Restore Light on Wake**: When enabled, a frontlight that was on before
    sleep is turned back on when the device wakes. If the light was off before
    sleep, a complete schedule can still decide whether it should turn on.
  - **Frontlight Schedule**: Enable the daily wake schedule, then set **Start**
    and **End** in local time. Times use one-minute increments. The schedule
    includes the Start time and excludes the End time, and it can cross
    midnight (for example, 9:00 PM to 7:00 AM). Start and End must both be set
    and different; otherwise the schedule is inactive.
  - The schedule is checked when the device boots or wakes, not continuously
    while it is already awake. Restore on Wake takes precedence when the light
    was on before sleep. Set the device's local time and UTC offset under
    **Settings > System > Device** so the schedule uses the expected clock.
    When the schedule is disabled or an endpoint is unset, the endpoint value
    is shown as `--`; saved endpoint times are retained for later re-enabling.

> [!NOTE]
> A battery charging indicator is shown on the battery icon whenever the device is actively charging.

#### 3.6.2 Reader

**Global settings and individual books:** Choices under **Settings > Reader**
are the defaults for your EPUB books. Layout choices such as font family, font
size, and margins can be saved for one book from the in-book **Font** and
**Layout** tabs. A saved book choice takes precedence over the matching global
setting; choices you have not changed for that book still follow the global defaults.
For example, a book with its own font size can still pick up a later change to
your global margins. If you open global settings through the pull-down panel
on a touch reader, inherited font and layout changes also apply to the open
book when you return. **Dark Mode** and **Stable Page Numbers** are global
settings.

If a global change does not affect one EPUB, open its Reader Menu **Settings**
tab and choose **Reset Book Reader Settings**, or use the book action menu in
**Browse Files** or **Library**. This removes that book's saved reader choices
so it follows the current global defaults again; it keeps reading progress,
bookmarks, clippings, and stats, and does not reset your other books or global
settings. Older saved books may need this reset before they can inherit
individual global changes. **Delete Book Cache** does not reset these choices.

- **Reader Font Family**: Choose the font used for reading:
  - "Lexend Deca" (default)
  - "Bitter"

- **Reader Font Size**: Choose from the sizes available for the selected font
  and firmware build. ESP32-S3 readers use scalable TTF fonts with every whole
  point size from **8–22 pt**; see [Scalable TTF Fonts](./scalable-fonts.md).

- **Reader Line Spacing**: Adjust the line height as a percentage.

- **Word Spacing**: In EPUB books, choose **Normal** or one of four wider
  spacing levels between words. Open the reader menu, then select **Font >
  Line/Word Spacing > Word Spacing**. Changing it reflows the current
  book, so page positions may change; it is not available for TXT books.

- **Reader Screen Margin**: Controls the screen margins in Reading Mode between 5 and 40 pixels in 5-pixel increments.

- **Reader Paragraph Alignment**: Set the alignment of paragraphs; options are "Justified" (default), "Left", "Center", "Right", or "Book's Style".

- **Publisher Page Numbers**: Show page numbers supplied by the EPUB when the
  book includes them.

  Note: This reserves 5px of left margin to your screen to provide space for the page numbers. This is only noticeable if your `Left/Right` margins are set to `5`. If the page has no publisher page number, your margins may appear uneven.

- **Hyphenation**: Whether to hyphenate text in Reading Mode; options are "ON" or "OFF". Korean text wraps at spaces when "OFF"; when "ON", a Korean word may also split at a line end without drawing a hyphen.

- **Reading Orientation**: Set the screen orientation for reading EPUB files:
  - "Portrait" (default) - Standard portrait orientation
  - "Landscape CW" - Landscape, rotated clockwise
  - "Portrait 180" - Portrait, upside down
  - "Landscape CCW" - Landscape, rotated counter-clockwise

- **Extra Paragraph Spacing**: Set how to handle paragraph breaks:
  - "ON" - Vertical space will be added between paragraphs in Reading Mode
  - "OFF" - Paragraphs will not have vertical space added; source-defined first-line indentation is shown when available

- **Embedded Style**, **Images**, **Focus Reading**, and
  **Guide Dots** are directly available from the Reader settings. See
  [Reader Features](./reader-features.md) for their behavior, including the
  [Focus Reading](./reader-features.md#focus-reading) guide.

- **Disable Touchscreen**: Block screen-touch input while a book is open,
  while leaving touch available in reader menus so you can turn it back on.
  To disable only touch page turns, use **Next Page** and **Previous Page**
  in **Settings > Controls > Taps & Gestures**.

- **Status Bars**: Configure top and bottom bars separately for EPUB, TXT, and
  XTC reading. Open **Settings > Reader > Status Bars**, or the EPUB Reader
  Menu's **Settings > Status Bars**. Select a bar to preview it at its reading
  position and choose its items:
  - Top and bottom bars each have three left slots, one center slot, and three right slots.
  - Stable Page Numbers - Add a reference-page count to a slot when an EPUB has
    CrossInk reference metadata; see [Stable Page Numbers](./reader-features.md#stable-page-numbers)
  - Chapter Page Count - Show/Hide the current page in the chapter (ex: 5/25). Page count may change based on the font size and margins set.
  - Book Progress Percentage - Show/Hide the current percent progress in the book.
  - Percentage Format - Show book progress as `10%` (default), `10.1%`, or `10.12%`.
  - Progress Bar - Show/Hide a progress bar for either the book or chapter.
  - Progress Bar Thickness - Set the thickness of the progress bar
  - Title - Display the chapter or book title
  - Time Left - Display the estimated reading time left for the book or chapter
  - Battery - Show/Hide the battery indicator
  - XTC Status Bar - Show/Hide a status bar for XTC files

On touchscreen readers, when **Tap to Hide Status Bar** is enabled (the default
in **Settings > Controls > Taps & Gestures**), tap the status-bar area while
reading to show or hide the entire status bar for the current reading session.
This tap is available while reader touch input is enabled. The quick
toggle does not change the page layout or page breaks; tap the same status-bar
region again to restore a hidden bar. Use **Status Bars** to choose
which status-bar items are shown.

#### 3.6.3 Controls

- **Power Button**: Configure short-press and long-press actions independently. On supported devices, you can also configure the **Power + Up** shortcut.

- **Front Buttons**: Configure front-button remapping, orientation awareness,
  reader-only long-press behavior, Back action, and Menu action.

- **Side Buttons**: Assign a **Short-press Action** and **Long-press Action**
  separately for **Left/Up** and **Right/Down** while reading. Choose page or
  chapter turns, font-size changes, **Rotate Page CCW**, **Rotate Page CW**,
  **Rotate Page 180°**, or other available shortcuts. Set an action to **Ignore**
  to leave that press unassigned. Existing layouts migrate to equivalent
  individual actions. **Orientation aware** and the side-button chord setting
  remain available on supported devices. Rotation and font-size actions apply
  to EPUB/TXT text; they cannot rotate or resize pre-rendered XTC pages.

- **Long-press Behavior**: Set whether long-pressing front page-turn buttons does nothing, skips to the next/previous chapter, or changes reader orientation.

- **Power Short-press Action / Long-press Action**: Choose what a short press or a hold of about 0.4 seconds does. Available actions include:
  - "Ignore" (default short-press action) - Do nothing for this press length
  - "Sleep/Wake" (default long-press action) - Put the awake device to sleep; when selected as **Short-press Action**, also allow a short press to wake it
  - "Sleep" - Put the awake device to sleep without enabling short-press wake
  - "Wake" - When selected as **Short-press Action**, allow a short press to wake the device; does nothing while the device is awake
  - "Next Page" - Turn to the next page while reading
  - "Toggle Bookmark", "Reading Stats", "Mark Finished", "Refresh", "Change Font", "Guide Dots", "Focus Reading", "Auto Page Turn", "Sync Progress", "File Transfer", "Calibre Wireless", "Join a Network", "Create Hotspot", "Screenshot", "Dark Mode", "Browse Files", "Library", or "Save Clipping" - Run the matching action
  - "Footnotes" - Select a footnote reference on the current page, with a list
    fallback when needed; see [Footnote Navigation](#footnote-navigation).

- **Quick-return from Footnotes**: This setting appears under **Settings > Controls > Power Button** after you assign **Footnotes** to the short- or long-press Power action, or to the long-press Back or Menu action. When enabled, a short Power press returns from a footnote page to the original reading page.

- **Taps & Gestures** (touchscreen devices): Configure the touch interactions
  available while reading from **Settings > Controls > Taps & Gestures**. The
  submenu includes:
  - **Next Page** and **Previous Page**: Choose which taps and swipes advance or
    go back one page. Each direction can be set to **Tap & Swipe** (default),
    **Tap Only**, **Swipe Only**, **Inverted Tap**, or **Disabled** independently.
  - **Pinch to Resize Font** (multi-touch devices): Enable or disable changing
    the font size with a two-finger pinch in EPUB and TXT readers.
  - **Two-finger Screen Rotation** (multi-touch devices): Enable or disable
    twisting two fingers to rotate the reading orientation in EPUB and TXT
    readers.
  - **Tap to Hide Status Bar**: Enable or disable tapping the visible status-bar
    area to show or hide it for the current reading session.
  - **Edge Gestures**: Assign upward and downward slides on the left and right
    screen edges; see [Edge Gestures](./controls.md#edge-gestures).
  - **Two-finger Swipe** (multi-touch devices): Assign an action to each
    two-finger swipe direction. The available actions depend on the device and
    reader format.
    The submenu is not shown on devices without a touchscreen, and the
    multi-touch entries appear only when the hardware supports them. See [Touch
    Reader Controls](#touch-reader-controls) for the gesture details.

#### 3.6.4 System

- **Time to Sleep**: Set the duration of inactivity before the device automatically goes to sleep. Values are in minutes, with a "Never" option at the end of the range.

- **Custom Boot Screen**: Enable or disable custom boot screens (enabled by
  default). When disabled, CrossInk uses the standard logo on cold boot and keeps the current sleep
  screen visible on power-button wake, even if a custom image or boot-screen
  folder is configured. Disabling this does not remove the selected image or
  the folders; turn it back on to use them again.

- **Device**: Set the device name and time-to-sleep timeout. Devices with a
  real-time clock also expose clock format, UTC offset, and a sync action.

- **Files & Cache**: Configure hidden files, file extensions, file-browser view,
  finished-book behavior, and clear the reading cache.

- **Reading Stats**: Turn device-wide stats tracking on or off, configure
  idle-time filtering, and access all-time stats backup/reset actions. Turning
  tracking off pauses new per-book and all-time stats but keeps the history
  and Time Left pace estimates already saved on the device. Stats entries and
  recorded-stat displays in menus and themes are hidden while tracking is off;
  the Dashboard can still show a saved Time Left estimate. Turn tracking back
  on to see the saved history and resume recording. Time and pages from the off
  period are not added later. See [Reader Features](./reader-features.md#turn-tracking-on-or-off)
  for per-book controls.

- **Wi-Fi Networks**: Connect to Wi-Fi networks for file transfers and firmware updates.

- **KOReader Sync**: Options for setting up KOReader for syncing book progress. **Smart sync** is the default for new configurations and auto-resolves simple push/pull decisions. Existing credential files retain **Ask every time** when migrated; you can switch Sync Behavior at any time if you prefer manual confirmation.

- **OPDS Servers**: Manage one or more OPDS [(Open Publication Distribution System)](https://en.wikipedia.org/wiki/Open_Publication_Distribution_System) libraries for browsing and downloading books. See [OPDS Servers (Multiple Libraries)](#365-opds-servers-multiple-libraries) below.

- **Check for Updates** and **SD Firmware Update**: Check for firmware updates
  over Wi-Fi or install a `firmware.bin` placed on the SD card.

- **Include Release Candidates**: Off by default. When on, Check for Updates also
  offers this fork's release candidates, the test builds published ahead of a
  release, whenever one is newer than both the installed firmware and the latest
  release. Turning it back off keeps the installed candidate until a newer
  release comes out: the updater never installs an older version.

- **Language**: Set the UI language. CrossInk supports 28 languages: English,
  Spanish, French, German, Czech, Brazilian Portuguese, Russian, Swedish,
  Romanian, Catalan, Ukrainian, Belarusian, Italian, Polish, Finnish, Danish,
  Dutch, Turkish, Kazakh, Hungarian, Lithuanian, Slovenian, Valencian, Hebrew,
  Vietnamese, Slovak, Portuguese (Portugal), and Arabic.

#### 3.6.5 OPDS Servers (Multiple Libraries)

CrossInk supports saving multiple OPDS servers and switching between them when browsing catalogs.

1. Open **Settings -> System -> OPDS Servers**.

2. Select **Add Server** to create a new entry, or select an existing server to edit it.

3. Configure these fields:
   - **Server Name**: Optional display name (for example, "Home Calibre" or "Public Catalog").

   - **OPDS Server URL**: Full catalog root URL (for Calibre Content Server, usually ends with `/opds`).

   - **Username / Password**: Optional credentials for authenticated servers.

4. Use **Delete Server** inside a server entry to remove it.

Behavior notes:

- You can store up to 8 OPDS servers.
- OPDS authentication supports HTTP Basic auth. If you use Calibre Content Server with authentication enabled, set it to Basic (not Digest).

You can also manage OPDS servers from the web interface while in File Transfer mode:

1. Connect to the device web UI.
2. Open `http://<device-ip>/settings`.
3. Use the **OPDS Servers** card to add, edit, or delete entries.

For web-based Wi-Fi network management, see [File Transfer](./webserver.md).

#### 3.6.6 Web Settings (Wi-Fi + OPDS)

While in **File Transfer** mode, the web settings page includes management cards for both **Wi-Fi Networks** and **OPDS Servers**.

1. On device: open **File Transfer** and connect through **Join a Network** or **Create Hotspot**.
2. In a browser, open `http://<device-ip>/settings` or `http://crosspoint.local/settings`.
3. In **Wi-Fi Networks**, add, edit, or delete saved network entries (SSID + optional password).
4. In **OPDS Servers**, add, edit, or delete OPDS catalogs.

Behavior notes:

- Passwords are never shown back in the web UI after saving.
- Leaving Password blank while editing keeps the existing saved password unchanged.
- The web UI can save hidden-network SSIDs, but connecting to hidden networks still depends on the device-side Wi-Fi connection flow.

#### 3.6.7 KOReader Sync Quick Setup

CrossInk can sync reading progress with KOReader-compatible sync servers.
It also interoperates with KOReader apps/devices when they use the same server and credentials.

##### Option A: CrossPoint Sync Server (`sync.crosspointreader.com`, default)

When **Sync Server URL** is left empty, CrossInk uses the free CrossPoint sync server at `https://sync.crosspointreader.com`. It speaks the standard KOReader sync protocol (so KOReader apps can use it too) and additionally stores an exact spine/page position for lossless CrossInk-to-CrossInk sync.

1. On each CrossInk device:
   - Go to **Settings -> System -> KOReader Sync**.

   - Set **Username** and **Password** (enter the plain password; CrossInk computes MD5 internally, and use the same values on all devices).

   - Leave **Sync Server URL** empty (or set it to `https://sync.crosspointreader.com`).

   - On the first device, run **Sign Up** once to create the account directly from the device. On every other device, just run **Authenticate**.

Accounts are per server. Existing `sync.koreader.rocks` credentials do not exist on the CrossPoint server; either sign up again with the same username/password or use Option B to keep using the legacy server.

##### Option B: Legacy Public KOReader Server (`sync.koreader.rocks`)

Use this if you already sync KOReader devices against the official public server.

1. On each CrossInk device:
   - Go to **Settings -> System -> KOReader Sync**.

   - Set **Sync Server URL** to `https://sync.koreader.rocks` (required; an empty URL now points at the CrossPoint server instead).

   - Set **Username** and **Password** to your existing KOReader Sync credentials.

   - Run **Authenticate**.

2. If you do not have an account yet, run **Sign Up** on the device, or register once with curl:

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "https://sync.koreader.rocks/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

When this returns `HTTP 402` with `{"code":2002,"message":"Username is already registered."}`, pick a different username or use that existing account.

##### Option C: Self-Hosted Server (Docker Compose)

1. Start a sync server:

```bash
mkdir -p kosync-quickstart
cd kosync-quickstart

cat > compose.yaml <<'YAML'
services:
  kosync:
    image: koreader/kosync:latest
    ports:
      - "7200:7200"
      - "17200:17200"
    volumes:
      - ./data/redis:/var/lib/redis
    environment:
      - ENABLE_USER_REGISTRATION=true
    restart: unless-stopped
YAML

# Docker
docker compose up -d

# Podman (alternative)
podman compose up -d
```

> [!NOTE]
> `ENABLE_USER_REGISTRATION=true` is convenient for first setup. After creating your users, set it to `false` (or remove it) to avoid unexpected registrations.

2. Verify the server:

```bash
curl -H "Accept: application/vnd.koreader.v1+json" "http://<server-ip>:17200/healthcheck"
# Expected: {"state":"OK"}
```

3. Register a user once.
   CrossInk authenticates against KOReader Sync (`koreader/kosync`) using an MD5 key, so register using the MD5 of your password:

> [!WARNING]
> Sending a reusable MD5-derived password over plain HTTP is insecure.
> Create unique sync-only credentials and do not reuse main account passwords.
> Prefer `https://<server-ip>:7200` whenever traffic leaves a fully trusted LAN or when using untrusted networks.
> Use `curl -k` only for self-signed certificate testing.

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "http://<server-ip>:17200/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

If this returns `HTTP 402` with `{"code":2002,"message":"Username is already registered."}`, the account already exists.

4. On each device:
   - Go to **Settings -> System -> KOReader Sync**.

   - Set **Username** and **Password** (enter the plain password; CrossInk computes MD5 internally, and use the same values on all devices).

   - Set **Sync Server URL** to `http://<server-ip>:17200`.

   - Run **Authenticate**.

If you use the HTTPS listener, use `https://<server-ip>:7200` (`curl -k` only for self-signed certificate testing).

##### Syncing While Reading

Once any of the options above is set up, press **Confirm** while reading to open the reader menu, then select **Sync Progress**. Alternatively, set **Settings -> Controls -> Long-press Menu** to **KOSync** and hold Confirm to launch sync directly.

- With **Sync Behavior** set to **Ask every time**, choose **Apply Remote** to jump to remote progress or **Upload Local** to push current progress.
- With **Sync Behavior** set to **Smart sync**, CrossInk auto-resolves simple cases: upload when no remote progress exists, confirm and leave both unchanged when local and remote progress are already synchronized, upload when local progress is further ahead, or apply remote when remote progress is further ahead.

### 3.7 Sleep Screen

The **Sleep Screen** setting controls what is displayed when the device goes to sleep:

| Mode               | Behavior                                                                                                                                               |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **Dark** (default) | The CrossInk logo on a dark background.                                                                                                                |
| **Light**          | The CrossInk logo on a white background.                                                                                                               |
| **Custom**         | A custom image from the SD card (see below). Falls back to **Dark** if no custom image is found.                                                       |
| **Cover**          | The cover of the currently open book. Falls back to **Dark** if no book is open.                                                                       |
| **Cover + Custom** | The cover of the currently open book, shown only while actively reading. Falls back to **Custom** behavior when not reading.                           |
| **Page Overlay**   | Keeps the current reader page visible and draws a sleep wallpaper over it. If no wallpaper is available, the page remains visible while reading.       |
| **Minimal**        | A compact sleep screen based on the Minimal home layout.                                                                                               |
| **Minimal Stats**  | A compact sleep screen with recent reading stats, on supported devices; falls back to **Minimal** while tracking is off for the device or recent book. |
| **None**           | A blank screen.                                                                                                                                        |

#### Cover settings

When using **Cover** or **Cover + Custom**, two additional settings apply:

- **Sleep Screen Cover Mode**: **Fit** (scale to fit, white borders) or **Crop** (scale and crop to fill the screen).
- **Sleep Screen Cover Filter**: **None** (grayscale), **Contrast** (black & white), or **Inverted** (inverted black & white).

#### Custom images

To use custom sleep images, set the sleep screen mode to **Custom**, **Cover + Custom**, or **Page Overlay**, then place images on the SD card:

- **Multiple Images (recommended):** Create a `.sleep` directory in the root of the SD card and place any number of `.bmp` images inside. In **Page Overlay** mode, `.png` images are also supported. One image will be randomly selected each time the device sleeps. (A directory named `sleep` is also accepted as a fallback.)
- **Single Image:** Place a file named `sleep.bmp` in the root directory. In **Page Overlay** mode, `sleep.png` is also supported. These files are used as fallbacks if no valid image is found in the `.sleep`/`sleep` directory.

In **Page Overlay** mode, white BMP pixels and transparent PNG pixels let the current reader page show through; the remaining wallpaper pixels are drawn over the page. PNG wallpapers are supported only in this mode.

> [!TIP]
> For best results:
>
> - Use uncompressed BMP files, preferably with 4-bit or 8-bit indexed BMP.
> - X4: Use a resolution of 480x800 pixels to match the device's screen resolution.
> - X3: Use a resolution of 528x792 pixels to match the device's screen resolution.

> [!TIP]
> You can set an image as the sleep screen cover directly from the BMP image viewer in the **[Browse Files](#33-browse-files-screen)** screen.

---

### 3.8 Boot Screen

The **Custom Boot Screen** toggle is in **Settings -> System** and is enabled by
default. When it is on, you can replace the default CrossInk logo shown during
a cold boot with BMP images stored on the SD card. A configured custom boot
screen is also shown after a power-button wake. This is separate from the
sleep screen.

CrossInk supports two ways to choose a custom boot screen:

- **One fixed image:** In **[Browse Files](#33-browse-files-screen)**, open a
  BMP image from any folder, open its context menu, and choose **Set Boot
  Screen**. This selected image always takes priority over a boot-screen
  folder. To stop using it, open the same image and choose **Clear Boot Screen**.

- **Rotating images:** Create a `/.bootscreen` folder in the root of the SD card
  and place BMP files directly inside it. CrossInk randomly selects an image
  each time it starts and avoids recently used images where possible. A
  `/bootscreen` folder is also supported. Folder names are case-insensitive; if
  both folders exist, `/.bootscreen` takes priority.

CrossInk caches folder contents for faster startup. If you add BMP files
directly to an already-used folder while the device is off, the new files may
not be selected until the index is rebuilt. Upload them through the **File
Transfer** web file manager or **Nearby File Transfer** to invalidate the
index automatically.

The selection and fallback order is:

1. A selected BMP set with **Set Boot Screen**.
2. A usable BMP selected from the active root-level boot-screen folder.
3. The standard CrossInk logo.

On a cold boot, if a selected image or the active folder is missing, unreadable,
empty, or contains no usable BMP, CrossInk falls back to the next option in the
order above. If both folder names exist, `/.bootscreen` masks `/bootscreen`
even when the hidden folder is empty or unusable; remove or rename it to use
`/bootscreen` instead.

On a power-button wake, an existing selected BMP or boot-screen folder (even
an empty or unusable folder) runs the boot-screen flow and then falls back to
the standard logo if no image can be shown. If the selected file was removed
and no boot-screen folder exists, the wake is splashless and keeps the current
sleep screen visible. With no selected image or boot-screen folder, a
power-button wake is also splashless.

> [!TIP]
> Use an uncompressed BMP at your device's screen resolution for the best result: 480x800 pixels on X4 or 528x792 pixels on X3. Images with other dimensions are centered and scaled down as needed.

---

### 3.9 Custom Fonts (SD Card)

CrossInk supports loading additional fonts from the SD card, extending beyond the built-in Lexend Deca and Bitter families. Custom fonts can include extended Unicode coverage, enabling CJK (Chinese, Japanese, Korean) and other scripts.

There are three ways to install fonts:

1. **Manual SD card copy (recommended):** Download font files from [Inky](https://inky.crossink.dev/#downloads) and copy them to `/.fonts/` (preferred) or `/fonts/` on your SD card.
2. **Download from device:** Go to **Settings -> Reader -> Font Options -> Manage Fonts**, browse the available font families, and select one to download over Wi-Fi. Note that downloading over Wi-Fi can be unstable. If you experience download errors, please use one of the alternate methods.
3. **Upload via web interface:** While in **File Transfer** mode, open the web UI in a browser and navigate to the **Fonts** tab to upload `.cpfont` files. For supported devices, `.ttf` font files can also be uploaded.

Once installed, custom fonts appear in **Settings -> Reader -> Font Options -> Font Family** alongside the built-in fonts.

See [SD Card Fonts](./sd-card-fonts.md) for full installation details and SD card folder structure.

---

## 4. Reading Mode

Once you have opened a book, the button layout changes to facilitate reading.

### Page Turning

| Action            | Buttons                       |
| ----------------- | ----------------------------- |
| **Previous Page** | Press **Left** _or_ **Up**    |
| **Next Page**     | Press **Right** _or_ **Down** |

Change the individual side-button actions in **Settings > Controls > Side Buttons**.

If the **Short-press Action** setting is set to "Next Page", you can also turn to the next page by briefly pressing the Power button.

### Chapter Navigation

- **Next Chapter:** Press and **hold** the **Right** (or **Down**) button briefly, then release.
- **Previous Chapter:** Press and **hold** the **Left** (or **Up**) button briefly, then release.

These are the default chapter-skip actions. Change front-button long presses in
**Settings > Controls > Front Buttons**, or assign each side-button hold
separately in **Settings > Controls > Side Buttons**.

### Auto Page Turn

Auto Page Turn automatically advances pages at a set interval, useful for hands-free reading. This feature can be enabled and configured from the **[Reader Menu](#5-reader-menu)** while reading an EPUB.

### Tilt Page Turn (X3 and Sticky)

On the **Xteink X3** and **Sticky**, the gyroscope can be used to turn pages by tilting the device. This feature and its left-right or forward-back direction are available in **Settings -> Controls**.

### Touch Reader Controls

On supported touchscreen devices, touch page turns are enabled by default.
**Next Page** and **Previous Page**, in **Settings > Controls > Taps & Gestures**,
are configured independently and both default to **Tap & Swipe**:

| Option           | Taps                             | Swipes   |
| ---------------- | -------------------------------- | -------- |
| **Tap & Swipe**  | Enabled                          | Enabled  |
| **Tap Only**     | Enabled                          | Disabled |
| **Swipe Only**   | Disabled                         | Enabled  |
| **Inverted Tap** | Enabled, with reversed tap zones | Disabled |
| **Disabled**     | Disabled                         | Disabled |

Swipe left for the next page when **Next Page** allows swipes, or swipe right
for the previous page when **Previous Page** allows swipes. In right-to-left
EPUBs, normal tap zones and horizontal swipe directions follow the book's
reading direction instead. In left-to-right books, when both directions
allow taps, the normal zones are the left third for the previous page and the
right two-thirds for the next page. If either of those settings is **Inverted
Tap**, the shared zones become the left two-thirds for the next page and the
right third for the previous page. If only one direction allows taps, taps
across the page turn in that direction. The top and bottom gesture bands are
reserved for vertical gestures, so taps in those bands do not turn pages.

For **EPUB readers**, vertical gestures depend on the device:

- On **Sticky**, swipe up to open the reader menu. Swipe down to open the
  reader-details/frontlight panel; use that panel's header to return Home.
- On **X4 Pro**, swipe up to open the reader menu and swipe down to open the
  frontlight panel, which also shows chapter pages and book progress while
  reading. The capacitive Home key returns Home on a short press and
  opens the reader menu on a long press by default. Configure these actions, or
  disable the key while reading, in **Settings > Controls > Home Button**.
- On other touchscreen devices, swipe down to open the reader menu and swipe up
  to return Home.

**XTC** and **TXT** readers use narrower, format-specific vertical routing. For
example, Sticky TXT has the down-swipe reader-details/frontlight panel but no
swipe-to-menu or swipe-to-Home action; on X4 Pro, XTC keeps the swipe-up reader
menu while its down-swipe frontlight panel is top-edge only, and TXT has no
swipe-to-menu action. These vertical gestures are separate from the page-turn
settings above.

Choose **Disabled** in **Next Page** or **Previous Page** to stop touch page
turns in that direction without disabling the touchscreen's vertical reader-menu
or frontlight gestures. **Disable Touchscreen**, in **Settings > Reader**,
prevents screen-touch input while a book is open but keeps it available in
reader menus. For the different touch selection gestures used by [dictionary
lookup](./dictionary.md#looking-up-a-word) and [clippings](./reader-features.md#clippings-and-highlights), see those feature guides.

On devices with multi-touch support, you can also assign actions to two-finger
swipes from **Settings > Controls > Taps & Gestures > Two-finger Swipe**. Set an
action for **Swipe Up**, **Swipe Down**, **Swipe Left**, or **Swipe Right**, then
move two fingers together in that direction while reading. Available actions
are **Not Set**, **Increase Brightness**, **Decrease Brightness**, **Increase
Warmth**, **Decrease Warmth**, **Next Chapter**, **Previous Chapter**, **Increase
Font Size**, and **Decrease Font Size**. Brightness and warmth options appear
only when the hardware supports them; chapter options apply to EPUBs, and font
size options apply to EPUB and TXT books. Each direction starts as **Not Set**,
and each action can be assigned to only one direction; choosing it again moves
it to the new direction. On image-based XTC books, chapter and font-size actions
are consumed but cannot change the pre-rendered pages. See [Two-finger Swipe
Actions](./controls.md#two-finger-swipe-actions) for the complete list and
reader-specific limitations.

On supported multi-touch devices, enable **Pinch to Resize Font** in the same
menu, then move two fingers apart to increase the font or together to decrease
it. Each completed pinch changes one available font-size step. Pinch resizing
works in EPUB and TXT readers; XTC pages are pre-rendered and cannot be
resized. Pinch input also requires reader touch input to remain enabled.

### Slider Controls

For sliders with five-unit increments in the Reader Menu and Settings, tapping
the slider track rounds the selected value to the nearest multiple of five.

### Footnote Navigation

When reading an EPUB that contains footnotes, tap a reference or choose
**Footnotes** from the Reader Menu's **More** tab (or an assigned shortcut).
If there is one footnote, it opens directly. With several visible references,
CrossInk highlights them on the reading page: use the direction buttons to
choose one and **Confirm** to open it, or tap the reference. Links without a
visible target use a list instead. Press **Back** to return to your original
reading position.

After following an in-book link to a full chapter, sleeping or closing the book reopens on the last page read. Back retains the three most recent link origins across reopening and KOReader sync. If you close a transient footnote preview, the book resumes on the page that opened that preview, with any earlier link origins still available through Back.

### System Navigation

- **Return to Home:** Press the **Back** button to close the book and return to the **[Home](#31-home-screen)** screen.
- **Return to Browse Files:** Press and hold the **Back** button to close the book and return to the **[Browse Files](#33-browse-files-screen)** screen.
- **Reader Menu:** Press **Confirm** to open the **[Reader Menu](#5-reader-menu)**, which includes chapter navigation, reading options, and more.

### Supported Languages

CrossInk renders text using the following Unicode character blocks, enabling support for a wide range of languages:

- **Latin Script (Basic, Supplement, Extended-A/B):** Covers English, German, French, Spanish, Portuguese, Italian, Dutch, Swedish, Norwegian, Danish, Finnish, Polish, Czech, Hungarian, Romanian, Slovak, Slovenian, Turkish, Catalan, and others.
- **Cyrillic Script (Standard and Extended):** Covers Russian, Ukrainian, Belarusian, Bulgarian, Serbian, Macedonian, Kazakh, Kyrgyz, Mongolian, and others.
- **Vietnamese:** Supported via extended Latin glyph coverage in the built-in reader fonts.

What is not supported with built-in reader fonts: Chinese, Japanese, Korean, Arabic, Greek, Hebrew, and Farsi. However, **CJK, Hebrew, Greek, and other extended scripts can be enabled by installing custom SD card fonts** — see [Custom Fonts (SD Card)](#39-custom-fonts-sd-card).

---

## 5. Reader Menu

Press **Confirm** while reading to open the Reader Menu. From here you can access reading utilities and navigation options without leaving the book.

EPUB books use the same five icon tabs on touch and button devices:

| Tab (icon)               | Main options                                                                                                                                                    |
| ------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Font** (letters)       | Reader Font, Dictionary Font, Line/Word Spacing, text anti-aliasing, Focus Reading, Guide Dots                                                                  |
| **Layout** (text lines)  | Margins, Orientation, Alignment, Images, Hyphenation, Publisher Page Numbers, paragraph spacing and publisher styling                                           |
| **More** (three dots)    | Word lookup, Select Chapter, Go to %, Go to Stable Page when available, Auto Page Turn, Footnotes; Reading Stats on button devices when tracking is enabled     |
| **Bookmarks** (bookmark) | Add/remove and view bookmarks, create/view clippings, screenshot, position QR; Sync Progress, Nearby Position Sync, and Send Nearby Book on button devices      |
| **Settings** (gear)      | Status Bars, Controls, Book Dictionary, EPUB Render Mode, Indexing Method, finished status, Track Reading Stats, cache/stats resets, Reset Book Reader Settings |

Some actions appear only when the book or device supports them. On touch
screens, tap a tab and then an option. On button devices, **Left/Right** switch
tabs, **Up/Down** select rows, and **Confirm** opens the selected option. Font,
spacing, and margin changes have a live preview. Global settings use the same
Left/Right tab and Up/Down row navigation.

**Reset Book Reader Settings** restores the current EPUB's inherited global
reader defaults without deleting progress, bookmarks, clippings, or stats.
**Delete Book Cache** rebuilds cached book data and keeps those reader choices.

TXT and XTC books have their own menus and a smaller set of options; the
five-tab layout above is for EPUBs. For stats tracking, use **Track Reading
Stats** in the EPUB **Settings** tab or the XTC Reader Menu. For format-specific
features, see [Reader Features](./reader-features.md).

Press **Back** at any time to close the menu and return to your current page.

### 5.1 Chapter Selection

In EPUB books, open the Reader Menu's **More** tab and select **Select Chapter**.

1. Use **Left** (or **Up**), or **Right** (or **Down**) to highlight the desired chapter.
2. Press **Confirm** to jump to that chapter.
3. _Alternatively, press **Back** to cancel and return to your current page._

---

### 5.2 Bookmarks

Bookmarks can be created to quickly save and restore your place in a book.

To create a bookmark, hold **Confirm** for 1 second while inside a book. A popup will appear letting you know a bookmark was created. The popup message will automatically disappear in a couple of seconds.

To open bookmarks, press **Confirm** while inside a book. Then navigate to the **Bookmarks** menu. Bookmarks can be opened by navigating to them and pressing **Confirm**, which will redirect you to that place in the book. You can delete bookmarks by holding **Confirm** for 1 second, and then pressing **Confirm** again to confirm deletion, or **Back** to cancel.

Bookmarks are stored as per-book `.bin` files in the `.crosspoint/bookmarks` folder.

### 5.3 Dictionary

Dictionary lookup supports word selection, recent per-book history, chained lookups from definitions, and per-book dictionary overrides. See the [Dictionary guide](./dictionary.md) for installation and preparation instructions.

## 6. Current Limitations & Roadmap

Please note that this firmware is currently in active development. The following features are **not yet supported** but are planned for future updates:

- **Cover Images:** Large cover images embedded into EPUB can take several
  seconds to convert for the sleep screen and home-screen thumbnail. Use the
  built-in [EPUB optimization](./webserver.md#epub-optimization) before upload
  if a book is slow or memory-sensitive.
- **Unsupported Image Formats:** Most JPG and PNG images in EPUBs render correctly. GIFs and progressive JPEGs are not supported and will fall back to an `[Image]` placeholder.

---

## 7. Troubleshooting Issues & Escaping Bootloop

If an issue or crash is encountered while using CrossInk, feel free to raise an issue ticket and attach the logs.

**Crash reports on SD card:** After a crash, CrossInk automatically saves a crash report to the SD card (no USB connection needed). Check the root of the SD card for a crash log file and include it with any bug report.

**Serial monitor logs:** For more detailed debugging, connect the device to a computer and run the custom debugging monitor script (requires Python 3 with `pyserial`, `colorama`, and `matplotlib`; install via `pip3 install pyserial colorama matplotlib`):

```
python3 scripts/debugging_monitor.py
```

The script auto-detects the serial port. You can also specify one explicitly:

```
python3 scripts/debugging_monitor.py /dev/ttyACM0        # Linux
python3 scripts/debugging_monitor.py /dev/tty.usbmodem1  # macOS
python3 scripts/debugging_monitor.py COM7                # Windows
```

**Features:**

- Color-coded log output by category (errors, memory, display, EPUB parsing, etc.)
- Live memory usage graph (free RAM, total RAM, max contiguous allocation) updated every second
- Interactive command prompt — type a command and press Enter to send it to the device
- Screenshot capture — saves the current display to `screenshot.bmp` when triggered by the device

**Options:**

| Option               | Description                                               |
| -------------------- | --------------------------------------------------------- |
| `--baud RATE`        | Baud rate (default: 115200)                               |
| `--filter KEYWORD`   | Show only lines containing the keyword (case-insensitive) |
| `--suppress KEYWORD` | Hide lines containing the keyword (case-insensitive)      |

**Examples:**

```
# Show only memory-related log lines
python3 scripts/debugging_monitor.py --filter MEM

# Hide noisy SD card log lines
python3 scripts/debugging_monitor.py --suppress "[SD]"
```

Press **Ctrl-C** or close the graph window to exit.

If the device is stuck in a bootloop, press and release the Reset button. Then, press and hold on to the configured Back button and the Power Button to boot to the Home Screen.

There can be issues with broken cache or config. In this case, delete the `.crosspoint` directory on your SD card (or consider deleting only `settings.json`, `state.json`, or `epub_*` cache directories in the `.crosspoint/` folder).
