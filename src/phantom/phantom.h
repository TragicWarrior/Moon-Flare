#ifndef MF_PHANTOM_H
#define MF_PHANTOM_H

/* Phantom modules: a built-in "phantom" driver for batteries, chargers and
 * inverters.  A phantom stands in for a unit the daemon cannot reach (a
 * second pack wired into the system but not into moonflared) by shadowing
 * one or more real modules of the same kind and reporting their average.
 * It counts toward the system totals like any module of its kind, records
 * no history, and has no actions. */

#include "loader.h"

#define MF_PHANTOM_DRIVER "phantom"

/* Add the phantom module types to the registry (after the plugins load).
 * Returns how many were added. */
int mf_phantom_register(mf_plugin_registry_t *reg);

#endif
