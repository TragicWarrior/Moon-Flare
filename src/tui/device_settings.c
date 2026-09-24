/*
 * Module settings dialog, in the style of vwm's Settings: every setting is
 * one row of a list ("Label ........ [value]") in a sunken frame, with
 * Modify / Save / Close below.  Modify (or Enter) edits the selected row
 * in a popup; Left/Right flips a true/false row in place.  Rows that
 * cannot be changed stay in the list, drawn in gray.  Save asks first and
 * reports the daemon's answer ("Settings saved." or the error); closing
 * with unsaved changes asks whether to discard them.
 */

#include "ui_screen.h"
#include "layout.h"

#include <cJSON.h>
#include <ctype.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vdk.h>
#include <ncursesw/curses.h>

#define COL_TEXT COLOR_BLACK
#define COL_MENU COLOR_CYAN

/* Read-only rows: bold black, which terminals show as dark gray. */
#define RO_FG    COLOR_BLACK
#define RO_ATTRS A_BOLD

#define MAX_ROWS 32

enum { ROW_TEXT = 0, ROW_NUM, ROW_BOOL };

typedef struct {
    char key[48];
    char value[160];
    char orig[160];
    int  ro;
    int  type;
} row_t;

enum { FOCUS_LIST = 0, FOCUS_MODIFY, FOCUS_SAVE, FOCUS_CLOSE, FOCUS_MAX };

/* Popups over the dialog: one editor/confirm, plus a message on top. */
enum { POP_NONE = 0, POP_MODIFY, POP_DISCARD, POP_SAVE };

static row_t          g_rows[MAX_ROWS];
static int            g_nrows;

static vk_window_t   *g_win;
static vk_box_t      *g_vbox, *g_bar;
static vk_frame_t    *g_frame;
static vk_listbox_t  *g_list;
static vk_scroller_t *g_scroll;
static vk_button_t   *g_btn[3];         /* Modify, Save/Add, Close/Cancel */
static vk_filler_t   *g_fill;
static int            g_focus;
static int            g_open;
static int            g_saving;         /* a PUT is out; waiting for reply */

static vk_popup_t    *g_pop;
static int            g_pop_kind;
static int            g_pop_row;        /* row being modified */
static int            g_pop_focus;      /* 0 = input/list, 1 = buttons */
static int            g_pop_btn;
static vk_box_t      *g_pop_client;
static vk_input_t    *g_pop_in;
static vk_listbox_t  *g_pop_lb;

static vk_popup_t    *g_msg;
static vk_box_t      *g_msg_client;

static char         g_name[32];
static char         g_id[40];
/* Add Module mode: every field editable, no device-only extras. */
static int          g_add_mode;
/* Kind of the module being edited: Graph Interval only means something for
 * batteries and chargers (they have graphs); services hide Active too. */
static char         g_kind[16];
static char         g_add_kind[16];
static char         g_add_driver[16];
/* Labels, hints and types the plugin supplied for its fields. */
#define MAX_PLAB 24
static struct {
    char key[48];
    char lab[32];
    char hint[32];
    char type[12];
} g_plab[MAX_PLAB];
static int          g_nplab;
static char         g_payload[2048];

/* Containers only detach their children when destroyed, so each part of
 * the dialog remembers the widgets it built and frees them itself:
 * containers are emptied first, then everything is destroyed. */
typedef enum { W_BOX, W_LABEL, W_FILLER, W_INPUT, W_LISTBOX, W_FRAME,
               W_BUTTON } wkind_t;
typedef struct {
    struct { void *w; wkind_t k; } w[16];
    int n;
} owned_t;
static owned_t g_own_dlg, g_own_pop, g_own_msg;

static void *own(owned_t *o, void *w, wkind_t k)
{
    if (w && o->n < 16)
    {
        o->w[o->n].w = w;
        o->w[o->n].k = k;
        o->n++;
    }
    return w;
}

static void free_owned(owned_t *o)
{
    int i;

    for (i = 0; i < o->n; i++)
    {
        if (o->w[i].k == W_BOX)
        {
            int j, n = vk_box_get_slot_count(o->w[i].w);

            for (j = 0; j < n; j++)
                vk_box_set_widget(o->w[i].w, j, NULL, VK_INHERIT_NONE);
        }
        else if (o->w[i].k == W_FRAME)
            vk_frame_set_child(o->w[i].w, NULL, VK_INHERIT_NONE);
    }
    for (i = o->n - 1; i >= 0; i--)
    {
        void *w = o->w[i].w;

        switch (o->w[i].k)
        {
        case W_BOX:     vk_box_destroy(w);     break;
        case W_LABEL:   vk_label_destroy(w);   break;
        case W_FILLER:  vk_filler_destroy(w);  break;
        case W_INPUT:   vk_input_destroy(w);   break;
        case W_LISTBOX: vk_listbox_destroy(w); break;
        case W_FRAME:   vk_frame_destroy(w);   break;
        case W_BUTTON:  vk_button_destroy(w);  break;
        }
    }
    o->n = 0;
}

static vk_window_t *g_cf_win;
static vk_label_t  *g_cf_l1, *g_cf_l2;
static int          g_cf_open;
static char         g_cf_key[16];

static void field_caption(const char *key, char *lab, size_t lab_cap,
                          char *hint, size_t hint_cap)
{
    static const struct {
        const char *k;
        const char *lab;
        const char *hint;
    } map[] = {
        { "name", "Name", "" },
        { "uuid", "UUID", "" },
        { "poll_interval_s", "Poll Interval", "(Seconds)" },
        { "capture_interval_s", "Capture Interval", "(Sec 0=off)" },
        { "retention_days", "Keep History", "(days 0=inf)" },
        { "graph_interval_min", "Graph Interval", "(minutes)" },
        { "ble.address", "BLE Address", "(MAC)" },
        { "ble.adapter", "BLE Adapter", "(hciN)" },
        { "ble.protocol", "BLE Protocol", "(JK02_32S)" },
        { "ble.password", "App Passcode", "(optional)" },
        { "usb.path", "USB Path", "(device)" },
        { "usb.serial_id", "USB Serial", "(id)" },
        { "usb.by_id", "USB By-ID", "(symlink)" },
        { "usb.baud", "USB Baud", "(baud)" },
        { "usb.addr", "USB Addr", "(addr)" },
        { "usb.auto_port", "USB Auto Port", "(true/false)" },
        { "modbus.ip", "Modbus IP", "(IPv4)" },
        { "modbus.port", "Modbus Port", "(TCP)" },
        { "modbus.unit_id", "Modbus Unit", "(unit)" },
        { "modbus.auto_net", "Modbus Auto", "(true/false)" },
        { "balance_trigger_v", "Balance Trigger", "(delta mV)" },
        { "start_balance_v", "Start Balance", "(Volts)" },
        { "cell_ovp_v", "Cell OVP", "(protect)" },
        { "cell_ovpr_v", "Cell OVPR", "(resume)" },
        { "cell_rcv_v", "Cell RCV", "(request)" },
        { "cell_count", "Cell Count", "(cells)" },
    };
    char tmp[64], *tok, *save;
    size_t i, n = 0;
    int first = 1;
    const char *suf;

    if (lab && lab_cap)
        lab[0] = '\0';
    if (hint && hint_cap)
        hint[0] = '\0';
    if (!key)
        return;
    /* The plugin's own wording wins (Add Module, and settings via _fields). */
    for (i = 0; i < (size_t)g_nplab; i++)
    {
        if (strcmp(key, g_plab[i].key) == 0 && g_plab[i].lab[0])
        {
            if (lab && lab_cap)
                snprintf(lab, lab_cap, "%s", g_plab[i].lab);
            if (hint && hint_cap && g_plab[i].hint[0])
                snprintf(hint, hint_cap, " %s", g_plab[i].hint);
            return;
        }
    }
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    {
        if (strcmp(key, map[i].k) == 0)
        {
            if (lab && lab_cap)
                snprintf(lab, lab_cap, "%s", map[i].lab);
            if (hint && hint_cap && map[i].hint[0])
                snprintf(hint, hint_cap, " %s", map[i].hint);
            return;
        }
    }
    suf = strrchr(key, '_');
    if (suf && hint && hint_cap)
    {
        if (strcmp(suf, "_s") == 0)
            snprintf(hint, hint_cap, " (Seconds)");
        else if (strcmp(suf, "_v") == 0)
            snprintf(hint, hint_cap, " (Volts)");
        else if (strcmp(suf, "_ah") == 0)
            snprintf(hint, hint_cap, " (Ah)");
        else if (strcmp(suf, "_pct") == 0)
            snprintf(hint, hint_cap, " (%%)");
    }
    if (!lab || lab_cap == 0)
        return;
    snprintf(tmp, sizeof(tmp), "%s", key);
    tok = tmp;
    while (*tok)
    {
        char word[32];
        char *end;
        int drop = 0;
        size_t k;

        while (*tok == '.' || *tok == '_' || *tok == ' ')
            tok++;
        if (!*tok)
            break;
        end = tok;
        while (*end && *end != '.' && *end != '_' && *end != ' ')
            end++;
        if ((size_t)(end - tok) >= sizeof(word))
            end = tok + sizeof(word) - 1;
        memcpy(word, tok, (size_t)(end - tok));
        word[end - tok] = '\0';
        tok = end;
        save = tok;
        while (*save == '.' || *save == '_' || *save == ' ')
            save++;
        if (!*save)
        {
            if (strcmp(word, "s") == 0 || strcmp(word, "v") == 0 ||
                strcmp(word, "a") == 0 || strcmp(word, "c") == 0 ||
                strcmp(word, "ah") == 0 || strcmp(word, "pct") == 0)
                drop = 1;
        }
        if (drop)
            continue;
        if (strcasecmp(word, "ble") == 0)
            snprintf(word, sizeof(word), "BLE");
        else if (strcasecmp(word, "usb") == 0)
            snprintf(word, sizeof(word), "USB");
        else if (strcasecmp(word, "ip") == 0)
            snprintf(word, sizeof(word), "IP");
        else if (strcasecmp(word, "id") == 0)
            snprintf(word, sizeof(word), "ID");
        else
        {
            word[0] = (char)toupper((unsigned char)word[0]);
            for (k = 1; word[k]; k++)
                word[k] = (char)tolower((unsigned char)word[k]);
        }
        n += (size_t)snprintf(lab + n, lab_cap - n, "%s%s", first ? "" : " ", word);
        first = 0;
        if (n >= lab_cap)
            break;
    }
    if (!lab[0])
        snprintf(lab, lab_cap, "%s", key);
}

