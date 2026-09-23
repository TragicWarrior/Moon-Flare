#include "ui_screen.h"
#include "layout.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vdk.h>

#define COL_BG   COLOR_WHITE
#define COL_TEXT COLOR_BLACK
#define COL_HI_FG COLOR_WHITE
#define COL_HI_BG COLOR_BLUE
#define COL_MENU_BG COLOR_CYAN
#define COL_DROP_FG COLOR_WHITE
#define COL_DROP_HI_FG COLOR_WHITE
#define COL_DROP_HI_BG COLOR_BLACK

enum { MB_FILE = 0, MB_DEVICES, MB_HELP, MB_COUNT };

struct mb_item {
    const char *label;
    void (*fn)(void);
    int end;
};

static vk_menubar_t *g_bar;
static vk_window_t  *g_drop;
static int           g_drop_idx = -1;
static int           g_focused;
static const struct mb_item *g_open_table;

static void on_quit(void)
{
    mf_ui_quit();
}
static void on_general(void)
{
    mf_ui_open_settings();
}
static void on_connections(void)
{
    mf_ui_open_connections();
}
static void on_save(void)
{
    mf_ui_save_config();
}
static void on_load(void)
{
    mf_ui_load_config();
}
static void on_dash(void)
{
    mf_ui_show_dashboard();
}
static void on_keys(void)
{
    mf_ui_show_help(0);
}
static void on_about(void)
{
    mf_ui_show_help(1);
}
static void on_noop(void)
{
}

static int g_dyn_cat[32];

static void on_dyn_item(void)
{
    /* filled via on_drop_item index lookup below */
}

static const struct mb_item file_items[] = {
    { "General\u2026", on_general, 0 },
    { "Connections\u2026", on_connections, 0 },
    { NULL, NULL, 0 },
    { "Save config", on_save, 0 },
    { "Load config", on_load, 0 },
    { NULL, NULL, 0 },
    { "Quit", on_quit, 0 },
    { NULL, NULL, 1 }
};
static const struct mb_item devices_items[] = {
    { "Add Device\u2026", on_noop, 0 },
    { "Remove Device\u2026", on_noop, 0 },
    { NULL, NULL, 1 }
};
static const struct mb_item help_items[] = {
    { "Keyboard", on_keys, 0 },
    { "About", on_about, 0 },
    { NULL, NULL, 1 }
};

static const struct mb_item *const tables[MB_COUNT] = {
    file_items, devices_items, help_items
};
static const char *const titles[MB_COUNT] = {
    "File", "Devices", "Help"
};

static void close_dropdown(void)
{
    if (g_drop)
    {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_drop));
        vk_window_destroy(g_drop);
        g_drop = NULL;
        mf_ui_front_clear();
    }
    g_drop_idx = -1;
}

static int on_drop_item(vk_widget_t *w, void *idxp)
{
    int i = (int)(intptr_t)idxp;
    (void)w;
    close_dropdown();
    g_focused = 0;
    if (g_bar)
    {
        vk_menubar_set_focused(g_bar, false);
        vk_menubar_update(g_bar);
    }
    mf_ui_refresh();
    if (g_open_table && i >= 0 && g_open_table[i].fn == on_dyn_item)
    {
        mf_ui_open_device_view(g_dyn_cat[i]);
        return 0;
    }
    if (g_open_table && i >= 0 && g_open_table[i].fn)
        g_open_table[i].fn();
    return 0;
}

