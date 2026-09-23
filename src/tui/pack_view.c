#include "ui_screen.h"
#include "layout.h"

#include <cJSON.h>
#include <stdio.h>
#include <string.h>
#include <vdk.h>

#define COL_BG   COLOR_BLUE
#define COL_TEXT COLOR_WHITE
#define COL_TROUGH COLOR_BLACK /* dark trough; 8-color has no gray */
#define NCELL_SHOW 16
#define CELL_IDX_W 2
#define CELL_LAB_W 9
#define CELL_GAP   1
#define CELL_BAR_W 8
#define NAME_W     5
#define METER_W    22
#define VAL_W      10
#define LEFT_X     2
#define RIGHT_X    42

static int g_kind; /* 0 pack, 1 charger */
static vk_label_t *g_chrome1, *g_chrome2, *g_hints;
static void pack_hints(void);
static int graph_bar_cells(int minutes);
static vk_window_t *g_fr_pack, *g_fr_cells, *g_fr_charger;
static vk_box_t *g_chg_box;
static vk_window_t *g_fr_prod;
static vk_box_t *g_prod_body;
static vk_meter_t *g_mt_pack, *g_mt_soc, *g_mt_batt, *g_mt_watts;
static vk_progress_t *g_pr_cap;
static vk_label_t *g_lb_npack, *g_lb_nsoc, *g_lb_ncap;
static vk_label_t *g_lb_vpack, *g_lb_vsoc, *g_lb_vcap;
static vk_label_t *g_lb_cur, *g_lb_temp, *g_lb_mos, *g_lb_spread;
static vk_label_t *g_lb_nbatt, *g_lb_nwatts, *g_lb_vbatt, *g_lb_vwatts;
static vk_label_t *g_lb_stage, *g_lb_energy, *g_lb_ctemp;
static vk_meter_t *g_mt_cell[NCELL_SHOW];
static vk_label_t *g_lb_cidx[NCELL_SHOW];
static vk_label_t *g_lb_cell[NCELL_SHOW];
static int g_visible;
static int g_has_switch;
static int g_chg_on, g_dsg_on, g_bal_on;
static vk_box_t *g_pack_body, *g_pack_row0, *g_pack_row1;
static vk_filler_t *g_pack_fill0, *g_pack_fill1;
static vk_box_t *g_cells_body, *g_cell_row[4], *g_cell_box[NCELL_SHOW];
static vk_box_t *g_cl_body, *g_cl_batt_row, *g_cl_watts_row;
static vk_filler_t *g_cl_rfill0, *g_cl_rfill1;
static vk_graph_t *g_cl_graph;
static vk_box_t *g_cl_graph_row;                 /* 1-cell L/R pad around graph */
static vk_filler_t *g_cl_pad_l, *g_cl_pad_r, *g_cl_pad_t, *g_cl_pad_b;
static vk_box_t *g_soc_body;                     /* vbox inside SOC History window */
static vk_graph_t *g_pk_graph;                   /* SOC chart (battery pack view) */
static vk_box_t *g_pk_graph_row;                 /* 1-cell L/R pad around SOC graph */
static vk_filler_t *g_pk_pad_l, *g_pk_pad_r, *g_pk_pad_t, *g_pk_pad_b;
static vk_frame_t *g_cf_batt, *g_cf_chg;   /* flat cyan/blue client frames */
static vk_window_t *g_fr_soc;                    /* SOC History window */
static vk_box_t *g_batt_box;               /* vbox holding Pack + Cells + SOC */
static char g_hist_id[64];

static void style_frame(vk_window_t *w)
{
    vk_widget_set_colors(VK_WIDGET(w), COL_TEXT, COL_BG);
    vk_widget_set_relief_colors(VK_WIDGET(w), COLOR_WHITE, COLOR_BLACK);
    vk_window_set_border_style(w, VK_BORDER_SINGLE | VK_RELIEF_SUNKEN);
    vk_window_set_border_colors(w, COL_TEXT, COL_BG);
}

static vk_label_t *mk_lab(int x, int y, int w)
{
    vk_label_t *l = vk_label_create(w);
    vk_widget_set_colors(VK_WIDGET(l), COL_TEXT, COL_BG);
    mf_ui_attach(VK_WIDGET(l), x, y);
    return l;
}

static vk_label_t *mk_lab_c(int w)
{
    vk_label_t *l = vk_label_create(w);
    return l;
}

static vk_label_t *mk_lab_txt_c(int w, const char *txt)
{
    vk_label_t *l = mk_lab_c(w);
    if (txt)
    {
        vk_label_set_text(l, txt);
        vk_label_update(l);
    }
    return l;
}

static vk_meter_t *mk_meter_c(int len, double lo, double hi)
{
    vk_meter_t *m = vk_meter_create(VK_PROGRESS_HORIZONTAL, len, 1);
    vk_progress_set_range(VK_PROGRESS(m), lo, hi);
    vk_progress_set_trough(VK_PROGRESS(m), VK_TROUGH_SOLID, COL_TROUGH, COL_BG);
    vk_progress_set_style(VK_PROGRESS(m), VK_PROGRESS_UNDERBAR);
    return m;
}

static void band_cell_v(vk_meter_t *m, double scale)
{
    vk_meter_clear_thresholds(m);
    vk_meter_add_threshold(m, 2.80 * scale, COLOR_YELLOW, COL_BG);
    vk_meter_add_threshold(m, 3.00 * scale, COLOR_GREEN, COL_BG);
    vk_meter_add_threshold(m, 3.45 * scale, COLOR_YELLOW, COL_BG);
    vk_meter_add_threshold(m, 3.60 * scale, COLOR_RED, COL_BG);
}

/* role: 0 default, 1 high (bright green), 2 low (bright yellow). */
static void style_cell_idx(vk_label_t *l, int role)
{
    if (!l)
        return;
    if (role == 1)
    {
        vk_widget_set_colors(VK_WIDGET(l), COLOR_GREEN, COL_BG);
        vk_widget_set_attrs(VK_WIDGET(l), A_BOLD);
    } else if (role == 2)
    {
        vk_widget_set_colors(VK_WIDGET(l), COLOR_YELLOW, COL_BG);
        vk_widget_set_attrs(VK_WIDGET(l), A_BOLD);
    }
    else
    {
        vk_widget_set_colors(VK_WIDGET(l), COL_TEXT, COL_BG);
        vk_widget_set_attrs(VK_WIDGET(l), A_NORMAL);
    }
}

static void band_pct(vk_meter_t *m)
{
    vk_meter_clear_thresholds(m);
    vk_meter_add_threshold(m, 10, COLOR_YELLOW, COL_BG);
    vk_meter_add_threshold(m, 25, COLOR_GREEN, COL_BG);
}

static void hide_w(vk_widget_t *w)
{
    if (w)
        vk_widget_set_state(w,
            (uint32_t)(vk_widget_get_state(w) & ~VK_STATE_VISIBLE));
}

static void show_w(vk_widget_t *w)
{
    if (w)
        vk_widget_show(w);
}

static void destroy_w(vk_widget_t *w)
{
    if (!w)
        return;
    vk_screen_detach_widget(mf_ui_screen(), 0, w);
    vk_widget_destroy(w);
}

