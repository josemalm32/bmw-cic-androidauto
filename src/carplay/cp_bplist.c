/* nbt_carplay: Apple binary property lists (bplist00).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 * Format: "bplist00", the objects, a table of their offsets, a 32-byte trailer {6 unused, offset size, reference
 * size, object count (8), top object (8), offset table start (8)}. Object markers: 08/09 false/true, 1n integer of
 * 2^n bytes (1, 2, 4 unsigned; 8 signed; 16 = 128-bit), 2n real, 4n data, 5n ASCII string, 6n UTF-16BE string, An
 * array, Dn dictionary (keys, then values); n = 15: the count follows as an integer object.
 * As xcertplay's encoder, negative integers are written as reals.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cp_bplist.h"

static struct bp* make(int type)
{
    struct bp* v = (struct bp*)calloc(1, sizeof(*v));
    if (v)
        v->type = type;
    return v;
}

struct bp* bp_dict(void)
{
    return make(BP_DICT);
}

struct bp* bp_array(void)
{
    return make(BP_ARRAY);
}

struct bp* bp_int(long long x)
{
    struct bp* v = make(BP_INT);
    if (v)
        v->u = (unsigned long long)x;
    return v;
}

struct bp* bp_uint(unsigned long long x)
{
    struct bp* v = make(BP_INT);
    if (v) {
        v->u = x;
        v->big = x >> 63 != 0;
    }
    return v;
}

struct bp* bp_real(double x)
{
    struct bp* v = make(BP_REAL);
    if (v)
        v->r = x;
    return v;
}

struct bp* bp_bool(int x)
{
    struct bp* v = make(BP_BOOL);
    if (v)
        v->u = x != 0;
    return v;
}

struct bp* bp_str(const char* s)
{
    struct bp* v = make(BP_STR);
    if (v) {
        v->n = (unsigned)strlen(s);
        v->s = (char*)malloc(v->n + 1);
        if (!v->s) {
            free(v);
            return 0;
        }
        memcpy(v->s, s, v->n + 1);
    }
    return v;
}

struct bp* bp_data(const void* d, unsigned n)
{
    struct bp* v = make(BP_DATA);
    if (v) {
        v->d = (unsigned char*)malloc(n ? n : 1);
        if (!v->d) {
            free(v);
            return 0;
        }
        if (n)
            memcpy(v->d, d, n);
        v->n = n;
    }
    return v;
}

static int grow(struct bp* c)
{
    if (c->n < c->cap)
        return 0;
    {
        const unsigned cap = c->cap ? c->cap * 2 : 8;
        struct bp** items = (struct bp**)realloc(c->items, cap * sizeof(*items));
        if (!items)
            return -1;
        c->items = items;
        if (c->type == BP_DICT) {
            char** keys = (char**)realloc(c->keys, cap * sizeof(*keys));
            if (!keys)
                return -1;
            c->keys = keys;
        }
        c->cap = cap;
    }
    return 0;
}

struct bp* bp_set(struct bp* dict, const char* key, struct bp* v)
{
    char* k;
    if (!dict || !v || dict->type != BP_DICT || grow(dict) || !(k = (char*)malloc(strlen(key) + 1))) {
        bp_free(v);
        return 0;
    }
    strcpy(k, key);
    dict->keys[dict->n] = k;
    dict->items[dict->n++] = v;
    return v;
}

struct bp* bp_add(struct bp* array, struct bp* v)
{
    if (!array || !v || array->type != BP_ARRAY || grow(array)) {
        bp_free(v);
        return 0;
    }
    array->items[array->n++] = v;
    return v;
}

const struct bp* bp_get(const struct bp* dict, const char* key)
{
    unsigned i;
    if (!dict || dict->type != BP_DICT)
        return 0;
    for (i = 0; i < dict->n; ++i)
        if (!strcmp(dict->keys[i], key))
            return dict->items[i];
    return 0;
}

int bp_num(const struct bp* v, long long* out)
{
    if (!v)
        return -1;
    if (v->type == BP_INT || v->type == BP_BOOL)
        *out = (long long)v->u;
    else if (v->type == BP_REAL)
        *out = (long long)v->r;
    else
        return -1;
    return 0;
}

long long bp_num_or(const struct bp* dict, const char* key, long long fallback)
{
    long long v;
    return bp_num(bp_get(dict, key), &v) ? fallback : v;
}

const char* bp_text(const struct bp* dict, const char* key)
{
    const struct bp* v = bp_get(dict, key);
    return v && v->type == BP_STR ? v->s : 0;
}

void bp_free(struct bp* v)
{
    unsigned i;
    if (!v)
        return;
    if (v->type == BP_ARRAY || v->type == BP_DICT) {
        for (i = 0; i < v->n; ++i) {
            bp_free(v->items[i]);
            if (v->type == BP_DICT)
                free(v->keys[i]);
        }
        free(v->items);
        free(v->keys);
    }
    free(v->s);
    free(v->d);
    free(v);
}

/* ---- encoding ---- */