static int skip_form_key(const char *k)
{
    /* Added by hand below: graph interval is TUI-only, RCV and the
     * balance voltages sit with the cell block, and the adapter name
     * is noise. */
    return k && (strcmp(k, "balance_trigger_v") == 0 ||
                 strcmp(k, "start_balance_v") == 0 ||
                 strcmp(k, "cell_rcv_v") == 0 ||
                 (strcmp(k, "ble.adapter") == 0 && !g_add_mode) ||
                 (strcmp(k, "active") == 0 && strcmp(g_kind, "service") == 0) ||
                 strcmp(k, "graph_interval_min") == 0);
}

static int field_readonly(const char *key)
{
    static const char *ro[] = {
        "uuid",
        "usb.path", "usb.serial_id", "usb.by_id", "usb.baud", "usb.auto_port",
        "ble.address", "ble.adapter",
        "modbus.ip", "modbus.port", "modbus.unit_id",
        "cell_count",
        "balance_trigger_v", "start_balance_v",
        /* This firmware echoes the previous RCV after a register write. */
        "cell_rcv_v",
    };
    size_t i;

    if (!key)
        return 1;
    /* A new device's transport is exactly what the form is for. */
    if (g_add_mode)
        return strcmp(key, "uuid") == 0;
    for (i = 0; i < sizeof(ro) / sizeof(ro[0]); i++)
    {
        if (strcmp(key, ro[i]) == 0)
            return 1;
    }
    return 0;
}

static void json_scalar(const cJSON *it, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return;
    buf[0] = '\0';
    if (!it)
        return;
    if (cJSON_IsString(it) && it->valuestring)
        snprintf(buf, cap, "%s", it->valuestring);
    else if (cJSON_IsNumber(it))
        snprintf(buf, cap, "%g", it->valuedouble);
    else if (cJSON_IsBool(it))
        snprintf(buf, cap, "%s", cJSON_IsTrue(it) ? "true" : "false");
}

static void json_field_text(const char *key, const cJSON *it,
                            char *buf, size_t cap)
{
    int volts;

    if (!buf || cap == 0)
        return;
    buf[0] = '\0';
    volts = key && (strcmp(key, "cell_ovp_v") == 0 ||
                    strcmp(key, "cell_ovpr_v") == 0 ||
                    strcmp(key, "cell_rcv_v") == 0 ||
                    strcmp(key, "start_balance_v") == 0);
    if (it && cJSON_IsNumber(it) && key &&
        strcmp(key, "balance_trigger_v") == 0)
    {
        snprintf(buf, cap, "%.0f", it->valuedouble * 1000.0);
        return;
    }
    if (it && cJSON_IsNumber(it) && volts)
    {
        snprintf(buf, cap, "%.2f", it->valuedouble);
        return;
    }

    json_scalar(it, buf, cap);
}

static int json_key_dup(const cJSON *root, const cJSON *cur)
{
    const cJSON *prev;

    if (!root || !cur || !cur->string)
        return 0;
    for (prev = root->child; prev && prev != cur; prev = prev->next)
    {
        if (prev->string && strcmp(prev->string, cur->string) == 0)
            return 1;
    }
    return 0;
}

static int json_bare(const char *s)
{
    char *end;

    if (!s || !s[0])
        return 0;
    if (strcmp(s, "true") == 0 || strcmp(s, "false") == 0 ||
        strcmp(s, "null") == 0)
        return 1;
    strtod(s, &end);
    return end != s && *end == '\0';
}

/* ---- rows ---------------------------------------------------------- */

static int row_find(const char *key)
{
    int i;

    for (i = 0; key && i < g_nrows; i++)
        if (strcmp(g_rows[i].key, key) == 0)
            return i;
    return -1;
}

static const char *plugin_type(const char *key)
{
    int i;

    for (i = 0; i < g_nplab; i++)
        if (strcmp(g_plab[i].key, key) == 0 && g_plab[i].type[0])
            return g_plab[i].type;
    return NULL;
}

static int row_type(const char *key, const cJSON *it)
{
    const char *pt = plugin_type(key);

    if (pt && strcmp(pt, "bool") == 0)
        return ROW_BOOL;
    if (pt && strcmp(pt, "number") == 0)
        return ROW_NUM;
    if (cJSON_IsBool(it))
        return ROW_BOOL;
    if (cJSON_IsNumber(it))
        return ROW_NUM;
    if (strcmp(key, "active") == 0)
        return ROW_BOOL;
    if (strcmp(key, "poll_interval_s") == 0 ||
        strcmp(key, "capture_interval_s") == 0 ||
        strcmp(key, "retention_days") == 0 ||
        strcmp(key, "graph_interval_min") == 0)
        return ROW_NUM;
    return ROW_TEXT;
}

static void add_row(const char *key, const char *val, const cJSON *it)
{
    row_t *r;

    if (g_nrows >= MAX_ROWS || !key || !key[0] || row_find(key) >= 0)
        return;
    r = &g_rows[g_nrows++];
    memset(r, 0, sizeof(*r));
    snprintf(r->key, sizeof(r->key), "%s", key);
    snprintf(r->value, sizeof(r->value), "%s", val ? val : "");
    memcpy(r->orig, r->value, sizeof(r->orig));
    r->ro = field_readonly(key);
    r->type = row_type(key, it);
}

static void add_json_rows(cJSON *root, int want_ro)
{
    cJSON *it;
    char buf[160];

    if (!root || !cJSON_IsObject(root))
        return;
    for (it = root->child; it; it = it->next)
    {
        int ro;

        if (!it->string || !it->string[0] || it->string[0] == '_')
            continue;
        if (cJSON_IsObject(it) || cJSON_IsArray(it))
            continue;
        if (strcmp(it->string, "name") == 0 ||
            strcmp(it->string, "uuid") == 0 ||
            strcmp(it->string, "poll_interval_s") == 0 ||
            strcmp(it->string, "capture_interval_s") == 0 ||
            strcmp(it->string, "retention_days") == 0)
            continue;
        if (json_key_dup(root, it) || skip_form_key(it->string))
            continue;
        ro = field_readonly(it->string);
        if ((want_ro && !ro) || (!want_ro && ro))
            continue;
        json_field_text(it->string, it, buf, sizeof(buf));
        add_row(it->string, buf, it);
    }
}

