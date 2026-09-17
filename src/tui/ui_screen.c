#include "config/config.h"
#include "ui_screen.h"
#include "http_client.h"
#include "layout.h"
#include "mouse.h"

#include <cJSON.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vdk.h>
#include <vkmio.h>

#define COL_BG   COLOR_WHITE
#define COL_TEXT COLOR_BLACK
#define COL_MENU COLOR_CYAN
#define COL_DASH_FG COLOR_WHITE
#define COL_DASH_BG COLOR_BLUE
#define FRONT_MAX 8
#define STALE_SECS 8.0

static vk_screen_t *g_screen;
static int g_kmio_fd = -1;
static int g_quit;
static vk_widget_t *g_front[FRONT_MAX];
static int g_nfront;
static mf_http_cli_t g_cli;
static mf_tui_config_t g_tui_cfg;
static char g_cfg_path[256];
static char g_hostport[128];
static char g_host[128];
static int g_port = 5250;
static double g_refresh = 1.0;
static double g_last_get;
static int g_settings_open;
static vk_window_t *g_set_win;
static vk_box_t *g_set_vbox, *g_set_mid, *g_set_inner, *g_set_form, *g_set_bar;
static vk_grid_t *g_set_row[3];
static vk_grid_t *g_set_fields[3];
static vk_label_t *g_set_lab[3];
static vk_label_t *g_set_hint[3];
static vk_input_t *g_set_in[3];
static vk_button_t *g_set_ok, *g_set_cancel;
static vk_filler_t *g_set_fill, *g_set_form_fill;
static vk_filler_t *g_set_pad_top, *g_set_pad_bot, *g_set_pad_left, *g_set_pad_right;
static int g_set_focus;
static void close_settings(void);
static void paint_settings(void);
static void apply_settings(void);
static vk_window_t *g_help_win;
static int g_view_idx = -1;
static char g_view_path[192];
static char g_view_json[65536];
static char g_view_name[32];
static int g_devset_fetch;
static int g_devset_wait_ovp;
static int g_devset_ovp_tries;
static double g_devset_next_try;
static char g_hist_json[65536];

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void wallpaper(vk_screen_t *s, int id, WINDOW *c)
{
    (void)s;
    (void)id;
    wbkgd(c, VDK_COLORS(COL_DASH_FG, COL_DASH_BG));
    werase(c);
}

vk_screen_t *mf_ui_screen(void) { return g_screen; }
int mf_ui_cols(void) { return COLS > 0 ? COLS : 80; }
int mf_ui_rows(void) { return LINES > 0 ? LINES : 25; }
void mf_ui_quit(void) { g_quit = 1; }

void mf_ui_attach(vk_widget_t *w, int x, int y)
{
    vk_screen_attach_widget(g_screen, 0, w);
    vk_widget_move(w, x, y);
}

/* Flat (no relief) client-area frame: cyan on blue.  Caller sizes, attaches
   and fills it. */
vk_frame_t *mf_ui_make_client_frame(int w, int h)
{
    vk_frame_t *f = vk_frame_create(w, h);
    if (!f)
        return NULL;
    vk_widget_set_colors(VK_WIDGET(f), COLOR_CYAN, COLOR_BLUE);
    vk_frame_set_border_style(f, VK_BORDER_SINGLE);
    vk_frame_set_border_colors(f, COLOR_CYAN, COLOR_BLUE);
    return f;
}

void mf_ui_front_clear(void) { g_nfront = 0; }

void mf_ui_front_push(vk_widget_t *w)
{
    if (g_nfront < FRONT_MAX)
        g_front[g_nfront++] = w;
}

void mf_ui_refresh(void)
{
    int i;
    vk_screen_refresh(g_screen);
    for (i = 0; i < g_nfront; i++)
        vk_widget_draw(g_front[i]);
}

void mf_ui_resize(void)
{
    vk_screen_resize(g_screen);
    mf_dash_on_resize();
    mf_pack_on_resize();
    mf_menubar_on_resize();
    mf_ui_refresh();
}

static int on_set_ok(vk_widget_t *w, void *a)
{
    (void)w;
    (void)a;
    apply_settings();
    return 1;
}

static int on_set_cancel(vk_widget_t *w, void *a)
{
    (void)w;
    (void)a;
    close_settings();
    return 1;
}

static vk_filler_t *mk_set_pad(int w, int h)
{
    vk_filler_t *f = vk_filler_create();
    uint32_t st = vk_widget_get_state(VK_WIDGET(f));

    vk_widget_set_state(VK_WIDGET(f), st & ~(uint32_t)VK_STATE_EXPAND);
    vk_widget_set_colors(VK_WIDGET(f), COL_TEXT, COL_MENU);
    vk_widget_resize(VK_WIDGET(f), w, h);
    return f;
}

static void style_set_input(vk_input_t *in, int focused)
{
    if (!in)
        return;
    if (focused) {
        vk_widget_set_colors(VK_WIDGET(in), COLOR_WHITE, COL_MENU);
        vk_widget_set_attrs(VK_WIDGET(in), A_BOLD);
    } else {
        vk_widget_set_colors(VK_WIDGET(in), COL_TEXT, COL_MENU);
        vk_widget_set_attrs(VK_WIDGET(in), A_NORMAL);
    }
    vk_widget_set_relief_colors(VK_WIDGET(in), COLOR_WHITE, COLOR_BLACK);
    vk_input_show_cursor(in, focused ? true : false);
    vk_input_update(in);
}

static void paint_settings(void)
{
    int pass, i;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < 3; i++) {
            if (g_set_lab[i])
                vk_label_update(g_set_lab[i]);
            if (g_set_hint[i])
                vk_label_update(g_set_hint[i]);
            style_set_input(g_set_in[i], i == g_set_focus);
            if (g_set_fields[i])
                vk_grid_update(g_set_fields[i]);
            if (g_set_row[i])
                vk_grid_update(g_set_row[i]);
        }
        if (g_set_form)
            vk_box_update(g_set_form);
        if (g_set_ok)
            vk_button_update(g_set_ok);
        if (g_set_cancel)
            vk_button_update(g_set_cancel);
        if (g_set_bar)
            vk_box_update(g_set_bar);
        if (g_set_inner)
            vk_box_update(g_set_inner);
        if (g_set_mid)
            vk_box_update(g_set_mid);
        if (g_set_vbox)
            vk_box_update(g_set_vbox);
    }
    if (g_set_win)
        vk_window_update(g_set_win);
    mf_ui_refresh();
}

