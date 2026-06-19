#include "symera.h"
#include "../gui/gui.h"
#include "../gui/font.h"
#include "../drivers/vbe.h"
#include "../drivers/keyboard.h"
#include "../drivers/sound.h"
#include "../kernel/timer.h"
#include "../kernel/users.h"
#include "../kernel/userspace.h"
#include "../fs/vfs.h"
#include "../net/net.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

extern uint64_t heap_used(void);
extern uint64_t heap_total(void);

static window_t *g_symera_win = NULL;
static int g_symera_win_id = -1;
static char g_symera_input[256];
static int g_symera_input_len = 0;
static int g_symera_cursor = 0;
static char g_symera_output[1536];
static char g_symera_status[160];
static char g_symera_last_voice[256];
static uint32_t g_symera_last_tick = 0;
static int g_symera_prompt_focused = 1;

static int sy_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static void sy_copy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    while (src && src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void sy_cat(char *dst, const char *src, int max) {
    int dl = sy_len(dst), i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) { dst[dl + i] = src[i]; i++; }
    dst[dl + i] = 0;
}

static char sy_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

static int sy_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int sy_contains_ci(const char *haystack, const char *needle) {
    if (!needle || !needle[0]) return 1;
    if (!haystack) return 0;
    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (needle[j] && haystack[i + j] && sy_lower(haystack[i + j]) == sy_lower(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static void sy_trim_copy(const char *src, char *dst, int max) {
    int start = 0, end;
    if (!dst || max <= 0) return;
    dst[0] = 0;
    if (!src) return;
    end = sy_len(src);
    while (src[start] && sy_is_space(src[start])) start++;
    while (end > start && sy_is_space(src[end - 1])) end--;
    int di = 0;
    for (int i = start; i < end && di < max - 1; i++) dst[di++] = src[i];
    dst[di] = 0;
}

static void sy_append_log(const char *prompt, const char *reply) {
    static char buf[VFS_MAX_DATA];
    int cur = 0;
    k_memset(buf, 0, sizeof(buf));
    if (vfs_exists("/var/log/symera.log") && !vfs_is_dir("/var/log/symera.log")) {
        vfs_get_contents("/var/log/symera.log", buf, sizeof(buf) - 1);
        cur = sy_len(buf);
    }
    if (cur >= VFS_MAX_DATA - 256) return;
    sy_cat(buf, "[", sizeof(buf));
    {
        char nb[16];
        uint32_t sec = timer_ms() / 1000;
        int p = 0;
        if (sec == 0) nb[p++] = '0';
        else {
            char tmp[16]; int tl = 0;
            while (sec && tl < 15) { tmp[tl++] = (char)('0' + (sec % 10)); sec /= 10; }
            while (tl > 0) nb[p++] = tmp[--tl];
        }
        nb[p] = 0;
        sy_cat(buf, nb, sizeof(buf));
    }
    sy_cat(buf, "s] user: ", sizeof(buf));
    sy_cat(buf, prompt ? prompt : "", sizeof(buf));
    sy_cat(buf, "\n[", sizeof(buf));
    {
        char nb[16];
        uint32_t sec = timer_ms() / 1000;
        int p = 0;
        if (sec == 0) nb[p++] = '0';
        else {
            char tmp[16]; int tl = 0;
            while (sec && tl < 15) { tmp[tl++] = (char)('0' + (sec % 10)); sec /= 10; }
            while (tl > 0) nb[p++] = tmp[--tl];
        }
        nb[p] = 0;
        sy_cat(buf, nb, sizeof(buf));
    }
    sy_cat(buf, "s] symera: ", sizeof(buf));
    sy_cat(buf, reply ? reply : "", sizeof(buf));
    sy_cat(buf, "\n\n", sizeof(buf));
    vfs_write_file("/var/log/symera.log", buf, (uint32_t)sy_len(buf));
}

static void sy_append_note(const char *note) {
    static char buf[VFS_MAX_DATA];
    int cur = 0;
    if (!note || !note[0]) return;
    k_memset(buf, 0, sizeof(buf));
    if (vfs_exists("/home/user/notes.txt") && !vfs_is_dir("/home/user/notes.txt")) {
        vfs_get_contents("/home/user/notes.txt", buf, sizeof(buf) - 1);
        cur = sy_len(buf);
    }
    if (cur > 0 && buf[cur - 1] != '\n') sy_cat(buf, "\n", sizeof(buf));
    sy_cat(buf, "- ", sizeof(buf));
    sy_cat(buf, note, sizeof(buf));
    sy_cat(buf, "\n", sizeof(buf));
    vfs_write_file("/home/user/notes.txt", buf, (uint32_t)sy_len(buf));
}

static int sy_window_alive(void) {
    if (!g_symera_win || g_symera_win_id < 0) return 0;
    for (int i = 0; i < gui.window_count; i++) {
        if (gui.windows[i].id == g_symera_win_id) {
            g_symera_win = &gui.windows[i];
            return 1;
        }
    }
    g_symera_win = NULL;
    g_symera_win_id = -1;
    return 0;
}

static int sy_extract_url(const char *prompt, char *out, int max) {
    int i = 0;
    if (!prompt || !out || max <= 0) return 0;
    out[0] = 0;
    while (prompt[i]) {
        if ((prompt[i] == 'h' || prompt[i] == 'H') &&
            sy_lower(prompt[i + 1]) == 't' && sy_lower(prompt[i + 2]) == 't' && sy_lower(prompt[i + 3]) == 'p') {
            int j = 0;
            while (prompt[i] && !sy_is_space(prompt[i]) && j < max - 1) out[j++] = prompt[i++];
            out[j] = 0;
            return j > 0;
        }
        if ((prompt[i] == 'w' || prompt[i] == 'W') && (prompt[i + 1] == 'w' || prompt[i + 1] == 'W') && (prompt[i + 2] == 'w' || prompt[i + 2] == 'W') && prompt[i + 3] == '.') {
            sy_copy(out, "https://", max);
            int j = sy_len(out);
            while (prompt[i] && !sy_is_space(prompt[i]) && j < max - 1) out[j++] = prompt[i++];
            out[j] = 0;
            return j > 8;
        }
        i++;
    }
    return 0;
}

static void sy_extract_note_text(const char *prompt, char *out, int max) {
    const char *keys[] = {"ajoute une note", "note ", "rappelle", "memorise", "mémorise", NULL};
    out[0] = 0;
    for (int k = 0; keys[k]; k++) {
        const char *hit = k_strstr(prompt, keys[k]);
        if (hit) {
            sy_trim_copy(hit + sy_len(keys[k]), out, max);
            if (out[0] == ':' || out[0] == '-') sy_trim_copy(out + 1, out, max);
            if (out[0]) return;
        }
    }
    sy_trim_copy(prompt, out, max);
}

static void sy_reply_default(char *response, int max) {
    sy_copy(response,
        "Interface de commandes locale. Je peux ouvrir des applications, lancer Firefox, naviguer vers une URL, ouvrir les notes de version, les applications, la checklist, le plan, le centre userspace, ajouter une note, verrouiller la session, donner un resume systeme et relire la reponse avec la synthese vocale locale.",
        max);
}

int symera_execute_prompt(const char *prompt, char *response, int max) {
    char clean[256];
    char url[256];
    char note[256];
    char ip_str[20];
    user_t *cur;
    if (!response || max <= 0) return 0;
    response[0] = 0;
    sy_trim_copy(prompt, clean, sizeof(clean));
    cur = users_get_current();

    if (!clean[0]) {
        sy_reply_default(response, max);
        sy_append_log(prompt, response);
        return 1;
    }

    if (sy_contains_ci(clean, "help") || sy_contains_ci(clean, "aide") || sy_contains_ci(clean, "que peux-tu") || sy_contains_ci(clean, "capabilit")) {
        sy_copy(response,
            "Commandes disponibles : ouvrir Terminal, Fichiers, Parametres, Navigateur, Firefox, Tableau de bord, Notes, Centre Systeme, Userspace, Calculatrice, Horloge, Guide et Editeur ; ouvrir une URL ; ouvrir les notes de version, les applications, le plan, la checklist, le livrable et le rapport userspace ; ajouter une note ; verrouiller la session ; donner l'etat memoire/reseau ; lecture vocale locale.",
            max);
    } else if (sy_extract_url(clean, url, sizeof(url)) || sy_contains_ci(clean, "ouvre firefox") || sy_contains_ci(clean, "lance firefox")) {
        if (!url[0]) sy_copy(url, "https://www.mozilla.org/firefox/new/", sizeof(url));
        app_browser_open_url(url);
        sy_copy(response, "Navigation envoyee au navigateur.", max);
    } else if (sy_contains_ci(clean, "release") || sy_contains_ci(clean, "changelog") || sy_contains_ci(clean, "nouveaut")) {
        app_browser_open_url("nova://release-notes");
        sy_copy(response, "Notes de version ouvertes dans le navigateur.", max);
    } else if (sy_contains_ci(clean, "store") || sy_contains_ci(clean, "boutique")) {
        app_browser_open_url("nova://store");
        sy_copy(response, "Applications ouvertes dans le navigateur.", max);
    } else if (sy_contains_ci(clean, "roadmap") || sy_contains_ci(clean, "feuille de route")) {
        app_browser_open_url("file:///home/user/Documents/Plan.txt");
        sy_copy(response, "Plan ouvert dans le navigateur.", max);
    } else if (sy_contains_ci(clean, "qa") || sy_contains_ci(clean, "checklist") || sy_contains_ci(clean, "validation")) {
        app_browser_open_url("file:///home/user/Documents/QA-Checklist.txt");
        sy_copy(response, "Checklist QA ouverte dans le navigateur.", max);
    } else if (sy_contains_ci(clean, "bundle") || sy_contains_ci(clean, "livraison")) {
        app_browser_open_url("file:///home/user/LIVRABLE.txt");
        sy_copy(response, "Livrable ouvert dans le navigateur.", max);
    } else if (sy_contains_ci(clean, "journal") || sy_contains_ci(clean, "logs") || sy_contains_ci(clean, "log")) {
        app_browser_open_url("file:///var/log/system.log");
        sy_copy(response, "Journal système ouvert dans le navigateur.", max);
    } else if (sy_contains_ci(clean, "userspace") && (sy_contains_ci(clean, "rapport") || sy_contains_ci(clean, "status") || sy_contains_ci(clean, "état") || sy_contains_ci(clean, "etat"))) {
        app_browser_open_url("file:///home/user/Documents/Userspace.txt");
        sy_copy(response, "Rapport userspace ouvert dans le navigateur.", max);
    } else if (sy_contains_ci(clean, "ouvre") || sy_contains_ci(clean, "lance") || sy_contains_ci(clean, "demarre") || sy_contains_ci(clean, "démarre")) {
        if (sy_contains_ci(clean, "terminal")) {
            app_terminal_open();
            sy_copy(response, "Terminal ouvert.", max);
        } else if (sy_contains_ci(clean, "fichier") || sy_contains_ci(clean, "explorateur")) {
            app_filemanager_open();
            sy_copy(response, "Gestionnaire de fichiers ouvert.", max);
        } else if (sy_contains_ci(clean, "param") || sy_contains_ci(clean, "setting")) {
            app_settings_open();
            sy_copy(response, "Paramètres ouverts.", max);
        } else if (sy_contains_ci(clean, "browser") || sy_contains_ci(clean, "navigateur") || sy_contains_ci(clean, "web")) {
            app_browser_open();
            sy_copy(response, "Navigateur ouvert.", max);
        } else if (sy_contains_ci(clean, "hub")) {
            app_nova_hub_open();
            sy_copy(response, "Tableau de bord ouvert.", max);
        } else if (sy_contains_ci(clean, "monitor") || sy_contains_ci(clean, "dashboard") || sy_contains_ci(clean, "centre systeme") || sy_contains_ci(clean, "centre système") || sy_contains_ci(clean, "supervision")) {
            app_system_monitor_open();
            sy_copy(response, "Centre Système ouvert.", max);
        } else if (sy_contains_ci(clean, "userspace") || sy_contains_ci(clean, "processus")) {
            app_userspace_open();
            sy_copy(response, "Centre userspace ouvert.", max);
        } else if (sy_contains_ci(clean, "note")) {
            app_quick_notes_open();
            sy_copy(response, "Notes ouvertes.", max);
        } else if (sy_contains_ci(clean, "calcul")) {
            app_calculator_open();
            sy_copy(response, "Calculatrice ouverte.", max);
        } else if (sy_contains_ci(clean, "horloge") || sy_contains_ci(clean, "clock")) {
            app_clock_open();
            sy_copy(response, "Horloge ouverte.", max);
        } else if (sy_contains_ci(clean, "tutoriel")) {
            app_tutorial_open();
            sy_copy(response, "Guide ouvert.", max);
        } else if (sy_contains_ci(clean, "editeur") || sy_contains_ci(clean, "éditeur") || sy_contains_ci(clean, "editor")) {
            app_editor_open();
            sy_copy(response, "Éditeur ouvert.", max);
        } else if (sy_contains_ci(clean, "a propos") || sy_contains_ci(clean, "about")) {
            app_about_open();
            sy_copy(response, "Fenêtre À propos ouverte.", max);
        } else if (sy_contains_ci(clean, "symera") || sy_contains_ci(clean, "commandes")) {
            sy_copy(response, "Je suis déjà là.", max);
        } else {
            sy_reply_default(response, max);
        }
    } else if (sy_contains_ci(clean, "verrouille") || sy_contains_ci(clean, "lock") || sy_contains_ci(clean, "écran de verrouillage") || sy_contains_ci(clean, "ecran de verrouillage")) {
        gui_activate_lockscreen();
        sy_copy(response, "Session verrouillée. Tape le mot de passe pour revenir.", max);
    } else if (sy_contains_ci(clean, "ajoute une note") || sy_contains_ci(clean, "rappelle") || sy_contains_ci(clean, "memorise") || sy_contains_ci(clean, "mémorise")) {
        sy_extract_note_text(clean, note, sizeof(note));
        sy_append_note(note);
        sy_copy(response, "Note ajoutee aux notes.", max);
    } else if (sy_contains_ci(clean, "userspace") || sy_contains_ci(clean, "processus")) {
        char nb[16];
        int count = userspace_process_count();
        int p = 0;
        if (count == 0) nb[p++] = '0';
        else {
            char tmp[16];
            int tl = 0;
            while (count && tl < 15) { tmp[tl++] = (char)('0' + (count % 10)); count /= 10; }
            while (tl > 0) nb[p++] = tmp[--tl];
        }
        nb[p] = 0;
        sy_copy(response, "Userspace actif avec ", max);
        sy_cat(response, nb, max);
        sy_cat(response, " processus publies. Ouvre Userspace pour le tableau complet.", max);
    } else if (sy_contains_ci(clean, "memoire") || sy_contains_ci(clean, "mémoire") || sy_contains_ci(clean, "ram") || sy_contains_ci(clean, "systeme") || sy_contains_ci(clean, "système")) {
        char a[32], b[32];
        uint64_t used = heap_used() / 1024;
        uint64_t total = heap_total() / 1024;
        int p = 0;
        if (used == 0) a[p++] = '0';
        else {
            char t[32]; int tl = 0; while (used && tl < 31) { t[tl++] = (char)('0' + (used % 10)); used /= 10; } while (tl > 0) a[p++] = t[--tl];
        }
        a[p] = 0; p = 0;
        if (total == 0) b[p++] = '0';
        else {
            char t[32]; int tl = 0; while (total && tl < 31) { t[tl++] = (char)('0' + (total % 10)); total /= 10; } while (tl > 0) b[p++] = t[--tl];
        }
        b[p] = 0;
        sy_copy(response, "État système : mémoire utilisée ", max);
        sy_cat(response, a, max);
        sy_cat(response, " Ko sur ", max);
        sy_cat(response, b, max);
        sy_cat(response, " Ko. GUI, VFS et pile audio sont actifs.", max);
    } else if (sy_contains_ci(clean, "reseau") || sy_contains_ci(clean, "réseau") || sy_contains_ci(clean, "ip")) {
        net_get_ip_str(net_eth0.ip, ip_str);
        sy_copy(response, "Réseau : interface eth0 ", max);
        sy_cat(response, net_eth0.connected ? "connectée, IP " : "hors ligne, dernière IP ", max);
        sy_cat(response, ip_str, max);
        sy_cat(response, ".", max);
    } else if (sy_contains_ci(clean, "qui suis-je") || sy_contains_ci(clean, "whoami") || sy_contains_ci(clean, "session")) {
        sy_copy(response, "Session active : ", max);
        sy_cat(response, (cur && cur->fullname[0]) ? cur->fullname : ((cur && cur->username[0]) ? cur->username : "aucune"), max);
        sy_cat(response, ".", max);
    } else if (sy_contains_ci(clean, "parle") || sy_contains_ci(clean, "voix") || sy_contains_ci(clean, "dis ")) {
        if (sy_contains_ci(clean, "bonjour")) sy_copy(response, "Bonjour. Interface locale active.", max);
        else sy_copy(response, "Synthese vocale locale activee.", max);
    } else {
        sy_reply_default(response, max);
    }

    sy_append_log(clean, response);
    return 1;
}

static void symera_speak(const char *text) {
    if (!text || !text[0]) return;
    sy_copy(g_symera_last_voice, text, sizeof(g_symera_last_voice));
    sound_speak_text(text);
}

static void symera_run_prompt(const char *prompt) {
    if (!prompt) return;
    symera_execute_prompt(prompt, g_symera_output, sizeof(g_symera_output));
    sy_copy(g_symera_status, "Commande locale executee", sizeof(g_symera_status));
    g_symera_last_tick = timer_ms();
    gui_notify("Commande traitee");
    if (g_symera_win) g_symera_win->needs_redraw = 1;
}

static void sy_draw_wrapped_text(int x, int y, int max_w, const char *text, color_t color) {
    char line[120];
    int li = 0;
    int row = 0;
    int limit = max_w / 8;
    if (limit < 16) limit = 16;
    for (int i = 0; text && text[i] && row < 12; i++) {
        if (text[i] == '\r') continue;
        if (text[i] == '\n' || li >= limit) {
            line[li] = 0;
            font_draw_string(x, y + row * 22, line, color, COLOR_TRANS, FONT_SMALL);
            li = 0;
            row++;
            if (text[i] == '\n') continue;
        }
        line[li++] = text[i];
    }
    if (li > 0 && row < 12) {
        line[li] = 0;
        font_draw_string(x, y + row * 22, line, color, COLOR_TRANS, FONT_SMALL);
    }
}

static int sy_hit(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void symera_paint(window_t *win) {
    int wx, wy, ww, wh;
    user_t *cur;
    if (!win || !win->visible) return;
    wx = win->x + 4;
    wy = win->y + TITLE_BAR_H + 4;
    ww = win->w - 8;
    wh = win->h - TITLE_BAR_H - 8;
    cur = users_get_current();

    vbe_gradient_v(wx, wy, ww, wh, RGB(10,16,30), RGB(16,26,48));
    vbe_rounded_rect_outline(wx, wy, ww, wh, 18, 1, RGB(88,132,214));
    vbe_blend_rounded_rect(wx + 18, wy + 18, ww - 36, 96, 20, RGB(18,28,52), 236);
    vbe_gradient_h(wx + 36, wy + 34, 150, 10, RGB(86,132,255), RGB(88,236,228));
    font_draw_string_shadow(wx + 34, wy + 28, "Commandes", COLOR_WHITE, FONT_TITLE);
    font_draw_string(wx + 34, wy + 62, "Interface locale : commandes naturelles, actions systeme et retour vocal.", RGB(184,204,236), COLOR_TRANS, FONT_SMALL);
    font_draw_string(wx + 34, wy + 82, (cur && cur->fullname[0]) ? cur->fullname : "Session locale", RGB(120,210,220), COLOR_TRANS, FONT_SMALL);

    vbe_blend_rounded_rect(wx + 20, wy + 132, ww - 40, 54, 16, RGB(255,255,255), g_symera_prompt_focused ? 250 : 220);
    vbe_rounded_rect_outline(wx + 20, wy + 132, ww - 40, 54, 16, 1, g_symera_prompt_focused ? RGB(86,160,255) : RGB(180,196,226));
    if (g_symera_input_len == 0) {
        font_draw_string(wx + 34, wy + 151, "Exemples : ouvre terminal • notes de version • ouvre QA • ajoute une note", RGB(120,134,160), COLOR_TRANS, FONT_SMALL);
    } else {
        font_draw_string(wx + 34, wy + 151, g_symera_input, RGB(32,44,72), COLOR_TRANS, FONT_NORMAL);
        if ((timer_ms() / 500) % 2) {
            int cx = wx + 34 + g_symera_cursor * 8;
            vbe_blend_rect(cx, wy + 147, 2, 20, RGB(48,92,180), 220);
        }
    }

    vbe_blend_rounded_rect(wx + 20, wy + 202, 140, 40, 14, RGB(86,132,255), 238);
    vbe_rounded_rect_outline(wx + 20, wy + 202, 140, 40, 14, 1, RGB(130,170,255));
    font_draw_string(wx + 54, wy + 215, "Exécuter", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(wx + 174, wy + 202, 140, 40, 14, RGB(70,214,176), 236);
    vbe_rounded_rect_outline(wx + 174, wy + 202, 140, 40, 14, 1, RGB(124,232,198));
    font_draw_string(wx + 208, wy + 215, "Parler", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(wx + 328, wy + 202, 150, 40, 14, RGB(252,186,92), 238);
    vbe_rounded_rect_outline(wx + 328, wy + 202, 150, 40, 14, 1, RGB(255,210,136));
    font_draw_string(wx + 364, wy + 215, "Firefox", RGB(72,52,18), COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(wx + 492, wy + 202, 146, 40, 14, RGB(162,122,255), 236);
    vbe_rounded_rect_outline(wx + 492, wy + 202, 146, 40, 14, 1, RGB(198,170,255));
    font_draw_string(wx + 524, wy + 215, "Terminal", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(wx + 652, wy + 202, 146, 40, 14, RGB(244,96,112), 236);
    vbe_rounded_rect_outline(wx + 652, wy + 202, 146, 40, 14, 1, RGB(255,152,170));
    font_draw_string(wx + 676, wy + 215, "Verrouiller", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(wx + 20, wy + 260, ww - 40, wh - 280, 18, RGB(250,252,255), 238);
    vbe_rounded_rect_outline(wx + 20, wy + 260, ww - 40, wh - 280, 18, 1, RGB(192,208,236));
    font_draw_string(wx + 36, wy + 278, "Réponse locale", RGB(36,52,86), COLOR_TRANS, FONT_NORMAL);
    sy_draw_wrapped_text(wx + 36, wy + 308, ww - 72, g_symera_output[0] ? g_symera_output : "Pret. Appuie sur F12 ou tape une commande.", RGB(78,94,126));

    if (g_symera_status[0]) {
        font_draw_string(wx + 36, wy + wh - 28, g_symera_status, RGB(86,134,214), COLOR_TRANS, FONT_SMALL);
    }
}

static void symera_event(widget_t *w, gui_event_t *evt) {
    int wx, wy;
    (void)w;
    if (!sy_window_alive() || !evt) return;
    wx = g_symera_win->x + 4;
    wy = g_symera_win->y + TITLE_BAR_H + 4;

    if (evt->type == EVT_MOUSEUP) {
        g_symera_prompt_focused = sy_hit(evt->x, evt->y, wx + 20, wy + 132, g_symera_win->w - 48, 54);
        if (sy_hit(evt->x, evt->y, wx + 20, wy + 202, 140, 40)) {
            symera_run_prompt(g_symera_input);
        } else if (sy_hit(evt->x, evt->y, wx + 174, wy + 202, 140, 40)) {
            symera_speak(g_symera_output[0] ? g_symera_output : "Bonjour.");
            sy_copy(g_symera_status, "Lecture vocale lancee", sizeof(g_symera_status));
        } else if (sy_hit(evt->x, evt->y, wx + 328, wy + 202, 150, 40)) {
            sy_copy(g_symera_input, "ouvre firefox", sizeof(g_symera_input));
            g_symera_input_len = sy_len(g_symera_input);
            g_symera_cursor = g_symera_input_len;
            symera_run_prompt(g_symera_input);
        } else if (sy_hit(evt->x, evt->y, wx + 492, wy + 202, 146, 40)) {
            sy_copy(g_symera_input, "ouvre terminal", sizeof(g_symera_input));
            g_symera_input_len = sy_len(g_symera_input);
            g_symera_cursor = g_symera_input_len;
            symera_run_prompt(g_symera_input);
        } else if (sy_hit(evt->x, evt->y, wx + 652, wy + 202, 146, 40)) {
            sy_copy(g_symera_input, "verrouille la session", sizeof(g_symera_input));
            g_symera_input_len = sy_len(g_symera_input);
            g_symera_cursor = g_symera_input_len;
            symera_run_prompt(g_symera_input);
        }
        g_symera_win->needs_redraw = 1;
        return;
    }

    if ((evt->type != EVT_KEYDOWN && evt->type != EVT_CHAR) || !g_symera_prompt_focused) return;
    if (evt->key.released) return;

    if (evt->key.scancode == KEY_ENTER) {
        symera_run_prompt(g_symera_input);
    } else if (evt->key.scancode == KEY_BACKSPACE) {
        if (g_symera_cursor > 0 && g_symera_input_len > 0) {
            g_symera_cursor--;
            for (int i = g_symera_cursor; i < g_symera_input_len - 1; i++) g_symera_input[i] = g_symera_input[i + 1];
            g_symera_input[--g_symera_input_len] = 0;
        }
    } else if (evt->key.scancode == KEY_LEFT) {
        if (g_symera_cursor > 0) g_symera_cursor--;
    } else if (evt->key.scancode == KEY_RIGHT) {
        if (g_symera_cursor < g_symera_input_len) g_symera_cursor++;
    } else if (evt->key.scancode == KEY_ESC) {
        g_symera_input_len = 0;
        g_symera_cursor = 0;
        g_symera_input[0] = 0;
        sy_copy(g_symera_status, "Champ efface", sizeof(g_symera_status));
    } else if (evt->key.ascii >= 32 && evt->key.ascii < 127 && g_symera_input_len < (int)sizeof(g_symera_input) - 2) {
        for (int i = g_symera_input_len; i > g_symera_cursor; i--) g_symera_input[i] = g_symera_input[i - 1];
        g_symera_input[g_symera_cursor++] = evt->key.ascii;
        g_symera_input[++g_symera_input_len - 1] = 0;
    }
    g_symera_win->needs_redraw = 1;
}

void app_symera_open(void) {
    int sw, sh, ww, wh;
    widget_t *focus_w;
    if (sy_window_alive()) { gui_focus_window(g_symera_win); return; }

    sw = vbe.width ? (int)vbe.width : 1920;
    sh = vbe.height ? (int)vbe.height : 1080;
    ww = 860;
    wh = 620;

    k_memset(g_symera_input, 0, sizeof(g_symera_input));
    k_memset(g_symera_output, 0, sizeof(g_symera_output));
    k_memset(g_symera_status, 0, sizeof(g_symera_status));
    g_symera_input_len = 0;
    g_symera_cursor = 0;
    g_symera_prompt_focused = 1;

    g_symera_win = gui_create_window((sw - ww) / 2, (sh - wh) / 2 - 24, ww, wh, "Commandes", WIN_DEFAULT);
    if (!g_symera_win) return;
    g_symera_win_id = g_symera_win->id;
    g_symera_win->bg_color = RGB(10,16,30);
    g_symera_win->on_paint = symera_paint;
    g_symera_win->on_event = symera_event;

    focus_w = gui_add_label(g_symera_win, 0, 0, ww, wh, "");
    if (focus_w) {
        focus_w->on_click = symera_event;
        focus_w->on_keydown = symera_event;
        focus_w->focused = 1;
    }

    sy_copy(g_symera_output, "Bonjour. Interface de commandes locale prete. Essaie : ouvre terminal, notes de version, ouvre QA, livrable, plan ou ajoute une note.", sizeof(g_symera_output));
    sy_copy(g_symera_status, "Pret", sizeof(g_symera_status));
    gui_show_window(g_symera_win);
    gui_focus_window(g_symera_win);
}
