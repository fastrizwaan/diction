# Dictd Format

Dictd files are offline archives built for the classic DICT protocol server infrastructure. Diction supports reading `.index` and `.dict` (or `.dict.dz`) file pairs natively, allowing users to access these vast repositories without running a local dictd server.

## Architecture and Loading
A standard Dictd archive consists of two files:
1. **`.index` file**: A plain text file where each line contains a headword, followed by a base64-encoded file offset and length.
2. **`.dict` or `.dict.dz` file**: The payload file containing the plain-text definitions. It is heavily standardized to use `DictZip` compression, allowing random access decompression.

### The Indexing Phase
Diction's `dict-dictd.c` parser processes the `.index` file linearly. 
It splits each line by its tab delimiters, decodes the base64 offset and length values into standard 64-bit integers, applies the standard Unicode normalization to the headword, and dumps the resulting optimized structure into Diction's native `.index` flat file cache. 

Because the base64 decoding is computationally inexpensive, indexing Dictd archives is incredibly fast.

## Decompression and Rendering
When a definition is requested, Diction uses the decoded offset to jump into the `.dict` file. If the file is compressed (e.g., `.dict.dz`), the request is routed through the `dz_mutex` locked `dictzip_read` function, which decompresses the specific chunk required in milliseconds.

Dictd definitions are almost entirely plain text, formatted with hardline breaks and ASCII indentation. WebKitGTK collapses plain text whitespace, which would destroy the layout of Dictd files.
To prevent this, Diction's rendering engine automatically wraps the extracted UTF-8 payload in a `<pre class="dictd-pre">` tag. This ensures that the original ASCII formatting, alignment, and spacing are perfectly preserved while still inheriting Diction's custom font and color themes.
