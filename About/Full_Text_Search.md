# Full-Text Search (FTS)

While standard prefix searching is instantaneous, it is limited to matching the dictionary's defined headwords. When a user wants to find a word buried deep inside a definition, Diction utilizes Full-Text Search (FTS).

```text
[User Queries "gravity" via FTS Mode]
             |
             v
 [Bypass Prefix Search]
             |
             v
+-----------------------------+
|    Construct FTS5 MATCH     |
|  "SELECT ... MATCH 'gravity'"|
+-------------+---------------+
              |
      (Broadcast Query)
              |
      +-------+-------+
      v               v
  [Dict 1 .fts]   [Dict 2 .fts]  ...
      |               |
      +-------+-------+
              v
[FTS5 Engine Tokenizes & Scores] -> (bm25 ranking)
              |
              v
[Extract Best Matches (Limit 300)]
              |
              v
[Inject <mark> Highlights in HTML]
              |
              v
    [Render in WebKitGTK]
```


## The SQLite FTS5 Engine
Diction leverages SQLite's highly optimized `FTS5` virtual table extension. During the indexing phase, if FTS is enabled, the engine strips HTML markup from every dictionary entry and inserts the raw text tokens into a persistent SQLite `.fts` database file.

SQLite builds an inverted index, associating every unique word with the exact dictionary offset it appears at. 

## Triggering FTS
FTS is significantly heavier than standard prefix search, so it is usually triggered intentionally by the user either by selecting the "Full Text Search" mode in the UI or by using keyboard shortcut `Ctrl+Shift+f`.

When an FTS query is initiated:
1. The standard prefix search loop is completely bypassed.
2. The engine constructs an FTS5 `MATCH` query based on the user's input.
3. The query is broadcast across the SQLite `.fts` databases of all currently active dictionaries.

## FTS Ranking and Limits
Because FTS can return thousands of results for common words, the engine enforces strict result limits (`MAX_FTS_RESULTS_PER_DICT`, typically 300) to ensure the UI remains responsive and the WebKit renderer does not crash under memory pressure.

FTS5 internally ranks the results using the `bm25()` scoring function, pushing the most relevant definitions (where the term appears frequently or heavily weighted) to the top of the SQL `ORDER BY` clause.

## FTS Hit Highlighting
When an FTS result is clicked in the sidebar, the user is navigated to the WebKit definition view. To help the user find the word they were looking for, Diction utilizes a post-processing `fts_highlight_query`. 

Before the HTML is injected into the WebKit DOM, the engine parses the raw HTML definition, locates instances of the FTS query, and wraps them in a `<mark class='fts-highlight'>` tag. This allows the CSS engine to brightly highlight the target word without breaking the underlying dictionary markup tags.
