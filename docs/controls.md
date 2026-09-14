---
title: Controls
nav_order: 6
---

# Controls

The Controls menu lets you customize front buttons, side buttons, and reader shortcuts.

## Settings Menu Layout

### Power Button

- Short-press action
- Long-press action
- Power + Up action

### Front Buttons

- Remap front buttons
- Remap front buttons while reading
- Orientation aware
- Long-press behavior (in-reader only)
- Long-press back action (in-reader only)
- Long-press menu action (in-reader only)

Note: Even though some actions assigned to the front buttons could be used globally, they are restricted to apply within the reader only due to the dynamic nature of the front buttons (they can mean different things based on the screen you're on).

### Side Buttons

- Layout
- Orientation aware
- Long-press action

### Taps & Gestures

- Next Page
- Previous Page
- Pinch to Resize Font (on supported multi-touch devices)
- Tap to Hide Status Bar
- Two-finger Swipe (on supported multi-touch devices)

### Next Page and Previous Page Gestures

On touchscreen devices, **Next Page** and **Previous Page** are in
**Settings > Controls > Taps & Gestures**. They change page-turn gestures while
reading and can be configured independently. **Next Page** controls taps and
left swipes; **Previous Page** controls taps and right swipes.

Both settings offer the same options:

| Option | Taps | Swipes |
| ------ | ---- | ------ |
| **Tap & Swipe** (default) | Enabled | Enabled |
| **Tap Only** | Enabled | Disabled |
| **Swipe Only** | Disabled | Enabled |
| **Inverted Tap** | Enabled, with reversed tap zones | Disabled |
| **Disabled** | Disabled | Disabled |

When both directions allow taps, the normal zones are the left third for the
previous page and the right two-thirds for the next page. If either of those
settings is **Inverted Tap**, the shared zones become the left two-thirds for
the next page and the right third for the previous page. If only one direction
allows taps, taps across the page turn in that direction. The top and bottom
gesture bands are reserved for vertical gestures, so taps there do not turn
pages. A **Previous Page** setting that does not allow swipes also prevents a
rightward edge swipe from being treated as Back/Home while reading.

## Pinch to Resize Font

On touchscreen devices with multi-touch support, enable **Pinch to Resize Font**
in **Settings > Controls > Taps & Gestures**. While reading an EPUB or TXT
book, move two fingers apart to increase the font or together to decrease it.
Each completed pinch changes one available font-size step. Pinch resizing also
requires **Touch Reader Controls** to be enabled. XTC pages are pre-rendered,
so this option cannot resize them.

## Tap to Hide Status Bar

**Tap to Hide Status Bar** is enabled by default. When it is enabled, tap the
visible status-bar area while reading to show or hide the entire bar for the
current reading session. The toggle does not change page layout or page breaks;
tap the same status-bar region again to restore a hidden bar. Use **Customize
Status Bar** to choose which items the bar contains. The tap is available while
**Touch Reader Controls** is enabled. For XTC books, when an XTC status bar is
enabled, tap its configured top or bottom edge both to hide and to restore it.

## Two-finger Swipe Actions

On touchscreen devices with multi-touch support, open **Settings > Controls >
Taps & Gestures > Two-finger Swipe** to assign actions to the four swipe
directions. During reading, place two fingers on the page and move them
together in the configured direction. Use **Not Set** to leave a direction
unassigned.

Each action can be assigned to only one direction. If you assign an action to a
new direction, CrossInk clears its previous direction automatically.

Available actions depend on the device and reader:

| Action | Availability |
| --- | --- |
| Increase Brightness / Decrease Brightness | Devices with a frontlight |
| Increase Warmth / Decrease Warmth | Devices with an adjustable warm/cool frontlight |
| Next Chapter / Previous Chapter | EPUB readers |
| Increase Font Size / Decrease Font Size | EPUB and TXT readers |

Use a clear, mostly straight motion so both contacts are recognized as one
gesture. Configured two-finger swipes are handled separately from the ordinary
one-finger page-turn mapping. On image-based XTC books, chapter and font-size
actions are consumed but cannot change the pre-rendered pages.

## Side Button Long-press Action

When set to `Change Font Size`, hold a side button for about 2 seconds:

- Up increases font size
- Down decreases font size

When set to `Orientation Change`, hold a side button for about 2 seconds:

- Up cycles through the orientations in the following order: `Landscape CCW` -> `Inverted` -> `Landscape CW` -> `Portrait`
- Down cycles through the orientations in the following order: `Landscape CW` -> `Inverted` -> `Landscape CCW` -> `Portrait`

## Power, Back, and Menu Button Actions

Defaults:

- Short-press Power Button Action: Ignore
- Long-press Power Button Action: Sleep
- Long-press Back Button Action: Browse Files
- Long Press Menu Button Action: Ignore

Available actions include:

- Ignore
- Sleep
- Page Turn
- Refresh Screen
- Change Font
- Guide Dots
- Focus Reading
- Toggle Bookmark
- Sync Progress
- Mark as Finished
- Reading Stats
- Take Screenshot
- Auto Page Turn Interval
- File Transfer
- Calibre Wireless
- Join a Network
- Create Hotspot
- Tilt Page Turn (X3 only)
- Footnotes
- Dark Mode
- Browse Files
- Create Clipping
- Look Up Word
- Quick Lock
- Quick Actions
- Toggle Frontlight (on supported devices)
- Toggle Touchscreen (on supported devices)

## Power + Up Shortcut

The **Power + Up** shortcut runs the action selected in **Settings > Controls >
Power + Up**. It is disabled by default. Press the **Power** and **Volume Up**
buttons together to trigger it, then release both buttons before using another
shortcut.

The shortcut can run many of the same actions available for the Power button,
including **Quick Lock**. The existing **Power + Volume Down** screenshot
shortcut is unchanged.

## Quick Actions Triggers

In **Settings > Quick Actions**, assign the menu to one shortcut. Available
triggers include short- or long-press Power, **Power + Up**, and on X4 Pro,
**Tap Home**, **Long-Press Home**, or **Double Tap Home**. Selecting a trigger
there replaces any previous Quick Actions trigger; it does not change the five
actions in the menu.

## Quick Lock

**Quick Lock** temporarily disables normal button and touchscreen input while
leaving the current screen visible. A lock badge appears on the display while
the device is locked. It is useful when carrying the reader or setting it down
while reading.

Quick Lock can be assigned to **Power + Up**, short- or long-press **Power**,
or long-press **Back** or **Menu**. Trigger the assigned action to toggle the
lock. While locked, repeat that same shortcut to unlock; all other buttons,
including **Power + Volume Down** screenshots, and touchscreen input stay
locked.

Quick Lock still follows the regular **Time to Sleep** setting. If the timeout
expires, the device sleeps and restores the lock when it wakes. Reading timers
pause while Quick Lock is active.

## Footnote Shortcut

When a shortcut is mapped to Footnotes, the shortcut opens the footnotes submenu while reading. If the current page has only one footnote, CrossInk opens that referenced page directly.

The **Quick-return from Footnotes** setting controls whether the Power button acts like Back after opening a footnote page, making it faster to return to the original reading position.
