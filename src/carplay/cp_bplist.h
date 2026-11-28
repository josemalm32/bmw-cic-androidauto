/* nbt_carplay: Apple binary property lists (bplist00), the bodies of AirPlay's requests and answers.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 */
#ifndef NBT_CARPLAY_BPLIST_H
#define NBT_CARPLAY_BPLIST_H

enum { BP_BOOL, BP_INT, BP_REAL, BP_STR, BP_DATA, BP_ARRAY, BP_DICT };

struct bp {
    int type;
    unsigned long long u;   /* BP_INT: the 64-bit value (negative ones in two's complement); BP_BOOL: 0 / 1 */
    int big;                /* BP_INT: written as 128-bit (unsigned values from 2^63 on) */
    double r;
    char* s;                /* BP_STR */
    unsigned char* d;       /* BP_DATA */
    unsigned n;             /* BP_DATA / BP_STR length; BP_ARRAY / BP_DICT entries */
    unsigned cap;
    char** keys;            /* BP_DICT */
    struct bp** items;
};

struct bp* bp_dict(void);
struct bp* bp_array(void);
struct bp* bp_int(long long v);
struct bp* bp_uint(unsigned long long v);
struct bp* bp_real(double v);
struct bp* bp_bool(int v);
struct bp* bp_str(const char* s);
struct bp* bp_data(const void* d, unsigned n);
struct bp* bp_set(struct bp* dict, const char* key, struct bp* v);   /* takes v; returns v */
struct bp* bp_add(struct bp* array, struct bp* v);                   /* takes v; returns v */
const struct bp* bp_get(const struct bp* dict, const char* key);
int bp_num(const struct bp* v, long long* out);                      /* BP_INT / BP_REAL / BP_BOOL: 0 */
long long bp_num_or(const struct bp* dict, const char* key, long long fallback);
const char* bp_text(const struct bp* dict, const char* key);         /* a string entry or 0 */
void bp_free(struct bp* v);

unsigned char* bp_encode(const struct bp* root, unsigned* n);   /* malloc'ed, 0 = no memory */
struct bp* bp_decode(const void* data, unsigned n);             /* 0 = not a property list */
void bp_print(const struct bp* v, char* out, unsigned size);    /* short text for the log */

#endif