static vk_button_t *mk_set_btn(const char *txt, VkWidgetFunc fn)
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

static void close_settings(void)
{
    int i;
    if (!g_settings_open)
        return;
    if (g_set_win)
        vk_window_set_child(g_set_win, NULL, VK_INHERIT_NONE);
    box_vacate(g_set_vbox);
    box_vacate(g_set_mid);
    box_vacate(g_set_inner);
    box_vacate(g_set_form);
    box_vacate(g_set_bar);
    for (i = 0; i < 3; i++) {
        if (g_set_row[i]) {
            vk_grid_destroy(g_set_row[i]);
            g_set_row[i] = NULL;
        }
        if (g_set_fields[i]) {
            vk_grid_destroy(g_set_fields[i]);
            g_set_fields[i] = NULL;
        }
        if (g_set_in[i]) {
            vk_input_destroy(g_set_in[i]);
            g_set_in[i] = NULL;
        }
        if (g_set_lab[i]) {
            vk_label_destroy(g_set_lab[i]);
            g_set_lab[i] = NULL;
        }
        if (g_set_hint[i]) {
            vk_label_destroy(g_set_hint[i]);
            g_set_hint[i] = NULL;
        }
    }
    if (g_set_ok) {
        vk_button_destroy(g_set_ok);
        g_set_ok = NULL;
    }
    if (g_set_cancel) {
        vk_button_destroy(g_set_cancel);
        g_set_cancel = NULL;
    }
    if (g_set_fill) {
        vk_filler_destroy(g_set_fill);
        g_set_fill = NULL;
    }
    if (g_set_form_fill) {
        vk_filler_destroy(g_set_form_fill);
        g_set_form_fill = NULL;
    }
    if (g_set_bar) {
        vk_box_destroy(g_set_bar);
        g_set_bar = NULL;
    }
    if (g_set_form) {
        vk_box_destroy(g_set_form);
        g_set_form = NULL;
    }
    if (g_set_inner) {
        vk_box_destroy(g_set_inner);
        g_set_inner = NULL;
    }
    if (g_set_pad_left) {
        vk_filler_destroy(g_set_pad_left);
        g_set_pad_left = NULL;
    }
    if (g_set_pad_right) {
        vk_filler_destroy(g_set_pad_right);
        g_set_pad_right = NULL;
    }
    if (g_set_mid) {
        vk_box_destroy(g_set_mid);
        g_set_mid = NULL;
    }
    if (g_set_pad_top) {
        vk_filler_destroy(g_set_pad_top);
        g_set_pad_top = NULL;
    }
    if (g_set_pad_bot) {
        vk_filler_destroy(g_set_pad_bot);
        g_set_pad_bot = NULL;
    }
    if (g_set_vbox) {
        vk_box_destroy(g_set_vbox);
        g_set_vbox = NULL;
    }
    if (g_set_win) {
        vk_screen_detach_widget(g_screen, 0, VK_WIDGET(g_set_win));
        vk_window_destroy(g_set_win);
        g_set_win = NULL;
    }
    g_settings_open = 0;
    mf_ui_front_clear();
    mf_ui_refresh();
}