void mf_pack_init(void)
{
    int i, r, c, cols = mf_ui_cols();
    int iw = cols > 4 ? cols - 4 : 76;

    g_chrome1 = mk_lab(0, 1, cols);
    g_chrome2 = mk_lab(0, 2, cols);
    g_fr_pack = vk_window_create(cols, 6);
    style_frame(g_fr_pack);
    vk_window_set_title(g_fr_pack, " Pack ");
    g_lb_npack = mk_lab_txt_c(NAME_W, "Pack");
    g_mt_pack = mk_meter_c(METER_W, 40.0, 58.4);
    band_cell_v(g_mt_pack, 16.0);
    g_lb_vpack = mk_lab_c(VAL_W);
    g_lb_nsoc = mk_lab_txt_c(NAME_W, "SOC");
    g_mt_soc = mk_meter_c(METER_W, 0.0, 100.0);
    band_pct(g_mt_soc);
    g_lb_vsoc = mk_lab_c(VAL_W);
    g_lb_ncap = mk_lab_txt_c(NAME_W, "Cap");
    g_pr_cap = vk_progress_create(VK_PROGRESS_HORIZONTAL, METER_W, 1);
    vk_progress_set_range(g_pr_cap, 0, 100);
    vk_progress_set_trough(g_pr_cap, VK_TROUGH_SOLID, COL_TROUGH, COL_BG);
    vk_progress_set_style(g_pr_cap, VK_PROGRESS_UNDERBAR);
    g_lb_vcap = mk_lab_c(VAL_W);
    g_lb_cur = mk_lab_c(30);
    g_lb_temp = mk_lab_c(iw);
    g_lb_mos = mk_lab_c(iw);

    g_pack_row0 = vk_box_create(cols - 2, 1, VK_BOX_HORIZONTAL, 7);
    vk_box_set_homogeneous(g_pack_row0, false);
    g_pack_fill0 = vk_filler_create();
    vk_widget_set_expand(VK_WIDGET(g_pack_fill0));

    g_pack_row1 = vk_box_create(cols - 2, 1, VK_BOX_HORIZONTAL, 5);
    vk_box_set_homogeneous(g_pack_row1, false);
    g_pack_fill1 = vk_filler_create();
    vk_widget_set_expand(VK_WIDGET(g_pack_fill1));

    g_pack_body = vk_box_create(cols - 2, 4, VK_BOX_VERTICAL, 4);
    vk_box_set_homogeneous(g_pack_body, false);
    vk_widget_set_expand(VK_WIDGET(g_pack_body));

    /* Top-down attach so the window's colours cascade to every descendant. */
    vk_window_set_child(g_fr_pack, VK_WIDGET(g_pack_body), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_body, 0, VK_WIDGET(g_pack_row0), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_body, 1, VK_WIDGET(g_pack_row1), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_body, 2, VK_WIDGET(g_lb_temp), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_body, 3, VK_WIDGET(g_lb_mos), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_row0, 0, VK_WIDGET(g_lb_npack), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_row0, 1, VK_WIDGET(g_mt_pack), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_row0, 2, VK_WIDGET(g_lb_vpack), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_row0, 3, VK_WIDGET(g_pack_fill0), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_row0, 4, VK_WIDGET(g_lb_nsoc), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_row0, 5, VK_WIDGET(g_mt_soc), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_row0, 6, VK_WIDGET(g_lb_vsoc), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_row1, 0, VK_WIDGET(g_lb_ncap), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_row1, 1, VK_WIDGET(g_pr_cap), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_row1, 2, VK_WIDGET(g_lb_vcap), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_row1, 3, VK_WIDGET(g_pack_fill1), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pack_row1, 4, VK_WIDGET(g_lb_cur), VK_INHERIT_COLOR);

    /* mk_lab_txt_c painted these static name labels before they inherited the
       window's colours (VK_INHERIT_COLOR only copies fg/bg, it doesn't repaint);
       repaint now so "Pack"/"SOC"/"Cap" pick up white-on-blue. */
    vk_label_update(g_lb_npack);
    vk_label_update(g_lb_nsoc);
    vk_label_update(g_lb_ncap);

    g_fr_cells = vk_window_create(cols, 7);
    style_frame(g_fr_cells);
    vk_window_set_title(g_fr_cells, " Cells ");
    for (i = 0; i < NCELL_SHOW; i++)
    {
        g_mt_cell[i] = mk_meter_c(CELL_BAR_W, 2.80, 3.65);
        band_cell_v(g_mt_cell[i], 1.0);
        g_lb_cidx[i] = mk_lab_c(CELL_IDX_W);
        g_lb_cell[i] = mk_lab_c(CELL_LAB_W - CELL_IDX_W);
        g_cell_box[i] = vk_box_create(iw / 4, 1, VK_BOX_HORIZONTAL, 3);
        vk_box_set_homogeneous(g_cell_box[i], false);
    }
    g_lb_spread = mk_lab_c(iw);
    for (r = 0; r < 4; r++)
    {
        g_cell_row[r] = vk_box_create(iw, 1, VK_BOX_HORIZONTAL, 4);
        vk_box_set_homogeneous(g_cell_row[r], false);
    }
    g_cells_body = vk_box_create(cols - 2, 5, VK_BOX_VERTICAL, 5);
    vk_box_set_homogeneous(g_cells_body, false);
    vk_widget_set_expand(VK_WIDGET(g_cells_body));

    /* Top-down attach so the window colours cascade to the whole cell tree. */
    vk_window_set_child(g_fr_cells, VK_WIDGET(g_cells_body), VK_INHERIT_COLOR);
    for (r = 0; r < 4; r++)
        vk_box_set_widget(g_cells_body, r, VK_WIDGET(g_cell_row[r]), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cells_body, 4, VK_WIDGET(g_lb_spread), VK_INHERIT_COLOR);
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            vk_box_set_widget(g_cell_row[r], c, VK_WIDGET(g_cell_box[r * 4 + c]),
                              VK_INHERIT_COLOR);
    for (i = 0; i < NCELL_SHOW; i++)
    {
        vk_box_set_widget(g_cell_box[i], 0, VK_WIDGET(g_lb_cidx[i]), VK_INHERIT_COLOR);
        vk_box_set_widget(g_cell_box[i], 1, VK_WIDGET(g_lb_cell[i]), VK_INHERIT_COLOR);
        vk_box_set_widget(g_cell_box[i], 2, VK_WIDGET(g_mt_cell[i]), VK_INHERIT_COLOR);
    }

    /* SOC History window: fixed height window with one EXPAND child (the graph). */
    g_fr_soc = vk_window_create(cols, 8);
    style_frame(g_fr_soc);
    vk_widget_set_expand(VK_WIDGET(g_fr_soc));
    vk_window_set_title(g_fr_soc, " SOC History ");
    g_soc_body = vk_box_create(cols - 2, 6, VK_BOX_VERTICAL, 3);
    vk_box_set_homogeneous(g_soc_body, false);
    vk_widget_set_expand(VK_WIDGET(g_soc_body));
    vk_window_set_child(g_fr_soc, VK_WIDGET(g_soc_body), VK_INHERIT_COLOR);

    g_fr_charger = vk_window_create(cols, 7);
    style_frame(g_fr_charger);
    vk_window_set_title(g_fr_charger, " Charger ");
    g_lb_nbatt = mk_lab_txt_c(NAME_W, "Batt");
    g_mt_batt = mk_meter_c(28, 40.0, 64.0);
    g_lb_vbatt = mk_lab_c(VAL_W);
    g_lb_nwatts = mk_lab_txt_c(NAME_W, "Watts");
    g_mt_watts = mk_meter_c(28, 0.0, 4000.0);
    g_lb_vwatts = mk_lab_c(VAL_W);
    g_lb_stage = mk_lab_c(iw);
    g_lb_energy = mk_lab_c(iw);
    g_lb_ctemp = mk_lab_c(iw);

    g_cl_batt_row = vk_box_create(cols - 2, 1, VK_BOX_HORIZONTAL, 4);
    vk_box_set_homogeneous(g_cl_batt_row, false);
    g_cl_rfill0 = vk_filler_create();
    vk_widget_set_expand(VK_WIDGET(g_cl_rfill0));
    g_cl_watts_row = vk_box_create(cols - 2, 1, VK_BOX_HORIZONTAL, 4);
    vk_box_set_homogeneous(g_cl_watts_row, false);
    g_cl_rfill1 = vk_filler_create();
    vk_widget_set_expand(VK_WIDGET(g_cl_rfill1));
    g_cl_graph = vk_graph_create(cols - 4, 8);
    vk_widget_set_colors(VK_WIDGET(g_cl_graph), COL_TEXT, COL_BG);
    vk_widget_set_expand(VK_WIDGET(g_cl_graph));
    vk_graph_set_bar_style(g_cl_graph, VK_GRAPH_BAR_BLOCK);
    vk_graph_set_bar_width(g_cl_graph, graph_bar_cells(30));
    vk_graph_set_y_range(g_cl_graph, 0.0, 100.0);
    vk_graph_set_colors(g_cl_graph, COLOR_MAGENTA, COL_BG);
    vk_graph_set_attrs(g_cl_graph, A_BOLD);

    /* 1-cell padding on all sides of the graph.  L/R: 1-wide fillers flank the
       EXPAND graph in a horizontal wrapper row.  T/B: 1-row fillers above and
       below that row in the body.  The wrapper and body box canvases are blue
       (COL_BG) and a vk_box fills its canvas with its bg every draw, so the
       reserved cells show as a uniform blue margin. */
    g_cl_pad_l = vk_filler_create();
    g_cl_pad_r = vk_filler_create();
    g_cl_pad_t = vk_filler_create();
    g_cl_pad_b = vk_filler_create();
    vk_widget_set_colors(VK_WIDGET(g_cl_pad_l), COL_TEXT, COL_BG);
    vk_widget_set_colors(VK_WIDGET(g_cl_pad_r), COL_TEXT, COL_BG);
    vk_widget_set_colors(VK_WIDGET(g_cl_pad_t), COL_TEXT, COL_BG);
    vk_widget_set_colors(VK_WIDGET(g_cl_pad_b), COL_TEXT, COL_BG);
    vk_widget_resize(VK_WIDGET(g_cl_pad_l), 1, 1);   /* reserve 1 col (L)       */
    vk_widget_resize(VK_WIDGET(g_cl_pad_r), 1, 1);   /* reserve 1 col (R)       */
    vk_widget_resize(VK_WIDGET(g_cl_pad_t), 1, 1);   /* reserve 1 row (top)     */
    vk_widget_resize(VK_WIDGET(g_cl_pad_b), 1, 1);   /* reserve 1 row (bottom)  */
    /* vk_filler_create() sets EXPAND -- an expand filler would split the box
       space evenly with the graph (each pad eating an equal share) instead of
       staying 1 cell.  Clear it so the pads are fixed and the graph gets all
       the leftover. */
    vk_widget_set_state(VK_WIDGET(g_cl_pad_l),
        (uint32_t)(vk_widget_get_state(VK_WIDGET(g_cl_pad_l)) & ~VK_STATE_EXPAND));
    vk_widget_set_state(VK_WIDGET(g_cl_pad_r),
        (uint32_t)(vk_widget_get_state(VK_WIDGET(g_cl_pad_r)) & ~VK_STATE_EXPAND));
    vk_widget_set_state(VK_WIDGET(g_cl_pad_t),
        (uint32_t)(vk_widget_get_state(VK_WIDGET(g_cl_pad_t)) & ~VK_STATE_EXPAND));
    vk_widget_set_state(VK_WIDGET(g_cl_pad_b),
        (uint32_t)(vk_widget_get_state(VK_WIDGET(g_cl_pad_b)) & ~VK_STATE_EXPAND));

    g_cl_graph_row = vk_box_create(cols - 2, 8, VK_BOX_HORIZONTAL, 3);
    vk_box_set_homogeneous(g_cl_graph_row, false);
    vk_widget_set_colors(VK_WIDGET(g_cl_graph_row), COL_TEXT, COL_BG);
    vk_widget_set_expand(VK_WIDGET(g_cl_graph_row));
    vk_box_set_widget(g_cl_graph_row, 0, VK_WIDGET(g_cl_pad_l), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cl_graph_row, 1, VK_WIDGET(g_cl_graph), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cl_graph_row, 2, VK_WIDGET(g_cl_pad_r), VK_INHERIT_COLOR);

    /* SOC chart graph: identical to charger but with "%" unit and 0-100 y-range. */
    g_pk_pad_l = vk_filler_create();
    g_pk_pad_r = vk_filler_create();
    g_pk_pad_t = vk_filler_create();
    g_pk_pad_b = vk_filler_create();
    vk_widget_set_colors(VK_WIDGET(g_pk_pad_l), COL_TEXT, COL_BG);
    vk_widget_set_colors(VK_WIDGET(g_pk_pad_r), COL_TEXT, COL_BG);
    vk_widget_set_colors(VK_WIDGET(g_pk_pad_t), COL_TEXT, COL_BG);
    vk_widget_set_colors(VK_WIDGET(g_pk_pad_b), COL_TEXT, COL_BG);
    vk_widget_resize(VK_WIDGET(g_pk_pad_l), 1, 1);
    vk_widget_resize(VK_WIDGET(g_pk_pad_r), 1, 1);
    vk_widget_resize(VK_WIDGET(g_pk_pad_t), 1, 1);
    vk_widget_resize(VK_WIDGET(g_pk_pad_b), 1, 1);
    vk_widget_set_state(VK_WIDGET(g_pk_pad_l),
        (uint32_t)(vk_widget_get_state(VK_WIDGET(g_pk_pad_l)) & ~VK_STATE_EXPAND));
    vk_widget_set_state(VK_WIDGET(g_pk_pad_r),
        (uint32_t)(vk_widget_get_state(VK_WIDGET(g_pk_pad_r)) & ~VK_STATE_EXPAND));
    vk_widget_set_state(VK_WIDGET(g_pk_pad_t),
        (uint32_t)(vk_widget_get_state(VK_WIDGET(g_pk_pad_t)) & ~VK_STATE_EXPAND));
    vk_widget_set_state(VK_WIDGET(g_pk_pad_b),
        (uint32_t)(vk_widget_get_state(VK_WIDGET(g_pk_pad_b)) & ~VK_STATE_EXPAND));

    g_pk_graph = vk_graph_create(cols - 4, 8);
    vk_widget_set_colors(VK_WIDGET(g_pk_graph), COL_TEXT, COL_BG);
    vk_widget_set_expand(VK_WIDGET(g_pk_graph));
    vk_graph_set_bar_style(g_pk_graph, VK_GRAPH_BAR_BLOCK);
    vk_graph_set_bar_width(g_pk_graph, graph_bar_cells(30));
    vk_graph_set_y_range(g_pk_graph, 0.0, 100.0);
    vk_graph_set_colors(g_pk_graph, COLOR_MAGENTA, COL_BG);
    vk_graph_set_attrs(g_pk_graph, A_BOLD);

    g_pk_graph_row = vk_box_create(cols - 2, 8, VK_BOX_HORIZONTAL, 3);
    vk_box_set_homogeneous(g_pk_graph_row, false);
    vk_widget_set_colors(VK_WIDGET(g_pk_graph_row), COL_TEXT, COL_BG);
    vk_widget_set_expand(VK_WIDGET(g_pk_graph_row));
    vk_box_set_widget(g_pk_graph_row, 0, VK_WIDGET(g_pk_pad_l), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pk_graph_row, 1, VK_WIDGET(g_pk_graph), VK_INHERIT_COLOR);
    vk_box_set_widget(g_pk_graph_row, 2, VK_WIDGET(g_pk_pad_r), VK_INHERIT_COLOR);

    /* Attach the padded chart into the SOC body: pad_t / graph_row / pad_b
       (mirrors the charger's g_cl_body).  This was missing, so the whole chart
       was orphaned and never composited into the SOC window. */
    vk_box_set_widget(g_soc_body, 0, VK_WIDGET(g_pk_pad_t), VK_INHERIT_COLOR);
    vk_box_set_widget(g_soc_body, 1, VK_WIDGET(g_pk_graph_row), VK_INHERIT_COLOR);
    vk_box_set_widget(g_soc_body, 2, VK_WIDGET(g_pk_pad_b), VK_INHERIT_COLOR);
    /* INHERIT_COLOR can clobber graph colors; restore them and set the SOC unit. */
    vk_graph_set_colors(g_pk_graph, COLOR_MAGENTA, COL_BG);
    vk_widget_set_colors(VK_WIDGET(g_pk_graph), COL_TEXT, COL_BG);
    vk_graph_set_unit_label(g_pk_graph, "%");
    vk_graph_set_unit_scale(g_pk_graph, 1.0);

    g_cl_body = vk_box_create(cols - 2, 5, VK_BOX_VERTICAL, 5);
    vk_box_set_homogeneous(g_cl_body, false);
    vk_widget_set_expand(VK_WIDGET(g_cl_body));

    vk_window_set_child(g_fr_charger, VK_WIDGET(g_cl_body), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cl_body, 0, VK_WIDGET(g_cl_batt_row), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cl_body, 1, VK_WIDGET(g_cl_watts_row), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cl_body, 2, VK_WIDGET(g_lb_stage), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cl_body, 3, VK_WIDGET(g_lb_energy), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cl_body, 4, VK_WIDGET(g_lb_ctemp), VK_INHERIT_COLOR);
    /* INHERIT_COLOR can clobber graph colors; restore explicit bar/box colors
        so werase draws on blue and bars use cyan on blue (not green on black). */
    vk_graph_set_colors(g_cl_graph, COLOR_MAGENTA, COL_BG);
    vk_widget_set_colors(VK_WIDGET(g_cl_graph), COL_TEXT, COL_BG);
    vk_graph_set_unit_label(g_cl_graph, "W");
    vk_graph_set_unit_scale(g_cl_graph, 1.0);

    /* Production History frame: EXPAND window with chart.
       Mirror g_fr_soc / g_soc_body from the battery side. */
    g_fr_prod = vk_window_create(cols, 8);
    style_frame(g_fr_prod);
    vk_widget_set_expand(VK_WIDGET(g_fr_prod));
    vk_window_set_title(g_fr_prod, " Production History ");
    g_prod_body = vk_box_create(cols - 2, 6, VK_BOX_VERTICAL, 3);
    vk_box_set_homogeneous(g_prod_body, false);
    vk_widget_set_expand(VK_WIDGET(g_prod_body));
    vk_window_set_child(g_fr_prod, VK_WIDGET(g_prod_body), VK_INHERIT_COLOR);
    vk_box_set_widget(g_prod_body, 0, VK_WIDGET(g_cl_pad_t), VK_INHERIT_COLOR);
    vk_box_set_widget(g_prod_body, 1, VK_WIDGET(g_cl_graph_row), VK_INHERIT_COLOR);
    vk_box_set_widget(g_prod_body, 2, VK_WIDGET(g_cl_pad_b), VK_INHERIT_COLOR);
    /* INHERIT_COLOR can clobber graph colors; restore them after attach. */
    vk_graph_set_colors(g_cl_graph, COLOR_MAGENTA, COL_BG);
    vk_widget_set_colors(VK_WIDGET(g_cl_graph), COL_TEXT, COL_BG);
    vk_graph_set_unit_label(g_cl_graph, "W");
    vk_graph_set_unit_scale(g_cl_graph, 1.0);
    vk_box_set_widget(g_cl_batt_row, 0, VK_WIDGET(g_lb_nbatt), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cl_batt_row, 1, VK_WIDGET(g_mt_batt), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cl_batt_row, 2, VK_WIDGET(g_lb_vbatt), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cl_batt_row, 3, VK_WIDGET(g_cl_rfill0), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cl_watts_row, 0, VK_WIDGET(g_lb_nwatts), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cl_watts_row, 1, VK_WIDGET(g_mt_watts), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cl_watts_row, 2, VK_WIDGET(g_lb_vwatts), VK_INHERIT_COLOR);
    vk_box_set_widget(g_cl_watts_row, 3, VK_WIDGET(g_cl_rfill1), VK_INHERIT_COLOR);

    /* Static name labels were painted by mk_lab_txt_c before inheriting; repaint. */
    vk_label_update(g_lb_nbatt);
    vk_label_update(g_lb_nwatts);

    /* Wrap the client areas in flat cyan/blue frames and reparent the windows.
       Top-down attach so the frame's colours cascade; the windows keep their own
       styling (INHERIT_NONE).  The charger box and battery box are EXPAND so the
       frame->box resize cascade sizes them; the charger's inner windows are sized
       explicitly in mf_pack_show (fixed + EXPAND pattern). */
    {
        int clientH = mf_ui_rows() - 4;   /* rows 3..rows-2: below chrome, above hints */

        if (clientH < 3)
            clientH = 3;
        g_cf_batt = mf_ui_make_client_frame(cols, clientH);
        g_batt_box = vk_box_create(cols - 2, clientH - 2, VK_BOX_VERTICAL, 3);
        vk_box_set_homogeneous(g_batt_box, false);
        vk_widget_set_expand(VK_WIDGET(g_batt_box));
        mf_ui_attach(VK_WIDGET(g_cf_batt), 0, 3);
        vk_frame_set_child(g_cf_batt, VK_WIDGET(g_batt_box), VK_INHERIT_COLOR);
        vk_box_set_widget(g_batt_box, 0, VK_WIDGET(g_fr_pack), VK_INHERIT_NONE);
        vk_box_set_widget(g_batt_box, 1, VK_WIDGET(g_fr_cells), VK_INHERIT_NONE);
        vk_box_set_widget(g_batt_box, 2, VK_WIDGET(g_fr_soc), VK_INHERIT_NONE);

        g_cf_chg = mf_ui_make_client_frame(cols, clientH);
        g_chg_box = vk_box_create(cols - 2, clientH - 2, VK_BOX_VERTICAL, 2);
        vk_box_set_homogeneous(g_chg_box, false);
        vk_widget_set_expand(VK_WIDGET(g_chg_box));
        mf_ui_attach(VK_WIDGET(g_cf_chg), 0, 3);
        vk_frame_set_child(g_cf_chg, VK_WIDGET(g_chg_box), VK_INHERIT_COLOR);
        vk_box_set_widget(g_chg_box, 0, VK_WIDGET(g_fr_charger), VK_INHERIT_NONE);
        vk_box_set_widget(g_chg_box, 1, VK_WIDGET(g_fr_prod), VK_INHERIT_NONE);
    }

    g_hints = mk_lab(0, 24, cols);
    mf_pack_hide();
}