static void open_dropdown(int idx)
{
    const struct mb_item *t;
    vk_listbox_t *lb;
    vk_window_t *win;
    int i, n = 0, max_w = 16, max_h, item_x, bar_x, bar_y;
    int cols = mf_ui_cols(), rows = mf_ui_rows();
    char cap[32];

    if (idx < 0 || idx >= MB_COUNT || !g_bar)
        return;
    close_dropdown();
    t = tables[idx];
    if (idx == MB_DEVICES)
    {
        static struct mb_item dyn[40];
        int nd = 0, c;
        memset(dyn, 0, sizeof(dyn));
        dyn[nd++] = (struct mb_item){ "Dashboard", on_dash, 0 };
        dyn[nd++] = (struct mb_item){ NULL, NULL, 0 };
        dyn[nd++] = (struct mb_item){ "Add Device…", on_noop, 0 };
        dyn[nd++] = (struct mb_item){ "Remove Device…", on_noop, 0 };
        dyn[nd++] = (struct mb_item){ NULL, NULL, 0 };
        for (c = 0; c < mf_dash_catalog_n() && nd < 34; c++)
        {
            const char *nm = mf_dash_catalog_name(c);
            if (!nm || !nm[0])
                continue;
            dyn[nd].label = nm;
            dyn[nd].fn = on_dyn_item;
            dyn[nd].end = 0;
            g_dyn_cat[nd] = c;
            nd++;
        }
        dyn[nd].end = 1;
        t = dyn;
    }
    g_open_table = t;
    for (i = 0; !t[i].end; i++)
    {
        if (t[i].label && t[i].fn)
        {
            int len = (int)strlen(t[i].label);
            if (len + 2 > max_w)
                max_w = len + 2;
            n++;
        } else if (t[i].label == NULL && t[i].fn == NULL)
            n++;
    }
    if (n < 1)
        n = 1;
    max_h = n;
    if (max_h > mf_tui_dropdown_max_h(rows))
        max_h = mf_tui_dropdown_max_h(rows);
    if (max_w > mf_tui_dropdown_max_w(cols))
        max_w = mf_tui_dropdown_max_w(cols);

    lb = vk_listbox_create(max_w, max_h);
    vk_listbox_set_wrap(lb, true);
    vk_listbox_set_highlight(lb, COL_DROP_HI_FG, COL_DROP_HI_BG);
    vk_listbox_set_highlight_attrs(lb, A_BOLD);
    vk_widget_set_colors(VK_WIDGET(lb), COL_DROP_FG, COL_MENU_BG);
    vk_widget_set_attrs(VK_WIDGET(lb), A_BOLD);
    for (i = 0; !t[i].end; i++)
    {
        if (!t[i].label && !t[i].fn)
            vk_listbox_add_separator(lb, VK_SEPARATOR_SINGLE);
        else if (t[i].label && t[i].fn)
            vk_listbox_add_item(lb, (char *)t[i].label, on_drop_item,
                                (void *)(intptr_t)i);
    }

    snprintf(cap, sizeof(cap), " %s ", titles[idx]);
    win = vk_window_create(max_w + 2, max_h + 2);
    vk_window_set_title(win, cap);
    vk_window_set_border_style(win, VK_BORDER_SINGLE);
    vk_window_set_border_colors(win, COL_DROP_FG, COL_MENU_BG);
    vk_window_set_border_attrs(win, A_BOLD);
    vk_widget_set_colors(VK_WIDGET(win), COL_DROP_FG, COL_MENU_BG);
    vk_widget_set_attrs(VK_WIDGET(win), A_BOLD);
    vk_window_set_child(win, VK_WIDGET(lb), VK_INHERIT_NONE);

    vk_widget_get_position(VK_WIDGET(g_bar), &bar_x, &bar_y);
    (void)bar_y;
    vk_menubar_get_item_position(g_bar, idx, &item_x);
    if (bar_x + item_x + max_w + 2 > cols)
        item_x = cols - max_w - 2 - bar_x;
    if (item_x < 0)
        item_x = 0;
    mf_ui_attach(VK_WIDGET(win), bar_x + item_x, MF_MENUBAR_H);
    vk_listbox_update(lb);
    vk_window_update(win);
    g_drop = win;
    g_drop_idx = idx;
    mf_ui_front_clear();
    mf_ui_front_push(VK_WIDGET(g_bar));
    mf_ui_front_push(VK_WIDGET(g_drop));
    mf_ui_refresh();
}

static int on_bar_activate(vk_widget_t *w, void *idxp)
{
    int idx = (int)(intptr_t)idxp;
    (void)w;
    if (g_drop && g_drop_idx == idx)
    {
        close_dropdown();
        mf_ui_refresh();
        return 0;
    }
    open_dropdown(idx);
    return 0;
}

