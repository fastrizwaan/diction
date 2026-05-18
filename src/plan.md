# Goal Description

Fix slow MDD loading and out-of-memory crashes by implementing lazy, on-the-fly Key Block binary search instead of loading all MDD entries into memory at startup. This also fixes a UTF-16 string decoding bug that caused the crash.

## User Review Required

Please review the proposed approach. This replaces the startup-time MDD index building with an on-the-fly approach. It significantly reduces memory usage (from 500MB+ for Wiktionary to ~50KB) and load time (from 15+ seconds to instant).

## Proposed Changes

### [MODIFY] dict-mdx.c

- **Remove `MDDRes *resources`**: Do not allocate and `qsort` the massive array of all MDD resources on startup.
- **Add `MDD_KBI` structure**: Parse `head_word` and `tail_word` from the Key Block Info header, storing them with their respective Key Block metadata (`comp_size`, `decomp_size`, `file_offset`).
- **Fix `mdd_encoding_is_utf16` logic**: Always assume UTF-16 for MDD files, as per the MDict specification, regardless of what the `.mdx` or `.mdd` header claims. This fixes the out-of-bounds KBI parsing bug that resulted in the `g_malloc(18446744069414638273)` crash.
- **Update `mdd_has` and `mdd_get`**:
  - Implement binary search across the small `MDD_KBI` array using the `head_word` and `tail_word` boundaries.
  - When a potential Key Block is identified, dynamically decompress it and perform a local linear search to find the requested resource's `start_off`.
  - Calculate `end_off` by reading the next entry's offset (dynamically decompressing the first entry of the subsequent Key Block if the word happens to be the last entry in its block).
  - Proceed with existing Record Block (RBI) decompression logic.
