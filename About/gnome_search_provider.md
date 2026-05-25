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

## Usage & Testing

### Installation
After building the project, install it to ensure the `.ini` and `.service` files are correctly located:
```bash
ninja -C build install
```

### Restart GNOME Shell
GNOME Shell only reads the `search-providers` directory on startup.
- **X11**: Press `Alt+F2`, type `r`, and press Enter.
- **Wayland**: Log out and log back in.

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
