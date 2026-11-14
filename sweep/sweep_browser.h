/* sweep_browser.h - Sweep browser foundation (tabs, navigation, session state).
 * Host-testable, bounded/static-friendly model. No dynamic allocation.
 */
#ifndef SWEEP_BROWSER_H
#define SWEEP_BROWSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sweep_text.h"

#define SWEEP_MAX_TABS 16u
#define SWEEP_MAX_HISTORY 32u
#define SWEEP_URL_TEXT_MAX 1024u
#define SWEEP_TITLE_MAX 256u

typedef enum _SWEEP_NAV_STATE {
    SWEEP_NAV_IDLE = 0,
    SWEEP_NAV_LOADING,
    SWEEP_NAV_READY,
    SWEEP_NAV_ERROR,
    SWEEP_NAV_STOPPED
} SWEEP_NAV_STATE;

typedef struct _SWEEP_HISTORY_ENTRY {
    char url[SWEEP_URL_TEXT_MAX];
    char title[SWEEP_TITLE_MAX];
    SWEEP_DOC_ENCODING encoding;
} SWEEP_HISTORY_ENTRY;

typedef struct _SWEEP_TAB {
    bool in_use;
    uint32_t id;
    SWEEP_NAV_STATE nav_state;
    SWEEP_DOC_ENCODING encoding; /* negotiated/selected for current document */
    char current_url[SWEEP_URL_TEXT_MAX];
    char title[SWEEP_TITLE_MAX];
    uint16_t load_progress; /* 0..1000 */
    SWEEP_HISTORY_ENTRY history[SWEEP_MAX_HISTORY];
    uint32_t history_len;
    uint32_t history_pos; /* index of current entry, valid when history_len>0 */
    bool can_go_back;
    bool can_go_forward;
} SWEEP_TAB;

typedef struct _SWEEP_BROWSER_SETTINGS {
    char homepage[SWEEP_URL_TEXT_MAX];
    char search_provider[SWEEP_URL_TEXT_MAX]; /* prefix, e.g. https://example/search?q= */
    bool javascript_enabled;
    SWEEP_TEXT_MODE default_text_mode; /* WEB_COMPAT by default */
    bool allow_native_superunicode;
} SWEEP_BROWSER_SETTINGS;

typedef struct _SWEEP_BROWSER {
    SWEEP_TAB tabs[SWEEP_MAX_TABS];
    uint32_t tab_count;
    uint32_t active_tab; /* 0 = none */
    uint32_t next_tab_id;
    SWEEP_BROWSER_SETTINGS settings;
} SWEEP_BROWSER;

void sweep_browser_init(SWEEP_BROWSER* b);
uint32_t sweep_browser_new_tab(SWEEP_BROWSER* b, const char* url);
bool sweep_browser_close_tab(SWEEP_BROWSER* b, uint32_t tab_id);
bool sweep_browser_set_active(SWEEP_BROWSER* b, uint32_t tab_id);
SWEEP_TAB* sweep_browser_find_tab(SWEEP_BROWSER* b, uint32_t tab_id);
const SWEEP_TAB* sweep_browser_active(const SWEEP_BROWSER* b);

/* Navigation: records history, sets loading state. Actual fetch is performed by
 * networking layer (not implemented here). Returns false on invalid/unsupported URL. */
bool sweep_browser_navigate(SWEEP_BROWSER* b, uint32_t tab_id, const char* url);
/* Address bar input: URL, or search query when not a navigable URL. */
bool sweep_browser_address_input(SWEEP_BROWSER* b, uint32_t tab_id, const char* input);
bool sweep_browser_back(SWEEP_BROWSER* b, uint32_t tab_id);
bool sweep_browser_forward(SWEEP_BROWSER* b, uint32_t tab_id);
bool sweep_browser_reload(SWEEP_BROWSER* b, uint32_t tab_id);
bool sweep_browser_stop(SWEEP_BROWSER* b, uint32_t tab_id);

/* Document lifecycle hooks (called by loader/parser/render integration). */
void sweep_tab_set_loading(SWEEP_TAB* t, uint16_t progress);
void sweep_tab_set_ready(SWEEP_TAB* t, const char* title, SWEEP_DOC_ENCODING enc);
void sweep_tab_set_error(SWEEP_TAB* t, const char* title);

#endif /* SWEEP_BROWSER_H */