void mf_ui_open_settings(void)
{
    int x, y, w, h, i, iw, ih;
    char buf[32];
    static const char *names[3] = { "Host", "Port", "Refresh" };

    if (g_settings_open)
        return;
    mf_tui_settings_geom(mf_ui_cols(), mf_ui_rows(), &x, &y, &w, &h);
    iw = w - 2;
    ih = h - 2;
    g_set_win = vk_window_create(w, h);
    vk_window_set_title(g_set_win, " General ");
    vk_window_set_border_style(g_set_win, VK_BORDER_SINGLE);
    vk_window_set_border_colors(g_set_win, COLOR_WHITE, COL_MENU);
    vk_window_set_border_attrs(g_set_win, A_BOLD);
    vk_widget_set_colors(VK_WIDGET(g_set_win), COL_TEXT, COL_MENU);

    g_set_form = vk_box_create(iw - 2, ih - 5, VK_BOX_VERTICAL, 4);
    vk_box_set_homogeneous(g_set_form, false);
    vk_widget_set_colors(VK_WIDGET(g_set_form), COL_TEXT, COL_MENU);
    vk_widget_set_expand(VK_WIDGET(g_set_form));
    for (i = 0; i < 3; i++) {
        static const char *hints[3] = { "", " (TCP)", " (Seconds)" };
        int fields_w = iw - 2 - 13;
        int in_w;

        if (fields_w < 12 + 8)
            fields_w = 12 + 8;
        in_w = fields_w - 12;
        if (in_w < 8)
            in_w = 8;

        g_set_fields[i] = vk_grid_create(fields_w, 3, 2, 1);
        vk_grid_set_homogeneous(g_set_fields[i], false);
        vk_grid_set_gap(g_set_fields[i], 0);
        vk_grid_set_col_width(g_set_fields[i], 0, 12);
        vk_grid_set_col_width(g_set_fields[i], 1, in_w);
        vk_grid_set_row_height(g_set_fields[i], 0, 3);
        vk_widget_set_colors(VK_WIDGET(g_set_fields[i]), COL_TEXT, COL_MENU);
        vk_widget_set_expand(VK_WIDGET(g_set_fields[i]));
        g_set_lab[i] = vk_label_create(12);
        vk_widget_set_colors(VK_WIDGET(g_set_lab[i]), COL_TEXT, COL_MENU);
        vk_label_set_text(g_set_lab[i], names[i]);
        vk_label_update(g_set_lab[i]);
        g_set_in[i] = vk_input_create(in_w);
        vk_input_set_border_style(g_set_in[i], VK_BORDER_SINGLE);
        vk_grid_set_widget(g_set_fields[i], 0, 0, VK_WIDGET(g_set_lab[i]), VK_INHERIT_NONE);
        vk_grid_set_widget(g_set_fields[i], 1, 0, VK_WIDGET(g_set_in[i]), VK_INHERIT_NONE);

        g_set_hint[i] = vk_label_create(13);
        vk_widget_set_colors(VK_WIDGET(g_set_hint[i]), COL_TEXT, COL_MENU);
        vk_label_set_text(g_set_hint[i], hints[i][0] ? hints[i] : "");
        vk_label_update(g_set_hint[i]);

        g_set_row[i] = vk_grid_create(iw - 2, 3, 2, 1);
        vk_grid_set_homogeneous(g_set_row[i], false);
        vk_grid_set_gap(g_set_row[i], 0);
        vk_grid_set_col_width(g_set_row[i], 0, fields_w);
        vk_grid_set_col_expand(g_set_row[i], 0, true);
        vk_grid_set_col_width(g_set_row[i], 1, 13);
        vk_grid_set_row_height(g_set_row[i], 0, 3);
        vk_widget_set_colors(VK_WIDGET(g_set_row[i]), COL_TEXT, COL_MENU);
        vk_grid_set_widget(g_set_row[i], 0, 0, VK_WIDGET(g_set_fields[i]), VK_INHERIT_NONE);
        vk_grid_set_widget(g_set_row[i], 1, 0, VK_WIDGET(g_set_hint[i]), VK_INHERIT_NONE);
        vk_box_set_widget(g_set_form, i, VK_WIDGET(g_set_row[i]), VK_INHERIT_NONE);
    }
    {
        int slack = (ih - 5) - 9;

        if (slack > 0) {
            uint32_t st;

            g_set_form_fill = vk_filler_create();
            st = vk_widget_get_state(VK_WIDGET(g_set_form_fill));
            vk_widget_set_state(VK_WIDGET(g_set_form_fill),
                                st & ~(uint32_t)VK_STATE_EXPAND);
            vk_widget_set_colors(VK_WIDGET(g_set_form_fill), COL_TEXT, COL_MENU);
            vk_widget_resize(VK_WIDGET(g_set_form_fill), iw - 2, slack);
            vk_box_set_widget(g_set_form, 3, VK_WIDGET(g_set_form_fill), VK_INHERIT_NONE);
        }
    }
    vk_input_set_text(g_set_in[0], g_host);
    snprintf(buf, sizeof(buf), "%d", g_port);
    vk_input_set_text(g_set_in[1], buf);
    snprintf(buf, sizeof(buf), "%.2f", g_refresh);
    vk_input_set_text(g_set_in[2], buf);

    g_set_bar = vk_box_create(iw - 2, 3, VK_BOX_HORIZONTAL, 3);
    vk_box_set_homogeneous(g_set_bar, false);
    vk_widget_set_colors(VK_WIDGET(g_set_bar), COL_TEXT, COL_MENU);
    g_set_ok = mk_set_btn("OK", on_set_ok);
    g_set_cancel = mk_set_btn("Cancel", on_set_cancel);
    g_set_fill = vk_filler_create();
    vk_widget_set_colors(VK_WIDGET(g_set_fill), COL_TEXT, COL_MENU);
    vk_widget_set_expand(VK_WIDGET(g_set_fill));
    vk_box_set_widget(g_set_bar, 0, VK_WIDGET(g_set_ok), VK_INHERIT_NONE);
    vk_box_set_widget(g_set_bar, 1, VK_WIDGET(g_set_fill), VK_INHERIT_NONE);
    vk_box_set_widget(g_set_bar, 2, VK_WIDGET(g_set_cancel), VK_INHERIT_NONE);

    g_set_inner = vk_box_create(iw - 2, ih - 2, VK_BOX_VERTICAL, 2);
    vk_box_set_homogeneous(g_set_inner, false);
    vk_widget_set_colors(VK_WIDGET(g_set_inner), COL_TEXT, COL_MENU);
    vk_widget_set_expand(VK_WIDGET(g_set_inner));
    vk_box_set_widget(g_set_inner, 0, VK_WIDGET(g_set_form), VK_INHERIT_NONE);
    vk_box_set_widget(g_set_inner, 1, VK_WIDGET(g_set_bar), VK_INHERIT_NONE);

    g_set_pad_left = mk_set_pad(1, ih - 2);
    g_set_pad_right = mk_set_pad(1, ih - 2);
    g_set_mid = vk_box_create(iw, ih - 2, VK_BOX_HORIZONTAL, 3);
    vk_box_set_homogeneous(g_set_mid, false);
    vk_widget_set_colors(VK_WIDGET(g_set_mid), COL_TEXT, COL_MENU);
    vk_widget_set_expand(VK_WIDGET(g_set_mid));
    vk_box_set_widget(g_set_mid, 0, VK_WIDGET(g_set_pad_left), VK_INHERIT_NONE);
    vk_box_set_widget(g_set_mid, 1, VK_WIDGET(g_set_inner), VK_INHERIT_NONE);
    vk_box_set_widget(g_set_mid, 2, VK_WIDGET(g_set_pad_right), VK_INHERIT_NONE);

    g_set_pad_top = mk_set_pad(iw, 1);
    g_set_pad_bot = mk_set_pad(iw, 1);
    g_set_vbox = vk_box_create(iw, ih, VK_BOX_VERTICAL, 3);
    vk_box_set_homogeneous(g_set_vbox, false);
    vk_widget_set_colors(VK_WIDGET(g_set_vbox), COL_TEXT, COL_MENU);
    vk_widget_set_expand(VK_WIDGET(g_set_vbox));
    vk_box_set_widget(g_set_vbox, 0, VK_WIDGET(g_set_pad_top), VK_INHERIT_NONE);
    vk_box_set_widget(g_set_vbox, 1, VK_WIDGET(g_set_mid), VK_INHERIT_NONE);
    vk_box_set_widget(g_set_vbox, 2, VK_WIDGET(g_set_pad_bot), VK_INHERIT_NONE);
    vk_window_set_child(g_set_win, VK_WIDGET(g_set_vbox), VK_INHERIT_NONE);
    mf_ui_attach(VK_WIDGET(g_set_win), x, y);
    paint_settings();

    g_set_focus = 0;
    g_settings_open = 1;
    mf_ui_front_clear();
    mf_ui_front_push(VK_WIDGET(g_set_win));
    mf_ui_refresh();
}

