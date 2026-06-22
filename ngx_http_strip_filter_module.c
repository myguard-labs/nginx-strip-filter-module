/*
 * ngx_http_strip_filter_module.c
 *
 * Response-body minifier for HTML, CSS, JavaScript and JSON. Strips \r\n,
 * redundant whitespace and comments while preserving regions whose bytes are
 * significant (HTML <pre>/<textarea>/<script>/<style>, string/template/regex
 * literals). The transform itself lives in strip_core.c so it can be fuzzed
 * independently of nginx.
 *
 * Because minifying correctly requires seeing a whole document (literals and
 * raw-text elements may straddle buffer boundaries), the filter accumulates the
 * entire response body in a per-request chain and runs the minifier once, when
 * the last buffer arrives. Bodies larger than strip_max_size, or smaller than
 * strip_min_size, are passed through untouched.
 *
 * Directives (http/server/location):
 *   strip          on|off;      master switch (HTML)
 *   strip_css      on|off;      minify standalone text/css responses
 *   strip_js       on|off;      minify standalone JavaScript responses
 *   strip_json     on|off;      minify application/json responses
 *   strip_svg      on|off;      minify image/svg+xml responses
 *   strip_xml      on|off;      minify XML responses (RSS/Atom/sitemap, +xml)
 *   strip_min_size <size>;      skip bodies smaller than this (default 0)
 *   strip_max_size <size>;      skip bodies larger than this (default 10m)
 *   strip_types    <mime> ...;  extra HTML-treated MIME types
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "strip_core.h"


typedef struct {
    ngx_flag_t   enable;       /* strip (HTML)            */
    ngx_flag_t   css;          /* strip_css               */
    ngx_flag_t   js;           /* strip_js                */
    ngx_flag_t   json;         /* strip_json              */
    ngx_flag_t   svg;          /* strip_svg               */
    ngx_flag_t   xml;          /* strip_xml               */
    size_t       min_size;     /* strip_min_size          */
    size_t       max_size;     /* strip_max_size          */
    ngx_hash_t   types;        /* extra HTML MIME types   */
    ngx_array_t *types_keys;
} ngx_http_strip_loc_conf_t;


typedef struct {
    strip_kind_t  kind;        /* resolved content kind   */
    ngx_chain_t  *in;          /* accumulated input chain */
    ngx_chain_t **last_in;
    size_t        len;         /* bytes buffered so far   */
    unsigned      buffering:1; /* still collecting        */
    unsigned      done:1;      /* emitted / bypassed      */
    unsigned      last_buf:1;      /* saw a last_buf buffer      */
    unsigned      last_in_chain:1; /* saw a last_in_chain buffer */
} ngx_http_strip_ctx_t;


static ngx_int_t ngx_http_strip_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_strip_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in);
static ngx_int_t ngx_http_strip_flush(ngx_http_request_t *r,
    ngx_http_strip_ctx_t *ctx);

static void *ngx_http_strip_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_strip_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_strip_init(ngx_conf_t *cf);


static ngx_str_t  ngx_http_strip_default_types[] = {
    ngx_string("text/html"),
    ngx_null_string
};


static ngx_command_t  ngx_http_strip_filter_commands[] = {

    { ngx_string("strip"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_strip_loc_conf_t, enable),
      NULL },

    { ngx_string("strip_css"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_strip_loc_conf_t, css),
      NULL },

    { ngx_string("strip_js"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_strip_loc_conf_t, js),
      NULL },

    { ngx_string("strip_json"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_strip_loc_conf_t, json),
      NULL },

    { ngx_string("strip_svg"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_strip_loc_conf_t, svg),
      NULL },

    { ngx_string("strip_xml"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_strip_loc_conf_t, xml),
      NULL },

    { ngx_string("strip_min_size"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_strip_loc_conf_t, min_size),
      NULL },

    { ngx_string("strip_max_size"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_strip_loc_conf_t, max_size),
      NULL },

    { ngx_string("strip_types"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_types_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_strip_loc_conf_t, types_keys),
      &ngx_http_strip_default_types[0] },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_strip_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_strip_init,                   /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_strip_create_loc_conf,        /* create location configuration */
    ngx_http_strip_merge_loc_conf          /* merge location configuration */
};


