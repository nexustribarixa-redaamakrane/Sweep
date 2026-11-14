/* sweep_resource.h - file-local trusted resource provider for Sweep.
 *
 * The provider is registered by the embedding host (browser core), not by
 * scripts.  A script's <script src="..."> string names a resource; this
 * provider resolves it ONLY under an explicitly configured root directory
 * and only for allowlisted resource types.  JavaScript cannot select the
 * provider, the root, or absolute paths, and there is no network path.
 *
 * Host-only: uses <stdio.h> for bounded file reads, so it is excluded from
 * the freestanding kernel image like the rest of the JS layer.
 */
#ifndef SWEEP_RESOURCE_H
#define SWEEP_RESOURCE_H

#include <stdbool.h>
#include <stddef.h>

#include "sweep_js.h"
#include "sweep_runtime.h"

#define SWEEP_RESOURCE_ROOT_MAX   256u
#define SWEEP_RESOURCE_PATH_MAX   128u
#define SWEEP_RESOURCE_MAX_BYTES  65536u

/* Configured root.  Must be set by the host before the provider is used;
 * an unset (empty) root resolves nothing. */
typedef struct _SWEEP_RESOURCE_ROOT {
    char root[SWEEP_RESOURCE_ROOT_MAX]; /* directory prefix, no trailing '/'; "" = unconfigured */
    size_t max_bytes;                    /* per-resource cap; 0 = default cap */
} SWEEP_RESOURCE_ROOT;

/* Initialize a root struct with a trusted directory path copied from `root`.
 * Returns false for an overlong or empty path. */
bool sweep_resource_root_init(SWEEP_RESOURCE_ROOT* out, const char* root);

/* Bounded path validation for a requested resource URL.
 * Rejects: empty/overlong names, absolute paths, drive letters, backslashes,
 * and any ".." path segment (no traversal, no escaping the root). */
bool sweep_resource_path_allowed(const char* url);

/* SWEEP_JS_RESOURCE_FETCH implementation: reads url under root->root and
 * writes its bytes (plus NUL) into out when they fit, sets *out_len, and
 * sets mime_out by extension allowlist (.js/.mjs -> text/javascript,
 * .css -> text/css, .html/.htm -> text/html, .txt -> text/plain).
 * Unknown extensions still load as text/plain ONLY when the bytes are
 * plain ASCII without control characters (deterministic MIME sniff guard):
 * binary content is rejected.  Missing files, over-cap resources and
 * disallowed paths return false.  This function is trusted browser code;
 * it never touches memory the caller did not hand it.
 *
 * This legacy entry point uses the hosted runtime (sweep_runtime_host_default)
 * and is host-only, exactly as before.  New/portable callers pass an explicit
 * runtime to sweep_resource_fetch_runtime. */
bool sweep_resource_fetch_file(void* user_data, const char* url,
                               char* out, size_t out_cap, size_t* out_len,
                               char* mime_out, size_t mime_cap);

/* Runtime-explicit variant.  All file access is mediated by rt->read_resource;
 * this translation unit contains no stdio dependency.  A NULL/uninitialized
 * runtime fails deterministically. */
bool sweep_resource_fetch_runtime(SWEEP_RUNTIME* rt,
                                  const SWEEP_RESOURCE_ROOT* root,
                                  const char* url,
                                  char* out, size_t out_cap, size_t* out_len,
                                  char* mime_out, size_t mime_cap);

#endif /* SWEEP_RESOURCE_H */
