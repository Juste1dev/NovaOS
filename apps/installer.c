

#include "../gui/gui.h"
#include "../gui/font.h"
#include "../drivers/vbe.h"
#include "../drivers/keyboard.h"
#include "../kernel/timer.h"
#include "../kernel/users.h"
#include "../fs/vfs.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

typedef enum {
    INST_WELCOME = 0,
    INST_LICENSE,
    INST_PARTITION,
    INST_USER,
    INST_SETTINGS,
    INST_INSTALL,
    INST_DONE
} inst_step_t;

typedef struct {
    inst_step_t step;
    int license_accepted;
    char username[64];
    char password[64];
    char fullname[128];
    char hostname[64];
    char timezone[64];
    char lang[32];
    int  install_progress;
    int  installing;
    window_t *win;
    int  need_redraw;

    int  focus_field;
    widget_t *username_w;
    widget_t *password_w;
    widget_t *fullname_w;
    widget_t *hostname_w;
    widget_t *progress_w;
} installer_t;

static installer_t g_inst;
static int inst_open = 0;

static int inst_strlen(const char *s) { int n=0; while(s[n]) n++; return n; }

static void inst_draw_step_indicator(installer_t *inst) {
    if (!inst->win) return;
    int wx = inst->win->x + 4;
    int wy = inst->win->y + TITLE_BAR_H + 4;
    int ww = inst->win->w - 8;

    const char *steps[] = {"Début","Accord","Partition","Compte","Options","Installation","Fin",NULL};
    int n = 7;
    int step_w = (ww - 40) / n;

    for (int i = 0; i < n; i++) {
        int sx = wx + 20 + i * step_w;
        int sy = wy + 6;
        color_t bc = (i == (int)inst->step) ? RGB(60,140,255) :
                     (i < (int)inst->step)  ? RGB(60,200,100) : RGB(180,190,210);
        vbe_blend_rounded_rect(sx, sy, step_w - 4, 28, 6, bc, 220);

        char num[4]; num[0]='1'+i; num[1]=0;
        font_draw_string(sx+6, sy+7, num, COLOR_WHITE, COLOR_TRANS, FONT_SMALL);
        font_draw_string(sx+18, sy+7, steps[i], COLOR_WHITE, COLOR_TRANS, FONT_SMALL);

        if (i < n-1) {
            font_draw_string(sx + step_w - 6, sy+7, "›", RGB(120,140,180), COLOR_TRANS, FONT_NORMAL);
        }
    }
}

