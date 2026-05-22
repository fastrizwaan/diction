# StarDict Format

StarDict is one of the oldest open-source dictionary formats, known for its modular file structure. Diction provides full support for reading, indexing, and rendering StarDict archives natively.

## Architecture and Loading
A standard StarDict dictionary consists of three to four interrelated files:
1. **`.ifo` file**: The central configuration file. It contains key-value pairs specifying the dictionary's name, version, word count, and crucially, the `sametypesequence` which defines the data structure of the definition file.
2. **`.idx` file**: The binary index file containing a list of headwords, along with the byte offset and length of each definition in the `.dict` file.
3. **`.dict` or `.dict.dz` file**: The payload file containing the actual definition text. If it is a `.dz` file, Diction automatically routes access through the `DictZip` decompression layer.
4. **`.syn` file (optional)**: A synonym file that maps alternate headwords to the same definition data.

### The Indexing Phase
Because the `.idx` file already acts as a binary index, Diction's ingestion process is highly efficient. The engine parses the `.idx` file, reads the pre-computed offsets, applies its own Unicode normalization rules to the headwords, and dumps the resulting optimized structure into Diction's `.index` cache. If a `.syn` file exists, its aliases are also resolved and appended to the cache.

## `sametypesequence` Parsing and Rendering
StarDict definitions do not use a unified markup language. Instead, the `.ifo` file specifies a `sametypesequence` (e.g., `m` or `tm`), which dictates exactly how the raw bytes in the `.dict` file should be interpreted.

Diction's StarDict renderer (`dict-sdict.c`) loops over the sequence character by character:
- **`m` (WordNet/Text markup)**: The string is treated as plain text, but newlines and basic formatting are converted into HTML `<br>` tags.
- **`h` (HTML markup)**: The string is injected directly as HTML.
- **`x` (XDXF markup)**: The string is routed through Diction's XDXF XML-to-HTML parser.
- **`t` (Phonetic transcription)**: The string is wrapped in a `<div class="phonetic">` tag to ensure proper IPA rendering.

By iterating through these blocks and wrapping them in semantic HTML, Diction successfully transforms fragmented StarDict data blocks into cohesive HTML5 documents ready for WebKit.
