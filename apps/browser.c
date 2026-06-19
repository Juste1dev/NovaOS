

#include "../gui/gui.h"
#include "../gui/font.h"
#include "../drivers/vbe.h"
#include "../drivers/keyboard.h"
#include "../kernel/timer.h"
#include "../net/net.h"
#include "../net/webcache.h"
#include "../fs/vfs.h"
#include "nova_pkg.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

#define BROWSER_URL_LEN   512
#define BROWSER_LINE_LEN  512
#define BROWSER_MAX_LINES 256
#define BROWSER_HIST_MAX  32
#define BROWSER_BOOKMARKS 12
#define BROWSER_LIVE_FETCH_MAX 32768
#define BROWSER_RENDER_W 240
#define BROWSER_RENDER_H 135
#define BROWSER_RENDER_MAX (BROWSER_RENDER_W * BROWSER_RENDER_H)
#define BROWSER_COM2 0x2F8

typedef struct {
    char   url[BROWSER_URL_LEN];
    char   url_input[BROWSER_URL_LEN];
    int    url_input_len;
    int    url_focused;
    int    url_input_seeded;
    char   lines[BROWSER_MAX_LINES][BROWSER_LINE_LEN];
    int    line_count;
    int    scroll;
    int    loading;
    char   status[256];
    char   host[128];
    int    last_latency;
    window_t *win;
    int    win_id;
    int    need_redraw;
    char   history[BROWSER_HIST_MAX][BROWSER_URL_LEN];
    int    hist_pos;
    int    hist_count;
    char   bookmarks[BROWSER_BOOKMARKS][BROWSER_URL_LEN];
    int    bm_count;
    uint8_t render_pixels[BROWSER_RENDER_MAX];
    int    render_active;
    int    render_w;
    int    render_h;
    char   render_title[128];
    char   render_transport[128];
} browser_t;

static browser_t g_browser;
static int browser_open = 0;

static void br_add_line(browser_t *br, const char *line);
static void br_wrap_line(browser_t *br, const char *text);
static void br_status(browser_t *br, const char *msg);
static int br_build_rendered_page(browser_t *br, const char *url);
static int br_fetch_html(const char *url, char *out, int max, char *meta, int meta_max);
static int br_build_html_page(browser_t *br, const char *url, const char *html, const char *transport);

static int br_window_alive(browser_t *br) {
    if (!br || !browser_open || br->win_id < 0) return 0;
    for (int i = 0; i < gui.window_count; i++) {
        if (gui.windows[i].id == br->win_id) {
            br->win = &gui.windows[i];
            return 1;
        }
    }
    br->win = NULL;
    return 0;
}

static int br_strlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static void br_strcpy(char *d, const char *s, int m) {
    int i = 0;
    if (!d || m <= 0) return;
    while (s && s[i] && i < m - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void br_strcat(char *d, const char *s, int m) {
    int dl = br_strlen(d), i = 0;
    while (s && s[i] && dl + i < m - 1) { d[dl + i] = s[i]; i++; }
    d[dl + i] = 0;
}

static int br_starts_with(const char *s, const char *prefix) {
    while (*prefix) if (*s++ != *prefix++) return 0;
    return 1;
}

static int br_contains(const char *s, const char *needle) {
    return k_strstr(s, needle) != NULL;
}

static int br_ieq(const char *a, const char *b) {
    while (a && b && *a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return (!a || !*a) && (!b || !*b);
}

static int br_is_space(char c) {
    return c == ' ' || c == 9 || c == 10 || c == 13;
}

static void br_trim_copy(char *dst, const char *src, int max) {
    int start = 0, end = 0, len = 0;
    if (!dst || max <= 0) return;
    dst[0] = 0;
    if (!src) return;
    while (src[start] && br_is_space(src[start])) start++;
    end = start;
    while (src[end]) end++;
    while (end > start && br_is_space(src[end - 1])) end--;
    while (start < end && len < max - 1) dst[len++] = src[start++];
    dst[len] = 0;
}

static int br_has_space(const char *s) {
    for (int i = 0; s && s[i]; i++) if (br_is_space(s[i])) return 1;
    return 0;
}

static int br_is_ipv4ish(const char *s) {
    int dots = 0;
    if (!s || !s[0]) return 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '.') dots++;
        else if (!(c >= '0' && c <= '9')) return 0;
    }
    return dots == 3;
}

static int br_is_simple_word(const char *s) {
    int has_alpha = 0;
    if (!s || !s[0]) return 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) has_alpha = 1;
        else if ((c >= '0' && c <= '9') || c == '-' || c == '_') {}
        else return 0;
    }
    return has_alpha;
}

static void br_url_encode_component(char *dst, const char *src, int max) {
    static const char hex[] = "0123456789ABCDEF";
    int di = 0;
    if (!dst || max <= 0) return;
    dst[0] = 0;
    if (!src) return;
    for (int i = 0; src[i] && di < max - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = (char)c;
        } else if (c == ' ') {
            dst[di++] = '+';
        } else if (di < max - 3) {
            dst[di++] = '%';
            dst[di++] = hex[(c >> 4) & 0xF];
            dst[di++] = hex[c & 0xF];
        }
    }
    dst[di] = 0;
}

static const char *br_alias_url(const char *s) {
    if (!s || !s[0]) return "about:home";
    if (br_ieq(s, "home") || br_ieq(s, "start") || br_ieq(s, "accueil") || br_ieq(s, "new")) return "about:home";
    if (br_ieq(s, "help") || br_ieq(s, "aide")) return "about:help";
    if (br_ieq(s, "about") || br_ieq(s, "apropos") || br_ieq(s, "version")) return "about:about";
    if (br_ieq(s, "net") || br_ieq(s, "reseau") || br_ieq(s, "network")) return "about:net";
    if (br_ieq(s, "bookmarks") || br_ieq(s, "favoris")) return "about:bookmarks";
    if (br_ieq(s, "files") || br_ieq(s, "fichiers")) return "nova://files";
    if (br_ieq(s, "store") || br_ieq(s, "boutique")) return "nova://store";
    if (br_ieq(s, "tutorial") || br_ieq(s, "guide")) return "nova://tutorial";
    if (br_ieq(s, "release") || br_ieq(s, "notes") || br_ieq(s, "release-notes")) return "nova://release-notes";
    if (br_ieq(s, "google") || br_ieq(s, "g")) return "https://www.google.com";
    if (br_ieq(s, "gmail")) return "https://mail.google.com";
    if (br_ieq(s, "maps") || br_ieq(s, "maps.google")) return "https://maps.google.com";
    if (br_ieq(s, "youtube") || br_ieq(s, "yt")) return "https://www.youtube.com";
    if (br_ieq(s, "github") || br_ieq(s, "gh")) return "https://github.com";
    if (br_ieq(s, "wikipedia") || br_ieq(s, "wiki")) return "https://www.wikipedia.org";
    if (br_ieq(s, "news") || br_ieq(s, "hn") || br_ieq(s, "hackernews")) return "https://news.ycombinator.com";
    if (br_ieq(s, "example")) return "https://example.com";
    if (br_ieq(s, "firefox")) return "https://www.mozilla.org/firefox/new/";
    return NULL;
}

static void br_normalize_input(const char *src, char *dst, int max) {
    char trimmed[BROWSER_URL_LEN];
    char encoded[BROWSER_URL_LEN];
    const char *alias;
    if (!dst || max <= 0) return;
    br_trim_copy(trimmed, src, sizeof(trimmed));
    if (!trimmed[0]) { br_strcpy(dst, "about:home", max); return; }

    alias = br_alias_url(trimmed);
    if (alias) { br_strcpy(dst, alias, max); return; }

    if (br_starts_with(trimmed, "about:") || br_starts_with(trimmed, "nova://") || br_starts_with(trimmed, "file://") ||
        br_starts_with(trimmed, "http://") || br_starts_with(trimmed, "https://")) {
        br_strcpy(dst, trimmed, max);
        return;
    }

    if (trimmed[0] == '/' && trimmed[1]) {
        br_strcpy(dst, "file://", max);
        br_strcat(dst, trimmed, max);
        return;
    }

    if (br_starts_with(trimmed, "www.")) {
        br_strcpy(dst, "https://", max);
        br_strcat(dst, trimmed, max);
        return;
    }

    if (br_has_space(trimmed)) {
        br_url_encode_component(encoded, trimmed, sizeof(encoded));
        br_strcpy(dst, "https://www.google.com/search?q=", max);
        br_strcat(dst, encoded, max);
        return;
    }

    if (br_is_ipv4ish(trimmed) || br_ieq(trimmed, "localhost") || br_starts_with(trimmed, "localhost:") || br_starts_with(trimmed, "127.") || br_starts_with(trimmed, "10.") || br_starts_with(trimmed, "192.168.")) {
        br_strcpy(dst, "http://", max);
        br_strcat(dst, trimmed, max);
        return;
    }

    if (br_contains(trimmed, ".") || br_contains(trimmed, "/") || br_contains(trimmed, ":")) {
        br_strcpy(dst, "https://", max);
        br_strcat(dst, trimmed, max);
        return;
    }

    if (br_is_simple_word(trimmed)) {
        br_strcpy(dst, "https://www.", max);
        br_strcat(dst, trimmed, max);
        br_strcat(dst, ".com", max);
        return;
    }

    br_strcpy(dst, trimmed, max);
}

