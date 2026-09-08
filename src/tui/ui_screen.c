#include "config/config.h"
#include "ui_screen.h"
#include "http_client.h"
#include "layout.h"

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
static vk_label_t *g_set_lab[4];
static vk_input_t *g_set_in[3];
static int g_set_focus;
static vk_window_t *g_help_win;
static int g_view_idx = -1;
static char g_view_path[192];
static char g_view_json[65536];
static char g_view_name[32];

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
    wbkgd(c, VDK_COLORS(COL_TEXT, COL_BG));
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
    mf_menubar_on_resize();
    mf_ui_refresh();
}

static void close_settings(void)
{
    int i;
    if (!g_settings_open)
        return;
    for (i = 0; i < 3; i++) {
        if (g_set_in[i]) {
            vk_screen_detach_widget(g_screen, 0, VK_WIDGET(g_set_in[i]));
            vk_input_destroy(g_set_in[i]);
            g_set_in[i] = NULL;
        }
    }
    for (i = 0; i < 4; i++) {
        if (g_set_lab[i]) {
            vk_screen_detach_widget(g_screen, 0, VK_WIDGET(g_set_lab[i]));
            vk_label_destroy(g_set_lab[i]);
            g_set_lab[i] = NULL;
        }
    }
    if (g_set_win) {
        vk_screen_detach_widget(g_screen, 0, VK_WIDGET(g_set_win));
        vk_window_destroy(g_set_win);
        g_set_win = NULL;
    }
    g_settings_open = 0;
    mf_ui_refresh();
}