ngx_module_t  ngx_http_strip_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_strip_filter_module_ctx,     /* module context */
    ngx_http_strip_filter_commands,        /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


/*
 * Resolve which minifier (if any) applies to this response. Standalone CSS/JS/
 * JSON are matched on Content-Type; everything in strip_types (text/html by
 * default) is treated as HTML. Returns 1 and sets *kind on a match.
 */
static ngx_int_t
ngx_http_strip_select(ngx_http_request_t *r, ngx_http_strip_loc_conf_t *slcf,
    strip_kind_t *kind)
{
    ngx_str_t  ct = r->headers_out.content_type;

    if (slcf->css
        && ct.len >= sizeof("text/css") - 1
        && ngx_strncasecmp(ct.data, (u_char *) "text/css",
                           sizeof("text/css") - 1) == 0)
    {
        *kind = STRIP_CSS;
        return 1;
    }

    if (slcf->js
        && ((ct.len >= sizeof("application/javascript") - 1
             && ngx_strncasecmp(ct.data, (u_char *) "application/javascript",
                                sizeof("application/javascript") - 1) == 0)
            || (ct.len >= sizeof("text/javascript") - 1
                && ngx_strncasecmp(ct.data, (u_char *) "text/javascript",
                                   sizeof("text/javascript") - 1) == 0)))
    {
        *kind = STRIP_JS;
        return 1;
    }

    if (slcf->json
        && ct.len >= sizeof("application/json") - 1
        && ngx_strncasecmp(ct.data, (u_char *) "application/json",
                           sizeof("application/json") - 1) == 0)
    {
        *kind = STRIP_JSON;
        return 1;
    }

    if (slcf->svg
        && ct.len >= sizeof("image/svg+xml") - 1
        && ngx_strncasecmp(ct.data, (u_char *) "image/svg+xml",
                           sizeof("image/svg+xml") - 1) == 0)
    {
        *kind = STRIP_SVG;
        return 1;
    }

    if (slcf->xml) {
        /* match text/xml, application/xml, and any structured-syntax
         * +xml subtype (application/rss+xml, application/atom+xml, …).
         * Consider only the media-type token, before any ; charset= part. */
        size_t mt = 0;
        while (mt < ct.len && ct.data[mt] != ';' && ct.data[mt] != ' ') {
            mt++;
        }

        if ((mt == sizeof("text/xml") - 1
             && ngx_strncasecmp(ct.data, (u_char *) "text/xml", mt) == 0)
            || (mt == sizeof("application/xml") - 1
                && ngx_strncasecmp(ct.data, (u_char *) "application/xml",
                                   mt) == 0)
            || (mt >= sizeof("+xml") - 1
                && ngx_strncasecmp(ct.data + mt - (sizeof("+xml") - 1),
                                   (u_char *) "+xml", sizeof("+xml") - 1) == 0))
        {
            *kind = STRIP_XML;
            return 1;
        }
    }

    if (slcf->enable
        && ngx_http_test_content_type(r, &slcf->types) != NULL)
    {
        *kind = STRIP_HTML;
        return 1;
    }

    return 0;
}


