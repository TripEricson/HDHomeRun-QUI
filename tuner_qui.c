/*
 * tuner_qui.c
 *
 * libhdhomerun implementation of the four tuner calls l1_details.c needs
 * (see tuner.h in this directory).
 *
 * HDHomeRun QUI - Copyright (C) 2025 - Mark J. Colombo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>
#include <hdhomerun.h>
#include "tuner.h"

/* Fetch a variable, logging the device's error text if it refuses */
static int get_logged(struct tuner *t, const char *name, const char **out)
{
    char *val = NULL;
    char *err = NULL;
    int r = hdhomerun_device_get_var(t->dev, name, &val, &err);
    if (r > 0 && val) {
        *out = val;
        return 1;
    }
    fprintf(stderr, "tuner_qui: get %s failed (%d): %s\n", name, r,
            err ? err : (r < 0 ? "communication error" : "no value"));
    return r > 0 ? 0 : r;
}

int tuner_get_plpinfo(struct tuner *t, const char **out)
{
    char name[32];
    if (!t || !t->dev || !out) return -1;
    snprintf(name, sizeof(name), "/tuner%d/plpinfo", t->index);
    return get_logged(t, name, out);
}

int tuner_get_streaminfo(struct tuner *t, const char **out)
{
    char name[32];
    if (!t || !t->dev || !out) return -1;
    snprintf(name, sizeof(name), "/tuner%d/streaminfo", t->index);
    return get_logged(t, name, out);
}

int tuner_get_version(struct tuner *t, const char **out)
{
    if (!t || !t->dev || !out) return -1;
    return get_logged(t, "/sys/version", out);
}

int tuner_get_l1_string(struct tuner *t, const char **out)
{
    char *raw = NULL;
    char *s = NULL;
    struct hdhomerun_tuner_status_t status;
    char path[32];

    if (!t || !t->dev || !out) return -1;

    /* Same gate as the GUI: only models that report dBm ("ss=NN(-xx dBm)")
     * have /tunerN/l1detail. */
    if (hdhomerun_device_get_tuner_status(t->dev, &raw, &status) <= 0 || !raw) return 0;
    {
        const char *ss = strstr(raw, "ss=");
        if (!ss || !strchr(ss, '(')) return 0;
    }

    snprintf(path, sizeof(path), "/tuner%d/l1detail", t->index);
    if (hdhomerun_device_get_var(t->dev, path, &s, NULL) <= 0 || !s || !s[0]) return 0;
    *out = s;
    return 1;
}
