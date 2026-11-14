/* kapa_dom_fixture.h - canned HTML fixture for the R7 KAPA integration test.
 *
 * Kept deliberately small and deterministic: it parses to a well-understood
 * tree and exercises every construct the runtime needs to prove end to end —
 * the document element, a head with a title, a body with a nested container,
 * a uniquely identifiable element carrying an attribute, a text node, and a
 * child element.  It exists so the KAPA app and any host-side regression can
 * share exactly one fixture; it is an input, not a second parser.
 */
#ifndef KAPA_DOM_FIXTURE_H
#define KAPA_DOM_FIXTURE_H

static const char kapa_dom_fixture[] =
    "<!doctype html>\n"
    "<html>\n"
    "<head>\n"
    "<title>Sweep R7</title>\n"
    "</head>\n"
    "<body>\n"
    "<div id=\"marker\" data-id=\"r7-marker\" class=\"probe\">hello "
    "<span class=\"inner\">sweep</span> dom</div>\n"
    "</body>\n"
    "</html>\n";

#define KAPA_DOM_FIXTURE_LEN (sizeof(kapa_dom_fixture) - 1u)

#endif /* KAPA_DOM_FIXTURE_H */