/* Editable settings first (the daemon's, then the plugin's), then the
 * read-only ones, UUID last. */
static void build_rows(const char *json)
{
    cJSON *root = json && json[0] ? cJSON_Parse(json) : NULL;
    cJSON *it;
    char buf[160];

    g_nrows = 0;
    if (root && !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        root = NULL;
    }

    buf[0] = '\0';
    json_scalar(cJSON_GetObjectItemCaseSensitive(root, "name"), buf, sizeof(buf));
    add_row("name", buf[0] || g_add_mode ? buf : g_name, NULL);

    it = cJSON_GetObjectItemCaseSensitive(root, "poll_interval_s");
    json_scalar(it, buf, sizeof(buf));
    add_row("poll_interval_s", buf[0] ? buf : "2", it);

    /* Only modules that capture history have an interval and a policy. */
    if ((it = cJSON_GetObjectItemCaseSensitive(root, "capture_interval_s")))
    {
        json_scalar(it, buf, sizeof(buf));
        add_row("capture_interval_s", buf, it);
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(root, "retention_days")))
    {
        json_scalar(it, buf, sizeof(buf));
        add_row("retention_days", buf, it);
    }

    if (!g_add_mode && (!g_kind[0] || strcmp(g_kind, "battery") == 0 ||
                        strcmp(g_kind, "charger") == 0))
        add_row("graph_interval_min", "30", NULL);

    add_json_rows(root, 0);

    json_field_text("cell_rcv_v",
                    cJSON_GetObjectItemCaseSensitive(root, "cell_rcv_v"),
                    buf, sizeof(buf));
    if (buf[0])
        add_row("cell_rcv_v", buf, NULL);

    if (root && !g_add_mode &&
        (cJSON_GetObjectItemCaseSensitive(root, "cell_ovp_v") ||
         cJSON_GetObjectItemCaseSensitive(root, "ble.address") ||
         cJSON_GetObjectItemCaseSensitive(root, "balance_trigger_v") ||
         cJSON_GetObjectItemCaseSensitive(root, "start_balance_v")))
    {
        json_field_text("balance_trigger_v",
                        cJSON_GetObjectItemCaseSensitive(root, "balance_trigger_v"),
                        buf, sizeof(buf));
        add_row("balance_trigger_v", buf[0] ? buf : "--", NULL);
        json_field_text("start_balance_v",
                        cJSON_GetObjectItemCaseSensitive(root, "start_balance_v"),
                        buf, sizeof(buf));
        add_row("start_balance_v", buf[0] ? buf : "--", NULL);
    }

    add_json_rows(root, 1);

    if (!g_add_mode)
    {
        json_scalar(cJSON_GetObjectItemCaseSensitive(root, "uuid"),
                    buf, sizeof(buf));
        add_row("uuid", buf[0] ? buf : g_id, NULL);
    }
    cJSON_Delete(root);
}

static int dirty(void)
{
    int i;

    for (i = 0; i < g_nrows; i++)
        if (!g_rows[i].ro && strcmp(g_rows[i].value, g_rows[i].orig) != 0)
            return 1;
    return 0;
}

/* ---- the main dialog ----------------------------------------------- */

static int list_text_w(void)
{
    int w = 0;

    if (g_list)
        vk_widget_get_metrics(VK_WIDGET(g_list), &w, NULL);
    return w - 3;                       /* side pads + scrollbar */
}

/* "Label ........ [value]", the value cut with an ellipsis to fit. */
static void row_text(const row_t *r, char *out, size_t cap)
{
    char lab[40], val[160];
    int w = list_text_w(), lw, vw, dots;

    field_caption(r->key, lab, sizeof(lab), NULL, 0);
    snprintf(val, sizeof(val), "%s", r->value);
    lw = (int)strlen(lab);
    vw = (int)strlen(val);
    if (lw + vw + 6 > w)                /* " .. [" + "]" at least */
    {
        int keep = w - lw - 7;

        if (keep < 1)
            keep = 1;
        if (keep < vw)
        {
            val[keep] = '\0';
            strncat(val, "\xe2\x80\xa6", sizeof(val) - strlen(val) - 1);
            vw = keep + 1;
        }
    }
    dots = w - lw - vw - 4;
    if (dots < 2)
        dots = 2;
    snprintf(out, cap, "%s %.*s [%s]", lab, dots,
             "........................................................"
             "........................................................", val);
}

static void rebuild_list(void)
{
    int i, cur;
    char text[256];

    if (!g_list)
        return;
    cur = vk_listbox_get_curr(g_list);
    vk_listbox_reset(g_list);
    for (i = 0; i < g_nrows; i++)
    {
        row_text(&g_rows[i], text, sizeof(text));
        vk_listbox_add_item(g_list, text, NULL, NULL);
        if (g_rows[i].ro)
            vk_listbox_set_item_colors(g_list, i, RO_FG, -1, RO_ATTRS);
    }
    if (cur < 0)
        cur = 0;
    if (cur >= g_nrows)
        cur = g_nrows - 1;
    if (cur >= 0)
        vk_listbox_set_curr(g_list, cur);
}

static void set_title(void)
{
    char cap[72];

    if (!g_win)
        return;
    if (g_saving)
        snprintf(cap, sizeof(cap), " %s - saving ", g_name);
    else if (dirty())
        snprintf(cap, sizeof(cap), " %s (modified) ", g_name);
    else
        snprintf(cap, sizeof(cap), " %s ", g_name);
    vk_window_set_title(g_win, cap);
}

static void highlight_buttons(void)
{
    int i;

    for (i = 0; i < 3; i++)
    {
        if (!g_btn[i])
            continue;
        vk_widget_set_colors(VK_WIDGET(g_btn[i]),
                             g_focus == FOCUS_MODIFY + i ? COLOR_YELLOW
                                                         : COL_TEXT,
                             COL_MENU);
        vk_widget_set_attrs(VK_WIDGET(g_btn[i]), A_BOLD);
        vk_button_release(g_btn[i]);
        vk_button_update(g_btn[i]);
    }
    if (g_frame)
    {
        vk_frame_set_border_colors(g_frame,
                                   g_focus == FOCUS_LIST ? COLOR_YELLOW
                                                         : COL_TEXT,
                                   COL_MENU);
        vk_frame_set_border_attrs(g_frame,
                                  g_focus == FOCUS_LIST ? A_BOLD : A_NORMAL);
    }
    if (g_list)
        vk_listbox_set_focused(g_list, g_focus == FOCUS_LIST);
}

static void paint_popups(void);

static void paint_dialog(void)
{
    if (!g_win)
        return;
    set_title();
    highlight_buttons();
    vk_listbox_update(g_list);
    if (g_scroll)
        vk_scroller_update(g_scroll);
    vk_frame_update(g_frame);
    vk_box_update(g_bar);
    vk_box_update(g_vbox);
    vk_window_update(g_win);
    paint_popups();
    mf_ui_refresh();
}

static void list_scroll_info(vk_widget_t *child, int *content_h,
                             int *content_w, int *scroll_y, int *scroll_x)
{
    vk_listbox_t *lb = VK_LISTBOX(child);
    int mw = 0;

    vk_listbox_get_metrics(lb, &mw, NULL);
    if (content_h)
        *content_h = vk_listbox_get_item_count(lb);
    if (content_w)
        *content_w = mw;
    if (scroll_y)
        *scroll_y = vk_listbox_get_curr(lb);
    if (scroll_x)
        *scroll_x = 0;
}

static vk_button_t *mk_btn(const char *txt)
{
    vk_button_t *b = vk_button_create(txt);

    if (!b)
        return NULL;
    vk_button_set_border_style(b, VK_BORDER_SINGLE);
    vk_widget_set_colors(VK_WIDGET(b), COL_TEXT, COL_MENU);
    vk_widget_set_attrs(VK_WIDGET(b), A_BOLD);
    vk_button_set_pressed_colors(b, COLOR_WHITE, COLOR_BLUE);
    return b;
}

