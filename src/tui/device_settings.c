#include "ui_screen.h"
#include "layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vdk.h>
#include <ncursesw/curses.h>

#define COL_BG   COLOR_WHITE

#define COL_BG   COLOR_WHITE
#define COL_TEXT COLOR_BLACK
#define COL_MENU COLOR_CYAN

static vk_window_t *g_win;
static vk_label_t  *g_lab_poll, *g_lab_act;
static vk_input_t  *g_in_poll;
static int          g_open;
static char         g_name[32];
static char         g_id[40];

static vk_window_t *g_cf_win;
static vk_label_t  *g_cf_l1, *g_cf_l2;
static int          g_cf_open;
static char         g_cf_key[16];

void mf_devset_close(void)
{
    if (!g_open)
        return;
    if (g_in_poll) {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_in_poll));
        vk_input_destroy(g_in_poll);
        g_in_poll = NULL;
    }
    if (g_lab_poll) {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_lab_poll));
        vk_label_destroy(g_lab_poll);
        g_lab_poll = NULL;
    }
    if (g_lab_act) {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_lab_act));
        vk_label_destroy(g_lab_act);
        g_lab_act = NULL;
    }
    if (g_win) {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_win));
        vk_window_destroy(g_win);
        g_win = NULL;
    }
    g_open = 0;
    mf_ui_refresh();
}

int mf_devset_open(void) { return g_open; }
const char *mf_devset_id(void) { return g_id; }
const char *mf_devset_poll_text(void)
{
    return g_in_poll ? vk_input_get_text(g_in_poll) : "2.0";
}

void mf_devset_show(const char *id, const char *name, double poll)
{
    int x, y, w, h;
    char cap[40], buf[32];

    mf_devset_close();
    snprintf(g_id, sizeof(g_id), "%s", id ? id : "");
    snprintf(g_name, sizeof(g_name), "%s", name ? name : "device");
    mf_tui_devsettings_geom(mf_ui_cols(), mf_ui_rows(), &x, &y, &w, &h);
    snprintf(cap, sizeof(cap), " %s ", g_name);
    g_win = vk_window_create(w, h);
    vk_window_set_title(g_win, cap);
    vk_window_set_border_style(g_win, VK_BORDER_SINGLE);
    vk_window_set_border_colors(g_win, COL_TEXT, COL_MENU);
    vk_widget_set_colors(VK_WIDGET(g_win), COL_TEXT, COL_MENU);
    mf_ui_attach(VK_WIDGET(g_win), x, y);
    vk_window_update(g_win);

    g_lab_poll = vk_label_create(18);
    vk_widget_set_colors(VK_WIDGET(g_lab_poll), COL_TEXT, COL_MENU);
    vk_label_set_text(g_lab_poll, "poll_interval_s");
    mf_ui_attach(VK_WIDGET(g_lab_poll), x + 2, y + 2);
    vk_label_update(g_lab_poll);

    g_in_poll = vk_input_create(12);
    snprintf(buf, sizeof(buf), "%.1f", poll > 0 ? poll : 2.0);
    vk_input_set_text(g_in_poll, buf);
    vk_input_set_border_style(g_in_poll, VK_BORDER_SINGLE);
    vk_widget_set_colors(VK_WIDGET(g_in_poll), COL_TEXT, COL_BG);
    mf_ui_attach(VK_WIDGET(g_in_poll), x + 22, y + 2);
    vk_input_show_cursor(g_in_poll, true);
    vk_input_update(g_in_poll);

    g_lab_act = vk_label_create(w - 4);
    vk_widget_set_colors(VK_WIDGET(g_lab_act), COL_TEXT, COL_MENU);
    vk_label_set_text(g_lab_act, "Save / Esc");
    mf_ui_attach(VK_WIDGET(g_lab_act), x + 2, y + h - 2);
    vk_label_update(g_lab_act);

    g_open = 1;
    mf_ui_front_clear();
    mf_ui_front_push(VK_WIDGET(g_win));
    mf_ui_refresh();
}

int mf_devset_key(wint_t c)
{
    if (!g_open)
        return 0;
    if (c == 27 || c == KEY_EXIT || c == KEY_CANCEL) {
        mf_devset_close();
        return 1;
    }
    if (c == '\n' || c == KEY_ENTER)
        return 2; /* save */
    if (c == KEY_BACKSPACE || c == 127) {
        vk_input_backspace(g_in_poll);
        vk_input_update(g_in_poll);
        mf_ui_refresh();
        return 1;
    }
    if (c == KEY_LEFT) {
        vk_input_move_cursor(g_in_poll, -1);
        vk_input_update(g_in_poll);
        mf_ui_refresh();
        return 1;
    }
    if (c == KEY_RIGHT) {
        vk_input_move_cursor(g_in_poll, 1);
        vk_input_update(g_in_poll);
        mf_ui_refresh();
        return 1;
    }
    if (c >= 32 && c < 127) {
        vk_input_insert_char(g_in_poll, (int)c);
        vk_input_update(g_in_poll);
        mf_ui_refresh();
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
    vk_window_set_border_colors(g_cf_win, COL_TEXT, COL_MENU);
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

/* Left-press mask. */
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

    /* Click inside confirm window: check if on "y / n" label row. */
    if (bstate & LEFT) {
        lx = x - win_x;
        ly = y - win_y;
        /* y / n label is at canvas y ≈ 4, spans interior width (win_w - 4). */
        if (ly >= 3 && ly <= 5) {
            int mid = win_w / 2;
            if (lx < mid)
                return 2;
            mf_confirm_close();
            return 1;
        }
    }

    /* Absorb clicks over confirm dialog. */
    return 1;
}

int
mf_devset_mouse(int x, int y, mmask_t bstate)
{
    int win_x, win_y, win_w, win_h;

    if (!g_open || !g_win)
        return 0;

    vk_widget_get_position(VK_WIDGET(g_win), &win_x, &win_y);
    vk_widget_get_metrics(VK_WIDGET(g_win), &win_w, &win_h);
    if (x < win_x || y < win_y || x >= win_x + win_w || y >= win_y + win_h)
        return 0;

    /* Absorb clicks over device settings dialog. */
    (void)bstate;
    return 1;
}
