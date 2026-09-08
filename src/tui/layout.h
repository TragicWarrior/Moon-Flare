#ifndef MF_TUI_LAYOUT_H
#define MF_TUI_LAYOUT_H

#define MF_TUI_COLS      80
#define MF_TUI_ROWS      25
#define MF_MENUBAR_H     1
#define MF_CARD_W        26
#define MF_CARD_H        22
#define MF_CARD_Y        2
#define MF_SETTINGS_W    60
#define MF_SETTINGS_H    18

int mf_tui_dropdown_max_h(int lines);
int mf_tui_dropdown_max_w(int cols);
void mf_tui_settings_geom(int cols, int rows, int *x, int *y, int *w, int *h);

/* Paint a 80x25 ASCII dashboard. grid[r] is a 81-byte NUL-terminated row. */
void mf_tui_paint_dashboard(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1],
                            const char *hostport, const char *conn_tag,
                            int nbatt, const char *const *batt,
                            int nchg, const char *const *chg,
                            int ninv, const char *const *inv);

void mf_tui_paint_settings(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1],
                           const char *host, int port, double refresh_s);

#define MF_DEVSET_W  70
#define MF_DEVSET_H  22
#define MF_CONFIRM_W 50
#define MF_CONFIRM_H 7

void mf_tui_devsettings_geom(int cols, int rows, int *x, int *y, int *w, int *h);
void mf_tui_confirm_geom(int cols, int rows, int *x, int *y, int *w, int *h);

void mf_tui_paint_pack(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1], int has_switch);
void mf_tui_paint_charger(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1]);
void mf_tui_paint_devsettings(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1],
                              const char *name);
void mf_tui_paint_confirm(char grid[MF_TUI_ROWS][MF_TUI_COLS + 1],
                          const char *name);

#endif