static void apply_settings(void)
{
    const char *h = vk_input_get_text(g_set_in[0]);
    const char *ps = vk_input_get_text(g_set_in[1]);
    const char *rs = vk_input_get_text(g_set_in[2]);
    int p;
    double r;
    if (h && h[0])
        snprintf(g_host, sizeof(g_host), "%s", h);
    p = ps ? atoi(ps) : g_port;
    if (p > 0 && p <= 65535)
        g_port = p;
    r = rs ? atof(rs) : g_refresh;
    if (r < 0.25)
        r = 0.25;
    if (r > 10.0)
        r = 10.0;
    g_refresh = r;
    snprintf(g_hostport, sizeof(g_hostport), "%.120s:%d", g_host, g_port);
    snprintf(g_tui_cfg.connect, sizeof(g_tui_cfg.connect), "%s", g_hostport);
    g_tui_cfg.refresh_interval_s = g_refresh;
    mf_http_cli_close(&g_cli);
    mf_http_cli_init(&g_cli, g_host, g_port);
    mf_http_cli_start(&g_cli, mono_now());
    close_settings();
}

static int settings_key(wint_t c)
{
    vk_input_t *in;
    if (!g_settings_open)
        return 0;
    if (c == 27 || c == KEY_EXIT || c == KEY_CANCEL) {
        close_settings();
        return 1;
    }
    if (c == '\t') {
        if (g_set_focus < 3)
            vk_input_show_cursor(g_set_in[g_set_focus], false);
        if (g_set_focus < 3)
            vk_input_update(g_set_in[g_set_focus]);
        g_set_focus = (g_set_focus + 1) % 5;
        if (g_set_focus < 3) {
            vk_input_show_cursor(g_set_in[g_set_focus], true);
            vk_input_update(g_set_in[g_set_focus]);
        }
        if (g_set_ok) {
            vk_widget_set_colors(VK_WIDGET(g_set_ok),
                                 g_set_focus == 3 ? COLOR_YELLOW : COL_TEXT,
                                 COL_MENU);
            vk_button_update(g_set_ok);
        }
        if (g_set_cancel) {
            vk_widget_set_colors(VK_WIDGET(g_set_cancel),
                                 g_set_focus == 4 ? COLOR_YELLOW : COL_TEXT,
                                 COL_MENU);
            vk_button_update(g_set_cancel);
        }
        paint_settings();
        return 1;
    }
    if (c == '\n' || c == KEY_ENTER) {
        if (g_set_focus == 4)
            close_settings();
        else
            apply_settings();
        return 1;
    }
    if (g_set_focus >= 3)
        return 1;
    in = g_set_in[g_set_focus];
    if (c == KEY_BACKSPACE || c == 127) {
        vk_input_backspace(in);
        paint_settings();
        return 1;
    }
    if (c == KEY_LEFT) {
        vk_input_move_cursor(in, -1);
        paint_settings();
        return 1;
    }
    if (c == KEY_RIGHT) {
        vk_input_move_cursor(in, 1);
        paint_settings();
        return 1;
    }
    if (c >= 32 && c < 127) {
        vk_input_insert_char(in, (int)c);
        paint_settings();
        return 1;
    }
    return 1;
}

static void close_help(void)
{
    if (g_help_win) {
        vk_screen_detach_widget(g_screen, 0, VK_WIDGET(g_help_win));
        vk_window_destroy(g_help_win);
        g_help_win = NULL;
        mf_ui_refresh();
    }
}

void mf_ui_show_help(int about)
{
    vk_listbox_t *lb;
    int w = 70, h = 22;
    if (g_help_win)
        close_help();
    if (w > mf_ui_cols() - 2)
        w = mf_ui_cols() - 2;
    if (h > mf_ui_rows() - 3)
        h = mf_ui_rows() - 3;
    g_help_win = vk_window_create(w, h);
    vk_window_set_title(g_help_win, about ? " About " : " Keyboard ");
    vk_window_set_border_style(g_help_win, VK_BORDER_SINGLE);
    vk_widget_set_colors(VK_WIDGET(g_help_win), COL_TEXT, COL_MENU);
    lb = vk_listbox_create(w - 2, h - 2);
    vk_widget_set_colors(VK_WIDGET(lb), COL_TEXT, COL_MENU);
    if (about) {
        vk_listbox_add_item(lb, "Moon Flare TUI 0.1.0", NULL, NULL);
        vk_listbox_add_item(lb, "libviper/VDK  80x25", NULL, NULL);
    } else {
        vk_listbox_add_item(lb, "F10  menubar", NULL, NULL);
        vk_listbox_add_item(lb, "q    quit", NULL, NULL);
        vk_listbox_add_item(lb, "Esc  close dialog", NULL, NULL);
        vk_listbox_add_item(lb, "Tab  next field", NULL, NULL);
    }
    vk_window_set_child(g_help_win, VK_WIDGET(lb), VK_INHERIT_NONE);
    mf_ui_attach(VK_WIDGET(g_help_win),
                 (mf_ui_cols() - w) / 2, 2);
    vk_listbox_update(lb);
    vk_window_update(g_help_win);
    mf_ui_refresh();
}

