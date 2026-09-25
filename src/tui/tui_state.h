#ifndef MF_TUI_STATE_H
#define MF_TUI_STATE_H

/*
 * What the TUI learns and keeps between runs; not settings, so File > Save
 * config never touches it.  Today: the Discharge meter's high mark per
 * daemon ("host:port"), for its Auto scale.
 *
 * $XDG_STATE_HOME/moonflare/tui-state.json, else
 * ~/.local/state/moonflare/tui-state.json.
 */

/* The highest discharge seen from addr, in watts; 0 when none yet. */
double mf_state_peak(const char *addr);
/* Remember w as addr's high mark (0 forgets it) and save the file. */
void   mf_state_set_peak(const char *addr, double w);

#endif
