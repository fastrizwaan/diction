# ABBYY Lingvo DSL Format

Diction natively supports the highly popular ABBYY Lingvo `.dsl` format, as well as its DictZip-compressed counterpart, `.dsl.dz`.

## Architecture and Loading
Unlike binary formats, DSL dictionaries are essentially large, structured text files (usually encoded in UTF-16).
1. **Header Parsing**: Diction first scans the top of the file to extract metadata tags like `#NAME`, `#INDEX_LANGUAGE`, and `#CONTENTS_LANGUAGE`.
2. **Headword Extraction**: The scanner parses the file line-by-line. A line without leading whitespace is considered a headword. Any subsequent lines with leading spaces/tabs are considered the definition body for that headword.
3. **Indexing**: The engine records the byte offset where the definition block begins and how many bytes it spans, injecting this data into the `.index` file.
4. **Compression Handling**: If the dictionary is a `.dsl.dz` file, Diction routes the indexing scanner through the `DictZip` layer, which transparently decompresses blocks in memory so the scanner can locate the true uncompressed byte offsets.

## Markup Conversion and Rendering
DSL definitions are written using a custom bracket-based markup language (e.g., `[m1]`, `[tr]`, `[c]`, `[b]`, `[i]`). WebKitGTK cannot render this native DSL markup, so Diction employs a sophisticated state-machine parser (`dict-dsl.c`) to transcode it into HTML5.

- **Margins**: `[m1]` through `[m4]` tags are converted into `<div class="m1">` blocks with varying CSS `margin-left` values.
- **Colors**: `[c blue]text[/c]` is converted into `<span style="color: blue">text</span>`.
- **References**: `[ref]word[/ref]` is converted into a clickable hyperlink `<a href="dict://word">word</a>`. Clicking this in Diction instantly navigates the sidebar to that word.
- **Phonetics**: `[tr]` phonetic pronunciation tags are wrapped in specialized CSS classes to ensure they use proper IPA fonts.

## Media and Resources
DSL files often reference external multimedia using the `[s]sound.wav[/s]` or `[s]picture.bmp[/s]` tags. 
Diction converts these tags into HTML `<audio>` or `<img>` elements pointing to the `dict://` scheme. 

Because DSL does not bundle resources into a single archive (unlike MDX), Diction provides an internal `ResourceReader` that maps a directory adjacent to the `.dsl` file. When the UI requests `dict://sound.wav`, Diction scans the resource directory (or its contained `.zip` files) and streams the media to the browser.
