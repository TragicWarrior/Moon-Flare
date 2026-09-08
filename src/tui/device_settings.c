#include "ui_screen.h"
#include "layout.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vdk.h>
#include <ncursesw/curses.h>

#define COL_BG   COLOR_WHITE
#define COL_TEXT COLOR_BLACK
#define COL_MENU COLOR_CYAN
#define MAX_FIELDS 8
#define LAB_W 22

static vk_window_t *g_win;
static vk_box_t    *g_vbox, *g_form, *g_bar;
static vk_box_t    *g_row[MAX_FIELDS];
static vk_label_t  *g_lab[MAX_FIELDS];
static vk_input_t  *g_in[MAX_FIELDS];
static vk_button_t *g_btn_save, *g_btn_exit;
static vk_filler_t *g_fill, *g_form_fill;
static int          g_nfields;
static int          g_focus;
static int          g_open;
static char         g_name[32];
static char         g_id[40];
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
    int save_hi = (g_focus == g_nfields);
    int exit_hi = (g_focus == g_nfields + 1);

    if (g_btn_save) {
        vk_widget_set_colors(VK_WIDGET(g_btn_save),
                             save_hi ? COLOR_YELLOW : COL_TEXT, COL_MENU);
        vk_widget_set_attrs(VK_WIDGET(g_btn_save), A_BOLD);
        vk_button_release(g_btn_save);
        vk_button_update(g_btn_save);
    }
    if (g_btn_exit) {
        vk_widget_set_colors(VK_WIDGET(g_btn_exit),
                             exit_hi ? COLOR_YELLOW : COL_TEXT, COL_MENU);
        vk_widget_set_attrs(VK_WIDGET(g_btn_exit), A_BOLD);
        vk_button_release(g_btn_exit);
        vk_button_update(g_btn_exit);
    }
}

