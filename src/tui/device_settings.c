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
#define MAX_FIELDS 16
#define LAB_W 22
#define HINT_W 13

static vk_window_t *g_win;
static vk_box_t    *g_vbox, *g_mid, *g_inner, *g_form, *g_bar;
static vk_scroller_t *g_vscroll;
static vk_grid_t   *g_row[MAX_FIELDS];
static vk_grid_t   *g_fields[MAX_FIELDS];
static vk_label_t  *g_lab[MAX_FIELDS];
static vk_label_t  *g_hint[MAX_FIELDS];
static vk_input_t  *g_in[MAX_FIELDS];
static vk_button_t *g_btn_save, *g_btn_exit;
static vk_filler_t *g_fill, *g_form_fill;
static vk_filler_t *g_pad_top, *g_pad_bot, *g_pad_left, *g_pad_right;
static int          g_nfields;
static int          g_nedit;
static int          g_edit[MAX_FIELDS];
static int          g_ro[MAX_FIELDS];
static int          g_focus;
static int          g_open;
static int          g_touched;
static int          g_scroll;
static int          g_form_h;
static int          g_row_h[MAX_FIELDS];
static int          g_shown[MAX_FIELDS];
static char         g_name[32];
static char         g_id[40];
/* Add Module mode: every field editable, no device-only extras. */
static int          g_add_mode;
static char         g_add_kind[16];
static char         g_add_driver[16];
/* Labels and hints the plugin supplied for its Add Module fields. */
#define MAX_PLAB 24
static struct {
    char key[48];
    char lab[32];
    char hint[32];
} g_plab[MAX_PLAB];
static int          g_nplab;
static char         g_keys[MAX_FIELDS][40];
static char         g_payload[2048];

static vk_window_t *g_cf_win;
static vk_label_t  *g_cf_l1, *g_cf_l2;
static int          g_cf_open;
static char         g_cf_key[16];

static void style_menu(vk_widget_t *w)
{
    vk_widget_set_colors(w, COL_TEXT, COL_MENU);
}

static vk_filler_t *mk_pad(int w, int h)
{
    vk_filler_t *f = vk_filler_create();
    uint32_t st = vk_widget_get_state(VK_WIDGET(f));

    vk_widget_set_state(VK_WIDGET(f), st & ~(uint32_t)VK_STATE_EXPAND);
    style_menu(VK_WIDGET(f));
    vk_widget_resize(VK_WIDGET(f), w, h);
    return f;
}

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

static void style_input(vk_input_t *in, int focused, int readonly)
{
    if (!in)
        return;
    if (readonly)
    {
        /* A_DIM + black is a darker gray than COLOR_WHITE on 8-color. */
        vk_widget_set_colors(VK_WIDGET(in), COLOR_BLACK, COL_MENU);
        vk_widget_set_attrs(VK_WIDGET(in), A_DIM);
        vk_input_show_cursor(in, false);
    }
    else if (focused)
    {
        vk_widget_set_colors(VK_WIDGET(in), COLOR_WHITE, COL_MENU);
        vk_widget_set_attrs(VK_WIDGET(in), A_BOLD);
        vk_input_show_cursor(in, true);
    }
    else
    {
        vk_widget_set_colors(VK_WIDGET(in), COL_TEXT, COL_MENU);
        vk_widget_set_attrs(VK_WIDGET(in), A_NORMAL);
        vk_input_show_cursor(in, false);
    }
    vk_widget_set_relief_colors(VK_WIDGET(in), COLOR_WHITE, COLOR_BLACK);
    vk_input_update(in);
}

/* Read-only captions use the same dim black as the value. */
static void style_caption(vk_label_t *lab, int readonly)
{
    if (!lab)
        return;
    if (readonly)
    {
        vk_widget_set_colors(VK_WIDGET(lab), COLOR_BLACK, COL_MENU);
        vk_widget_set_attrs(VK_WIDGET(lab), A_DIM);
    }
    else
    {
        vk_widget_set_colors(VK_WIDGET(lab), COL_TEXT, COL_MENU);
        vk_widget_set_attrs(VK_WIDGET(lab), A_NORMAL);
    }
    vk_label_update(lab);
}

static int on_save_btn(vk_widget_t *w, void *a)
{
    (void)w;
    (void)a;
    return 2;
}

static int on_exit_btn(vk_widget_t *w, void *a)
{
    (void)w;
    (void)a;
    mf_devset_close();
    return 1;
}

static void highlight_buttons(void)
{
    int save_hi = (g_focus == g_nedit);
    int exit_hi = (g_focus == g_nedit + 1);

    if (g_btn_save)
    {
        vk_widget_set_colors(VK_WIDGET(g_btn_save),
                             save_hi ? COLOR_YELLOW : COL_TEXT, COL_MENU);
        vk_widget_set_attrs(VK_WIDGET(g_btn_save), A_BOLD);
        vk_button_release(g_btn_save);
        vk_button_update(g_btn_save);
    }
    if (g_btn_exit)
    {
        vk_widget_set_colors(VK_WIDGET(g_btn_exit),
                             exit_hi ? COLOR_YELLOW : COL_TEXT, COL_MENU);
        vk_widget_set_attrs(VK_WIDGET(g_btn_exit), A_BOLD);
        vk_button_release(g_btn_exit);
        vk_button_update(g_btn_exit);
    }
}

static void set_field_focus(int idx)
{
    int i, fi;

    g_focus = idx;
    fi = (idx >= 0 && idx < g_nedit) ? g_edit[idx] : -1;
    for (i = 0; i < g_nfields; i++)
    {
        if (!g_in[i])
            continue;
        vk_input_show_cursor(g_in[i], !g_ro[i] && i == fi);
    }
}

