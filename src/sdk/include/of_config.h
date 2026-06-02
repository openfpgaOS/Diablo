//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_config.h -- access to os.ini configuration sections.
 *
 * The OS parses os.ini once at boot and exposes section/key lookups through
 * the services table. Missing files and missing keys are normal; callers
 * should provide defaults where appropriate.
 */

#ifndef OF_CONFIG_H
#define OF_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define OF_CONFIG_ERR_NOENT (-2)
#define OF_CONFIG_ERR_INVAL (-22)
#define OF_CONFIG_ERR_NOSPC (-28)

#ifndef OF_PC

#include "of_services.h"

#define OF_CONFIG_SVC_INDEX(field) \
    ((uint32_t)((offsetof(struct of_services_table, field) - \
                 offsetof(struct of_services_table, video_init)) / sizeof(void *)))

#define OF_CONFIG_SVC_HAS(field) \
    (OF_SVC && (OF_SVC->count > OF_CONFIG_SVC_INDEX(field)))

static inline int of_config_get(const char *section, const char *key,
                                char *out, uint32_t out_len) {
    if (!OF_CONFIG_SVC_HAS(config_get) || !OF_SVC->config_get)
        return OF_CONFIG_ERR_NOENT;
    return OF_SVC->config_get(section, key, out, out_len);
}

static inline int of_config_get_int(const char *section, const char *key,
                                    int default_value) {
    if (!OF_CONFIG_SVC_HAS(config_get_int) || !OF_SVC->config_get_int)
        return default_value;
    return OF_SVC->config_get_int(section, key, default_value);
}

static inline int of_config_get_bool(const char *section, const char *key,
                                     int default_value) {
    if (!OF_CONFIG_SVC_HAS(config_get_bool) || !OF_SVC->config_get_bool)
        return default_value;
    return OF_SVC->config_get_bool(section, key, default_value);
}

static inline int of_config_next(const char *section, uint32_t *cursor,
                                 char *key_out, uint32_t key_len,
                                 char *value_out, uint32_t value_len) {
    if (!OF_CONFIG_SVC_HAS(config_next) || !OF_SVC->config_next)
        return OF_CONFIG_ERR_NOENT;
    return OF_SVC->config_next(section, cursor, key_out, key_len,
                               value_out, value_len);
}

#else

static inline int of_config_get(const char *section, const char *key,
                                char *out, uint32_t out_len) {
    (void)section;
    (void)key;
    (void)out;
    (void)out_len;
    return OF_CONFIG_ERR_NOENT;
}

static inline int of_config_get_int(const char *section, const char *key,
                                    int default_value) {
    (void)section;
    (void)key;
    return default_value;
}

static inline int of_config_get_bool(const char *section, const char *key,
                                     int default_value) {
    (void)section;
    (void)key;
    return default_value;
}

static inline int of_config_next(const char *section, uint32_t *cursor,
                                 char *key_out, uint32_t key_len,
                                 char *value_out, uint32_t value_len) {
    (void)section;
    (void)cursor;
    (void)key_out;
    (void)key_len;
    (void)value_out;
    (void)value_len;
    return OF_CONFIG_ERR_NOENT;
}

#endif /* OF_PC */

#endif /* OF_CONFIG_H */
