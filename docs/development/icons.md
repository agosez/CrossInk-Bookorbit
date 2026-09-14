---
title: Icon Libraries
nav_order: 4
---

# Icon Libraries

CrossInk rasterizes only the icons it uses into 1-bit C++ headers. This keeps
the firmware small enough for ESP32-C3 devices; adding an icon does not embed a
whole icon library.

The local manifests in `src/components/icons/` can use either source:

- **Lucide:** `freeink-sdk/libs/assets/Icons/lucide/icons`
- **Tabler outline:** `assets/tabler-icons/icons/outline`
- **Tabler filled:** `assets/tabler-icons/icons/filled`

Tabler is a CrossInk submodule pinned to a release commit. Clone it with the
other dependencies before generating icons:

```sh
git submodule update --init --recursive
```

## Generate an icon header

Use one library per manifest and generated header. Each manifest maps a C++
alias to an SVG filename without `.svg`:

```ini
tabler-settings = settings
```

Generate it with the firmware wrapper, which selects the SVG source and calls
the existing FreeInk icon generator:

```sh
python3 scripts/generate_icons.py --library tabler \
  --manifest src/components/icons/tablerIcons.manifest \
  --sizes 28 --out src/components/icons/tablerIcons.h
clang-format -i src/components/icons/tablerIcons.h
```

Use `--library lucide` for existing Lucide manifests. The generated `Icon`
objects are library-neutral FreeInk assets, so use them with
`freeink::ui::bitmapFromIcon()` exactly like the existing Lucide icons:

Use `--library tabler-filled` for filled Tabler icons:

```cpp
target.bitmap(rect, freeink::ui::bitmapFromIcon(icon_tabler_settings_28));
```

Do not edit generated `.h` files by hand. Add the SVG alias to its manifest,
regenerate the header, and include that header only in the translation units
that use its icons. This avoids duplicating bitmap data across the firmware.
