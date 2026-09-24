#ifndef MF_UI_SCREEN_H
#define MF_UI_SCREEN_H

#include <ncursesw/curses.h>
#include <vdk.h>

vk_screen_t *mf_ui_screen(void);
void mf_ui_refresh(void);
void mf_ui_resize(void);
void mf_ui_attach(vk_widget_t *w, int x, int y);
void mf_ui_front_clear(void);
void mf_ui_front_push(vk_widget_t *w);
vk_frame_t *mf_ui_make_client_frame(int w, int h);
void mf_ui_quit(void);
int  mf_ui_cols(void);
int  mf_ui_rows(void);
void mf_ui_open_settings(void);
void mf_ui_open_connections(void);
void mf_ui_show_help(int about);
int  mf_ui_help_open(void);
int  mf_ui_settings_open(void);
void mf_ui_save_config(void);
void mf_ui_load_config(void);

void mf_menubar_init(void);
void mf_menubar_on_resize(void);
void mf_menubar_tick(void);
void mf_menubar_shutdown(void);
int  mf_menubar_key(wint_t c);
int  mf_menubar_active(void);
int  mf_menubar_mouse(int x, int y, mmask_t bstate);
int  mf_menubar_is_init(void);

void mf_dash_init(void);
void mf_dash_on_resize(void);
void mf_dash_update(const char *hostport, const char *tag, const char *json);
void mf_dash_shutdown(void);
void mf_dash_set_visible(int vis);
int  mf_dash_mouse(int x, int y, mmask_t bstate);
int  mf_dash_catalog_n(void);
const char *mf_dash_catalog_id(int i);
const char *mf_dash_catalog_name(int i);
const char *mf_dash_catalog_kind(int i);
int  mf_dash_catalog_active(int i);

/* Dashboard keys: arrows/Tab move the cursor, Enter opens, e edits the
 * selected module's settings, Space toggles whether it counts toward the
 * System totals. */
#define MF_DASH_KEY_NONE    0
#define MF_DASH_KEY_HANDLED 1
#define MF_DASH_KEY_OPEN    2
#define MF_DASH_KEY_TOGGLE  3
#define MF_DASH_KEY_EDIT    4   /* 'e': settings for the selected module */
int  mf_dash_key(wint_t c, int *cat_idx);
void mf_dash_select(int cat_idx);

void mf_pack_init(void);
void mf_pack_show(int charger);
void mf_pack_hide(void);
void mf_pack_update(const char *json);
void mf_pack_shutdown(void);
int  mf_pack_visible(void);
int  mf_pack_is_charger(void);
int  mf_pack_has_switch(void);
int  mf_pack_switch_on(const char *key);
void mf_pack_on_resize(void);
void mf_pack_set_device_id(const char *id);
const char *mf_pack_get_device_id(void);
void mf_pack_set_history(const double *values, int count, double y_max,
    const char * const *labels);
void mf_pack_set_graph_interval(int minutes);
int  mf_pack_get_graph_interval(void);
int  mf_pack_graph_zoom(int finer);
int  mf_pack_graph_bar_width(void);

void mf_ui_request_history(const char *id);
void mf_ui_handle_history(void);

void mf_devset_show(const char *id, const char *name, const char *json);
/* Add Module form: the settings form with every field editable, seeded
 * from a driver's defaults; Save posts a new device instead of a PUT. */
void mf_devset_show_add(const char *kind, const char *driver, const char *json,
                        const char *fields);
int  mf_devset_is_add(void);
const char *mf_devset_add_kind(void);
const char *mf_devset_add_driver(void);
void mf_devset_set_error(const char *msg);
void mf_devset_apply_json(const char *json);
/* The daemon's reply to a settings PUT: "Settings saved." or its error. */
void mf_devset_put_result(const char *json);
int  mf_devset_saving(void);          /* a PUT is out, reply pending */
void mf_devset_close(void);
int  mf_devset_open(void);
int  mf_devset_touched(void);
int  mf_devset_has_key(const char *key);
int  mf_devset_key(wint_t c);
int  mf_devset_mouse(int x, int y, mmask_t bstate);
const char *mf_devset_id(void);
const char *mf_devset_poll_text(void);
const char *mf_devset_payload(void);
int mf_devset_get_graph_interval(void);   /* 0 when not shown */
void mf_devset_set_kind(const char *kind);
void mf_devset_set_graph_interval(int minutes);

void mf_confirm_show(const char *name, const char *action);
/* General y/n prompt; `key` comes back from mf_confirm_action(). */
void mf_confirm_show_msg(const char *title, const char *msg, const char *key);
void mf_confirm_close(void);
int  mf_confirm_open(void);
int  mf_confirm_handle(wint_t c);
int  mf_confirm_mouse(int x, int y, mmask_t bstate);
const char *mf_confirm_action(void);

/* Modal pick-one list (picker.c). */
#define MF_PICK_NONE    0
#define MF_PICK_HANDLED 1
#define MF_PICK_CHOSEN  2
#define MF_PICK_CANCEL  3
void mf_picker_show(const char *tag, const char *title,
                    const char *const *rows, int n,
                    const char *hint, const char *empty);
void mf_picker_close(void);
int  mf_picker_open(void);
int  mf_picker_key(wint_t c);
int  mf_picker_index(void);
const char *mf_picker_tag(void);

void mf_ui_add_module(void);
void mf_ui_remove_module(void);
void mf_ui_open_device_view(int idx);
void mf_ui_open_device_settings(int idx);
void mf_ui_show_dashboard(void);
const char *mf_ui_poll_path(void);
int mf_ui_connections_open(void);
int mf_ui_editor_open(void);

int  mf_tui_run(const char *connect, const char *profile, const char *config_path);
int  mf_tui_dump_layout_main(const char *which);

/* Mouse handlers for dialogs defined in ui_screen.c. */
int  mf_help_mouse(int x, int y, mmask_t bstate);
int  mf_settings_mouse(int x, int y, mmask_t bstate);
int  mf_connections_mouse(int x, int y, mmask_t bstate);
int  mf_editor_mouse(int x, int y, mmask_t bstate);

#endif
