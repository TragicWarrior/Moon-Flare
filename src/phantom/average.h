#ifndef MF_PHANTOM_AVERAGE_H
#define MF_PHANTOM_AVERAGE_H

/* A phantom module's reading: the average of the readings of the modules
 * it shadows.  Kept free of the daemon so it can be unit-tested. */

#include <cJSON.h>

/* Average n readings (n >= 1) of modules of one kind, shaped like the
 * first.  A number is the mean of the readings that have a number there
 * (rounded to 4 decimals); objects are averaged key by key and arrays
 * element by element (per cell, per temperature sensor); strings, bools
 * and nulls are taken from the first reading.  Returns a new object, or
 * NULL when n < 1. */
cJSON *mf_phantom_average(const cJSON *const *readings, int n);

#endif