void mf_pack_hide(void)
{
    hide_w(VK_WIDGET(g_chrome1));
    hide_w(VK_WIDGET(g_chrome2));
    if (g_cf_batt)
        hide_w(VK_WIDGET(g_cf_batt));
    if (g_cf_chg)
        hide_w(VK_WIDGET(g_cf_chg));
    hide_w(VK_WIDGET(g_hints));
    g_visible = 0;
}

static void show_pack_widgets(void)
{
    show_w(VK_WIDGET(g_chrome1));
    show_w(VK_WIDGET(g_chrome2));
    if (g_cf_batt)
        show_w(VK_WIDGET(g_cf_batt));
    show_w(VK_WIDGET(g_hints));
}

static void show_charger_widgets(void)
{
    show_w(VK_WIDGET(g_chrome1));
    show_w(VK_WIDGET(g_chrome2));
    if (g_cf_chg)
        show_w(VK_WIDGET(g_cf_chg));
    show_w(VK_WIDGET(g_hints));
}

void mf_pack_show(int charger)
{
    int i;
    mf_pack_hide();
    g_kind = charger ? 1 : 0;
     /* Size the client frames to the current terminal BEFORE the window bodies
        are rendered below, so the resize cascade (on_resize) grows the Pack/
        Cells/Charger windows first and they render at full size.  The frame is
        already at its target size on device open, so resize through a different
        height to force the cascade. */
    {
        int cols = mf_ui_cols();
        int rows = mf_ui_rows();
        int clientH = rows - 4;

        if (clientH < 3)
            clientH = 3;
        if (g_cf_batt)
        {
            vk_widget_resize(VK_WIDGET(g_cf_batt), cols, clientH + 1);
            vk_widget_resize(VK_WIDGET(g_cf_batt), cols, clientH);
        }
        if (g_cf_chg)
        {
            vk_widget_resize(VK_WIDGET(g_cf_chg), cols, clientH + 1);
            vk_widget_resize(VK_WIDGET(g_cf_chg), cols, clientH);
        }
        /* Keep the hint line full-width at the very bottom, below the frame.
           It is created at a fixed row and otherwise only repositioned on a
           resize event, so on a tall terminal opened directly it would sit
           inside the frame and punch through its left border. */
        if (g_hints)
        {
            vk_widget_resize(VK_WIDGET(g_hints), cols, 1);
            vk_widget_move(VK_WIDGET(g_hints), 0, rows - 1);
        }
        /* Reflow the row boxes to the window-interior width.  They are
           non-expand children of the (expand) body boxes, so the resize cascade
           never reaches them; without this they keep their creation width and,
           after a downsize, the right-aligned content (SOC meter, current, the
           rightmost cell column) overflows the body and is clipped. */
        {
            int bw = cols - 4;   /* window interior == body/row width */
            int cw;

            if (bw < 1)
                bw = 1;
            cw = bw / 4;
            if (cw < 1)
                cw = 1;
            /* Pack and Cells are fixed-height, non-EXPAND children of
               g_batt_box, so the box layout never resizes them -- it only
               grows the one EXPAND child (SOC).  Left at their creation width
               (cols) they overhang the client-frame's right border by 2 cols
               and the frame's right edge falls off the screen on first open.
               Size them to the box interior (window width == bw + 2 borders),
               the same width the box hands the SOC window. */
            vk_widget_resize(VK_WIDGET(g_fr_pack), bw + 2, 6);
            vk_widget_resize(VK_WIDGET(g_fr_cells), bw + 2, 7);
            vk_widget_resize(VK_WIDGET(g_pack_row0), bw, 1);
            vk_widget_resize(VK_WIDGET(g_pack_row1), bw, 1);
            vk_widget_resize(VK_WIDGET(g_cl_batt_row), bw, 1);
            vk_widget_resize(VK_WIDGET(g_cl_watts_row), bw, 1);
            for (i = 0; i < 4; i++)
                vk_widget_resize(VK_WIDGET(g_cell_row[i]), bw, 1);
            for (i = 0; i < NCELL_SHOW; i++)
                vk_widget_resize(VK_WIDGET(g_cell_box[i]), cw, 1);
            /* Scale the meters with the frame instead of letting the gaps
               around them grow.  The bars are non-EXPAND, so the box never
               resizes them -- left alone they stay METER_W/CELL_BAR_W and the
               EXPAND fillers (and the slack at the right of each cell) soak up
               all the extra width.  Size the bars explicitly from the row
               width so they grow, leaving the fillers only a fixed 2-col gap;
               then re-render each at its new width (a resize copies the old
               bar, it does not redraw it).  At 80 cols these match METER_W(22)
               and CELL_BAR_W(8), so the default layout is unchanged. */
            {
                int m0 = (bw - 2 * NAME_W - 2 * VAL_W - 2) / 2;  /* Pack/SOC/Cap */
                int mc = cw - CELL_LAB_W - 2;                    /* cell bar    */

                if (m0 < 6)
                    m0 = 6;
                if (mc < 4)
                    mc = 4;
                vk_widget_resize(VK_WIDGET(g_mt_pack), m0, 1);
                vk_widget_resize(VK_WIDGET(g_mt_soc), m0, 1);
                vk_widget_resize(VK_WIDGET(g_pr_cap), m0, 1);
                vk_progress_update(VK_PROGRESS(g_mt_pack));
                vk_progress_update(VK_PROGRESS(g_mt_soc));
                vk_progress_update(g_pr_cap);
                for (i = 0; i < NCELL_SHOW; i++)
                {
                    vk_widget_resize(VK_WIDGET(g_mt_cell[i]), mc, 1);
                    vk_progress_update(VK_PROGRESS(g_mt_cell[i]));
                }
            }
            /* Size the charger graph area explicitly.  The frame->window->body
               resize cascade only fires on an actual size change and only
               resizes an EXPAND child one level deep, so g_cl_body's canvas is
               NOT reliably grown to the window interior here -- it keeps its
               small creation height, and vk_box_update(g_cl_body) then hands the
               graph wrapper only that tiny leftover.  Grow the body ourselves
               from the terminal geometry (same derivation as clientH), then size
               the wrapper row and graph to fill the space below the 5 label rows
               minus the 1-row top/bottom pads (graph is 1 col narrower each side
               for the L/R pads). */
             {
                 int prod_h, bodyH_prod, gh_prod;

                 /* Charger frame is fixed at 7 rows; production gets the
                    leftover from the box interior.  The frame->box->window
                    resize cascade never re-lays-out nested boxes, so size
                    everything explicitly. */
                 prod_h = (clientH - 2) - 7;   /* box interior minus charger frame */
                 if (prod_h < 8)
                     prod_h = 8;
                 vk_widget_resize(VK_WIDGET(g_fr_charger), cols, 7);
                 vk_widget_resize(VK_WIDGET(g_fr_prod), cols, (short)prod_h);

                 bodyH_prod = prod_h - 2;      /* production window interior */
                 if (bodyH_prod < 1)
                     bodyH_prod = 1;
                 vk_widget_resize(VK_WIDGET(g_prod_body), bw, bodyH_prod);

                 gh_prod = bodyH_prod - 2;     /* 1-row top pad + 1-row bottom pad */
                 if (gh_prod < 1)
                     gh_prod = 1;
                 vk_widget_resize(VK_WIDGET(g_cl_graph_row), bw, gh_prod);
                 vk_widget_resize(VK_WIDGET(g_cl_graph), bw - 2, gh_prod);

                 /* Size the SOC chart area in the battery branch.  The batt box
                    holds Pack(6) + Cells(7) + SOC(EXPAND) so SOC gets the
                    remaining space.  SOC interior = g_fr_soc height - 2 border.
                    g_soc_body fills that interior; g_pk_graph_row fills the
                    body below the 1-row top/bottom pads (like the charger). */
                if (g_kind == 0 && g_soc_body && g_pk_graph)
                {
                    int soc_h = clientH - 15;    /* box leftover: (clientH-2) - pack(6) - cells(7) */
                    int bodyH_soc, gh_soc;

                    if (soc_h < 3)
                        soc_h = 3;
                    vk_widget_resize(VK_WIDGET(g_fr_soc), cols, soc_h);
                    bodyH_soc = soc_h - 2;       /* SOC window interior (1 border each side) */
                    if (bodyH_soc < 1)
                        bodyH_soc = 1;
                    vk_widget_resize(VK_WIDGET(g_soc_body), bw, bodyH_soc);
                    gh_soc = bodyH_soc - 2;      /* 1-row top pad + 1-row bottom pad */
                    if (gh_soc < 1)
                        gh_soc = 1;
                    vk_widget_resize(VK_WIDGET(g_pk_graph_row), bw, gh_soc);
                    vk_widget_resize(VK_WIDGET(g_pk_graph), bw - 2, gh_soc);
                    /* Force pad repaint (same gotcha as charger pads). */
                    vk_widget_resize(VK_WIDGET(g_pk_pad_l), 1, gh_soc);
                    vk_widget_resize(VK_WIDGET(g_pk_pad_r), 1, gh_soc);
                    vk_widget_resize(VK_WIDGET(g_pk_pad_t), bw, 1);
                    vk_widget_resize(VK_WIDGET(g_pk_pad_b), bw, 1);
                    vk_widget_recreate(VK_WIDGET(g_pk_pad_l));
                    vk_widget_recreate(VK_WIDGET(g_pk_pad_r));
                    vk_widget_recreate(VK_WIDGET(g_pk_pad_t));
                    vk_widget_recreate(VK_WIDGET(g_pk_pad_b));
                }
                /* A vk_filler only paints its (otherwise black) canvas on an
                   ON_RESIZE/ON_RECREATE event, and a box lays out -- i.e.
                   resizes -- only its EXPAND children.  These pads are fixed
                   (non-expand) so they would never repaint and would show as
                   black cells over the box's blue.  Size each to span its full
                   edge (so it also fully covers that edge) and recreate it to
                   force the blue fill. */
                vk_widget_resize(VK_WIDGET(g_cl_pad_l), 1, gh_prod);
                vk_widget_resize(VK_WIDGET(g_cl_pad_r), 1, gh_prod);
                vk_widget_resize(VK_WIDGET(g_cl_pad_t), bw, 1);
                vk_widget_resize(VK_WIDGET(g_cl_pad_b), bw, 1);
                vk_widget_recreate(VK_WIDGET(g_cl_pad_l));
                vk_widget_recreate(VK_WIDGET(g_cl_pad_r));
                vk_widget_recreate(VK_WIDGET(g_cl_pad_t));
                vk_widget_recreate(VK_WIDGET(g_cl_pad_b));
            }
        }
    }
    if (g_kind)
        show_charger_widgets();
    else
        show_pack_widgets();
    g_visible = 1;
    pack_hints();
    if (g_fr_pack)
    {
        vk_box_update(g_pack_row0);
        vk_box_update(g_pack_row1);
        vk_box_update(g_pack_body);
        vk_window_update(g_fr_pack);
    }
    if (g_fr_cells)
    {
        for (i = 0; i < NCELL_SHOW; i++)
            vk_box_update(g_cell_box[i]);
        for (i = 0; i < 4; i++)
            vk_box_update(g_cell_row[i]);
        vk_box_update(g_cells_body);
        vk_window_update(g_fr_cells);
    }
    if (g_fr_charger)
    {
        vk_box_update(g_cl_batt_row);
        vk_box_update(g_cl_watts_row);
        vk_box_update(g_cl_body);
        vk_window_update(g_fr_charger);
    }
    if (g_fr_prod)
    {
        vk_box_update(g_cl_graph_row);
        vk_box_update(g_prod_body);
        vk_window_update(g_fr_prod);
    }
    if (g_kind)
    {
        vk_box_update(g_chg_box);
        if (g_cf_chg)
            vk_frame_update(g_cf_chg);
    }
    else
    {
        if (g_pk_graph)
        {
            vk_box_update(g_pk_graph_row);
            vk_box_update(g_soc_body);
            vk_window_update(g_fr_soc);
            if (g_cf_batt)
                vk_frame_update(g_cf_batt);
        }
        if (g_batt_box)
            vk_box_update(g_batt_box);
        if (g_cf_batt)
            vk_frame_update(g_cf_batt);
    }
}

