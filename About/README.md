# Diction

Diction is a modern, high-performance dictionary application built for Linux using GTK4, Libadwaita, and WebKitGTK. It is designed to handle hundreds of large dictionary archives simultaneously while providing instantaneous search results, rich HTML rendering, and full-text search capabilities.

## Supported Dictionary Formats
Diction is capable of loading, indexing, and rendering a wide variety of standard offline dictionary formats:
- **MDX/MDD** (MDict 2.0+ with RIPEMD-128 decryption and resource support)
- **DSL / DSL.DZ** (ABBYY Lingvo with extensive text-markup rendering)
- **StarDict** (.ifo / .dict.dz / .idx / .syn)
- **XDXF** (XML Dictionary Exchange Format)
- **BGL** (Babylon Glossary)
- **Slob** (Aard Dictionary format)
- **Dictd** (DICT protocol offline archives)
- **Diction HTML5** (The engine's native, highly optimized HTML format)

## Key Features
- **Instantaneous Search**: Relies on a highly optimized custom indexing engine utilizing memory-mapped binary search trees (O(log N)) and SQLite for zero-latency lookups across millions of entries.
- **Fuzzy Matching & Substrings**: Intelligent search candidate classification using Gestalt pattern matching algorithms to gracefully handle typos and substrings.
- **Full-Text Search (FTS)**: Built-in deep text search via SQLite FTS5, allowing users to query within the definition bodies across all loaded dictionaries.
- **Rich Media Rendering**: Leverages WebKitGTK with a custom `dict://` URI scheme handler to display images and play audio pronunciations extracted directly from `.mdd` or `.zip` resource files on-the-fly without dumping to disk.
- **Modern Adaptive UI**: Built natively for the GNOME desktop environment using Libadwaita, fully responsive, and supports dynamic light/dark mode with customized typography and CSS themes.

## Documentation Index
To understand the intricate internals of the Diction application, please explore the following documentation files:

1. [Architecture Overview](Architecture.md) - The high-level engine architecture and concurrency model.
2. [Indexing Process](Indexing_Process.md) - How dictionary archives are ingested, cached, and prepared for rapid querying.
3. [Search Process Overview](Search_Process.md) - The anatomy of a search request and how the engine aggregates data.
4. [Standard Search (Prefix/Exact)](Standard_Search.md) - The high-speed memory-mapped prefix lookup pipeline.
5. [Full-Text Search (FTS)](Full_Text_Search.md) - SQLite FTS5 integration for deep definition queries.
6. [UI & Rendering Pipeline](UI.md) - How GTK4, WebKit, and the custom DOM injection system present the final result to the user.
7. [System Integration](System_Integration.md) - How global keyboard shortcuts, clipboard monitoring (PRIMARY and CLIPBOARD selections), and the system tray (SNI/AppIndicator) are integrated.
8. [Rendering Internals](Rendering.md) - The unified format-to-HTML transformation and resource interception pipeline.

### Supported Formats Architecture
Explore how each specific dictionary format is parsed, indexed, decompressed, and rendered:
- [MDX/MDD (MDict 2.0+)](MDX.md)
- [DSL / DSL.DZ (ABBYY Lingvo)](DSL.md)
- [StarDict](StarDict.md)
- [XDXF](XDXF.md)
- [Slob](Slob.md)
- [BGL (Babylon)](BGL.md)
- [Dictd](Dictd.md)
