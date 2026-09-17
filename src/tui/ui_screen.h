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
void mf_ui_show_help(int about);
int  mf_ui_help_open(void);
int  mf_ui_settings_open(void);
void mf_ui_save_config(void);
void mf_ui_load_config(void);

void mf_menubar_init(void);
void mf_menubar_on_resize(void);
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

void mf_devset_show(const char *id, const char *name, const char *json);
void mf_devset_apply_json(const char *json);
void mf_devset_close(void);
int  mf_devset_open(void);
int  mf_devset_touched(void);
int  mf_devset_has_key(const char *key);
int  mf_devset_key(wint_t c);
int  mf_devset_mouse(int x, int y, mmask_t bstate);
const char *mf_devset_id(void);
const char *mf_devset_poll_text(void);
const char *mf_devset_payload(void);

void mf_confirm_show(const char *name, const char *action);
void mf_confirm_close(void);
int  mf_confirm_open(void);
int  mf_confirm_handle(wint_t c);
int  mf_confirm_mouse(int x, int y, mmask_t bstate);
const char *mf_confirm_action(void);

void mf_ui_open_device_view(int idx);
void mf_ui_open_device_settings(int idx);
void mf_ui_show_dashboard(void);
const char *mf_ui_poll_path(void);

int  mf_tui_run(const char *connect, const char *config_path);
int  mf_tui_dump_layout_main(const char *which);

/* Mouse handlers for dialogs defined in ui_screen.c. */
int  mf_help_mouse(int x, int y, mmask_t bstate);
int  mf_settings_mouse(int x, int y, mmask_t bstate);

#endif
