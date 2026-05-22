# Standard Search (Prefix / Exact)

The standard search pipeline in Diction is designed to populate the sidebar instantly as the user types. Because users expect real-time feedback with every keystroke, this process must be incredibly fast, bypassing expensive database queries and relying entirely on memory-mapped operations.

```text
[User Types "gara"] -> [Debounced 350ms] -> (Trigger Search)
                                                 |
         +---------------------------------------+
         |
 [Normalize Key: "gara"]
         |
         v
 [Binary Search (.index)] -> O(log N) lookup (Microseconds)
         |
         v
 [Index Lands on "garage"]
         |
         v
 [Sequential Scan]
  ├── "garage"     (Prefix Match! Add to Results)
  ├── "garaged"    (Prefix Match! Add to Results)
  ├── "garagesale" (Prefix Match! Add to Results)
  ├── "garbage"    (Prefix Mismatch! BREAK LOOP)
  └── [Search concludes instantly for this dict]
```


## The Search Request
When the user types a word (e.g., `gar`), the UI debounces the input (350ms) to prevent overwhelming the engine. Once the debounce timer fires, a background `GTask` thread is launched to perform the search.

The search string undergoes the exact same normalization pipeline used during indexing: it is decomposed, stripped of diacritics, lowered, and spaces are removed. Thus, the query `garage sale` becomes the search key `garagesale`.

## O(log N) Binary Search
The core of the standard search relies on the `FlatIndex` `.index` cache file. Because this file is strictly alphabetically sorted by the normalized keys and loaded into memory via `mmap`, Diction does not need to scan the dictionary sequentially. 

Instead, it performs a binary search (`flat_index_search_prefix_fast`) to jump directly to the exact index where the prefix `gar` first appears among millions of entries. This lookup takes mere microseconds.

## Prefix Scanning and Alphabetical Boundaries
Once the binary search lands on the first matching prefix (e.g., `garage`), the engine begins a rapid sequential scan of the subsequent words (`garaged`, `garages`, `garaging`). 

Because the `FlatIndex` is sorted alphabetically, the engine knows with absolute certainty that once it encounters a word that *no longer* starts with the prefix (e.g., `garbage`), the prefix matches have ended. The engine instantly breaks the loop for that dictionary, ensuring the scan operates in O(1) time after the initial lookup, rather than O(N) linear time. This algorithmic safeguard is what guarantees 0ms latency even with 100+ loaded dictionaries.

## Fuzzy and Substring Matching
While the fast prefix search handles the bulk of the results, the engine also evaluates candidates using the `classify_search_candidate_flexible` function. This function utilizes Gestalt pattern matching (SequenceMatcher) to assign fuzzy scores to words. 
Results are categorized into buckets:
1. **Exact Matches**: Perfect 1:1 string match.
2. **Prefix Matches**: Starts with the query.
3. **Substring/Phrase**: Contains the query.
4. **Fuzzy**: Typo-tolerant matches.

These buckets are streamed directly back to the main UI thread via `g_idle_add` so the user sees results populating in the sidebar instantly, rather than waiting for all 100 dictionaries to finish global sorting.
