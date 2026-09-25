#include "ui_screen.h"
#include "layout.h"
#include "system/system.h"
#include "tui_state.h"

#include <cJSON.h>
#include <math.h>
#include <stdarg.h>
#include <wchar.h>
#include <stdio.h>
#include <string.h>
#include <vdk.h>
#include <stdlib.h>

#define COL_BG   COLOR_BLUE
#define COL_TEXT COLOR_WHITE
#define MAX_LINE 32
#define MAX_CAT  32

/* System panel columns: names | bars.  Three solid 1-row bars, each followed
 * by a blank row (the last one keeps the cards off the bottom bar); each
 * reading is centred inside its bar.  The bars take what the names leave. */
#define SYS_ROWS       3
#define SYS_LINES      (SYS_ROWS * 2)
#define SYS_NAME_W     11
#define SYS_PAD_W      1              /* blank column right of the bars */
#define SYS_METER_MIN  10
#define SYS_FILL       COLOR_GREEN
#define SYS_TROUGH     COLOR_WHITE    /* not bold: light gray */

typedef struct {
    char id[40];
    char name[32];
    char kind[16];
    char driver[16];
    bool active;
} cat_dev_t;

/* Device cards, in keyboard order.  The grid is 3 x 2: row 0 is Batteries,
 * Chargers and the Info panel; row 1 is Inverters, Actuators, Services. */
#define NCARD 5
static const char *const g_card_kind[NCARD] = {
    "battery", "charger", "inverter", "actuator", "service"
};
static const char *const g_card_title[NCARD] = {
    "Batteries", "Chargers", "Inverters", "Actuators", "Services"
};
static const int g_card_row[NCARD] = { 0, 0, 1, 1, 1 };
static const int g_card_col[NCARD] = { 0, 1, 0, 1, 2 };
#define INFO_ROW 0
#define INFO_COL 2

static cat_dev_t g_cat[MAX_CAT];
static int g_ncat;
static int g_dash_visible = 1;
static int g_sel_card;
static int g_sel_row[NCARD];

static vk_frame_t   *g_fr[NCARD];
static vk_listbox_t *g_lb[NCARD];
static vk_frame_t   *g_info_fr;     /* row 0, column 2: weather display */
static vk_listbox_t *g_info_lb;
static char          g_info_cap[48];
static vk_box_t     *g_grid_row[2];
static vk_label_t   *g_status;
static vk_label_t   *g_hints;
static vk_label_t   *g_small;
static vk_frame_t   *g_client;      /* flat cyan/blue client-area frame */
static vk_box_t     *g_body;        /* vertical: System panel over the cards */
static vk_box_t     *g_cards_box;   /* vertical: the two grid rows */
static vk_box_t     *g_sys_body;            /* names | bars | pad */
static vk_label_t   *g_sys_pad;
static vk_box_t     *g_sys_col[2];
static vk_label_t   *g_sys_name[SYS_LINES];
static vk_progress_t *g_sys_mt[SYS_ROWS];
static vk_label_t   *g_sys_gap[SYS_ROWS];       /* blank row under each bar */
static char          g_caps[NCARD][32];
static char          g_last_hp[128];
static char          g_last_tag[24];
static char          g_last_json[65536];

/* vk_widget_hide() expands to (state & ~VK_STATE_VISIBLE) -- an unsigned-long
   result narrowed back to the uint32_t state param, which trips -Wconversion.
   Wrap it with the intended cast. */
static void hide_w(vk_widget_t *w)
{
    vk_widget_set_state(w, (uint32_t)(vk_widget_get_state(w) & ~VK_STATE_VISIBLE));
}

static int frame_caption(vk_object_t *obj, int event, void *anything)
{
    WINDOW *canvas = vk_widget_get_canvas(VK_WIDGET(obj));
    const char *cap = anything;
    (void)event;
    if (!canvas || !cap)
        return 0;
    wattron(canvas, VDK_COLORS(COL_TEXT, COL_BG) | A_BOLD);
    mvwprintw(canvas, 0, 2, " %s ", cap);
    wattroff(canvas, VDK_COLORS(COL_TEXT, COL_BG) | A_BOLD);
    return 0;
}

static vk_frame_t *mk_card(int idx, char *cap)
{
    vk_frame_t *f = vk_frame_create(MF_CARD_W, MF_CARD_H);
    vk_listbox_t *lb;
    if (!f)
        return NULL;
    vk_widget_set_colors(VK_WIDGET(f), COL_TEXT, COL_BG);
    vk_widget_set_relief_colors(VK_WIDGET(f), COLOR_WHITE, COLOR_BLACK);
    vk_frame_set_border_style(f, VK_BORDER_SINGLE | VK_RELIEF_SUNKEN);
    vk_frame_set_border_colors(f, COL_TEXT, COL_BG);
    vk_widget_set_expand(VK_WIDGET(f));
    lb = vk_listbox_create(MF_CARD_W - 2, MF_CARD_H - 2);
    /* EXPAND so the frame's resize cascade sizes the list to the frame
       interior.  Without it the list stays MF_CARD_H-2 rows; when a card is
       shrunk below its MF_CARD_H creation height (terminals under 27 rows)
       the fixed-height list overflows the interior and paints over the
       frame's bottom border, erasing most of it. */
    vk_widget_set_expand(VK_WIDGET(lb));
    vk_widget_set_colors(VK_WIDGET(lb), COL_TEXT, COL_BG);
    vk_listbox_set_wrap(lb, true);
    /* Only the selected card shows its cursor. */
    vk_listbox_set_highlight(lb, COLOR_BLACK, COLOR_CYAN);
    vk_listbox_set_unfocused(lb, COL_TEXT, COL_BG);
    vk_listbox_set_focused(lb, idx == g_sel_card);
    /* Phantom rows carry the marker at their right edge. */
    vk_listbox_set_submenu_marker(lb, mf_ui_phantom_marker());
    vk_frame_set_child(f, VK_WIDGET(lb), VK_INHERIT_NONE);
    vk_object_register_event(VK_OBJECT(f), VK_EVENT_ON_FINALIZE,
                             frame_caption, cap);
    vk_frame_update(f);
    g_lb[idx] = lb;
    return f;
}

/* The Info panel: a card-styled frame with a list that is only drawn,
 * never selected. */
