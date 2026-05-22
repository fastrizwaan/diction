# The Rendering Pipeline

Diction's rendering engine is responsible for taking raw dictionary data (which may be custom XML, proprietary tags, or legacy HTML) and transforming it into a modern, beautifully styled HTML5 document suitable for WebKitGTK.

## General Rendering Architecture

Regardless of the source dictionary format, every definition passes through a unified rendering pipeline in `src/dict-render.c`. 

```text
[Raw Byte Payload (offset + length)]
                 |
        (Decompress/Read)
                 |
    +------------+------------+
    |                         |
[UTF-8 check]           (Transcode)
    |                         |
    +------------+------------+
                 |
      (Format-Specific Parser)
                 |
        [Translate Markup]
    (e.g., [b] -> <b>, <k> -> <h1>)
                 |
      [Intercept Media Links]
   (e.g., src="dict://image.png")
                 |
        [Inject User Theme]
 (Dark Mode, Custom Fonts, Font Size)
                 |
                 v
     [Final HTML5 String Output]
                 |
                 v
   [webkit_web_view_load_html()]
```


### 1. Extraction and Transcoding
When a definition is requested:
1. The raw byte payload is extracted via memory-mapped pointers (for raw files), `pread` (for MDX chunks), or `dictzip_read` (for compressed archives).
2. The payload is transcoded from its native encoding (UTF-16, Windows-1251, Big5, etc.) into strict UTF-8. 
3. If the payload is invalid UTF-8, it is forcibly sanitized via `g_utf8_make_valid` to prevent SQLite or WebKit from crashing.

### 2. Format-Specific HTML Transformation
Most dictionaries do not output clean, modern HTML. The engine routes the UTF-8 payload to format-specific renderers:
- **DSL**: Converts `[m1]`, `[tr]`, `[c blue]` into corresponding `<div>`, `<span class="tr">`, and `<span style="color: blue">`.
- **StarDict**: Reads the `sametypesequence` array and wraps phonetic ('m'), pure text ('t'), or HTML ('h') payloads in appropriate containers.
- **XDXF / BGL**: Translates proprietary XML/Binary nodes into semantic HTML elements.
- **MDX**: Cleans up legacy inline `<script>` tags, strips dangerous `<object>` embeddings, and corrects broken resource links.

### 3. Resource Link Interception
During transformation, all local media paths (`sound.wav`, `/images/pic.jpg`) are intercepted and prepended with Diction's custom URI scheme: `dict://`. This ensures that when the HTML is rendered, WebKit requests the asset from Diction's internal memory router instead of looking on the local hard drive.

### 4. CSS Injection and Theming
Before final output, Diction constructs a unified `<head>` block.
- It injects `json-theme.c` configurations (Font Size, Font Family, Line Heights).
- It generates light or dark mode specific variables based on the active `AdwStyleManager` state.
- It appends any dictionary-specific stylesheets (e.g., an MDX file might have an accompanying `.css` file in its directory).

The final string is wrapped in a `<div class='word-group'>` and passed to WebKit via `webkit_web_view_load_html`.