void mf_pack_on_resize(void)
{
    int cols = mf_ui_cols();
    int rows = mf_ui_rows();
    int clientH = rows - 4;   /* rows 3..rows-2: below chrome, above hints */

    if (!g_visible)
        return;
    if (clientH < 3)
        clientH = 3;
    vk_widget_resize(VK_WIDGET(g_chrome1), cols, 1);
    vk_widget_resize(VK_WIDGET(g_chrome2), cols, 1);
    vk_widget_resize(VK_WIDGET(g_hints), cols, 1);
    vk_widget_move(VK_WIDGET(g_hints), 0, rows - 1);
    vk_widget_resize(VK_WIDGET(g_cf_batt), cols, clientH);
    vk_widget_resize(VK_WIDGET(g_cf_chg), cols, clientH);
    mf_pack_show(g_kind);
}

int mf_pack_visible(void)
{
    return g_visible;
}
int mf_pack_is_charger(void)
{
    return g_kind;
}
int mf_pack_has_switch(void)
{
    return g_has_switch;
}

int mf_pack_switch_on(const char *key)
{
    if (!key)
        return 0;
    if (strcmp(key, "charge") == 0)
        return g_chg_on;
    if (strcmp(key, "discharge") == 0)
        return g_dsg_on;
    if (strcmp(key, "balance") == 0)
        return g_bal_on;
    return 0;
}

