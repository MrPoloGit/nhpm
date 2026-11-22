/* site.c - all knowledge of the website: URLs and XPath expressions.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "site.h"

#include <ctype.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>

#define SITE_BASE_URL "https://nhpmdb.alt.org"

/* ---- small libxml2 helpers ---- */

static htmlDocPtr parse_html(const char *html, size_t html_len)
{
    return htmlReadMemory(html, (int)html_len, "nhpm", "windows-1252",
                          HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING |
                              HTML_PARSE_NONET);
}

static char *node_text_simple(xmlNodePtr node)
{
    xmlChar *content = xmlNodeGetContent(node);
    char *result = (content != NULL) ? str_dup((const char *)content) : str_dup("");
    xmlFree(content);
    if (result != NULL) {
        str_trim(result);
    }
    return result;
}

/* Like node_text_simple(), but a <br> anywhere among the descendants
 * becomes "\n" instead of being dropped (for the long-description field). */
static void collect_text_with_breaks(xmlNodePtr node, buf *out)
{
    for (xmlNodePtr child = node->children; child != NULL; child = child->next) {
        if (child->type == XML_ELEMENT_NODE &&
            xmlStrcasecmp(child->name, (const xmlChar *)"br") == 0) {
            buf_append_str(out, "\n");
        } else if (child->type == XML_TEXT_NODE || child->type == XML_CDATA_SECTION_NODE) {
            if (child->content != NULL) {
                buf_append_str(out, (const char *)child->content);
            }
        } else if (child->type == XML_ELEMENT_NODE) {
            collect_text_with_breaks(child, out);
        }
    }
}

static char *node_text_with_breaks(xmlNodePtr node)
{
    buf b;
    buf_init(&b);
    collect_text_with_breaks(node, &b);
    char *result = buf_take(&b);
    str_trim(result);
    return result;
}

static char *node_attr(xmlNodePtr node, const char *attr)
{
    xmlChar *value = xmlGetProp(node, (const xmlChar *)attr);
    if (value == NULL) {
        return NULL;
    }
    char *result = str_dup((const char *)value);
    xmlFree(value);
    return result;
}

static xmlNodePtr xpath_first_node(xmlXPathContextPtr ctx, xmlNodePtr relative_to, const char *expr)
{
    xmlXPathObjectPtr obj = xmlXPathNodeEval(relative_to, (const xmlChar *)expr, ctx);
    if (obj == NULL) {
        return NULL;
    }
    xmlNodePtr result = NULL;
    if (obj->nodesetval != NULL && obj->nodesetval->nodeNr > 0) {
        result = obj->nodesetval->nodeTab[0];
    }
    xmlXPathFreeObject(obj);
    return result;
}

static bool looks_like_cloudflare_challenge(const char *html)
{
    return strstr(html, "Just a moment") != NULL ||
           strstr(html, "cf-browser-verification") != NULL ||
           strstr(html, "Checking your browser before accessing") != NULL;
}

static char *strip_id_from_href(const char *href)
{
    const char *p = href;
    if (p[0] == '/' && p[1] == '?') {
        p += 2;
    }
    return str_dup(p);
}

/* Takes ownership of `s` (as returned by node_text_simple()) and strips a
 * trailing "%", if any, returning the same pointer. */
static char *strip_percent(char *s)
{
    if (s == NULL) {
        return str_dup("-");
    }
    size_t len = strlen(s);
    if (len > 0 && s[len - 1] == '%') {
        s[len - 1] = '\0';
    }
    return s;
}

static char *resolve_site_url(const char *href)
{
    buf b;
    buf_init(&b);
    buf_append_str(&b, SITE_BASE_URL);
    buf_append_str(&b, href);
    return buf_take(&b);
}

/* ---- browse pages ---- */

