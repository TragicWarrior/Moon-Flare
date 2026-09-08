#include "ui_screen.h"
#include "layout.h"

#include <cJSON.h>
#include <stdio.h>
#include <string.h>
#include <vdk.h>
#include <stdlib.h>

#define COL_BG   COLOR_BLUE
#define COL_TEXT COLOR_WHITE
#define MAX_LINE 32
#define MAX_CAT  32

typedef struct {
    char id[40];
    char name[32];
    char kind[16];
} cat_dev_t;

static cat_dev_t g_cat[MAX_CAT];
static int g_ncat;

static vk_frame_t   *g_fr[3];
static vk_listbox_t *g_lb[3];
static vk_label_t   *g_status;
static vk_label_t   *g_hints;
static vk_label_t   *g_small;
static char          g_caps[3][32];
static char          g_last_hp[128];
static char          g_last_tag[24];
static char          g_last_json[65536];

static int frame_caption(vk_object_t *obj, int event, void *anything)
{
    WINDOW *canvas = vk_widget_get_canvas(VK_WIDGET(obj));
    const char *cap = anything;
    (void)event;
    if (!canvas || !cap)
        return 0;
    wattron(canvas, VDK_COLORS(COL_TEXT, COL_BG));
    mvwprintw(canvas, 0, 2, " %s ", cap);
    wattroff(canvas, VDK_COLORS(COL_TEXT, COL_BG));
    return 0;
}

static vk_frame_t *mk_card(int x, int y, char *cap)
{
    vk_frame_t *f = vk_frame_create(MF_CARD_W, MF_CARD_H);
    vk_listbox_t *lb;
    if (!f)
        return NULL;
    vk_widget_set_colors(VK_WIDGET(f), COL_TEXT, COL_BG);
    vk_frame_set_border_style(f, VK_BORDER_SINGLE);
    vk_frame_set_border_colors(f, COL_TEXT, COL_BG);
    lb = vk_listbox_create(MF_CARD_W - 2, MF_CARD_H - 2);
    vk_widget_set_colors(VK_WIDGET(lb), COL_TEXT, COL_BG);
    vk_listbox_set_wrap(lb, true);
    vk_frame_set_child(f, VK_WIDGET(lb));
    mf_ui_attach(VK_WIDGET(f), x, y);
    vk_object_register_event(VK_OBJECT(f), VK_EVENT_ON_FINALIZE,
                             frame_caption, cap);
    vk_frame_update(f);
    if (x == 0)
        g_lb[0] = lb;
    else if (x == 27)
        g_lb[1] = lb;
    else
        g_lb[2] = lb;
    return f;
}

static void ensure_cards(void)
{
    if (g_fr[0])
        return;
    g_fr[0] = mk_card(0, MF_CARD_Y, g_caps[0]);
    g_fr[1] = mk_card(27, MF_CARD_Y, g_caps[1]);
    g_fr[2] = mk_card(54, MF_CARD_Y, g_caps[2]);
}

static void show_too_small(int cols)
{
    int w = cols > 4 ? cols - 2 : 20;
    if (!g_small) {
        g_small = vk_label_create(w);
        vk_widget_set_colors(VK_WIDGET(g_small), COLOR_RED, COL_BG);
        vk_label_set_text(g_small, "Terminal too small (need >= 80x25)");
        mf_ui_attach(VK_WIDGET(g_small), 2, 8);
    } else {
        vk_widget_resize(VK_WIDGET(g_small), w, 1);
        vk_widget_show(VK_WIDGET(g_small));
    }
    vk_label_update(g_small);
}