static void build_dialog(int w, int h)
{
    int iw = w - 2, ih = h - 2;
    int lb_h = ih - 3 - 2;

    g_win = vk_window_create(w, h);
    vk_window_set_border_style(g_win, VK_BORDER_SINGLE);
    vk_window_set_border_colors(g_win, COLOR_WHITE, COL_MENU);
    vk_window_set_border_attrs(g_win, A_BOLD);
    vk_widget_set_colors(VK_WIDGET(g_win), COL_TEXT, COL_MENU);

    g_own_dlg.n = 0;
    g_vbox = own(&g_own_dlg, vk_box_create(iw, ih, VK_BOX_VERTICAL, 2), W_BOX);
    vk_box_set_homogeneous(g_vbox, false);
    vk_widget_set_colors(VK_WIDGET(g_vbox), COL_TEXT, COL_MENU);

    g_list = own(&g_own_dlg, vk_listbox_create(iw - 2, lb_h), W_LISTBOX);
    vk_listbox_set_wrap(g_list, false);
    vk_listbox_set_highlight(g_list, COLOR_BLACK, COLOR_RED);
    vk_listbox_set_unfocused(g_list, COLOR_BLACK, COLOR_WHITE);
    vk_widget_set_colors(VK_WIDGET(g_list), COL_TEXT, COL_MENU);

    g_frame = own(&g_own_dlg, vk_frame_create(iw, lb_h + 2), W_FRAME);
    vk_frame_set_border_style(g_frame, VK_BORDER_SINGLE | VK_RELIEF_SUNKEN);
    vk_frame_set_border_colors(g_frame, COLOR_YELLOW, COL_MENU);
    vk_frame_set_border_attrs(g_frame, A_BOLD);
    vk_frame_set_child(g_frame, VK_WIDGET(g_list), VK_INHERIT_NONE);
    vk_widget_set_expand(VK_WIDGET(g_frame));

    g_scroll = vk_scroller_create(VK_SCROLLBAR_VERTICAL);
    vk_scroller_set_border_style(g_scroll, VK_BORDER_SINGLE);
    vk_scroller_set_border_colors(g_scroll, COL_TEXT, COL_MENU);
    vk_scroller_set_scroll_source(g_scroll, VK_WIDGET(g_list));
    vk_scroller_set_scroll_info(g_scroll, list_scroll_info);
    vk_widget_attach_scroller(VK_WIDGET(g_list), g_scroll);

    g_bar = own(&g_own_dlg, vk_box_create(iw, 3, VK_BOX_HORIZONTAL, 4), W_BOX);
    vk_box_set_homogeneous(g_bar, false);
    vk_widget_set_colors(VK_WIDGET(g_bar), COL_TEXT, COL_MENU);
    g_btn[0] = own(&g_own_dlg, mk_btn("Modify"), W_BUTTON);
    g_btn[1] = own(&g_own_dlg, mk_btn(g_add_mode ? "Add" : "Save"), W_BUTTON);
    g_btn[2] = own(&g_own_dlg, mk_btn(g_add_mode ? "Cancel" : "Close"),
                   W_BUTTON);
    g_fill = own(&g_own_dlg, vk_filler_create(), W_FILLER);
    vk_widget_set_colors(VK_WIDGET(g_fill), COL_TEXT, COL_MENU);
    vk_widget_set_expand(VK_WIDGET(g_fill));
    vk_box_set_widget(g_bar, 0, VK_WIDGET(g_btn[0]), VK_INHERIT_NONE);
    vk_box_set_widget(g_bar, 1, VK_WIDGET(g_fill), VK_INHERIT_NONE);
    vk_box_set_widget(g_bar, 2, VK_WIDGET(g_btn[1]), VK_INHERIT_NONE);
    vk_box_set_widget(g_bar, 3, VK_WIDGET(g_btn[2]), VK_INHERIT_NONE);

    vk_box_set_widget(g_vbox, 0, VK_WIDGET(g_frame), VK_INHERIT_NONE);
    vk_box_set_widget(g_vbox, 1, VK_WIDGET(g_bar), VK_INHERIT_NONE);
    vk_window_set_child(g_win, VK_WIDGET(g_vbox), VK_INHERIT_NONE);
}

static void destroy_dialog(void)
{
    if (g_list && g_scroll)
        vk_widget_attach_scroller(VK_WIDGET(g_list), NULL);
    if (g_scroll)
        vk_scroller_destroy(g_scroll);
    if (g_win)
    {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_win));
        vk_window_set_child(g_win, NULL, VK_INHERIT_NONE);
        vk_window_destroy(g_win);
    }
    free_owned(&g_own_dlg);
    g_win = NULL;
    g_vbox = g_bar = NULL;
    g_frame = NULL;
    g_list = NULL;
    g_scroll = NULL;
    g_btn[0] = g_btn[1] = g_btn[2] = NULL;
    g_fill = NULL;
}

/* ---- popups -------------------------------------------------------- */

static void front_restack(void)
{
    mf_ui_front_clear();
    if (g_win)
        mf_ui_front_push(VK_WIDGET(g_win));
    if (g_pop)
        mf_ui_front_push(VK_WIDGET(g_pop));
    if (g_msg)
        mf_ui_front_push(VK_WIDGET(g_msg));
}

static void center(int w, int h, int *x, int *y)
{
    *x = (mf_ui_cols() - w) / 2;
    *y = (mf_ui_rows() - h) / 2;
    if (*x < 0)
        *x = 0;
    if (*y < 1)
        *y = 1;
}

/* A popup in one color scheme, its button bar painted to match. */
static vk_popup_t *pop_new(int w, int h, const char *title, int fg, int bg,
                           const char *b1, const char *b2)
{
    vk_popup_t *p = b2 ? vk_popup_create(w, h, VK_BORDER_SINGLE, b1, b2, NULL)
                       : vk_popup_create(w, h, VK_BORDER_SINGLE, b1, NULL);
    vk_box_t *bar;

    if (!p)
        return NULL;
    vk_popup_set_title(p, title);
    vk_popup_set_border_colors(p, (short)fg, (short)bg);
    vk_popup_set_border_attrs(p, A_BOLD);
    vk_popup_set_colors(p, (short)fg, (short)bg);
    vk_popup_set_button_colors(p, (short)fg, (short)bg);
    vk_popup_set_button_attrs(p, A_BOLD);
    bar = vk_popup_get_button_bar(p);
    if (bar)
    {
        vk_widget_set_colors(VK_WIDGET(bar), fg, bg);
        vk_widget_fill(VK_WIDGET(bar),
                       ' ' | COLOR_PAIR(vdk_color_pair((short)fg, (short)bg)));
    }
    return p;
}

/* The active button in yellow; focus 0 (the client) lights none. */
static void pop_buttons(vk_popup_t *p, int lit, int fg, int bg)
{
    int i, n;

    if (!p)
        return;
    n = vk_popup_get_button_count(p);
    for (i = 0; i < n; i++)
    {
        vk_button_t *b = vk_popup_get_button(p, i);

        vk_button_release(b);
        vk_widget_set_colors(VK_WIDGET(b), i == lit ? COLOR_YELLOW : fg, bg);
        vk_widget_set_attrs(VK_WIDGET(b), A_BOLD);
        vk_button_update(b);
    }
}

/* Top pad, one centred label per line, bottom pad. */
static vk_box_t *pop_lines(owned_t *o, int w, int fg, int bg, const char *l1,
                           const char *l2)
{
    int n = l2 ? 4 : 3, i = 0;
    vk_box_t *box = own(o, vk_box_create(w, n, VK_BOX_VERTICAL, n), W_BOX);
    const char *lines[2] = { l1, l2 };
    int k;

    vk_box_set_homogeneous(box, true);
    vk_widget_set_colors(VK_WIDGET(box), fg, bg);
    {
        vk_filler_t *pad = own(o, vk_filler_create(), W_FILLER);

        vk_widget_set_colors(VK_WIDGET(pad), fg, bg);
        vk_box_set_widget(box, i++, VK_WIDGET(pad), VK_INHERIT_NONE);
    }
    for (k = 0; k < (l2 ? 2 : 1); k++)
    {
        vk_label_t *lab = own(o, vk_label_create(w), W_LABEL);

        vk_label_set_justify(lab, VK_JUSTIFY_CENTER);
        vk_label_set_text(lab, lines[k]);
        vk_widget_set_colors(VK_WIDGET(lab), fg, bg);
        vk_label_update(lab);
        vk_box_set_widget(box, i++, VK_WIDGET(lab), VK_INHERIT_NONE);
    }
    {
        vk_filler_t *pad = own(o, vk_filler_create(), W_FILLER);

        vk_widget_set_colors(VK_WIDGET(pad), fg, bg);
        vk_box_set_widget(box, i, VK_WIDGET(pad), VK_INHERIT_NONE);
    }
    return box;
}

