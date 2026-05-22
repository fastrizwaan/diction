# MDX/MDD (MDict) Format

Diction provides comprehensive, native support for the MDict format (`.mdx` for text definitions, `.mdd` for embedded media), including modern MDict 2.0+ specifications.

## Architecture and Loading
MDX files are highly complex binary containers. They consist of:
1. **Header Block**: Contains XML metadata outlining encoding (UTF-8, UTF-16), version (1.2, 2.0), and encryption flags.
2. **Key Block**: A compressed, nested tree structure mapping headwords to integer offsets.
3. **Record Block**: A heavily compressed block containing the raw HTML definition data.

### Encryption and RIPEMD-128
MDict 2.0+ dictionaries frequently encrypt their Key and Record blocks to prevent tampering. Diction implements a custom `ripemd128.c` decryption pipeline. During the cache building phase (`mdx_build_index`), the engine decrypts the block headers using the dictionary's embedded checksums to accurately traverse the key tree.

### The Indexing Phase
Unlike simpler formats, parsing MDX requires expanding the entire key tree. Diction decompresses the `Key Blocks` (which may use `zlib` or `LZO` compression) sequentially. For each entry, it calculates its exact relative offset within the `Record Blocks`. This information is dumped into Diction's native `.index` flat file.

## On-The-Fly Rendering
Because MDX `Record Blocks` can be massive (often 64KB+ compressed chunks), extracting definitions requires targeted reads:
1. When a word is requested, the `FlatIndex` provides the exact byte offset.
2. `mdx_get_definition_on_the_fly` utilizes `pread()` to safely and concurrently seek to the correct `Record Block` in the file.
3. The specific block is decompressed entirely into memory.
4. The requested definition is sliced out of the decompressed block using the relative offsets.

## MDD Resource Routing
An MDX file is often accompanied by an `.mdd` file containing images, pronunciation audio, and CSS.
When the WebKit renderer requests a local file via `dict://image.png`, Diction routes the request to `mdd_get()`. This function performs a rapid binary search inside the `.mdd` archive's Key Block, locates the compressed binary file, extracts it instantly into memory, and serves it back to WebKit—all without generating temporary files on disk.
