# Babylon Glossary (BGL) Format

BGL is the proprietary binary format used by the Babylon translation software. Due to its proprietary nature, its internal structure was reverse-engineered by the open-source community. Diction implements a custom BGL parser to ingest and render these archives.

## Architecture and Loading
A `.bgl` file is essentially a `gzip` compressed binary file with a custom header.
Because the entire dictionary is compressed as a single continuous block, random access (seeking directly to a word) is impossible without decompressing the whole file.

### The Indexing and Decompression Phase
To achieve zero-latency searches, Diction cannot decompress the BGL file on every query. Instead, it processes the BGL file entirely during the indexing phase.
1. Diction uses `zlib` to stream-decompress the `.bgl` file into a temporary memory buffer.
2. The scanner parses the custom binary nodes. Each entry block contains a headword, potentially several alternate headwords (aliases), and the HTML definition.
3. Because the uncompressed BGL data cannot be mapped directly from disk, Diction converts the BGL data into its own native uncompressed cache block or relies on its internal FTS database to store the strings if configured.

## Rendering and HTML Transformation
BGL files store their definitions in a primitive subset of HTML, often mixed with proprietary Babylon tags and escape sequences (e.g., `\x14` or `\x1e`).

Diction's `dict-bgl.c` renderer cleans the extracted payload:
- It strips out binary control characters and proprietary Babylon-specific metadata.
- It normalizes the legacy HTML tags to ensure they do not break WebKit's modern parser.
- The cleaned HTML is then routed through the standard CSS injection pipeline, allowing legacy Babylon dictionaries to inherit modern dark mode and font themes seamlessly.
