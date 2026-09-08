#ifndef MF_UI_SCREEN_H
#define MF_UI_SCREEN_H

#include <vdk.h>

vk_screen_t *mf_ui_screen(void);
void mf_ui_refresh(void);
void mf_ui_attach(vk_widget_t *w, int x, int y);
void mf_ui_front_clear(void);
void mf_ui_front_push(vk_widget_t *w);
void mf_ui_quit(void);
int  mf_ui_cols(void);
int  mf_ui_rows(void);
void mf_ui_open_settings(void);
void mf_ui_show_help(int about);
void mf_ui_save_config(void);
void mf_ui_load_config(void);

void mf_menubar_init(void);
void mf_menubar_shutdown(void);
int  mf_menubar_key(wint_t c);
int  mf_menubar_active(void);

void mf_dash_init(void);
void mf_dash_update(const char *hostport, const char *tag, const char *json);
void mf_dash_shutdown(void);

int  mf_tui_run(const char *connect, const char *config_path);
int  mf_tui_dump_layout_main(void);

#endif
