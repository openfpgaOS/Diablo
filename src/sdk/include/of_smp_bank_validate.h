/* Validate all metadata and sample ranges before exposing a preloaded bank. */
#ifndef OF_SMP_BANK_VALIDATE_H
#define OF_SMP_BANK_VALIDATE_H

#include "of_smp_bank.h"

static inline int of_smp_bank_valid(const void *data, uint32_t size)
{
    const uint32_t prefix = sizeof(ofsf_header_t)
                          + OFSF_PRESET_COUNT * sizeof(ofsf_preset_t);
    if (!data || size < prefix)
        return 0;
    const ofsf_header_t *h = (const ofsf_header_t *)data;
    if (h->magic != OFSF_MAGIC || h->version != OFSF_VERSION ||
        !h->sample_rate || !h->zone_count ||
        h->zone_count > (size - prefix) / sizeof(ofsf_zone_t))
        return 0;
    uint32_t metadata_end = prefix + h->zone_count * sizeof(ofsf_zone_t);
    if (h->sample_data_offset < metadata_end ||
        h->sample_data_offset > size ||
        h->sample_data_size > size - h->sample_data_offset ||
        ((h->sample_data_offset | h->sample_data_size) & 1u))
        return 0;

    const uint8_t *base = (const uint8_t *)data;
    const ofsf_preset_t *presets = (const ofsf_preset_t *)(base + sizeof(*h));
    for (unsigned i = 0; i < OFSF_PRESET_COUNT; ++i) {
        if (presets[i].zone_count &&
            (presets[i].zone_start > h->zone_count ||
             presets[i].zone_count > h->zone_count - presets[i].zone_start))
            return 0;
    }
    const ofsf_zone_t *zones = (const ofsf_zone_t *)(base + prefix);
    for (uint32_t i = 0; i < h->zone_count; ++i) {
        const ofsf_zone_t *z = &zones[i];
        if ((z->sample_offset & 1u) || z->sample_offset > h->sample_data_size ||
            z->sample_length > (h->sample_data_size - z->sample_offset) / 2u ||
            z->sample_length > 0x3fffffu)
            return 0;
        if ((z->loop_mode == OFSF_LOOP_FORWARD || z->loop_mode == OFSF_LOOP_BIDI) &&
            (z->loop_start >= z->loop_end || z->loop_end > z->sample_length))
            return 0;
    }
    return 1;
}
#endif