void site_browse_page_free(site_browse_page *page)
{
    for (size_t i = 0; i < page->row_count; i++) {
        site_row *r = &page->rows[i];
        free(r->id);
        free(r->name);
        free(r->author);
        free(r->target);
        free(r->rating);
        free(r->short_description);
    }
    free(page->rows);
    page->rows = NULL;
    page->row_count = 0;
    page->total_pages = 0;
}

static site_row parse_browse_row(xmlXPathContextPtr ctx, xmlNodePtr row)
{
    site_row r = {0};

    xmlNodePtr link = xpath_first_node(ctx, row, "td[2]/a");
    if (link != NULL) {
        char *href = node_attr(link, "href");
        r.id = (href != NULL) ? strip_id_from_href(href) : str_dup("");
        free(href);
        r.name = node_text_simple(link);
    } else {
        r.id = str_dup("");
        r.name = str_dup("");
    }

    xmlNodePtr author_node = xpath_first_node(ctx, row, "td[3]");
    r.author = (author_node != NULL) ? node_text_simple(author_node) : str_dup("");

    xmlNodePtr target_node = xpath_first_node(ctx, row, "td[4]");
    r.target = (target_node != NULL) ? node_text_simple(target_node) : str_dup("");

    xmlNodePtr rating_node = xpath_first_node(ctx, row, ".//span[@class='ratingtext']");
    r.rating = (rating_node != NULL) ? strip_percent(node_text_simple(rating_node)) : str_dup("-");

    xmlNodePtr desc_node = xpath_first_node(ctx, row, "td[6]");
    r.short_description = (desc_node != NULL) ? node_text_simple(desc_node) : str_dup("");

    return r;
}

nhp_err site_parse_browse_page(const char *html, size_t html_len, site_browse_page *out)
{
    out->rows = NULL;
    out->row_count = 0;
    out->total_pages = 0;

    htmlDocPtr doc = parse_html(html, html_len);
    if (doc == NULL) {
        err_set("could not parse the browse page as HTML");
        return NHP_EPARSE;
    }

    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    if (ctx == NULL) {
        xmlFreeDoc(doc);
        err_set("out of memory creating an XPath context");
        return NHP_ENOMEM;
    }

    xmlXPathObjectPtr rows_obj =
        xmlXPathEvalExpression((const xmlChar *)"//table[@class='patchbrowse']//tr[td]", ctx);
    size_t row_count = (rows_obj != NULL && rows_obj->nodesetval != NULL)
                           ? (size_t)rows_obj->nodesetval->nodeNr
                           : 0;

    if (row_count == 0) {
        xmlXPathFreeObject(rows_obj);
        xmlXPathFreeContext(ctx);
        xmlFreeDoc(doc);
        if (looks_like_cloudflare_challenge(html)) {
            err_set("the site is blocking automated requests right now (Cloudflare challenge)");
            return NHP_ENET;
        }
        err_set("couldn't find the patch table; the site's layout may have changed");
        return NHP_EPARSE;
    }

    site_row *rows = calloc(row_count, sizeof(*rows));
    if (rows == NULL) {
        xmlXPathFreeObject(rows_obj);
        xmlXPathFreeContext(ctx);
        xmlFreeDoc(doc);
        return NHP_ENOMEM;
    }
    for (size_t i = 0; i < row_count; i++) {
        rows[i] = parse_browse_row(ctx, rows_obj->nodesetval->nodeTab[i]);
    }
    xmlXPathFreeObject(rows_obj);

    size_t total_pages = 0;
    xmlXPathObjectPtr pages_obj =
        /* the real page repeats the pager above and below the list
         * (docs/features.md's own fixture only shows one, but the site has
         * two): count just the first one's <option>s. */
        xmlXPathEvalExpression((const xmlChar *)"(//select[@name='pageselect'])[1]/option", ctx);
    if (pages_obj != NULL && pages_obj->nodesetval != NULL) {
        total_pages = (size_t)pages_obj->nodesetval->nodeNr;
    }
    xmlXPathFreeObject(pages_obj);

    xmlXPathFreeContext(ctx);
    xmlFreeDoc(doc);

    out->rows = rows;
    out->row_count = row_count;
    out->total_pages = total_pages;
    return NHP_OK;
}