void mf_dash_init(void)
{
    int cols = mf_ui_cols();
    int rows = mf_ui_rows();
    snprintf(g_caps[0], sizeof(g_caps[0]), "Batteries (0)");
    snprintf(g_caps[1], sizeof(g_caps[1]), "Chargers (0)");
    snprintf(g_caps[2], sizeof(g_caps[2]), "Inverters (0)");
    g_status = vk_label_create(cols > 0 ? cols : 80);
    vk_widget_set_colors(VK_WIDGET(g_status), COL_TEXT, COL_BG);
    mf_ui_attach(VK_WIDGET(g_status), 0, 1);
    g_hints = vk_label_create(cols > 0 ? cols : 80);
    vk_widget_set_colors(VK_WIDGET(g_hints), COL_TEXT, COL_BG);
    vk_label_set_text(g_hints, "F10 menu  Enter open  q quit");
    mf_ui_attach(VK_WIDGET(g_hints), 0, rows > 0 ? rows - 1 : 24);
    vk_label_update(g_hints);
    mf_dash_on_resize();
}

void mf_dash_on_resize(void)
{
    int cols = mf_ui_cols();
    int rows = mf_ui_rows();
    int i, small = (cols < MF_TUI_COLS || rows < MF_TUI_ROWS);
    int cw = cols > 0 ? cols : 80;

    if (g_status)
        vk_widget_resize(VK_WIDGET(g_status), cw, 1);
    if (g_hints) {
        vk_widget_resize(VK_WIDGET(g_hints), cw, 1);
        vk_widget_move(VK_WIDGET(g_hints), 0, rows > 0 ? rows - 1 : 24);
    }
    if (small) {
        show_too_small(cols);
        for (i = 0; i < 3; i++) {
            if (g_fr[i])
                vk_widget_hide(VK_WIDGET(g_fr[i]));
        }
        return;
    }
    if (g_small)
        vk_widget_hide(VK_WIDGET(g_small));
    ensure_cards();
    for (i = 0; i < 3; i++) {
        if (g_fr[i])
            vk_widget_show(VK_WIDGET(g_fr[i]));
    }
    mf_dash_update(g_last_hp[0] ? g_last_hp : "127.0.0.1:5250",
                   g_last_tag[0] ? g_last_tag : "WAIT",
                   g_last_json[0] ? g_last_json : NULL);
}

static void fill_lb(vk_listbox_t *lb, cJSON *arr, const char *kind)
{
    int n, i;
    vk_listbox_reset(lb);
    n = arr ? cJSON_GetArraySize(arr) : 0;
    if (n <= 0) {
        vk_listbox_add_item(lb, "not connected", NULL, NULL);
        vk_listbox_update(lb);
        return;
    }
    for (i = 0; i < n && i < MAX_LINE; i++) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
        char line[40], nm[12];
        nm[0] = '\0';
        if (cJSON_IsString(name) && name->valuestring)
            snprintf(nm, sizeof(nm), "%.8s", name->valuestring);
        if (strcmp(kind, "battery") == 0) {
            cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "pack_voltage_v");
            cJSON *s = cJSON_GetObjectItemCaseSensitive(o, "soc_pct");
            snprintf(line, sizeof(line), "%s %.1fV %.0f%%",
                     nm[0] ? nm : "pack",
                     v && cJSON_IsNumber(v) ? v->valuedouble : 0,
                     s && cJSON_IsNumber(s) ? s->valuedouble : 0);
        } else if (strcmp(kind, "charger") == 0) {
            cJSON *st = cJSON_GetObjectItemCaseSensitive(o, "charge_stage");
            cJSON *w = cJSON_GetObjectItemCaseSensitive(o, "charging_watts");
            snprintf(line, sizeof(line), "%s %.0fW %s",
                     nm[0] ? nm : "chg",
                     w && cJSON_IsNumber(w) ? w->valuedouble : 0,
                     st && cJSON_IsString(st) ? st->valuestring : "");
        } else {
            snprintf(line, sizeof(line), "%s", nm[0] ? nm : "inv");
        }
        vk_listbox_add_item(lb, line, NULL, NULL);
    }
    vk_listbox_update(lb);
}