static vk_frame_t *mk_info(void)
{
    vk_frame_t *f = vk_frame_create(MF_CARD_W, MF_CARD_H);
    vk_listbox_t *lb;

    vk_widget_set_colors(VK_WIDGET(f), COL_TEXT, COL_BG);
    vk_widget_set_relief_colors(VK_WIDGET(f), COLOR_WHITE, COLOR_BLACK);
    vk_frame_set_border_style(f, VK_BORDER_SINGLE | VK_RELIEF_SUNKEN);
    vk_frame_set_border_colors(f, COL_TEXT, COL_BG);
    vk_widget_set_expand(VK_WIDGET(f));
    lb = vk_listbox_create(MF_CARD_W - 2, MF_CARD_H - 2);
    vk_widget_set_expand(VK_WIDGET(lb));
    vk_widget_set_colors(VK_WIDGET(lb), COL_TEXT, COL_BG);
    vk_listbox_set_highlight(lb, COL_TEXT, COL_BG);
    vk_listbox_set_unfocused(lb, COL_TEXT, COL_BG);
    vk_listbox_set_focused(lb, false);
    vk_frame_set_child(f, VK_WIDGET(lb), VK_INHERIT_NONE);
    snprintf(g_info_cap, sizeof(g_info_cap), "Info");
    vk_object_register_event(VK_OBJECT(f), VK_EVENT_ON_FINALIZE,
                             frame_caption, g_info_cap);
    vk_frame_update(f);
    g_info_lb = lb;
    return f;
}

static int sys_meter_w(int iw)
{
    int mw = iw - SYS_NAME_W - SYS_PAD_W;
    return mw < SYS_METER_MIN ? SYS_METER_MIN : mw;
}

/* Bars run 0-100 (percent of full scale).  Solid: light gray where empty,
 * green where filled.  The in-bar reading is reverse video of the cell under
 * it, so its colour is that cell's *background*: black over both. */
static vk_progress_t *mk_sys_bar(int len)
{
    vk_progress_t *p = vk_progress_create(VK_PROGRESS_HORIZONTAL, len, 1);
    vk_widget_set_colors(VK_WIDGET(p), COL_TEXT, COL_BG);
    vk_progress_set_range(p, 0.0, 100.0);
    vk_progress_set_colors(p, SYS_FILL, COLOR_BLACK);
    vk_progress_set_trough(p, VK_TROUGH_SOLID, SYS_TROUGH, COLOR_BLACK);
    return p;
}

static vk_label_t *mk_sys_label(int w, const char *text)
{
    vk_label_t *l = vk_label_create(w);
    vk_widget_set_colors(VK_WIDGET(l), COL_TEXT, COL_BG);
    if (text)
        vk_label_set_text(l, text);
    return l;
}

/* The name column: SYS_LINES one-row labels, text only beside each bar.
 * The blank ones paint the blue background.  Colours are set on each label
 * because the column has none of its own yet when they attach, so
 * inheriting would leave them grey. */
static vk_box_t *mk_sys_name_col(const char *const *text)
{
    vk_box_t *col = vk_box_create(SYS_NAME_W, SYS_LINES, VK_BOX_VERTICAL,
                                  SYS_LINES);
    int i;

    vk_box_set_homogeneous(col, false);
    vk_widget_set_colors(VK_WIDGET(col), COL_TEXT, COL_BG);
    for (i = 0; i < SYS_LINES; i++)
    {
        g_sys_name[i] = mk_sys_label(SYS_NAME_W, i % 2 == 0 ? text[i / 2] : NULL);
        if (i % 2 == 0)   /* bold white reads bright white */
            vk_widget_set_attrs(VK_WIDGET(g_sys_name[i]), A_BOLD);
        vk_box_set_widget(col, i, VK_WIDGET(g_sys_name[i]), VK_INHERIT_NONE);
        vk_label_update(g_sys_name[i]);
    }
    return col;
}

static void mk_system(int w)
{
    static const char *const names[SYS_ROWS] = { " Input", " Capacity", " Discharge" };
    int iw = w;
    int i;

    g_sys_body = vk_box_create(iw, SYS_LINES, VK_BOX_HORIZONTAL, 3);
    vk_box_set_homogeneous(g_sys_body, false);
    vk_widget_set_colors(VK_WIDGET(g_sys_body), COL_TEXT, COL_BG);
    g_sys_col[0] = mk_sys_name_col(names);
    g_sys_col[1] = vk_box_create(sys_meter_w(iw), SYS_LINES,
                                 VK_BOX_VERTICAL, SYS_LINES);
    vk_box_set_homogeneous(g_sys_col[1], false);
    vk_widget_set_colors(VK_WIDGET(g_sys_col[1]), COL_TEXT, COL_BG);
    for (i = 0; i < 2; i++)
        vk_box_set_widget(g_sys_body, i, VK_WIDGET(g_sys_col[i]), VK_INHERIT_NONE);
    /* One blank label spans the pad column; a label fills its whole canvas. */
    g_sys_pad = mk_sys_label(SYS_PAD_W, NULL);
    vk_widget_resize(VK_WIDGET(g_sys_pad), SYS_PAD_W, SYS_LINES);
    vk_box_set_widget(g_sys_body, 2, VK_WIDGET(g_sys_pad), VK_INHERIT_NONE);
    vk_label_update(g_sys_pad);
    for (i = 0; i < SYS_ROWS; i++)
    {
        g_sys_mt[i] = mk_sys_bar(sys_meter_w(iw));
        vk_box_set_widget(g_sys_col[1], i * 2, VK_WIDGET(g_sys_mt[i]),
                          VK_INHERIT_NONE);
        g_sys_gap[i] = mk_sys_label(sys_meter_w(iw), NULL);
        vk_box_set_widget(g_sys_col[1], i * 2 + 1, VK_WIDGET(g_sys_gap[i]),
                          VK_INHERIT_NONE);
        vk_label_update(g_sys_gap[i]);
    }
}

/* The System panel, its columns and meters are fixed-size box children, so
 * the layout never resizes them; size them from the body width by hand. */
static void size_system(int w)
{
    int iw = w;
    int i;

    vk_widget_resize(VK_WIDGET(g_sys_body), iw, SYS_LINES);
    vk_widget_resize(VK_WIDGET(g_sys_col[1]), sys_meter_w(iw), SYS_LINES);
    vk_widget_resize(VK_WIDGET(g_sys_pad), SYS_PAD_W, SYS_LINES);
    vk_label_update(g_sys_pad);
    for (i = 0; i < SYS_ROWS; i++)
    {
        vk_widget_resize(VK_WIDGET(g_sys_mt[i]), sys_meter_w(iw), 1);
        vk_progress_update(g_sys_mt[i]);
        vk_widget_resize(VK_WIDGET(g_sys_gap[i]), sys_meter_w(iw), 1);
        vk_label_update(g_sys_gap[i]);
    }
}