static void pop_show(vk_popup_t *p, vk_box_t *client, int w, int h)
{
    int x, y;
    uint32_t st;

    vk_popup_set_client(p, VK_WIDGET(client));
    st = vk_widget_get_state(VK_WIDGET(client));
    vk_widget_set_state(VK_WIDGET(client), st & ~(uint32_t)VK_STATE_EXPAND);
    center(w, h, &x, &y);
    mf_ui_attach(VK_WIDGET(p), x, y);
}

static void msg_close(void)
{
    if (!g_msg)
        return;
    vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_msg));
    vk_popup_destroy(g_msg);
    free_owned(&g_own_msg);
    g_msg = NULL;
    g_msg_client = NULL;
    front_restack();
    paint_dialog();
}

/* "Saved" (blue) or an error (red on white); OK closes it. */
static void msg_show(const char *title, const char *l1, const char *l2,
                     int error)
{
    int fg = error ? COLOR_RED : COLOR_WHITE;
    int bg = error ? COLOR_WHITE : COLOR_BLUE;
    int w = 44, h = l2 ? 9 : 8;
    size_t n1 = l1 ? strlen(l1) : 0, n2 = l2 ? strlen(l2) : 0;

    if ((int)n1 + 6 > w)
        w = (int)n1 + 6;
    if ((int)n2 + 6 > w)
        w = (int)n2 + 6;
    if (w > mf_ui_cols() - 2)
        w = mf_ui_cols() - 2;
    if (g_msg)
        msg_close();
    g_msg = pop_new(w, h, title, fg, bg, "OK", NULL);
    if (!g_msg)
        return;
    g_own_msg.n = 0;
    g_msg_client = pop_lines(&g_own_msg, w - 2, fg, bg, l1 ? l1 : "", l2);
    pop_buttons(g_msg, 0, fg, bg);
    pop_show(g_msg, g_msg_client, w, h);
    vk_widget_fill(VK_WIDGET(g_msg_client),
                   ' ' | COLOR_PAIR(vdk_color_pair((short)fg, (short)bg)));
    front_restack();
    paint_dialog();
}

static void pop_close(void)
{
    if (!g_pop)
        return;
    vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_pop));
    vk_popup_destroy(g_pop);
    free_owned(&g_own_pop);
    g_pop = NULL;
    g_pop_client = NULL;
    g_pop_in = NULL;
    g_pop_lb = NULL;
    g_pop_kind = POP_NONE;
    front_restack();
    paint_dialog();
}

static void pop_colors(int *fg, int *bg)
{
    if (g_pop_kind == POP_MODIFY)
    {
        *fg = COLOR_WHITE;
        *bg = COLOR_BLUE;
    }
    else
    {
        *fg = COLOR_RED;
        *bg = COLOR_WHITE;
    }
}

static void paint_popups(void)
{
    int fg, bg;

    if (g_pop)
    {
        pop_colors(&fg, &bg);
        if (g_pop_in)
        {
            vk_widget_set_colors(VK_WIDGET(g_pop_in),
                                 g_pop_focus == 0 ? COLOR_CYAN : COLOR_WHITE,
                                 COLOR_BLUE);
            vk_input_show_cursor(g_pop_in, g_pop_focus == 0);
            vk_input_update(g_pop_in);
        }
        if (g_pop_lb)
        {
            vk_listbox_set_focused(g_pop_lb, g_pop_focus == 0);
            vk_listbox_update(g_pop_lb);
        }
        pop_buttons(g_pop, g_pop_focus == 1 ? g_pop_btn : -1, fg, bg);
        if (g_pop_client)
            vk_box_update(g_pop_client);
        vk_popup_update(g_pop);
    }
    if (g_msg)
    {
        if (g_msg_client)
            vk_box_update(g_msg_client);
        vk_popup_update(g_msg);
    }
}

static void modify_open(int ri)
{
    row_t *r;
    char title[64], prompt[80], lab[40], hint[32];
    int w, h;

    if (ri < 0 || ri >= g_nrows)
        return;
    r = &g_rows[ri];
    field_caption(r->key, lab, sizeof(lab), hint, sizeof(hint));
    if (r->ro)
    {
        char l1[96];

        snprintf(l1, sizeof(l1), "%s is read-only.", lab);
        msg_show(" Read-only ", l1, "It can't be changed here.", 0);
        return;
    }
    snprintf(title, sizeof(title), " Modify: %s ", lab);
    g_pop_row = ri;
    g_pop_focus = 0;
    g_pop_btn = 0;
    g_pop_kind = POP_MODIFY;
    g_own_pop.n = 0;
    if (r->type == ROW_BOOL)
    {
        w = 36;
        h = 9;
        g_pop = pop_new(w, h, title, COLOR_WHITE, COLOR_BLUE, "Apply", "Cancel");
        g_pop_client = own(&g_own_pop,
                           vk_box_create(w - 2, h - 5, VK_BOX_VERTICAL, 1), W_BOX);
        vk_box_set_homogeneous(g_pop_client, false);
        vk_widget_set_colors(VK_WIDGET(g_pop_client), COLOR_WHITE, COLOR_BLUE);
        g_pop_lb = own(&g_own_pop, vk_listbox_create(w - 2, h - 5), W_LISTBOX);
        vk_listbox_set_wrap(g_pop_lb, false);
        vk_listbox_set_highlight(g_pop_lb, COLOR_BLACK, COLOR_RED);
        vk_listbox_set_unfocused(g_pop_lb, COLOR_BLACK, COLOR_WHITE);
        vk_widget_set_colors(VK_WIDGET(g_pop_lb), COLOR_WHITE, COLOR_BLUE);
        vk_listbox_add_item(g_pop_lb, "true", NULL, NULL);
        vk_listbox_add_item(g_pop_lb, "false", NULL, NULL);
        vk_listbox_set_curr(g_pop_lb, strcmp(r->value, "false") == 0 ? 1 : 0);
        vk_box_set_widget(g_pop_client, 0, VK_WIDGET(g_pop_lb), VK_INHERIT_NONE);
    }
    else
    {
        vk_label_t *pl;

        w = 48;
        h = 9;
        g_pop = pop_new(w, h, title, COLOR_WHITE, COLOR_BLUE, "Apply", "Cancel");
        g_pop_client = own(&g_own_pop,
                           vk_box_create(w - 2, h - 5, VK_BOX_VERTICAL, 2), W_BOX);
        vk_box_set_homogeneous(g_pop_client, false);
        vk_widget_set_colors(VK_WIDGET(g_pop_client), COLOR_WHITE, COLOR_BLUE);
        pl = own(&g_own_pop, vk_label_create(w - 2), W_LABEL);
        snprintf(prompt, sizeof(prompt), "  %s%s:", lab, hint);
        vk_label_set_text(pl, prompt);
        vk_widget_set_colors(VK_WIDGET(pl), COLOR_WHITE, COLOR_BLUE);
        vk_label_update(pl);
        vk_box_set_widget(g_pop_client, 0, VK_WIDGET(pl), VK_INHERIT_NONE);
        g_pop_in = own(&g_own_pop, vk_input_create(w - 4), W_INPUT);
        vk_input_set_border_style(g_pop_in, VK_BORDER_SINGLE);
        vk_input_set_text(g_pop_in, r->value);
        vk_box_set_widget(g_pop_client, 1, VK_WIDGET(g_pop_in), VK_INHERIT_NONE);
    }
    pop_show(g_pop, g_pop_client, w, h);
    front_restack();
    paint_dialog();
}

static int numeric(const char *s)
{
    char *end;

    if (!s || !s[0])
        return 1;                       /* empty: let the daemon decide */
    strtod(s, &end);
    return *end == '\0';
}

static void modify_apply(void)
{
    row_t *r = &g_rows[g_pop_row];
    char val[160];

    if (g_pop_lb)
        snprintf(val, sizeof(val), "%s",
                 vk_listbox_get_curr(g_pop_lb) == 1 ? "false" : "true");
    else
        snprintf(val, sizeof(val), "%s",
                 g_pop_in ? vk_input_get_text(g_pop_in) : "");
    if (r->type == ROW_NUM && !numeric(val))
    {
        char lab[40], l1[96];

        field_caption(r->key, lab, sizeof(lab), NULL, 0);
        snprintf(l1, sizeof(l1), "%s must be a number.", lab);
        msg_show(" Error ", l1, NULL, 1);
        return;
    }
    snprintf(r->value, sizeof(r->value), "%s", val);
    pop_close();
    rebuild_list();
    paint_dialog();
}