struct obj {
    const struct bp* v;   /* or a key: */
    const char* key;
    unsigned* refs;
    unsigned nrefs;
};

struct enc {
    struct obj* o;
    unsigned n, cap;
    int failed;
};

static unsigned add_obj(struct enc* e, const struct bp* v, const char* key)
{
    if (e->n == e->cap) {
        const unsigned cap = e->cap ? e->cap * 2 : 64;
        struct obj* o = (struct obj*)realloc(e->o, cap * sizeof(*o));
        if (!o) {
            e->failed = 1;
            return 0;
        }
        e->o = o;
        e->cap = cap;
    }
    memset(&e->o[e->n], 0, sizeof(e->o[0]));
    e->o[e->n].v = v;
    e->o[e->n].key = key;
    return e->n++;
}

static unsigned flatten(struct enc* e, const struct bp* v)
{
    const unsigned me = add_obj(e, v, 0);
    unsigned i, k = 0, count;
    unsigned* refs;
    if (e->failed || (v->type != BP_ARRAY && v->type != BP_DICT))
        return me;
    count = v->type == BP_DICT ? 2 * v->n : v->n;
    refs = (unsigned*)malloc((count ? count : 1) * sizeof(unsigned));
    if (!refs) {
        e->failed = 1;
        return me;
    }
    if (v->type == BP_DICT)
        for (i = 0; i < v->n; ++i)
            refs[k++] = add_obj(e, 0, v->keys[i]);
    for (i = 0; i < v->n; ++i)
        refs[k++] = flatten(e, v->items[i]);
    e->o[me].refs = refs;   /* e->o may have moved: indexed again */
    e->o[me].nrefs = count;
    return me;
}

struct out {
    unsigned char* b;
    unsigned n, cap;
    int failed;
};

static void emit(struct out* o, const void* data, unsigned n)
{
    if (o->n + n > o->cap) {
        unsigned cap = o->cap ? o->cap : 256;
        unsigned char* b;
        while (cap < o->n + n)
            cap *= 2;
        b = (unsigned char*)realloc(o->b, cap);
        if (!b) {
            o->failed = 1;
            return;
        }
        o->b = b;
        o->cap = cap;
    }
    memcpy(o->b + o->n, data, n);
    o->n += n;
}

static void emit_be(struct out* o, unsigned long long v, unsigned bytes)
{
    unsigned char b[8];
    unsigned i;
    for (i = 0; i < bytes; ++i)
        b[i] = (unsigned char)(v >> (8 * (bytes - 1 - i)));
    emit(o, b, bytes);
}

static void emit_int(struct out* o, unsigned long long v, int big)
{
    unsigned char m;
    if (big) {
        m = 0x14;
        emit(o, &m, 1);
        emit_be(o, 0, 8);
        emit_be(o, v, 8);
        return;
    }
    if (v < 0x100) {
        m = 0x10;
        emit(o, &m, 1);
        emit_be(o, v, 1);
    } else if (v < 0x10000) {
        m = 0x11;
        emit(o, &m, 1);
        emit_be(o, v, 2);
    } else if (v < 0x100000000ULL) {
        m = 0x12;
        emit(o, &m, 1);
        emit_be(o, v, 4);
    } else {
        m = 0x13;
        emit(o, &m, 1);
        emit_be(o, v, 8);
    }
}

static void emit_marker(struct out* o, unsigned type, unsigned count)
{
    unsigned char m;
    if (count < 15) {
        m = (unsigned char)(type << 4 | count);
        emit(o, &m, 1);
        return;
    }
    m = (unsigned char)(type << 4 | 15);
    emit(o, &m, 1);
    emit_int(o, count, 0);
}

