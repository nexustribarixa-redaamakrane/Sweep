/* sweep_resource_test.c - host tests for the file-local resource provider.
 * Covers path safety, MIME/extension policy, size bounds, and CSS loading.
 * Build (from Sweep root): included in `make sweep-tests`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../sweep_resource.h"

static int g_failures = 0;
static int g_passed = 0;

static void check(const char* label, int ok) {
    if (ok) { ++g_passed; printf("[PASS] %s\n", label); }
    else { ++g_failures; printf("[FAIL] %s\n", label); }
}

static void write_file(const char* name, const void* bytes, size_t len) {
    char path[512];
    FILE* f;
    snprintf(path, sizeof(path), "build-generated/resource_fixtures/%s", name);
    f = fopen(path, "wb");
    if (!f) return;
    (void) fwrite(bytes, 1u, len, f);
    fclose(f);
}

int main(void) {
    SWEEP_RESOURCE_ROOT root;
    char out[1024];
    char mime[64];
    size_t out_len = 0u;

    printf("=== Sweep resource provider tests ===\n");
    (void) mkdir("build-generated/resource_fixtures");
    write_file("ok.css", "body{color:red;}", strlen("body{color:red;}"));
    write_file("note.txt", "hello", 5u);
    write_file("bad.bin", "a\0b", 3u);
    write_file("big.css", "0123456789", 10u);

    check("[resource] root init",
          sweep_resource_root_init(&root, "build-generated/resource_fixtures"));
    check("[resource] empty/unset root rejected",
          !sweep_resource_root_init(&root, ""));

    /* Relative CSS path loads with correct MIME. */
    {
        memset(out, 0, sizeof(out)); memset(mime, 0, sizeof(mime)); out_len = 0u;
        check("[resource] relative CSS path loads",
              sweep_resource_fetch_file(&root, "ok.css", out, sizeof(out),
                                        &out_len, mime, sizeof(mime)) &&
              out_len == strlen("body{color:red;}") &&
              strcmp(out, "body{color:red;}") == 0 &&
              strcmp(mime, "text/css") == 0);
    }

    /* Path safety / attacks. */
    check("[resource] absolute path rejected",
          !sweep_resource_path_allowed("/etc/passwd") &&
          !sweep_resource_fetch_file(&root, "/etc/passwd", out, sizeof(out),
                                     &out_len, mime, sizeof(mime)));
    check("[resource] parent-directory traversal rejected",
          !sweep_resource_path_allowed("../secret.css") &&
          !sweep_resource_path_allowed("a/../../b.css") &&
          !sweep_resource_path_allowed("..") &&
          !sweep_resource_fetch_file(&root, "../secret.css", out, sizeof(out),
                                     &out_len, mime, sizeof(mime)));
    check("[resource] backslash rejected",
          !sweep_resource_path_allowed("a\\b.css") &&
          !sweep_resource_fetch_file(&root, "a\\b.css", out, sizeof(out),
                                     &out_len, mime, sizeof(mime)));
    check("[resource] drive/control chars rejected",
          !sweep_resource_path_allowed("C:foo.css") &&
          !sweep_resource_path_allowed("a\nb.css") &&
          !sweep_resource_path_allowed(""));

    /* Missing / binary / unsupported / oversized. */
    check("[resource] missing stylesheet fails",
          !sweep_resource_fetch_file(&root, "nope.css", out, sizeof(out),
                                     &out_len, mime, sizeof(mime)));
    check("[resource] binary resource rejected",
          !sweep_resource_fetch_file(&root, "bad.bin", out, sizeof(out),
                                     &out_len, mime, sizeof(mime)));
    {
        /* Unknown extension with binary bytes: rejected by sniff.  Unknown
         * extension with ASCII text would be text/plain (tested via .txt). */
        write_file("mystery.xyz", "\x01\x02", 2u);
        check("[resource] unsupported/binary extension rejected",
              !sweep_resource_fetch_file(&root, "mystery.xyz", out, sizeof(out),
                                         &out_len, mime, sizeof(mime)));
    }
    check("[resource] plain text sniffed as text/plain",
          sweep_resource_fetch_file(&root, "note.txt", out, sizeof(out),
                                    &out_len, mime, sizeof(mime)) &&
          strcmp(mime, "text/plain") == 0);

    {
        SWEEP_RESOURCE_ROOT tiny = root;
        char small[4];
        tiny.max_bytes = 4u;
        check("[resource] oversized stylesheet rejected",
              !sweep_resource_fetch_file(&tiny, "big.css", small, sizeof(small),
                                         &out_len, mime, sizeof(mime)));
    }
    check("[resource] caller buffer overflow rejected",
          !sweep_resource_fetch_file(&root, "ok.css", out, 4u,
                                     &out_len, mime, sizeof(mime)));

    /* Unconfigured provider resolves nothing. */
    check("[resource] NULL root resolves nothing",
          !sweep_resource_fetch_file(NULL, "ok.css", out, sizeof(out),
                                     &out_len, mime, sizeof(mime)));
    {
        SWEEP_RESOURCE_ROOT unset;
        memset(&unset, 0, sizeof(unset));
        check("[resource] unset root resolves nothing",
              !sweep_resource_fetch_file(&unset, "ok.css", out, sizeof(out),
                                         &out_len, mime, sizeof(mime)));
    }

    printf("--------------------------------\n");
    printf("passed: %d, failed: %d\n", g_passed, g_failures);
    return g_failures ? 1 : 0;
}
