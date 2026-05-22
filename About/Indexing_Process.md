# The Indexing Process

To provide instantaneous search capabilities, Diction cannot scan raw dictionary archives on the fly. Instead, when a new dictionary is added, it must undergo a one-time indexing process. This process converts the dictionary into native high-speed cache files.

## Cache Files
For every loaded dictionary, Diction generates two core cache files in the `~/.cache/diction/` directory (or directly adjacent to the dictionary file if permissions allow):
1. **`.index` file**: A binary file containing a strictly alphabetically sorted `FlatIndex` mapping headwords to data offsets.
2. **`.fts` file**: An SQLite database utilizing the FTS5 extension to store full-text definition tokens.

```text
[Raw Dictionary (MDX, DSL, etc.)]
             |
      (Format Parser)
             |
     [Extract Headword & Body]
             |
      (Normalization) ---> (Lowercase, remove accents)
             |
             +-----------------------+
             |                       |
      [Sort by Key]            [Strip HTML]
             |                       |
             v                       v
      +-------------+          +-------------+
      |   .index    |          |    .fts     |
      | (FlatIndex) |          | (SQLite FTS)|
      +-------------+          +-------------+
```


## Phase 1: Ingestion and Parsing
The indexing process begins by identifying the dictionary format (MDX, DSL, StarDict, etc.) and invoking the appropriate parser.
The parser linearly scans the raw archive from start to finish. For every entry it finds, it extracts:
- The **Headword** (the word being defined).
- Any **Aliases** or **Synonyms** attached to that definition.
- The **Definition Offset and Length** (the exact byte location of the HTML/markup body in the file).

## Phase 2: Key Normalization
Before writing to the index, every headword undergoes strict normalization to ensure predictable searching:
1. **Unicode Decomposition**: Characters are fully decomposed to their base forms (e.g., `é` becomes `e` + ´).
2. **Diacritic Stripping**: Accents and marks are removed so that a user typing `cafe` will match `café`.
3. **Case Folding**: All text is converted to lowercase.
4. **Punctuation Stripping**: Spaces and dashes are removed, creating a single contiguous string (e.g., `garage sale` becomes `garagesale`).

## Phase 3: FlatIndex Generation
The normalized keys, along with their original display headwords and byte offsets, are sorted alphabetically in memory. This array is then dumped to the `.index` binary file. 
Because the `.index` file is perfectly sorted by the normalized key, Diction can use `mmap` to map this file directly into memory and perform O(log N) binary searches instantly, completely bypassing the need to parse the dictionary ever again.

## Phase 4: Full-Text Search Generation (Optional)
If FTS is enabled, the engine also strips HTML tags and markup from the definition bodies and pipes the raw text into an SQLite database (`.fts` file). The SQLite FTS5 extension tokenizes the text and builds an inverted index, allowing users to query any word occurring *within* a definition across millions of entries in milliseconds.