void mf_ui_open_settings(void)
{
    int x, y, w, h;
    char buf[32];
    if (g_settings_open)
        return;
    mf_tui_settings_geom(mf_ui_cols(), mf_ui_rows(), &x, &y, &w, &h);
    g_set_win = vk_window_create(w, h);
    vk_window_set_title(g_set_win, " General ");
    vk_window_set_border_style(g_set_win, VK_BORDER_SINGLE);
    vk_window_set_border_colors(g_set_win, COL_TEXT, COL_MENU);
    vk_widget_set_colors(VK_WIDGET(g_set_win), COL_TEXT, COL_MENU);
    mf_ui_attach(VK_WIDGET(g_set_win), x, y);
    vk_window_update(g_set_win);

    g_set_lab[0] = vk_label_create(10);
    vk_label_set_text(g_set_lab[0], "Host");
    mf_ui_attach(VK_WIDGET(g_set_lab[0]), x + 2, y + 2);
    g_set_in[0] = vk_input_create(w - 20);
    vk_input_set_text(g_set_in[0], g_host);
    vk_input_set_border_style(g_set_in[0], VK_BORDER_SINGLE);
    mf_ui_attach(VK_WIDGET(g_set_in[0]), x + 14, y + 2);

    g_set_lab[1] = vk_label_create(10);
    vk_label_set_text(g_set_lab[1], "Port");
    mf_ui_attach(VK_WIDGET(g_set_lab[1]), x + 2, y + 5);
    g_set_in[1] = vk_input_create(12);
    snprintf(buf, sizeof(buf), "%d", g_port);
    vk_input_set_text(g_set_in[1], buf);
    vk_input_set_border_style(g_set_in[1], VK_BORDER_SINGLE);
    mf_ui_attach(VK_WIDGET(g_set_in[1]), x + 14, y + 5);

    g_set_lab[2] = vk_label_create(12);
    vk_label_set_text(g_set_lab[2], "Refresh s");
    mf_ui_attach(VK_WIDGET(g_set_lab[2]), x + 2, y + 8);
    g_set_in[2] = vk_input_create(12);
    snprintf(buf, sizeof(buf), "%.2f", g_refresh);
    vk_input_set_text(g_set_in[2], buf);
    vk_input_set_border_style(g_set_in[2], VK_BORDER_SINGLE);
    mf_ui_attach(VK_WIDGET(g_set_in[2]), x + 14, y + 8);

    g_set_lab[3] = vk_label_create(w - 4);
    vk_label_set_text(g_set_lab[3], "OK / Cancel");
    mf_ui_attach(VK_WIDGET(g_set_lab[3]), x + 2, y + h - 2);

    {
        int i;
        for (i = 0; i < 3; i++) {
            vk_widget_set_colors(VK_WIDGET(g_set_lab[i]), COL_TEXT, COL_MENU);
            vk_label_update(g_set_lab[i]);
            vk_widget_set_colors(VK_WIDGET(g_set_in[i]), COL_TEXT, COL_BG);
            vk_input_show_cursor(g_set_in[i], i == 0);
            vk_input_update(g_set_in[i]);
        }
        vk_widget_set_colors(VK_WIDGET(g_set_lab[3]), COL_TEXT, COL_MENU);
        vk_label_update(g_set_lab[3]);
    }
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
        vk_input_show_cursor(g_set_in[g_set_focus], false);
        vk_input_update(g_set_in[g_set_focus]);
        g_set_focus = (g_set_focus + 1) % 3;
        vk_input_show_cursor(g_set_in[g_set_focus], true);
        vk_input_update(g_set_in[g_set_focus]);
        mf_ui_refresh();
        return 1;
    }
    if (c == '\n' || c == KEY_ENTER) {
        apply_settings();
        return 1;
    }
    in = g_set_in[g_set_focus];
    if (c == KEY_BACKSPACE || c == 127) {
        vk_input_backspace(in);
        vk_input_update(in);
        mf_ui_refresh();
        return 1;
    }
    if (c == KEY_LEFT) {
        vk_input_move_cursor(in, -1);
        vk_input_update(in);
        mf_ui_refresh();
        return 1;
    }
    if (c == KEY_RIGHT) {
        vk_input_move_cursor(in, 1);
        vk_input_update(in);
        mf_ui_refresh();
        return 1;
    }
    if (c >= 32 && c < 127) {
        vk_input_insert_char(in, (int)c);
        vk_input_update(in);
        mf_ui_refresh();
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
    vk_window_set_child(g_help_win, VK_WIDGET(lb));
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

void mf_ui_open_device_settings(int idx)
{
    const char *id = mf_dash_catalog_id(idx);
    const char *name = mf_dash_catalog_name(idx);
    if (!id || !id[0])
        return;
    mf_devset_show(id, name, 2.0);
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
            t - g_last_get >= g_refresh) {
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
                dirty = 1;
            }
            if (strcmp(tag, last_tag) != 0) {
                snprintf(last_tag, sizeof(last_tag), "%s", tag);
                dirty = 1;
            }
            if (dirty) {
                if (g_view_idx >= 0 && strstr(last_json, "\"data\"")) {
                    snprintf(g_view_json, sizeof(g_view_json), "%s", last_json);
                    mf_pack_update(g_view_json);
                } else {
                    mf_dash_update(g_hostport, tag,
                                   last_json[0] ? last_json : NULL);
                }
                mf_ui_refresh();
            }
        }

        if (t - g_cli.last_fresh > STALE_SECS && g_cli.state == MF_CONN_UP)
            g_cli.stale = 1;

        key = vk_kmio_fetch(&mev);
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
                    char path[192], payload[80];
                    snprintf(path, sizeof(path),
                             "/api/v1/devices/%s/settings", mf_devset_id());
                    snprintf(payload, sizeof(payload),
                             "{\"poll_interval_s\":%s}", mf_devset_poll_text());
                    mf_devset_close();
                    (void)mf_http_cli_put(&g_cli, path, payload);
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
            if (mf_pack_visible() && !mf_pack_is_charger() && mf_pack_has_switch()) {
                if (key == 'c' || key == 'C') {
                    mf_confirm_show(g_view_name, "charge");
                    continue;
                }
                if (key == 'd' || key == 'D') {
                    mf_confirm_show(g_view_name, "discharge");
                    continue;
                }
                if (key == 'b' || key == 'B') {
                    char path[192];
                    snprintf(path, sizeof(path),
                             "/api/v1/devices/%s/actions/set_switch",
                             mf_dash_catalog_id(g_view_idx));
                    (void)mf_http_cli_post(&g_cli, path,
                                           "{\"key\":\"balance\",\"value\":true}");
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