static void confirm_open(int kind)
{
    int w = 42, h = 9;

    g_own_pop.n = 0;
    g_pop_kind = kind;
    g_pop_focus = 1;
    g_pop_btn = 0;
    if (kind == POP_DISCARD)
    {
        g_pop = pop_new(w, h, " Confirm ", COLOR_RED, COLOR_WHITE,
                        "Discard", "Cancel");
        g_pop_client = pop_lines(&g_own_pop, w - 2, COLOR_RED, COLOR_WHITE,
                                 "You have unsaved changes.",
                                 "Discard changes and close?");
    }
    else
    {
        g_pop = pop_new(w, h, " Confirm Save ", COLOR_RED, COLOR_WHITE,
                        "Save", "Cancel");
        g_pop_client = pop_lines(&g_own_pop, w - 2, COLOR_RED, COLOR_WHITE,
                                 "Save module settings?",
                                 "Changes take effect now.");
    }
    pop_show(g_pop, g_pop_client, w, h);
    vk_widget_fill(VK_WIDGET(g_pop_client),
                   ' ' | COLOR_PAIR(vdk_color_pair(COLOR_RED, COLOR_WHITE)));
    front_restack();
    paint_dialog();
}

/* ---- actions ------------------------------------------------------- */

static void on_close(void)
{
    if (dirty() && !g_saving)
    {
        confirm_open(POP_DISCARD);
        return;
    }
    mf_devset_close();
}

/* 2: the caller sends the payload (PUT, or POST in add mode). */
static int on_save(void)
{
    if (g_saving)
        return 1;
    if (g_add_mode)
        return 2;
    if (!dirty())
    {
        msg_show(" Saved ", "No changes to save.", NULL, 0);
        return 1;
    }
    confirm_open(POP_SAVE);
    return 1;
}

static int pop_key(wint_t c)
{
    int n = g_pop ? vk_popup_get_button_count(g_pop) : 0;

    if (c == 27 || c == KEY_EXIT || c == KEY_CANCEL)
    {
        pop_close();
        return 1;
    }
    if (c == '\t'
#ifdef KEY_BTAB
        || c == KEY_BTAB
#endif
       )
    {
        if (g_pop_kind == POP_MODIFY)
        {
            /* input -> Apply -> Cancel -> input */
            if (g_pop_focus == 0)
            {
                g_pop_focus = 1;
                g_pop_btn = 0;
            }
            else if (g_pop_btn + 1 < n)
                g_pop_btn++;
            else
                g_pop_focus = 0;
        }
        else
            g_pop_btn = (g_pop_btn + 1) % (n ? n : 1);
        paint_dialog();
        return 1;
    }
    if (g_pop_focus == 1 && (c == KEY_LEFT || c == KEY_RIGHT))
    {
        g_pop_btn = (g_pop_btn + (c == KEY_LEFT ? n - 1 : 1)) % (n ? n : 1);
        paint_dialog();
        return 1;
    }
    if (c == '\n' || c == KEY_ENTER || (c == ' ' && g_pop_focus == 1))
    {
        int apply = g_pop_focus == 0 || g_pop_btn == 0;

        if (g_pop_kind == POP_MODIFY)
        {
            if (apply)
                modify_apply();
            else
                pop_close();
            return 1;
        }
        if (g_pop_kind == POP_DISCARD)
        {
            pop_close();
            if (apply)
                mf_devset_close();
            return 1;
        }
        if (g_pop_kind == POP_SAVE)
        {
            pop_close();
            if (apply)
            {
                g_saving = 1;
                paint_dialog();
                return 2;
            }
            return 1;
        }
        return 1;
    }
    if (g_pop_kind != POP_MODIFY || g_pop_focus != 0)
        return 1;
    if (g_pop_lb)
    {
        if (c == KEY_UP)
            vk_listbox_set_prev(g_pop_lb);
        else if (c == KEY_DOWN)
            vk_listbox_set_next(g_pop_lb);
        paint_dialog();
        return 1;
    }
    if (!g_pop_in)
        return 1;
    if (c == KEY_BACKSPACE || c == 127 || c == 8)
        vk_input_backspace(g_pop_in);
    else if (c == KEY_LEFT)
        vk_input_move_cursor(g_pop_in, -1);
    else if (c == KEY_RIGHT)
        vk_input_move_cursor(g_pop_in, 1);
    else if (c >= 32 && c < 127)
        vk_input_insert_char(g_pop_in, (int)c);
    else
        return 1;
    paint_dialog();
    return 1;
}

static void select_row(int i)
{
    if (i < 0 || i >= g_nrows || !g_list)
        return;
    vk_listbox_set_curr(g_list, i);
}

static void toggle_bool(int i)
{
    row_t *r;

    if (i < 0 || i >= g_nrows)
        return;
    r = &g_rows[i];
    if (r->ro || r->type != ROW_BOOL)
        return;
    snprintf(r->value, sizeof(r->value), "%s",
             strcmp(r->value, "true") == 0 ? "false" : "true");
    rebuild_list();
}

static int activate_focus(void)
{
    switch (g_focus)
    {
    case FOCUS_LIST:
    case FOCUS_MODIFY:
        modify_open(vk_listbox_get_curr(g_list));
        return 1;
    case FOCUS_SAVE:
        return on_save();
    case FOCUS_CLOSE:
        on_close();
        return 1;
    }
    return 1;
}

/* ---- public API ---------------------------------------------------- */

void mf_devset_close(void)
{
    if (!g_open)
        return;
    if (g_msg)
    {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_msg));
        vk_popup_destroy(g_msg);
        free_owned(&g_own_msg);
        g_msg = NULL;
    }
    if (g_pop)
    {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_pop));
        vk_popup_destroy(g_pop);
        free_owned(&g_own_pop);
        g_pop = NULL;
    }
    g_pop_in = NULL;
    g_pop_lb = NULL;
    g_pop_client = NULL;
    g_msg_client = NULL;
    g_pop_kind = POP_NONE;
    destroy_dialog();
    g_open = 0;
    g_add_mode = 0;
    g_saving = 0;
    g_nplab = 0;
    g_nrows = 0;
    mf_ui_front_clear();
    mf_ui_refresh();
}

int mf_devset_open(void)
{
    return g_open;
}

int mf_devset_touched(void)
{
    return g_open && dirty();
}

int mf_devset_has_key(const char *key)
{
    return row_find(key) >= 0;
}

const char *mf_devset_id(void)
{
    return g_id;
}

const char *mf_devset_poll_text(void)
{
    int i = row_find("poll_interval_s");

    return i >= 0 ? g_rows[i].value : "2";
}

int mf_devset_get_graph_interval(void)
{
    int i = row_find("graph_interval_min");
    int val;

    if (i < 0)
        return 0;                       /* not shown for this module */
    val = atoi(g_rows[i].value);
    return val < 1 ? 1 : val;
}

void mf_devset_set_kind(const char *kind)
{
    snprintf(g_kind, sizeof(g_kind), "%s", kind ? kind : "");
}

void mf_devset_set_graph_interval(int minutes)
{
    int i = row_find("graph_interval_min");

    if (i < 0)
        return;
    snprintf(g_rows[i].value, sizeof(g_rows[i].value), "%d",
             minutes < 1 ? 1 : minutes);
    memcpy(g_rows[i].orig, g_rows[i].value, sizeof(g_rows[i].orig));
    rebuild_list();
    paint_dialog();
}

const char *mf_devset_payload(void)
{
    size_t off = 1;
    int i;

    g_payload[0] = '{';
    g_payload[1] = '\0';
    for (i = 0; i < g_nrows; i++)
    {
        const row_t *r = &g_rows[i];
        char piece[256];
        int n;

        /* Read-only rows and the TUI-only graph interval stay home. */
        if (r->ro || strcmp(r->key, "graph_interval_min") == 0)
            continue;
        if (json_bare(r->value))
            n = snprintf(piece, sizeof(piece), "%s\"%s\":%s",
                         off > 1 ? "," : "", r->key, r->value);
        else
            n = snprintf(piece, sizeof(piece), "%s\"%s\":\"%s\"",
                         off > 1 ? "," : "", r->key, r->value);
        if (n < 0 || (size_t)n >= sizeof(piece))
            continue;   /* value too long to encode safely; skip this field */
        n = snprintf(g_payload + off, sizeof(g_payload) - off, "%s", piece);
        if (n < 0)
            break;
        off += (size_t)n;
        if (off >= sizeof(g_payload) - 2)
            break;
    }
    if (off < sizeof(g_payload) - 1)
    {
        g_payload[off] = '}';
        g_payload[off + 1] = '\0';
    }
    return g_payload;
}