static ngx_int_t
ngx_http_strip_header_filter(ngx_http_request_t *r)
{
    ngx_http_strip_ctx_t       *ctx;
    ngx_http_strip_loc_conf_t  *slcf;
    strip_kind_t                kind;

    slcf = ngx_http_get_module_loc_conf(r, ngx_http_strip_filter_module);

    if (!slcf->enable && !slcf->css && !slcf->js && !slcf->json && !slcf->svg
        && !slcf->xml)
    {
        return ngx_http_next_header_filter(r);
    }

    /* nothing to minify */
    if (r->headers_out.status == NGX_HTTP_NO_CONTENT
        || r->headers_out.status < NGX_HTTP_OK
        || (r->headers_out.status >= NGX_HTTP_SPECIAL_RESPONSE
            && r->headers_out.status != NGX_HTTP_INTERNAL_SERVER_ERROR)
        || r->headers_out.status == NGX_HTTP_PARTIAL_CONTENT
        || r->headers_out.content_length_n == 0
        || r->header_only        /* HEAD etc.: no body will ever arrive */
        || r != r->main)
    {
        return ngx_http_next_header_filter(r);
    }

    /* A non-identity Content-Encoding means the body bytes are compressed
     * (gzip/br/zstd) or otherwise encoded — minifying them would corrupt the
     * stream. Leave encoded responses untouched. */
    if (r->headers_out.content_encoding
        && r->headers_out.content_encoding->value.len)
    {
        return ngx_http_next_header_filter(r);
    }

    if (!ngx_http_strip_select(r, slcf, &kind)) {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_strip_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ctx->kind = kind;
    ctx->last_in = &ctx->in;
    ctx->buffering = 1;

    ngx_http_set_ctx(r, ctx, ngx_http_strip_filter_module);

    /* the content length is no longer known until we have minified */
    ngx_http_clear_content_length(r);
    ngx_http_weak_etag(r);

    return ngx_http_next_header_filter(r);
}


static ngx_int_t
ngx_http_strip_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_chain_t                *cl;
    ngx_http_strip_ctx_t       *ctx;
    ngx_http_strip_loc_conf_t  *slcf;
    int                         last = 0;

    ctx = ngx_http_get_module_ctx(r, ngx_http_strip_filter_module);

    if (ctx == NULL || ctx->done) {
        return ngx_http_next_body_filter(r, in);
    }

    slcf = ngx_http_get_module_loc_conf(r, ngx_http_strip_filter_module);

    /* copy the incoming buffers into our accumulation chain */
    for (cl = in; cl; cl = cl->next) {
        ngx_chain_t  *nl;
        ngx_buf_t    *b = cl->buf;
        off_t         n = ngx_buf_size(b);

        /* A buffer whose data is NOT in memory (file-backed, e.g. served from
         * cache or sendfile) cannot be coalesced via b->pos. Don't try to
         * minify such a body — stop buffering and flush what we have, then let
         * the remaining chain pass through untouched. */
        if (n > 0 && !ngx_buf_in_memory(b)) {
            ctx->buffering = 0;
        }

        if (n > 0 && ctx->buffering) {
            /* Stop buffering once the body would exceed max_size. The
             * subtraction is overflow-safe because ctx->len <= max_size is an
             * invariant maintained here (we never add past the cap). */
            if ((size_t) n > slcf->max_size - ctx->len) {
                ctx->buffering = 0;
            } else {
                ctx->len += (size_t) n;
            }
        }

        nl = ngx_alloc_chain_link(r->pool);
        if (nl == NULL) {
            return NGX_ERROR;
        }

        /* While we are still buffering we will return NGX_OK and hold this
         * chain across filter calls. We must NOT retain the upstream ngx_buf_t
         * pointer: its owner may recycle or refill it before we flush, which
         * would corrupt our coalesced copy or, if the buffer grows, overrun the
         * src allocation sized to ctx->len. Snapshot the in-memory bytes into a
         * request-pool-owned buffer now. Buffers that triggered a bypass
         * (file-backed / oversize: ctx->buffering already 0) are emitted in
         * THIS same call, so aliasing the original pointer for them is safe. */
        if (ctx->buffering && n > 0 && ngx_buf_in_memory(b)) {
            ngx_buf_t  *cb = ngx_calloc_buf(r->pool);
            u_char     *cp = ngx_pnalloc(r->pool, (size_t) n);
            if (cb == NULL || cp == NULL) {
                return NGX_ERROR;
            }
            ngx_memcpy(cp, b->pos, (size_t) n);
            cb->pos = cp;
            cb->last = cp + n;
            cb->memory = 1;
            cb->last_buf = b->last_buf;
            cb->last_in_chain = b->last_in_chain;
            cb->flush = b->flush;
            nl->buf = cb;
        } else {
            nl->buf = b;
        }
        nl->next = NULL;
        *ctx->last_in = nl;
        ctx->last_in = &nl->next;

        /* Track the real terminal flags so we can reproduce them exactly on
         * the emitted buffer (a main-request body ends on last_buf; a
         * subrequest body ends on last_in_chain). */
        if (b->last_buf) {
            ctx->last_buf = 1;
            last = 1;
        }
        if (b->last_in_chain) {
            ctx->last_in_chain = 1;
            last = 1;
        }
    }

    if (!last && ctx->buffering) {
        /* hold the chain; nothing to emit downstream yet */
        return NGX_OK;
    }

    if (!ctx->buffering || ctx->len < slcf->min_size) {
        /* too large or too small: emit the buffered chain unchanged */
        ngx_chain_t *out = ctx->in;
        ctx->in = NULL;
        ctx->done = 1;
        return ngx_http_next_body_filter(r, out);
    }

    return ngx_http_strip_flush(r, ctx);
}