static vk_button_t *mk_btn(const char *txt, VkWidgetFunc fn)
{
    vk_button_t *b = vk_button_create(txt);

    if (!b)
        return NULL;

    vk_button_set_border_style(b, VK_BORDER_SINGLE);
    vk_widget_set_colors(VK_WIDGET(b), COL_TEXT, COL_MENU);
    vk_widget_set_attrs(VK_WIDGET(b), A_BOLD);
    vk_button_set_pressed_colors(b, COLOR_WHITE, COLOR_BLUE);
    vk_widget_set_relief_colors(VK_WIDGET(b), COLOR_WHITE, COLOR_BLACK);
    vk_button_set_on_press(b, fn, NULL);
    return b;
}

static void box_vacate(vk_box_t *box)
{
    int i, n;

    if (!box)
        return;
    n = vk_box_get_slot_count(box);
    for (i = 0; i < n; i++)
        vk_box_set_widget(box, i, NULL, VK_INHERIT_NONE);
}

static void destroy_form(void)
{
    int i;

    if (g_vscroll)
    {
        if (g_form)
            vk_widget_detach_scroller(VK_WIDGET(g_form), g_vscroll);
        vk_scroller_destroy(g_vscroll);
        g_vscroll = NULL;
    }

    /* Unparent before free: box/grid dtors list_del children still slotted. */
    box_vacate(g_vbox);
    box_vacate(g_mid);
    box_vacate(g_inner);
    box_vacate(g_form);
    box_vacate(g_bar);

    for (i = 0; i < MAX_FIELDS; i++)
    {
        if (g_row[i])
        {
            vk_grid_destroy(g_row[i]);
            g_row[i] = NULL;
        }
        if (g_fields[i])
        {
            vk_grid_destroy(g_fields[i]);
            g_fields[i] = NULL;
        }
        if (g_in[i])
        {
            vk_input_destroy(g_in[i]);
            g_in[i] = NULL;
        }
        if (g_lab[i])
        {
            vk_label_destroy(g_lab[i]);
            g_lab[i] = NULL;
        }
        if (g_hint[i])
        {
            vk_label_destroy(g_hint[i]);
            g_hint[i] = NULL;
        }
        g_keys[i][0] = '\0';
        g_row_h[i] = 0;
        g_shown[i] = 0;
    }
    if (g_btn_save)
    {
        vk_button_destroy(g_btn_save);
        g_btn_save = NULL;
    }
    if (g_btn_exit)
    {
        vk_button_destroy(g_btn_exit);
        g_btn_exit = NULL;
    }
    if (g_fill)
    {
        vk_filler_destroy(g_fill);
        g_fill = NULL;
    }
    if (g_form_fill)
    {
        vk_filler_destroy(g_form_fill);
        g_form_fill = NULL;
    }
    if (g_bar)
    {
        vk_box_destroy(g_bar);
        g_bar = NULL;
    }
    if (g_form)
    {
        vk_box_destroy(g_form);
        g_form = NULL;
    }
    if (g_inner)
    {
        vk_box_destroy(g_inner);
        g_inner = NULL;
    }
    if (g_pad_left)
    {
        vk_filler_destroy(g_pad_left);
        g_pad_left = NULL;
    }
    if (g_pad_right)
    {
        vk_filler_destroy(g_pad_right);
        g_pad_right = NULL;
    }
    if (g_mid)
    {
        vk_box_destroy(g_mid);
        g_mid = NULL;
    }
    if (g_pad_top)
    {
        vk_filler_destroy(g_pad_top);
        g_pad_top = NULL;
    }
    if (g_pad_bot)
    {
        vk_filler_destroy(g_pad_bot);
        g_pad_bot = NULL;
    }
    if (g_vbox)
    {
        vk_box_destroy(g_vbox);
        g_vbox = NULL;
    }
    g_nfields = 0;
    g_nedit = 0;
    g_touched = 0;
    g_scroll = 0;
    g_form_h = 0;
}