/* Remember the plugin's labels, hints and types from its field list. */
static void load_plugin_labels(const cJSON *arr)
{
    const cJSON *f;

    g_nplab = 0;
    cJSON_ArrayForEach(f, arr)
    {
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(f, "key");
        const cJSON *l = cJSON_GetObjectItemCaseSensitive(f, "label");
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(f, "hint");
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(f, "type");

        if (g_nplab >= MAX_PLAB || !cJSON_IsString(k))
            continue;
        snprintf(g_plab[g_nplab].key, sizeof(g_plab[0].key), "%s", k->valuestring);
        snprintf(g_plab[g_nplab].lab, sizeof(g_plab[0].lab), "%s",
                 cJSON_IsString(l) ? l->valuestring : "");
        snprintf(g_plab[g_nplab].hint, sizeof(g_plab[0].hint), "%s",
                 cJSON_IsString(h) ? h->valuestring : "");
        snprintf(g_plab[g_nplab].type, sizeof(g_plab[0].type), "%s",
                 cJSON_IsString(t) ? t->valuestring : "");
        g_nplab++;
    }
}

static void show_form(const char *id, const char *name, const char *json)
{
    int x, y, w, h;

    /* A settings reply carries the plugin's labels as "_fields". */
    if (!g_add_mode && json)
    {
        cJSON *r = cJSON_Parse(json);
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(r, "_fields");

        if (cJSON_IsArray(f))
            load_plugin_labels(f);
        cJSON_Delete(r);
    }
    snprintf(g_id, sizeof(g_id), "%s", id ? id : "");
    snprintf(g_name, sizeof(g_name), "%s", name ? name : "device");
    mf_tui_devsettings_geom(mf_ui_cols(), mf_ui_rows(), &x, &y, &w, &h);
    build_dialog(w, h);
    build_rows(json);
    g_focus = FOCUS_LIST;
    rebuild_list();
    select_row(0);
    mf_ui_attach(VK_WIDGET(g_win), x, y);
    g_open = 1;
    front_restack();
    paint_dialog();
}

void mf_devset_show(const char *id, const char *name, const char *json)
{
    mf_devset_close();
    show_form(id, name, json);
}

void mf_devset_show_add(const char *kind, const char *driver, const char *json,
                        const char *fields)
{
    char cap[40];
    cJSON *arr = fields ? cJSON_Parse(fields) : NULL;

    mf_devset_close();
    g_add_mode = 1;
    load_plugin_labels(arr);
    cJSON_Delete(arr);
    snprintf(g_add_kind, sizeof(g_add_kind), "%s", kind ? kind : "");
    snprintf(g_add_driver, sizeof(g_add_driver), "%s", driver ? driver : "");
    snprintf(cap, sizeof(cap), "Add %s / %s", g_add_kind, g_add_driver);
    show_form("", cap, json);
}

int mf_devset_is_add(void)
{
    return g_open && g_add_mode;
}

const char *mf_devset_add_kind(void)
{
    return g_add_kind;
}

const char *mf_devset_add_driver(void)
{
    return g_add_driver;
}

/* A daemon or form error: an error popup over the dialog, which stays
 * open so the user can fix the value. */
void mf_devset_set_error(const char *msg)
{
    if (!g_open)
        return;
    g_saving = 0;
    msg_show(" Error ", msg ? msg : "error", NULL, 1);
}

/* The daemon's answer to a settings PUT. */
void mf_devset_put_result(const char *json)
{
    cJSON *root;
    const cJSON *err;
    int i;

    if (!g_open || !g_saving)
        return;
    g_saving = 0;
    root = json ? cJSON_Parse(json) : NULL;
    err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (!root || cJSON_IsString(err))
    {
        char l1[80];

        snprintf(l1, sizeof(l1), "%.70s",
                 cJSON_IsString(err) ? err->valuestring : "no reply");
        cJSON_Delete(root);
        msg_show(" Not Saved ", "The daemon refused the change:", l1, 1);
        return;
    }
    cJSON_Delete(root);
    for (i = 0; i < g_nrows; i++)
        memcpy(g_rows[i].orig, g_rows[i].value, sizeof(g_rows[i].orig));
    msg_show(" Saved ", "Settings saved.", NULL, 0);
}

int mf_devset_saving(void)
{
    return g_open && g_saving;
}

/* A fresh settings reply (e.g. a JK frame that now has its OVP values). */
void mf_devset_apply_json(const char *json)
{
    int cur;
    cJSON *r;
    const cJSON *f;

    if (!g_open || dirty() || g_pop || g_saving)
        return;
    r = json ? cJSON_Parse(json) : NULL;
    f = cJSON_GetObjectItemCaseSensitive(r, "_fields");
    if (cJSON_IsArray(f))
        load_plugin_labels(f);
    cJSON_Delete(r);
    cur = g_list ? vk_listbox_get_curr(g_list) : 0;
    build_rows(json);
    rebuild_list();
    select_row(cur);
    paint_dialog();
}

int mf_devset_key(wint_t c)
{
    int cur;

    if (!g_open)
        return 0;
    if (g_msg)
    {
        if (c == 27 || c == '\n' || c == KEY_ENTER || c == ' ' ||
            c == KEY_EXIT)
            msg_close();
        return 1;
    }
    if (g_pop)
        return pop_key(c);
    if (c == 27 || c == KEY_EXIT || c == KEY_CANCEL)
    {
        on_close();
        return 1;
    }
    if (c == '\t')
    {
        g_focus = (g_focus + 1) % FOCUS_MAX;
        paint_dialog();
        return 1;
    }
#ifdef KEY_BTAB
    if (c == KEY_BTAB)
    {
        g_focus = (g_focus + FOCUS_MAX - 1) % FOCUS_MAX;
        paint_dialog();
        return 1;
    }
#endif
    if (g_focus != FOCUS_LIST)
    {
        if (c == KEY_LEFT || c == KEY_RIGHT)
        {
            int f = g_focus + (c == KEY_LEFT ? -1 : 1);

            if (f >= FOCUS_MODIFY && f <= FOCUS_CLOSE)
                g_focus = f;
            paint_dialog();
            return 1;
        }
        if (c == KEY_UP)
        {
            g_focus = FOCUS_LIST;
            paint_dialog();
            return 1;
        }
        if (c == '\n' || c == KEY_ENTER || c == ' ')
            return activate_focus();
        return 1;
    }
    cur = vk_listbox_get_curr(g_list);
    switch (c)
    {
    case KEY_UP:
        select_row(cur - 1);
        break;
    case KEY_DOWN:
        if (cur + 1 < g_nrows)
            select_row(cur + 1);
        else
            g_focus = FOCUS_MODIFY;     /* off the bottom: to the buttons */
        break;
    case KEY_PPAGE:
        select_row(cur - 10 < 0 ? 0 : cur - 10);
        break;
    case KEY_NPAGE:
        select_row(cur + 10 >= g_nrows ? g_nrows - 1 : cur + 10);
        break;
    case KEY_HOME:
        select_row(0);
        break;
    case KEY_END:
        select_row(g_nrows - 1);
        break;
    case KEY_LEFT:
    case KEY_RIGHT:
        toggle_bool(cur);
        break;
    case '\n':
    case KEY_ENTER:
    case ' ':
        return activate_focus();
    default:
        return 1;
    }
    paint_dialog();
    return 1;
}

void mf_confirm_close(void)
{
    if (!g_cf_open)
        return;
    if (g_cf_l1)
    {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_cf_l1));
        vk_label_destroy(g_cf_l1);
        g_cf_l1 = NULL;
    }
    if (g_cf_l2)
    {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_cf_l2));
        vk_label_destroy(g_cf_l2);
        g_cf_l2 = NULL;
    }
    if (g_cf_win)
    {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_cf_win));
        vk_window_destroy(g_cf_win);
        g_cf_win = NULL;
    }
    g_cf_open = 0;
    mf_ui_refresh();
}