static void ensure_cards(void)
{
    int i;
    if (g_fr[0])
        return;
    g_client = mf_ui_make_client_frame(MF_CARD_W * 3, MF_CARD_H);
    g_body = vk_box_create(MF_CARD_W * 3 - 2, MF_CARD_H - 2,
                           VK_BOX_VERTICAL, 2);
    vk_box_set_homogeneous(g_body, false);
    vk_widget_set_expand(VK_WIDGET(g_body));
    mk_system(MF_CARD_W * 3 - 2);
    g_cards_box = vk_box_create(MF_CARD_W * 3 - 2, MF_CARD_H - 2 - MF_SYS_H,
                                VK_BOX_VERTICAL, 2);
    vk_box_set_homogeneous(g_cards_box, false);
    vk_widget_set_expand(VK_WIDGET(g_cards_box));
    for (i = 0; i < 2; i++)
    {
        g_grid_row[i] = vk_box_create(MF_CARD_W * 3 - 2,
                                      (MF_CARD_H - 2 - MF_SYS_H) / 2,
                                      VK_BOX_HORIZONTAL, 3);
        vk_box_set_homogeneous(g_grid_row[i], false);
        vk_widget_set_expand(VK_WIDGET(g_grid_row[i]));
    }
    for (i = 0; i < NCARD; i++)
        g_fr[i] = mk_card(i, g_caps[i]);
    g_info_fr = mk_info();
    /* Top-down attach: frame -> body -> System panel over the cards box ->
       two grid rows -> cards.  Body, cards box and rows are EXPAND and
       inherit the frame's cyan/blue so any bare gap is blue; the System
       panel is fixed height and sized by size_system(); the cards are EXPAND
       too (each row splits its width across them) and keep their own
       white-on-blue (INHERIT_NONE). */
    mf_ui_attach(VK_WIDGET(g_client), 0, MF_CARD_Y);
    vk_frame_set_child(g_client, VK_WIDGET(g_body), VK_INHERIT_COLOR);
    vk_box_set_widget(g_body, 0, VK_WIDGET(g_sys_body), VK_INHERIT_NONE);
    vk_box_set_widget(g_body, 1, VK_WIDGET(g_cards_box), VK_INHERIT_COLOR);
    for (i = 0; i < 2; i++)
        vk_box_set_widget(g_cards_box, i, VK_WIDGET(g_grid_row[i]),
                          VK_INHERIT_COLOR);
    for (i = 0; i < NCARD; i++)
        vk_box_set_widget(g_grid_row[g_card_row[i]], g_card_col[i],
                          VK_WIDGET(g_fr[i]), VK_INHERIT_NONE);
    vk_box_set_widget(g_grid_row[INFO_ROW], INFO_COL, VK_WIDGET(g_info_fr),
                      VK_INHERIT_NONE);
}

static void show_too_small(int cols)
{
    int w = cols > 4 ? cols - 2 : 20;
    if (!g_small)
    {
        g_small = vk_label_create(w);
        vk_widget_set_colors(VK_WIDGET(g_small), COLOR_RED, COL_BG);
        vk_label_set_text(g_small, "Terminal too small (need >= 80x25)");
        mf_ui_attach(VK_WIDGET(g_small), 2, 8);
    }
    else
    {
        vk_widget_resize(VK_WIDGET(g_small), w, 1);
        vk_widget_show(VK_WIDGET(g_small));
    }
    vk_label_update(g_small);
}

void mf_dash_init(void)
{
    int cols = mf_ui_cols();
    int rows = mf_ui_rows();
    {
        int k;

        for (k = 0; k < NCARD; k++)
            snprintf(g_caps[k], sizeof(g_caps[k]), "%s (0)", g_card_title[k]);
    }
    g_status = vk_label_create(cols > 0 ? cols : 80);
    vk_widget_set_colors(VK_WIDGET(g_status), COL_TEXT, COL_BG);
    mf_ui_attach(VK_WIDGET(g_status), 0, 1);
    g_hints = vk_label_create(cols > 0 ? cols : 80);
    vk_widget_set_colors(VK_WIDGET(g_hints), COL_TEXT, COL_BG);
    vk_label_set_text(g_hints,
                      "F10 menu  Arrows select  Enter open  e edit  Space active  q quit");
    mf_ui_attach(VK_WIDGET(g_hints), 0, rows > 0 ? rows - 1 : 24);
    vk_label_update(g_hints);
    mf_dash_on_resize();
}

void mf_dash_on_resize(void)
{
    int cols = mf_ui_cols();
    int rows = mf_ui_rows();
    int small = (cols < MF_TUI_COLS || rows < MF_TUI_ROWS);
    int cw = cols > 0 ? cols : 80;

    if (g_status)
        vk_widget_resize(VK_WIDGET(g_status), cw, 1);
    if (g_hints)
    {
        vk_widget_resize(VK_WIDGET(g_hints), cw, 1);
        vk_widget_move(VK_WIDGET(g_hints), 0, rows > 0 ? rows - 1 : 24);
        /* A resize keeps the old pixels; the new columns stay black. */
        vk_label_update(g_hints);
    }
    if (!g_dash_visible)
    {
        /* Not the active view (a device/pack view is up) -- keep the client
           frame hidden so it doesn't leak in behind it on resize. */
        if (g_client)
            hide_w(VK_WIDGET(g_client));
        return;
    }
    if (small)
    {
        show_too_small(cols);
        if (g_client)
            hide_w(VK_WIDGET(g_client));
        return;
    }
    if (g_small)
        hide_w(VK_WIDGET(g_small));
    ensure_cards();
    {
        int fh = rows - MF_CARD_Y - 1;   /* client area: below status, above hints */

        if (fh < 3)
            fh = 3;
        vk_widget_resize(VK_WIDGET(g_client), cw, fh);
        vk_widget_move(VK_WIDGET(g_client), 0, MF_CARD_Y);
        /* Two levels below the client frame, so size them explicitly. */
        {
            int bw = cw - 2;
            int bh = fh - 2;
            int ch = bh - MF_SYS_H;

            vk_widget_resize(VK_WIDGET(g_body), bw, bh);
            size_system(bw);
            if (ch < 6)
                ch = 6;
            vk_widget_resize(VK_WIDGET(g_cards_box), bw, ch);
            vk_widget_resize(VK_WIDGET(g_grid_row[0]), bw, ch / 2);
            vk_widget_resize(VK_WIDGET(g_grid_row[1]), bw, ch - ch / 2);
        }
        vk_widget_show(VK_WIDGET(g_client));
    }
    mf_dash_update(g_last_hp[0] ? g_last_hp : "127.0.0.1:5250",
                   g_last_tag[0] ? g_last_tag : "WAIT",
                   g_last_json[0] ? g_last_json : NULL);
}