void mf_ui_save_config(void)
{
    char *s = mf_tui_config_serialize(&g_tui_cfg);
    FILE *f;
    const char *path = g_cfg_path[0] ? g_cfg_path : NULL;
    char tmp[256];
    if (!path) {
        const char *home = getenv("HOME");
        snprintf(tmp, sizeof(tmp), "%s/.config/moonflare/moonflare.json",
                 home ? home : ".");
        path = tmp;
    }
    if (!s)
        return;
    f = fopen(path, "w");
    if (f) {
        fputs(s, f);
        fclose(f);
    }
    free(s);
}

void mf_ui_show_dashboard(void)
{
    mf_confirm_close();
    mf_devset_close();
    mf_pack_hide();
    mf_dash_set_visible(1);
    g_view_idx = -1;
    g_view_path[0] = '\0';
    mf_ui_refresh();
}

static void post_switch(const char *key, int on)
{
    char path[192], payload[80];

    if (g_view_idx < 0 || !key)
        return;
    snprintf(path, sizeof(path),
             "/api/v1/devices/%s/actions/set_switch",
             mf_dash_catalog_id(g_view_idx));
    snprintf(payload, sizeof(payload),
             "{\"key\":\"%s\",\"value\":%s}", key, on ? "true" : "false");
    (void)mf_http_cli_post(&g_cli, path, payload);
}

const char *mf_ui_poll_path(void)
{
    if (g_view_idx >= 0 && g_view_path[0])
        return g_view_path;
    return "/api/v1/status";
}

void mf_ui_open_device_view(int idx)
{
    const char *id = mf_dash_catalog_id(idx);
    const char *kind = mf_dash_catalog_kind(idx);
    const char *name = mf_dash_catalog_name(idx);
    if (!id || !id[0])
        return;
    g_view_idx = idx;
    snprintf(g_view_name, sizeof(g_view_name), "%s", name ? name : id);
    snprintf(g_view_path, sizeof(g_view_path), "/api/v1/devices/%s", id);
    mf_dash_set_visible(0);
    mf_pack_show(kind && strcmp(kind, "charger") == 0);
    (void)mf_http_cli_get(&g_cli, g_view_path);
    mf_ui_refresh();
}

void mf_ui_request_history(const char *id)
{
    char path[192];
    snprintf(path, sizeof(path), "/api/v1/devices/%s/history", id);
    (void)mf_http_cli_get(&g_cli, path);
}

void mf_ui_handle_history(void)
{
    cJSON *root, *ts_arr, *val_arr;
    int i, n, n_bars, bi;
    double *ts, *vals;
    double bucketed[512], max_val = 0;
    int bucket_count;
    double interval_s;
    double t_start, t_end;

    if (g_hist_json[0] == '\0')
        return;
    root = cJSON_Parse(g_hist_json);
    if (!root)
    {
        g_hist_json[0] = '\0';
        return;
    }
    ts_arr = cJSON_GetObjectItemCaseSensitive(root, "ts");
    val_arr = cJSON_GetObjectItemCaseSensitive(root, "values");
    if (!ts_arr || !cJSON_IsArray(ts_arr) ||
        !val_arr || !cJSON_IsArray(val_arr))
    {
        cJSON_Delete(root);
        g_hist_json[0] = '\0';
        return;
    }
    n = cJSON_GetArraySize(ts_arr);
    if (n != cJSON_GetArraySize(val_arr) || n <= 0)
    {
        cJSON_Delete(root);
        g_hist_json[0] = '\0';
        return;
    }

    /* Data is oldest-first. Keep the latest 4096 (match daemon cap). */
    int cap = 4096;
    int skip = 0;
    if (n > cap) {
        skip = n - cap;
        n = cap;
    }

    ts = malloc((size_t)n * sizeof(double));
    vals = malloc((size_t)n * sizeof(double));
    if (!ts || !vals) {
        free(ts);
        free(vals);
        cJSON_Delete(root);
        g_hist_json[0] = '\0';
        return;
    }
    for (i = 0; i < n; i++) {
        int idx = i + skip;
        cJSON *te = cJSON_GetArrayItem(ts_arr, idx);
        cJSON *ve = cJSON_GetArrayItem(val_arr, idx);
        ts[i] = (te && cJSON_IsNumber(te)) ? te->valuedouble : 0.0;
        vals[i] = (ve && cJSON_IsNumber(ve)) ? ve->valuedouble : 0.0;
    }

    /* Rightmost bar = current timeslot; work left with a full row of
       slots (zeros if no samples) so bars meet the Y spine. */
    interval_s = (double)mf_pack_get_graph_interval() * 60.0;
    t_end = ts[n - 1];
    {
        int cells = mf_pack_graph_bar_width();
        double last_start;

        if (cells < 1)
            cells = 1;
        n_bars = (mf_ui_cols() - 12) / cells;
        if (n_bars < 8)
            n_bars = 8;
        if (n_bars > 512)
            n_bars = 512;
        last_start = (double)((long)(t_end / interval_s)) * interval_s;
        t_start = last_start - (double)(n_bars - 1) * interval_s;
        t_end = last_start + interval_s;
    }

    /* Initialize bucketed array to 0. */
    for (bi = 0; bi < n_bars; bi++)
        bucketed[bi] = 0.0;

    /* Assign each sample to a bucket. */
    for (i = 0; i < n; i++) {
        int bucket = (int)((ts[i] - t_start) / interval_s);
        if (bucket < 0 || bucket >= n_bars)
            continue;
        bucketed[bucket] += vals[i];
    }

    /* Compute mean per bucket. */
    bucket_count = n_bars;
    for (bi = 0; bi < n_bars; bi++) {
        /* Count samples in this bucket to compute the mean. */
        int cnt = 0;
        double sum = 0;
        for (i = 0; i < n; i++) {
            int b = (int)((ts[i] - t_start) / interval_s);
            if (b < 0 || b >= n_bars)
                continue;
            if (b == bi) {
                sum += vals[i];
                cnt++;
            }
        }
        if (cnt > 0)
            bucketed[bi] = sum / (double)cnt;
        else
            bucketed[bi] = 0.0;
        if (bucketed[bi] > max_val)
            max_val = bucketed[bi];
    }

    double y_max = max_val * 1.2;
    if (y_max < 1.0)
        y_max = 1.0;

    /* Build X-axis time labels, one per bar, oldest-first. */
    const char *labels[512];
    int label_count = 0;
    int interval_min = mf_pack_get_graph_interval();
    for (bi = 0; bi < n_bars && label_count < 512; bi++) {
        time_t bt = (time_t)(t_start + (double)bi * interval_s);
        struct tm tm_buf;
        struct tm *tmp = localtime_r(&bt, &tm_buf);
        char buf[16];
        if (tmp) {
            if (interval_min >= 24 * 60)
                strftime(buf, sizeof(buf), "%b %d %H:%M", tmp);
            else
                strftime(buf, sizeof(buf), "%H:%M", tmp);
        } else {
            snprintf(buf, sizeof(buf), "??:??");
        }
        labels[label_count] = strdup(buf);
        label_count++;
    }

    mf_pack_set_history(bucketed, bucket_count, y_max,
        label_count > 0 ? (const char * const *)labels : NULL);

    for (bi = 0; bi < label_count; bi++)
        free((void *)labels[bi]);

    free(ts);
    free(vals);
    cJSON_Delete(root);
    g_hist_json[0] = '\0';
}