char *site_browse_url(size_t page_number)
{
    buf b;
    buf_init(&b);
    if (page_number <= 1) {
        buf_append_str(&b, SITE_BASE_URL "/?browse");
    } else {
        buf_append_fmt(&b, SITE_BASE_URL "/?browse&page=%zu", page_number - 1);
    }
    return buf_take(&b);
}

/* ---- detail pages ---- */

void site_detail_free(site_detail *detail)
{
    free(detail->name);
    free(detail->rating);
    free(detail->author);
    free(detail->target);
    free(detail->description);
    free(detail->long_description);
    free(detail->download_url);
    free(detail->added);
    free(detail->changed);
    memset(detail, 0, sizeof(*detail));
}

static bool label_is_blank(const char *label)
{
    return label == NULL || label[0] == '\0' || str_eq(label, "\xc2\xa0");
}

/* One row's worth of work for site_parse_detail_page(): updates *out from a
 * single <tr>'s label/value, given the already-extracted (non-blank-aware)
 * `label` text. Takes ownership of `label`. */
static void apply_detail_row(xmlXPathContextPtr ctx, char *label, xmlNodePtr value_td,
                             site_detail *out, bool *have_download)
{
    if (label_is_blank(label)) {
        free(out->long_description);
        out->long_description = (value_td != NULL) ? node_text_with_breaks(value_td) : str_dup("");
    } else if (str_eq(label, "Name")) {
        free(out->name);
        out->name = (value_td != NULL) ? node_text_simple(value_td) : str_dup("");
    } else if (str_eq(label, "Rating")) {
        xmlNodePtr rating_span =
            (value_td != NULL) ? xpath_first_node(ctx, value_td, ".//span[@class='ratingtext']")
                               : NULL;
        free(out->rating);
        out->rating =
            (rating_span != NULL) ? strip_percent(node_text_simple(rating_span)) : str_dup("-");
    } else if (str_eq(label, "Author")) {
        free(out->author);
        out->author = (value_td != NULL) ? node_text_simple(value_td) : str_dup("");
    } else if (str_eq(label, "For")) {
        free(out->target);
        out->target = (value_td != NULL) ? node_text_simple(value_td) : str_dup("");
    } else if (str_eq(label, "Description")) {
        free(out->description);
        out->description = (value_td != NULL) ? node_text_simple(value_td) : str_dup("");
    } else if (str_eq(label, "Download")) {
        *have_download = true;
        xmlNodePtr link = (value_td != NULL) ? xpath_first_node(ctx, value_td, ".//a") : NULL;
        char *href = (link != NULL) ? node_attr(link, "href") : NULL;
        free(out->download_url);
        if (href != NULL && strncmp(href, "/?download=", 11) == 0) {
            out->download_hosted = true;
            out->download_url = resolve_site_url(href);
            free(href);
        } else {
            out->download_hosted = false;
            out->download_url = href; /* already absolute, or NULL */
        }
    } else if (str_eq(label, "Added")) {
        free(out->added);
        out->added = (value_td != NULL) ? node_text_simple(value_td) : str_dup("");
    } else if (str_eq(label, "Changed")) {
        free(out->changed);
        out->changed = (value_td != NULL) ? node_text_simple(value_td) : str_dup("");
    }
    free(label);
}

