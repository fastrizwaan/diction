# Slob Format

Slob is a highly compressed, read-only dictionary format created for the Aard 2 dictionary reader. It is heavily utilized for large offline Wikipedia dumps. Diction fully supports reading and extracting data from `.slob` archives.

## Architecture and Loading
A Slob file is structured as a series of compressed chunks. It contains:
1. **Header**: Magic bytes and UUID.
2. **Tags/Metadata**: JSON-encoded metadata describing the dictionary's title, copyright, and content type.
3. **Blob Index**: A binary tree indexing the physical locations of compressed data blocks.
4. **Data Chunks**: The actual payload, typically compressed using `LZMA2` or `Bzip2`.

### The Indexing Phase
Unlike older formats, Slob already contains a highly optimized internal index. Diction's `dict-slob.c` parser navigates the Slob file's binary tree to extract all available headwords and aliases.
Instead of recording raw file offsets (since the data is locked inside LZMA2 chunks), Diction records the specific Slob `Blob ID` and relative item index into its own `.index` file.

## Decompression and Extraction
Because Slob chunks are often entirely self-contained HTML strings, the extraction process is different from MDX or StarDict.
When a definition is requested:
1. Diction uses the `Blob ID` from its `.index` cache to locate the exact compressed chunk in the `.slob` file.
2. It allocates a buffer and decompresses the chunk using the appropriate algorithm (`liblzma` for LZMA2, or `zlib` for DEFLATE).
3. The specific definition is extracted from the decompressed block using the item index.

## Rendering and Media Routing
Slob dictionaries natively store their content as HTML, so Diction does not need to perform complex markup translation (like DSL or XDXF). The HTML is passed directly to the standard rendering pipeline.

However, Wikipedia Slob files often contain thousands of embedded images. Slob stores these images in the exact same chunked architecture as the text definitions. Diction intercepts the `<img src="dict://...">` requests and routes them back through the `dict-slob.c` extractor, which decompresses the specific image chunk and streams it natively to WebKit.