static void fmt_interface(const char *ep, const char *driver,
                          char *line, size_t cap)
{
    const char *kind = "unknown";
    const char *id = "";

    if (ep && strncmp(ep, "ble:", 4) == 0)
    {
        kind = "bluetooth";
        id = ep + 4;
    } else if (ep && strncmp(ep, "usb-id:", 7) == 0)
    {
        kind = "usb";
        id = ep + 7;
    } else if (ep && strncmp(ep, "usb:", 4) == 0)
    {
        kind = "usb";
        id = ep + 4;
    } else if (ep && strncmp(ep, "tcp:", 4) == 0)
    {
        kind = "tcp";
        id = ep + 4;
    } else if (ep && ep[0])
    {
        kind = ep;
    } else if (driver && driver[0])
    {
        kind = driver;
    }
    if (id[0])
        snprintf(line, cap, "interface: %s  %s", kind, id);
    else
        snprintf(line, cap, "interface: %s", kind);
}

static cJSON *data_obj(cJSON *root)
{
    cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "data");
    return d ? d : root;
}

static double jnum(cJSON *o, const char *k, double def)
{
    cJSON *it = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return (it && cJSON_IsNumber(it)) ? it->valuedouble : def;
}

static const char *jstr(cJSON *o, const char *k, const char *def)
{
    cJSON *it = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return (it && cJSON_IsString(it) && it->valuestring) ? it->valuestring : def;
}