nhp_err site_parse_detail_page(const char *html, size_t html_len, site_detail *out)
{
    memset(out, 0, sizeof(*out));

    htmlDocPtr doc = parse_html(html, html_len);
    if (doc == NULL) {
        err_set("could not parse the detail page as HTML");
        return NHP_EPARSE;
    }
    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    if (ctx == NULL) {
        xmlFreeDoc(doc);
        err_set("out of memory creating an XPath context");
        return NHP_ENOMEM;
    }

    xmlXPathObjectPtr rows_obj =
        xmlXPathEvalExpression((const xmlChar *)"//table[@class='patchshow']//tr", ctx);
    size_t row_count = (rows_obj != NULL && rows_obj->nodesetval != NULL)
                           ? (size_t)rows_obj->nodesetval->nodeNr
                           : 0;

    if (row_count == 0) {
        xmlXPathFreeObject(rows_obj);
        xmlXPathFreeContext(ctx);
        xmlFreeDoc(doc);
        if (looks_like_cloudflare_challenge(html)) {
            err_set("the site is blocking automated requests right now (Cloudflare challenge)");
            return NHP_ENET;
        }
        err_set("couldn't find the patch details; the site's layout may have changed");
        return NHP_EPARSE;
    }

    bool have_download = false;
    for (size_t i = 0; i < row_count; i++) {
        xmlNodePtr row = rows_obj->nodesetval->nodeTab[i];
        xmlNodePtr label_td = xpath_first_node(ctx, row, "td[@class='rowname']");
        if (label_td == NULL) {
            continue;
        }
        xmlNodePtr value_td = xpath_first_node(ctx, row, "td[not(@class='rowname')]");
        apply_detail_row(ctx, node_text_simple(label_td), value_td, out, &have_download);
    }
    xmlXPathFreeObject(rows_obj);
    xmlXPathFreeContext(ctx);
    xmlFreeDoc(doc);

    if (out->name == NULL) {
        out->name = str_dup("");
    }
    if (out->rating == NULL) {
        out->rating = str_dup("-");
    }
    if (out->author == NULL) {
        out->author = str_dup("");
    }
    if (out->target == NULL) {
        out->target = str_dup("");
    }
    if (out->description == NULL) {
        out->description = str_dup("");
    }
    if (out->long_description == NULL) {
        out->long_description = str_dup("");
    }
    if (out->added == NULL) {
        out->added = str_dup("");
    }

    if (!have_download) {
        err_set("the detail page has no Download row");
        return NHP_EPARSE;
    }
    return NHP_OK;
}

char *site_detail_url(const char *id)
{
    buf b;
    buf_init(&b);
    buf_append_fmt(&b, SITE_BASE_URL "/?%s", id);
    return buf_take(&b);
}

char *site_download_url(const char *id)
{
    buf b;
    buf_init(&b);
    buf_append_fmt(&b, SITE_BASE_URL "/?download=%s", id);
    return buf_take(&b);
}

/* ---- name/version splitting ---- */

nhp_err site_split_name_version(const char *combined, char **out_name, char **out_version)
{
    *out_name = NULL;
    *out_version = NULL;

    char *trimmed = str_dup(combined);
    if (trimmed == NULL) {
        return NHP_ENOMEM;
    }
    str_trim(trimmed);

    char *last_space = NULL;
    for (char *p = trimmed; *p != '\0'; p++) {
        if (isspace((unsigned char)*p)) {
            last_space = p;
        }
    }
    char *last_word = (last_space != NULL) ? last_space + 1 : trimmed;

    regex_t re;
    bool have_re =
        regcomp(&re, "^v?[0-9]+(\\.[0-9]+)*[a-z]?$", REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0;
    bool is_version = have_re && regexec(&re, last_word, 0, NULL, 0) == 0;
    if (have_re) {
        regfree(&re);
    }

    if (is_version && last_space != NULL) {
        *out_version = str_dup(last_word);
        *last_space = '\0';
        str_trim(trimmed);
        *out_name = str_dup(trimmed);
    } else if (is_version) {
        *out_version = str_dup(last_word);
        *out_name = str_dup("");
    } else {
        *out_version = str_dup("unversioned");
        *out_name = str_dup(trimmed);
    }
    free(trimmed);

    if (*out_name == NULL || *out_version == NULL) {
        free(*out_name);
        free(*out_version);
        *out_name = NULL;
        *out_version = NULL;
        return NHP_ENOMEM;
    }
    return NHP_OK;
}
