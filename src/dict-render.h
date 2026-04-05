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
                         int dark_mode,
                         const char *theme_name,
                         const char *render_style,
                         const char *font_family,
                         int font_size);

typedef struct {
    const char *bg;
    const char *fg;
    const char *accent;
    const char *link;
    const char *border;
    const char *heading;
    const char *trn;
    const char *translit;
    const char *ex;
    const char *com;
} dsl_theme_palette;

void dict_render_get_theme_palette(const char *theme_name, int dark_mode, dsl_theme_palette *out_palette);
