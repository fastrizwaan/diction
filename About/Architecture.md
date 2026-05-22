# Architecture Overview

Diction utilizes a high-performance C-based engine specifically designed for concurrent dictionary processing and zero-latency rendering. The architecture strictly separates the backend data extraction layer from the GTK/WebKit presentation layer.

## Core Data Structures
At the heart of the engine are three primary components:

1. **`DictEntry`**: A linked-list node representing an active dictionary in the user's workspace. It holds metadata (name, format, status) and a reference to the underlying `DictMmap` payload.
2. **`DictMmap`**: The core data access object for a single dictionary. It manages file descriptors, memory-mapped pointers, SQLite connections (for the FTS database), and format-specific extraction contexts (like `MdxContext` for MDX, or `DictZip` for `.dz` files).
3. **`FlatIndex`**: A highly optimized binary-searchable structure that maps normalized headwords to definition offsets within the raw dictionary file.

```text
                           +------------------------+
                           |  UI Layer (GTK4/Adw)   |
                           +-----------+------------+
                                       |
                                       v
                           +------------------------+
                           | Background GTasks pool |
                           +-----------+------------+
                                       |
          +----------------------------+-----------------------------+
          |                            |                             |
          v                            v                             v
 +------------------+         +------------------+          +------------------+
 |    DictEntry     | ------> |    DictEntry     | -------> |    DictEntry     |
 | (Dictionary 1)   |         | (Dictionary 2)   |          | (Dictionary N)   |
 +--------+---------+         +--------+---------+          +--------+---------+
          |                            |                             |
          v                            v                             v
 +------------------+         +------------------+          +------------------+
 |    DictMmap      |         |    DictMmap      |          |    DictMmap      |
 |  (File Access)   |         |  (File Access)   |          |  (File Access)   |
 +--------+---------+         +--------+---------+          +--------+---------+
          |
  +-------+-------+
  v               v
FlatIndex      SQLite FTS
(.index)       (.fts)
```


## Concurrency Model
Diction is designed to handle hundreds of dictionaries simultaneously without ever blocking the GTK main UI thread. It achieves this via a robust asynchronous threading model powered by GLib's `GTask`.

### Async Background Workers
When the user triggers an action (like typing in the search bar or clicking a link), the engine spawns detached `GTask` threads.
- **`sidebar_search_task_func`**: Scans the `FlatIndex` of all loaded dictionaries to rapidly populate the left-hand sidebar with prefix and fuzzy matches.
- **`search_webview_task_thread`**: Extracts the actual raw HTML definitions from the archives, normalizes them, injects the user's custom CSS, and prepares the final HTML payload for WebKit.

### Thread Safety and Mutexes
Because background threads run concurrently, they inevitably attempt to access the same dictionary files on disk. Thread safety is enforced via strategic locking:
- **`dict_loader_mutex`**: A global lock that protects the `all_dicts` linked list. Any thread that needs to iterate over loaded dictionaries must lock this mutex, take a reference (`dict_entry_ref`), and unlock before proceeding with expensive I/O.
- **`DictZip` Mutex (`dz_mutex`)**: Compressed dictionary archives (like `.dict.dz`) require block-level `fseek`, `fread`, and `zlib` decompression. To prevent concurrent threads from corrupting the decompression state or the zlib stream, every `DictZip` struct has an internal mutex that guarantees safe atomic access to its LRU cache and disk I/O operations.
- **SQLite Mutexes**: FTS database connections (`stmt_get`, `stmt_search`) inside `FlatIndex` are protected by internal mutexes, allowing multiple threads to safely issue SQLite steps simultaneously.

## Data Extraction Pipeline
Diction avoids loading massive files into RAM. Instead, it relies on lazy-loading and memory-mapping (`mmap`).
1. When a word is requested, the engine queries the `FlatIndex` to retrieve the `offset` and `length` of the definition.
2. The `dict_get_definition` dispatcher routes the request based on the archive format:
   - For uncompressed `.dsl` or `.dict`, it simply returns a direct pointer to the `mmap` payload `(dict->source_mmap + offset)`.
   - For `.dz` files, it calls `dictzip_read` which safely decompresses the specific chunk required.
   - For `.mdx`, it utilizes `mdx_get_definition_on_the_fly` with thread-safe `pread()` calls to extract compressed blocks instantly.
3. The raw output is verified for valid UTF-8 and passed to the WebKit rendering pipeline.
