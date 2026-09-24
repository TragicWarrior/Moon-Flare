/* A small modal list: pick one row with Up/Down + Enter, Esc cancels.
 * Used by Modules -> Add Module (pick a driver) and Remove Module (pick a
 * module).  Styled like the Connections dialog. */

#include "ui_screen.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vdk.h>

#define COL_TEXT COLOR_BLACK
#define COL_MENU COLOR_CYAN
#define PICK_MAX 32

static vk_window_t  *g_win;
static vk_box_t     *g_vbox;
static vk_listbox_t *g_list;
static vk_label_t   *g_spacer;
static vk_label_t   *g_hint;
static int           g_open;
static int           g_n;
static int           g_sel;
static char          g_tag[16];

void mf_picker_close(void)
{
    int i;

    if (!g_open)
        return;
    if (g_win)
        vk_window_set_child(g_win, NULL, VK_INHERIT_NONE);
    if (g_vbox)
    {
        for (i = 0; i < 3; i++)
            vk_box_set_widget(g_vbox, i, NULL, VK_INHERIT_NONE);
        vk_box_destroy(g_vbox);
        g_vbox = NULL;
    }
    if (g_list)
    {
        vk_listbox_destroy(g_list);
        g_list = NULL;
    }
    if (g_spacer)
    {
        vk_label_destroy(g_spacer);
        g_spacer = NULL;
    }
    if (g_hint)
    {
        vk_label_destroy(g_hint);
        g_hint = NULL;
    }
    if (g_win)
    {
        vk_screen_detach_widget(mf_ui_screen(), 0, VK_WIDGET(g_win));
        vk_window_destroy(g_win);
        g_win = NULL;
    }
    g_open = 0;
    mf_ui_front_clear();
    mf_ui_refresh();
}

int mf_picker_open(void)
{
    return g_open;
}

const char *mf_picker_tag(void)
{
    return g_tag;
}

int mf_picker_index(void)
{
    return g_sel;
}

static vk_label_t *mk_line(int w, const char *text)
{
    vk_label_t *l = vk_label_create(w);

    vk_widget_set_colors(VK_WIDGET(l), COL_TEXT, COL_MENU);
    vk_label_set_text(l, text ? text : "");
    vk_label_update(l);
    return l;
}

/* rows may be empty: the list then shows `empty` and Enter does nothing. */
void mf_picker_show(const char *tag, const char *title,
                    const char *const *rows, int n,
                    const char *hint, const char *empty)
{
    int cols = mf_ui_cols(), lines = mf_ui_rows();
    int i, len, inner_w, list_h, win_w, win_h, x, y;
    char cap[64];

    mf_picker_close();
    if (n > PICK_MAX)
        n = PICK_MAX;
    g_n = n;
    g_sel = 0;
    snprintf(g_tag, sizeof(g_tag), "%s", tag ? tag : "");

    inner_w = hint ? (int)strlen(hint) : 0;
    for (i = 0; i < n; i++)
    {
        len = (int)strlen(rows[i]);
        if (len > inner_w)
            inner_w = len;
    }
    if (n == 0 && empty && (int)strlen(empty) > inner_w)
        inner_w = (int)strlen(empty);
    inner_w += 2;                       /* listbox side padding */
    if (inner_w < 28)
        inner_w = 28;
    if (inner_w > cols - 4)
        inner_w = cols - 4;
    list_h = n < 1 ? 1 : n;
    if (list_h > lines - 8)
        list_h = lines - 8;
    if (list_h < 1)
        list_h = 1;
    win_w = inner_w + 2;
    win_h = list_h + 1 /* spacer */ + 1 /* hint */ + 2 /* borders */;
    x = (cols - win_w) / 2;
    y = (lines - win_h) / 2;
    if (x < 0)
        x = 0;
    if (y < 1)
        y = 1;

    snprintf(cap, sizeof(cap), " %s ", title ? title : "");
    g_win = vk_window_create(win_w, win_h);
    vk_window_set_title(g_win, cap);
    vk_window_set_border_style(g_win, VK_BORDER_SINGLE);
    vk_window_set_border_colors(g_win, COLOR_WHITE, COL_MENU);
    vk_window_set_border_attrs(g_win, A_BOLD);
    vk_widget_set_colors(VK_WIDGET(g_win), COL_TEXT, COL_MENU);

    g_list = vk_listbox_create(inner_w, list_h);
    vk_listbox_set_wrap(g_list, true);
    vk_listbox_set_highlight(g_list, COLOR_WHITE, COLOR_BLACK);
    vk_listbox_set_highlight_attrs(g_list, A_BOLD);
    vk_widget_set_colors(VK_WIDGET(g_list), COLOR_WHITE, COLOR_CYAN);
    vk_widget_set_attrs(VK_WIDGET(g_list), A_BOLD);
    vk_widget_set_expand(VK_WIDGET(g_list));
    if (n == 0)
        vk_listbox_add_item(g_list, (char *)(empty ? empty : "(none)"),
                            NULL, NULL);
    for (i = 0; i < n; i++)
        vk_listbox_add_item(g_list, (char *)rows[i], NULL,
                            (void *)(intptr_t)i);

    g_spacer = mk_line(inner_w, "");
    g_hint = mk_line(inner_w, hint);

    g_vbox = vk_box_create(inner_w, list_h + 2, VK_BOX_VERTICAL, 3);
    vk_box_set_homogeneous(g_vbox, false);
    vk_widget_set_colors(VK_WIDGET(g_vbox), COL_TEXT, COL_MENU);
    vk_widget_set_expand(VK_WIDGET(g_vbox));
    vk_box_set_widget(g_vbox, 0, VK_WIDGET(g_list), VK_INHERIT_NONE);
    vk_box_set_widget(g_vbox, 1, VK_WIDGET(g_spacer), VK_INHERIT_NONE);
    vk_box_set_widget(g_vbox, 2, VK_WIDGET(g_hint), VK_INHERIT_NONE);

    vk_window_set_child(g_win, VK_WIDGET(g_vbox), VK_INHERIT_NONE);
    mf_ui_attach(VK_WIDGET(g_win), x, y);
    vk_listbox_set_curr(g_list, 0);
    vk_listbox_update(g_list);
    vk_box_update(g_vbox);
    vk_window_update(g_win);
    g_open = 1;
    mf_ui_front_clear();
    mf_ui_front_push(VK_WIDGET(g_win));
    mf_ui_refresh();
}

static void move_sel(int d)
{
    int s = g_sel + d;

    if (s < 0 || s >= g_n)
        return;
    g_sel = s;
    vk_listbox_set_curr(g_list, g_sel);
    /* Recomposite every level: the list sits in a box inside the window,
       and a container only picks up a child's new pixels on its own update. */
    vk_listbox_update(g_list);
    vk_box_update(g_vbox);
    vk_window_update(g_win);
    mf_ui_refresh();
}

int mf_picker_key(wint_t c)
{
    if (!g_open)
        return MF_PICK_NONE;
    if (c == 27 || c == KEY_EXIT || c == KEY_CANCEL)
    {
        mf_picker_close();
        return MF_PICK_CANCEL;
    }
    if (c == KEY_UP || c == KEY_BTAB)
        move_sel(-1);
    else if (c == KEY_DOWN || c == '\t')
        move_sel(1);
    else if ((c == '\n' || c == KEY_ENTER) && g_n > 0)
        return MF_PICK_CHOSEN;
    return MF_PICK_HANDLED;
}