static void br_focus_url(browser_t *br, int prefer_empty) {
    if (!br) return;
    br->url_focused = 1;
    if (prefer_empty || !br->url[0] || br_ieq(br->url, "about:home")) {
        br->url_input[0] = 0;
        br->url_input_len = 0;
        br->url_input_seeded = 0;
    } else {
        br_strcpy(br->url_input, br->url, sizeof(br->url_input));
        br->url_input_len = br_strlen(br->url_input);
        br->url_input_seeded = 1;
    }
    br->need_redraw = 1;
}

static void br_extract_host(const char *url, char *host, int max) {
    int i = 0;
    const char *p = url;
    if (!host || max <= 0) return;
    host[0] = 0;
    if (!url || !url[0]) { br_strcpy(host, "nova.local", max); return; }

    if (br_starts_with(url, "about:") || br_starts_with(url, "nova:")) {
        br_strcpy(host, "nova.local", max);
        return;
    }

    const char *scheme = k_strstr(url, "://");
    if (scheme) p = scheme + 3;
    while (*p == '/') p++;
    while (p[i] && p[i] != '/' && p[i] != ':' && p[i] != '?' && p[i] != '#' && i < max - 1) {
        host[i] = p[i];
        i++;
    }
    host[i] = 0;
    if (!host[0]) br_strcpy(host, "nova.local", max);
}

static const nova_webcache_entry_t *br_find_cache_entry(const char *url_or_host) {
    char host[128];
    br_extract_host(url_or_host, host, sizeof(host));
    for (int i = 0; i < nova_webcache_entry_count; i++) {
        if (br_contains(host, nova_webcache_entries[i].host)) return &nova_webcache_entries[i];
    }
    return NULL;
}

static inline void br_outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}

static inline uint8_t br_inb(uint16_t p) {
    uint8_t v;
    __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));
    return v;
}

