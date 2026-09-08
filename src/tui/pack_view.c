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
static vk_frame_t *g_fr_pack, *g_fr_cells, *g_fr_classic;
static vk_meter_t *g_mt_pack, *g_mt_soc, *g_mt_batt, *g_mt_watts;
static vk_progress_t *g_pr_cap;
static vk_label_t *g_lb_npack, *g_lb_nsoc, *g_lb_ncap;
static vk_label_t *g_lb_vpack, *g_lb_vsoc, *g_lb_vcap;
static vk_label_t *g_lb_cur, *g_lb_temp, *g_lb_mos, *g_lb_spread;
static vk_label_t *g_lb_nbatt, *g_lb_nwatts, *g_lb_vbatt, *g_lb_vwatts;
static vk_label_t *g_lb_stage, *g_lb_energy, *g_lb_ctemp;
static vk_meter_t *g_mt_cell[NCELL_SHOW];
static vk_label_t *g_lb_cell[NCELL_SHOW];
static int g_visible;
static int g_has_switch;
static int g_chg_on, g_dsg_on, g_bal_on;

static int frame_caption(vk_object_t *obj, int event, void *anything)
{
    WINDOW *canvas = vk_widget_get_canvas(VK_WIDGET(obj));
    const char *cap = anything;
    (void)event;
    if (!canvas || !cap)
        return 0;
    wattron(canvas, VDK_COLORS(COL_TEXT, COL_BG) | A_BOLD);
    mvwprintw(canvas, 0, 2, " %s ", cap);
    wattroff(canvas, VDK_COLORS(COL_TEXT, COL_BG) | A_BOLD);
    return 0;
}

static void style_frame(vk_frame_t *f)
{
    vk_widget_set_colors(VK_WIDGET(f), COL_TEXT, COL_BG);
    vk_widget_set_relief_colors(VK_WIDGET(f), COLOR_WHITE, COLOR_BLACK);
    vk_frame_set_border_style(f, VK_BORDER_SINGLE | VK_RELIEF_SUNKEN);
    vk_frame_set_border_colors(f, COL_TEXT, COL_BG);
}

static vk_label_t *mk_lab(int x, int y, int w)
{
    vk_label_t *l = vk_label_create(w);
    vk_widget_set_colors(VK_WIDGET(l), COL_TEXT, COL_BG);
    mf_ui_attach(VK_WIDGET(l), x, y);
    return l;
}

static vk_label_t *mk_lab_txt(int x, int y, int w, const char *txt)
{
    vk_label_t *l = mk_lab(x, y, w);
    if (txt) {
        vk_label_set_text(l, txt);
        vk_label_update(l);
    }
    return l;
}