static int key_already(const char *key)
{
    int i;

    for (i = 0; i < g_nfields; i++)
    {
        if (strcmp(g_keys[i], key) == 0)
            return 1;
    }
    return 0;
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

static void add_field(const char *key, const char *val, int row_h, int iw)
{
    int i = g_nfields;
    vk_grid_t *row, *fields;
    vk_label_t *lab, *hint;
    vk_input_t *in;
    int in_w, fields_w, ro;
    char pretty[40], hint_txt[16];

    if (i >= MAX_FIELDS || !key || !key[0] || key_already(key))
        return;
    ro = field_readonly(key);
    if (ro)
        row_h = 1;
    fields_w = iw - HINT_W;
    if (fields_w < LAB_W + 8)
        fields_w = LAB_W + 8;
    in_w = fields_w - LAB_W;
    if (in_w < 8)
        in_w = 8;

    /* Inner 2-col grid (label | input) — this is the layout that paints. */
    fields = vk_grid_create(fields_w, row_h, 2, 1);
    if (!fields)
        return;
    vk_grid_set_homogeneous(fields, false);
    vk_grid_set_gap(fields, 0);
    vk_grid_set_col_width(fields, 0, LAB_W);
    vk_grid_set_col_width(fields, 1, in_w);
    vk_grid_set_row_height(fields, 0, row_h);
    style_menu(VK_WIDGET(fields));
    vk_widget_set_expand(VK_WIDGET(fields));

    field_caption(key, pretty, sizeof(pretty), hint_txt, sizeof(hint_txt));
    lab = vk_label_create(LAB_W);
    style_menu(VK_WIDGET(lab));
    vk_label_set_text(lab, pretty);
    vk_label_update(lab);

    in = vk_input_create(in_w);
    if (!in)
    {
        vk_label_destroy(lab);
        vk_grid_destroy(fields);
        return;
    }
    if (row_h < 3)
    {
        /* 1-row SINGLE relief paints text on row 1 (off-canvas). BASIC is [value]. */
        vk_input_set_border_style(in, VK_BUTTON_BASIC);
        vk_widget_resize(VK_WIDGET(in), in_w, 1);
    }
    else
    {
        vk_input_set_border_style(in, VK_BORDER_SINGLE);
        vk_widget_set_relief_colors(VK_WIDGET(in), COLOR_WHITE, COLOR_BLACK);
    }
    vk_input_set_text(in, val ? val : "");
    style_input(in, 0, ro);
    vk_grid_set_widget(fields, 0, 0, VK_WIDGET(lab), VK_INHERIT_NONE);
    vk_grid_set_widget(fields, 1, 0, VK_WIDGET(in), VK_INHERIT_NONE);

    hint = vk_label_create(HINT_W);
    style_menu(VK_WIDGET(hint));
    vk_label_set_text(hint, hint_txt[0] ? hint_txt : "");
    vk_label_update(hint);

    /* Outer 2-col grid: fields | hint. Hint is 1-row so the grid vcenters it. */
    row = vk_grid_create(iw, row_h, 2, 1);
    if (!row)
    {
        vk_label_destroy(hint);
        vk_input_destroy(in);
        vk_label_destroy(lab);
        vk_grid_destroy(fields);
        return;
    }
    vk_grid_set_homogeneous(row, false);
    vk_grid_set_gap(row, 0);
    vk_grid_set_col_width(row, 0, fields_w);
    vk_grid_set_col_expand(row, 0, true);
    vk_grid_set_col_width(row, 1, HINT_W);
    vk_grid_set_row_height(row, 0, row_h);
    style_menu(VK_WIDGET(row));
    vk_grid_set_widget(row, 0, 0, VK_WIDGET(fields), VK_INHERIT_NONE);
    vk_grid_set_widget(row, 1, 0, VK_WIDGET(hint), VK_INHERIT_NONE);

    g_row[i] = row;
    g_fields[i] = fields;
    g_lab[i] = lab;
    g_hint[i] = hint;
    g_in[i] = in;
    g_ro[i] = ro;
    g_row_h[i] = row_h;
    style_caption(lab, ro);
    style_caption(hint, ro);
    snprintf(g_keys[i], sizeof(g_keys[i]), "%s", key);
    if (!ro && g_nedit < MAX_FIELDS)
        g_edit[g_nedit++] = i;
    g_nfields++;
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

/* Volt fields keep two decimals so 3.60 is not shown as 3.6.
 * Balance trigger is a millivolt gap, not a cell voltage. */
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

static void add_json_group(cJSON *root, int want_ro, int row_h, int iw, int n)
{
    cJSON *it;
    char buf[160];

    if (!root || !cJSON_IsObject(root))
        return;
    for (it = root->child; it && g_nfields < n; it = it->next)
    {
        int ro;

        if (!it->string || !it->string[0])
            continue;
        if (cJSON_IsObject(it) || cJSON_IsArray(it))
            continue;
        if (strcmp(it->string, "name") == 0 ||
            strcmp(it->string, "uuid") == 0 ||
            strcmp(it->string, "poll_interval_s") == 0 ||
            strcmp(it->string, "capture_interval_s") == 0)
            continue;
        if (json_key_dup(root, it))
            continue;
        if (skip_form_key(it->string))
            continue;
        ro = field_readonly(it->string);
        if ((want_ro && !ro) || (!want_ro && ro))
            continue;
        json_field_text(it->string, it, buf, sizeof(buf));
        add_field(it->string, buf, row_h, iw);
    }
}

/*
 * vk_box stacks every child and does not scroll. vk_widget_draw also
 * cannot show the bottom of a row whose top sits above the canvas, so
 * the scroll offset stays on a field boundary. vk_scroller draws the
 * bar; apply slots only the rows that fit.
 */
static int form_row_h(int i)
{
    if (i < 0 || i >= g_nfields)
        return 1;
    return g_row_h[i] > 0 ? g_row_h[i] : 1;
}

static int form_scroll_limit(void)
{
    int i;
    int y = 0;
    int content = 0;

    for (i = 0; i < g_nfields; i++)
        content += form_row_h(i);
    if (g_form_h < 1 || content <= g_form_h)
        return 0;
    for (i = 0; i < g_nfields; i++)
    {
        if (content - y <= g_form_h)
            return y;
        y += form_row_h(i);
    }
    return 0;
}

static int snap_back(int y)
{
    int i;
    int top = 0;
    int best = 0;
    int limit = form_scroll_limit();

    if (y > limit)
        y = limit;
    if (y < 0)
        y = 0;
    for (i = 0; i < g_nfields; i++)
    {
        if (top > y)
            break;
        best = top;
        top += form_row_h(i);
    }
    return best;
}

static int snap_forward(int y)
{
    int i;
    int top = 0;
    int limit = form_scroll_limit();

    if (y > limit)
        y = limit;
    if (y < 0)
        y = 0;
    for (i = 0; i < g_nfields; i++)
    {
        if (top >= y)
            return top > limit ? limit : top;
        top += form_row_h(i);
    }
    return limit;
}

static void form_slot_visible(void)
{
    int i;
    int y;
    int slot;
    int used;
    int nslots;
    int limit;

    if (!g_form)
        return;
    limit = form_scroll_limit();
    if (g_scroll > limit)
        g_scroll = limit;
    if (g_scroll < 0)
        g_scroll = 0;
    g_scroll = snap_back(g_scroll);

    nslots = vk_box_get_slot_count(g_form);
    for (i = 0; i < nslots; i++)
        vk_box_set_widget(g_form, i, NULL, VK_INHERIT_NONE);

    y = 0;
    slot = 0;
    used = 0;
    for (i = 0; i < g_nfields; i++)
    {
        int h = form_row_h(i);
        int vis = (y >= g_scroll && y + h <= g_scroll + g_form_h);

        g_shown[i] = vis;
        if (vis && g_row[i] && slot < nslots)
        {
            vk_box_set_widget(g_form, slot, VK_WIDGET(g_row[i]),
                              VK_INHERIT_NONE);
            slot++;
            used += h;
        }
        y += h;
    }
    if (g_form_fill && slot < nslots && used < g_form_h)
    {
        int slack = g_form_h - used;
        int fw = 1;
        int fh = 1;

        vk_widget_get_metrics(VK_WIDGET(g_form), &fw, &fh);
        if (slack < 1)
            slack = 1;
        if (fw < 1)
            fw = 1;
        vk_widget_resize(VK_WIDGET(g_form_fill), fw, slack);
        vk_box_set_widget(g_form, slot, VK_WIDGET(g_form_fill),
                          VK_INHERIT_NONE);
    }
}

static void form_scroll_info(vk_widget_t *child,
                             int *content_h, int *content_w,
                             int *scroll_y, int *scroll_x)
{
    int visible = g_form_h > 0 ? g_form_h : 1;
    int limit = form_scroll_limit();

    (void)child;
    if (content_h)
        *content_h = visible + limit;
    if (content_w)
        *content_w = 0;
    if (scroll_y)
        *scroll_y = g_scroll;
    if (scroll_x)
        *scroll_x = 0;
}

static int form_scroll_apply(vk_widget_t *source, int scroll_y, int scroll_x)
{
    int y;

    (void)scroll_x;
    if (scroll_y > g_scroll)
        y = snap_forward(scroll_y);
    else if (scroll_y < g_scroll)
        y = snap_back(scroll_y);
    else
        return 1;
    if (y == g_scroll)
        return 1;
    g_scroll = y;
    form_slot_visible();
    if (source)
        vk_object_emit(VK_OBJECT(source), VK_EVENT_ON_SCROLL);
    return 0;
}

static int form_nudge(int dy)
{
    if (!g_vscroll || dy == 0)
        return 0;
    return vk_scroller_nudge(g_vscroll, dy, 0) == 0;
}

static void scroll_focus_into_view(void)
{
    int fi;
    int i;
    int top;
    int start;
    int end;
    int y;

    if (!g_vscroll || g_focus < 0 || g_focus >= g_nedit)
        return;
    fi = g_edit[g_focus];
    if (fi < 0 || fi >= g_nfields)
        return;
    start = 0;
    for (i = 0; i < fi; i++)
        start += form_row_h(i);
    end = start + form_row_h(fi);
    if (start >= g_scroll && end <= g_scroll + g_form_h)
        return;
    if (start < g_scroll)
    {
        y = start;
    }
    else
    {
        y = start;
        top = 0;
        for (i = 0; i < g_nfields; i++)
        {
            int h = form_row_h(i);

            if (top > start)
                break;
            if (end <= top + g_form_h)
                y = top;
            top += h;
        }
    }
    if (g_form)
        (void)form_scroll_apply(VK_WIDGET(g_form), y, 0);
}

static void paint_form_scroller(void)
{
    vk_widget_t *host;
    vk_widget_t *sw;
    int h;
    int x;

    if (!g_vscroll || !g_form)
        return;
    host = VK_WIDGET(g_form);
    sw = VK_WIDGET(g_vscroll);
    if (!vk_widget_get_canvas(host))
        return;
    vk_widget_get_metrics(host, &x, &h);
    if (h < 1 || x < 1)
        return;
    x -= 1;
    vk_widget_set_surface(sw, vk_widget_get_canvas(host));
    vk_widget_resize(sw, 1, h);
    vk_widget_move(sw, x, 0);
    if (vk_scroller_update(g_vscroll) > 0)
        vk_widget_draw(sw);
}

/*
 * Leave the scrollbar column and one blank column to its left.
 * Resizing the row already shrinks its expand child (the label|input
 * grid) through the grid's resize handler. Shrinking that grid again
 * clips the input's right border.
 */
static void narrow_rows(int delta)
{
    int i;

    if (delta < 1)
        return;
    for (i = 0; i < g_nfields; i++)
    {
        int w = 0;
        int h = 0;
        int in_w = 0;
        int in_h = 0;

        if (!g_row[i])
            continue;
        vk_widget_get_metrics(VK_WIDGET(g_row[i]), &w, &h);
        if (w - delta < LAB_W + HINT_W + 8)
            continue;
        vk_widget_resize(VK_WIDGET(g_row[i]), w - delta, h);
        if (!g_in[i] || !g_fields[i])
            continue;
        vk_widget_get_metrics(VK_WIDGET(g_in[i]), &in_w, &in_h);
        if (in_w <= delta)
            continue;
        vk_widget_resize(VK_WIDGET(g_in[i]), in_w - delta, in_h);
        vk_grid_set_col_width(g_fields[i], 1, in_w - delta);
    }
}

static void attach_form_scroller(void)
{
    if (form_scroll_limit() <= 0 || !g_form)
        return;
    g_vscroll = vk_scroller_create(VK_SCROLLBAR_VERTICAL);
    if (!g_vscroll)
        return;
    vk_scroller_set_border_style(g_vscroll, VK_BORDER_SINGLE);
    vk_scroller_set_border_colors(g_vscroll, COL_TEXT, COL_MENU);
    vk_scroller_set_scroll_source(g_vscroll, VK_WIDGET(g_form));
    vk_scroller_set_scroll_info(g_vscroll, form_scroll_info);
    vk_scroller_set_scroll_apply(g_vscroll, form_scroll_apply);
    vk_widget_attach_scroller(VK_WIDGET(g_form), g_vscroll);
}

static void build_form(int iw, int ih, const char *json)
{
    cJSON *root = NULL, *it;
    int n = 0, nedit = 1, nro = 1, row_h;
    char buf[160];
    int form_h;

    destroy_form();
    if (json && json[0])
        root = cJSON_Parse(json);
    if (root && cJSON_IsObject(root))
    {
        nedit = 0;
        nro = 0;
        for (it = root->child; it; it = it->next)
        {
            cJSON *prev;
            int dup = 0;

            if (!it->string || !it->string[0])
                continue;
            if (cJSON_IsObject(it) || cJSON_IsArray(it))
                continue;
            if (strcmp(it->string, "name") == 0 ||
                strcmp(it->string, "uuid") == 0)
                continue;
            for (prev = root->child; prev != it; prev = prev->next)
            {
                if (prev->string && strcmp(prev->string, it->string) == 0)
                    dup = 1;
            }
            if (dup)
                continue;
            if (skip_form_key(it->string))
                continue;
            if (field_readonly(it->string))
                nro++;
            else
                nedit++;
        }
        nedit++; /* name */
        if (!g_add_mode)
            nro++;   /* uuid */
    }
    n = nedit + nro;
    if (n < 2)
        n = 2;
    if (n > MAX_FIELDS)
        n = MAX_FIELDS;
    /* Top pad, one blank row above the buttons, and the 3-row button bar. */
    form_h = ih - 5;
    if (form_h < 1)
        form_h = 1;
    /* Editable knobs stay 3-row sunken whenever they fit; identity is 1-row. */
    row_h = (nedit * 3 <= form_h) ? 3 : 1;

    g_form = vk_box_create(iw - 2, form_h, VK_BOX_VERTICAL, MAX_FIELDS + 1);
    vk_box_set_homogeneous(g_form, false);
    style_menu(VK_WIDGET(g_form));
    vk_widget_set_expand(VK_WIDGET(g_form));

    buf[0] = '\0';
    if (root)
        json_scalar(cJSON_GetObjectItemCaseSensitive(root, "name"),
                    buf, sizeof(buf));
    add_field("name", buf[0] || g_add_mode ? buf : g_name, row_h, iw - 2);

    buf[0] = '\0';
    if (root)
        json_scalar(cJSON_GetObjectItemCaseSensitive(root, "poll_interval_s"),
                    buf, sizeof(buf));
    add_field("poll_interval_s", buf[0] ? buf : "2.0", row_h, iw - 2);

    buf[0] = '\0';
    if (root)
        json_scalar(cJSON_GetObjectItemCaseSensitive(root, "capture_interval_s"),
                    buf, sizeof(buf));
    add_field("capture_interval_s", buf[0] ? buf : "10", row_h, iw - 2);

    if (!g_add_mode)
        add_field("graph_interval_min", "30", row_h, iw - 2);

    add_json_group(root, 0, row_h, iw - 2, MAX_FIELDS);

    buf[0] = '\0';
    if (root)
        json_field_text("cell_rcv_v",
                        cJSON_GetObjectItemCaseSensitive(root, "cell_rcv_v"),
                        buf, sizeof(buf));
    if (buf[0])
        add_field("cell_rcv_v", buf, 1, iw - 2);

    if (root && !g_add_mode &&
        (cJSON_GetObjectItemCaseSensitive(root, "cell_ovp_v") ||
                 cJSON_GetObjectItemCaseSensitive(root, "ble.address") ||
                 cJSON_GetObjectItemCaseSensitive(root, "balance_trigger_v") ||
                 cJSON_GetObjectItemCaseSensitive(root, "start_balance_v")))
    {
        buf[0] = '\0';
        json_field_text("balance_trigger_v",
                        cJSON_GetObjectItemCaseSensitive(root, "balance_trigger_v"),
                        buf, sizeof(buf));
        add_field("balance_trigger_v", buf[0] ? buf : "--", 1, iw - 2);
        buf[0] = '\0';
        json_field_text("start_balance_v",
                        cJSON_GetObjectItemCaseSensitive(root, "start_balance_v"),
                        buf, sizeof(buf));
        add_field("start_balance_v", buf[0] ? buf : "--", 1, iw - 2);
    }

    add_json_group(root, 1, row_h, iw - 2, MAX_FIELDS);

    buf[0] = '\0';
    if (root)
        json_scalar(cJSON_GetObjectItemCaseSensitive(root, "uuid"),
                    buf, sizeof(buf));
    if (!g_add_mode)
        add_field("uuid", buf[0] ? buf : g_id, 1, iw - 2);

    if (root)
        cJSON_Delete(root);

    g_form_h = form_h;
    g_scroll = 0;
    if (form_scroll_limit() > 0)
        narrow_rows(2);
    g_form_fill = vk_filler_create();
    if (g_form_fill)
    {
        uint32_t st = vk_widget_get_state(VK_WIDGET(g_form_fill));

        vk_widget_set_state(VK_WIDGET(g_form_fill),
                            st & ~(uint32_t)VK_STATE_EXPAND);
        style_menu(VK_WIDGET(g_form_fill));
        vk_widget_resize(VK_WIDGET(g_form_fill), iw - 2, 1);
    }
    form_slot_visible();
    attach_form_scroller();

    g_bar = vk_box_create(iw - 2, 3, VK_BOX_HORIZONTAL, 3);
    vk_box_set_homogeneous(g_bar, false);
    style_menu(VK_WIDGET(g_bar));
    g_btn_save = mk_btn(g_add_mode ? "Add" : "Save", on_save_btn);
    g_btn_exit = mk_btn(g_add_mode ? "Cancel" : "Exit", on_exit_btn);
    g_fill = vk_filler_create();
    style_menu(VK_WIDGET(g_fill));
    vk_widget_set_expand(VK_WIDGET(g_fill));
    vk_box_set_widget(g_bar, 0, VK_WIDGET(g_btn_save), VK_INHERIT_NONE);
    vk_box_set_widget(g_bar, 1, VK_WIDGET(g_fill), VK_INHERIT_NONE);
    vk_box_set_widget(g_bar, 2, VK_WIDGET(g_btn_exit), VK_INHERIT_NONE);

    g_pad_bot = mk_pad(iw - 2, 1);
    g_inner = vk_box_create(iw - 2, ih - 1, VK_BOX_VERTICAL, 3);
    vk_box_set_homogeneous(g_inner, false);
    style_menu(VK_WIDGET(g_inner));
    vk_widget_set_expand(VK_WIDGET(g_inner));
    vk_box_set_widget(g_inner, 0, VK_WIDGET(g_form), VK_INHERIT_NONE);
    vk_box_set_widget(g_inner, 1, VK_WIDGET(g_pad_bot), VK_INHERIT_NONE);
    vk_box_set_widget(g_inner, 2, VK_WIDGET(g_bar), VK_INHERIT_NONE);

    g_pad_left = mk_pad(1, ih - 1);
    g_pad_right = mk_pad(1, ih - 1);
    g_mid = vk_box_create(iw, ih - 1, VK_BOX_HORIZONTAL, 3);
    vk_box_set_homogeneous(g_mid, false);
    style_menu(VK_WIDGET(g_mid));
    vk_widget_set_expand(VK_WIDGET(g_mid));
    vk_box_set_widget(g_mid, 0, VK_WIDGET(g_pad_left), VK_INHERIT_NONE);
    vk_box_set_widget(g_mid, 1, VK_WIDGET(g_inner), VK_INHERIT_NONE);
    vk_box_set_widget(g_mid, 2, VK_WIDGET(g_pad_right), VK_INHERIT_NONE);

    g_pad_top = mk_pad(iw, 1);
    g_vbox = vk_box_create(iw, ih, VK_BOX_VERTICAL, 2);
    vk_box_set_homogeneous(g_vbox, false);
    style_menu(VK_WIDGET(g_vbox));
    vk_widget_set_expand(VK_WIDGET(g_vbox));
    vk_box_set_widget(g_vbox, 0, VK_WIDGET(g_pad_top), VK_INHERIT_NONE);
    vk_box_set_widget(g_vbox, 1, VK_WIDGET(g_mid), VK_INHERIT_NONE);
}

void mf_devset_close(void)
{
    if (!g_open)
        return;
    if (g_win)
        vk_window_set_child(g_win, NULL, VK_INHERIT_NONE);
    destroy_form();
    if (g_win)
    {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_win));
        vk_window_destroy(g_win);
        g_win = NULL;
    }
    g_open = 0;
    g_add_mode = 0;
    g_nplab = 0;
    mf_ui_front_clear();
    mf_ui_refresh();
}