static void cat_add(cJSON *arr, const char *kind)
{
    int i, n;
    if (!arr || !cJSON_IsArray(arr))
        return;
    n = cJSON_GetArraySize(arr);
    for (i = 0; i < n && g_ncat < MAX_CAT; i++) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        cJSON *id = cJSON_GetObjectItemCaseSensitive(o, "id");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
        cat_dev_t *d = &g_cat[g_ncat++];
        memset(d, 0, sizeof(*d));
        snprintf(d->kind, sizeof(d->kind), "%s", kind);
        if (cJSON_IsString(id) && id->valuestring)
            snprintf(d->id, sizeof(d->id), "%s", id->valuestring);
        if (cJSON_IsString(name) && name->valuestring)
            snprintf(d->name, sizeof(d->name), "%s", name->valuestring);
    }
}

int mf_dash_catalog_n(void) { return g_ncat; }
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

void mf_dash_set_visible(int vis)
{
    int i;
    for (i = 0; i < 3; i++) {
        if (g_fr[i]) {
            if (vis)
                vk_widget_show(VK_WIDGET(g_fr[i]));
            else
                vk_widget_hide(VK_WIDGET(g_fr[i]));
        }
    }
    if (g_status) {
        if (vis)
            vk_widget_show(VK_WIDGET(g_status));
        else
            vk_widget_hide(VK_WIDGET(g_status));
    }
    if (g_hints) {
        if (vis)
            vk_widget_show(VK_WIDGET(g_hints));
        else
            vk_widget_hide(VK_WIDGET(g_hints));
    }
}

void mf_dash_update(const char *hostport, const char *tag, const char *json)
{
    char st[96];
    snprintf(g_last_hp, sizeof(g_last_hp), "%s", hostport ? hostport : "");
    snprintf(g_last_tag, sizeof(g_last_tag), "%s", tag ? tag : "");
    if (json)
        snprintf(g_last_json, sizeof(g_last_json), "%s", json);
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    cJSON *b = root ? cJSON_GetObjectItemCaseSensitive(root, "batteries") : NULL;
    cJSON *c = root ? cJSON_GetObjectItemCaseSensitive(root, "chargers") : NULL;
    cJSON *i = root ? cJSON_GetObjectItemCaseSensitive(root, "inverters") : NULL;
    int nb = b && cJSON_IsArray(b) ? cJSON_GetArraySize(b) : 0;
    int nc = c && cJSON_IsArray(c) ? cJSON_GetArraySize(c) : 0;
    int ni = i && cJSON_IsArray(i) ? cJSON_GetArraySize(i) : 0;
    g_ncat = 0;
    cat_add(b, "battery");
    cat_add(c, "charger");
    cat_add(i, "inverter");

    snprintf(st, sizeof(st), "%s  [%s]",
             hostport ? hostport : "", tag ? tag : "");
    if (g_status) {
        vk_label_set_text(g_status, st);
        vk_label_update(g_status);
    }
    snprintf(g_caps[0], sizeof(g_caps[0]), "Batteries (%d)", nb);
    snprintf(g_caps[1], sizeof(g_caps[1]), "Chargers (%d)", nc);
    snprintf(g_caps[2], sizeof(g_caps[2]), "Inverters (%d)", ni);
    if (g_lb[0])
        fill_lb(g_lb[0], b, "battery");
    if (g_lb[1])
        fill_lb(g_lb[1], c, "charger");
    if (g_lb[2])
        fill_lb(g_lb[2], i, "inverter");
    if (g_fr[0])
        vk_frame_update(g_fr[0]);
    if (g_fr[1])
        vk_frame_update(g_fr[1]);
    if (g_fr[2])
        vk_frame_update(g_fr[2]);
    if (root)
        cJSON_Delete(root);
}

void mf_dash_shutdown(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        if (g_fr[i]) {
            vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_fr[i]));
            vk_frame_destroy(g_fr[i]);
            g_fr[i] = NULL;
            g_lb[i] = NULL;
        }
    }
    if (g_status) {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_status));
        vk_label_destroy(g_status);
        g_status = NULL;
    }
    if (g_hints) {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_hints));
        vk_label_destroy(g_hints);
        g_hints = NULL;
    }
    if (g_small) {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_small));
        vk_label_destroy(g_small);
        g_small = NULL;
    }
}