static vk_meter_t *mk_meter(int x, int y, int len, double lo, double hi)
{
    vk_meter_t *m = vk_meter_create(VK_PROGRESS_HORIZONTAL, len, 1);
    vk_widget_set_colors(VK_WIDGET(m), COL_TEXT, COL_BG);
    vk_progress_set_range(VK_PROGRESS(m), lo, hi);
    vk_progress_set_trough(VK_PROGRESS(m), VK_TROUGH_SOLID, COL_TROUGH, COL_BG);
    vk_progress_set_style(VK_PROGRESS(m), VK_PROGRESS_UNDERBAR);
    mf_ui_attach(VK_WIDGET(m), x, y);
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

static void band_pct(vk_meter_t *m)
{
    vk_meter_clear_thresholds(m);
    vk_meter_add_threshold(m, 10, COLOR_YELLOW, COL_BG);
    vk_meter_add_threshold(m, 25, COLOR_GREEN, COL_BG);
}

static void hide_w(vk_widget_t *w)
{
    if (w)
        vk_widget_hide(w);
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
    int i, col, row, ccx, ccy, cols = mf_ui_cols();
    int iw = cols > 4 ? cols - 4 : 76;

    g_chrome1 = mk_lab(0, 1, cols);
    g_chrome2 = mk_lab(0, 2, cols);
    g_fr_pack = vk_frame_create(cols, 6);
    style_frame(g_fr_pack);
    mf_ui_attach(VK_WIDGET(g_fr_pack), 0, 3);
    vk_object_register_event(VK_OBJECT(g_fr_pack), VK_EVENT_ON_FINALIZE,
                             frame_caption, "Pack");
    g_lb_npack = mk_lab_txt(LEFT_X, 4, NAME_W, "Pack");
    g_mt_pack = mk_meter(LEFT_X + NAME_W, 4, METER_W, 40.0, 58.4);
    band_cell_v(g_mt_pack, 16.0);
    g_lb_vpack = mk_lab(LEFT_X + NAME_W + METER_W + 1, 4, VAL_W);
    g_lb_nsoc = mk_lab_txt(RIGHT_X, 4, NAME_W, "SOC");
    g_mt_soc = mk_meter(RIGHT_X + NAME_W, 4, METER_W, 0.0, 100.0);
    band_pct(g_mt_soc);
    g_lb_vsoc = mk_lab(RIGHT_X + NAME_W + METER_W + 1, 4, VAL_W);
    g_lb_ncap = mk_lab_txt(LEFT_X, 5, NAME_W, "Cap");
    g_pr_cap = vk_progress_create(VK_PROGRESS_HORIZONTAL, METER_W, 1);
    vk_widget_set_colors(VK_WIDGET(g_pr_cap), COL_TEXT, COL_BG);
    vk_progress_set_range(g_pr_cap, 0, 100);
    vk_progress_set_trough(g_pr_cap, VK_TROUGH_SOLID, COL_TROUGH, COL_BG);
    vk_progress_set_style(g_pr_cap, VK_PROGRESS_UNDERBAR);
    mf_ui_attach(VK_WIDGET(g_pr_cap), LEFT_X + NAME_W, 5);
    g_lb_vcap = mk_lab(LEFT_X + NAME_W + METER_W + 1, 5, VAL_W);
    g_lb_cur = mk_lab(RIGHT_X, 5, 30);
    g_lb_temp = mk_lab(2, 6, iw);
    g_lb_mos = mk_lab(2, 7, iw);

    g_fr_cells = vk_frame_create(cols, 7);
    style_frame(g_fr_cells);
    mf_ui_attach(VK_WIDGET(g_fr_cells), 0, 9);
    vk_object_register_event(VK_OBJECT(g_fr_cells), VK_EVENT_ON_FINALIZE,
                             frame_caption, "Cells");
    for (i = 0; i < NCELL_SHOW; i++) {
        col = i % 4;
        row = i / 4;
        ccx = 2 + col * (iw / 4);
        ccy = 10 + row;
        g_mt_cell[i] = mk_meter(ccx + CELL_LAB_W + CELL_GAP, ccy, CELL_BAR_W,
                                2.80, 3.65);
        band_cell_v(g_mt_cell[i], 1.0);
        g_lb_cell[i] = mk_lab(ccx, ccy, CELL_LAB_W);
    }
    g_lb_spread = mk_lab(2, 14, iw);

    g_fr_classic = vk_frame_create(cols, 13);
    style_frame(g_fr_classic);
    mf_ui_attach(VK_WIDGET(g_fr_classic), 0, 3);
    vk_object_register_event(VK_OBJECT(g_fr_classic), VK_EVENT_ON_FINALIZE,
                             frame_caption, "Classic");
    g_lb_nbatt = mk_lab_txt(LEFT_X, 4, NAME_W, "Batt");
    g_mt_batt = mk_meter(LEFT_X + NAME_W, 4, 28, 40.0, 64.0);
    g_lb_vbatt = mk_lab(LEFT_X + NAME_W + 28 + 1, 4, VAL_W);
    g_lb_nwatts = mk_lab_txt(LEFT_X, 5, NAME_W, "Watts");
    g_mt_watts = mk_meter(LEFT_X + NAME_W, 5, 28, 0.0, 4000.0);
    g_lb_vwatts = mk_lab(LEFT_X + NAME_W + 28 + 1, 5, VAL_W);
    g_lb_stage = mk_lab(2, 7, iw);
    g_lb_energy = mk_lab(2, 8, iw);
    g_lb_ctemp = mk_lab(2, 10, iw);

    g_hints = mk_lab(0, 24, cols);
    mf_pack_hide();
}

void mf_pack_hide(void)
{
    int i;
    hide_w(VK_WIDGET(g_chrome1));
    hide_w(VK_WIDGET(g_chrome2));
    hide_w(VK_WIDGET(g_fr_pack));
    hide_w(VK_WIDGET(g_fr_cells));
    hide_w(VK_WIDGET(g_fr_classic));
    hide_w(VK_WIDGET(g_mt_pack));
    hide_w(VK_WIDGET(g_mt_soc));
    hide_w(VK_WIDGET(g_pr_cap));
    hide_w(VK_WIDGET(g_lb_npack));
    hide_w(VK_WIDGET(g_lb_nsoc));
    hide_w(VK_WIDGET(g_lb_ncap));
    hide_w(VK_WIDGET(g_lb_vpack));
    hide_w(VK_WIDGET(g_lb_vsoc));
    hide_w(VK_WIDGET(g_lb_vcap));
    hide_w(VK_WIDGET(g_lb_cur));
    hide_w(VK_WIDGET(g_lb_temp));
    hide_w(VK_WIDGET(g_lb_mos));
    hide_w(VK_WIDGET(g_lb_spread));
    hide_w(VK_WIDGET(g_mt_batt));
    hide_w(VK_WIDGET(g_mt_watts));
    hide_w(VK_WIDGET(g_lb_nbatt));
    hide_w(VK_WIDGET(g_lb_nwatts));
    hide_w(VK_WIDGET(g_lb_vbatt));
    hide_w(VK_WIDGET(g_lb_vwatts));
    hide_w(VK_WIDGET(g_lb_stage));
    hide_w(VK_WIDGET(g_lb_energy));
    hide_w(VK_WIDGET(g_lb_ctemp));
    hide_w(VK_WIDGET(g_hints));
    for (i = 0; i < NCELL_SHOW; i++) {
        hide_w(VK_WIDGET(g_mt_cell[i]));
        hide_w(VK_WIDGET(g_lb_cell[i]));
    }
    g_visible = 0;
}

static void show_pack_widgets(void)
{
    int i;
    show_w(VK_WIDGET(g_chrome1));
    show_w(VK_WIDGET(g_chrome2));
    show_w(VK_WIDGET(g_fr_pack));
    show_w(VK_WIDGET(g_fr_cells));
    show_w(VK_WIDGET(g_mt_pack));
    show_w(VK_WIDGET(g_mt_soc));
    show_w(VK_WIDGET(g_pr_cap));
    show_w(VK_WIDGET(g_lb_npack));
    show_w(VK_WIDGET(g_lb_nsoc));
    show_w(VK_WIDGET(g_lb_ncap));
    show_w(VK_WIDGET(g_lb_vpack));
    show_w(VK_WIDGET(g_lb_vsoc));
    show_w(VK_WIDGET(g_lb_vcap));
    show_w(VK_WIDGET(g_lb_cur));
    show_w(VK_WIDGET(g_lb_temp));
    show_w(VK_WIDGET(g_lb_mos));
    show_w(VK_WIDGET(g_lb_spread));
    show_w(VK_WIDGET(g_hints));
    for (i = 0; i < NCELL_SHOW; i++) {
        show_w(VK_WIDGET(g_mt_cell[i]));
        show_w(VK_WIDGET(g_lb_cell[i]));
    }
}

static void show_charger_widgets(void)
{
    show_w(VK_WIDGET(g_chrome1));
    show_w(VK_WIDGET(g_chrome2));
    show_w(VK_WIDGET(g_fr_classic));
    show_w(VK_WIDGET(g_mt_batt));
    show_w(VK_WIDGET(g_mt_watts));
    show_w(VK_WIDGET(g_lb_nbatt));
    show_w(VK_WIDGET(g_lb_nwatts));
    show_w(VK_WIDGET(g_lb_vbatt));
    show_w(VK_WIDGET(g_lb_vwatts));
    show_w(VK_WIDGET(g_lb_stage));
    show_w(VK_WIDGET(g_lb_energy));
    show_w(VK_WIDGET(g_lb_ctemp));
    show_w(VK_WIDGET(g_hints));
}

void mf_pack_show(int charger)
{
    mf_pack_hide();
    g_kind = charger ? 1 : 0;
    if (g_kind)
        show_charger_widgets();
    else
        show_pack_widgets();
    g_visible = 1;
    vk_label_set_text(g_hints, g_kind ? "Esc dashboard" :
                      "c charge  d discharge  b balancer  Esc dashboard");
    vk_label_update(g_hints);
    if (g_fr_pack)
        vk_frame_update(g_fr_pack);
    if (g_fr_cells)
        vk_frame_update(g_fr_cells);
    if (g_fr_classic)
        vk_frame_update(g_fr_classic);
}

int mf_pack_visible(void) { return g_visible; }
int mf_pack_is_charger(void) { return g_kind; }
int mf_pack_has_switch(void) { return g_has_switch; }

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

    if (ep && strncmp(ep, "ble:", 4) == 0) {
        kind = "bluetooth";
        id = ep + 4;
    } else if (ep && strncmp(ep, "usb-id:", 7) == 0) {
        kind = "usb";
        id = ep + 7;
    } else if (ep && strncmp(ep, "usb:", 4) == 0) {
        kind = "usb";
        id = ep + 4;
    } else if (ep && strncmp(ep, "tcp:", 4) == 0) {
        kind = "tcp";
        id = ep + 4;
    } else if (ep && ep[0]) {
        kind = ep;
    } else if (driver && driver[0]) {
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
    if (!temps || !cJSON_IsArray(temps) || cJSON_GetArraySize(temps) < 1) {
        snprintf(line, cap, "%s", fallback);
        return;
    }
    n = cJSON_GetArraySize(temps);
    nlab = (labels && cJSON_IsArray(labels)) ? cJSON_GetArraySize(labels) : 0;
    line[0] = '\0';
    for (i = 0; i < n && off < (int)cap - 1; i++) {
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
    for (i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(caps, i);
        if (cJSON_IsString(it) && it->valuestring &&
            strstr(it->valuestring, "switch"))
            return 1;
    }
    return 0;
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

    if (!g_kind) {
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
            if (full > 0) {
                vk_progress_set_range(g_pr_cap, 0, full);
                vk_progress_set_value(g_pr_cap, rem > 0 ? rem : 0);
                snprintf(line, sizeof(line), "%.1f Ah", rem);
            } else {
                vk_progress_set_range(g_pr_cap, 0, 100);
                vk_progress_set_value(g_pr_cap, jnum(data, "soh_pct", 0));
                snprintf(line, sizeof(line), "SOH %.0f%%",
                         jnum(data, "soh_pct", 0));
            }
        }
        vk_progress_update(VK_PROGRESS(g_mt_pack));
        vk_progress_update(VK_PROGRESS(g_mt_soc));
        vk_progress_update(g_pr_cap);
        vk_label_set_text(g_lb_vcap, line);
        vk_label_update(g_lb_vcap);
        snprintf(line, sizeof(line), "%.2f V", pack_v);
        vk_label_set_text(g_lb_vpack, line);
        vk_label_update(g_lb_vpack);
        snprintf(line, sizeof(line), "%.0f%%", soc);
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
        if (g_has_switch) {
            double iba = jnum(data, "balance_current_a", 0);
            int n = snprintf(line, sizeof(line), "CHG %s  DSG %s  BAL %s",
                             g_chg_on ? "on" : "off",
                             g_dsg_on ? "on" : "off",
                             g_bal_on ? "on" : "off");
            if (n > 0 && iba != 0.0 && (size_t)n < sizeof(line))
                snprintf(line + n, sizeof(line) - (size_t)n, "  %+.2f A", iba);
            vk_label_set_text(g_lb_mos, line);
        } else {
            vk_label_set_text(g_lb_mos, "--");
        }
        vk_label_update(g_lb_mos);
        cells = cJSON_GetObjectItemCaseSensitive(data, "cells");
        if (cells && cJSON_IsArray(cells))
            ncell = cJSON_GetArraySize(cells);
        {
            double vsum = 0;
            int nv = 0;
            for (i = 0; i < NCELL_SHOW; i++) {
                double v = 0;
                int bal = 0;
                char lab[12];
                if (i < ncell) {
                    cJSON *cell = cJSON_GetArrayItem(cells, i);
                    v = jnum(cell, "voltage_v", 0);
                    bal = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cell,
                                                                       "balancing"));
                    if (v > 0) {
                        vsum += v;
                        nv++;
                        if (v < vmin)
                            vmin = v;
                        if (v > vmax)
                            vmax = v;
                    }
                }
                if (v > 0)
                    snprintf(lab, sizeof(lab), "%02d %.3f%c", i + 1, v,
                             bal ? 'b' : ' ');
                else
                    snprintf(lab, sizeof(lab), "%02d --", i + 1);
                vk_label_set_text(g_lb_cell[i], lab);
                vk_label_update(g_lb_cell[i]);
                vk_progress_set_value(VK_PROGRESS(g_mt_cell[i]), v > 0 ? v : 2.80);
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
        snprintf(line, sizeof(line), "%.0f%%", soc);
        vk_label_set_text(g_lb_vsoc, line);
        vk_label_update(g_lb_vsoc);
        if (ncell > 0 && vmax >= vmin)
            snprintf(line, sizeof(line), "dV %.0f mV", (vmax - vmin) * 1000.0);
        else
            snprintf(line, sizeof(line), "dV --");
        if (ncell > NCELL_SHOW) {
            char extra[32];
            snprintf(extra, sizeof(extra), "  +%d", ncell - NCELL_SHOW);
            strncat(line, extra, sizeof(line) - strlen(line) - 1);
        }
        vk_label_set_text(g_lb_spread, line);
        vk_label_update(g_lb_spread);
        vk_frame_update(g_fr_pack);
        vk_frame_update(g_fr_cells);
    } else {
        double bv = jnum(data, "battery_voltage_v", 0);
        double w = jnum(data, "charging_watts", 0);
        vk_progress_set_value(VK_PROGRESS(g_mt_batt), bv);
        vk_progress_set_value(VK_PROGRESS(g_mt_watts), w);
        vk_progress_update(VK_PROGRESS(g_mt_batt));
        vk_progress_update(VK_PROGRESS(g_mt_watts));
        snprintf(line, sizeof(line), "%.2f V", bv);
        vk_label_set_text(g_lb_vbatt, line);
        vk_label_update(g_lb_vbatt);
        snprintf(line, sizeof(line), "%.0f W", w);
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
        vk_frame_update(g_fr_classic);
    }
    cJSON_Delete(root);
}

void mf_pack_shutdown(void)
{
    int i;
    destroy_w(VK_WIDGET(g_chrome1));
    destroy_w(VK_WIDGET(g_chrome2));
    destroy_w(VK_WIDGET(g_hints));
    destroy_w(VK_WIDGET(g_fr_pack));
    destroy_w(VK_WIDGET(g_fr_cells));
    destroy_w(VK_WIDGET(g_fr_classic));
    destroy_w(VK_WIDGET(g_mt_pack));
    destroy_w(VK_WIDGET(g_mt_soc));
    destroy_w(VK_WIDGET(g_pr_cap));
    destroy_w(VK_WIDGET(g_lb_npack));
    destroy_w(VK_WIDGET(g_lb_nsoc));
    destroy_w(VK_WIDGET(g_lb_ncap));
    destroy_w(VK_WIDGET(g_lb_vpack));
    destroy_w(VK_WIDGET(g_lb_vsoc));
    destroy_w(VK_WIDGET(g_lb_vcap));
    destroy_w(VK_WIDGET(g_lb_cur));
    destroy_w(VK_WIDGET(g_lb_temp));
    destroy_w(VK_WIDGET(g_lb_mos));
    destroy_w(VK_WIDGET(g_lb_spread));
    destroy_w(VK_WIDGET(g_mt_batt));
    destroy_w(VK_WIDGET(g_mt_watts));
    destroy_w(VK_WIDGET(g_lb_nbatt));
    destroy_w(VK_WIDGET(g_lb_nwatts));
    destroy_w(VK_WIDGET(g_lb_vbatt));
    destroy_w(VK_WIDGET(g_lb_vwatts));
    destroy_w(VK_WIDGET(g_lb_stage));
    destroy_w(VK_WIDGET(g_lb_energy));
    destroy_w(VK_WIDGET(g_lb_ctemp));
    for (i = 0; i < NCELL_SHOW; i++) {
        destroy_w(VK_WIDGET(g_mt_cell[i]));
        destroy_w(VK_WIDGET(g_lb_cell[i]));
    }
    memset(&g_chrome1, 0, sizeof(g_chrome1));
    g_visible = 0;
}
