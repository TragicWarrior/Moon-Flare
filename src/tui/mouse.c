#include "mouse.h"
#include "ui_screen.h"

#include <vdk.h>

/* ---- Main dispatcher (llama-chess pattern) ---- */

int
mf_mouse_handle(const MEVENT *mev)
{
    int x, y;
    mmask_t bstate;

    if (!mev)
        return 0;

    x = mev->x;
    y = mev->y;
    bstate = mev->bstate;

    /*
     * Modal on top: sink every mouse event until dismissed.
     * Order: confirm > help > device-settings > settings > menubar > dashboard.
     * Handlers may return 0 for clicks outside their widget; those must
     * not fall through to the menubar or dashboard (contention artifacts).
     */

    /* Confirm overlay (y/n). Sink all mouse while open. 2 = Yes. */
    if (mf_confirm_open())
    {
        int r = mf_confirm_mouse(x, y, bstate);
        return r == 2 ? 2 : 1;
    }

    /* Module picker (Add / Remove Module): keyboard only; sink clicks. */
    if (mf_picker_open())
        return 1;

    /* Help / settings: sink until dismissed. */
    if (mf_ui_help_open())
    {
        (void)mf_help_mouse(x, y, bstate);
        return 1;
    }

    /* Profile editor overlays the connections manager: route to it first and
     * sink clicks outside it (modal). Then the manager itself. */
    if (mf_ui_editor_open())
    {
        (void)mf_editor_mouse(x, y, bstate);
        return 1;
    }
    if (mf_ui_connections_open())
    {
        (void)mf_connections_mouse(x, y, bstate);
        return 1;
    }

    if (mf_devset_open())
    {
        int r = mf_devset_mouse(x, y, bstate);
        return r == 2 ? 2 : 1;
    }
    if (mf_ui_settings_open())
    {
        (void)mf_settings_mouse(x, y, bstate);
        return 1;
    }

    /* Menubar (bar + dropdown). */
    if (mf_menubar_mouse(x, y, bstate))
        return 1;

    /* Dashboard cards (only when visible). */
    if (mf_dash_mouse(x, y, bstate))
        return 1;

    return 0;
}