static void emit_real(struct out* o, double r)
{
    unsigned long long bits;
    unsigned char m = 0x23;
    memcpy(&bits, &r, 8);
    emit(o, &m, 1);
    emit_be(o, bits, 8);
}

/* a string: ASCII as it is, anything else as UTF-16BE from UTF-8 */
static void emit_string(struct out* o, const char* s, unsigned n)
{
    unsigned i, ascii = 1, units = 0;
    for (i = 0; i < n; ++i)
        if ((unsigned char)s[i] >= 0x80)
            ascii = 0;
    if (ascii) {
        emit_marker(o, 5, n);
        emit(o, s, n);
        return;
    }
    {
        unsigned short* u = (unsigned short*)malloc(2 * (n + 1));
        if (!u) {
            o->failed = 1;
            return;
        }
        for (i = 0; i < n;) {
            const unsigned char c = (unsigned char)s[i];
            unsigned cp = c;
            if (c >= 0xe0 && i + 2 < n) {
                cp = (c & 0x0f) << 12 | ((unsigned char)s[i + 1] & 0x3f) << 6 | ((unsigned char)s[i + 2] & 0x3f);
                i += 3;
            } else if (c >= 0xc0 && i + 1 < n) {
                cp = (c & 0x1f) << 6 | ((unsigned char)s[i + 1] & 0x3f);
                i += 2;
            } else
                ++i;
            u[units++] = (unsigned short)cp;
        }
        emit_marker(o, 6, units);
        for (i = 0; i < units; ++i)
            emit_be(o, u[i], 2);
        free(u);
    }
}

unsigned char* bp_encode(const struct bp* root, unsigned* n)
{
    struct enc e;
    struct out o;
    unsigned* offsets = 0;
    unsigned i, k, refsize, offsize, table;
    memset(&e, 0, sizeof(e));
    memset(&o, 0, sizeof(o));
    flatten(&e, root);
    if (e.failed)
        goto fail;
    offsets = (unsigned*)malloc(e.n * sizeof(unsigned));
    if (!offsets)
        goto fail;
    refsize = e.n > 0xffff ? 4 : e.n > 0xff ? 2 : 1;
    emit(&o, "bplist00", 8);
    for (i = 0; i < e.n; ++i) {
        const struct obj* ob = &e.o[i];
        const struct bp* v = ob->v;
        offsets[i] = o.n;
        if (ob->key) {
            emit_string(&o, ob->key, (unsigned)strlen(ob->key));
            continue;
        }
        switch (v->type) {
        case BP_BOOL: {
            const unsigned char m = v->u ? 0x09 : 0x08;
            emit(&o, &m, 1);
            break;
        }
        case BP_INT:
            if ((long long)v->u < 0 && !v->big)
                emit_real(&o, (double)(long long)v->u);
            else
                emit_int(&o, v->u, v->big);
            break;
        case BP_REAL:
            emit_real(&o, v->r);
            break;
        case BP_STR:
            emit_string(&o, v->s, v->n);
            break;
        case BP_DATA:
            emit_marker(&o, 4, v->n);
            emit(&o, v->d, v->n);
            break;
        default:
            emit_marker(&o, v->type == BP_DICT ? 0xd : 0xa, v->type == BP_DICT ? ob->nrefs / 2 : ob->nrefs);
            for (k = 0; k < ob->nrefs; ++k)
                emit_be(&o, ob->refs[k], refsize);
            break;
        }
    }
    table = o.n;
    offsize = table > 0xffffff ? 4 : table > 0xffff ? 3 : table > 0xff ? 2 : 1;
    for (i = 0; i < e.n; ++i)
        emit_be(&o, offsets[i], offsize);
    {
        unsigned char t[32];
        memset(t, 0, sizeof(t));
        t[6] = (unsigned char)offsize;
        t[7] = (unsigned char)refsize;
        emit(&o, t, 8);
        emit_be(&o, e.n, 8);
        emit_be(&o, 0, 8);
        emit_be(&o, table, 8);
    }
    if (o.failed)
        goto fail;
    for (i = 0; i < e.n; ++i)
        free(e.o[i].refs);
    free(e.o);
    free(offsets);
    *n = o.n;
    return o.b;
fail:
    for (i = 0; i < e.n; ++i)
        free(e.o[i].refs);
    free(e.o);
    free(offsets);
    free(o.b);
    return 0;
}