static void set_field_focus(int idx)
{
    int i;

    g_focus = idx;
    for (i = 0; i < g_nfields; i++) {
        if (!g_in[i])
            continue;
        vk_input_show_cursor(g_in[i], i == idx);
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

static void destroy_form(void)
{
    int i;

    for (i = 0; i < MAX_FIELDS; i++) {
        if (g_in[i]) {
            vk_input_destroy(g_in[i]);
            g_in[i] = NULL;
        }
        if (g_lab[i]) {
            vk_label_destroy(g_lab[i]);
            g_lab[i] = NULL;
        }
        if (g_row[i]) {
            vk_box_destroy(g_row[i]);
            g_row[i] = NULL;
        }
        g_keys[i][0] = '\0';
    }
    if (g_btn_save) {
        vk_button_destroy(g_btn_save);
        g_btn_save = NULL;
    }
    if (g_btn_exit) {
        vk_button_destroy(g_btn_exit);
        g_btn_exit = NULL;
    }
    if (g_fill) {
        vk_filler_destroy(g_fill);
        g_fill = NULL;
    }
    if (g_form_fill) {
        vk_filler_destroy(g_form_fill);
        g_form_fill = NULL;
    }
    if (g_bar) {
        vk_box_destroy(g_bar);
        g_bar = NULL;
    }
    if (g_form) {
        vk_box_destroy(g_form);
        g_form = NULL;
    }
    if (g_vbox) {
        vk_box_destroy(g_vbox);
        g_vbox = NULL;
    }
    g_nfields = 0;
}

static void add_field(const char *key, const char *val, int row_h, int iw)
{
    int i = g_nfields;
    vk_box_t *row;
    vk_label_t *lab;
    vk_input_t *in;
    int in_w;

    if (i >= MAX_FIELDS || !key || !key[0])
        return;
    in_w = iw - LAB_W;
    if (in_w < 8)
        in_w = 8;
    row = vk_box_create(iw, row_h, VK_BOX_HORIZONTAL, 2);
    if (!row)
        return;
    vk_box_set_homogeneous(row, false);
    style_menu(VK_WIDGET(row));

    lab = vk_label_create(LAB_W);
    if (row_h > 1)
        vk_widget_resize(VK_WIDGET(lab), LAB_W, row_h);
    style_menu(VK_WIDGET(lab));
    vk_label_set_text(lab, key);
    vk_label_update(lab);

    in = vk_input_create(in_w);
    if (row_h < 3)
        vk_widget_resize(VK_WIDGET(in), in_w, 1);
    else {
        vk_input_set_border_style(in, VK_BORDER_SINGLE);
        vk_widget_set_relief_colors(VK_WIDGET(in), COLOR_WHITE, COLOR_BLACK);
    }
    vk_widget_set_colors(VK_WIDGET(in), COL_TEXT, COL_BG);
    vk_input_set_text(in, val ? val : "");
    vk_input_show_cursor(in, false);
    vk_input_update(in);
    vk_widget_set_expand(VK_WIDGET(in));

    vk_box_set_widget(row, 0, VK_WIDGET(lab));
    vk_box_set_widget(row, 1, VK_WIDGET(in));

    g_row[i] = row;
    g_lab[i] = lab;
    g_in[i] = in;
    snprintf(g_keys[i], sizeof(g_keys[i]), "%s", key);
    g_nfields++;
}

static void build_form(int iw, int ih, const char *json)
{
    cJSON *root, *it;
    int n = 0, row_h, i;
    char def[32];

    destroy_form();
    if (json && json[0]) {
        root = cJSON_Parse(json);
        if (root && cJSON_IsObject(root)) {
            for (it = root->child; it && n < MAX_FIELDS; it = it->next) {
                if (!it->string || !it->string[0])
                    continue;
                if (cJSON_IsObject(it) || cJSON_IsArray(it))
                    continue;
                n++;
            }
        }
        if (root)
            cJSON_Delete(root);
    }
    if (n < 1)
        n = 1;
    row_h = (n * 3 + 3 <= ih) ? 3 : 1;

    g_form = vk_box_create(iw, ih - 3, VK_BOX_VERTICAL, n + 1);
    vk_box_set_homogeneous(g_form, false);
    style_menu(VK_WIDGET(g_form));
    vk_widget_set_expand(VK_WIDGET(g_form));

    if (json && json[0]) {
        root = cJSON_Parse(json);
        if (root && cJSON_IsObject(root)) {
            for (it = root->child; it && g_nfields < n; it = it->next) {
                char buf[128];

                if (!it->string || !it->string[0])
                    continue;
                if (cJSON_IsObject(it) || cJSON_IsArray(it))
                    continue;
                if (cJSON_IsString(it) && it->valuestring)
                    snprintf(buf, sizeof(buf), "%s", it->valuestring);
                else if (cJSON_IsNumber(it))
                    snprintf(buf, sizeof(buf), "%g", it->valuedouble);
                else if (cJSON_IsBool(it))
                    snprintf(buf, sizeof(buf), "%s",
                             cJSON_IsTrue(it) ? "true" : "false");
                else
                    buf[0] = '\0';
                add_field(it->string, buf, row_h, iw);
            }
        }
        if (root)
            cJSON_Delete(root);
    }
    if (g_nfields < 1) {
        snprintf(def, sizeof(def), "2.0");
        add_field("poll_interval_s", def, row_h, iw);
    }
    for (i = 0; i < g_nfields; i++)
        vk_box_set_widget(g_form, i, VK_WIDGET(g_row[i]));
    g_form_fill = vk_filler_create();
    style_menu(VK_WIDGET(g_form_fill));
    vk_widget_set_expand(VK_WIDGET(g_form_fill));
    vk_box_set_widget(g_form, g_nfields, VK_WIDGET(g_form_fill));

    g_bar = vk_box_create(iw, 3, VK_BOX_HORIZONTAL, 3);
    vk_box_set_homogeneous(g_bar, false);
    style_menu(VK_WIDGET(g_bar));
    g_btn_save = mk_btn("Save", on_save_btn);
    g_btn_exit = mk_btn("Exit", on_exit_btn);
    g_fill = vk_filler_create();
    style_menu(VK_WIDGET(g_fill));
    vk_widget_set_expand(VK_WIDGET(g_fill));
    vk_box_set_widget(g_bar, 0, VK_WIDGET(g_btn_save));
    vk_box_set_widget(g_bar, 1, VK_WIDGET(g_fill));
    vk_box_set_widget(g_bar, 2, VK_WIDGET(g_btn_exit));

    g_vbox = vk_box_create(iw, ih, VK_BOX_VERTICAL, 2);
    vk_box_set_homogeneous(g_vbox, false);
    style_menu(VK_WIDGET(g_vbox));
    vk_widget_set_expand(VK_WIDGET(g_vbox));
    vk_box_set_widget(g_vbox, 0, VK_WIDGET(g_form));
    vk_box_set_widget(g_vbox, 1, VK_WIDGET(g_bar));
}

void mf_devset_close(void)
{
    if (!g_open)
        return;
    if (g_win)
        vk_window_set_child(g_win, NULL);
    destroy_form();
    if (g_win) {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_win));
        vk_window_destroy(g_win);
        g_win = NULL;
    }
    g_open = 0;
    mf_ui_front_clear();
    mf_ui_refresh();
}