static void fmt_temps(cJSON *data, char *line, size_t cap, const char *fallback)
{
    cJSON *temps, *labels, *tv, *lb;
    int i, n, nlab, off = 0;

    if (!line || cap == 0)
        return;
    temps = data ? cJSON_GetObjectItemCaseSensitive(data, "temperatures_c") : NULL;
    labels = data ? cJSON_GetObjectItemCaseSensitive(data, "temp_labels") : NULL;
    if (!temps || !cJSON_IsArray(temps) || cJSON_GetArraySize(temps) < 1)
    {
        snprintf(line, cap, "%s", fallback);
        return;
    }
    n = cJSON_GetArraySize(temps);
    nlab = (labels && cJSON_IsArray(labels)) ? cJSON_GetArraySize(labels) : 0;
    line[0] = '\0';
    for (i = 0; i < n && off < (int)cap - 1; i++)
    {
        const char *lab;
        char piece[24];
        int m;

        tv = cJSON_GetArrayItem(temps, i);
        lb = (i < nlab) ? cJSON_GetArrayItem(labels, i) : NULL;
        lab = (lb && cJSON_IsString(lb) && lb->valuestring) ? lb->valuestring : "T";
        if (tv && cJSON_IsNumber(tv))
            snprintf(piece, sizeof(piece), "%s%s %.0f", i ? "  " : "", lab,
                     tv->valuedouble);
        else
            snprintf(piece, sizeof(piece), "%s%s --", i ? "  " : "", lab);
        m = snprintf(line + off, cap - (size_t)off, "%s", piece);
        if (m < 0)
            break;
        off += m;
    }
    if (!line[0])
        snprintf(line, cap, "%s", fallback);
}

static int caps_switch(cJSON *root)
{
    cJSON *caps = cJSON_GetObjectItemCaseSensitive(root, "caps");
    int i, n;
    if (!caps || !cJSON_IsArray(caps))
        return 0;
    n = cJSON_GetArraySize(caps);
    for (i = 0; i < n; i++)
    {
        cJSON *it = cJSON_GetArrayItem(caps, i);
        if (cJSON_IsString(it) && it->valuestring &&
            strstr(it->valuestring, "switch"))
            return 1;
    }
    return 0;
}

void mf_pack_set_device_id(const char *id)
{
    if (id && id[0])
        snprintf(g_hist_id, sizeof(g_hist_id), "%s", id);
    else
        g_hist_id[0] = '\0';
}

const char *mf_pack_get_device_id(void)
{
    return g_hist_id[0] ? g_hist_id : "";
}

static int g_graph_interval_min = 30;
static const int g_zoom_min[] = { 1, 5, 10, 15, 30, 60 };

static void pack_hints(void)
{
    char buf[128];

    if (!g_hints)
        return;
    if (g_kind)
    {
        if (g_graph_interval_min >= 60)
            snprintf(buf, sizeof(buf),
                      "+/- zoom (1h)  e settings  Esc dashboard");
        else
            snprintf(buf, sizeof(buf),
                      "+/- zoom (%dm)  e settings  Esc dashboard",
                      g_graph_interval_min);
        vk_label_set_text(g_hints, buf);
    }
    else
    {
        if (g_graph_interval_min >= 60)
            snprintf(buf, sizeof(buf),
                      "+/- zoom (1h)  c charge  d discharge  b balancer  e settings  Esc dashboard");
        else
            snprintf(buf, sizeof(buf),
                      "+/- zoom (%dm)  c charge  d discharge  b balancer  e settings  Esc dashboard",
                      g_graph_interval_min);
        vk_label_set_text(g_hints, buf);
    }
    vk_label_update(g_hints);
}

static int graph_bar_cells(int minutes)
{
    if (minutes >= 60)
        return 4;
    if (minutes >= 30)
        return 3;
    if (minutes >= 15)
        return 2;
    if (minutes >= 10)
        return 2;
    return 1;
}

void mf_pack_set_graph_interval(int minutes)
{
    if (minutes < 1)
        minutes = 1;
    g_graph_interval_min = minutes;
    if (g_cl_graph)
        vk_graph_set_bar_width(g_cl_graph, graph_bar_cells(minutes));
    if (g_pk_graph)
        vk_graph_set_bar_width(g_pk_graph, graph_bar_cells(minutes));
    pack_hints();
}

int mf_pack_graph_bar_width(void)
{
    return graph_bar_cells(g_graph_interval_min);
}

int mf_pack_graph_zoom(int finer)
{
    int n = (int)(sizeof(g_zoom_min) / sizeof(g_zoom_min[0]));
    int i, best = 0, best_d = 10000;

    for (i = 0; i < n; i++)
    {
        int d = g_zoom_min[i] - g_graph_interval_min;
        if (d < 0)
            d = -d;
        if (d < best_d)
        {
            best_d = d;
            best = i;
        }
    }
    if (finer)
    {
        if (best <= 0)
            return 0;
        best--;
    }
    else
    {
        if (best >= n - 1)
            return 0;
        best++;
    }
    if (g_zoom_min[best] == g_graph_interval_min)
        return 0;
    mf_pack_set_graph_interval(g_zoom_min[best]);
    return 1;
}

void mf_pack_set_history(const double *values, int count, double y_max,
    const char * const *labels)
{
    if (count <= 0)
        return;
    if (g_kind == 0 && g_pk_graph)
    {
        /* Battery view: SOC chart, always 0-100% y-range. */
        vk_graph_set_data(g_pk_graph, values, count);
        if (labels)
            vk_graph_set_x_labels(g_pk_graph, labels, count);
        vk_graph_set_y_range(g_pk_graph, 0.0, 100.0);
        vk_graph_update(g_pk_graph);
        vk_box_update(g_pk_graph_row);
        vk_box_update(g_soc_body);
        vk_window_update(g_fr_soc);
        if (g_cf_batt)
            vk_frame_update(g_cf_batt);
     } else if (g_cl_graph)
     {
         /* Charger view: power chart, data-driven y-range. */
         vk_graph_set_data(g_cl_graph, values, count);
         if (labels)
             vk_graph_set_x_labels(g_cl_graph, labels, count);
         vk_graph_set_y_range(g_cl_graph, 0.0, y_max);
         vk_graph_update(g_cl_graph);
         vk_box_update(g_cl_batt_row);
         vk_box_update(g_cl_watts_row);
         vk_box_update(g_cl_body);
         vk_window_update(g_fr_charger);
         vk_box_update(g_cl_graph_row);
         vk_box_update(g_prod_body);
         vk_window_update(g_fr_prod);
         vk_box_update(g_chg_box);
         if (g_cf_chg)
             vk_frame_update(g_cf_chg);
     }
}

int mf_pack_get_graph_interval(void)
{
    return g_graph_interval_min;
}