static int cat_index(int card, int row)
{
    int k, seen = 0;

    for (k = 0; k < g_ncat; k++)
    {
        if (strcmp(g_cat[k].kind, g_card_kind[card]) != 0)
            continue;
        if (seen == row)
            return k;
        seen++;
    }
    return -1;
}

static int card_count(int card)
{
    int k, n = 0;

    for (k = 0; k < g_ncat; k++)
        if (strcmp(g_cat[k].kind, g_card_kind[card]) == 0)
            n++;
    return n;
}

static void fill_lb(vk_listbox_t *lb, cJSON *arr, int card)
{
    const char *kind = g_card_kind[card];
    int n, i;
    vk_listbox_reset(lb);
    n = arr ? cJSON_GetArraySize(arr) : 0;
    if (n <= 0)
    {
        vk_listbox_add_item(lb, "not connected", NULL, NULL);
        vk_listbox_update(lb);
        return;
    }
    for (i = 0; i < n && i < MAX_LINE; i++)
    {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
        int ci = cat_index(card, i);
        /* [x] counts toward the System totals; [ ] is inactive. */
        const char *mark = (ci < 0 || g_cat[ci].active) ? "[x] " : "[ ] ";
        cJSON *drv = cJSON_GetObjectItemCaseSensitive(o, "driver");
        /* A phantom (an estimate from the modules it shadows) is marked at
           the row's right edge, which costs the row two columns. */
        int ph = cJSON_IsString(drv) && strcmp(drv->valuestring, "phantom") == 0;
        char line[40], nm[20], row[48], rd[24];
        nm[0] = '\0';
        rd[0] = '\0';
        if (cJSON_IsString(name) && name->valuestring)
            snprintf(nm, sizeof(nm), "%.16s", name->valuestring);
        if (strcmp(kind, "battery") == 0)
        {
            cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "pack_voltage_v");
            cJSON *s = cJSON_GetObjectItemCaseSensitive(o, "soc_pct");
            cJSON *cur = cJSON_GetObjectItemCaseSensitive(o, "current_a");
            cJSON *ns = cJSON_GetObjectItemCaseSensitive(o, "cell_count");
            cJSON *full = cJSON_GetObjectItemCaseSensitive(o, "full_capacity_ah");
            cJSON *rem = cJSON_GetObjectItemCaseSensitive(o, "remaining_capacity_ah");
            double pack = v && cJSON_IsNumber(v) ? v->valuedouble : 0;
            double ncell = ns && cJSON_IsNumber(ns) ? ns->valuedouble : 0;
            double avg = ncell > 0 ? pack / ncell : 0;
            double soc = mf_display_soc(
                s && cJSON_IsNumber(s) ? s->valuedouble : 0,
                rem && cJSON_IsNumber(rem) ? rem->valuedouble : -1,
                full && cJSON_IsNumber(full) ? full->valuedouble : 0,
                avg,
                cur && cJSON_IsNumber(cur) ? cur->valuedouble : 0,
                mf_soc_voltage_check(cJSON_IsString(drv) ? drv->valuestring : ""));
            snprintf(rd, sizeof(rd), "%.1fV %.0f%%", pack, soc);
            if (!nm[0])
                snprintf(nm, sizeof(nm), "pack");
        }
        else if (strcmp(kind, "charger") == 0)
        {
            cJSON *w = cJSON_GetObjectItemCaseSensitive(o, "charging_watts");
            snprintf(rd, sizeof(rd), "%.0fW",
                     w && cJSON_IsNumber(w) ? w->valuedouble : 0);
            if (!nm[0])
                snprintf(nm, sizeof(nm), "chg");
        }
        else if (strcmp(kind, "service") == 0)
        {
            /* A notification pathway (an SMS service) shows its credits. */
            cJSON *cr = cJSON_GetObjectItemCaseSensitive(
                cJSON_GetObjectItemCaseSensitive(o, "data"), "credits_remaining");

            if (cJSON_IsNumber(cr))
                snprintf(rd, sizeof(rd), "%.0f credits", cr->valuedouble);
            if (!nm[0])
                snprintf(nm, sizeof(nm), "svc");
        }
        else if (!nm[0])
            snprintf(nm, sizeof(nm), "inv");
        /* Shorten the name, not the reading, when the row is too narrow. */
        {
            int lw = 0;
            int room;

            vk_widget_get_metrics(VK_WIDGET(lb), &lw, NULL);
            room = lw - 2 - (int)strlen(mark) - (rd[0] ? (int)strlen(rd) + 1 : 0);
            if (ph)
                room -= 2;
            if (room < 4)
                room = 4;
            if ((int)strlen(nm) > room)
                nm[room] = '\0';
        }
        if (rd[0])
            snprintf(line, sizeof(line), "%s %s", nm, rd);
        else
            snprintf(line, sizeof(line), "%s", nm);
        snprintf(row, sizeof(row), "%s%s", mark, line);
        vk_listbox_add_item(lb, row, NULL, NULL);
        /* libviper draws the row marker (its "submenu" marker) at the
           right edge, whatever the card's width. */
        if (ph)
            vk_listbox_set_item_submenu(lb, i, true);
    }
    vk_listbox_update(lb);
}

/* Keep the cursor on a real row; hop off an empty card when another has
 * devices.  fill_lb() resets each list, so re-apply every refresh. */
static void apply_selection(void)
{
    int i, n;

    if (card_count(g_sel_card) == 0)
    {
        for (i = 0; i < NCARD; i++)
        {
            if (card_count(i) > 0)
            {
                g_sel_card = i;
                break;
            }
        }
    }
    for (i = 0; i < NCARD; i++)
    {
        n = card_count(i);
        if (g_sel_row[i] >= n)
            g_sel_row[i] = n > 0 ? n - 1 : 0;
        if (g_sel_row[i] < 0)
            g_sel_row[i] = 0;
        if (!g_lb[i])
            continue;
        vk_listbox_set_focused(g_lb[i], i == g_sel_card);
        vk_listbox_set_curr(g_lb[i], g_sel_row[i]);
        vk_listbox_update(g_lb[i]);
    }
}

static void set_sys_row(int i, double pct, const char *val)
{
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    vk_progress_set_value(g_sys_mt[i], pct);
    vk_progress_set_value_text(g_sys_mt[i], val);
    vk_progress_update(g_sys_mt[i]);
}

static double jnum_or(const cJSON *o, const char *key, double dflt)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(n) ? n->valuedouble : dflt;
}

/* ---- the Discharge meter's full scale ------------------------------ */