int mf_confirm_open(void)
{
    return g_cf_open;
}
const char *mf_confirm_action(void)
{
    return g_cf_key;
}

void mf_confirm_show(const char *name, const char *key)
{
    const char *k = key ? key : "charge";
    const char *msg;

    if (strcmp(k, "balance") == 0)
        msg = "Turn off balancer?";
    else if (strcmp(k, "discharge") == 0)
        msg = "Turn off discharge MOSFET?";
    else
        msg = "Turn off charge MOSFET?";
    mf_confirm_show_msg(name, msg, k);
}

void mf_confirm_show_msg(const char *name, const char *text, const char *key)
{
    int x, y, w, h;
    char cap[40], msg[64];

    mf_confirm_close();
    snprintf(g_cf_key, sizeof(g_cf_key), "%s", key ? key : "");
    mf_tui_confirm_geom(mf_ui_cols(), mf_ui_rows(), &x, &y, &w, &h);
    snprintf(cap, sizeof(cap), " %s ", name ? name : "device");
    g_cf_win = vk_window_create(w, h);
    vk_window_set_title(g_cf_win, cap);
    vk_window_set_border_style(g_cf_win, VK_BORDER_SINGLE);
    vk_window_set_border_colors(g_cf_win, COLOR_WHITE, COL_MENU);
    vk_window_set_border_attrs(g_cf_win, A_BOLD);
    vk_widget_set_colors(VK_WIDGET(g_cf_win), COL_TEXT, COL_MENU);
    mf_ui_attach(VK_WIDGET(g_cf_win), x, y);
    vk_window_update(g_cf_win);

    g_cf_l1 = vk_label_create(w - 4);
    vk_widget_set_colors(VK_WIDGET(g_cf_l1), COL_TEXT, COL_MENU);
    snprintf(msg, sizeof(msg), "%s", text ? text : "");
    vk_label_set_text(g_cf_l1, msg);
    mf_ui_attach(VK_WIDGET(g_cf_l1), x + 2, y + 2);
    vk_label_update(g_cf_l1);

    g_cf_l2 = vk_label_create(w - 4);
    vk_widget_set_colors(VK_WIDGET(g_cf_l2), COL_TEXT, COL_MENU);
    vk_label_set_text(g_cf_l2, "y / n");
    mf_ui_attach(VK_WIDGET(g_cf_l2), x + 2, y + 4);
    vk_label_update(g_cf_l2);

    g_cf_open = 1;
    mf_ui_front_clear();
    mf_ui_front_push(VK_WIDGET(g_cf_win));
    mf_ui_refresh();
}

int mf_confirm_handle(wint_t c)
{
    if (!g_cf_open)
        return 0;
    if (c == 'y' || c == 'Y')
        return 2;
    if (c == 'n' || c == 'N' || c == 27 || c == KEY_EXIT)
    {
        mf_confirm_close();
        return 1;
    }
    return 1;
}

#define LEFT (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)

int
mf_confirm_mouse(int x, int y, mmask_t bstate)
{
    int win_x, win_y, win_w, win_h;
    int lx, ly;

    if (!g_cf_open || !g_cf_win)
        return 0;

    vk_widget_get_position(VK_WIDGET(g_cf_win), &win_x, &win_y);
    vk_widget_get_metrics(VK_WIDGET(g_cf_win), &win_w, &win_h);
    if (x < win_x || y < win_y || x >= win_x + win_w || y >= win_y + win_h)
        return 0;

    if (bstate & LEFT)
    {
        lx = x - win_x;
        ly = y - win_y;
        if (ly >= 3 && ly <= 5)
        {
            int mid = win_w / 2;
            if (lx < mid)
                return 2;
            mf_confirm_close();
            return 1;
        }
    }
    return 1;
}


/* ---- mouse --------------------------------------------------------- */

static int inside(vk_widget_t *w, int ox, int oy, int x, int y)
{
    int wx, wy, ww, wh;

    if (!w)
        return 0;
    vk_widget_get_position(w, &wx, &wy);
    vk_widget_get_metrics(w, &ww, &wh);
    wx += ox;
    wy += oy;
    return x >= wx && x < wx + ww && y >= wy && y < wy + wh;
}

/* Which of a popup's buttons (x, y) is on, or -1.  A popup is a window
 * whose child (at 1,1 inside the border) stacks the client over the bar. */
static int pop_button_at(vk_popup_t *p, int x, int y)
{
    vk_box_t *bar = p ? vk_popup_get_button_bar(p) : NULL;
    int px, py, bx, by, i, n;

    if (!bar)
        return -1;
    vk_widget_get_position(VK_WIDGET(p), &px, &py);
    vk_widget_get_position(VK_WIDGET(bar), &bx, &by);
    n = vk_popup_get_button_count(p);
    for (i = 0; i < n; i++)
        if (inside(VK_WIDGET(vk_popup_get_button(p, i)),
                   px + 1 + bx, py + 1 + by, x, y))
            return i;
    return -1;
}

int
mf_devset_mouse(int x, int y, mmask_t bstate)
{
    int wx, wy, vx, vy, fx, fy, lx, ly, bx, by, i;

    if (!g_open || !g_win)
        return 0;
    if (!(bstate & LEFT) && !(bstate & (BUTTON4_PRESSED | BUTTON5_PRESSED)))
        return 1;

    /* Popups are modal: only their buttons (and a bool list) take clicks. */
    if (g_msg)
    {
        if ((bstate & LEFT) && pop_button_at(g_msg, x, y) == 0)
            msg_close();
        return 1;
    }
    if (g_pop)
    {
        int b = (bstate & LEFT) ? pop_button_at(g_pop, x, y) : -1;

        if (b >= 0)
        {
            g_pop_focus = 1;
            g_pop_btn = b;
            return pop_key('\n');
        }
        if (g_pop_lb && (bstate & LEFT))
        {
            int px, py, cx, cy, lbx, lby;

            vk_widget_get_position(VK_WIDGET(g_pop), &px, &py);
            vk_widget_get_position(VK_WIDGET(g_pop_client), &cx, &cy);
            vk_widget_get_position(VK_WIDGET(g_pop_lb), &lbx, &lby);
            i = y - (py + 1 + cy + lby) + vk_listbox_get_scroll_pos(g_pop_lb);
            if (inside(VK_WIDGET(g_pop_lb), px + 1 + cx, py + 1 + cy, x, y) &&
                i >= 0 && i < vk_listbox_get_item_count(g_pop_lb))
            {
                vk_listbox_set_curr(g_pop_lb, i);
                g_pop_focus = 0;
                paint_dialog();
            }
        }
        return 1;
    }

    vk_widget_get_position(VK_WIDGET(g_win), &wx, &wy);
    vk_widget_get_position(VK_WIDGET(g_vbox), &vx, &vy);
    vk_widget_get_position(VK_WIDGET(g_frame), &fx, &fy);
    vk_widget_get_position(VK_WIDGET(g_list), &lx, &ly);
    vk_widget_get_position(VK_WIDGET(g_bar), &bx, &by);

    if (bstate & (BUTTON4_PRESSED | BUTTON5_PRESSED))
    {
        int cur = vk_listbox_get_curr(g_list);

        select_row((bstate & BUTTON4_PRESSED) ? (cur > 0 ? cur - 1 : 0)
                                              : (cur + 1 < g_nrows ? cur + 1
                                                                   : cur));
        paint_dialog();
        return 1;
    }

    for (i = 0; i < 3; i++)
    {
        if (inside(VK_WIDGET(g_btn[i]), wx + vx + bx, wy + vy + by, x, y))
        {
            g_focus = FOCUS_MODIFY + i;
            paint_dialog();
            return activate_focus();
        }
    }

    /* A row: the first click selects it, a click on the selected row
     * opens it (as Enter does). */
    {
        int ox = wx + vx + fx + lx, oy = wy + vy + fy + ly;
        int row = y - oy + vk_listbox_get_scroll_pos(g_list);

        if (inside(VK_WIDGET(g_list), wx + vx + fx, wy + vy + fy, x, y) &&
            x < ox + list_text_w() + 2 && row >= 0 && row < g_nrows)
        {
            int was = vk_listbox_get_curr(g_list) == row &&
                      g_focus == FOCUS_LIST;

            g_focus = FOCUS_LIST;
            select_row(row);
            paint_dialog();
            if (was)
                modify_open(row);
        }
    }
    return 1;
}
