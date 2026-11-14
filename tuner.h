/*
 * tuner.h  (HDHomeRun-QUI version)
 *
 * The TUI's l1_details.c talks to tuners through the TUI's tuner.h backend
 * interface. The QUI doesn't use that backend layer, so this file provides
 * just the four calls l1_details.c needs, implemented with libhdhomerun in
 * tuner_qui.c. That lets l1_details.c / l1_detail_parser.c be dropped in from
 * the TUI unchanged.
 *
 * Do NOT replace this with the TUI's tuner.h -- they share a name on purpose
 * (l1_details.c does #include "tuner.h") but not their contents.
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

#ifndef QUI_TUNER_H
#define QUI_TUNER_H

#ifdef __cplusplus
extern "C" {
#endif

struct hdhomerun_device_t;

/* A tuner as l1_details.c sees it: an open device handle (already set to
 * the right tuner with hdhomerun_device_set_tuner) and its tuner index. */
struct tuner {
    struct hdhomerun_device_t *dev;
    int index;
};

/* Each returns > 0 on success and points *out at a string owned by
 * libhdhomerun, valid until the next call on the same device handle.
 * Returns <= 0 if the value isn't available. */
int tuner_get_plpinfo(struct tuner *t, const char **out);
int tuner_get_streaminfo(struct tuner *t, const char **out);
int tuner_get_version(struct tuner *t, const char **out);

/* The base64 L1 string from /tunerN/l1detail. Only models that report
 * signal levels in dBm (and firmware with l1detail) have it. */
int tuner_get_l1_string(struct tuner *t, const char **out);

#ifdef __cplusplus
}
#endif

#endif /* QUI_TUNER_H */