int mf_devset_open(void) { return g_open; }
const char *mf_devset_id(void) { return g_id; }

const char *mf_devset_poll_text(void)
{
    int i;

    for (i = 0; i < g_nfields; i++) {
        if (strcmp(g_keys[i], "poll_interval_s") == 0 && g_in[i])
            return vk_input_get_text(g_in[i]);
    }
    return g_nfields && g_in[0] ? vk_input_get_text(g_in[0]) : "2.0";
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
    for (i = 0; i < g_nfields; i++) {
        const char *v = g_in[i] ? vk_input_get_text(g_in[i]) : "";
        char piece[256];
        int n;

        if (!g_keys[i][0])
            continue;
        if (!v)
            v = "";
        if (json_bare(v))
            snprintf(piece, sizeof(piece), "%s\"%s\":%s",
                     off > 1 ? "," : "", g_keys[i], v);
        else
            snprintf(piece, sizeof(piece), "%s\"%s\":\"%s\"",
                     off > 1 ? "," : "", g_keys[i], v);
        n = snprintf(g_payload + off, sizeof(g_payload) - off, "%s", piece);
        if (n < 0)
            break;
        off += (size_t)n;
        if (off >= sizeof(g_payload) - 2)
            break;
    }
    if (off < sizeof(g_payload) - 1) {
        g_payload[off] = '}';
        g_payload[off + 1] = '\0';
    }
    return g_payload;
}

static void paint_dialog(void)
{
    int i;
    WINDOW *c;

    /* Nested boxes blit children; leaves must be painted first. */
    for (i = 0; i < g_nfields; i++) {
        if (g_in[i])
            vk_input_update(g_in[i]);
        if (g_lab[i])
            vk_label_update(g_lab[i]);
        if (g_row[i])
            vk_box_update(g_row[i]);
    }
    highlight_buttons();
    if (g_form)
        vk_box_update(g_form);
    if (g_bar)
        vk_box_update(g_bar);
    if (g_vbox)
        vk_box_update(g_vbox);
    if (g_win) {
        c = vk_widget_get_canvas(VK_WIDGET(g_win));
        if (c)
            wbkgd(c, VDK_COLORS(COL_TEXT, COL_MENU));
        vk_window_update(g_win);
    }
    mf_ui_refresh();
}