static void inst_draw_welcome(installer_t *inst) {
    int wx = inst->win->x + 4;
    int wy = inst->win->y + TITLE_BAR_H + 50;
    int ww = inst->win->w - 8;

    int lx = wx + ww/2 - 200;
    vbe_blend_rounded_rect(lx, wy + 10, 400, 160, 20, RGB(240,246,255), 240);
    vbe_rounded_rect_outline(lx, wy + 10, 400, 160, 20, 1, RGB(200,214,240));
    vbe_gradient_h(lx+20, wy+18, 80, 10, RGB(104,132,255), RGB(110,210,255));
    font_draw_string_shadow(lx+20, wy+18, "NovaOS 4.0", RGB(30,44,72), FONT_TITLE);
    font_draw_string(lx+20, wy+60, "L'OS moderne, léger et performant", RGB(80,100,140), COLOR_TRANS, FONT_LARGE);
    font_draw_string(lx+20, wy+88, "Installation guidée • sessions locales • stockage en mémoire", RGB(100,120,160), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(lx+20, wy+108, "Gestionnaire de fichiers  •  Navigateur  •  Terminal", RGB(100,120,160), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(lx+20, wy+128, "Installation rapide  •  Configuration facile", RGB(100,120,160), COLOR_TRANS, FONT_NORMAL);

    font_draw_string(wx+20, wy+190, "Installateur", RGB(40,60,100), COLOR_TRANS, FONT_LARGE);
    font_draw_string(wx+20, wy+218, "L'installation guidée va configurer complètement le système.", RGB(70,85,120), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(wx+20, wy+238, "Vous devrez disposer d'au moins 2 Go d'espace disque.", RGB(70,85,120), COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(wx + ww - 140, wy + 280, 120, 40, 10, RGB(60,140,255), 240);
    vbe_rounded_rect_outline(wx + ww - 140, wy + 280, 120, 40, 10, 1, RGB(80,160,255));
    font_draw_string(wx + ww - 120, wy + 293, "Suivant →", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
}

static void inst_draw_license(installer_t *inst) {
    int wx = inst->win->x + 4;
    int wy = inst->win->y + TITLE_BAR_H + 50;
    int ww = inst->win->w - 8;

    font_draw_string_shadow(wx+20, wy+10, "Accord", RGB(30,44,72), FONT_TITLE);

    vbe_blend_rounded_rect(wx+10, wy+50, ww-20, 220, 8, RGB(250,252,255), 250);
    vbe_rounded_rect_outline(wx+10, wy+50, ww-20, 220, 8, 1, RGB(200,212,238));

    const char *lic_lines[] = {
        "LICENCE DESKTOPS NOVA 4.0 – OPEN SOURCE",
        "",
        "NovaOS est distribué sous la Licence MIT.",
        "",
        "Permission est accordée, à titre gratuit, à toute personne obtenant une copie",
        "de ce logiciel et des fichiers de documentation associés, de traiter le Logiciel",
        "sans restriction, incluant sans limitation les droits d'utiliser, copier, modifier,",
        "fusionner, publier, distribuer, sous-licencier, et/ou vendre des copies du Logiciel.",
        "",
        "LE LOGICIEL EST FOURNI \"EN L'ÉTAT\", SANS GARANTIE D'AUCUNE SORTE.",
        "",
        "Copyright 2026 – Équipe NovaOS",
        NULL
    };
    for (int i = 0; lic_lines[i] && i < 12; i++) {
        font_draw_string(wx+18, wy+58 + i*16, lic_lines[i], RGB(50,60,90), COLOR_TRANS, FONT_SMALL);
    }

    int chx = wx + 20;
    int chy = wy + 288;
    vbe_blend_rounded_rect(chx, chy, 20, 20, 4,
                           inst->license_accepted ? RGB(60,200,100) : RGB(240,244,255), 240);
    vbe_rounded_rect_outline(chx, chy, 20, 20, 4, 1, RGB(150,160,200));
    if (inst->license_accepted) {
        font_draw_string(chx+4, chy+3, "✓", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
    }
    font_draw_string(chx+28, chy+3, "Accord accepté", RGB(50,60,90), COLOR_TRANS, FONT_NORMAL);

    if (inst->license_accepted) {
        vbe_blend_rounded_rect(wx + ww - 140, wy + 280, 120, 40, 10, RGB(60,140,255), 240);
        font_draw_string(wx + ww - 120, wy + 293, "Suivant →", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
    }
    vbe_blend_rounded_rect(wx + 20, wy + 280, 100, 40, 10, RGB(180,190,210), 220);
    font_draw_string(wx + 30, wy + 293, "← Retour", RGB(50,60,90), COLOR_TRANS, FONT_NORMAL);
}

static void inst_draw_user(installer_t *inst) {
    int wx = inst->win->x + 4;
    int wy = inst->win->y + TITLE_BAR_H + 50;
    int ww = inst->win->w - 8;

    font_draw_string_shadow(wx+20, wy+10, "Créer votre compte", RGB(30,44,72), FONT_TITLE);
    font_draw_string(wx+20, wy+46, "Configurez votre identité et votre accès au système.", RGB(70,85,120), COLOR_TRANS, FONT_NORMAL);

    struct { const char *lbl; const char *val; int fy; } fields[] = {
        {"Nom complet:",     inst->fullname,  80},
        {"Nom d'utilisateur:", inst->username, 130},
        {"Nom d'hôte:",     inst->hostname,  180},
        {NULL, NULL, 0}
    };

    for (int i = 0; fields[i].lbl; i++) {
        int fy = wy + fields[i].fy;
        font_draw_string(wx+20, fy, fields[i].lbl, RGB(50,65,100), COLOR_TRANS, FONT_NORMAL);
        vbe_blend_rounded_rect(wx+20, fy+18, ww-40, 32, 6, RGB(255,255,255), 255);
        vbe_rounded_rect_outline(wx+20, fy+18, ww-40, 32, 6, 1,
                                 inst->focus_field == i ? RGB(60,140,255) : RGB(190,202,228));
        font_draw_string(wx+30, fy+26, fields[i].val[0] ? fields[i].val : "…",
                         fields[i].val[0] ? RGB(30,40,70) : RGB(160,170,200),
                         COLOR_TRANS, FONT_NORMAL);
    }

    font_draw_string(wx+20, wy+230, "Mot de passe:", RGB(50,65,100), COLOR_TRANS, FONT_NORMAL);
    vbe_blend_rounded_rect(wx+20, wy+248, ww-40, 32, 6, RGB(255,255,255), 255);
    vbe_rounded_rect_outline(wx+20, wy+248, ww-40, 32, 6, 1,
                             inst->focus_field == 3 ? RGB(60,140,255) : RGB(190,202,228));

    if (inst->password[0]) {
        char stars[65];
        int pl = inst_strlen(inst->password);
        if (pl > 64) pl = 64;
        for (int i=0; i<pl; i++) stars[i]='*';
        stars[pl] = 0;
        font_draw_string(wx+30, wy+256, stars, RGB(30,40,70), COLOR_TRANS, FONT_NORMAL);
    } else {
        font_draw_string(wx+30, wy+256, "••••••", RGB(160,170,200), COLOR_TRANS, FONT_NORMAL);
    }

    font_draw_string(wx+20, wy+288, "Note: Le compte sera créé avec les droits administrateur.", RGB(100,120,160), COLOR_TRANS, FONT_SMALL);

    vbe_blend_rounded_rect(wx + ww - 140, wy + 310, 120, 40, 10, RGB(60,140,255), 240);
    font_draw_string(wx + ww - 120, wy + 323, "Suivant →", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
    vbe_blend_rounded_rect(wx + 20, wy + 310, 100, 40, 10, RGB(180,190,210), 220);
    font_draw_string(wx + 30, wy + 323, "← Retour", RGB(50,60,90), COLOR_TRANS, FONT_NORMAL);
}

static void inst_draw_install(installer_t *inst) {
    int wx = inst->win->x + 4;
    int wy = inst->win->y + TITLE_BAR_H + 50;
    int ww = inst->win->w - 8;

    font_draw_string_shadow(wx+20, wy+10, "Installation en cours…", RGB(30,44,72), FONT_TITLE);

    const char *steps[] = {
        "Préparation du système de fichiers",
        "Copie des fichiers système",
        "Installation du chargeur d'amorçage (GRUB)",
        "Configuration des pilotes",
        "Création du compte utilisateur",
        "Configuration du réseau",
        "Finalisation de l'installation",
        NULL
    };

    int prog = inst->install_progress;
    for (int i = 0; steps[i]; i++) {
        int sy = wy + 60 + i * 28;
        int step_done = (prog > (i+1)*14);
        int step_cur  = (prog >= i*14 && prog <= (i+1)*14);
        color_t ic = step_done ? RGB(60,200,100) : (step_cur ? RGB(60,140,255) : RGB(180,190,210));
        vbe_circle_fill(wx+30, sy+8, 8, ic);
        if (step_done) font_draw_string(wx+24, sy+2, "✓", COLOR_WHITE, COLOR_TRANS, FONT_SMALL);
        else if (step_cur) {
            uint32_t pulse = timer_ms() / 300;
            vbe_circle_fill(wx+30, sy+8, 4 + (int)(pulse % 3), RGB(100,200,255));
        }
        font_draw_string(wx+46, sy+1, steps[i],
                         step_done ? RGB(60,200,100) : (step_cur ? RGB(60,140,255) : RGB(140,155,185)),
                         COLOR_TRANS, FONT_NORMAL);
    }

    int pb_y = wy + 260;
    vbe_blend_rounded_rect(wx+20, pb_y, ww-40, 24, 6, RGB(220,228,248), 230);
    if (prog > 0) {
        int pw = (ww-44) * prog / 100;
        if (pw > 0) vbe_blend_rounded_rect(wx+22, pb_y+2, pw, 20, 5, RGB(60,140,255), 240);
    }
    vbe_rounded_rect_outline(wx+20, pb_y, ww-40, 24, 6, 1, RGB(190,202,228));

    char pct_buf[8];
    char tmp[4]; int p = prog;
    if (p >= 100) { tmp[0]='1'; tmp[1]='0'; tmp[2]='0'; tmp[3]=0; }
    else if (p >= 10) { tmp[0]='0'+p/10; tmp[1]='0'+p%10; tmp[2]=0; }
    else { tmp[0]='0'+p; tmp[1]=0; }
    k_memcpy(pct_buf, tmp, 4);
    k_memcpy(pct_buf + inst_strlen(pct_buf), "%", 2);
    int pw_t = inst_strlen(pct_buf) * 8;
    font_draw_string(wx + 20 + (ww-40-pw_t)/2, pb_y+4, pct_buf, RGB(40,60,100), COLOR_TRANS, FONT_SMALL);
}

static void inst_draw_done(installer_t *inst) {
    int wx = inst->win->x + 4;
    int wy = inst->win->y + TITLE_BAR_H + 50;
    int ww = inst->win->w - 8;

    vbe_blend_rounded_rect(wx + ww/2 - 220, wy+10, 440, 200, 20, RGB(245,255,248), 250);
    vbe_rounded_rect_outline(wx + ww/2 - 220, wy+10, 440, 200, 20, 2, RGB(60,200,100));
    vbe_circle_fill(wx + ww/2, wy+70, 40, RGB(60,200,100));
    font_draw_string(wx + ww/2 - 12, wy+57, "✓", COLOR_WHITE, COLOR_TRANS, FONT_TITLE);
    font_draw_string_shadow(wx + ww/2 - 140, wy+120, "Installation réussie !", RGB(40,140,80), FONT_TITLE);
    font_draw_string(wx + ww/2 - 180, wy+158, "NovaOS 4.0 est installé et prêt à l'emploi.", RGB(60,90,60), COLOR_TRANS, FONT_NORMAL);

    font_draw_string(wx+20, wy+230, "Résumé:", RGB(50,65,100), COLOR_TRANS, FONT_LARGE);
    char line[256]; k_memset(line, 0, 256);
    k_memcpy(line, "  Utilisateur créé: ", 21);
    k_memcpy(line+20, inst->username[0] ? inst->username : "user", 20);
    font_draw_string(wx+20, wy+258, line, RGB(70,85,120), COLOR_TRANS, FONT_NORMAL);
    k_memset(line, 0, 256);
    k_memcpy(line, "  Nom d'hôte: ", 15);
    k_memcpy(line+14, inst->hostname[0] ? inst->hostname : "desktops", 20);
    font_draw_string(wx+20, wy+278, line, RGB(70,85,120), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(wx+20, wy+298, "  Redémarrez pour utiliser votre nouveau système.", RGB(70,85,120), COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(wx + ww/2 - 80, wy + 330, 160, 44, 12, RGB(60,200,100), 240);
    vbe_rounded_rect_outline(wx + ww/2 - 80, wy + 330, 160, 44, 12, 1, RGB(80,220,120));
    font_draw_string(wx + ww/2 - 56, wy + 345, "Redémarrer", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
}

static void inst_draw(installer_t *inst) {
    if (!inst->win || !inst->win->visible) return;
    int wx = inst->win->x + 2;
    int wy = inst->win->y + TITLE_BAR_H;
    int ww = inst->win->w - 4;
    int wh = inst->win->h - TITLE_BAR_H - 4;

    vbe_gradient_v(wx, wy, ww, wh, RGB(248,252,255), RGB(235,244,255));
    inst_draw_step_indicator(inst);

    switch(inst->step) {
        case INST_WELCOME:   inst_draw_welcome(inst); break;
        case INST_LICENSE:   inst_draw_license(inst); break;
        case INST_PARTITION: {

            int wx2 = inst->win->x + 24;
            int wy2 = inst->win->y + TITLE_BAR_H + 60;
            int ww2 = inst->win->w - 8;
            font_draw_string_shadow(wx2, wy2, "Partitionnement", RGB(30,44,72), FONT_TITLE);
            font_draw_string(wx2, wy2+48, "Le disque sera automatiquement partitionné :", RGB(70,85,120), COLOR_TRANS, FONT_NORMAL);
            font_draw_string(wx2, wy2+78, "  /boot   - 512 Mo (FAT32 - UEFI ESP)", RGB(70,85,120), COLOR_TRANS, FONT_NORMAL);
            font_draw_string(wx2, wy2+98, "  /       - 8 Go   (ext4 - Système)", RGB(70,85,120), COLOR_TRANS, FONT_NORMAL);
            font_draw_string(wx2, wy2+118,"  /home   - Reste  (ext4 - Données)", RGB(70,85,120), COLOR_TRANS, FONT_NORMAL);
            font_draw_string(wx2, wy2+148,"Mode: Installation automatique (recommandé)", RGB(100,140,200), COLOR_TRANS, FONT_NORMAL);

            vbe_blend_rounded_rect(wx2, wy2+176, ww2-40, 40, 6, RGB(220,228,248), 240);
            vbe_blend_rounded_rect(wx2, wy2+176, 80, 40, 6, RGB(100,160,255), 220);
            vbe_blend_rounded_rect(wx2+82, wy2+176, 150, 40, 0, RGB(80,200,120), 220);
            vbe_blend_rounded_rect(wx2+234, wy2+176, ww2-40-234, 40, 6, RGB(255,180,60), 220);
            font_draw_string(wx2+12, wy2+189, "/boot", COLOR_WHITE, COLOR_TRANS, FONT_SMALL);
            font_draw_string(wx2+96, wy2+189, "/", COLOR_WHITE, COLOR_TRANS, FONT_SMALL);
            font_draw_string(wx2+248, wy2+189, "/home", COLOR_WHITE, COLOR_TRANS, FONT_SMALL);
            vbe_blend_rounded_rect(wx2 + ww2-40 - 140, wy2+250, 120, 40, 10, RGB(60,140,255), 240);
            font_draw_string(wx2 + ww2-40 - 120, wy2+263, "Suivant →", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
            vbe_blend_rounded_rect(wx2 + 0, wy2+250, 100, 40, 10, RGB(180,190,210), 220);
            font_draw_string(wx2+10, wy2+263, "← Retour", RGB(50,60,90), COLOR_TRANS, FONT_NORMAL);
            break;
        }
        case INST_USER:      inst_draw_user(inst); break;
        case INST_SETTINGS:  {
            int wx2 = inst->win->x + 24;
            int wy2 = inst->win->y + TITLE_BAR_H + 60;
            int ww2 = inst->win->w - 8;
            font_draw_string_shadow(wx2, wy2, "Paramètres système", RGB(30,44,72), FONT_TITLE);
            font_draw_string(wx2, wy2+48, "Fuseau horaire:", RGB(50,65,100), COLOR_TRANS, FONT_NORMAL);
            vbe_blend_rounded_rect(wx2, wy2+66, 300, 30, 6, RGB(255,255,255), 255);
            vbe_rounded_rect_outline(wx2, wy2+66, 300, 30, 6, 1, RGB(190,202,228));
            font_draw_string(wx2+10, wy2+74, "Europe/Paris (UTC+2)", RGB(40,55,90), COLOR_TRANS, FONT_NORMAL);
            font_draw_string(wx2, wy2+108, "Langue:", RGB(50,65,100), COLOR_TRANS, FONT_NORMAL);
            vbe_blend_rounded_rect(wx2, wy2+126, 200, 30, 6, RGB(255,255,255), 255);
            vbe_rounded_rect_outline(wx2, wy2+126, 200, 30, 6, 1, RGB(190,202,228));
            font_draw_string(wx2+10, wy2+134, "Français (fr_FR)", RGB(40,55,90), COLOR_TRANS, FONT_NORMAL);
            font_draw_string(wx2, wy2+168, "Clavier:", RGB(50,65,100), COLOR_TRANS, FONT_NORMAL);
            vbe_blend_rounded_rect(wx2, wy2+186, 200, 30, 6, RGB(255,255,255), 255);
            vbe_rounded_rect_outline(wx2, wy2+186, 200, 30, 6, 1, RGB(190,202,228));
            font_draw_string(wx2+10, wy2+194, "AZERTY (fr)", RGB(40,55,90), COLOR_TRANS, FONT_NORMAL);
            vbe_blend_rounded_rect(wx2 + ww2 - 40 - 140, wy2+250, 120, 40, 10, RGB(60,140,255), 240);
            font_draw_string(wx2 + ww2 - 40 - 120, wy2+263, "Installer →", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
            vbe_blend_rounded_rect(wx2, wy2+250, 100, 40, 10, RGB(180,190,210), 220);
            font_draw_string(wx2+10, wy2+263, "← Retour", RGB(50,60,90), COLOR_TRANS, FONT_NORMAL);
            break;
        }
        case INST_INSTALL:   inst_draw_install(inst); break;
        case INST_DONE:      inst_draw_done(inst); break;
    }

    inst->need_redraw = 0;
}

static void inst_on_click(widget_t *w, gui_event_t *evt) {
    (void)w;
    installer_t *inst = &g_inst;
    if (evt->type != EVT_CLICK && evt->type != EVT_KEYDOWN && evt->type != EVT_CHAR) return;

    if (evt->type == EVT_CLICK) {
        int mx = evt->x - inst->win->x;
        int my = evt->y - inst->win->y - TITLE_BAR_H;
        int ww = inst->win->w - 8;

        int next_y = 50 + 310;
        int next_x = ww - 140;
        if (mx >= next_x && mx < next_x + 130 && my >= next_y && my < next_y + 50) {
            if (inst->step == INST_WELCOME) {
                inst->step = INST_LICENSE; inst->need_redraw = 1;
            } else if (inst->step == INST_LICENSE && inst->license_accepted) {
                inst->step = INST_PARTITION; inst->need_redraw = 1;
            } else if (inst->step == INST_PARTITION) {
                inst->step = INST_USER; inst->need_redraw = 1;
            } else if (inst->step == INST_USER) {
                if (!inst->username[0]) k_memcpy(inst->username, "user", 5);
                if (!inst->hostname[0]) k_memcpy(inst->hostname, "desktops", 9);
                inst->step = INST_SETTINGS; inst->need_redraw = 1;
            } else if (inst->step == INST_SETTINGS) {
                inst->step = INST_INSTALL;
                inst->installing = 1;
                inst->install_progress = 0;
                inst->need_redraw = 1;
            } else if (inst->step == INST_DONE) {
                if (inst->win) gui_close_window(inst->win);
            }
        }

        if (mx >= 20 && mx < 130 && my >= next_y && my < next_y + 50) {
            if (inst->step > INST_WELCOME) {
                inst->step = (inst_step_t)((int)inst->step - 1);
                inst->need_redraw = 1;
            }
        }

        if (inst->step == INST_LICENSE && mx >= 20 && mx < 46 && my >= 338 && my < 358) {
            inst->license_accepted = !inst->license_accepted;
            inst->need_redraw = 1;
        }

        if (inst->step == INST_USER) {
            if (my >= 148 && my < 180) inst->focus_field = 0;
            else if (my >= 198 && my < 230) inst->focus_field = 1;
            else if (my >= 248 && my < 280) inst->focus_field = 2;
            else if (my >= 298 && my < 330) inst->focus_field = 3;
            inst->need_redraw = 1;
        }
    }

    if ((evt->type == EVT_KEYDOWN || evt->type == EVT_CHAR) && inst->step == INST_USER) {
        key_event_t *k = &evt->key;
        if (k->released) return;
        char *fields[4] = {inst->fullname, inst->username, inst->hostname, inst->password};
        int maxlen[4] = {127, 63, 63, 63};
        int f = inst->focus_field;
        if (f >= 0 && f < 4) {
            if (k->scancode == KEY_BACKSPACE) {
                int l2 = inst_strlen(fields[f]);
                if (l2 > 0) fields[f][l2-1] = 0;
            } else if (k->ascii >= 32 && k->ascii < 127) {
                int l2 = inst_strlen(fields[f]);
                if (l2 < maxlen[f]) {
                    fields[f][l2] = k->ascii; fields[f][l2+1] = 0;
                }
            } else if (k->scancode == KEY_TAB) {
                inst->focus_field = (inst->focus_field + 1) % 4;
            }
        }
        inst->need_redraw = 1;
    }
}

static void inst_paint(window_t *win) {
    (void)win;
    inst_draw(&g_inst);
}

static void inst_timer_cb(void) {
    installer_t *inst = &g_inst;
    if (!inst->installing) return;
    if (inst->install_progress < 100) {
        inst->install_progress++;
        inst->need_redraw = 1;
    } else {

        inst->installing = 0;
        inst->step = INST_DONE;
        inst->need_redraw = 1;

        vfs_mkdir("/home");
        vfs_mkdir("/etc");
        char username[64];
        char fullname[128];
        char hostname[64];
        char user_home[128];
        char compat_home[128];
        char subdir[256];
        char profile[1024];
        char tutorial[1024];
        const char *dirs[] = {"Documents","Desktop","Images","Music","Videos","Downloads", NULL};

        k_memset(username, 0, sizeof(username));
        k_memset(fullname, 0, sizeof(fullname));
        k_memset(hostname, 0, sizeof(hostname));
        k_memcpy(username, inst->username[0] ? inst->username : "user", inst_strlen(inst->username[0] ? inst->username : "user") + 1);
        k_memcpy(fullname, inst->fullname[0] ? inst->fullname : "Utilisateur Nova", inst_strlen(inst->fullname[0] ? inst->fullname : "Utilisateur Nova") + 1);
        k_memcpy(hostname, inst->hostname[0] ? inst->hostname : "nova-desktop", inst_strlen(inst->hostname[0] ? inst->hostname : "nova-desktop") + 1);

        k_memset(user_home, 0, sizeof(user_home));
        k_memcpy(user_home, "/home/", 7);
        k_memcpy(user_home + 6, username, inst_strlen(username) + 1);
        vfs_mkdir(user_home);

        k_memset(compat_home, 0, sizeof(compat_home));
        k_memcpy(compat_home, "/home/user", 11);
        vfs_mkdir(compat_home);

        for (int i = 0; dirs[i]; i++) {
            k_memset(subdir, 0, sizeof(subdir));
            k_memcpy(subdir, user_home, inst_strlen(user_home));
            int hl = inst_strlen(subdir);
            subdir[hl] = '/';
            k_memcpy(subdir + hl + 1, dirs[i], inst_strlen(dirs[i]) + 1);
            vfs_mkdir(subdir);

            k_memset(subdir, 0, sizeof(subdir));
            k_memcpy(subdir, compat_home, inst_strlen(compat_home));
            hl = inst_strlen(subdir);
            subdir[hl] = '/';
            k_memcpy(subdir + hl + 1, dirs[i], inst_strlen(dirs[i]) + 1);
            vfs_mkdir(subdir);
        }

        if (!users_find(username)) {
            users_add(username, fullname, inst->password[0] ? inst->password : "1234", USER_ROLE_ADMIN);
        }

        const char *profile_text =
            "Bienvenue dans Nova Desktop !\n"
            "===========================\n"
            "- Ouvrez le Terminal avec F1\n"
            "- Ouvrez Fichiers avec F5\n"
            "- Ouvrez Parametres avec F6\n"
            "- Relancez ce guide avec F8\n"
            "\nVotre installation est prete.\n";
        const char *tutorial_text =
            "Tutoriel rapide Nova Desktop\n"
            "============================\n"
            "1. Connectez-vous depuis l'ecran de verrouillage.\n"
            "2. Lancez vos apps avec F1 a F8.\n"
            "3. Utilisez Fichiers pour parcourir le volume FAT32 en RAM.\n"
            "4. Ouvrez le Navigateur pour lire nova://tutorial et charger des pages HTTP/HTTPS live.\n"
            "5. Personnalisez l'apparence dans Parametres.\n";

        k_memset(profile, 0, sizeof(profile));
        k_memcpy(profile, profile_text, inst_strlen(profile_text) + 1);
        k_memset(tutorial, 0, sizeof(tutorial));
        k_memcpy(tutorial, tutorial_text, inst_strlen(tutorial_text) + 1);

        vfs_write_file("/etc/install.done", "done\n", 5);
        vfs_write_file("/etc/hostname", hostname, (uint32_t)inst_strlen(hostname));
        vfs_write_file("/etc/locale.conf", inst->lang[0] ? inst->lang : "fr_FR", (uint32_t)inst_strlen(inst->lang[0] ? inst->lang : "fr_FR"));

        k_memset(subdir, 0, sizeof(subdir));
        k_memcpy(subdir, user_home, inst_strlen(user_home));
        k_memcpy(subdir + inst_strlen(subdir), "/Bienvenue.txt", 15);
        vfs_write_file(subdir, profile, (uint32_t)inst_strlen(profile));

        k_memset(subdir, 0, sizeof(subdir));
        k_memcpy(subdir, user_home, inst_strlen(user_home));
        k_memcpy(subdir + inst_strlen(subdir), "/Tutoriel.txt", 14);
        vfs_write_file(subdir, tutorial, (uint32_t)inst_strlen(tutorial));

        vfs_write_file("/home/user/Bienvenue.txt", profile, (uint32_t)inst_strlen(profile));
        vfs_write_file("/home/user/Tutoriel.txt", tutorial, (uint32_t)inst_strlen(tutorial));
        gui_notify("Installation terminée");
        app_tutorial_open();
    }
}

void app_installer_open(void) {
    installer_t *inst = &g_inst;
    if (inst_open) {
        if (inst->win) { gui_focus_window(inst->win); return; }
    }

    k_memset(inst, 0, sizeof(installer_t));
    inst->step = INST_WELCOME;
    k_memcpy(inst->timezone, "Europe/Paris", 13);
    k_memcpy(inst->lang, "fr_FR", 6);
    k_memcpy(inst->username, "user", 5);
    k_memcpy(inst->fullname, "Utilisateur Nova", 17);
    k_memcpy(inst->hostname, "nova-desktop", 13);

    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    int ww = 900;
    int wh = 640;
    if (ww > sw - 40) ww = sw - 40;
    if (wh > sh - 60) wh = sh - 60;
    int wx = (sw - ww)/2;
    int wy = (sh - wh)/2;

    window_t *win = gui_create_window(wx, wy, ww, wh, "Installer NovaOS 4.0", WIN_DEFAULT & ~WIN_RESIZABLE);
    if (!win) return;
    win->bg_color = RGB(16,22,34);
    win->on_paint = inst_paint;
    inst->win = win;

    widget_t *mw = gui_add_label(win, 0, 0, ww, wh, "");
    if (mw) { mw->on_click = inst_on_click; mw->on_keydown = inst_on_click; }

    timer_register_callback(inst_timer_cb);

    gui_show_window(win);
    gui_focus_window(win);
    inst_open = 1;
}
