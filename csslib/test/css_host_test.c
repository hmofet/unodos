/* css_host_test.c - host smoke test for the vendored CSS stack + port.
 *
 * Links the SAME freestanding-compiled csslib objects against the host CRT
 * and drives parse -> cascade -> computed-style checks. The select handler,
 * unit context and single-node "document" scaffolding are adapted from
 * libcss's examples/example1.c (MIT, Copyright 2010 The NetSurf Browser
 * Project); the handler bodies are included VERBATIM from
 * css_test_handlers.inc so an upstream bump can re-extract them
 * mechanically (examples/example1.c lines under "Select handlers").
 *
 * Build: sh csslib/test/build-host-test.sh   (from the repo root)
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <libcss/libcss.h>

#define UNUSED(x) ((x) = (x))

/* forward declarations for the handler table (extracted from example1.c) */
#include "css_test_decls.inc"

static css_unit_ctx unit_len_ctx = {
    .viewport_width    = 800 * (1 << CSS_RADIX_POINT),
    .viewport_height   = 600 * (1 << CSS_RADIX_POINT),
    .font_size_default =  16 * (1 << CSS_RADIX_POINT),
    .font_size_minimum =   6 * (1 << CSS_RADIX_POINT),
    .device_dpi        =  96 * (1 << CSS_RADIX_POINT),
    .root_style        = NULL,
    .pw                = NULL,
    .measure           = NULL,
};

static css_select_handler select_handler = {
    CSS_SELECT_HANDLER_VERSION_1,

    node_name,
    node_classes,
    node_id,
    named_ancestor_node,
    named_parent_node,
    named_sibling_node,
    named_generic_sibling_node,
    parent_node,
    sibling_node,
    node_has_name,
    node_has_class,
    node_has_id,
    node_has_attribute,
    node_has_attribute_equal,
    node_has_attribute_dashmatch,
    node_has_attribute_includes,
    node_has_attribute_prefix,
    node_has_attribute_suffix,
    node_has_attribute_substring,
    node_is_root,
    node_count_siblings,
    node_is_empty,
    node_is_link,
    node_is_visited,
    node_is_hover,
    node_is_active,
    node_is_focus,
    node_is_enabled,
    node_is_disabled,
    node_is_checked,
    node_is_target,
    node_is_lang,
    node_presentational_hint,
    ua_default_for_property,
    set_libcss_node_data,
    get_libcss_node_data,
};

static void die(const char *text, css_error code)
{
    printf("ERROR: %s: %i: %s\n", text, code, css_error_to_string(code));
    abort();
}

static css_error resolve_url(void *pw,
        const char *base, lwc_string *rel, lwc_string **abs)
{
    UNUSED(pw);
    UNUSED(base);
    *abs = lwc_string_ref(rel);
    return CSS_OK;
}

/* the handler bodies, verbatim from example1.c */
#include "css_test_handlers.inc"

/* ---- the checks ---------------------------------------------------------- */
static int g_pass, g_fail;

static void note(int ok, const char *name)
{ printf("%s %s\n", ok ? "pass" : "FAIL", name); if (ok) g_pass++; else g_fail++; }

/* select the style for element `name` and return it (caller destroys) */
static css_select_results *sel(css_select_ctx *ctx, const char *name)
{
    lwc_string *el;
    css_select_results *style;
    css_media media = { .type = CSS_MEDIA_SCREEN };
    css_error code;

    lwc_intern_string(name, strlen(name), &el);
    code = css_select_style(ctx, el, &unit_len_ctx, &media, NULL,
                            &select_handler, 0, &style);
    if (code != CSS_OK)
        die("css_select_style", code);
    lwc_string_unref(el);
    return style;
}

static css_color color_of(css_select_results *style, uint8_t *type)
{
    css_color c = 0;
    *type = css_computed_color(style->styles[CSS_PSEUDO_ELEMENT_NONE], &c);
    return c;
}

int main(void)
{
    css_error code;
    css_stylesheet *sheet;
    css_select_ctx *select_ctx;
    css_stylesheet_params params;
    static const char data[] =
        "h1 { color: red } "
        "h4 { color: #321; } "
        "h4, h5 { color: #123456; } "
        "p  { color: rgb(10, 20, 30); display: none } "
        "@media print { h1 { color: blue } }";
    css_select_results *style;
    uint8_t t;
    css_color c;

    memset(&params, 0, sizeof params);
    params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    params.level = CSS_LEVEL_21;
    params.charset = "UTF-8";
    params.url = "test";
    params.title = "test";
    params.resolve = resolve_url;

    code = css_stylesheet_create(&params, &sheet);
    if (code != CSS_OK) die("css_stylesheet_create", code);

    code = css_stylesheet_append_data(sheet, (const uint8_t *)data,
                                      sizeof data - 1);
    if (code != CSS_OK && code != CSS_NEEDDATA)
        die("css_stylesheet_append_data", code);
    code = css_stylesheet_data_done(sheet);
    if (code != CSS_OK) die("css_stylesheet_data_done", code);
    note(1, "parse: 5-rule sheet incl. @media");

    code = css_select_ctx_create(&select_ctx);
    if (code != CSS_OK) die("css_select_ctx_create", code);
    code = css_select_ctx_append_sheet(select_ctx, sheet, CSS_ORIGIN_AUTHOR,
                                       NULL);
    if (code != CSS_OK) die("css_select_ctx_append_sheet", code);

    style = sel(select_ctx, "h1");
    c = color_of(style, &t);
    note(t == CSS_COLOR_COLOR && c == 0xffff0000, "h1: red keyword");
    css_select_results_destroy(style);

    style = sel(select_ctx, "h4");
    c = color_of(style, &t);
    note(t == CSS_COLOR_COLOR && c == 0xff123456, "h4: later rule wins cascade");
    css_select_results_destroy(style);

    style = sel(select_ctx, "h5");
    c = color_of(style, &t);
    note(t == CSS_COLOR_COLOR && c == 0xff123456, "h5: grouped selector");
    css_select_results_destroy(style);

    /* An element no rule matches computes to the property's INITIAL value
     * (color: fully-transparent 0). A UA sheet supplies real defaults in
     * production - that is CS2's job, exactly like the browser's current
     * default text colour. (Old libcss returned INHERIT here; 0.9.x
     * composes initials - assert the contract we actually build on.) */
    style = sel(select_ctx, "h2");
    c = color_of(style, &t);
    note(t == CSS_COLOR_COLOR && c == 0x00000000, "h2: unmatched -> initial value");
    css_select_results_destroy(style);

    style = sel(select_ctx, "p");
    c = color_of(style, &t);
    note(t == CSS_COLOR_COLOR && c == 0xff0a141e, "p: rgb() function");
    note(css_computed_display(style->styles[CSS_PSEUDO_ELEMENT_NONE], false)
             == CSS_DISPLAY_NONE, "p: display property");
    css_select_results_destroy(style);

    /* the @media print rule must NOT apply on screen media */
    style = sel(select_ctx, "h1");
    c = color_of(style, &t);
    note(c == 0xffff0000, "h1: @media print ignored on screen");
    css_select_results_destroy(style);

    css_select_ctx_destroy(select_ctx);
    css_stylesheet_destroy(sheet);

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail != 0;
}