void mf_pack_update(const char *json)
{
    cJSON *root, *data, *cells;
    char line[96];
    const char *name, *kind, *driver, *state;
    int seq, i, ncell = 0;
    double pack_v, soc, cur, vmin = 99, vmax = 0;

    if (!g_visible || !json)
        return;
    root = cJSON_Parse(json);
    if (!root)
        return;
    data = data_obj(root);
    name = jstr(root, "name", "device");
    kind = jstr(root, "kind", "");
    driver = jstr(root, "driver", "");
    state = jstr(root, "state", "");
    seq = (int)jnum(root, "seq", 0);
    mf_pack_set_device_id(jstr(root, "id", ""));
    snprintf(line, sizeof(line), "%s  %s/%s  %s  seq %d",
             name, kind, driver, state, seq);
    vk_label_set_text(g_chrome1, line);
    vk_label_update(g_chrome1);
    {
        const char *ep = jstr(root, "endpoint", "");
        fmt_interface(ep, driver, line, sizeof(line));
        vk_label_set_text(g_chrome2, line);
        vk_label_update(g_chrome2);
    }
    g_has_switch = caps_switch(root);

    if (!g_kind)
    {
        pack_v = jnum(data, "pack_voltage_v", 0);
        soc = jnum(data, "soc_pct", 0);
        cur = jnum(data, "current_a", 0);
        {
            int ns = (int)jnum(data, "cell_count", 16);
            double full = jnum(data, "full_capacity_ah", 0);
            double rem = jnum(data, "remaining_capacity_ah", 0);

            if (ns < 1)
                ns = 16;
            vk_progress_set_range(VK_PROGRESS(g_mt_pack), 2.80 * ns, 3.65 * ns);
            band_cell_v(g_mt_pack, (double)ns);
            vk_progress_set_value(VK_PROGRESS(g_mt_pack), pack_v);
            vk_progress_set_value(VK_PROGRESS(g_mt_soc), soc);
            if (full > 0)
            {
                vk_progress_set_range(g_pr_cap, 0, full);
                vk_progress_set_value(g_pr_cap, rem > 0 ? rem : 0);
                snprintf(line, sizeof(line), " %.1f Ah", rem);
            }
            else
            {
                vk_progress_set_range(g_pr_cap, 0, 100);
                vk_progress_set_value(g_pr_cap, jnum(data, "soh_pct", 0));
                snprintf(line, sizeof(line), " SOH %.0f%%",
                         jnum(data, "soh_pct", 0));
            }
        }
        vk_progress_update(VK_PROGRESS(g_mt_pack));
        vk_progress_update(VK_PROGRESS(g_mt_soc));
        vk_progress_update(g_pr_cap);
        vk_label_set_text(g_lb_vcap, line);
        vk_label_update(g_lb_vcap);
        snprintf(line, sizeof(line), " %.2f V", pack_v);
        vk_label_set_text(g_lb_vpack, line);
        vk_label_update(g_lb_vpack);
        snprintf(line, sizeof(line), " %.0f%%", soc);
        vk_label_set_text(g_lb_vsoc, line);
        vk_label_update(g_lb_vsoc);
        snprintf(line, sizeof(line), "%+.2f A", cur);
        vk_label_set_text(g_lb_cur, line);
        vk_label_update(g_lb_cur);
        fmt_temps(data, line, sizeof(line), "MOS --  T1 --  T2 --");
        vk_label_set_text(g_lb_temp, line);
        vk_label_update(g_lb_temp);
        g_chg_on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "charge_mosfet_on"));
        g_dsg_on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "discharge_mosfet_on"));
        g_bal_on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "balancer_switch"));
        if (g_has_switch)
        {
            double iba = jnum(data, "balance_current_a", 0);
            int n = snprintf(line, sizeof(line), "CHG %s  DSG %s  BAL %s",
                             g_chg_on ? "on" : "off",
                             g_dsg_on ? "on" : "off",
                             g_bal_on ? "on" : "off");
            if (n > 0 && iba != 0.0 && (size_t)n < sizeof(line))
                snprintf(line + n, sizeof(line) - (size_t)n, "  %+.2f A", iba);
            vk_label_set_text(g_lb_mos, line);
        }
        else
        {
            vk_label_set_text(g_lb_mos, "--");
        }
        vk_label_update(g_lb_mos);
        cells = cJSON_GetObjectItemCaseSensitive(data, "cells");
        if (cells && cJSON_IsArray(cells))
            ncell = cJSON_GetArraySize(cells);
        {
            double vsum = 0;
            int nv = 0;
            double cv[NCELL_SHOW];
            int cbal[NCELL_SHOW];
            int spread;

            for (i = 0; i < NCELL_SHOW; i++)
            {
                cv[i] = 0;
                cbal[i] = 0;
                if (i < ncell)
                {
                    cJSON *cell = cJSON_GetArrayItem(cells, i);
                    cv[i] = jnum(cell, "voltage_v", 0);
                    cbal[i] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                                               cell, "balancing"));
                    if (cv[i] > 0)
                    {
                        vsum += cv[i];
                        nv++;
                        if (cv[i] < vmin)
                            vmin = cv[i];
                        if (cv[i] > vmax)
                            vmax = cv[i];
                    }
                }
            }
            spread = (nv > 1 && vmax > vmin);
            for (i = 0; i < NCELL_SHOW; i++)
            {
                char idx[4], lab[12];
                int role = 0;

                snprintf(idx, sizeof(idx), "%02d", i + 1);
                if (cv[i] > 0)
                    snprintf(lab, sizeof(lab), " %.3f%c", cv[i],
                             cbal[i] ? 'b' : ' ');
                else
                    snprintf(lab, sizeof(lab), " --");
                if (spread && cv[i] > 0)
                {
                    if (cv[i] == vmax)
                        role = 1;
                    else if (cv[i] == vmin)
                        role = 2;
                }
                vk_label_set_text(g_lb_cidx[i], idx);
                style_cell_idx(g_lb_cidx[i], role);
                vk_label_update(g_lb_cidx[i]);
                vk_label_set_text(g_lb_cell[i], lab);
                vk_label_update(g_lb_cell[i]);
                vk_progress_set_value(VK_PROGRESS(g_mt_cell[i]),
                                      cv[i] > 0 ? cv[i] : 2.80);
                vk_progress_update(VK_PROGRESS(g_mt_cell[i]));
            }
            {
                double full = jnum(data, "full_capacity_ah", 0);
                double rem = jnum(data, "remaining_capacity_ah", -1);
                double avg = nv > 0 ? vsum / (double)nv : 0;
                soc = mf_tui_display_soc(jnum(data, "soc_pct", soc),
                                         rem, full, avg, cur);
            }
        }
        vk_progress_set_value(VK_PROGRESS(g_mt_soc), soc);
        vk_progress_update(VK_PROGRESS(g_mt_soc));
        snprintf(line, sizeof(line), " %.0f%%", soc);
        vk_label_set_text(g_lb_vsoc, line);
        vk_label_update(g_lb_vsoc);
        if (ncell > 0 && vmax >= vmin)
            snprintf(line, sizeof(line), "dV %.0f mV", (vmax - vmin) * 1000.0);
        else
            snprintf(line, sizeof(line), "dV --");
        if (ncell > NCELL_SHOW)
        {
            char extra[32];
            snprintf(extra, sizeof(extra), "  +%d", ncell - NCELL_SHOW);
            strncat(line, extra, sizeof(line) - strlen(line) - 1);
        }
        vk_label_set_text(g_lb_spread, line);
        vk_label_update(g_lb_spread);
        vk_box_update(g_pack_row0);
        vk_box_update(g_pack_row1);
        vk_box_update(g_pack_body);
        vk_window_update(g_fr_pack);
        for (i = 0; i < NCELL_SHOW; i++)
            vk_box_update(g_cell_box[i]);
        for (i = 0; i < 4; i++)
            vk_box_update(g_cell_row[i]);
        vk_box_update(g_cells_body);
        vk_window_update(g_fr_cells);
    }
    else
    {
        double bv = jnum(data, "battery_voltage_v", 0);
        double w = jnum(data, "charging_watts", 0);
        vk_progress_set_value(VK_PROGRESS(g_mt_batt), bv);
        vk_progress_set_value(VK_PROGRESS(g_mt_watts), w);
        vk_progress_update(VK_PROGRESS(g_mt_batt));
        vk_progress_update(VK_PROGRESS(g_mt_watts));
        snprintf(line, sizeof(line), " %.2f V", bv);
        vk_label_set_text(g_lb_vbatt, line);
        vk_label_update(g_lb_vbatt);
        snprintf(line, sizeof(line), " %.0f W", w);
        vk_label_set_text(g_lb_vwatts, line);
        vk_label_update(g_lb_vwatts);
        snprintf(line, sizeof(line), "stage %s", jstr(data, "charge_stage", "--"));
        vk_label_set_text(g_lb_stage, line);
        vk_label_update(g_lb_stage);
        snprintf(line, sizeof(line), "kWh today %.1f   Ah today %.0f",
                 jnum(data, "kwh_today", 0), jnum(data, "ah_today", 0));
        vk_label_set_text(g_lb_energy, line);
        vk_label_update(g_lb_energy);
        fmt_temps(data, line, sizeof(line), "FET --  Batt --  PCB --");
        vk_label_set_text(g_lb_ctemp, line);
        vk_label_update(g_lb_ctemp);
        vk_box_update(g_cl_batt_row);
        vk_box_update(g_cl_watts_row);
        vk_box_update(g_cl_body);
        vk_window_update(g_fr_charger);
        vk_box_update(g_cl_graph_row);
        vk_box_update(g_prod_body);
        vk_window_update(g_fr_prod);
    }
    /* Composite the freshly rendered windows up through the client frame to the
       screen.  The vk_window_update calls above only redraw onto the window
       canvases; without this the new content never reaches the frame. */
    if (g_kind)
    {
        vk_box_update(g_chg_box);
        if (g_cf_chg)
            vk_frame_update(g_cf_chg);
    }
    else
    {
        if (g_batt_box)
            vk_box_update(g_batt_box);
        if (g_cf_batt)
            vk_frame_update(g_cf_batt);
    }
    cJSON_Delete(root);
}

