/* sweep_resource.c - bounded file-local resource provider.
 *
 * Path policy, MIME allowlist and size caps live here.  Actual byte reads are
 * mediated by SWEEP_RUNTIME.read_resource, so this translation unit has no
 * stdio dependency: the hosted implementation lives in sweep_runtime_host.c
 * and a future KAPA backend can replace it without touching path policy.
 */
#include "sweep_resource.h"

#include <string.h>

bool sweep_resource_root_init(SWEEP_RESOURCE_ROOT* out, const char* root) {
    size_t n;
    if (!out || !root || root[0] == '\0') return false;
    n = strlen(root);
    if (n >= SWEEP_RESOURCE_ROOT_MAX) return false;
    if (n > 1u && root[n - 1u] == '/') return false; /* joined with '/' below */
    memset(out, 0, sizeof(*out));
    memcpy(out->root, root, n + 1u);
    out->max_bytes = SWEEP_RESOURCE_MAX_BYTES;
    return true;
}

bool sweep_resource_path_allowed(const char* url) {
    size_t i, n;
    if (!url || url[0] == '\0') return false;
    n = strlen(url);
    if (n >= SWEEP_RESOURCE_PATH_MAX) return false;
    if (url[0] == '/') return false; /* absolute path */
    for (i = 0u; i < n; ++i) {
        unsigned char c = (unsigned char) url[i];
        if (c == '\\' || c == ':') return false; /* Windows separators/drive */
        if (c < 0x20u || c == 0x7Fu) return false; /* control characters */
    }
    /* Reject any ".." segment (traversal), including "x/../y" and ".." alone. */
    for (i = 0u; i < n; ++i) {
        if (url[i] == '.' && (i + 1u) < n && url[i + 1u] == '.') {
            bool lead_ok = (i == 0u) || (url[i - 1u] == '/');
            bool tail_ok = ((i + 2u) == n) || (url[i + 2u] == '/');
            if (lead_ok && tail_ok) return false;
        }
    }
    return true;
}

static const char* mime_for(const char* url) {
    size_t n = strlen(url);
    size_t i;
    for (i = n; i > 0u; --i) {
        if (url[i - 1u] == '.') {
            const char* ext = url + i;
            if (strcmp(ext, "js") == 0 || strcmp(ext, "mjs") == 0) return "text/javascript";
            if (strcmp(ext, "css") == 0) return "text/css";
            if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0) return "text/html";
            if (strcmp(ext, "txt") == 0) return "text/plain";
            return NULL; /* unknown extension: sniff below */
        }
    }
    return NULL;
}

bool sweep_resource_fetch_runtime(SWEEP_RUNTIME* rt,
                                  const SWEEP_RESOURCE_ROOT* root,
                                  const char* url,
                                  char* out, size_t out_cap, size_t* out_len,
                                  char* mime_out, size_t mime_cap) {
    char full[SWEEP_RESOURCE_ROOT_MAX + SWEEP_RESOURCE_PATH_MAX + 2u];
    size_t max_bytes, rlen, ulen, n = 0u, i;
    const char* mime = NULL;

    if (out_len) *out_len = 0u;
    if (!rt || root == NULL || root->root[0] == '\0' ||
        out == NULL || out_cap == 0u) {
        return false;
    }
    if (!sweep_resource_path_allowed(url)) return false;
    max_bytes = root->max_bytes ? root->max_bytes : SWEEP_RESOURCE_MAX_BYTES;
    if (max_bytes > SWEEP_RESOURCE_MAX_BYTES) max_bytes = SWEEP_RESOURCE_MAX_BYTES;
    rlen = strlen(root->root);
    ulen = strlen(url);
    if (rlen + 1u + ulen >= sizeof(full)) return false;
    memcpy(full, root->root, rlen);
    full[rlen] = '/';
    memcpy(full + rlen + 1u, url, ulen + 1u);

    /* The runtime enforces the caller's capacity and rejects overflow. */
    if (!sweep_runtime_read_resource(rt, full, out, out_cap, &n)) return false;
    if (n > max_bytes) {
        out[0] = '\0';
        return false;
    }
    if (out_len) *out_len = n;

    mime = mime_for(url);
    if (mime == NULL) {
        /* Deterministic guard: unknown types load only as plain ASCII text.
         * Binary NUL/control bytes are rejected rather than executed. */
        for (i = 0u; i < n; ++i) {
            unsigned char c = (unsigned char) out[i];
            if (c == 0u || (c < 0x09u) || (c > 0x0Du && c < 0x20u) ||
                c == 0x7Fu) {
                out[0] = '\0';
                return false;
            }
        }
        mime = "text/plain";
    }
    if (mime_out && mime_cap > 0u) {
        size_t mlen = strlen(mime);
        if (mlen >= mime_cap) mlen = mime_cap - 1u;
        memcpy(mime_out, mime, mlen);
        mime_out[mlen] = '\0';
    }
    return true;
}

bool sweep_resource_fetch_file(void* user_data, const char* url,
                               char* out, size_t out_cap, size_t* out_len,
                               char* mime_out, size_t mime_cap) {
    const SWEEP_RESOURCE_ROOT* root =
        (const SWEEP_RESOURCE_ROOT*) user_data;
    SWEEP_RUNTIME* rt = sweep_runtime_host_default();

    if (!root || rt == NULL) return false;
    return sweep_resource_fetch_runtime(rt, root, url, out, out_cap, out_len,
                                        mime_out, mime_cap);
}
