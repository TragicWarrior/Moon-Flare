#include "layout.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

int main(void)
{
    char grid[MF_TUI_ROWS][MF_TUI_COLS + 1];
    int r, x, y, w, h;

    CHECK(mf_tui_dropdown_max_h(25) == 21, "dropdown max_h LINES-4");
    CHECK(mf_tui_dropdown_max_w(80) == 78, "dropdown max_w COLS-2");
    mf_tui_settings_geom(80, 25, &x, &y, &w, &h);
    CHECK(w == 60 && h == 18, "settings 18x60");
    CHECK(y >= 3 && y + h <= 25, "settings fits 80x25");
    CHECK(x >= 0 && x + w <= 80, "settings x in 80");

    mf_tui_paint_dashboard(grid, "127.0.0.1:5250", "UP", 0, NULL, 0, NULL, 0, NULL);
    for (r = 0; r < MF_TUI_ROWS; r++)
        CHECK(strlen(grid[r]) == 80, "row width 80");
    CHECK(strstr(grid[0], "File") && strstr(grid[0], "Settings") &&
          strstr(grid[0], "Devices") && strstr(grid[0], "View") &&
          strstr(grid[0], "Help"), "menubar labels");
    CHECK(strstr(grid[1], "127.0.0.1:5250") && strstr(grid[1], "[UP]"),
          "status line");
    {
        int found_b = 0, found_c = 0, found_i = 0, found_nc = 0, found_small = 0;
        for (r = 0; r < MF_TUI_ROWS; r++) {
            if (strstr(grid[r], "Batteries"))
                found_b = 1;
            if (strstr(grid[r], "Chargers"))
                found_c = 1;
            if (strstr(grid[r], "Inverters"))
                found_i = 1;
            if (strstr(grid[r], "not connected"))
                found_nc = 1;
            if (strstr(grid[r], "too small") || strstr(grid[r], "72x22"))
                found_small = 1;
        }
        CHECK(found_b && found_c && found_i, "three cards");
        CHECK(found_nc, "empty inverters not connected");
        CHECK(!found_small, "no 72x22 too-small path");
    }
    CHECK(strstr(grid[24], "F10"), "hints on row 24");

    mf_tui_paint_settings(grid, "127.0.0.1", 5250, 1.0);
    CHECK(strstr(grid[y], "General"), "settings title");
    CHECK(strstr(grid[y + h - 2], "OK") && strstr(grid[y + h - 2], "Cancel"),
          "OK/Cancel on last interior row");
    CHECK(y + h - 1 < MF_TUI_ROWS, "settings bottom visible");

    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("tui_layout: ok\n");
    return 0;
}