static int br_parse_int(const char *s) {
    int n = 0;
    while (s && *s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

static int br_parse_next_int(const char **ps) {
    int n = 0;
    const char *s = ps ? *ps : (const char *)0;
    while (s && *s == ' ') s++;
    while (s && *s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    if (ps) *ps = s;
    return n;
}

static void br_serial2_init(void) {
    static int initialized = 0;
    if (initialized) return;

    br_outb(BROWSER_COM2 + 1, 0x00);
    br_outb(BROWSER_COM2 + 3, 0x80);
    br_outb(BROWSER_COM2 + 0, 0x01);
    br_outb(BROWSER_COM2 + 1, 0x00);
    br_outb(BROWSER_COM2 + 3, 0x03);
    br_outb(BROWSER_COM2 + 2, 0xC7);
    br_outb(BROWSER_COM2 + 4, 0x0B);

    for (int i = 0; i < 128 && (br_inb(BROWSER_COM2 + 5) & 0x01); i++) (void)br_inb(BROWSER_COM2);
    initialized = 1;
}

static int br_serial2_tx_ready(void) { return br_inb(BROWSER_COM2 + 5) & 0x20; }
static int br_serial2_rx_ready(void) { return br_inb(BROWSER_COM2 + 5) & 0x01; }

static void br_serial2_flush_rx(void) {
    for (int i = 0; i < 512 && br_serial2_rx_ready(); i++) (void)br_inb(BROWSER_COM2);
}

static int br_serial2_putc(char c, uint32_t timeout_ms) {
    uint32_t start = timer_ms();
    while (!br_serial2_tx_ready()) {
        if (timer_ms() - start > timeout_ms) return 0;
    }
    br_outb(BROWSER_COM2, (uint8_t)c);
    return 1;
}

static int br_serial2_getc(char *c, uint32_t timeout_ms) {
    uint32_t start = timer_ms();
    while (!br_serial2_rx_ready()) {
        if (timer_ms() - start > timeout_ms) return 0;
    }
    *c = (char)br_inb(BROWSER_COM2);
    return 1;
}

static int br_serial2_read_line(char *buf, int max, uint32_t timeout_ms) {
    int pos = 0;
    char c = 0;
    if (!buf || max <= 1) return 0;
    while (pos < max - 1) {
        if (!br_serial2_getc(&c, timeout_ms)) break;
        if (c == '\r') continue;
        if (c == '\n') break;
        buf[pos++] = c;
    }
    buf[pos] = 0;
    return pos;
}

static int br_live_fetch(const char *url, char *out, int max, char *meta, int meta_max) {
    char header[128];
    int expected = 0;
    int got = 0;

    if (!url || !out || max <= 1) return -1;
    if (meta && meta_max > 0) meta[0] = 0;

    br_serial2_init();
    br_serial2_flush_rx();

    {
        const char *prefix = "FETCH ";
        for (int i = 0; prefix[i]; i++) if (!br_serial2_putc(prefix[i], 400)) return -1;
        for (int i = 0; url[i]; i++) if (!br_serial2_putc(url[i], 400)) return -1;
        if (!br_serial2_putc('\n', 400)) return -1;
    }

    if (!br_serial2_read_line(header, sizeof(header), 8000)) {
        if (meta && meta_max > 0) br_strcpy(meta, "Pont live indisponible", meta_max);
        return -1;
    }

    if (br_starts_with(header, "ERR ")) {
        if (meta && meta_max > 0) br_strcpy(meta, header + 4, meta_max);
        return -1;
    }
    if (!br_starts_with(header, "OK ")) {
        if (meta && meta_max > 0) br_strcpy(meta, "Reponse live invalide", meta_max);
        return -1;
    }

    expected = br_parse_int(header + 3);
    if (expected <= 0) {
        if (meta && meta_max > 0) br_strcpy(meta, "Reponse live vide", meta_max);
        return -1;
    }
    if (expected >= max) expected = max - 1;

    while (got < expected) {
        char c = 0;
        if (!br_serial2_getc(&c, 12000)) break;
        out[got++] = c;
    }
    out[got] = 0;

    if (meta && meta_max > 0) br_strcpy(meta, "HTTP/HTTPS live via pont hote QEMU (COM2)", meta_max);
    return got > 0 ? got : -1;
}

static int br_render_fetch(const char *url, browser_t *br, char *meta, int meta_max) {
    char header[128];
    const char *cursor;
    int width = 0, height = 0, expected = 0, got = 0;

    if (!url || !br) return 0;
    if (meta && meta_max > 0) meta[0] = 0;

    br_serial2_init();
    br_serial2_flush_rx();

    {
        const char *prefix = "RENDER ";
        for (int i = 0; prefix[i]; i++) if (!br_serial2_putc(prefix[i], 400)) return 0;
        for (int i = 0; url[i]; i++) if (!br_serial2_putc(url[i], 400)) return 0;
        if (!br_serial2_putc(' ', 400)) return 0;
        {
            const char *w = "240";
            for (int i = 0; w[i]; i++) if (!br_serial2_putc(w[i], 400)) return 0;
        }
        if (!br_serial2_putc(' ', 400)) return 0;
        {
            const char *h = "135";
            for (int i = 0; h[i]; i++) if (!br_serial2_putc(h[i], 400)) return 0;
        }
        if (!br_serial2_putc('\n', 400)) return 0;
    }

    if (!br_serial2_read_line(header, sizeof(header), 20000)) {
        if (meta && meta_max > 0) br_strcpy(meta, "Pont de rendu indisponible", meta_max);
        return 0;
    }
    if (br_starts_with(header, "ERR ")) {
        if (meta && meta_max > 0) br_strcpy(meta, header + 4, meta_max);
        return 0;
    }
    if (!br_starts_with(header, "OK ")) {
        if (meta && meta_max > 0) br_strcpy(meta, "Reponse rendu invalide", meta_max);
        return 0;
    }

    cursor = header + 3;
    width = br_parse_next_int(&cursor);
    height = br_parse_next_int(&cursor);
    expected = br_parse_next_int(&cursor);
    if (width <= 0 || height <= 0 || expected <= 0 || expected > BROWSER_RENDER_MAX) {
        if (meta && meta_max > 0) br_strcpy(meta, "Taille de rendu invalide", meta_max);
        return 0;
    }
    if (!br_serial2_read_line(br->render_title, sizeof(br->render_title), 8000)) br_strcpy(br->render_title, url, sizeof(br->render_title));
    while (got < expected) {
        char c = 0;
        if (!br_serial2_getc(&c, 20000)) break;
        br->render_pixels[got++] = (uint8_t)c;
    }
    if (got != expected) {
        if (meta && meta_max > 0) br_strcpy(meta, "Flux de rendu incomplet", meta_max);
        return 0;
    }
    br->render_w = width;
    br->render_h = height;
    br->render_active = 1;
    br_strcpy(br->render_transport, "Rendu HTML/CSS/JS complet via navigateur hote", sizeof(br->render_transport));
    if (meta && meta_max > 0) br_strcpy(meta, br->render_transport, meta_max);
    return 1;
}

static color_t br_rgb332_to_color(uint8_t v) {
    uint8_t r = (uint8_t)(((v >> 5) & 0x07u) * 255u / 7u);
    uint8_t g = (uint8_t)(((v >> 2) & 0x07u) * 255u / 7u);
    uint8_t b = (uint8_t)((v & 0x03u) * 255u / 3u);
    return RGB(r, g, b);
}

static void br_draw_render_preview(browser_t *br, int x, int y, int w, int h) {
    int dw, dh, ox, oy;
    if (!br || !br->render_active || br->render_w <= 0 || br->render_h <= 0) return;
    dw = w;
    dh = (w * br->render_h) / br->render_w;
    if (dh > h) {
        dh = h;
        dw = (h * br->render_w) / br->render_h;
    }
    ox = x + (w - dw) / 2;
    oy = y + (h - dh) / 2;

    vbe_blend_rounded_rect(x, y, w, h, 18, RGB(236,242,252), 255);
    vbe_rounded_rect_outline(x, y, w, h, 18, 1, RGB(200,210,235));
    vbe_blend_rect(ox, oy, dw, dh, RGB(255,255,255), 255);
    vbe_blend_rect(ox, oy, dw, 32, RGB(246,248,252), 255);
    vbe_blend_rect(ox, oy + 31, dw, 1, RGB(218,225,238), 255);
    vbe_blend_rounded_rect(ox + 14, oy + 9, 12, 12, 6, RGB(255,95,86), 230);
    vbe_blend_rounded_rect(ox + 32, oy + 9, 12, 12, 6, RGB(255,189,46), 230);
    vbe_blend_rounded_rect(ox + 50, oy + 9, 12, 12, 6, RGB(39,201,63), 230);
    if (br->render_title[0]) font_draw_string(ox + 78, oy + 9, br->render_title, RGB(70,84,118), COLOR_TRANS, FONT_SMALL);

    for (int py = 32; py < dh; py++) {
        int sy = (py * br->render_h) / (dh ? dh : 1);
        for (int px = 0; px < dw; px++) {
            int sx = (px * br->render_w) / (dw ? dw : 1);
            uint8_t sample = br->render_pixels[sy * br->render_w + sx];
            vbe_put_pixel(ox + px, oy + py, br_rgb332_to_color(sample));
        }
    }

    vbe_rounded_rect_outline(ox, oy, dw, dh, 14, 1, RGB(188,198,220));
}

static int br_bridge_request(const char *command, const char *arg, char *out, int max, char *meta, int meta_max) {
    char header[128];
    int expected = 0;
    int got = 0;

    if (!command || !out || max <= 1) return -1;
    if (meta && meta_max > 0) meta[0] = 0;

    br_serial2_init();
    br_serial2_flush_rx();

    for (int i = 0; command[i]; i++) if (!br_serial2_putc(command[i], 400)) return -1;
    if (arg && arg[0]) {
        if (!br_serial2_putc(' ', 400)) return -1;
        for (int i = 0; arg[i]; i++) if (!br_serial2_putc(arg[i], 400)) return -1;
    }
    if (!br_serial2_putc('\n', 400)) return -1;

    if (!br_serial2_read_line(header, sizeof(header), 12000)) {
        if (meta && meta_max > 0) br_strcpy(meta, "Pont store indisponible", meta_max);
        return -1;
    }
    if (br_starts_with(header, "ERR ")) {
        if (meta && meta_max > 0) br_strcpy(meta, header + 4, meta_max);
        return -1;
    }
    if (!br_starts_with(header, "OK ")) {
        if (meta && meta_max > 0) br_strcpy(meta, "Reponse store invalide", meta_max);
        return -1;
    }

    expected = br_parse_int(header + 3);
    if (expected <= 0) {
        if (meta && meta_max > 0) br_strcpy(meta, "Reponse store vide", meta_max);
        return -1;
    }
    if (expected >= max) expected = max - 1;

    while (got < expected) {
        char c = 0;
        if (!br_serial2_getc(&c, 15000)) break;
        out[got++] = c;
    }
    out[got] = 0;
    if (meta && meta_max > 0) br_strcpy(meta, "Flathub live via helper QEMU", meta_max);
    return got > 0 ? got : -1;
}

static void br_url_decode_path(char *dst, const char *src, int max) {
    int di = 0;
    if (!dst || max <= 0) return;
    if (!src) { dst[0] = 0; return; }
    for (int i = 0; src[i] && di < max - 1; i++) {
        if (src[i] == '+') {
            dst[di++] = ' ';
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char a = src[i + 1], b = src[i + 2];
            int hi = (a >= '0' && a <= '9') ? (a - '0') : ((a >= 'A' && a <= 'F') ? (a - 'A' + 10) : ((a >= 'a' && a <= 'f') ? (a - 'a' + 10) : -1));
            int lo = (b >= '0' && b <= '9') ? (b - '0') : ((b >= 'A' && b <= 'F') ? (b - 'A' + 10) : ((b >= 'a' && b <= 'f') ? (b - 'a' + 10) : -1));
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                dst[di++] = src[i];
            }
        } else {
            dst[di++] = src[i];
        }
    }
    dst[di] = 0;
}

static void br_safe_filename(const char *appid, char *out, int max) {
    int j = 0;
    if (!out || max <= 0) return;
    if (!appid) { out[0] = 0; return; }
    for (int i = 0; appid[i] && j < max - 1; i++) {
        char c = appid[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') out[j++] = c;
        else out[j++] = '_';
    }
    out[j] = 0;
}

static int br_fetch_html(const char *url, char *out, int max, char *meta, int meta_max) {
    int got;
    got = br_bridge_request("HTML", url, out, max, meta, meta_max);
    if (got > 0 && meta && meta_max > 0) br_strcpy(meta, "HTML brut via pont hote QEMU (COM2)", meta_max);
    return got;
}

static int br_ends_with_ci(const char *s, const char *suffix) {
    int ls = br_strlen(s);
    int lf = br_strlen(suffix);
    if (lf <= 0 || ls < lf) return 0;
    return br_ieq(s + ls - lf, suffix);
}

static int br_is_html_like(const char *text) {
    if (!text || !text[0]) return 0;
    return br_contains(text, "<!doctype html") || br_contains(text, "<html") || br_contains(text, "<body") || br_contains(text, "<head") || br_contains(text, "<div") || br_contains(text, "<p") || br_contains(text, "<h1") || br_contains(text, "<title");
}

static int br_path_is_html(const char *path) {
    if (!path) return 0;
    return br_ends_with_ci(path, ".html") || br_ends_with_ci(path, ".htm") || br_ends_with_ci(path, ".xhtml");
}

static int br_tolower_ascii(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

static int br_tag_eq(const char *a, const char *b) {
    int i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) {
        if (br_tolower_ascii(a[i]) != br_tolower_ascii(b[i])) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static int br_match_ci_prefix(const char *s, const char *needle) {
    int i = 0;
    if (!s || !needle) return 0;
    while (needle[i]) {
        if (!s[i]) return 0;
        if (br_tolower_ascii(s[i]) != br_tolower_ascii(needle[i])) return 0;
        i++;
    }
    return 1;
}

static int br_find_ci(const char *haystack, const char *needle) {
    int nlen = br_strlen(needle);
    if (!haystack || !needle || nlen <= 0) return -1;
    for (int i = 0; haystack[i]; i++) {
        int ok = 1;
        for (int j = 0; j < nlen; j++) {
            if (!haystack[i + j] || br_tolower_ascii(haystack[i + j]) != br_tolower_ascii(needle[j])) { ok = 0; break; }
        }
        if (ok) return i;
    }
    return -1;
}

static void br_html_push_char(char *dst, int *di, int max, char c) {
    if (!dst || !di || *di >= max - 1) return;
    dst[(*di)++] = c;
    dst[*di] = 0;
}

static void br_html_push_text(char *dst, int *di, int max, const char *text) {
    for (int i = 0; text && text[i] && *di < max - 1; i++) br_html_push_char(dst, di, max, text[i]);
}

static void br_html_break(char *dst, int *di, int max) {
    if (!dst || !di || max <= 1) return;
    if (*di > 0 && dst[*di - 1] != '\n') br_html_push_char(dst, di, max, '\n');
    if (*di > 0 && dst[*di - 1] == '\n' && *di >= 2 && dst[*di - 2] != '\n') br_html_push_char(dst, di, max, '\n');
}

static void br_html_space(char *dst, int *di, int max) {
    if (!dst || !di || max <= 1) return;
    if (*di == 0) return;
    if (dst[*di - 1] != ' ' && dst[*di - 1] != '\n') br_html_push_char(dst, di, max, ' ');
}

static int br_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int br_html_decode_entity(const char *src, char *out, int *consumed) {
    int code = 0;
    int i = 1;
    if (!src || src[0] != '&' || !out || !consumed) return 0;
    if (src[1] == '#') {
        int base = 10;
        i = 2;
        if (src[i] == 'x' || src[i] == 'X') { base = 16; i++; }
        if (!src[i]) return 0;
        while (src[i] && src[i] != ';') {
            int digit = (base == 16) ? br_hex_digit(src[i]) : ((src[i] >= '0' && src[i] <= '9') ? src[i] - '0' : -1);
            if (digit < 0) return 0;
            code = code * base + digit;
            i++;
        }
        if (src[i] != ';') return 0;
        *out = (code >= 32 && code < 127) ? (char)code : ' ';
        *consumed = i + 1;
        return 1;
    }
    if (br_match_ci_prefix(src, "&amp;"))  { *out = '&';  *consumed = 5; return 1; }
    if (br_match_ci_prefix(src, "&lt;"))   { *out = '<';  *consumed = 4; return 1; }
    if (br_match_ci_prefix(src, "&gt;"))   { *out = '>';  *consumed = 4; return 1; }
    if (br_match_ci_prefix(src, "&quot;")) { *out = '"'; *consumed = 6; return 1; }
    if (br_match_ci_prefix(src, "&#39;"))  { *out = '\''; *consumed = 5; return 1; }
    if (br_match_ci_prefix(src, "&apos;")) { *out = '\''; *consumed = 6; return 1; }
    if (br_match_ci_prefix(src, "&nbsp;")) { *out = ' '; *consumed = 6; return 1; }
    return 0;
}

static void br_html_extract_title(const char *html, char *title, int max) {
    int start, end, len;
    if (!title || max <= 0) return;
    title[0] = 0;
    if (!html) return;
    start = br_find_ci(html, "<title");
    if (start < 0) return;
    while (html[start] && html[start] != '>') start++;
    if (html[start] != '>') return;
    start++;
    end = br_find_ci(html + start, "</title>");
    if (end < 0) return;
    len = end;
    if (len >= max) len = max - 1;
    for (int i = 0; i < len; i++) title[i] = html[start + i];
    title[len] = 0;
    br_trim_copy(title, title, max);
}

static void br_html_tag_name(const char *src, char *tag, int max, int *closing) {
    int i = 0;
    const char *p = src;
    if (!tag || max <= 0) return;
    tag[0] = 0;
    if (closing) *closing = 0;
    if (!src) return;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '/') { if (closing) *closing = 1; p++; }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    while (*p && *p != '>' && *p != '/' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && i < max - 1) {
        tag[i++] = (char)br_tolower_ascii(*p++);
    }
    tag[i] = 0;
}

static void br_html_render_to_text(const char *html, char *out, int max) {
    int di = 0;
    int in_pre = 0;
    char title[128];
    if (!out || max <= 0) return;
    out[0] = 0;
    if (!html) return;

    br_html_extract_title(html, title, sizeof(title));
    if (title[0]) {
        br_html_push_text(out, &di, max, "# ");
        br_html_push_text(out, &di, max, title);
        br_html_break(out, &di, max);
    }

    for (int i = 0; html[i] && di < max - 1; ) {
        if (html[i] == '<') {
            char tag[32];
            int closing = 0;
            int j = i + 1;
            if (html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
                i += 4;
                while (html[i] && !(html[i] == '-' && html[i + 1] == '-' && html[i + 2] == '>')) i++;
                if (html[i]) i += 3;
                continue;
            }
            while (html[j] && html[j] != '>') j++;
            if (!html[j]) break;
            br_html_tag_name(html + i + 1, tag, sizeof(tag), &closing);
            if (!closing && (br_tag_eq(tag, "script") || br_tag_eq(tag, "style") || br_tag_eq(tag, "noscript"))) {
                i = j + 1;
                while (html[i]) {
                    if (html[i] == '<' && html[i + 1] == '/') {
                        char close_tag[32];
                        int tmp = 0;
                        br_html_tag_name(html + i + 1, close_tag, sizeof(close_tag), &tmp);
                        if (br_tag_eq(close_tag, tag)) {
                            while (html[i] && html[i] != '>') i++;
                            if (html[i] == '>') i++;
                            break;
                        }
                    }
                    i++;
                }
                continue;
            }
            if (br_tag_eq(tag, "br") || br_tag_eq(tag, "p") || br_tag_eq(tag, "div") || br_tag_eq(tag, "section") || br_tag_eq(tag, "article") || br_tag_eq(tag, "main") || br_tag_eq(tag, "header") || br_tag_eq(tag, "footer") || br_tag_eq(tag, "aside") || br_tag_eq(tag, "tr") || br_tag_eq(tag, "table")) {
                br_html_break(out, &di, max);
            } else if (br_tag_eq(tag, "li")) {
                br_html_break(out, &di, max);
                br_html_push_text(out, &di, max, "- ");
            } else if (br_tag_eq(tag, "h1") || br_tag_eq(tag, "h2") || br_tag_eq(tag, "h3") || br_tag_eq(tag, "h4") || br_tag_eq(tag, "h5") || br_tag_eq(tag, "h6")) {
                br_html_break(out, &di, max);
                br_html_push_text(out, &di, max, "# ");
            } else if (br_tag_eq(tag, "pre") || br_tag_eq(tag, "code")) {
                br_html_break(out, &di, max);
                if (!closing) in_pre = 1;
                else in_pre = 0;
            }
            i = j + 1;
            continue;
        }
        if (html[i] == '&') {
            char decoded = 0;
            int consumed = 0;
            if (br_html_decode_entity(html + i, &decoded, &consumed)) {
                if (decoded == ' ') br_html_space(out, &di, max);
                else br_html_push_char(out, &di, max, decoded);
                i += consumed;
                continue;
            }
        }
        if (!in_pre && (html[i] == '\n' || html[i] == '\r' || html[i] == '\t' || html[i] == ' ')) {
            br_html_space(out, &di, max);
            i++;
            continue;
        }
        br_html_push_char(out, &di, max, html[i]);
        i++;
    }
    while (di > 0 && (out[di - 1] == ' ' || out[di - 1] == '\n' || out[di - 1] == '\r' || out[di - 1] == '\t')) di--;
    out[di] = 0;
}

static int br_build_html_page(browser_t *br, const char *url, const char *html, const char *transport) {
    static char rendered[BROWSER_LIVE_FETCH_MAX];
    char title[128];
    char line[640];
    if (!br || !html || !html[0]) return 0;
    br_html_render_to_text(html, rendered, sizeof(rendered));
    if (!rendered[0]) return 0;
    br_html_extract_title(html, title, sizeof(title));

    br_add_line(br, "# Page HTML");
    br_add_line(br, "");
    if (url && url[0]) {
        k_memset(line, 0, sizeof(line));
        br_strcpy(line, "Adresse : ", sizeof(line)); br_strcat(line, url, sizeof(line)); br_add_line(br, line);
    }
    if (title[0]) {
        k_memset(line, 0, sizeof(line));
        br_strcpy(line, "Titre : ", sizeof(line)); br_strcat(line, title, sizeof(line)); br_add_line(br, line);
    }
    if (transport && transport[0]) {
        k_memset(line, 0, sizeof(line));
        br_strcpy(line, "Moteur : ", sizeof(line)); br_strcat(line, transport, sizeof(line)); br_add_line(br, line);
    }
    br_add_line(br, "Rendu : interpretation HTML native simplifiee du navigateur local.");
    br_add_line(br, "");
    br_wrap_line(br, rendered);
    br_status(br, "Page HTML chargee");
    return 1;
}

static void br_store_home(browser_t *br) {
    br_add_line(br, "# Applications");
    br_add_line(br, "");
    br_add_line(br, "Catalogue d'applications et de raccourcis .nova.");
    br_add_line(br, "Chaque package .nova installe un lanceur web et cree les raccourcis Bureau + menu Demarrer.");
    br_add_line(br, "");
    br_add_line(br, "Acces rapides:");
    br_add_line(br, "- nova://store/search/firefox");
    br_add_line(br, "- nova://store/search/github");
    br_add_line(br, "- nova://store/search/youtube");
    br_add_line(br, "- nova://store/app/org.mozilla.firefox");
    br_add_line(br, "- nova://store/app/com.github.desktop");
    br_add_line(br, "- nova://store/app/com.spotify.web");
    br_add_line(br, "");
    br_add_line(br, "Installation:");
    br_add_line(br, "- ouvrez une fiche app puis allez sur nova://store/install/<app-id>");
    br_add_line(br, "- le package .nova est telecharge dans /home/user/Downloads/");
    br_add_line(br, "- il est ensuite installe automatiquement avec creation des raccourcis");
    br_add_line(br, "- exemple: nova://store/install/org.mozilla.firefox");
    br_add_line(br, "");
    br_add_line(br, "Note : les packages .nova installent des raccourcis compatibles avec le navigateur integre.");
    br_status(br, "Catalogue pret");
}

static int br_store_search(browser_t *br, const char *query) {
    static char payload[BROWSER_LIVE_FETCH_MAX];
    char meta[128];
    int got;
    got = br_bridge_request("STORE SEARCH", query && query[0] ? query : "firefox", payload, sizeof(payload), meta, sizeof(meta));
    br_add_line(br, "# Applications");
    br_add_line(br, "");
    if (got <= 0) {
        br_add_line(br, "Le service Applications n'a pas repondu.");
        br_add_line(br, meta[0] ? meta : "Erreur inconnue");
        br_add_line(br, "Essayez de relancer QEMU avec le helper live_fetch_bridge.py.");
        br_status(br, "Catalogue indisponible");
        return 0;
    }
    br_wrap_line(br, payload);
    br_status(br, "Resultats du catalogue charges");
    return 1;
}

static int br_store_details(browser_t *br, const char *appid) {
    static char payload[BROWSER_LIVE_FETCH_MAX];
    char meta[128];
    int got;
    got = br_bridge_request("STORE DETAILS", appid, payload, sizeof(payload), meta, sizeof(meta));
    br_add_line(br, "# Applications");
    br_add_line(br, "");
    if (got <= 0) {
        br_add_line(br, "Impossible de recuperer la fiche application.");
        br_add_line(br, meta[0] ? meta : "Erreur inconnue");
        br_status(br, "Fiche app indisponible");
        return 0;
    }
    br_wrap_line(br, payload);
    br_status(br, "Fiche application chargee");
    return 1;
}

static int br_store_install(browser_t *br, const char *appid) {
    static char payload[BROWSER_LIVE_FETCH_MAX];
    char meta[128];
    char decoded[256];
    char safe[256];
    char path[384];
    char status[192];
    int got;

    br_url_decode_path(decoded, appid, sizeof(decoded));
    got = br_bridge_request("STORE INSTALL", decoded, payload, sizeof(payload), meta, sizeof(meta));
    br_add_line(br, "# Applications");
    br_add_line(br, "");
    if (got <= 0) {
        br_add_line(br, "Telechargement du package .nova impossible.");
        br_add_line(br, meta[0] ? meta : "Erreur inconnue");
        br_status(br, "Telechargement echoue");
        return 0;
    }

    vfs_mkdir("/home/user/Downloads");
    br_safe_filename(decoded, safe, sizeof(safe));
    k_memset(path, 0, sizeof(path));
    br_strcpy(path, "/home/user/Downloads/", sizeof(path));
    br_strcat(path, safe, sizeof(path));
    br_strcat(path, ".nova", sizeof(path));
    (void)vfs_write_file(path, payload, (uint32_t)got);

    if (!nova_pkg_install_from_text(payload, status, sizeof(status))) {
        br_add_line(br, "Le package .nova a ete telecharge mais l'installation a echoue.");
        br_add_line(br, status[0] ? status : "Erreur inconnue");
        br_status(br, "Installation echouee");
        return 0;
    }

    gui_refresh_shortcuts();
    br_add_line(br, "Package .nova telecharge puis installe avec succes.");
    br_add_line(br, "");
    br_add_line(br, "Package sauvegarde dans Downloads:");
    br_add_line(br, path);
    br_add_line(br, "");
    br_add_line(br, "Resultat installation:");
    br_add_line(br, status);
    br_add_line(br, "");
    br_add_line(br, "Raccourcis crees:");
    br_add_line(br, "- Bureau : /home/user/Desktop/");
    br_add_line(br, "- Menu Demarrer : /system/menu/");
    br_add_line(br, "");
    br_add_line(br, "Apercu du manifeste .nova:");
    br_wrap_line(br, payload);
    br_status(br, "Package .nova installe et raccourcis crees");
    return 1;
}

static void br_add_line(browser_t *br, const char *line) {
    if (!br || br->line_count >= BROWSER_MAX_LINES) return;
    br_strcpy(br->lines[br->line_count++], line ? line : "", BROWSER_LINE_LEN);
}

static void br_wrap_line(browser_t *br, const char *text) {
    char buf[BROWSER_LINE_LEN];
    int bi = 0;
    int last_space = -1;
    if (!text) { br_add_line(br, ""); return; }

    for (int i = 0; text[i]; i++) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n') {
            buf[bi] = 0;
            br_add_line(br, buf);
            bi = 0;
            last_space = -1;
            continue;
        }
        if (bi < BROWSER_LINE_LEN - 2) {
            buf[bi++] = c;
            buf[bi] = 0;
            if (c == ' ') last_space = bi - 1;
        }
        if (bi >= 78) {
            if (last_space > 0) {
                buf[last_space] = 0;
                br_add_line(br, buf);
                int rem = bi - last_space - 1;
                for (int j = 0; j < rem; j++) buf[j] = buf[last_space + 1 + j];
                bi = rem;
            } else {
                buf[bi] = 0;
                br_add_line(br, buf);
                bi = 0;
            }
            buf[bi] = 0;
            last_space = -1;
        }
        if (br->line_count >= BROWSER_MAX_LINES - 2) break;
    }

    if (bi > 0 && br->line_count < BROWSER_MAX_LINES) {
        buf[bi] = 0;
        br_add_line(br, buf);
    }
}

static void br_reset_page(browser_t *br) {
    br->line_count = 0;
    br->scroll = 0;
    br->loading = 0;
    br->render_active = 0;
    br->render_w = 0;
    br->render_h = 0;
    br->render_title[0] = 0;
    br->render_transport[0] = 0;
    br->need_redraw = 1;
}

static void br_status(browser_t *br, const char *msg) {
    br_strcpy(br->status, msg, 256);
}

static void br_prepare_navigation(browser_t *br, const char *url) {
    char normalized[BROWSER_URL_LEN];
    if (!br) return;
    br_normalize_input(url, normalized, sizeof(normalized));
    br_strcpy(br->url, normalized, sizeof(br->url));
}

static void br_build_home(browser_t *br) {
    br_add_line(br, "# Navigateur");
    br_add_line(br, "");
    br_add_line(br, "Navigation locale et web.");
    br_add_line(br, "");
    br_add_line(br, "- Pages internes : about:home, about:help, about:about, about:net, about:bookmarks");
    br_add_line(br, "- Pages locales : nova://files");
    br_add_line(br, "- Barre d'adresse : tapez google, github, wiki, youtube, gmail, maps ou une phrase, puis Entree.");
    br_add_line(br, "- Les mots simples deviennent automatiquement des domaines web; les phrases lancent une recherche web.");
    br_add_line(br, "- Pages web réelles : rendu HTML/CSS/JS via moteur navigateur hôte, avec repli HTML natif simplifié, texte live et cache local.");
    br_add_line(br, "");
    br_add_line(br, "Accès rapide :");
    br_add_line(br, "- https://github.com");
    br_add_line(br, "- https://www.wikipedia.org");
    br_add_line(br, "- https://news.ycombinator.com");
    br_add_line(br, "- https://example.com");
    br_status(br, "Prêt");
}

static void br_build_help(browser_t *br) {
    br_add_line(br, "# Aide");
    br_add_line(br, "");
    br_add_line(br, "Aide du navigateur.");
    br_add_line(br, "");
    br_add_line(br, "- Tapez directement google, github, wiki, youtube, gmail, maps ou firefox");
    br_add_line(br, "- Tapez une phrase complete pour lancer une recherche web automatiquement");
    br_add_line(br, "- Ctrl+L : placer le focus dans la barre d'adresse (la première frappe remplace l'adresse courante)");
    br_add_line(br, "- Ctrl+R : recharger la vue courante");
    br_add_line(br, "- Ctrl+D : mémoriser la page dans les favoris");
    br_add_line(br, "- Alt+← / Alt+→ : revenir ou avancer dans l'historique");
    br_add_line(br, "- ↑ / ↓ / PgUp / PgDn : défiler");
    br_add_line(br, "");
    br_status(br, "Aide affichée");
}

static void br_build_about(browser_t *br) {
    br_add_line(br, "# À propos");
    br_add_line(br, "");
    br_add_line(br, "Navigateur local");
    br_add_line(br, "Historique, favoris, diagnostic réseau et lecture de fichiers texte.");
    br_add_line(br, "");
    br_add_line(br, "Fonctions actives :");
    br_add_line(br, "- Barre d'adresse éditable");
    br_add_line(br, "- Historique arrière / avant");
    br_add_line(br, "- Favoris persistants pendant la session");
    br_add_line(br, "- Lecteur file:/// pour les fichiers texte du VFS");
    br_add_line(br, "- Rendu live HTML/CSS/JS complet via navigateur hote QEMU");
    br_add_line(br, "- Repli automatique vers un moteur HTML natif simplifie, puis vers le cache local si le pont live est indisponible");
    br_status(br, "Informations version");
}

static void br_build_net(browser_t *br) {
    char ip[20], mac[20], dns[20], gw[20];
    net_get_ip_str(net_eth0.ip, ip);
    net_get_ip_str(net_eth0.dns, dns);
    net_get_ip_str(net_eth0.gateway, gw);
    net_get_mac_str(net_eth0.mac, mac);

    br_add_line(br, "# Réseau");
    br_add_line(br, "");
    br_add_line(br, net_eth0.connected ? "Lien actif sur eth0" : "Aucun lien actif sur eth0");
    br_add_line(br, "");
    br_wrap_line(br, "DHCP : activé sur une configuration compatible QEMU user networking.");
    br_wrap_line(br, "IP : "); br->line_count--;

    {
        char line[128];
        k_memset(line, 0, sizeof(line));
        br_strcpy(line, "Adresse IP : ", 128); br_strcat(line, ip, 128); br_add_line(br, line);
        k_memset(line, 0, sizeof(line)); br_strcpy(line, "Passerelle : ", 128); br_strcat(line, gw, 128); br_add_line(br, line);
        k_memset(line, 0, sizeof(line)); br_strcpy(line, "DNS : ", 128); br_strcat(line, dns, 128); br_add_line(br, line);
        k_memset(line, 0, sizeof(line)); br_strcpy(line, "MAC : ", 128); br_strcat(line, mac, 128); br_add_line(br, line);
    }

    br_add_line(br, "");
    br_add_line(br, "Pont navigateur : COM2 vers service QEMU cote hote.");
    br_add_line(br, "Essais rapides :");
    br_add_line(br, "- https://github.com");
    br_add_line(br, "- https://www.wikipedia.org");
    br_add_line(br, "- https://news.ycombinator.com");
    br_add_line(br, "- https://example.com");
    br_status(br, "État réseau local");
}

static void br_build_bookmarks(browser_t *br) {
    br_add_line(br, "# Favoris");
    br_add_line(br, "");
    if (br->bm_count == 0) {
        br_add_line(br, "Aucun favori enregistré pour cette session.");
        br_add_line(br, "Ajoutez-en un avec Ctrl+D.");
    } else {
        for (int i = 0; i < br->bm_count; i++) {
            char line[640];
            k_memset(line, 0, sizeof(line));
            br_strcpy(line, "- ", sizeof(line));
            br_strcat(line, br->bookmarks[i], sizeof(line));
            br_add_line(br, line);
        }
    }
    br_status(br, "Liste des favoris");
}

static void br_build_file(browser_t *br, const char *url) {
    const char *path = url;
    char preview[2048];
    if (br_starts_with(url, "file://")) path = url + 7;
    if (!path[0]) path = "/home/user/readme.txt";

    br_add_line(br, "# Lecteur de fichiers");
    br_add_line(br, "");
    br_add_line(br, path);
    br_add_line(br, "");

    if (!vfs_exists(path)) {
        br_add_line(br, "Le fichier demandé est introuvable.");
        br_status(br, "Fichier introuvable");
        return;
    }
    if (vfs_is_dir(path)) {
        br_add_line(br, "Ce chemin pointe vers un dossier. Ouvrez plutôt nova://files.");
        br_status(br, "Dossier détecté");
        return;
    }

    k_memset(preview, 0, sizeof(preview));
    vfs_get_contents(path, preview, sizeof(preview) - 1);
    if (!preview[0]) {
        br_add_line(br, "Fichier vide.");
    } else if (br_path_is_html(path) || br_is_html_like(preview)) {
        if (!br_build_html_page(br, path, preview, "Moteur HTML natif simplifie (fichier local)")) br_wrap_line(br, preview);
    } else {
        br_wrap_line(br, preview);
    }
    if (!br->status[0]) br_status(br, "Fichier chargé");
}

static void br_build_release_notes(browser_t *br) {
    br_add_line(br, "# Informations");
    br_add_line(br, "");
    br_status(br, "Informations");
}

static int br_build_rendered_page(browser_t *br, const char *url) {
    char meta[128];
    char host[128];
    char line[640];

    br_extract_host(url, host, sizeof(host));
    br_strcpy(br->host, host, sizeof(br->host));
    if (!br_render_fetch(url, br, meta, sizeof(meta))) return 0;

    br_add_line(br, "# Page web reelle (rendu complet)");
    br_add_line(br, "");
    k_memset(line, 0, sizeof(line));
    br_strcpy(line, "Adresse : ", sizeof(line)); br_strcat(line, url, sizeof(line)); br_add_line(br, line);
    k_memset(line, 0, sizeof(line));
    br_strcpy(line, "Titre : ", sizeof(line)); br_strcat(line, br->render_title[0] ? br->render_title : "(sans titre)", sizeof(line)); br_add_line(br, line);
    k_memset(line, 0, sizeof(line));
    br_strcpy(line, "Transport : ", sizeof(line)); br_strcat(line, meta, sizeof(line)); br_add_line(br, line);
    br_add_line(br, "Apercu capture par un moteur navigateur cote hote, puis affiche dans l'interface locale.");
    br_status(br, "Page rendue chargee");
    return 1;
}

static int br_build_live_page(browser_t *br, const char *url) {
    static char html[BROWSER_LIVE_FETCH_MAX];
    static char live[BROWSER_LIVE_FETCH_MAX];
    char meta[128];
    char host[128];
    char line[640];
    int live_len;

    br_extract_host(url, host, sizeof(host));
    br_strcpy(br->host, host, sizeof(br->host));

    live_len = br_fetch_html(url, html, sizeof(html), meta, sizeof(meta));
    if (live_len > 0 && br_is_html_like(html) && br_build_html_page(br, url, html, meta)) return 1;

    live_len = br_live_fetch(url, live, sizeof(live), meta, sizeof(meta));
    if (live_len <= 0) return 0;

    br_add_line(br, "# Page web reelle (live)");
    br_add_line(br, "");
    k_memset(line, 0, sizeof(line));
    br_strcpy(line, "Adresse : ", sizeof(line)); br_strcat(line, url, sizeof(line)); br_add_line(br, line);
    k_memset(line, 0, sizeof(line));
    br_strcpy(line, "Hote : ", sizeof(line)); br_strcat(line, host, sizeof(line)); br_add_line(br, line);
    k_memset(line, 0, sizeof(line));
    br_strcpy(line, "Transport : ", sizeof(line)); br_strcat(line, meta, sizeof(line)); br_add_line(br, line);
    br_add_line(br, "Rendu : extraction texte live d'une vraie page web au moment de la navigation.");
    br_add_line(br, "");
    br_wrap_line(br, live);
    br_status(br, "Page live chargee");
    return 1;
}

static void br_build_site(browser_t *br, const char *url) {
    char host[128];
    char line[640];
    int lat;

    br_extract_host(url, host, sizeof(host));
    br_strcpy(br->host, host, sizeof(br->host));
    lat = net_ping(host);
    br->last_latency = lat;

    br_add_line(br, "# Fiche de site");
    br_add_line(br, "");
    k_memset(line, 0, sizeof(line));
    br_strcpy(line, "Adresse : ", sizeof(line)); br_strcat(line, url, sizeof(line)); br_add_line(br, line);
    k_memset(line, 0, sizeof(line));
    br_strcpy(line, "Hôte : ", sizeof(line)); br_strcat(line, host, sizeof(line)); br_add_line(br, line);
    k_memset(line, 0, sizeof(line));
    br_strcpy(line, "Connexion : ", sizeof(line)); br_strcat(line, net_eth0.connected ? "active" : "hors ligne", sizeof(line)); br_add_line(br, line);
    k_memset(line, 0, sizeof(line));
    br_strcpy(line, "Latence estimée : ", sizeof(line));
    if (lat >= 0) {
        char n[16]; gui_int_to_str(lat, n); br_strcat(line, n, sizeof(line)); br_strcat(line, " ms", sizeof(line));
    } else {
        br_strcat(line, "échec", sizeof(line));
    }
    br_add_line(br, line);
    br_add_line(br, "");

    if (br_contains(host, "genspark.ai")) {
        br_add_line(br, "Service web distant detecte.");
        br_add_line(br, "Ajoutez cette page aux favoris si besoin.");
    } else if (br_contains(host, "github.com")) {
        br_add_line(br, "Plateforme de code source et collaboration logicielle.");
        br_add_line(br, "Usage recommande : consulter un depot, des versions ou une documentation exportee localement.");
    } else if (br_contains(host, "wikipedia.org")) {
        br_add_line(br, "Encyclopédie collaborative riche en articles et références.");
        br_add_line(br, "Ce mode simplifie fournit un diagnostic reseau et une fiche de destination rapide.");
    } else if (br_contains(host, "news.ycombinator.com")) {
        br_add_line(br, "Flux de veille technique souvent utile pour suivre les nouveautés développeur.");
    } else if (br_contains(host, "localhost") || br_contains(host, "127.0.0.1")) {
        br_add_line(br, "Adresse locale detectee. Utile pour les services de developpement integres.");
    } else {
        br_add_line(br, "Le navigateur a reconnu l'hôte et produit une fiche lisible si aucun rendu HTML n'est disponible.");
        br_add_line(br, "Cette vue reste pratique pour vérifier une adresse, conserver un favori et mesurer la réactivité réseau.");
    }

    br_add_line(br, "");
    br_add_line(br, "Actions utiles :");
    br_add_line(br, "- Ctrl+D pour ajouter ce site aux favoris");
    br_add_line(br, "- Ctrl+L pour saisir une nouvelle adresse");
    br_add_line(br, "- about:net pour revoir la configuration réseau");
    br_add_line(br, "- Pour afficher une vraie page web dans QEMU, demarrez le systeme via tools/run_live_web_qemu.sh ou tools/run_persistent_qemu.sh.");
    br_add_line(br, "- Le navigateur utilisera alors le pont COM2 et un moteur de rendu hote pour afficher HTML/CSS/JS.");

    if (lat >= 0) {
        char msg[64];
        k_memset(msg, 0, sizeof(msg));
        br_strcpy(msg, "Diagnostic réseau OK · ", sizeof(msg));
        {
            char n[16]; gui_int_to_str(lat, n); br_strcat(msg, n, sizeof(msg)); br_strcat(msg, " ms", sizeof(msg));
        }
        br_status(br, msg);
    } else {
        br_status(br, "Aucun accès réseau détecté");
    }
}

static void br_build_query(browser_t *br, const char *query) {
    br_add_line(br, "# Recherche locale");
    br_add_line(br, "");
    br_add_line(br, "Requête :");
    br_add_line(br, query);
    br_add_line(br, "");
    br_add_line(br, "Suggestions :");
    br_add_line(br, "- about:help");
    br_add_line(br, "- nova://tutorial");
    br_add_line(br, "- file:///home/user/readme.txt");
    br_add_line(br, "- https://github.com");
    br_status(br, "Suggestions disponibles");
}

static void br_build_cached_page(browser_t *br, const char *url) {
    char preview[8192];
    char line[640];
    const nova_webcache_entry_t *entry = br_find_cache_entry(url);

    br_add_line(br, "# Page web reelle (cache local)");
    br_add_line(br, "");
    if (!entry) {
        br_add_line(br, "Aucun miroir local disponible pour cette adresse.");
        br_add_line(br, "Utilisez about:help pour voir les sites disponibles.");
        br_status(br, "Aucun miroir disponible");
        return;
    }

    k_memset(line, 0, sizeof(line));
    br_strcpy(line, "Site : ", sizeof(line)); br_strcat(line, entry->label, sizeof(line)); br_add_line(br, line);
    k_memset(line, 0, sizeof(line));
    br_strcpy(line, "URL source : ", sizeof(line)); br_strcat(line, entry->url, sizeof(line)); br_add_line(br, line);
    k_memset(line, 0, sizeof(line));
    br_strcpy(line, "Capture : ", sizeof(line)); br_strcat(line, entry->captured_at, sizeof(line)); br_add_line(br, line);
    br_add_line(br, "Rendu : miroir texte stocke dans le VFS pour consultation dans QEMU.");
    br_add_line(br, "");

    if (!vfs_exists(entry->path)) {
        br_add_line(br, "Le fichier miroir est introuvable dans /webcache.");
        br_status(br, "Cache VFS absent");
        return;
    }

    k_memset(preview, 0, sizeof(preview));
    vfs_get_contents(entry->path, preview, sizeof(preview) - 1);
    br_wrap_line(br, preview);
    br_status(br, "Page chargee depuis le cache local");
}

static void br_open(browser_t *br, const char *url, int push_history) {
    br_reset_page(br);
    br_prepare_navigation(br, url && url[0] ? url : "about:home");
    br_strcpy(br->host, "nova.local", sizeof(br->host));
    br->last_latency = 0;

    if (push_history) {
        if (br->hist_pos < br->hist_count - 1) br->hist_count = br->hist_pos + 1;
        if (br->hist_count >= BROWSER_HIST_MAX) {
            for (int i = 1; i < BROWSER_HIST_MAX; i++) br_strcpy(br->history[i - 1], br->history[i], BROWSER_URL_LEN);
            br->hist_count = BROWSER_HIST_MAX - 1;
            if (br->hist_pos > 0) br->hist_pos--;
        }
        br_strcpy(br->history[br->hist_count], br->url, BROWSER_URL_LEN);
        br->hist_count++;
        br->hist_pos = br->hist_count - 1;
    }

    if (k_strcmp(br->url, "") == 0 || k_strcmp(br->url, "about:home") == 0 || k_strcmp(br->url, "about:new") == 0) {
        br_build_home(br);
    } else if (k_strcmp(br->url, "about:help") == 0) {
        br_build_help(br);
    } else if (k_strcmp(br->url, "about:about") == 0) {
        br_build_about(br);
    } else if (k_strcmp(br->url, "about:net") == 0) {
        br_build_net(br);
    } else if (k_strcmp(br->url, "about:bookmarks") == 0) {
        br_build_bookmarks(br);
    } else if (k_strcmp(br->url, "nova://tutorial") == 0) {
        br_add_line(br, "# Guide");
        br_add_line(br, "");
        br_add_line(br, "- F1 : Terminal");
        br_add_line(br, "- F3 : Web");
        br_add_line(br, "- F5 : Fichiers");
        br_add_line(br, "- F6 : Paramètres");
        br_status(br, "Guide");
    } else if (k_strcmp(br->url, "nova://files") == 0) {
        br_add_line(br, "# Gestionnaire de fichiers");
        br_add_line(br, "");
        br_add_line(br, "Utilisez F5 ou l'icône Fichiers pour profiter de l'aperçu intégré et de la navigation historique.");
        br_add_line(br, "Les chemins texte peuvent être lus directement avec file:///.");
        br_status(br, "Passerelle vers Fichiers");
    } else if (k_strcmp(br->url, "nova://release-notes") == 0) {
        br_build_release_notes(br);
    } else if (k_strcmp(br->url, "nova://store") == 0 || k_strcmp(br->url, "nova://store/") == 0 || k_strcmp(br->url, "nova://store/home") == 0) {
        br_store_home(br);
    } else if (br_starts_with(br->url, "nova://store/search/")) {
        char query[256];
        br_url_decode_path(query, br->url + 20, sizeof(query));
        br_store_search(br, query);
    } else if (br_starts_with(br->url, "nova://store/app/")) {
        char appid[256];
        br_url_decode_path(appid, br->url + 17, sizeof(appid));
        br_store_details(br, appid);
    } else if (br_starts_with(br->url, "nova://store/install/")) {
        char appid[256];
        br_url_decode_path(appid, br->url + 21, sizeof(appid));
        br_store_install(br, appid);
    } else if (br_starts_with(br->url, "file://")) {
        br_build_file(br, br->url);
    } else if (br_starts_with(br->url, "http://") || br_starts_with(br->url, "https://")) {
        if (!br_build_rendered_page(br, br->url) && !br_build_live_page(br, br->url)) {
            if (br_find_cache_entry(br->url)) br_build_cached_page(br, br->url);
            else br_build_site(br, br->url);
        }
    } else {
        br_build_query(br, br->url);
    }

    br->loading = 0;
    br->need_redraw = 1;
}

static void br_navigate(browser_t *br, const char *url) {
    br->loading = 1;
    br_status(br, "Chargement...");
    br_open(br, url, 1);
}

static void br_draw(browser_t *br) {
    if (!br->win || !br->win->visible) return;

    int wx = br->win->x + 2;
    int wy = br->win->y + TITLE_BAR_H;
    int ww = br->win->w - 4;
    int wh = br->win->h - TITLE_BAR_H - 4;
    int tb_h = 48;

    vbe_blend_rect(wx, wy, ww, wh, RGB(250,252,255), 255);
    vbe_gradient_v(wx, wy, ww, tb_h, RGB(250,252,255), RGB(238,244,255));
    vbe_blend_rect(wx, wy + tb_h, ww, 1, RGB(210,220,240), 200);

    int bx = wx + 6;
    int by = wy + 10;
    int can_back = br->hist_pos > 0;
    int can_forward = br->hist_pos >= 0 && br->hist_pos < br->hist_count - 1;

    vbe_blend_rounded_rect(bx, by, 30, 28, 8, can_back ? RGB(80,140,255) : RGB(200,210,230), 210);
    font_draw_string(bx + 9, by + 7, "←", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
    bx += 36;
    vbe_blend_rounded_rect(bx, by, 30, 28, 8, can_forward ? RGB(80,140,255) : RGB(200,210,230), 210);
    font_draw_string(bx + 9, by + 7, "→", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
    bx += 36;
    vbe_blend_rounded_rect(bx, by, 30, 28, 8, RGB(200,210,230), 210);
    font_draw_string(bx + 8, by + 7, "↻", RGB(80,90,120), COLOR_TRANS, FONT_NORMAL);
    bx += 36;
    vbe_blend_rounded_rect(bx, by, 30, 28, 8, RGB(200,210,230), 210);
    font_draw_string(bx + 8, by + 7, "⌂", RGB(80,90,120), COLOR_TRANS, FONT_NORMAL);
    bx += 40;

    int addr_w = ww - bx + wx - 80;
    vbe_blend_rounded_rect(bx, by, addr_w, 28, 8, RGB(255,255,255), 255);
    vbe_rounded_rect_outline(bx, by, addr_w, 28, 8, 2, br->url_focused ? RGB(80,140,255) : RGB(200,210,235));
    if (br->url_focused) {
        char disp[BROWSER_URL_LEN + 2];
        k_memset(disp, 0, sizeof(disp));
        k_memcpy(disp, br->url_input, br->url_input_len);
        disp[br->url_input_len] = '|';
        disp[br->url_input_len + 1] = 0;
        if (br->url_input_len == 0) {
            font_draw_string(bx + 10, by + 7, "Tapez un site, un domaine ou une recherche puis Entree", RGB(132,144,170), COLOR_TRANS, FONT_NORMAL);
        } else {
            font_draw_string(bx + 10, by + 7, disp, RGB(30,40,70), COLOR_TRANS, FONT_NORMAL);
        }
    } else {
        font_draw_string(bx + 10, by + 7, br->url[0] ? br->url : "Cliquez ici pour taper une URL", RGB(50,60,90), COLOR_TRANS, FONT_NORMAL);
    }

    int bbx = bx + addr_w + 8;
    vbe_blend_rounded_rect(bbx, by, 60, 28, 8, RGB(80,140,255), 220);
    font_draw_string(bbx + 6, by + 7, "Favoris", COLOR_WHITE, COLOR_TRANS, FONT_SMALL);

    int ca_y = wy + tb_h + 4;
    int ca_h = wh - tb_h - 34;
    int char_h = 18;
    int rows_vis = ca_h / char_h;

    if (br->render_active) {
        int pill_w = ww > 760 ? 760 : ww - 44;
        vbe_blend_rounded_rect(wx + 18, ca_y + 10, pill_w, 24, 12, RGB(232,240,255), 232);
        font_draw_string(wx + 30, ca_y + 16, br->url, RGB(72,96,148), COLOR_TRANS, FONT_SMALL);
        br_draw_render_preview(br, wx + 12, ca_y + 40, ww - 24, ca_h - 46);
    } else {
        for (int r = 0; r < rows_vis; r++) {
            int src = br->scroll + r;
            int py = ca_y + r * char_h;
            if (src >= br->line_count) break;
            const char *line = br->lines[src];
            color_t lc = RGB(40,50,80);
            font_size_t fs = FONT_NORMAL;
            int px = wx + 20;
            if (line[0] == '#') { lc = RGB(34,54,102); fs = FONT_LARGE; px = wx + 16; }
            else if (line[0] == '-') { lc = RGB(74,112,198); }
            else if (!line[0]) { continue; }
            font_draw_string(px, py, line, lc, COLOR_TRANS, fs);
        }
    }

    int st_y = br->win->y + br->win->h - 28;
    vbe_blend_rect(wx, st_y, ww, 26, RGB(240,244,252), 252);
    vbe_blend_rect(wx, st_y, ww, 1, RGB(210,220,240), 200);
    {
        char full[512];
        k_memset(full, 0, sizeof(full));
        br_strcpy(full, br->status, sizeof(full));
        br_strcat(full, net_eth0.connected ? "   | Réseau actif" : "   | Hors ligne", sizeof(full));
        if (br->bm_count > 0) {
            char n[16];
            br_strcat(full, "   | Favoris: ", sizeof(full));
            gui_int_to_str(br->bm_count, n);
            br_strcat(full, n, sizeof(full));
        }
        font_draw_string(wx + 8, st_y + 7, full, RGB(80,90,120), COLOR_TRANS, FONT_SMALL);
    }

    br->need_redraw = 0;
}

static void br_on_event(widget_t *w, gui_event_t *evt) {
    (void)w;
    browser_t *br = &g_browser;

    if (evt->type == EVT_SCROLL && br->win) {
        int max_scroll = br->line_count > 0 ? br->line_count - 1 : 0;
        int step = evt->scroll > 0 ? -3 : 3;
        br->scroll += step;
        if (br->scroll < 0) br->scroll = 0;
        if (br->scroll > max_scroll) br->scroll = max_scroll;
        br->need_redraw = 1;
        return;
    }

    if ((evt->type == EVT_KEYDOWN || evt->type == EVT_CHAR) && br->win) {
        key_event_t *k = &evt->key;
        if (k->released) return;

        if (k->alt && k->scancode == KEY_LEFT && br->hist_pos > 0) {
            br->hist_pos--;
            br_open(br, br->history[br->hist_pos], 0);
            return;
        }
        if (k->alt && k->scancode == KEY_RIGHT && br->hist_pos < br->hist_count - 1) {
            br->hist_pos++;
            br_open(br, br->history[br->hist_pos], 0);
            return;
        }
        if (k->alt && (k->ascii == 'g' || k->ascii == 'G')) { br_navigate(br, "https://github.com"); return; }
        if (k->alt && (k->ascii == 'w' || k->ascii == 'W')) { br_navigate(br, "https://www.wikipedia.org"); return; }
        if (k->alt && (k->ascii == 'h' || k->ascii == 'H')) { br_navigate(br, "https://news.ycombinator.com"); return; }
        if (k->alt && (k->ascii == 'e' || k->ascii == 'E')) { br_navigate(br, "https://example.com"); return; }
        if (k->ctrl && (k->ascii == 'l' || k->ascii == 'L')) {
            br_focus_url(br, 0);
            return;
        }
        if (k->ctrl && (k->ascii == 'r' || k->ascii == 'R')) {
            br_open(br, br->url, 0);
            return;
        }
        if (k->ctrl && (k->ascii == 'd' || k->ascii == 'D')) {
            if (br->bm_count < BROWSER_BOOKMARKS && br->url[0]) {
                br_strcpy(br->bookmarks[br->bm_count], br->url, BROWSER_URL_LEN);
                br->bm_count++;
                gui_notify("Favori ajouté");
            }
            return;
        }
        if (k->ctrl && (k->ascii == 'b' || k->ascii == 'B')) {
            br_navigate(br, "about:bookmarks");
            return;
        }

        if (br->url_focused) {
            if (k->scancode == KEY_ENTER) {
                br->url_input[br->url_input_len] = 0;
                br->url_focused = 0;
                br->url_input_seeded = 0;
                if (br->url_input_len > 0) br_navigate(br, br->url_input);
                else br->need_redraw = 1;
            } else if (k->scancode == KEY_BACKSPACE) {
                if (br->url_input_seeded) {
                    br->url_input[0] = 0;
                    br->url_input_len = 0;
                    br->url_input_seeded = 0;
                    br->need_redraw = 1;
                } else if (br->url_input_len > 0) {
                    br->url_input[--br->url_input_len] = 0;
                    br->need_redraw = 1;
                }
            } else if (k->scancode == KEY_ESC) {
                br->url_focused = 0;
                br->url_input_seeded = 0;
                br->need_redraw = 1;
            } else if (k->ascii >= 32 && k->ascii < 127 && br->url_input_len < BROWSER_URL_LEN - 2) {
                if (br->url_input_seeded) {
                    br->url_input[0] = 0;
                    br->url_input_len = 0;
                    br->url_input_seeded = 0;
                }
                br->url_input[br->url_input_len++] = k->ascii;
                br->url_input[br->url_input_len] = 0;
                br->need_redraw = 1;
            }
            return;
        }

        if (k->scancode == KEY_UP && br->scroll > 0) { br->scroll--; br->need_redraw = 1; }
        else if (k->scancode == KEY_DOWN && br->scroll < br->line_count - 1) { br->scroll++; br->need_redraw = 1; }
        else if (k->scancode == KEY_PGUP) { br->scroll -= 10; if (br->scroll < 0) br->scroll = 0; br->need_redraw = 1; }
        else if (k->scancode == KEY_PGDN) { br->scroll += 10; if (br->scroll > br->line_count - 1) br->scroll = br->line_count - 1; br->need_redraw = 1; }
        return;
    }

    if (evt->type == EVT_CLICK && br->win) {
        int mx = evt->x - br->win->x - 2;
        int my = evt->y - br->win->y - TITLE_BAR_H;
        int by = 10;
        int bx = 6;
        int addr_x = 6 + 36 + 36 + 36 + 40;
        int addr_w = (br->win->w - 4) - addr_x - 80;
        int fav_x = addr_x + addr_w + 8;

        if (my >= by && my < by + 28) {
            if (mx >= bx && mx < bx + 30 && br->hist_pos > 0) {
                br->hist_pos--;
                br_open(br, br->history[br->hist_pos], 0);
                return;
            }
            bx += 36;
            if (mx >= bx && mx < bx + 30 && br->hist_pos < br->hist_count - 1) {
                br->hist_pos++;
                br_open(br, br->history[br->hist_pos], 0);
                return;
            }
            bx += 36;
            if (mx >= bx && mx < bx + 30) { br_open(br, br->url, 0); return; }
            bx += 36;
            if (mx >= bx && mx < bx + 30) { br_navigate(br, "about:home"); return; }
            if (mx >= addr_x && mx < addr_x + addr_w) {
                br_focus_url(br, 0);
                return;
            }
            if (mx >= fav_x && mx < fav_x + 60) {
                br_navigate(br, "about:bookmarks");
                return;
            }
        }
        br->url_focused = 0;
        br->need_redraw = 1;
    }
}

static void br_paint(window_t *win) {
    (void)win;
    br_draw(&g_browser);
}

void app_browser_open(void) {
    browser_t *br = &g_browser;
    if (br_window_alive(br)) { gui_focus_window(br->win); return; }

    browser_open = 0;
    k_memset(br, 0, sizeof(browser_t));
    br->win_id = -1;

    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    int ww = sw * 4 / 5;
    int wh = sh * 4 / 5;
    int wx = (sw - ww) / 2;
    int wy = (sh - wh) / 2;

    window_t *win = gui_create_window(wx, wy, ww, wh, "Navigateur", WIN_DEFAULT);
    if (!win) return;
    win->bg_color = RGB(16,22,34);
    win->on_paint = br_paint;
    win->on_event = br_on_event;
    br->win = win;
    br->win_id = win->id;

    widget_t *mw = gui_add_label(win, 0, 0, ww, wh, "");
    if (mw) { mw->on_click = br_on_event; mw->on_keydown = br_on_event; mw->focused = 1; }

    gui_show_window(win);
    gui_focus_window(win);
    browser_open = 1;
    br_navigate(br, "about:home");
    br->url_focused = 1;
    br->url_input[0] = 0;
    br->url_input_len = 0;
    br->url_input_seeded = 0;
    br_status(br, "Pret : tapez un site (google, github, wiki...) ou une recherche puis Entree");
    br->need_redraw = 1;
}

void app_browser_open_url(const char *url) {
    app_browser_open();
    if (br_window_alive(&g_browser)) {
        br_navigate(&g_browser, (url && url[0]) ? url : "about:home");
        gui_focus_window(g_browser.win);
    }
}