/* Coalesce the buffered chain, minify it once and emit a single buffer. */
static ngx_int_t
ngx_http_strip_flush(ngx_http_request_t *r, ngx_http_strip_ctx_t *ctx)
{
    ngx_chain_t  *cl, *out;
    ngx_buf_t    *b;
    u_char       *src, *p, *dst;
    size_t        outlen;

    src = ngx_pnalloc(r->pool, ctx->len ? ctx->len : 1);
    if (src == NULL) {
        return NGX_ERROR;
    }

    p = src;
    for (cl = ctx->in; cl; cl = cl->next) {
        size_t n = ngx_buf_size(cl->buf);
        /* only in-memory buffers reach here (the body filter stops buffering
         * on the first file-backed buffer); guard anyway against pos misuse */
        if (n > 0 && ngx_buf_in_memory(cl->buf)) {
            ngx_memcpy(p, cl->buf->pos, n);
            p += n;
        }
    }

    /* output is never larger than input */
    dst = ngx_pnalloc(r->pool, ctx->len ? ctx->len : 1);
    if (dst == NULL) {
        return NGX_ERROR;
    }

    outlen = strip_minify(ctx->kind, src, ctx->len, dst);

    b = ngx_calloc_buf(r->pool);
    if (b == NULL) {
        return NGX_ERROR;
    }

    b->pos = dst;
    b->last = dst + outlen;
    b->memory = (outlen > 0) ? 1 : 0;
    /* reproduce exactly the terminal flags we observed on the input chain */
    b->last_buf = ctx->last_buf;
    b->last_in_chain = ctx->last_in_chain;
    b->sync = (outlen == 0) ? 1 : 0;

    out = ngx_alloc_chain_link(r->pool);
    if (out == NULL) {
        return NGX_ERROR;
    }
    out->buf = b;
    out->next = NULL;

    ctx->done = 1;
    ctx->in = NULL;

    return ngx_http_next_body_filter(r, out);
}


static void *
ngx_http_strip_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_strip_loc_conf_t  *slcf;

    slcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_strip_loc_conf_t));
    if (slcf == NULL) {
        return NULL;
    }

    slcf->enable   = NGX_CONF_UNSET;
    slcf->css      = NGX_CONF_UNSET;
    slcf->js       = NGX_CONF_UNSET;
    slcf->json     = NGX_CONF_UNSET;
    slcf->svg      = NGX_CONF_UNSET;
    slcf->xml      = NGX_CONF_UNSET;
    slcf->min_size = NGX_CONF_UNSET_SIZE;
    slcf->max_size = NGX_CONF_UNSET_SIZE;

    return slcf;
}


static char *
ngx_http_strip_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_strip_loc_conf_t  *prev = parent;
    ngx_http_strip_loc_conf_t  *conf = child;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    ngx_conf_merge_value(conf->css, prev->css, 0);
    ngx_conf_merge_value(conf->js, prev->js, 0);
    ngx_conf_merge_value(conf->json, prev->json, 0);
    ngx_conf_merge_value(conf->svg, prev->svg, 0);
    ngx_conf_merge_value(conf->xml, prev->xml, 0);
    ngx_conf_merge_size_value(conf->min_size, prev->min_size, 0);
    ngx_conf_merge_size_value(conf->max_size, prev->max_size,
                              10 * 1024 * 1024);

    if (ngx_http_merge_types(cf, &conf->types_keys, &conf->types,
                             &prev->types_keys, &prev->types,
                             ngx_http_strip_default_types)
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_strip_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_strip_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_strip_body_filter;


    return NGX_OK;
}