/* The highest discharge seen from the daemon now shown (kept between
 * runs, see tui_state.h), and whose it is. */
static double g_peak_w;
static char   g_peak_hp[128];

/* 500 W steps: a calm full scale for the Auto choice. */
static double scale_step(double w)
{
    double s = 500.0 * ceil(w / 500.0);

    return s < 500.0 ? 500.0 : s;
}

/* The high mark for this daemon, raised to w; saved when its 500 W step
 * goes up, so the file is written rarely. */
static double note_peak(double w)
{
    if (strcmp(g_peak_hp, g_last_hp) != 0)
    {
        snprintf(g_peak_hp, sizeof(g_peak_hp), "%s", g_last_hp);
        g_peak_w = mf_state_peak(g_peak_hp);
    }
    if (w > g_peak_w)
    {
        int up = g_peak_w <= 0 || scale_step(w) > scale_step(g_peak_w);

        g_peak_w = w;
        if (up)
            mf_state_set_peak(g_peak_hp, g_peak_w);
    }
    return g_peak_w;
}

/* The full scale by File > General's choice.  Batteries, inverters and a
 * fixed value fall back to Auto while unknown (or 0). */
static double discharge_scale(const cJSON *sys, double dis_w)
{
    double peak = note_peak(dis_w), v = 0.0;

    switch (mf_ui_discharge_scale())
    {
    case MF_SCALE_BATTERY:
        /* Current limit times pack voltage moves with the voltage: round it
         * to 100 W so the scale holds still. */
        v = 100.0 * round(jnum_or(sys, "battery_limit_w", 0.0) / 100.0);
        break;
    case MF_SCALE_INVERTER:
        v = jnum_or(sys, "inverter_rated_w", 0.0);
        break;
    case MF_SCALE_FIXED:
        v = mf_ui_discharge_fixed_w();
        break;
    default:
        break;
    }
    return v > 0.0 ? v : scale_step(peak);
}

/* What a Discharge choice gives right now, for File > General. */
void mf_dash_discharge_info(int mode, double fixed_w, char *out, size_t cap)
{
    cJSON *root = g_last_json[0] ? cJSON_Parse(g_last_json) : NULL;
    const cJSON *sys = cJSON_GetObjectItemCaseSensitive(root, "system");
    double peak = note_peak(0.0), v;

    switch (mode)
    {
    case MF_SCALE_BATTERY:
        v = 100.0 * round(jnum_or(sys, "battery_limit_w", 0.0) / 100.0);
        if (v > 0.0)
            snprintf(out, cap, "The batteries can deliver %.0f W.", v);
        else
            snprintf(out, cap, "The batteries don't say; uses Auto.");
        break;
    case MF_SCALE_INVERTER:
        v = jnum_or(sys, "inverter_rated_w", 0.0);
        if (v > 0.0)
            snprintf(out, cap, "The inverters are rated %.0f W.", v);
        else
            snprintf(out, cap, "No inverter rating known; uses Auto.");
        break;
    case MF_SCALE_FIXED:
        if (fixed_w > 0.0)
            snprintf(out, cap, "Full scale %.0f W.", fixed_w);
        else
            snprintf(out, cap, "Enter the watts below.");
        break;
    default:
        if (peak > 0.0)
            snprintf(out, cap, "High mark %.0f W: full scale %.0f W.", peak,
                     scale_step(peak));
        else
            snprintf(out, cap, "No discharge seen yet.");
        break;
    }
    cJSON_Delete(root);
}

/* Forget the high mark for the daemon now shown; Auto learns it again. */
void mf_dash_discharge_reset(void)
{
    (void)note_peak(0.0);               /* whose it is */
    g_peak_w = 0.0;
    mf_state_set_peak(g_peak_hp, 0.0);
}

/* Totals come from the daemon so the TUI, CLI and MCP agree; the
 * Discharge meter's scale is the TUI's own choice (File > General). */
static void fill_system(const cJSON *sys)
{
    char val[48];
    double in_w, in_max, dis_w, dis_max, chg_w, stored, cap;
    const cJSON *soc;

    if (!g_sys_body)
        return;
    if (!cJSON_IsObject(sys))
    {
        set_sys_row(0, 0, "--");
        set_sys_row(1, 0, "--");
        set_sys_row(2, 0, "--");
        return;
    }
    in_w = jnum_or(sys, "input_w", 0);
    in_max = jnum_or(sys, "input_max_w", 0);
    snprintf(val, sizeof(val), "%.0f W / %.0f W", in_w, in_max);
    set_sys_row(0, in_max > 0 ? in_w / in_max * 100.0 : 0, val);

    soc = cJSON_GetObjectItemCaseSensitive(sys, "soc_pct");
    stored = jnum_or(sys, "stored_wh", 0) / 1000.0;
    cap = jnum_or(sys, "capacity_wh", 0) / 1000.0;
    if (!cJSON_IsNumber(soc))
        set_sys_row(1, 0, "no data");
    else
    {
        if (cap > 0)
            snprintf(val, sizeof(val), "%.0f%%  %.1f / %.1f kWh",
                     soc->valuedouble, stored, cap);
        else
            snprintf(val, sizeof(val), "%.0f%%", soc->valuedouble);
        set_sys_row(1, soc->valuedouble, val);
    }

    dis_w = jnum_or(sys, "discharge_w", 0);
    chg_w = jnum_or(sys, "charge_w", 0);
    dis_max = discharge_scale(sys, dis_w);
    if (dis_w <= 0 && chg_w > 0)
        snprintf(val, sizeof(val), "0 W  (charging %.0f W)", chg_w);
    else if (dis_w > dis_max)          /* a fixed or known scale, exceeded */
        snprintf(val, sizeof(val), "%.0f W (above %.0f W)", dis_w, dis_max);
    else
        snprintf(val, sizeof(val), "%.0f W / %.0f W", dis_w, dis_max);
    set_sys_row(2, dis_w / dis_max * 100.0, val);
}

static void cat_add(cJSON *arr, const char *kind)
{
    int i, n;
    if (!arr || !cJSON_IsArray(arr))
        return;
    n = cJSON_GetArraySize(arr);
    for (i = 0; i < n && g_ncat < MAX_CAT; i++)
    {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        cJSON *id = cJSON_GetObjectItemCaseSensitive(o, "id");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
        cat_dev_t *d = &g_cat[g_ncat++];
        memset(d, 0, sizeof(*d));
        /* Older daemons send no "active": treat every device as counted. */
        d->active = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "active"));
        snprintf(d->kind, sizeof(d->kind), "%s", kind);
        if (cJSON_IsString(id) && id->valuestring)
            snprintf(d->id, sizeof(d->id), "%s", id->valuestring);
        if (cJSON_IsString(name) && name->valuestring)
            snprintf(d->name, sizeof(d->name), "%s", name->valuestring);
        {
            cJSON *drv = cJSON_GetObjectItemCaseSensitive(o, "driver");

            if (cJSON_IsString(drv) && drv->valuestring)
                snprintf(d->driver, sizeof(d->driver), "%s", drv->valuestring);
        }
    }
}

