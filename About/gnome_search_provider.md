# GNOME Activities Search Provider Integration

This document outlines the architecture, implementation details, and usage instructions for the GNOME Search Provider feature in **Diction**.

## Architecture Overview

To provide instant dictionary definitions from the GNOME Shell overview, Diction implements the `org.gnome.Shell.SearchProvider2` DBus interface.

Instead of keeping the entire GTK4 UI loaded in the background (which wastes system resources), Diction uses a **decoupled headless architecture**:

1. **`diction-search-provider` Binary**: A small, lightweight executable built using the core `engine_srcs`. It is compiled with `-DDICTION_NO_GTK` to entirely omit GUI components, resulting in minimal memory overhead and ultra-fast startup times.
2. **DBus Activation**: When a user types in the GNOME overview, GNOME Shell activates the `io.github.fastrizwaan.diction.SearchProvider` DBus service if it isn't already running.
3. **Lazy-Loading Index**: The provider loads user dictionaries (from `settings.json`) and queries the `.hw.sqlite` persistent indexes directly via the high-performance `flat_index_search_fast()` binary matching algorithm.
4. **App Invocation**: If the user selects a search result, the headless provider spawns the main `diction` application using the CLI arguments `--search "term"`.

## Implementation Details

### Configuration Files
- `data/org.gnome.Shell.SearchProvider2.xml`: Standard DBus interface required by GNOME Shell. Used by `gdbus-codegen` to generate C boilerplate bindings.
- `data/io.github.fastrizwaan.diction.search-provider.ini`: Placed in `~/.local/share/gnome-shell/search-providers/` to register the application with GNOME Shell.
- `data/io.github.fastrizwaan.diction.SearchProvider.service.in`: A DBus service file that directs DBus to launch `@bindir@/diction-search-provider` when the service name is called.

### DBus Methods (`src/search-provider.c`)
- **`GetInitialResultSet(terms)`**: 
  Concats the terms and performs a fast prefix scan on the loaded FlatIndex databases. Returns an array of string identifiers formatted as `dictionary_id::term`. Limit: 5 results.
- **`GetSubsearchResultSet(previous, terms)`**: 
  Currently redirects to `GetInitialResultSet` to refresh the query string.
- **`GetResultMetas(identifiers)`**:
  Receives the array of IDs from the result set. It queries the definition from the `.dict` payload, strips away HTML/formatting tags using a custom character scanner, truncates the string to 150 characters, and returns a metadata dictionary `{"id", "name", "description", "icon"}` to populate the GNOME UI.
- **`ActivateResult(identifier)`**:
  Parses the selected word and calls `g_spawn_command_line_async("diction --search <word>")`.
- **`LaunchSearch(terms)`**:
  Invoked when the user presses "Enter" on the Diction application icon inside the search provider results. Calls `diction --scan "<query>"`.

### Build System (`meson.build`)
The integration relies on `gnome.gdbus_codegen()` to parse the XML. The executable target links specifically against GLib, GIO, and all the parsing dependencies (Zlib, libarchive, etc.) without injecting `gtk4` or `libadwaita`.

## Performance & Scalability

Even if a user has hundreds of large dictionaries enabled, the search provider guarantees zero system slowdown or UI stuttering in the GNOME overview due to several architectural optimizations:

1. **Lazy Database Connections**: When the DBus provider wakes up, it does not load dictionary texts into RAM. It merely executes a zero-copy virtual memory map (`mmap`) of the dictionary files and opens connections to their `.hw.sqlite` index caches. Establishing hundreds of SQLite connections sequentially takes mere milliseconds and is imperceptible to human reaction times.
2. **Early-Exit Short-circuiting**: GNOME Shell only requests a handful of results to display in the overview. The provider is hard-coded to stop scanning dictionaries the moment it finds 5 matching results. If your query is satisfied by the first few dictionaries, the provider completely skips querying the remaining databases.
3. **The 10-Second Warm Buffer**: The background process includes a 10-second inactivity timeout. When a user types their first keystroke, the SQLite databases initialize. As they continue typing subsequent keystrokes, the provider is already warm, executing instantaneous binary searches across the open connections without any initialization overhead. Once the user stops searching, the timeout expires and kills the DBus daemon to free all resources.

## Usage & Testing

### Installation
After building the project, install it to ensure the `.ini` and `.service` files are correctly located:
```bash
ninja -C build install
```

### Restart GNOME Shell
GNOME Shell caches the search providers directory on startup. If you install, update, or remove the Diction Search Provider, GNOME Shell must be restarted to pick up the `.ini` file changes.
- **Wayland (Important)**: You **must log out and log back in**. GNOME Shell on Wayland cannot be restarted gracefully without ending your session.
- **X11**: Press `Alt+F2`, type `r`, and press Enter.

## Packaging and Flatpak Integration

### Installation Prefixes (`/usr` vs `/usr/local`)
The build system utilizes `@bindir@` inside the `io.github.fastrizwaan.diction.SearchProvider.service.in` file. When you compile and install Diction (whether prefixed to `/usr` or `/usr/local`), Meson automatically replaces `@bindir@` with the absolute path to your `diction-search-provider` executable. 
This means the DBus session daemon will always launch the correct binary corresponding to your installation.

### Flatpak Support
The search provider fully works in a Flatpak environment. 
- When built as a Flatpak, the `.ini` and DBus `.service` files are exported to the host system. 
- The DBus service executes the provider *inside* the Flatpak sandbox.
- **Priority**: If you have both a native system install (`/usr/local`) and a Flatpak install, the behavior depends on `XDG_DATA_DIRS`. Usually, local user installations (`~/.local/share/flatpak/...`) take precedence over system-wide installs. 
- **Conflicts**: It is highly recommended to **only have one version** (Flatpak or Native) installed at a time. If you install both, they compete for the same DBus name (`io.github.fastrizwaan.diction.SearchProvider`), which can lead to one version executing while the other is clicked.
- **Switching Versions**: If you uninstall the Flatpak and switch to native (or vice versa), you **must log out of Wayland and log back in** to ensure GNOME Shell and the DBus session daemon clear the cached paths and point to the correct newly installed service.

### Usage
1. Press the `Super` key to open the GNOME Activities overview.
2. Start typing a word (e.g., "hello").
3. Diction results will instantly populate alongside files and apps.
4. Click on a definition to launch Diction and view the complete article.

### Manual Testing with `gdbus`
You can query the provider manually to ensure it's responding correctly:
```bash
gdbus call --session \
  --dest io.github.fastrizwaan.diction.SearchProvider \
  --object-path /io/github/fastrizwaan/diction/SearchProvider \
  --method org.gnome.Shell.SearchProvider2.GetInitialResultSet \
  "['apple']"
```
