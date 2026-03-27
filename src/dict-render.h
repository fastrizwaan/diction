#include "dict-loader.h"

/**
 * Render DSL tags inside an entry segment to straightforward HTML.
 * The resulting string must be freed by the caller.
 *
 * @param dark_mode If non-zero, use dark theme colors; otherwise use light theme.
 */
char* dsl_render_to_html(const char *dsl_text,
                         size_t length,
                         const char *headword,
                         size_t hw_length,
                         DictFormat format,
                         const char *resource_dir,
                         const char *source_dir,
                         const char *mdx_stylesheet,
                         int dark_mode);
