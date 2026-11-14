/* sweep_browser.c - browser foundation. */
#include "sweep_browser.h"

#include <string.h>

static void copy_str(char* dst, size_t cap, const char* src) {
    size_t n;
    if (!dst || cap == 0u) return;
    if (!src) { dst[0] = '\0'; return; }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool is_http_url(const char* s) {
    if (!s) return false;
    return (strncmp(s, "http://", 7) == 0) || (strncmp(s, "https://", 8) == 0);
}

void sweep_browser_init(SWEEP_BROWSER* b) {
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->next_tab_id = 1u;
    copy_str(b->settings.homepage, sizeof(b->settings.homepage), "about:blank");
    copy_str(b->settings.search_provider, sizeof(b->settings.search_provider), "https://search.example/?q=");
    b->settings.javascript_enabled = true;
    b->settings.default_text_mode = SWEEP_TEXT_WEB_COMPAT;
    b->settings.allow_native_superunicode = true;
}

SWEEP_TAB* sweep_browser_find_tab(SWEEP_BROWSER* b, uint32_t tab_id) {
    uint32_t i;
    if (!b || tab_id == 0u) return 0;
    for (i = 0u; i < SWEEP_MAX_TABS; ++i) {
        if (b->tabs[i].in_use && b->tabs[i].id == tab_id) return &b->tabs[i];
    }
    return 0;
}

const SWEEP_TAB* sweep_browser_active(const SWEEP_BROWSER* b) {
    uint32_t i;
    if (!b || b->active_tab == 0u) return 0;
    for (i = 0u; i < SWEEP_MAX_TABS; ++i) {
        if (b->tabs[i].in_use && b->tabs[i].id == b->active_tab) return &b->tabs[i];
    }
    return 0;
}

static void tab_update_nav_flags(SWEEP_TAB* t) {
    if (!t || t->history_len == 0u) { if (t) { t->can_go_back = false; t->can_go_forward = false; } return; }
    t->can_go_back = t->history_pos > 0u;
    t->can_go_forward = (t->history_pos + 1u) < t->history_len;
}

uint32_t sweep_browser_new_tab(SWEEP_BROWSER* b, const char* url) {
    uint32_t i;
    if (!b) return 0u;
    for (i = 0u; i < SWEEP_MAX_TABS; ++i) {
        if (!b->tabs[i].in_use) {
            SWEEP_TAB* t = &b->tabs[i];
            memset(t, 0, sizeof(*t));
            t->in_use = true;
            t->id = b->next_tab_id++;
            if (t->id == 0u) t->id = b->next_tab_id++;
            t->nav_state = SWEEP_NAV_IDLE;
            t->encoding = SWEEP_DOC_ENC_UNKNOWN;
            copy_str(t->title, sizeof(t->title), "New Tab");
            copy_str(t->current_url, sizeof(t->current_url), "about:blank");
            b->active_tab = t->id;
            b->tab_count++;
            if (url && url[0]) sweep_browser_navigate(b, t->id, url);
            return t->id;
        }
    }
    return 0u;
}

bool sweep_browser_close_tab(SWEEP_BROWSER* b, uint32_t tab_id) {
    SWEEP_TAB* t = sweep_browser_find_tab(b, tab_id);
    uint32_t i;
    if (!b || !t) return false;
    t->in_use = false;
    if (b->tab_count > 0u) b->tab_count--;
    if (b->active_tab == tab_id) {
        b->active_tab = 0u;
        for (i = 0u; i < SWEEP_MAX_TABS; ++i) {
            if (b->tabs[i].in_use) { b->active_tab = b->tabs[i].id; break; }
        }
    }
    return true;
}

bool sweep_browser_set_active(SWEEP_BROWSER* b, uint32_t tab_id) {
    if (!b || !sweep_browser_find_tab(b, tab_id)) return false;
    b->active_tab = tab_id;
    return true;
}

static void push_history(SWEEP_TAB* t, const char* url) {
    if (!t || !url) return;
    /* Navigating from middle truncates forward history. */
    if (t->history_len > 0u && t->history_pos + 1u < t->history_len) {
        t->history_len = t->history_pos + 1u;
    }
    if (t->history_len < SWEEP_MAX_HISTORY) {
        t->history_pos = t->history_len;
        t->history_len++;
    } else {
        /* drop oldest */
        memmove(&t->history[0], &t->history[1], (SWEEP_MAX_HISTORY - 1u) * sizeof(t->history[0]));
        t->history_pos = SWEEP_MAX_HISTORY - 1u;
    }
    copy_str(t->history[t->history_pos].url, sizeof(t->history[t->history_pos].url), url);
    copy_str(t->history[t->history_pos].title, sizeof(t->history[t->history_pos].title), url);
    t->history[t->history_pos].encoding = SWEEP_DOC_ENC_UNKNOWN;
    tab_update_nav_flags(t);
}

bool sweep_browser_navigate(SWEEP_BROWSER* b, uint32_t tab_id, const char* url) {
    SWEEP_TAB* t = sweep_browser_find_tab(b, tab_id);
    (void)b;
    if (!t || !url || !url[0]) return false;
    if (!(is_http_url(url) || strcmp(url, "about:blank") == 0)) return false;
    copy_str(t->current_url, sizeof(t->current_url), url);
    copy_str(t->title, sizeof(t->title), url);
    t->nav_state = SWEEP_NAV_LOADING;
    t->load_progress = 0u;
    t->encoding = SWEEP_DOC_ENC_UNKNOWN;
    push_history(t, url);
    return true;
}

bool sweep_browser_address_input(SWEEP_BROWSER* b, uint32_t tab_id, const char* input) {
    char buf[SWEEP_URL_TEXT_MAX];
    if (!b || !input || !input[0]) return false;
    if (is_http_url(input) || strcmp(input, "about:blank") == 0) {
        return sweep_browser_navigate(b, tab_id, input);
    }
    if (strchr(input, '.') && !strchr(input, ' ')) {
        /* domain-like: normalize to https */
        copy_str(buf, sizeof(buf), "https://");
        copy_str(buf + 8u, sizeof(buf) - 8u, input);
        return sweep_browser_navigate(b, tab_id, buf);
    }
    /* search query */
    copy_str(buf, sizeof(buf), b->settings.search_provider);
    copy_str(buf + strlen(buf), sizeof(buf) - strlen(buf), input);
    return sweep_browser_navigate(b, tab_id, buf);
}

bool sweep_browser_back(SWEEP_BROWSER* b, uint32_t tab_id) {
    SWEEP_TAB* t = sweep_browser_find_tab(b, tab_id);
    if (!t || !t->can_go_back || t->history_pos == 0u) return false;
    t->history_pos--;
    copy_str(t->current_url, sizeof(t->current_url), t->history[t->history_pos].url);
    copy_str(t->title, sizeof(t->title), t->history[t->history_pos].title);
    t->nav_state = SWEEP_NAV_LOADING;
    tab_update_nav_flags(t);
    return true;
}

bool sweep_browser_forward(SWEEP_BROWSER* b, uint32_t tab_id) {
    SWEEP_TAB* t = sweep_browser_find_tab(b, tab_id);
    if (!t || !t->can_go_forward || t->history_pos + 1u >= t->history_len) return false;
    t->history_pos++;
    copy_str(t->current_url, sizeof(t->current_url), t->history[t->history_pos].url);
    copy_str(t->title, sizeof(t->title), t->history[t->history_pos].title);
    t->nav_state = SWEEP_NAV_LOADING;
    tab_update_nav_flags(t);
    return true;
}

bool sweep_browser_reload(SWEEP_BROWSER* b, uint32_t tab_id) {
    SWEEP_TAB* t = sweep_browser_find_tab(b, tab_id);
    (void)b;
    if (!t || t->current_url[0] == '\0') return false;
    t->nav_state = SWEEP_NAV_LOADING;
    t->load_progress = 0u;
    return true;
}

bool sweep_browser_stop(SWEEP_BROWSER* b, uint32_t tab_id) {
    SWEEP_TAB* t = sweep_browser_find_tab(b, tab_id);
    (void)b;
    if (!t) return false;
    t->nav_state = SWEEP_NAV_STOPPED;
    return true;
}

void sweep_tab_set_loading(SWEEP_TAB* t, uint16_t progress) {
    if (!t) return;
    t->nav_state = SWEEP_NAV_LOADING;
    t->load_progress = progress > 1000u ? 1000u : progress;
}

void sweep_tab_set_ready(SWEEP_TAB* t, const char* title, SWEEP_DOC_ENCODING enc) {
    if (!t) return;
    t->nav_state = SWEEP_NAV_READY;
    t->load_progress = 1000u;
    t->encoding = enc;
    if (title) copy_str(t->title, sizeof(t->title), title);
    if (t->history_len > 0u && t->history_pos < t->history_len) {
        copy_str(t->history[t->history_pos].title, sizeof(t->history[t->history_pos].title), t->title);
        t->history[t->history_pos].encoding = enc;
    }
}

void sweep_tab_set_error(SWEEP_TAB* t, const char* title) {
    if (!t) return;
    t->nav_state = SWEEP_NAV_ERROR;
    if (title) copy_str(t->title, sizeof(t->title), title);
}