/* ---- decoding ---- */

struct dec {
    const unsigned char* b;
    unsigned n;
    unsigned offsize, refsize;
    unsigned long long count, table;
};

static unsigned long long be(const unsigned char* p, unsigned bytes)
{
    unsigned long long v = 0;
    unsigned i;
    for (i = 0; i < bytes; ++i)
        v = v << 8 | p[i];
    return v;
}

static struct bp* object(struct dec* d, unsigned long long index, int depth);

/* the count of an object at p (after its marker); *p moved past it */
static int count_of(struct dec* d, unsigned low, unsigned* p, unsigned* count)
{
    unsigned bytes;
    if (low != 15) {
        *count = low;
        return 0;
    }
    if (*p >= d->n || (d->b[*p] & 0xf0) != 0x10)
        return -1;
    bytes = 1u << (d->b[*p] & 0x0f);
    if (bytes > 8 || *p + 1 + bytes > d->n)
        return -1;
    *count = (unsigned)be(d->b + *p + 1, bytes);
    *p += 1 + bytes;
    return 0;
}

static struct bp* object(struct dec* d, unsigned long long index, int depth)
{
    unsigned p, type, low, count, i;
    struct bp* v = 0;
    if (depth > 32 || index >= d->count)
        return 0;
    {
        const unsigned long long at = d->table + index * d->offsize;
        if (at + d->offsize > d->n)
            return 0;
        p = (unsigned)be(d->b + at, d->offsize);
    }
    if (p >= d->n)
        return 0;
    type = d->b[p] >> 4;
    low = d->b[p] & 15;
    ++p;
    switch (type) {
    case 0x0:
        if (low == 8 || low == 9)
            return bp_bool(low == 9);
        return 0;
    case 0x1: {
        const unsigned bytes = 1u << low;
        if (bytes > 16 || p + bytes > d->n)
            return 0;
        if (bytes == 16) {   /* 128-bit: the low 64 bits */
            v = bp_uint(be(d->b + p + 8, 8));
        } else
            v = bp_uint(be(d->b + p, bytes));
        if (v && bytes == 8)
            v->big = 0;   /* 8 bytes are signed: keep the bits, written back as 8 bytes */
        return v;
    }
    case 0x2: {
        const unsigned bytes = 1u << low;
        if (p + bytes > d->n)
            return 0;
        if (bytes == 4) {
            unsigned x = (unsigned)be(d->b + p, 4);
            float f;
            memcpy(&f, &x, 4);
            return bp_real(f);
        }
        if (bytes == 8) {
            unsigned long long x = be(d->b + p, 8);
            double r;
            memcpy(&r, &x, 8);
            return bp_real(r);
        }
        return 0;
    }
    case 0x3:   /* date: seconds since 2001 as a real */
        if (p + 8 > d->n)
            return 0;
        {
            unsigned long long x = be(d->b + p, 8);
            double r;
            memcpy(&r, &x, 8);
            return bp_real(r);
        }
    case 0x4:
        if (count_of(d, low, &p, &count) || p + count > d->n)
            return 0;
        return bp_data(d->b + p, count);
    case 0x5:
        if (count_of(d, low, &p, &count) || p + count > d->n)
            return 0;
        v = make(BP_STR);
        if (!v || !(v->s = (char*)malloc(count + 1))) {
            free(v);
            return 0;
        }
        memcpy(v->s, d->b + p, count);
        v->s[count] = 0;
        v->n = count;
        return v;
    case 0x6: {   /* UTF-16BE to UTF-8 (BMP) */
        unsigned k = 0;
        if (count_of(d, low, &p, &count) || p + 2ull * count > d->n)
            return 0;
        v = make(BP_STR);
        if (!v || !(v->s = (char*)malloc(3 * count + 1))) {
            free(v);
            return 0;
        }
        for (i = 0; i < count; ++i) {
            const unsigned c = (unsigned)be(d->b + p + 2 * i, 2);
            if (c < 0x80)
                v->s[k++] = (char)c;
            else if (c < 0x800) {
                v->s[k++] = (char)(0xc0 | c >> 6);
                v->s[k++] = (char)(0x80 | (c & 0x3f));
            } else {
                v->s[k++] = (char)(0xe0 | c >> 12);
                v->s[k++] = (char)(0x80 | ((c >> 6) & 0x3f));
                v->s[k++] = (char)(0x80 | (c & 0x3f));
            }
        }
        v->s[k] = 0;
        v->n = k;
        return v;
    }
    case 0xa:
    case 0xd: {
        const unsigned per = type == 0xd ? 2 : 1;
        if (count_of(d, low, &p, &count) || p + (unsigned long long)count * per * d->refsize > d->n)
            return 0;
        v = make(type == 0xd ? BP_DICT : BP_ARRAY);
        if (!v)
            return 0;
        for (i = 0; i < count; ++i) {
            struct bp* item;
            if (type == 0xd) {
                struct bp* key = object(d, be(d->b + p + i * d->refsize, d->refsize), depth + 1);
                if (!key || key->type != BP_STR) {
                    bp_free(key);
                    bp_free(v);
                    return 0;
                }
                item = object(d, be(d->b + p + (count + i) * d->refsize, d->refsize), depth + 1);
                if (!item || !bp_set(v, key->s, item)) {
                    bp_free(key);
                    bp_free(v);
                    return 0;
                }
                bp_free(key);
            } else {
                item = object(d, be(d->b + p + i * d->refsize, d->refsize), depth + 1);
                if (!item || !bp_add(v, item)) {
                    bp_free(v);
                    return 0;
                }
            }
        }
        return v;
    }
    default:
        return 0;
    }
}

