# Diction

**A high-performance, multi-format offline dictionary.**

Diction is a fast, lightweight, and feature-rich dictionary application built in C using GTK4, Libadwaita, and WebKitGTK. It provides a seamless and beautiful user interface for looking up words across multiple offline dictionary formats.

## Features

- **Multiple Formats Supported:** Read dictionary files in **MDX**, **SLOB**, **BGL** (Babylon), and **IFO** (StarDict) formats natively without manual conversion.
- **Modern User Interface:** Built with GTK4 and Libadwaita, featuring responsive design, dark mode support, and a sidebar navigation system.
- **Fast Search:** Features multi-bucket fuzzy text searching for exact, suffix, prefix, phrase, and substring matches.
- **Global Dictionary Scan:** Press `Ctrl+Alt+d` (or custom keyboard shortcut) to trigger a global scan popup that looks up text from your clipboard instantly.
- **Tray Icon Support:** Can run optionally in the background via the system tray, providing quick access whenever needed.
- **Audio Pronunciations:** Built-in audio player support via Gstreamer, FFmpeg, and other backends down to raw PCM, capable of playing dictionary audio `.spx`, `.ogg`, and `.oga` files.
- **User History & Favorites:** Tracks your search history and allows you to bookmark favorite words for later reference.
- **Random Word:** Explore new vocabulary with the integrated random word button.
- **Render Engine:** Utilizes WebKitGTK to render dictionary definitions flawlessly, retaining the original DSL-like markdown or web styling.

## Dependencies

The project uses the Meson build system and requires the following libraries and tools to be installed on your system:

- C Compiler (e.g., `gcc` or `clang`)
- `meson` and `ninja-build`
- `gtk4-devel`
- `libadwaita-devel`
- `webkitgtk6.0-devel` (or `webkit2gtk4.1-devel`)
- `glib2-devel` and `json-glib-devel`
- Archiving & Compression libraries: `zlib-devel`, `libarchive-devel`, `libzstd-devel`, `xz-devel` (liblzma), and `bzip2-devel`.

*On Fedora Silverblue/Kinoite, you can use `rpm-ostree install` with the above dependencies.*

## Build and Install

Diction uses `meson` and `ninja` for compilation:

```bash
# 1. Setup the build directory
meson setup build

# 2. Compile the application
ninja -C build

# 3. Install the application (and its desktop shortcuts)
sudo ninja -C build install
```

This will also automatically install the necessary desktop integration files (.desktop, dbus services, SVG icons) to standard system directories. You can also run the provided `install.sh` script to streamline this process.

### Uninstall
If you need to remove the application from your system:

```bash
sudo ninja -C build uninstall
```

## Usage

Simply run `diction` from your command line, or find **Diction** in your application launcher.

Once inside the app:
1. Load your directory containing the dictionary files via user settings. The app will automatically scan for supported formats (`.mdx`, `.slob`, `.bgl`, `.ifo`).
2. Search through the indexed dictionaries using the search bar.
3. Access your favorites or history from the sidebar.
4. Enjoy fast, offline definition access!