void mf_ui_open_device_settings(int idx)
{
    const char *id = mf_dash_catalog_id(idx);
    const char *name = mf_dash_catalog_name(idx);
    char path[192];

    if (!id || !id[0])
        return;
    mf_devset_show(id, name, NULL);
    mf_devset_set_graph_interval(mf_pack_get_graph_interval());
    snprintf(path, sizeof(path), "/api/v1/devices/%s/settings", id);
    g_devset_fetch = 1;
    g_devset_wait_ovp = 1;
    g_devset_ovp_tries = 0;
    g_devset_next_try = 0;
    (void)mf_http_cli_get(&g_cli, path);
}

void mf_ui_load_config(void)
{
    mf_tui_config_load(g_cfg_path[0] ? g_cfg_path : NULL, &g_tui_cfg);
    if (g_tui_cfg.refresh_interval_s >= 0.25 &&
        g_tui_cfg.refresh_interval_s <= 10.0)
        g_refresh = g_tui_cfg.refresh_interval_s;
}

static void parse_connect(const char *spec)
{
    const char *colon;
    if (!spec || !spec[0])
        spec = "127.0.0.1:5250";
    colon = strrchr(spec, ':');
    if (colon && colon != spec) {
        size_t n = (size_t)(colon - spec);
        if (n >= sizeof(g_host))
            n = sizeof(g_host) - 1;
        memcpy(g_host, spec, n);
        g_host[n] = '\0';
        g_port = atoi(colon + 1);
        if (g_port <= 0)
            g_port = 5250;
    } else {
        snprintf(g_host, sizeof(g_host), "%s", spec);
        g_port = 5250;
    }
    snprintf(g_hostport, sizeof(g_hostport), "%.120s:%d", g_host, g_port);
}

static int dump_layout(const char *which)
{
    char grid[MF_TUI_ROWS][MF_TUI_COLS + 1];
    int r;
    if (which && strcmp(which, "pack") == 0)
        mf_tui_paint_pack(grid, 1);
    else if (which && strcmp(which, "charger") == 0)
        mf_tui_paint_charger(grid);
    else if (which && (strcmp(which, "settings") == 0 ||
                       strcmp(which, "devsettings") == 0))
        mf_tui_paint_devsettings(grid, "pack-demo");
    else if (which && strcmp(which, "confirm") == 0)
        mf_tui_paint_confirm(grid, "pack-demo");
    else
        mf_tui_paint_dashboard(grid, "127.0.0.1:5250", "UP",
                               0, NULL, 0, NULL, 0, NULL);
    for (r = 0; r < MF_TUI_ROWS; r++)
        puts(grid[r]);
    return 0;
}