struct bp* bp_decode(const void* data, unsigned n)
{
    struct dec d;
    const unsigned char* t;
    unsigned long long top;
    d.b = (const unsigned char*)data;
    d.n = n;
    if (n < 40 || memcmp(d.b, "bplist00", 8))
        return 0;
    t = d.b + n - 32;
    d.offsize = t[6];
    d.refsize = t[7];
    d.count = be(t + 8, 8);
    top = be(t + 16, 8);
    d.table = be(t + 24, 8);
    if (!d.offsize || d.offsize > 8 || !d.refsize || d.refsize > 8 || d.table >= n ||
        d.count > (n - d.table) / d.offsize)
        return 0;
    return object(&d, top, 0);
}

/* ---- text for the log ---- */

struct pr {
    char* o;
    unsigned size, n;
};

static void pr_add(struct pr* p, const char* s)
{
    while (*s && p->n + 1 < p->size)
        p->o[p->n++] = *s++;
    p->o[p->n] = 0;
}

static void pr(struct pr* p, const struct bp* v, int depth)
{
    char b[64];
    unsigned i;
    if (!v) {
        pr_add(p, "?");
        return;
    }
    switch (v->type) {
    case BP_BOOL:
        pr_add(p, v->u ? "true" : "false");
        break;
    case BP_INT:
        snprintf(b, sizeof(b), "%lld", (long long)v->u);
        pr_add(p, b);
        break;
    case BP_REAL:
        snprintf(b, sizeof(b), "%g", v->r);
        pr_add(p, b);
        break;
    case BP_STR:
        pr_add(p, "\"");
        pr_add(p, v->s);
        pr_add(p, "\"");
        break;
    case BP_DATA:
        snprintf(b, sizeof(b), "<%u bytes>", v->n);
        pr_add(p, b);
        break;
    default:
        if (depth > 6) {
            pr_add(p, "...");
            break;
        }
        pr_add(p, v->type == BP_DICT ? "{" : "[");
        for (i = 0; i < v->n; ++i) {
            if (i)
                pr_add(p, ", ");
            if (v->type == BP_DICT) {
                pr_add(p, v->keys[i]);
                pr_add(p, ": ");
            }
            pr(p, v->items[i], depth + 1);
        }
        pr_add(p, v->type == BP_DICT ? "}" : "]");
        break;
    }
}

void bp_print(const struct bp* v, char* out, unsigned size)
{
    struct pr p;
    p.o = out;
    p.size = size;
    p.n = 0;
    if (size)
        out[0] = 0;
    pr(&p, v, 0);
}