int mf_dash_catalog_n(void)
{
    return g_ncat;
}
const char *mf_dash_catalog_id(int i)
{
    return (i >= 0 && i < g_ncat) ? g_cat[i].id : "";
}
const char *mf_dash_catalog_name(int i)
{
    return (i >= 0 && i < g_ncat) ? g_cat[i].name : "";
}
const char *mf_dash_catalog_kind(int i)
{
    return (i >= 0 && i < g_ncat) ? g_cat[i].kind : "";
}
int mf_dash_catalog_active(int i)
{
    return (i >= 0 && i < g_ncat) ? g_cat[i].active : 1;
}
/* The phantom marker: an approximately-equal sign, or "~" without UTF-8. */
const char *mf_ui_phantom_marker(void)
{
    return vdk_has_utf8() ? "\xe2\x89\x88" : "~";
}

const char *mf_dash_catalog_driver(int i)
{
    return (i >= 0 && i < g_ncat) ? g_cat[i].driver : "";
}

void mf_dash_set_visible(int vis)
{
    g_dash_visible = vis;
    if (g_client)
    {
        if (vis)
            vk_widget_show(VK_WIDGET(g_client));
        else
            hide_w(VK_WIDGET(g_client));
    }
    if (g_status)
    {
        if (vis)
            vk_widget_show(VK_WIDGET(g_status));
        else
            hide_w(VK_WIDGET(g_status));
    }
    if (g_hints)
    {
        if (vis)
            vk_widget_show(VK_WIDGET(g_hints));
        else
            hide_w(VK_WIDGET(g_hints));
    }
}

/* Recomposite the cards up through the client frame. */
static void repaint_cards(void)
{
    int i;

    for (i = 0; i < NCARD; i++)
        if (g_fr[i])
            vk_frame_update(g_fr[i]);
    if (g_info_fr)
        vk_frame_update(g_info_fr);
    for (i = 0; i < 2; i++)
        if (g_grid_row[i])
            vk_box_update(g_grid_row[i]);
    if (g_cards_box)
        vk_box_update(g_cards_box);
    if (g_body)
        vk_box_update(g_body);
    if (g_client)
        vk_frame_update(g_client);
}

static void info_line(const char *fmt, ...)
{
    char line[64];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    vk_listbox_add_item(g_info_lb, line, NULL, NULL);
}

/* Symbol for a service's neutral weather icon key ("" when unknown). */
static const char *wx_symbol(const char *key, int is_day)
{
    static const struct {
        const char *key;
        const char *day;
        const char *night;
    } map[] = {
        { "clear",         "\u2600",      "\U0001F319" },  /* sun / moon */
        { "partly_cloudy", "\u26C5",      "\u2601" },      /* sun behind cloud / cloud */
        { "mostly_cloudy", "\U0001F325",  "\u2601" },      /* sun behind big cloud */
        { "cloudy",        "\u2601",      "\u2601" },
        { "wind",          "\U0001F32C",  "\U0001F32C" },  /* wind face */
        { "showers",       "\U0001F326",  "\U0001F327" },  /* sun + rain / rain */
        { "rain",          "\U0001F327",  "\U0001F327" },
        { "freezing_rain", "\U0001F327",  "\U0001F327" },
        { "thunderstorm",  "\u26C8",      "\u26C8" },      /* thunder cloud + rain */
        { "snow",          "\U0001F328",  "\U0001F328" },
        { "sleet",         "\U0001F328",  "\U0001F328" },
        { "blizzard",      "\u2744",      "\u2744" },      /* snowflake */
        { "fog",           "\U0001F32B",  "\U0001F32B" },
        { "haze",          "\U0001F32B",  "\U0001F32B" },
        { "tornado",       "\U0001F32A",  "\U0001F32A" },
        { "hurricane",     "\U0001F300",  "\U0001F300" },  /* cyclone */
        { "hot",           "\U0001F525",  "\U0001F525" },  /* fire */
        { "cold",          "\U0001F976",  "\U0001F976" },  /* cold face */
    };
    size_t i;

    for (i = 0; key && i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(key, map[i].key) == 0)
            return is_day ? map[i].day : map[i].night;
    return "";
}

/* Screen columns of a UTF-8 string. */
static int text_cols(const char *s)
{
    wchar_t w[128];
    size_t n = mbstowcs(w, s, 127);
    int c;

    if (n == (size_t)-1)
        return (int)strlen(s);
    c = wcswidth(w, n);
    return c < 0 ? (int)n : c;
}

/* The icon in a fixed 2-column slot.  Many weather symbols are 1 column by
 * wcwidth() but drawn 2 wide by some terminals; the padding space absorbs
 * that, so the text after the icon never shifts. */
static void wx_slot(const cJSON *o, char *out, size_t cap)
{
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(o, "icon");
    const char *sym = wx_symbol(cJSON_IsString(k) ? k->valuestring : NULL,
        !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "is_day")));
    int w = sym[0] ? text_cols(sym) : 0;

    snprintf(out, cap, "%s%s", sym, w >= 2 ? "" : w == 1 ? " " : "  ");
}

/* The Info panel shows the first active, online service that publishes a
 * provider-neutral "weather" object (see the weather service plugins). */
