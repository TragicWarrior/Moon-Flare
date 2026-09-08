#include "layout.h"

#include <stdio.h>
#include <string.h>

int mf_tui_dropdown_max_h(int lines)
{
    int h = lines - 4;
    if (h < 1)
        h = 1;
    return h;
}

int mf_tui_dropdown_max_w(int cols)
{
    int w = cols - 2;
    if (w < 16)
        w = 16;
    return w;
}

void mf_tui_settings_geom(int cols, int rows, int *x, int *y, int *w, int *h)
{
    *w = MF_SETTINGS_W;
    *h = MF_SETTINGS_H;
    if (*w > cols - 2)
        *w = cols - 2;
    if (*h > rows - 4)
        *h = rows - 4;
    *x = (cols - *w) / 2;
    *y = (rows - *h) / 2;
    if (*y < 3)
        *y = 3;
}

static void fill_blank(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1])
{
    int r, c;
    for (r = 0; r < MF_TUI_ROWS; r++) {
        for (c = 0; c < MF_TUI_COLS; c++)
            grid[r][c] = ' ';
        grid[r][MF_TUI_COLS] = '\0';
    }
}

static void put_str(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1],
                    int y, int x, const char *s)
{
    int i;
    if (y < 0 || y >= MF_TUI_ROWS)
        return;
    for (i = 0; s[i] && x + i < MF_TUI_COLS; i++)
        grid[y][x + i] = s[i];
}

static void draw_box(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1],
                     int y, int x, int w, int h, const char *title)
{
    int r, c;
    char cap[32];
    if (w < 3 || h < 2)
        return;
    for (c = 0; c < w && x + c < MF_TUI_COLS; c++) {
        grid[y][x + c] = (c == 0) ? '+' : (c == w - 1) ? '+' : '-';
        if (y + h - 1 < MF_TUI_ROWS)
            grid[y + h - 1][x + c] = (c == 0) ? '+' : (c == w - 1) ? '+' : '-';
    }
    for (r = 1; r < h - 1 && y + r < MF_TUI_ROWS; r++) {
        grid[y + r][x] = '|';
        if (x + w - 1 < MF_TUI_COLS)
            grid[y + r][x + w - 1] = '|';
    }
    snprintf(cap, sizeof(cap), " %s ", title ? title : "");
    put_str(grid, y, x + 2, cap);
}

static void fill_card(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1],
                      int x, const char *title, int n, const char *const *lines)
{
    int i;
    draw_box(grid, MF_CARD_Y, x, MF_CARD_W, MF_CARD_H, title);
    if (n <= 0)
        put_str(grid, MF_CARD_Y + 1, x + 2, "not connected");
    else {
        for (i = 0; i < n && i < MF_CARD_H - 2; i++)
            put_str(grid, MF_CARD_Y + 1 + i, x + 2, lines[i]);
    }
}

void mf_tui_paint_dashboard(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1],
                            const char *hostport, const char *conn_tag,
                            int nbatt, const char *const *batt,
                            int nchg, const char *const *chg,
                            int ninv, const char *const *inv)
{
    char t1[32], t2[32], t3[32], st[81];
    fill_blank(grid);
    put_str(grid, 0, 1, "File  Settings  Devices  View  Help");
    snprintf(st, sizeof(st), "%s  [%s]",
             hostport ? hostport : "127.0.0.1:5250",
             conn_tag ? conn_tag : "WAIT");
    put_str(grid, 1, 0, st);
    snprintf(t1, sizeof(t1), "Batteries (%d)", nbatt);
    snprintf(t2, sizeof(t2), "Chargers (%d)", nchg);
    snprintf(t3, sizeof(t3), "Inverters (%d)", ninv);
    fill_card(grid, 0, t1, nbatt, batt);
    fill_card(grid, 27, t2, nchg, chg);
    fill_card(grid, 54, t3, ninv, inv);
    put_str(grid, 24, 0, "F10 menu  Enter open  q quit");
}

void mf_tui_paint_settings(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1],
                           const char *host, int port, double refresh_s)
{
    int x, y, w, h;
    char line[72];
    mf_tui_paint_dashboard(grid, "127.0.0.1:5250", "UP", 0, NULL, 0, NULL, 0, NULL);
    mf_tui_settings_geom(MF_TUI_COLS, MF_TUI_ROWS, &x, &y, &w, &h);
    draw_box(grid, y, x, w, h, "General");
    put_str(grid, y + 2, x + 2, "Host");
    snprintf(line, sizeof(line), "[%s]", host ? host : "127.0.0.1");
    put_str(grid, y + 2, x + 16, line);
    put_str(grid, y + 4, x + 2, "Port");
    snprintf(line, sizeof(line), "[%d]", port);
    put_str(grid, y + 4, x + 16, line);
    put_str(grid, y + 6, x + 2, "Refresh s");
    snprintf(line, sizeof(line), "[%.2f]", refresh_s);
    put_str(grid, y + 6, x + 16, line);
    put_str(grid, y + h - 2, x + 2, "OK / Cancel");
}