void mf_menubar_init(void)
{
    int i, width = mf_ui_cols();
    if (width < 80)
        width = 80;
    g_bar = vk_menubar_create(width);
    vk_widget_set_colors(VK_WIDGET(g_bar), COL_TEXT, COL_BG);
    vk_menubar_set_highlight(g_bar, COL_HI_FG, COL_HI_BG);
    for (i = 0; i < MB_COUNT; i++)
        vk_menubar_add_item(g_bar, (char *)titles[i], on_bar_activate,
                            (void *)(intptr_t)i);
    mf_ui_attach(VK_WIDGET(g_bar), 0, 0);
    vk_menubar_set_focused(g_bar, false);
    vk_menubar_update(g_bar);
    g_focused = 0;
}

void mf_menubar_on_resize(void)
{
    int width = mf_ui_cols();
    close_dropdown();
    if (width < 80)
        width = 80;
    g_focused = 0;
    if (g_bar)
    {
        vk_widget_resize(VK_WIDGET(g_bar), width, 1);
        vk_menubar_set_focused(g_bar, false);
        vk_menubar_update(g_bar);
    }
}

void mf_menubar_shutdown(void)
{
    close_dropdown();
    if (g_bar)
    {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_bar));
        vk_menubar_destroy(g_bar);
        g_bar = NULL;
    }
}

int mf_menubar_active(void)
{
    return g_focused || g_drop != NULL;
}

int mf_menubar_key(wint_t c)
{
    vk_listbox_t *lb;
    if (c == KEY_F(10))
    {
        if (mf_menubar_active())
        {
            close_dropdown();
            g_focused = 0;
            if (g_bar)
            {
                vk_menubar_set_focused(g_bar, false);
                vk_menubar_update(g_bar);
            }
        }
        else
        {
            g_focused = 1;
            vk_menubar_set_focused(g_bar, true);
            if (vk_menubar_get_curr(g_bar) < 0)
                vk_menubar_set_curr(g_bar, 0);
            vk_menubar_update(g_bar);
        }
        mf_ui_refresh();
        return 1;
    }
    if (!mf_menubar_active())
        return 0;
    if (g_drop)
    {
        lb = VK_LISTBOX(vk_window_get_child(g_drop));
        if (c == 27)
        {
            close_dropdown();
            mf_ui_refresh();
            return 1;
        }
        if (c == KEY_UP && lb)
        {
            vk_listbox_set_prev(lb);
            vk_listbox_update(lb);
            vk_window_update(g_drop);
            mf_ui_refresh();
            return 1;
        }
        if (c == KEY_DOWN && lb)
        {
            vk_listbox_set_next(lb);
            vk_listbox_update(lb);
            vk_window_update(g_drop);
            mf_ui_refresh();
            return 1;
        }
        if ((c == '\n' || c == KEY_ENTER) && lb)
        {
            vk_listbox_exec_curr(lb);
            return 1;
        }
        if (c == KEY_LEFT)
        {
            close_dropdown();
            vk_menubar_set_prev(g_bar);
            vk_menubar_update(g_bar);
            open_dropdown(vk_menubar_get_curr(g_bar));
            return 1;
        }
        if (c == KEY_RIGHT)
        {
            close_dropdown();
            vk_menubar_set_next(g_bar);
            vk_menubar_update(g_bar);
            open_dropdown(vk_menubar_get_curr(g_bar));
            return 1;
        }
        return 1;
    }
    if (c == 27)
    {
        g_focused = 0;
        vk_menubar_set_focused(g_bar, false);
        vk_menubar_update(g_bar);
        mf_ui_refresh();
        return 1;
    }
    if (c == KEY_LEFT)
    {
        vk_menubar_set_prev(g_bar);
        vk_menubar_update(g_bar);
        mf_ui_refresh();
        return 1;
    }
    if (c == KEY_RIGHT)
    {
        vk_menubar_set_next(g_bar);
        vk_menubar_update(g_bar);
        mf_ui_refresh();
        return 1;
    }
    if (c == KEY_DOWN || c == '\n' || c == KEY_ENTER)
    {
        open_dropdown(vk_menubar_get_curr(g_bar));
        return 1;
    }
    return 1;
}