int mf_devset_open(void)
{
    return g_open;
}
int mf_devset_touched(void)
{
    return g_touched;
}

int mf_devset_has_key(const char *key)
{
    int i;

    if (!key || !key[0])
        return 0;
    for (i = 0; i < g_nfields; i++)
    {
        if (strcmp(g_keys[i], key) == 0)
            return 1;
    }
    return 0;
}

const char *mf_devset_id(void)
{
    return g_id;
}

const char *mf_devset_poll_text(void)
{
    int i;

    for (i = 0; i < g_nfields; i++)
    {
        if (strcmp(g_keys[i], "poll_interval_s") == 0 && g_in[i])
            return vk_input_get_text(g_in[i]);
    }
    return g_nfields && g_in[0] ? vk_input_get_text(g_in[0]) : "2.0";
}

int mf_devset_get_graph_interval(void)
{
    int i;
    for (i = 0; i < g_nfields; i++)
    {
        if (strcmp(g_keys[i], "graph_interval_min") == 0 && g_in[i])
        {
            const char *v = vk_input_get_text(g_in[i]);
            int val = atoi(v);
            if (val < 1)
                val = 1;
            return val;
        }
    }
    return 30;
}

void mf_devset_set_graph_interval(int minutes)
{
    char buf[16];
    int i;
    if (minutes < 1)
        minutes = 1;
    snprintf(buf, sizeof(buf), "%d", minutes);
    for (i = 0; i < g_nfields; i++)
    {
        if (strcmp(g_keys[i], "graph_interval_min") == 0 && g_in[i])
        {
            vk_input_set_text(g_in[i], buf);
            vk_input_update(g_in[i]);
            return;
        }
    }
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

const char *mf_devset_payload(void)
{
    size_t off = 0;
    int i;

    g_payload[0] = '{';
    g_payload[1] = '\0';
    off = 1;
    for (i = 0; i < g_nfields; i++)
    {
        const char *v = g_in[i] ? vk_input_get_text(g_in[i]) : "";
        char piece[256];
        int n;

        if (!g_keys[i][0] || g_ro[i])
            continue;
        /* TUI-only key: never send to daemon/plugin. */
        if (strcmp(g_keys[i], "graph_interval_min") == 0)
            continue;
        if (!v)
            v = "";
        if (json_bare(v))
            n = snprintf(piece, sizeof(piece), "%s\"%s\":%s",
                         off > 1 ? "," : "", g_keys[i], v);
        else
            n = snprintf(piece, sizeof(piece), "%s\"%s\":\"%s\"",
                         off > 1 ? "," : "", g_keys[i], v);
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

static void paint_dialog(void)
{
    int pass, i;

    /*
     * Nested box/grid update blits children and may resize expand
     * widgets (wiping their canvases). Two passes: layout, then paint
     * onto the final sizes.
     */
    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < g_nfields; i++)
        {
            style_caption(g_lab[i], g_ro[i]);
            style_caption(g_hint[i], g_ro[i]);
            style_input(g_in[i],
                        (g_focus >= 0 && g_focus < g_nedit &&
                         g_edit[g_focus] == i),
                        g_ro[i]);
            if (g_fields[i])
                vk_grid_update(g_fields[i]);
            if (g_row[i])
                vk_grid_update(g_row[i]);
        }
        highlight_buttons();
        if (g_form)
            vk_box_update(g_form);
        paint_form_scroller();
        if (g_bar)
            vk_box_update(g_bar);
        if (g_inner)
            vk_box_update(g_inner);
        if (g_mid)
            vk_box_update(g_mid);
        if (g_vbox)
            vk_box_update(g_vbox);
    }
    if (g_win)
        vk_window_update(g_win);
    mf_ui_refresh();
}

static void show_form(const char *id, const char *name, const char *json);

void mf_devset_show(const char *id, const char *name, const char *json)
{
    mf_devset_close();
    show_form(id, name, json);
}

/* Remember the plugin's labels and hints from its field list. */
static void load_plugin_labels(const cJSON *arr)
{
    const cJSON *f;

    g_nplab = 0;
    cJSON_ArrayForEach(f, arr)
    {
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(f, "key");
        const cJSON *l = cJSON_GetObjectItemCaseSensitive(f, "label");
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(f, "hint");

        if (g_nplab >= MAX_PLAB || !cJSON_IsString(k))
            continue;
        snprintf(g_plab[g_nplab].key, sizeof(g_plab[0].key), "%s", k->valuestring);
        snprintf(g_plab[g_nplab].lab, sizeof(g_plab[0].lab), "%s",
                 cJSON_IsString(l) ? l->valuestring : "");
        snprintf(g_plab[g_nplab].hint, sizeof(g_plab[0].hint), "%s",
                 cJSON_IsString(h) ? h->valuestring : "");
        g_nplab++;
    }
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

/* Show a daemon error in the title bar; the form stays open to fix it. */
void mf_devset_set_error(const char *msg)
{
    char cap[72];

    if (!g_open || !g_win)
        return;
    snprintf(cap, sizeof(cap), " %.66s ", msg ? msg : "error");
    vk_window_set_title(g_win, cap);
    paint_dialog();
    mf_ui_refresh();
}

static void show_form(const char *id, const char *name, const char *json)
{
    int x, y, w, h;
    char cap[40];

    /* A settings reply carries the plugin's labels as "_fields". */
    if (!g_add_mode && json)
    {
        cJSON *r = cJSON_Parse(json);

        load_plugin_labels(cJSON_GetObjectItemCaseSensitive(r, "_fields"));
        cJSON_Delete(r);
    }

    snprintf(g_id, sizeof(g_id), "%s", id ? id : "");
    snprintf(g_name, sizeof(g_name), "%s", name ? name : "device");
    mf_tui_devsettings_geom(mf_ui_cols(), mf_ui_rows(), &x, &y, &w, &h);
    snprintf(cap, sizeof(cap), " %s ", g_name);
    g_win = vk_window_create(w, h);
    vk_window_set_title(g_win, cap);
    vk_window_set_border_style(g_win, VK_BORDER_SINGLE);
    vk_window_set_border_colors(g_win, COLOR_WHITE, COL_MENU);
    vk_window_set_border_attrs(g_win, A_BOLD);
    vk_widget_set_colors(VK_WIDGET(g_win), COL_TEXT, COL_MENU);
    vk_widget_set_attrs(VK_WIDGET(g_win), A_BOLD);

    build_form(w - 2, h - 2, json);
    vk_window_set_child(g_win, VK_WIDGET(g_vbox), VK_INHERIT_NONE);
    mf_ui_attach(VK_WIDGET(g_win), x, y);
    set_field_focus(0);
    paint_dialog();

    g_open = 1;
    mf_ui_front_clear();
    mf_ui_front_push(VK_WIDGET(g_win));
    mf_ui_refresh();
}

void mf_devset_apply_json(const char *json)
{
    char id[40], name[32];

    if (!g_open)
        return;
    snprintf(id, sizeof(id), "%s", g_id);
    snprintf(name, sizeof(name), "%s", g_name);
    mf_devset_show(id, name, json);
}

int mf_devset_key(wint_t c)
{
    vk_input_t *in;
    int nbtn;

    if (!g_open)
        return 0;
    nbtn = g_nedit + 2;
    if (c == 27 || c == KEY_EXIT || c == KEY_CANCEL)
    {
        mf_devset_close();
        return 1;
    }
    if (c == '\t')
    {
        set_field_focus((g_focus + 1) % nbtn);
        scroll_focus_into_view();
        paint_dialog();
        return 1;
    }
#ifdef KEY_BTAB
    if (c == KEY_BTAB)
    {
        set_field_focus((g_focus + nbtn - 1) % nbtn);
        scroll_focus_into_view();
        paint_dialog();
        return 1;
    }
#endif
    /* Up/Down step through the fields and buttons (no wrap); the form
       scrolls to keep the focus in view.  PgUp/PgDn scroll the form, which
       still reaches the read-only rows below the last field. */
    if (c == KEY_UP || c == KEY_DOWN)
    {
        int f = g_focus + (c == KEY_UP ? -1 : 1);

        if (f >= 0 && f < nbtn)
        {
            set_field_focus(f);
            scroll_focus_into_view();
            paint_dialog();
        }
        return 1;
    }
    if (c == KEY_PPAGE || c == KEY_NPAGE)
    {
        int dy = c == KEY_PPAGE ? (g_form_h > 0 ? -g_form_h : -1)
                                : (g_form_h > 0 ? g_form_h : 1);

        if (form_nudge(dy))
            paint_dialog();
        return 1;
    }
    /* Left/Right on a button: switch between the two buttons. */
    if ((c == KEY_LEFT || c == KEY_RIGHT) && g_focus >= g_nedit)
    {
        set_field_focus(c == KEY_LEFT ? g_nedit : g_nedit + 1);
        paint_dialog();
        return 1;
    }
    if (c == '\n' || c == KEY_ENTER)
    {
        if (g_focus == g_nedit + 1)
        {
            mf_devset_close();
            return 1;
        }
        return 2;
    }
    if (g_focus >= g_nedit)
        return 1;
    in = g_in[g_edit[g_focus]];
    if (!in)
        return 1;
    if (c == KEY_BACKSPACE || c == 127)
    {
        g_touched = 1;
        vk_input_backspace(in);
        vk_input_update(in);
        scroll_focus_into_view();
        paint_dialog();
        return 1;
    }
    if (c == KEY_LEFT)
    {
        vk_input_move_cursor(in, -1);
        vk_input_update(in);
        scroll_focus_into_view();
        paint_dialog();
        return 1;
    }
    if (c == KEY_RIGHT)
    {
        vk_input_move_cursor(in, 1);
        vk_input_update(in);
        scroll_focus_into_view();
        paint_dialog();
        return 1;
    }
    if (c >= 32 && c < 127)
    {
        g_touched = 1;
        vk_input_insert_char(in, (int)c);
        vk_input_update(in);
        scroll_focus_into_view();
        paint_dialog();
        return 1;
    }
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

static int hit_btn(vk_button_t *btn, int ox, int oy, int x, int y)
{
    int bx, by, bw, bh;

    if (!btn)
        return 0;
    vk_widget_get_position(VK_WIDGET(btn), &bx, &by);
    vk_widget_get_metrics(VK_WIDGET(btn), &bw, &bh);
    bx += ox;
    by += oy;
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

int
mf_devset_mouse(int x, int y, mmask_t bstate)
{
    int win_x, win_y, win_w, win_h;
    int ox, oy, bx, by;
    int i, lx, ly, cx, cy;

    if (!g_open || !g_win)
        return 0;

    vk_widget_get_position(VK_WIDGET(g_win), &win_x, &win_y);
    vk_widget_get_metrics(VK_WIDGET(g_win), &win_w, &win_h);
    if (x < win_x || y < win_y || x >= win_x + win_w || y >= win_y + win_h)
        return 1;

    if (bstate & (BUTTON4_PRESSED | BUTTON5_PRESSED))
    {
        if (form_nudge((bstate & BUTTON4_PRESSED) ? -1 : 1))
            paint_dialog();
        return 1;
    }

    if (g_vscroll && (bstate & LEFT))
    {
        int bar_y = y - win_y;

        /* Bar sits on the form's last column: border + left pad + form. */
        if (x - win_x == win_w - 3 && bar_y >= 2 && bar_y < 2 + g_form_h)
        {
            int dy;

            if (bar_y == 2)
                dy = -1;
            else if (bar_y == 2 + g_form_h - 1)
                dy = 1;
            else if (bar_y < 2 + g_form_h / 2)
                dy = g_form_h > 0 ? -g_form_h : -1;
            else
                dy = g_form_h > 0 ? g_form_h : 1;
            if (form_nudge(dy))
                paint_dialog();
            return 1;
        }
    }

    if (!(bstate & LEFT) || !g_vbox || !g_bar)
        return 1;

    /*
     * Buttons first. On a tall form (more field rows than fit -- e.g. a JK
     * pack) the field rows overflow down into the button bar's row; testing
     * field rows first would swallow the Save/Exit clicks before they reach
     * the buttons. The bar sits at vbox -> mid -> inner -> bar, so sum that
     * whole chain: skipping mid/inner (the 1-char pads) left the hit box off
     * by (1,1).
     */
    {
        int vx, vy, mx, my, ix, iy;

        vk_widget_get_position(VK_WIDGET(g_vbox), &vx, &vy);
        vk_widget_get_position(VK_WIDGET(g_mid), &mx, &my);
        vk_widget_get_position(VK_WIDGET(g_inner), &ix, &iy);
        vk_widget_get_position(VK_WIDGET(g_bar), &bx, &by);
        ox = win_x + vx + mx + ix + bx;
        oy = win_y + vy + my + iy + by;
    }
    if (hit_btn(g_btn_save, ox, oy, x, y))
    {
        int rc = vk_button_press(g_btn_save);
        vk_button_update(g_btn_save);
        paint_dialog();
        return rc == 2 ? 2 : 1;
    }
    if (hit_btn(g_btn_exit, ox, oy, x, y))
    {
        vk_button_press(g_btn_exit);
        return 1;
    }

    /* Frame + 1-char pad; form rows stack from there. */
    lx = x - win_x;
    ly = y - win_y;
    cx = 2;
    cy = 2;
    for (i = 0; i < g_nfields; i++)
    {
        int rh;

        if (!g_shown[i] || !g_row[i])
            continue;
        rh = form_row_h(i);
        if (lx >= cx && lx < win_w - (g_vscroll ? 4 : 2) &&
            ly >= cy && ly < cy + rh)
        {
            if (!g_ro[i])
            {
                int e;

                for (e = 0; e < g_nedit; e++)
                {
                    if (g_edit[e] == i)
                    {
                        set_field_focus(e);
                        paint_dialog();
                        break;
                    }
                }
            }
            return 1;
        }
        cy += rh;
    }
    return 1;
}
