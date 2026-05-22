# XDXF (XML Dictionary Exchange Format)

XDXF was designed to be a universal, structured XML format for exchanging dictionary data. Diction supports parsing and rendering `.xdxf` archives.

## Architecture and Loading
An XDXF file is essentially a massive, monolithic XML document. The root `<xdxf>` tag encapsulates a metadata `<info>` header and an `<article>` tree containing the dictionary data.

### The Indexing Phase
Because XDXF is standard XML, attempting to parse a 100MB file using a DOM parser (which loads the entire tree into RAM) would crash most systems. Instead, Diction utilizes an event-driven SAX (Simple API for XML) approach during indexing.

The scanner reads the file sequentially, searching for `<ar>` (article) tags. 
When it finds an article:
1. It scans inside for the `<k>` (key/headword) tag to extract the search term.
2. It records the byte offset where the `<ar>` tag begins and ends.
3. It pushes the normalized headword and byte range into the `.index` cache.

## Rendering and XSLT-style Transformation
When a user requests an XDXF definition, Diction retrieves the exact substring containing the `<ar>` XML block. However, WebKit cannot natively render XDXF tags like `<k>` or `<tr>`.

Diction's `dict-xdxf.c` renderer acts as a fast, hard-coded XSLT processor. It traverses the XML snippet and translates XDXF tags into semantic HTML:
- `<ar>` (Article) → `<div class="xdxf-article">`
- `<k>` (Key/Headword) → `<h1>` or `<div class="xdxf-headword">`
- `<tr>` (Transcription) → `<span class="phonetic">`
- `<def>` (Definition Block) → `<div class="xdxf-def">`

This process transforms the strict, semantic XML structure into a flexible HTML5 layout that can be easily styled using Diction's dynamic CSS engine.