void mf_devset_show(const char *id, const char *name, const char *json)
{
    int x, y, w, h;
    char cap[40];

    mf_devset_close();
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
    vk_window_set_child(g_win, VK_WIDGET(g_vbox));
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
    nbtn = g_nfields + 2;
    if (c == 27 || c == KEY_EXIT || c == KEY_CANCEL) {
        mf_devset_close();
        return 1;
    }
    if (c == '\t') {
        set_field_focus((g_focus + 1) % nbtn);
        paint_dialog();
        return 1;
    }
#ifdef KEY_BTAB
    if (c == KEY_BTAB) {
        set_field_focus((g_focus + nbtn - 1) % nbtn);
        paint_dialog();
        return 1;
    }
#endif
    if (c == '\n' || c == KEY_ENTER) {
        if (g_focus == g_nfields + 1) {
            mf_devset_close();
            return 1;
        }
        return 2;
    }
    if (g_focus >= g_nfields)
        return 1;
    in = g_in[g_focus];
    if (!in)
        return 1;
    if (c == KEY_BACKSPACE || c == 127) {
        vk_input_backspace(in);
        vk_input_update(in);
        paint_dialog();
        return 1;
    }
    if (c == KEY_LEFT) {
        vk_input_move_cursor(in, -1);
        vk_input_update(in);
        paint_dialog();
        return 1;
    }
    if (c == KEY_RIGHT) {
        vk_input_move_cursor(in, 1);
        vk_input_update(in);
        paint_dialog();
        return 1;
    }
    if (c >= 32 && c < 127) {
        vk_input_insert_char(in, (int)c);
        vk_input_update(in);
        paint_dialog();
        return 1;
    }
    return 1;
}

void mf_confirm_close(void)
{
    if (!g_cf_open)
        return;
    if (g_cf_l1) {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_cf_l1));
        vk_label_destroy(g_cf_l1);
        g_cf_l1 = NULL;
    }
    if (g_cf_l2) {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_cf_l2));
        vk_label_destroy(g_cf_l2);
        g_cf_l2 = NULL;
    }
    if (g_cf_win) {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_cf_win));
        vk_window_destroy(g_cf_win);
        g_cf_win = NULL;
    }
    g_cf_open = 0;
    mf_ui_refresh();
}

int mf_confirm_open(void) { return g_cf_open; }
const char *mf_confirm_action(void) { return g_cf_key; }

void mf_confirm_show(const char *name, const char *key)
{
    int x, y, w, h;
    char cap[40], msg[64];

    mf_confirm_close();
    snprintf(g_cf_key, sizeof(g_cf_key), "%s", key ? key : "charge");
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
    if (strcmp(g_cf_key, "balance") == 0)
        snprintf(msg, sizeof(msg), "Turn off balancer?");
    else if (strcmp(g_cf_key, "discharge") == 0)
        snprintf(msg, sizeof(msg), "Turn off discharge MOSFET?");
    else
        snprintf(msg, sizeof(msg), "Turn off charge MOSFET?");
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
    if (c == 'n' || c == 'N' || c == 27 || c == KEY_EXIT) {
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

    if (bstate & LEFT) {
        lx = x - win_x;
        ly = y - win_y;
        if (ly >= 3 && ly <= 5) {
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

    if (!g_open || !g_win)
        return 0;

    vk_widget_get_position(VK_WIDGET(g_win), &win_x, &win_y);
    vk_widget_get_metrics(VK_WIDGET(g_win), &win_w, &win_h);
    if (x < win_x || y < win_y || x >= win_x + win_w || y >= win_y + win_h)
        return 1;

    if (!(bstate & LEFT) || !g_vbox || !g_bar)
        return 1;

    vk_widget_get_position(VK_WIDGET(g_vbox), &ox, &oy);
    vk_widget_get_position(VK_WIDGET(g_bar), &bx, &by);
    ox += win_x + bx;
    oy += win_y + by;

    if (hit_btn(g_btn_save, ox, oy, x, y)) {
        int rc = vk_button_press(g_btn_save);
        vk_button_update(g_btn_save);
        paint_dialog();
        return rc == 2 ? 2 : 1;
    }
    if (hit_btn(g_btn_exit, ox, oy, x, y)) {
        vk_button_press(g_btn_exit);
        return 1;
    }
    return 1;
}