static void fill_info(const cJSON *services)
{
    const cJSON *svc, *wx = NULL;
    const char *why = NULL;
    int nsvc = 0;

    if (!g_info_lb)
        return;
    vk_listbox_reset(g_info_lb);
    cJSON_ArrayForEach(svc, services)
    {
        const cJSON *w = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(svc, "data"), "weather");

        nsvc++;
        if (!why && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(svc, "last_error")))
            why = cJSON_GetObjectItemCaseSensitive(svc, "last_error")->valuestring;
        if (!wx && cJSON_IsObject(w) &&
            !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(svc, "active")) &&
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(svc, "online")))
            wx = w;
    }
    if (!wx)
    {
        snprintf(g_info_cap, sizeof(g_info_cap), "Info");
        info_line("%s", nsvc ? "waiting for weather" : "no weather service");
        if (why)                        /* e.g. "ZIP 12345 not found" */
            info_line("%s", why);
        vk_listbox_update(g_info_lb);
        return;
    }
    {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(wx, "temp_f");
        const cJSON *cond = cJSON_GetObjectItemCaseSensitive(wx, "conditions");
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(wx, "station");
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(wx, "observed_local");
        const cJSON *fc = cJSON_GetObjectItemCaseSensitive(wx, "forecast");
        const cJSON *rise = cJSON_GetObjectItemCaseSensitive(wx, "sunrise");
        const cJSON *set = cJSON_GetObjectItemCaseSensitive(wx, "sunset");
        const cJSON *p;
        char ps[2][16], temp[2][16], line[96];
        const char *nm[2];
        int nf = 0, form;

        snprintf(g_info_cap, sizeof(g_info_cap), "Weather");
        char slot[16];
        int avail = 22;

        {
            int lw = 0;

            vk_widget_get_metrics(VK_WIDGET(g_info_lb), &lw, NULL);
            if (lw > 2)
                avail = lw - 2;         /* the listbox pads one column each side */
        }
        wx_slot(wx, slot, sizeof(slot));
        if (cJSON_IsNumber(t))
            info_line("%s %.0fF %s", slot, t->valuedouble,
                      cJSON_IsString(cond) ? cond->valuestring : "");
        else
            info_line("%s %s", slot, cJSON_IsString(cond) ? cond->valuestring : "--");
        /* The next two forecast periods on one line, in order and named by
         * what they are ("Day 85F  Night 65F", or in the evening "Night 65F
         * Day 88F"), each with its icon.  A narrow card (80 columns) drops
         * the units first, then a space; only then the icons. */
        cJSON_ArrayForEach(p, fc)
        {
            const cJSON *pt = cJSON_GetObjectItemCaseSensitive(p, "temp_f");

            if (nf >= 2)
                break;
            nm[nf] = cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p, "is_day"))
                     ? "Night" : "Day";
            if (cJSON_IsNumber(pt))
                snprintf(temp[nf], sizeof(temp[nf]), "%.0f", pt->valuedouble);
            else
                snprintf(temp[nf], sizeof(temp[nf]), "--");
            wx_slot(p, ps[nf], sizeof(ps[nf]));
            nf++;
        }
        /* 0: icons, units, two spaces between; 1: no units; 2: one space;
         * 3: no icons. */
        for (form = 0; nf > 0 && form < 4; form++)
        {
            size_t off = 0;
            int i;

            line[0] = '\0';
            for (i = 0; i < nf && off < sizeof(line); i++)
                off += (size_t)snprintf(line + off, sizeof(line) - off,
                                        "%s%s%s%s %s%s",
                                        i == 0 ? "" : form == 2 ? " " : "  ",
                                        form == 3 ? "" : ps[i],
                                        form == 3 ? "" : " ", nm[i], temp[i],
                                        (form == 0 || form == 3) &&
                                        strcmp(temp[i], "--") != 0 ? "F" : "");
            if (text_cols(line) <= avail)
                break;
        }
        if (nf > 0)
            info_line("%s", line);
        if (cJSON_IsString(rise))
            info_line("Sunrise %s", rise->valuestring);
        if (cJSON_IsString(set))
            info_line("Sunset  %s", set->valuestring);
        if (cJSON_IsString(st) || cJSON_IsString(at))
            info_line("%s %s", cJSON_IsString(st) ? st->valuestring : "",
                      cJSON_IsString(at) ? at->valuestring : "");
    }
    vk_listbox_update(g_info_lb);
}

void mf_dash_update(const char *hostport, const char *tag, const char *json)
{
    char st[96];
    cJSON *root, *arr[NCARD];
    int k;

    snprintf(g_last_hp, sizeof(g_last_hp), "%s", hostport ? hostport : "");
    snprintf(g_last_tag, sizeof(g_last_tag), "%s", tag ? tag : "");
    snprintf(st, sizeof(st), "%s  [%s]",
             hostport ? hostport : "", tag ? tag : "");
    if (g_status)
    {
        vk_label_set_text(g_status, st);
        vk_label_update(g_status);
    }
    if (!json || !json[0])
        return;

    snprintf(g_last_json, sizeof(g_last_json), "%s", json);
    root = cJSON_Parse(json);
    g_ncat = 0;
    for (k = 0; k < NCARD; k++)
    {
        static const char *const key[NCARD] = {
            "batteries", "chargers", "inverters", "actuators", "services"
        };
        cJSON *a = root ? cJSON_GetObjectItemCaseSensitive(root, key[k]) : NULL;

        arr[k] = cJSON_IsArray(a) ? a : NULL;
        cat_add(arr[k], g_card_kind[k]);
        snprintf(g_caps[k], sizeof(g_caps[k]), "%s (%d)", g_card_title[k],
                 arr[k] ? cJSON_GetArraySize(arr[k]) : 0);
    }
    for (k = 0; k < NCARD; k++)
        if (g_lb[k])
            fill_lb(g_lb[k], arr[k], k);
    fill_info(arr[4]);
    apply_selection();
    fill_system(root ? cJSON_GetObjectItemCaseSensitive(root, "system") : NULL);
    if (g_sys_body)
    {
        int r;

        for (r = 0; r < 2; r++)
            vk_box_update(g_sys_col[r]);
        vk_box_update(g_sys_body);
    }
    repaint_cards();
    if (root)
        cJSON_Delete(root);
}



static void move_card(int dir)
{
    int i, c = g_sel_card;

    for (i = 0; i < NCARD; i++)
    {
        c = (c + dir + NCARD) % NCARD;
        if (card_count(c) > 0)
        {
            g_sel_card = c;
            return;
        }
    }
}

/* Put the cursor on catalog entry `cat_idx` (e.g. chosen from the menu). */
void mf_dash_select(int cat_idx)
{
    int card, row = 0, k;

    if (cat_idx < 0 || cat_idx >= g_ncat)
        return;
    for (card = 0; card < NCARD; card++)
        if (strcmp(g_card_kind[card], g_cat[cat_idx].kind) == 0)
            break;
    if (card == NCARD)
        return;
    for (k = 0; k < cat_idx; k++)
        if (strcmp(g_cat[k].kind, g_card_kind[card]) == 0)
            row++;
    g_sel_card = card;
    g_sel_row[card] = row;
    apply_selection();
    repaint_cards();
}

