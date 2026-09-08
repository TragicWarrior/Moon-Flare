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

    mf_tui_paint_pack(grid, 1);
    for (r = 0; r < MF_TUI_ROWS; r++) {
        CHECK(strlen(grid[r]) == 80, "pack row width 80");
        CHECK(!strstr(grid[r], "72x22") && !strstr(grid[r], "too small"),
              "pack no 72x22");
    }
    CHECK(strstr(grid[0], "File"), "pack menubar");
    CHECK(strstr(grid[3], "Pack"), "pack frame");
    CHECK(strstr(grid[9], "Cells"), "cells frame");
    CHECK(strstr(grid[6], "MOS") && strstr(grid[6], "T1"), "temp line");
    CHECK(strstr(grid[7], "CHG"), "MOSFET line");
    CHECK(strstr(grid[10], "01 3.32"), "cell 1 voltage");
    CHECK(strstr(grid[24], "Esc dashboard"), "pack hints row 24");
    CHECK(strstr(grid[14], "dV") || strstr(grid[14], "mV"), "spread on row 14");
    {
        int overflow = 0;
        for (r = 16; r <= 23; r++) {
            if (strstr(grid[r], "Pack") || strstr(grid[r], "Cells"))
                overflow = 1;
        }
        CHECK(!overflow, "pack frames do not occupy rows 16-23");
    }

    mf_tui_paint_charger(grid);
    CHECK(strstr(grid[3], "Classic"), "charger frame");
    CHECK(strstr(grid[7], "Absorb") || strstr(grid[7], "stage"), "stage");
    CHECK(strstr(grid[24], "Esc dashboard"), "charger hints");
    for (r = 0; r < MF_TUI_ROWS; r++)
        CHECK(strlen(grid[r]) == 80, "charger row width 80");

    mf_tui_devsettings_geom(80, 25, &x, &y, &w, &h);
    CHECK(w == 70 && h == 22, "device settings 22x70");
    CHECK(y == 2 && y + h <= 25, "device settings fits");
    mf_tui_paint_devsettings(grid, "pack-demo");
    CHECK(strstr(grid[y + 2], "poll_interval_s"), "poll first");
    CHECK(strstr(grid[y + h - 2], "Save") && strstr(grid[y + h - 2], "Esc"),
          "Save/Esc pinned");

    mf_tui_confirm_geom(80, 25, &x, &y, &w, &h);
    CHECK(w == 50 && h == 7, "confirm 7x50");
    CHECK(y >= 1 && y + h <= 24, "confirm below menubar above hints");
    mf_tui_paint_confirm(grid, "pack-demo");
    CHECK(strstr(grid[0], "File"), "confirm keeps menubar");
    CHECK(strstr(grid[y], "pack-demo") || strstr(grid[y], "pack"), "confirm title");
    CHECK(strstr(grid[y + 4], "y") && strstr(grid[y + 4], "n"), "y/n visible");

    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("tui_layout: ok\n");
    return 0;
}
