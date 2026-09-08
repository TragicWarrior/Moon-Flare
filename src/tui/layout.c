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

void mf_tui_devsettings_geom(int cols, int rows, int *x, int *y, int *w, int *h)
{
    *w = MF_DEVSET_W;
    *h = MF_DEVSET_H;
    if (*w > cols - 2)
        *w = cols - 2;
    if (*h > rows - 3)
        *h = rows - 3;
    *x = (cols - *w) / 2;
    *y = 2;
    if (*y + *h > rows)
        *y = rows - *h;
}

void mf_tui_confirm_geom(int cols, int rows, int *x, int *y, int *w, int *h)
{
    *w = MF_CONFIRM_W;
    *h = MF_CONFIRM_H;
    if (*w > cols - 2)
        *w = cols - 2;
    if (*h > rows - 2)
        *h = rows - 2;
    *x = (cols - *w) / 2;
    *y = (rows - *h) / 2;
    if (*y < 1)
        *y = 1;
}

void mf_tui_paint_pack(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1], int has_switch)
{
    int r, c, i;
    char cell[20];
    fill_blank(grid);
    put_str(grid, 0, 1, "File  Settings  Devices  View  Help");
    put_str(grid, 1, 0, "pack-jk  battery/demo  streaming  seq 44");
    put_str(grid, 2, 0, "interface: bluetooth  28:D4:1E:A7:23:39");
    draw_box(grid, 3, 0, 80, 6, "Pack");
    put_str(grid, 4, 2, "Pack 53.2V            SOC 76%");
    put_str(grid, 5, 2, "Cap  -- Ah            -1.20 A");
    put_str(grid, 6, 2, "MOS 32  T1 28  T2 27");
    if (has_switch)
        put_str(grid, 7, 2, "CHG on  DSG on  BAL on");
    else
        put_str(grid, 7, 2, "--");
    draw_box(grid, 9, 0, 80, 7, "Cells");
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            i = r * 4 + c + 1;
            snprintf(cell, sizeof(cell), "%02d 3.32", i);
            put_str(grid, 10 + r, 2 + c * 19, cell);
        }
    }
    put_str(grid, 14, 2, "dV 18 mV");
    put_str(grid, 24, 0, "c charge  d discharge  b balancer  Esc dashboard");
}

void mf_tui_paint_charger(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1])
{
    fill_blank(grid);
    put_str(grid, 0, 1, "File  Settings  Devices  View  Help");
    put_str(grid, 1, 0, "classic-1  charger/classic  streaming  seq 4");
    put_str(grid, 2, 0, "interface: tcp  172.16.0.20:502");
    draw_box(grid, 3, 0, 80, 13, "Classic");
    put_str(grid, 4, 2, "Batt 54.1V            PV -- V");
    put_str(grid, 5, 2, "Watts 840");
    put_str(grid, 7, 2, "stage Absorb");
    put_str(grid, 8, 2, "kWh today 3.2   Ah today --");
    put_str(grid, 10, 2, "FET --  Batt --  PCB --");
    put_str(grid, 24, 0, "Esc dashboard");
}

void mf_tui_paint_devsettings(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1],
                              const char *name)
{
    int x, y, w, h;
    char cap[40];
    mf_tui_paint_dashboard(grid, "127.0.0.1:5250", "UP", 0, NULL, 0, NULL, 0, NULL);
    mf_tui_devsettings_geom(MF_TUI_COLS, MF_TUI_ROWS, &x, &y, &w, &h);
    snprintf(cap, sizeof(cap), "%s", name ? name : "device");
    draw_box(grid, y, x, w, h, cap);
    put_str(grid, y + 2, x + 2, "poll_interval_s");
    put_str(grid, y + 2, x + 22, "[2.0]");
    put_str(grid, y + h - 2, x + 2, "Save / Esc");
}

void mf_tui_paint_confirm(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1],
                          const char *name)
{
    int x, y, w, h;
    char line[64];
    mf_tui_paint_pack(grid, 1);
    mf_tui_confirm_geom(MF_TUI_COLS, MF_TUI_ROWS, &x, &y, &w, &h);
    snprintf(line, sizeof(line), "%s", name ? name : "device");
    draw_box(grid, y, x, w, h, line);
    put_str(grid, y + 2, x + 2, "Turn off charge MOSFET?");
    put_str(grid, y + 4, x + 2, "y / n");
}