int mf_dash_key(wint_t c, int *cat_idx)
{
    int n;

    if (!g_dash_visible || !g_client || mf_pack_visible())
        return MF_DASH_KEY_NONE;
    n = card_count(g_sel_card);
    switch (c)
    {
        case KEY_UP:
            if (g_sel_row[g_sel_card] > 0)
                g_sel_row[g_sel_card]--;
            break;
        case KEY_DOWN:
            if (g_sel_row[g_sel_card] < n - 1)
                g_sel_row[g_sel_card]++;
            break;
        case KEY_LEFT:
        case KEY_BTAB:
            move_card(-1);
            break;
        case KEY_RIGHT:
        case '\t':
            move_card(1);
            break;
        case '\n':
        case KEY_ENTER:
        case ' ':
        case 'e':
        case 'E':
        {
            int k = cat_index(g_sel_card, g_sel_row[g_sel_card]);

            if (k < 0 || !g_cat[k].id[0])
                return MF_DASH_KEY_HANDLED;
            if (cat_idx)
                *cat_idx = k;
            if (c == ' ')
                return MF_DASH_KEY_TOGGLE;
            if (c == 'e' || c == 'E')
                return MF_DASH_KEY_EDIT;
            /* Only batteries and chargers have a detail view so far. */
            if (g_sel_card > 1)
                return MF_DASH_KEY_HANDLED;
            return MF_DASH_KEY_OPEN;
        }
        default:
            return MF_DASH_KEY_NONE;
    }
    apply_selection();
    repaint_cards();
    return MF_DASH_KEY_HANDLED;
}

void mf_dash_shutdown(void)
{
    int i;
    for (i = 0; i < 2; i++)
    {
        int c;

        if (!g_grid_row[i])
            continue;
        for (c = 0; c < 3; c++)
            vk_box_set_widget(g_grid_row[i], c, NULL, VK_INHERIT_NONE);
    }
    if (g_cards_box)
    {
        for (i = 0; i < 2; i++)
            vk_box_set_widget(g_cards_box, i, NULL, VK_INHERIT_NONE);
    }
    if (g_body)
    {
        vk_box_set_widget(g_body, 0, NULL, VK_INHERIT_NONE);
        vk_box_set_widget(g_body, 1, NULL, VK_INHERIT_NONE);
    }
    if (g_client)
        vk_frame_set_child(g_client, NULL, VK_INHERIT_NONE);
    if (g_sys_body)
    {
        for (i = 0; i < SYS_LINES; i++)
        {
            vk_box_set_widget(g_sys_col[0], i, NULL, VK_INHERIT_NONE);
            vk_label_destroy(g_sys_name[i]);
            g_sys_name[i] = NULL;
        }
        for (i = 0; i < SYS_LINES; i++)
            vk_box_set_widget(g_sys_col[1], i, NULL, VK_INHERIT_NONE);
        for (i = 0; i < SYS_ROWS; i++)
        {
            vk_widget_destroy(VK_WIDGET(g_sys_mt[i]));
            g_sys_mt[i] = NULL;
            vk_label_destroy(g_sys_gap[i]);
            g_sys_gap[i] = NULL;
        }
        for (i = 0; i < 2; i++)
        {
            vk_box_set_widget(g_sys_body, i, NULL, VK_INHERIT_NONE);
            vk_box_destroy(g_sys_col[i]);
            g_sys_col[i] = NULL;
        }
        vk_box_set_widget(g_sys_body, 2, NULL, VK_INHERIT_NONE);
        vk_label_destroy(g_sys_pad);
        g_sys_pad = NULL;
        vk_box_destroy(g_sys_body);
        g_sys_body = NULL;
    }
    for (i = 0; i < NCARD; i++)
    {
        if (g_fr[i])
        {
            vk_frame_destroy(g_fr[i]);
            g_fr[i] = NULL;
            g_lb[i] = NULL;
        }
    }
    if (g_info_fr)
    {
        vk_frame_destroy(g_info_fr);
        g_info_fr = NULL;
        g_info_lb = NULL;
    }
    for (i = 0; i < 2; i++)
    {
        if (g_grid_row[i])
        {
            vk_box_destroy(g_grid_row[i]);
            g_grid_row[i] = NULL;
        }
    }
    if (g_cards_box)
    {
        vk_box_destroy(g_cards_box);
        g_cards_box = NULL;
    }
    if (g_body)
    {
        vk_box_destroy(g_body);
        g_body = NULL;
    }
    if (g_client)
    {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_client));
        vk_frame_destroy(g_client);
        g_client = NULL;
    }
    if (g_status)
    {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_status));
        vk_label_destroy(g_status);
        g_status = NULL;
    }
    if (g_hints)
    {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_hints));
        vk_label_destroy(g_hints);
        g_hints = NULL;
    }
    if (g_small)
    {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_small));
        vk_label_destroy(g_small);
        g_small = NULL;
    }
}

/* Left-press mask. */
#define LEFT (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)

int mf_dash_mouse(int x, int y, mmask_t bstate)
{
    int i, k, seen, row, ly;
    int fx, fy, fw, fh;
    int cw, rows, slot, top, ch, rh;
    vk_listbox_t *lb;

    if (!(bstate & LEFT))
        return 0;
    if (mf_pack_visible())
        return 0;
    if (!g_client)
        return 0;

    cw = mf_ui_cols();
    rows = mf_ui_rows();
    if (cw <= 0)
        cw = 80;
    slot = (cw - 2) / 3;
    if (slot < 1)
        slot = 1;
    top = MF_CARD_Y + 1 + MF_SYS_H;          /* grid top, below the bars */
    ch = rows - MF_CARD_Y - 1 - 2 - MF_SYS_H; /* grid height */
    if (ch < 6)
        ch = 6;
    rh = ch / 2;

    for (i = 0; i < NCARD; i++)
    {
        if (!g_fr[i] || !g_lb[i])
            continue;
        fx = 1 + g_card_col[i] * slot;       /* inside the client frame */
        fw = slot;
        fy = top + g_card_row[i] * rh;
        fh = g_card_row[i] ? ch - rh : rh;
        if (x < fx || x >= fx + fw || y < fy || y >= fy + fh)
            continue;
        lb = g_lb[i];
        ly = y - fy - 1;
        row = vk_listbox_get_scroll_pos(lb) + ly;
        if (row < 0 || row >= vk_listbox_get_item_count(lb))
            return 1;
        seen = 0;
        for (k = 0; k < g_ncat; k++)
        {
            if (strcmp(g_cat[k].kind, g_card_kind[i]) != 0)
                continue;
            if (seen == row)
            {
                if (!g_cat[k].id[0])
                    return 1;
                g_sel_card = i;
                g_sel_row[i] = row;
                apply_selection();
                repaint_cards();
                /* Only batteries and chargers have a detail view so far. */
                if (i <= 1)
                    mf_ui_open_device_view(k);
                return 1;
            }
            seen++;
        }
        return 1;
    }
    return 0;
}
