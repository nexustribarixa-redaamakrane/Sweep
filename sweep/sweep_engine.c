/* sweep_engine.c - Sweep browser-engine slice.  See sweep_engine.h. */
#include "sweep_engine.h"

#include <string.h>

static bool host_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
}

bool SweepUrlParse(const char* text, SWEEP_URL* out_url) {
    const char* scheme_end;
    const char* host_start;
    const char* p;
    size_t host_len = 0u;
    size_t i;

    if (text == (const char*)0 || out_url == (SWEEP_URL*)0) {
        return false;
    }
    memset(out_url, 0, sizeof(*out_url));

    scheme_end = strstr(text, "://");
    if (scheme_end == (const char*)0 || scheme_end == text) {
        return false;
    }
    /* Only http/https are admitted by the Sweep engine slice. */
    if (!((scheme_end - text == 4 && strncmp(text, "http", 4) == 0) ||
          (scheme_end - text == 5 && strncmp(text, "https", 5) == 0))) {
        return false;
    }

    host_start = scheme_end + 3;
    p = host_start;
    while (*p != '\0' && *p != '/' && *p != ':' && *p != '?' && *p != '#') {
        if (!host_char_ok(*p)) {
            return false;
        }
        ++host_len;
        ++p;
    }
    if (host_len == 0u || host_len >= sizeof(out_url->host)) {
        return false;
    }
    memcpy(out_url->host, host_start, host_len);
    out_url->host[host_len] = '\0';

    if (*p == ':') {
        unsigned long port = 0ul;
        ++p;
        if (*p == '\0') {
            return false;
        }
        while (*p >= '0' && *p <= '9') {
            port = port * 10ul + (unsigned long)(*p - '0');
            ++p;
        }
        if (port == 0ul || port > 65535ul) {
            return false;
        }
        out_url->port = (uint16_t)port;
        out_url->has_port = true;
    }

    /* Path is everything from the first '/' onward (or "/"). */
    {
        const char* path = (*p == '/') ? p : "/";
        size_t n = strlen(path);
        if (n >= sizeof(out_url->path)) {
            return false;
        }
        memcpy(out_url->path, path, n + 1u);
    }

    for (i = 0u; out_url->path[i] != '\0'; ++i) {
        if (out_url->path[i] < ' ') {
            return false;
        }
    }
    return true;
}

bool SweepViewportNegotiate(const SWEEP_VIEWPORT* requested,
                            SWEEP_VIEWPORT* out_viewport) {
    if (requested == (const SWEEP_VIEWPORT*)0 ||
        out_viewport == (SWEEP_VIEWPORT*)0) {
        return false;
    }
    if (requested->width_px == 0u || requested->height_px == 0u) {
        return false;
    }
    out_viewport->width_px = requested->width_px;
    if (out_viewport->width_px > 8192u) {
        out_viewport->width_px = 8192u;
    }
    out_viewport->height_px = requested->height_px;
    if (out_viewport->height_px > 8192u) {
        out_viewport->height_px = 8192u;
    }
    out_viewport->device_scale = requested->device_scale;
    if (out_viewport->device_scale == 0u) {
        out_viewport->device_scale = 1u;
    }
    if (out_viewport->device_scale > 4u) {
        out_viewport->device_scale = 4u;
    }
    return true;
}

bool SweepPaintAppend(SWEEP_PAINT_CMD* list, size_t capacity, size_t* count,
                      SWEEP_PAINT_CMD cmd) {
    if (list == (SWEEP_PAINT_CMD*)0 || count == (size_t*)0) {
        return false;
    }
    if (cmd.op < (uint8_t)SWEEP_PAINT_CLEAR ||
        cmd.op > (uint8_t)SWEEP_PAINT_IMAGE) {
        return false;
    }
    if (cmd.op == (uint8_t)SWEEP_PAINT_TEXT && cmd.glyph == 0u) {
        return false;
    }
    if (*count >= capacity) {
        return false;
    }
    list[*count] = cmd;
    ++(*count);
    return true;
}
