/* sweep_browser_test.c - host tests for Sweep browser foundation. */
#include <stdio.h>
#include <string.h>

#include "../sweep_browser.h"

static int g_failures = 0, g_passed = 0;
static void check(const char* label, int ok) {
    if (ok) { ++g_passed; printf("[PASS] %s\n", label); }
    else { ++g_failures; printf("[FAIL] %s\n", label); }
}

int main(void) {
    SWEEP_BROWSER b;
    uint32_t t1, t2;
    SWEEP_TAB* tab;

    sweep_browser_init(&b);
    check("init: no active tab", b.active_tab == 0u);
    check("init: web compat default", b.settings.default_text_mode == SWEEP_TEXT_WEB_COMPAT);
    check("init: native su allowed", b.settings.allow_native_superunicode);

    t1 = sweep_browser_new_tab(&b, 0);
    check("new tab created", t1 != 0u);
    check("active is new tab", b.active_tab == t1);
    tab = sweep_browser_find_tab(&b, t1);
    check("tab found", tab != 0);
    check("tab idle/blank", tab && tab->nav_state == SWEEP_NAV_IDLE);

    check("navigate https", sweep_browser_navigate(&b, t1, "https://example.com/"));
    tab = sweep_browser_find_tab(&b, t1);
    check("loading state", tab && tab->nav_state == SWEEP_NAV_LOADING);
    check("current url set", tab && strcmp(tab->current_url, "https://example.com/") == 0);
    check("history len 1", tab && tab->history_len == 1u);
    check("cannot go back yet", tab && !tab->can_go_back);

    sweep_tab_set_ready(tab, "Example", SWEEP_DOC_ENC_UNICODE_WEB);
    check("ready state", tab && tab->nav_state == SWEEP_NAV_READY);
    check("title updated", tab && strcmp(tab->title, "Example") == 0);
    check("encoding web", tab && tab->encoding == SWEEP_DOC_ENC_UNICODE_WEB);

    check("navigate second", sweep_browser_navigate(&b, t1, "https://example.org/page"));
    tab = sweep_browser_find_tab(&b, t1);
    check("can go back", tab && tab->can_go_back);
    check("history len 2", tab && tab->history_len == 2u);
    check("back works", sweep_browser_back(&b, t1));
    tab = sweep_browser_find_tab(&b, t1);
    check("back url", tab && strcmp(tab->current_url, "https://example.com/") == 0);
    check("can go forward", tab && tab->can_go_forward);
    check("forward works", sweep_browser_forward(&b, t1));
    tab = sweep_browser_find_tab(&b, t1);
    check("forward url", tab && strcmp(tab->current_url, "https://example.org/page") == 0);

    check("reload works", sweep_browser_reload(&b, t1));
    check("stop works", sweep_browser_stop(&b, t1));
    tab = sweep_browser_find_tab(&b, t1);
    check("stopped", tab && tab->nav_state == SWEEP_NAV_STOPPED);

    t2 = sweep_browser_new_tab(&b, "about:blank");
    check("second tab", t2 != 0u && t2 != t1);
    check("active switched to t2", b.active_tab == t2);
    check("switch to t1", sweep_browser_set_active(&b, t1));
    check("active is t1", b.active_tab == t1);

    check("address domain normalizes to https",
          sweep_browser_address_input(&b, t1, "example.net"));
    tab = sweep_browser_find_tab(&b, t1);
    check("normalized url", tab && strcmp(tab->current_url, "https://example.net") == 0);

    check("address search query",
          sweep_browser_address_input(&b, t1, "hello world"));
    tab = sweep_browser_find_tab(&b, t1);
    check("search uses provider", tab && strncmp(tab->current_url, "https://search.example/?q=", 26) == 0);

    check("reject ftp", !sweep_browser_navigate(&b, t1, "ftp://example.com/"));
    check("close t2", sweep_browser_close_tab(&b, t2));
    check("t2 gone", sweep_browser_find_tab(&b, t2) == 0);
    check("close t1", sweep_browser_close_tab(&b, t1));
    check("no active after close", b.active_tab == 0u);

    printf("=== Sweep browser: %d passed, %d failed ===\n", g_passed, g_failures);
    return g_failures ? 1 : 0;
}