int mf_tui_run(const char *connect, const char *config_path)
{
    mf_tui_config_defaults(&g_tui_cfg);
    if (config_path && config_path[0])
        snprintf(g_cfg_path, sizeof(g_cfg_path), "%s", config_path);
    mf_tui_config_load(config_path, &g_tui_cfg);
    if (connect && connect[0])
        parse_connect(connect);
    else if (g_tui_cfg.connect[0])
        parse_connect(g_tui_cfg.connect);
    else
        parse_connect("127.0.0.1:5250");
    if (g_tui_cfg.refresh_interval_s >= 0.25)
        g_refresh = g_tui_cfg.refresh_interval_s;
    snprintf(g_tui_cfg.connect, sizeof(g_tui_cfg.connect), "%s", g_hostport);
    g_tui_cfg.refresh_interval_s = g_refresh;

    g_screen = vk_screen_create();
    if (!g_screen) {
        fprintf(stderr, "vk_screen_create failed\n");
        return 1;
    }
    vdk_color_init();
    curs_set(0);
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    set_escdelay(1);
    vk_screen_set_wallpaper(g_screen, wallpaper);
    g_kmio_fd = vk_screen_get_fd(g_screen);
    if (g_kmio_fd < 0)
        g_kmio_fd = STDOUT_FILENO;
    vk_kmio_init(g_kmio_fd, VK_KMIO_MOUSE);

    mf_dash_init();
    mf_pack_init();
    mf_menubar_init();
    mf_ui_front_clear();
    mf_ui_refresh();

    signal(SIGINT, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    mf_http_cli_init(&g_cli, g_host, g_port);
    mf_http_cli_start(&g_cli, mono_now());
    g_last_get = 0;

    while (!g_quit) {
        fd_set r, w;
        int maxfd = STDIN_FILENO;
        struct timeval tv = { 0, 100000 };
        double t;
        int n, key;
        MEVENT mev;
        char body[65536];

        FD_ZERO(&r);
        FD_ZERO(&w);
        FD_SET(STDIN_FILENO, &r);
        mf_http_cli_prepare_fds(&g_cli, &r, &w, &maxfd);
        n = select(maxfd + 1, &r, &w, NULL, &tv);
        (void)n;
        t = mono_now();
        mf_http_cli_pump(&g_cli,
                         g_cli.fd >= 0 && FD_ISSET(g_cli.fd, &r),
                         g_cli.fd >= 0 && FD_ISSET(g_cli.fd, &w), t);

        if (g_cli.state == MF_CONN_UP && !g_cli.inflight &&
            t - g_last_get >= g_refresh && !g_devset_fetch &&
            !mf_devset_open()) {
            if (mf_http_cli_get(&g_cli, mf_ui_poll_path()) == 1)
                g_last_get = t;
        }
        {
            const char *tag = "WAIT";
            static char last_json[65536];
            static char last_tag[24];
            int dirty = 0;
            if (g_cli.state == MF_CONN_UP)
                tag = g_cli.stale ? "STALE" : "UP";
            else if (g_cli.state == MF_CONN_CONNECTING)
                tag = "reconnecting";
            if (mf_http_cli_take_body(&g_cli, body, sizeof(body))) {
                snprintf(last_json, sizeof(last_json), "%s", body);
                if (strstr(body, "\"values\"") && strstr(body, "\"column\""))
                    snprintf(g_hist_json, sizeof(g_hist_json), "%s", body);
                dirty = 1;
            }
            if (strcmp(tag, last_tag) != 0) {
                snprintf(last_tag, sizeof(last_tag), "%s", tag);
                dirty = 1;
            }
            if (g_devset_fetch && !mf_devset_open()) {
                g_devset_fetch = 0;
                g_devset_wait_ovp = 0;
            }
            if (mf_devset_touched())
                g_devset_wait_ovp = 0;
            if (mf_devset_has_key("cell_ovp_v"))
                g_devset_wait_ovp = 0;
            if (mf_devset_open() && g_devset_wait_ovp && !g_devset_fetch &&
                !g_cli.inflight && g_cli.state == MF_CONN_UP &&
                !mf_devset_touched() &&
                t >= g_devset_next_try && g_devset_ovp_tries < 15) {
                char spath[192];

                snprintf(spath, sizeof(spath),
                         "/api/v1/devices/%s/settings", mf_devset_id());
                if (mf_http_cli_get(&g_cli, spath) == 1) {
                    g_devset_fetch = 1;
                    g_devset_ovp_tries++;
                    g_devset_next_try = t + 1.0;
                }
            }
            if (dirty) {
                int is_status = last_json[0] &&
                    (strstr(last_json, "\"batteries\"") != NULL ||
                     strstr(last_json, "\"chargers\"") != NULL);
                int is_devset = last_json[0] &&
                    strstr(last_json, "\"uuid\"") != NULL && !is_status;
                if (g_devset_fetch && mf_devset_open()) {
                    g_devset_fetch = 0;
                    if (is_devset && !mf_devset_touched()) {
                        mf_devset_apply_json(last_json);
                        /* JK waits for a settings frame with OVP. XD/Classic
                         * never send those keys — stop retrying or the form
                         * rebuilds every second and flashes. */
                        if (strstr(last_json, "\"cell_ovp_v\"") ||
                            strstr(last_json, "\"ble.address\"") == NULL)
                            g_devset_wait_ovp = 0;
                    }
                } else if (g_view_idx >= 0 && strstr(last_json, "\"data\"")) {
                    snprintf(g_view_json, sizeof(g_view_json), "%s", last_json);
                    mf_pack_update(g_view_json);
                    mf_ui_handle_history();
                    if (mf_pack_is_charger() && !g_cli.inflight)
                        mf_ui_request_history(mf_pack_get_device_id());
                } else if (strstr(last_json, "\"values\"") &&
                    strstr(last_json, "\"column\"")) {
                    mf_ui_handle_history();
                } else if (is_status) {
                    mf_dash_update(g_hostport, tag, last_json);
                } else {
                    /* PUT/POST replies are not a device list; keep catalog. */
                    mf_dash_update(g_hostport, tag, NULL);
                }
                mf_ui_refresh();
            }
        }

        if (t - g_cli.last_fresh > STALE_SECS && g_cli.state == MF_CONN_UP)
            g_cli.stale = 1;

        key = vk_kmio_fetch(&mev);
        if (key == KEY_MOUSE) {
            int mr = mf_mouse_handle(&mev);
            if (mr == 2 && mf_confirm_open() && g_view_idx >= 0) {
                char path[192], payload[80];
                snprintf(path, sizeof(path),
                         "/api/v1/devices/%s/actions/set_switch",
                         mf_dash_catalog_id(g_view_idx));
                snprintf(payload, sizeof(payload),
                         "{\"key\":\"%s\",\"value\":false}",
                         mf_confirm_action());
                mf_confirm_close();
                (void)mf_http_cli_post(&g_cli, path, payload);
            } else if (mr == 2 && mf_devset_open()) {
                char path[192];
                int gi = mf_devset_get_graph_interval();
                snprintf(path, sizeof(path),
                         "/api/v1/devices/%s/settings", mf_devset_id());
                (void)mf_http_cli_put(&g_cli, path, mf_devset_payload());
                mf_pack_set_graph_interval(gi);
                mf_devset_close();
                g_last_get = 0;
            }
            continue;
        }
        if (key <= 0) {
            int ch = getch();
            if (ch != ERR)
                key = ch;
        }
        if (key > 0) {
            if (key == KEY_RESIZE) {
                mf_ui_resize();
                continue;
            }
            if (g_help_win && (key == 27 || key == 'q')) {
                close_help();
                continue;
            }
            if (mf_confirm_open()) {
                int cr = mf_confirm_handle((wint_t)key);
                if (cr == 2 && g_view_idx >= 0) {
                    char path[192], payload[80];
                    snprintf(path, sizeof(path),
                             "/api/v1/devices/%s/actions/set_switch",
                             mf_dash_catalog_id(g_view_idx));
                    snprintf(payload, sizeof(payload),
                             "{\"key\":\"%s\",\"value\":false}",
                             mf_confirm_action());
                    mf_confirm_close();
                    (void)mf_http_cli_post(&g_cli, path, payload);
                }
                continue;
            }
            if (mf_devset_open()) {
                int sr = mf_devset_key((wint_t)key);
                if (sr == 2) {
                    char path[192];
                    int gi = mf_devset_get_graph_interval();
                    snprintf(path, sizeof(path),
                             "/api/v1/devices/%s/settings", mf_devset_id());
                    (void)mf_http_cli_put(&g_cli, path, mf_devset_payload());
                    mf_pack_set_graph_interval(gi);
                    mf_devset_close();
                    g_last_get = 0;
                }
                continue;
            }
            if (g_settings_open && settings_key((wint_t)key))
                continue;
            if (mf_menubar_key((wint_t)key))
                continue;
            if (mf_pack_visible() && (key == 27 || key == KEY_EXIT)) {
                mf_ui_show_dashboard();
                continue;
            }
            if (mf_pack_visible() && (key == 'e' || key == 'E')) {
                if (g_view_idx >= 0)
                    mf_ui_open_device_settings(g_view_idx);
                continue;
            }
            if (mf_pack_visible() && mf_pack_is_charger() &&
                (key == '+' || key == '=')) {
                if (mf_pack_graph_zoom(1))
                    mf_ui_handle_history();
                continue;
            }
            if (mf_pack_visible() && mf_pack_is_charger() &&
                (key == '-' || key == '_')) {
                if (mf_pack_graph_zoom(0))
                    mf_ui_handle_history();
                continue;
            }
            if (mf_pack_visible() && !mf_pack_is_charger() && mf_pack_has_switch()) {
                if (key == 'c' || key == 'C') {
                    if (mf_pack_switch_on("charge"))
                        mf_confirm_show(g_view_name, "charge");
                    else
                        post_switch("charge", 1);
                    continue;
                }
                if (key == 'd' || key == 'D') {
                    if (mf_pack_switch_on("discharge"))
                        mf_confirm_show(g_view_name, "discharge");
                    else
                        post_switch("discharge", 1);
                    continue;
                }
                if (key == 'b' || key == 'B') {
                    post_switch("balance", !mf_pack_switch_on("balance"));
                    continue;
                }
            }
            if (key == 'q' || key == 'Q')
                g_quit = 1;
        }
    }

    close_help();
    close_settings();
    mf_confirm_close();
    mf_devset_close();
    mf_menubar_shutdown();
    mf_pack_shutdown();
    mf_dash_shutdown();
    mf_http_cli_close(&g_cli);
    if (g_kmio_fd >= 0)
        vk_kmio_shutdown(g_kmio_fd);
    vk_screen_destroy(g_screen);
    g_screen = NULL;
    return 0;
}

int mf_tui_dump_layout_main(const char *which)
{
    return dump_layout(which);
}

/* ---- Mouse helpers ---- */

/* Left-press mask. */
#define LEFT (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)

int
mf_help_mouse(int x, int y, mmask_t bstate)
{
    int win_x, win_y, win_w, win_h;

    if (!g_help_win)
        return 0;

    vk_widget_get_position(VK_WIDGET(g_help_win), &win_x, &win_y);
    vk_widget_get_metrics(VK_WIDGET(g_help_win), &win_w, &win_h);
    if (x < win_x || y < win_y || x >= win_x + win_w || y >= win_y + win_h)
        return 0;

    /* Left-click on help window → dismiss (same as Esc). */
    if (bstate & LEFT)
        close_help();
    return 1;
}

int
mf_settings_mouse(int x, int y, mmask_t bstate)
{
    int win_x, win_y, win_w, win_h;
    int ox, oy, bx, by, px, py, bw, bh;

    if (!g_settings_open || !g_set_win)
        return 0;

    vk_widget_get_position(VK_WIDGET(g_set_win), &win_x, &win_y);
    vk_widget_get_metrics(VK_WIDGET(g_set_win), &win_w, &win_h);
    if (x < win_x || y < win_y || x >= win_x + win_w || y >= win_y + win_h)
        return 0;

    if (!(bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)))
        return 1;
    if (!g_set_vbox || !g_set_bar)
        return 1;
    {
        int i, lx = x - win_x, ly = y - win_y, cy = 2;

        for (i = 0; i < 3; i++) {
            int rw = 0, rh = 3;

            if (g_set_row[i])
                vk_widget_get_metrics(VK_WIDGET(g_set_row[i]), &rw, &rh);
            if (rh < 1)
                rh = 3;
            if (lx >= 2 && lx < win_w - 2 && ly >= cy && ly < cy + rh) {
                if (g_set_focus != i) {
                    g_set_focus = i;
                    paint_settings();
                }
                return 1;
            }
            cy += rh;
        }
    }
    vk_widget_get_position(VK_WIDGET(g_set_vbox), &ox, &oy);
    vk_widget_get_position(VK_WIDGET(g_set_bar), &bx, &by);
    ox += win_x + bx;
    oy += win_y + by;
    if (g_set_ok) {
        vk_widget_get_position(VK_WIDGET(g_set_ok), &px, &py);
        vk_widget_get_metrics(VK_WIDGET(g_set_ok), &bw, &bh);
        if (x >= ox + px && x < ox + px + bw &&
            y >= oy + py && y < oy + py + bh) {
            vk_button_press(g_set_ok);
            return 1;
        }
    }
    if (g_set_cancel) {
        vk_widget_get_position(VK_WIDGET(g_set_cancel), &px, &py);
        vk_widget_get_metrics(VK_WIDGET(g_set_cancel), &bw, &bh);
        if (x >= ox + px && x < ox + px + bw &&
            y >= oy + py && y < oy + py + bh) {
            vk_button_press(g_set_cancel);
            return 1;
        }
    }
    return 1;
}

int mf_ui_help_open(void) { return g_help_win != NULL; }
int mf_ui_settings_open(void) { return g_settings_open; }