int mf_menubar_is_init(void)
{
    return g_bar != NULL;
}

/* Left-press mask (pressed / clicked / double-clicked). */
#define LEFT (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)

int
mf_menubar_mouse(int x, int y, mmask_t bstate)
{
    int bar_x, bar_y, bar_w, bar_h;
    int idx, lx;
    int dx, dy, dw, dh;
    vk_listbox_t *lb;
    int ly, scroll, n, row;

    /* Wheel over open dropdown → move selection. */
    if (g_drop && (bstate & (BUTTON4_PRESSED | BUTTON5_PRESSED)))
    {
        vk_widget_get_position(VK_WIDGET(g_drop), &dx, &dy);
        vk_widget_get_metrics(VK_WIDGET(g_drop), &dw, &dh);
        if (x >= dx && x < dx + dw && y >= dy && y < dy + dh)
        {
            lb = VK_LISTBOX(vk_window_get_child(g_drop));
            if (lb)
            {
                if (bstate & BUTTON4_PRESSED)
                    vk_listbox_set_prev(lb);
                else
                    vk_listbox_set_next(lb);
                vk_listbox_update(lb);
                vk_window_update(g_drop);
                mf_ui_refresh();
            }
            return 1;
        }
    }

    if (!(bstate & LEFT))
        return 0;

    /* Click on open dropdown list → select and activate. */
    if (g_drop)
    {
        vk_widget_get_position(VK_WIDGET(g_drop), &dx, &dy);
        vk_widget_get_metrics(VK_WIDGET(g_drop), &dw, &dh);
        if (x >= dx && x < dx + dw && y >= dy && y < dy + dh)
        {
            lb = VK_LISTBOX(vk_window_get_child(g_drop));
            /* Interior of window frame: inset 1 for border. */
            ly = y - dy - 1;
            if (lb && ly >= 0)
            {
                n = vk_listbox_get_item_count(lb);
                scroll = vk_listbox_get_scroll_pos(lb);
                row = scroll + ly;
                if (row >= 0 && row < n)
                {
                    vk_listbox_set_curr(lb, row);
                    vk_listbox_update(lb);
                    vk_window_update(g_drop);
                    vk_listbox_exec_curr(lb);
                    return 1;
                }
            }
            return 1;
        }
        /* Click outside dropdown while open → close it. */
        close_dropdown();
    }

    /* Check if click lands on the menubar bar itself. */
    if (!g_bar)
        return 0;

    vk_widget_get_position(VK_WIDGET(g_bar), &bar_x, &bar_y);
    vk_widget_get_metrics(VK_WIDGET(g_bar), &bar_w, &bar_h);
    if (y < bar_y || y >= bar_y + bar_h || x < bar_x || x >= bar_x + bar_w)
    {
        if (mf_menubar_active())
        {
            g_focused = 0;
            vk_menubar_set_focused(g_bar, false);
            vk_menubar_update(g_bar);
            mf_ui_refresh();
            return 1;
        }
        return 0;
    }

    lx = x - bar_x;
    idx = vk_menubar_hit_test(g_bar, lx);
    if (idx < 0)
    {
        if (!mf_menubar_active())
        {
            g_focused = 1;
            vk_menubar_set_focused(g_bar, true);
            if (vk_menubar_get_curr(g_bar) < 0)
                vk_menubar_set_curr(g_bar, 0);
            vk_menubar_update(g_bar);
            mf_ui_refresh();
        }
        return 1;
    }

    if (!mf_menubar_active())
    {
        g_focused = 1;
        vk_menubar_set_focused(g_bar, true);
    }
    vk_menubar_set_curr(g_bar, idx);
    vk_menubar_update(g_bar);
    open_dropdown(idx);
    return 1;
}
