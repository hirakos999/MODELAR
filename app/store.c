/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Hirakos */
/* MODELAR: the patches in the flash. Sixteen slots and a quick backup, each a patch and its eight patterns, in
 * fm1-base's data area (plat_data: 3 sectors a slot from 0x97000; the panel calibration's pair at 0xDC000 stays
 * clear), checked by a CRC. The system settings are a store object of their own (A/B, torn-write safe).
 * Part of the unity build (app.c), main loop only. */

#define SLOT_BASE 0x97000u
#define SLOT_SIZE 0x3000u                /* 12 KiB: the patch and its patterns, with room */
#define SLOT_MAGIC 0x544C534Du           /* "MSLT" */
#define SLOT_BACKUP NSLOT                /* the 17th: SHIFT + SAVE held */
typedef struct {
    uint32_t magic, len, crc;
} slot_head_t;
#define SLOT_DATA (sizeof(patch_t) + sizeof(pats))
typedef char slot_fits[(sizeof(slot_head_t) + SLOT_DATA <= SLOT_SIZE) ? 1 : -1];

static uint32_t crc32_add(uint32_t c, const uint8_t *p, uint32_t n)
{
    uint32_t k;
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t slot_off(int i) { return SLOT_BASE + (uint32_t)i * SLOT_SIZE; }

static const uint8_t *slot_valid(int i)   /* the slot's data in place, or 0 */
{
    const slot_head_t *h = (const slot_head_t *)plat_data(slot_off(i));
    const uint8_t *d;
    if (!h || h->magic != SLOT_MAGIC || h->len != SLOT_DATA)
        return 0;
    d = (const uint8_t *)(h + 1);
    if (crc32_add(0, d, (uint32_t)SLOT_DATA) != h->crc)
        return 0;
    if (((const patch_t *)d)->magic != PATCH_MAGIC || ((const patch_t *)d)->format != PATCH_FORMAT)
        return 0;
    return d;
}

int slot_name(int i, char *b)            /* the slot's name (NAME_LEN + 1 bytes); 0 = empty */
{
    const uint8_t *d = slot_valid(i);
    int k;
    if (!d) {
        for (k = 0; k < NAME_LEN; k++)
            b[k] = "--------"[k];
        b[NAME_LEN] = 0;
        return 0;
    }
    for (k = 0; k < NAME_LEN; k++)
        b[k] = ((const patch_t *)d)->name[k];
    b[NAME_LEN] = 0;
    return 1;
}

int slot_save(int i)                     /* 0 = done */
{
    slot_head_t h;
    uint32_t off = slot_off(i), s;
    h.magic = SLOT_MAGIC;
    h.len = SLOT_DATA;
    h.crc = crc32_add(crc32_add(0, (const uint8_t *)&patch, sizeof patch), (const uint8_t *)pats, sizeof pats);
    for (s = 0; s < SLOT_SIZE; s += 0x1000u)
        if (plat_data_erase(off + s))
            return 1;
    if (plat_data_write(off + sizeof h, &patch, sizeof patch) ||
        plat_data_write(off + sizeof h + sizeof patch, pats, sizeof pats))
        return 1;
    return plat_data_write(off, &h, sizeof h);   /* the header last: a cut-off write leaves no valid slot */
}

int slot_load(int i)                     /* 0 = loaded; the knobs back to 0, as at power-on */
{
    const uint8_t *d = slot_valid(i);
    uint32_t k;
    if (!d)
        return 1;
    for (k = 0; k < sizeof patch; k++)
        ((uint8_t *)&patch)[k] = d[k];
    for (k = 0; k < sizeof pats; k++)
        ((uint8_t *)pats)[k] = d[sizeof patch + k];
    for (k = 0; k < SYN_NOSC; k++)
        vol[k] = 0;
    return 0;
}

static void sys_defaults(void)
{
    uint32_t k;
    for (k = 0; k < sizeof sys; k++)
        ((uint8_t *)&sys)[k] = 0;
    sys.magic = SYS_MAGIC;
    sys.leds = 0;
}

void sys_load(void)
{
    if (plat_store_load(OBJ_SYS, &sys, sizeof sys) != (int)sizeof sys || sys.magic != SYS_MAGIC ||
        sys.slot >= NSLOT || sys.midi_ch > 16u || sys.theme > 2u || sys.leds > 2u)
        sys_defaults();
}

int sys_save(void) { return plat_store_save(OBJ_SYS, &sys, sizeof sys); }