void mf_pack_shutdown(void)
{
    int i;
    destroy_w(VK_WIDGET(g_chrome1));
    destroy_w(VK_WIDGET(g_chrome2));
    destroy_w(VK_WIDGET(g_hints));
    /* Pull the windows out of their client frames first: vk_box/vk_frame
       dtors list_del still-slotted children, so a still-slotted window
       freed first would corrupt the list. */
    vk_box_set_widget(g_batt_box, 0, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_batt_box, 1, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_batt_box, 2, NULL, VK_INHERIT_NONE);
    vk_frame_set_child(g_cf_batt, NULL, VK_INHERIT_NONE);
    vk_frame_set_child(g_cf_chg, NULL, VK_INHERIT_NONE);
    vk_window_set_child(g_fr_pack, NULL, VK_INHERIT_NONE);
    /* Unparent every box child before freeing: vk_box's dtor list_dels the
       children still in its slots, so a leaf freed while still slotted would
       be a use-after-free (see device_settings.c box_vacate). */
    for (i = 0; i < 7; i++)
        vk_box_set_widget(g_pack_row0, i, NULL, VK_INHERIT_NONE);
    for (i = 0; i < 5; i++)
        vk_box_set_widget(g_pack_row1, i, NULL, VK_INHERIT_NONE);
    for (i = 0; i < 4; i++)
        vk_box_set_widget(g_pack_body, i, NULL, VK_INHERIT_NONE);
    vk_widget_destroy(VK_WIDGET(g_lb_npack));
    vk_widget_destroy(VK_WIDGET(g_mt_pack));
    vk_widget_destroy(VK_WIDGET(g_lb_vpack));
    vk_widget_destroy(VK_WIDGET(g_lb_nsoc));
    vk_widget_destroy(VK_WIDGET(g_mt_soc));
    vk_widget_destroy(VK_WIDGET(g_lb_vsoc));
    vk_widget_destroy(VK_WIDGET(g_lb_ncap));
    vk_widget_destroy(VK_WIDGET(g_pr_cap));
    vk_widget_destroy(VK_WIDGET(g_lb_vcap));
    vk_widget_destroy(VK_WIDGET(g_lb_cur));
    vk_widget_destroy(VK_WIDGET(g_lb_temp));
    vk_widget_destroy(VK_WIDGET(g_lb_mos));
    vk_widget_destroy(VK_WIDGET(g_pack_fill0));
    vk_widget_destroy(VK_WIDGET(g_pack_fill1));
    vk_box_destroy(g_pack_row0);
    vk_box_destroy(g_pack_row1);
    vk_box_destroy(g_pack_body);
    vk_widget_destroy(VK_WIDGET(g_fr_pack));
    vk_window_set_child(g_fr_cells, NULL, VK_INHERIT_NONE);
    for (i = 0; i < NCELL_SHOW; i++)
    {
        vk_box_set_widget(g_cell_box[i], 0, NULL, VK_INHERIT_NONE);
        vk_box_set_widget(g_cell_box[i], 1, NULL, VK_INHERIT_NONE);
        vk_box_set_widget(g_cell_box[i], 2, NULL, VK_INHERIT_NONE);
    }
    for (i = 0; i < 4; i++)
    {
        vk_box_set_widget(g_cell_row[i], 0, NULL, VK_INHERIT_NONE);
        vk_box_set_widget(g_cell_row[i], 1, NULL, VK_INHERIT_NONE);
        vk_box_set_widget(g_cell_row[i], 2, NULL, VK_INHERIT_NONE);
        vk_box_set_widget(g_cell_row[i], 3, NULL, VK_INHERIT_NONE);
    }
    for (i = 0; i < 5; i++)
        vk_box_set_widget(g_cells_body, i, NULL, VK_INHERIT_NONE);
    for (i = 0; i < NCELL_SHOW; i++)
    {
        vk_widget_destroy(VK_WIDGET(g_lb_cidx[i]));
        vk_widget_destroy(VK_WIDGET(g_lb_cell[i]));
        vk_widget_destroy(VK_WIDGET(g_mt_cell[i]));
    }
    vk_widget_destroy(VK_WIDGET(g_lb_spread));
    for (i = 0; i < NCELL_SHOW; i++)
        vk_box_destroy(g_cell_box[i]);
    for (i = 0; i < 4; i++)
        vk_box_destroy(g_cell_row[i]);
    vk_box_destroy(g_cells_body);
    vk_widget_destroy(VK_WIDGET(g_fr_cells));
    /* SOC history teardown: vacate slots, destroy leaves (pads/graph),
       then boxes, then the window — leaves before boxes. */
    if (g_soc_body)
    {
        vk_box_set_widget(g_soc_body, 0, NULL, VK_INHERIT_NONE);
        vk_box_set_widget(g_soc_body, 1, NULL, VK_INHERIT_NONE);
        vk_box_set_widget(g_soc_body, 2, NULL, VK_INHERIT_NONE);
    }
    vk_box_set_widget(g_pk_graph_row, 0, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_pk_graph_row, 1, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_pk_graph_row, 2, NULL, VK_INHERIT_NONE);
    vk_window_set_child(g_fr_soc, NULL, VK_INHERIT_NONE);
    vk_widget_destroy(VK_WIDGET(g_pk_pad_l));
    vk_widget_destroy(VK_WIDGET(g_pk_pad_r));
    vk_widget_destroy(VK_WIDGET(g_pk_pad_t));
    vk_widget_destroy(VK_WIDGET(g_pk_pad_b));
    vk_widget_destroy(VK_WIDGET(g_pk_graph));
    vk_box_destroy(g_pk_graph_row);
    vk_box_destroy(g_soc_body);
    vk_widget_destroy(VK_WIDGET(g_fr_soc));
    /* Charger teardown: vacate g_chg_box slots, prod_body slots, graph_row
       slots, then destroy leaves (labels/meters/pads/graph) → boxes → windows
       → g_chg_box.  Leaves before boxes (a vk_box dtor list_dels slotted
       children; freeing a still-slotted leaf is a UAF). */
    vk_window_set_child(g_fr_charger, NULL, VK_INHERIT_NONE);
    vk_window_set_child(g_fr_prod, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_chg_box, 0, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_chg_box, 1, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_prod_body, 0, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_prod_body, 1, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_prod_body, 2, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_graph_row, 0, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_graph_row, 1, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_graph_row, 2, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_body, 0, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_body, 1, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_body, 2, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_body, 3, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_body, 4, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_batt_row, 0, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_batt_row, 1, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_batt_row, 2, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_batt_row, 3, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_watts_row, 0, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_watts_row, 1, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_watts_row, 2, NULL, VK_INHERIT_NONE);
    vk_box_set_widget(g_cl_watts_row, 3, NULL, VK_INHERIT_NONE);
    vk_widget_destroy(VK_WIDGET(g_lb_nbatt));
    vk_widget_destroy(VK_WIDGET(g_mt_batt));
    vk_widget_destroy(VK_WIDGET(g_lb_vbatt));
    vk_widget_destroy(VK_WIDGET(g_lb_nwatts));
    vk_widget_destroy(VK_WIDGET(g_mt_watts));
    vk_widget_destroy(VK_WIDGET(g_lb_vwatts));
    vk_widget_destroy(VK_WIDGET(g_lb_stage));
    vk_widget_destroy(VK_WIDGET(g_lb_energy));
    vk_widget_destroy(VK_WIDGET(g_lb_ctemp));
    vk_widget_destroy(VK_WIDGET(g_cl_rfill0));
    vk_widget_destroy(VK_WIDGET(g_cl_rfill1));
    vk_widget_destroy(VK_WIDGET(g_cl_graph));
    vk_widget_destroy(VK_WIDGET(g_cl_pad_l));
    vk_widget_destroy(VK_WIDGET(g_cl_pad_r));
    vk_widget_destroy(VK_WIDGET(g_cl_pad_t));
    vk_widget_destroy(VK_WIDGET(g_cl_pad_b));
    vk_box_destroy(g_cl_batt_row);
    vk_box_destroy(g_cl_watts_row);
    vk_box_destroy(g_cl_graph_row);
    vk_box_destroy(g_cl_body);
    vk_box_destroy(g_prod_body);
    vk_widget_destroy(VK_WIDGET(g_fr_charger));
    vk_widget_destroy(VK_WIDGET(g_fr_prod));
    vk_box_destroy(g_chg_box);
    vk_box_destroy(g_batt_box);
    destroy_w(VK_WIDGET(g_cf_batt));
    destroy_w(VK_WIDGET(g_cf_chg));
    memset(&g_chrome1, 0, sizeof(g_chrome1));
    g_visible = 0;
}
