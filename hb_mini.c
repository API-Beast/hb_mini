#include "hb_mini.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <stdio.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#else
#include <windows.h>
#endif

#define HB_MIN(a,b) ((a) <= (b) ? (a) : (b))
#define HB_MAX(a,b) ((a) >= (b) ? (a) : (b))
#define HB_CLAMP(v,lo,hi) HB_MIN (HB_MAX (v, lo), hi)

static inline float hbroundf (float x) { return floorf (x + .5f); }
static inline double hbround (double x) { return floor (x + .5); }
static inline int sat_add (int a, int b) { long long r = (long long) a + b; return r > INT_MAX ? INT_MAX : r < INT_MIN ? INT_MIN : (int) r; }
static inline int sat_sub (int a, int b) { long long r = (long long) a - b; return r > INT_MAX ? INT_MAX : r < INT_MIN ? INT_MIN : (int) r; }

#define DEFINE_VEC(name, T) \
typedef struct { T *a; unsigned len, cap; } name; \
static int name##_reserve (name *v, unsigned n) \
{ \
  if (n <= v->cap) return 1; \
  unsigned c = v->cap + (v->cap >> 1) + 8; \
  if (c < n) c = n; \
  T *a = (T *) realloc (v->a, (size_t) c * sizeof (T)); \
  if (!a) return 0; \
  v->a = a; v->cap = c; \
  return 1; \
} \
static int name##_resize (name *v, unsigned n, int zero) \
{ \
  if (!name##_reserve (v, n)) return 0; \
  if (zero && n > v->len) memset (v->a + v->len, 0, (size_t) (n - v->len) * sizeof (T)); \
  v->len = n; \
  return 1; \
} \
static int name##_push (name *v, T x) \
{ \
  if (!name##_reserve (v, v->len + 1)) return 0; \
  v->a[v->len++] = x; \
  return 1; \
}

static inline unsigned rd8 (const uint8_t *p) { return p[0]; }
static inline unsigned rd16 (const uint8_t *p) { return (p[0] << 8) | p[1]; }
static inline int rds16 (const uint8_t *p) { return (int16_t) rd16 (p); }
static inline unsigned rd24 (const uint8_t *p) { return (p[0] << 16) | (p[1] << 8) | p[2]; }
static inline uint32_t rd32 (const uint8_t *p) { return ((uint32_t) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static inline int32_t rds32 (const uint8_t *p) { return (int32_t) rd32 (p); }
static inline float rdf2dot14 (const uint8_t *p) { return rds16 (p) / 16384.f; }
static inline float rdfixed (const uint8_t *p) { return rds32 (p) / 65536.f; }

typedef struct { const uint8_t *d; unsigned len; } tab_t;

static inline int tab_has (tab_t t, unsigned off, unsigned n) { return off <= t.len && n <= t.len - off; }
static inline tab_t tab_sub (tab_t t, unsigned off)
{
  tab_t r = {0, 0};
  if (off && off < t.len) { r.d = t.d + off; r.len = t.len - off; }
  return r;
}
static inline tab_t tab_at (tab_t t, unsigned off)
{
  tab_t r = {0, 0};
  if (off <= t.len) { r.d = t.d + off; r.len = t.len - off; }
  return r;
}

typedef int (*hb_cmp_t) (const void *key, const uint8_t *item, const void *ud);
static int hb_bsearch (const void *key, const uint8_t *base, unsigned nmemb, unsigned stride, hb_cmp_t cmp, const void *ud)
{
  int min = 0, max = (int) nmemb - 1;
  while (min <= max)
  {
    int mid = ((unsigned) min + (unsigned) max) / 2;
    int c = cmp (key, base + mid * stride, ud);
    if (c < 0) max = mid - 1;
    else if (c > 0) min = mid + 1;
    else return mid;
  }
  return -1;
}

struct hb_blob_t
{
  int ref;
  const uint8_t *data;
  unsigned length;
  void *mapping;
  size_t mapping_len;
  void *owned;
};

static hb_blob_t *blob_wrap (const uint8_t *data, unsigned len)
{
  hb_blob_t *b = (hb_blob_t *) calloc (1, sizeof (hb_blob_t));
  if (!b) return NULL;
  b->ref = 1;
  b->data = data;
  b->length = len;
  return b;
}

hb_blob_t *
hb_blob_create_from_file_or_fail (const char *file_name)
{
#ifndef _WIN32
  int fd = open (file_name, O_RDONLY, 0);
  if (fd != -1)
  {
    struct stat st;
    if (fstat (fd, &st) != -1 && st.st_size > 0)
    {
      void *m = mmap (NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      if (m != MAP_FAILED)
      {
        close (fd);
        hb_blob_t *b = blob_wrap ((const uint8_t *) m, (unsigned) st.st_size);
        if (!b) { munmap (m, st.st_size); return NULL; }
        b->mapping = m;
        b->mapping_len = st.st_size;
        return b;
      }
    }
    close (fd);
  }
#endif
  FILE *f = fopen (file_name, "rb");
  if (!f) return NULL;
  size_t cap = 1 << 16, len = 0;
  uint8_t *data = (uint8_t *) malloc (cap);
  while (data)
  {
    size_t n = fread (data + len, 1, cap - len, f);
    len += n;
    if (len < cap) break;
    if (cap >= (1u << 29)) { free (data); data = NULL; break; }
    uint8_t *nd = (uint8_t *) realloc (data, cap * 2);
    if (!nd) { free (data); data = NULL; break; }
    data = nd; cap *= 2;
  }
  int err = ferror (f);
  fclose (f);
  if (!data || err) { free (data); return NULL; }
  hb_blob_t *b = blob_wrap (data, (unsigned) len);
  if (!b) { free (data); return NULL; }
  b->owned = data;
  return b;
}

void
hb_blob_destroy (hb_blob_t *blob)
{
  if (!blob || --blob->ref) return;
#ifndef _WIN32
  if (blob->mapping) munmap (blob->mapping, blob->mapping_len);
#endif
  free (blob->owned);
  free (blob);
}

#define SCALAR_INVALID INT_MIN
#define SCALAR_MULT ((float) (1 << 30))

typedef struct { unsigned length; int *values; } scalar_cache_t;

static scalar_cache_t *scalar_cache_create (unsigned count)
{
  if (!count) return NULL;
  scalar_cache_t *c = (scalar_cache_t *) malloc (sizeof (scalar_cache_t) + count * sizeof (int));
  if (!c) return NULL;
  c->length = count;
  c->values = (int *) (c + 1);
  for (unsigned i = 0; i < count; i++) c->values[i] = SCALAR_INVALID;
  return c;
}

static float var_region_axis_eval (const uint8_t *a, int coord)
{
  int start = rds16 (a), peak = rds16 (a + 2), end = rds16 (a + 4);
  if (peak == 0 || coord == peak) return 1.f;
  else if (coord == 0) return 0.f;
  if (start > peak || peak > end) return 1.f;
  if (start < 0 && end > 0) return 1.f;
  if (coord <= start || end <= coord) return 0.f;
  if (coord < peak) return (float) (coord - start) / (peak - start);
  else return (float) (end - coord) / (end - peak);
}

typedef struct { tab_t t; int ok; } ivs_t;

static int ivs_valid (tab_t t)
{
  if (!tab_has (t, 0, 8) || rd16 (t.d) != 1) return 0;
  uint32_t ro = rd32 (t.d + 2);
  if (ro)
  {
    tab_t r = tab_at (t, ro);
    if (!tab_has (r, 0, 4)) return 0;
    unsigned ac = rd16 (r.d), rc = rd16 (r.d + 2) & 0x7FFF;
    if (!tab_has (r, 4, ac * rc * 6)) return 0;
  }
  unsigned n = rd16 (t.d + 6);
  if (!tab_has (t, 8, n * 4)) return 0;
  return 1;
}

static int var_data_valid (tab_t vd)
{
  if (!tab_has (vd, 0, 6)) return 0;
  unsigned items = rd16 (vd.d), wsc = rd16 (vd.d + 2), rcount = rd16 (vd.d + 4);
  unsigned wc = wsc & 0x7FFF, lw = wsc & 0x8000;
  if (!tab_has (vd, 6, rcount * 2) || wc > rcount) return 0;
  unsigned row = (wc + rcount) * (lw ? 2 : 1);
  return tab_has (vd, 6 + rcount * 2, items * row);
}

static unsigned ivs_region_count (tab_t ivs)
{
  if (!ivs.d) return 0;
  uint32_t ro = rd32 (ivs.d + 2);
  if (!ro) return 0;
  return rd16 (ivs.d + ro + 2) & 0x7FFF;
}

static float ivs_region_eval (tab_t ivs, unsigned region, const int *coords, unsigned coord_len, scalar_cache_t *cache)
{
  uint32_t ro = rd32 (ivs.d + 2);
  if (!ro) return 0.f;
  const uint8_t *r = ivs.d + ro;
  unsigned axis_count = rd16 (r), region_count = rd16 (r + 2) & 0x7FFF;
  if (region >= region_count) return 0.f;
  if (cache && region < cache->length)
  {
    int v = cache->values[region];
    if (!v) return 0.f;
    if (v != SCALAR_INVALID) return v * (1.f / SCALAR_MULT);
  }
  else if (cache)
    return 0.f;
  const uint8_t *axes = r + 4 + region * axis_count * 6;
  float v = 1.f;
  for (unsigned i = 0; i < axis_count; i++)
  {
    int coord = i < coord_len ? coords[i] : 0;
    float f = var_region_axis_eval (axes + i * 6, coord);
    if (f == 0.f) { v = 0.f; break; }
    v *= f;
  }
  if (cache) cache->values[region] = (int) hbroundf (v * SCALAR_MULT);
  return v;
}

static float ivs_get_delta (tab_t ivs, uint32_t index, const int *coords, unsigned coord_count, scalar_cache_t *cache)
{
  if (!ivs.d) return 0.f;
  unsigned outer = index >> 16, inner = index & 0xFFFF;
  if (outer >= rd16 (ivs.d + 6)) return 0.f;
  uint32_t off = rd32 (ivs.d + 8 + outer * 4);
  if (!off) return 0.f;
  tab_t vd = tab_at (ivs, off);
  if (!var_data_valid (vd)) return 0.f;
  unsigned items = rd16 (vd.d), wsc = rd16 (vd.d + 2), count = rd16 (vd.d + 4);
  if (!count) return 0.f;
  if (inner >= items) return 0.f;
  int is_long = (wsc & 0x8000) != 0;
  unsigned word_count = wsc & 0x7FFF;
  unsigned scount = is_long ? count : word_count;
  unsigned lcount = is_long ? word_count : 0;
  unsigned row_size = (word_count + count) * (is_long ? 2 : 1);
  const uint8_t *regions = vd.d + 6;
  const uint8_t *row = vd.d + 6 + count * 2 + inner * row_size;
  float delta = 0.f;
  unsigned i = 0;
  for (; i < lcount; i++, row += 4)
  {
    float s = ivs_region_eval (ivs, rd16 (regions + i * 2), coords, coord_count, cache);
    if (s) delta += s * rds32 (row);
  }
  for (; i < scount; i++, row += 2)
  {
    float s = ivs_region_eval (ivs, rd16 (regions + i * 2), coords, coord_count, cache);
    if (s) delta += s * rds16 (row);
  }
  for (; i < count; i++, row += 1)
  {
    float s = ivs_region_eval (ivs, rd16 (regions + i * 2), coords, coord_count, cache);
    if (s) delta += s * (int8_t) row[0];
  }
  return delta;
}

static uint32_t delta_set_index_map (tab_t m, uint32_t v)
{
  if (!m.d || m.len < 2) return v;
  unsigned format = m.d[0], ef = m.d[1];
  unsigned count, hdr;
  if (format == 0) { if (m.len < 4) return v; count = rd16 (m.d + 2); hdr = 4; }
  else if (format == 1) { if (m.len < 6) return v; count = rd32 (m.d + 2); hdr = 6; }
  else return v;
  unsigned w = ((ef >> 4) & 3) + 1;
  if (!count || !tab_has (m, hdr, count * w)) return v;
  if (v >= count) v = count - 1;
  const uint8_t *p = m.d + hdr + w * v;
  uint32_t u = 0;
  for (; w; w--) u = (u << 8) + *p++;
  unsigned n = (ef & 0xF) + 1;
  return ((u >> n) << 16) | (u & ((1u << n) - 1));
}

typedef struct plan_node_t plan_node_t;

typedef struct
{
  int valid;
  const uint8_t *subtable;
  unsigned subtable_len;
  int format;
  const uint8_t *uvs;
  unsigned uvs_len;
  uint32_t *cache;
  unsigned seg_count, glyph_id_array_len;
} cmap_accel_t;

typedef struct
{
  int init;
  unsigned num_long_metrics, num_bearings, num_advances, num_glyphs, default_advance;
  tab_t hvar, ivs, adv_map;
} hmtx_accel_t;

struct hb_face_t
{
  int ref;
  hb_blob_t *blob;
  unsigned index, base_offset;
  tab_t sfnt;
  unsigned upem, num_glyphs;
  tab_t head, maxp, hhea, hmtx, os2, cmap, fvar, avar, mvar, mvar_ivs;
  tab_t glyf, loca, gvar, cff, gdef, gsub, gpos, colr, cpal, cblc, cbdt, sbix;
  cmap_accel_t cmap_accel;
  hmtx_accel_t hmtx_accel;
  int glyf_init, glyf_short;
  unsigned glyf_num_glyphs;
  int gvar_init;
  unsigned gvar_glyph_count;
  struct glyf_scratch_t *glyf_scratch;
  void *cff_accel;
  void *colr_accel;
  void *layout_accel;
  plan_node_t *plans;
};

static tab_t face_table (hb_face_t *face, hb_tag_t tag)
{
  tab_t r = {0, 0};
  const uint8_t *d = face->sfnt.d;
  if (!d || face->sfnt.len < 12) return r;
  unsigned n = rd16 (d + 4);
  if (!tab_has (face->sfnt, 12, n * 16)) return r;
  int found = -1;
  if (n < 16)
  {
    for (unsigned i = 0; i < n; i++)
      if (rd32 (d + 12 + i * 16) == tag) { found = i; break; }
  }
  else
  {
    int min = 0, max = (int) n - 1;
    while (min <= max)
    {
      int mid = ((unsigned) min + (unsigned) max) / 2;
      uint32_t t = rd32 (d + 12 + mid * 16);
      if (tag < t) max = mid - 1;
      else if (tag > t) min = mid + 1;
      else { found = mid; break; }
    }
  }
  if (found < 0) return r;
  const uint8_t *rec = d + 12 + found * 16;
  uint32_t off = rd32 (rec + 8), len = rd32 (rec + 12);
  const hb_blob_t *b = face->blob;
  if (off > UINT32_MAX - face->base_offset) return r;
  off += face->base_offset;
  if (!len || off >= b->length) return r;
  if (len > b->length - off) len = b->length - off;
  r.d = b->data + off;
  r.len = len;
  return r;
}

static void face_load_tables (hb_face_t *face)
{
  face->head = face_table (face, HB_TAG('h','e','a','d'));
  if (!(tab_has (face->head, 0, 54) && rd16 (face->head.d) == 1 && rd32 (face->head.d + 12) == 0x5F0F3CF5u))
    face->head.d = NULL, face->head.len = 0;
  face->maxp = face_table (face, HB_TAG('m','a','x','p'));
  {
    tab_t m = face->maxp;
    int ok = tab_has (m, 0, 6);
    if (ok)
    {
      unsigned major = rd16 (m.d), minor = rd16 (m.d + 2);
      if (major == 1) ok = tab_has (m, 6, 26);
      else ok = major == 0 && minor == 0x5000u;
    }
    if (!ok) face->maxp.d = NULL, face->maxp.len = 0;
  }
  face->hhea = face_table (face, HB_TAG('h','h','e','a'));
  if (!(tab_has (face->hhea, 0, 36) && rd16 (face->hhea.d) == 1)) face->hhea.d = NULL, face->hhea.len = 0;
  face->hmtx = face_table (face, HB_TAG('h','m','t','x'));
  face->os2 = face_table (face, HB_TAG('O','S','/','2'));
  {
    tab_t o = face->os2;
    int ok = tab_has (o, 0, 78);
    if (ok && rd16 (o.d) >= 1) ok = tab_has (o, 78, 8);
    if (ok && rd16 (o.d) >= 2) ok = tab_has (o, 86, 10);
    if (ok && rd16 (o.d) >= 5) ok = tab_has (o, 96, 4);
    if (!ok) face->os2.d = NULL, face->os2.len = 0;
  }
  face->cmap = face_table (face, HB_TAG('c','m','a','p'));
  if (!(tab_has (face->cmap, 0, 4) && rd16 (face->cmap.d) == 0 && tab_has (face->cmap, 4, rd16 (face->cmap.d + 2) * 8)))
    face->cmap.d = NULL, face->cmap.len = 0;
  face->fvar = face_table (face, HB_TAG('f','v','a','r'));
  {
    tab_t f = face->fvar;
    int ok = tab_has (f, 0, 16) && rd16 (f.d) == 1;
    if (ok)
    {
      unsigned first = rd16 (f.d + 4), ac = rd16 (f.d + 8), as = rd16 (f.d + 10), ic = rd16 (f.d + 12), is = rd16 (f.d + 14);
      ok = as == 20 && is >= ac * 4 + 4 && first && tab_has (f, first, ac * 20) && tab_has (f, first + ac * 20, ic * is);
    }
    if (!ok) face->fvar.d = NULL, face->fvar.len = 0;
  }
  face->avar = face_table (face, HB_TAG('a','v','a','r'));
  {
    tab_t a = face->avar;
    int ok = tab_has (a, 0, 8) && rd16 (a.d) == 1;
    if (ok)
    {
      unsigned count = rd16 (a.d + 6), off = 8;
      for (unsigned i = 0; ok && i < count; i++)
      {
        ok = tab_has (a, off, 2) && tab_has (a, off + 2, rd16 (a.d + off) * 4);
        if (ok) off += 2 + rd16 (a.d + off) * 4;
      }
    }
    if (!ok) face->avar.d = NULL, face->avar.len = 0;
  }
  face->mvar = face_table (face, HB_TAG('M','V','A','R'));
  {
    tab_t m = face->mvar;
    int ok = tab_has (m, 0, 12) && rd16 (m.d) == 1 && rd16 (m.d + 6) >= 8;
    if (ok)
    {
      unsigned off = rd16 (m.d + 10);
      if (off) { face->mvar_ivs = tab_at (m, off); ok = ivs_valid (face->mvar_ivs); }
      ok = ok && tab_has (m, 12, rd16 (m.d + 8) * rd16 (m.d + 6));
    }
    if (!ok) { face->mvar.d = NULL; face->mvar.len = 0; face->mvar_ivs.d = NULL; face->mvar_ivs.len = 0; }
  }
  face->glyf = face_table (face, HB_TAG('g','l','y','f'));
  face->loca = face_table (face, HB_TAG('l','o','c','a'));
  face->gvar = face_table (face, HB_TAG('g','v','a','r'));
  face->cff = face_table (face, HB_TAG('C','F','F',' '));
  face->gdef = face_table (face, HB_TAG('G','D','E','F'));
  face->gsub = face_table (face, HB_TAG('G','S','U','B'));
  face->gpos = face_table (face, HB_TAG('G','P','O','S'));
  face->colr = face_table (face, HB_TAG('C','O','L','R'));
  face->cpal = face_table (face, HB_TAG('C','P','A','L'));
  face->cblc = face_table (face, HB_TAG('C','B','L','C'));
  face->cbdt = face_table (face, HB_TAG('C','B','D','T'));
  face->sbix = face_table (face, HB_TAG('s','b','i','x'));

  unsigned upem = face->head.d ? rd16 (face->head.d + 18) : 0;
  face->upem = 16 <= upem && upem <= 16384 ? upem : 1000;
  face->num_glyphs = face->maxp.d ? rd16 (face->maxp.d + 4) : 0;
}

static void face_select (hb_face_t *face, hb_blob_t *blob, unsigned index)
{
  const uint8_t *d = blob->data;
  unsigned len = blob->length;
  face->sfnt.d = NULL; face->sfnt.len = 0;
  if (len < 4) return;
  uint32_t tag = rd32 (d);
  if (tag == 0x00010000u || tag == HB_TAG('O','T','T','O') || tag == HB_TAG('t','r','u','e') || tag == HB_TAG('t','y','p','1'))
  {
    if (len < 12 || len - 12 < rd16 (d + 4) * 16u) return;
    face->sfnt.d = d; face->sfnt.len = len;
  }
  else if (tag == 0x00000100u)
  {
    if (len < 16) return;
    uint32_t data = rd32 (d), map = rd32 (d + 4), data_len = rd32 (d + 8);
    if (data > len || len - data < data_len || map > len || len - map < 28) return;
    const uint8_t *m = d + map;
    unsigned tl = rd16 (m + 24);
    if (map + tl > len || len - map - tl < 2) return;
    const uint8_t *types = m + tl;
    unsigned ntypes = rd16 (types) + 1;
    if ((len - map - tl - 2) / 8 < ntypes) return;
    for (unsigned i = 0; i < ntypes; i++)
    {
      const uint8_t *t = types + 2 + i * 8;
      if (rd32 (t) != HB_TAG('s','f','n','t')) continue;
      unsigned count = rd16 (t + 4) + 1;
      const uint8_t *recs = types + rd16 (t + 6);
      if ((uintptr_t) (recs - d) > len || (len - (recs - d)) / 12 < count) return;
      if (index >= count) continue;
      uint32_t off = data + rd24 (recs + index * 12 + 5);
      if (off > len || len - off < 4 || len - off - 4 < rd32 (d + off)) return;
      off += 4;
      if (len - off < 12 || (len - off - 12) / 16 < rd16 (d + off + 4)) return;
      face->sfnt.d = d + off; face->sfnt.len = len - off;
      face->base_offset = off;
      return;
    }
  }
  else if (tag == HB_TAG('t','t','c','f'))
  {
    if (len < 12) return;
    unsigned major = rd16 (d + 4);
    if (major != 1 && major != 2) return;
    uint32_t n = rd32 (d + 8);
    if ((len - 12) / 4 < n) return;
    if (index >= n) return;
    uint32_t off = rd32 (d + 12 + index * 4);
    if (off >= len || len - off < 12 || (len - off - 12) / 16 < rd16 (d + off + 4)) return;
    face->sfnt.d = d + off; face->sfnt.len = len - off;
  }
}

hb_face_t *
hb_face_create (hb_blob_t *blob, unsigned int index)
{
  hb_face_t *face = (hb_face_t *) calloc (1, sizeof (hb_face_t));
  if (!face) return NULL;
  face->ref = 1;
  if (blob)
  {
    blob->ref++;
    face->blob = blob;
    face_select (face, blob, index & 0xFFFF);
  }
  face->index = index;
  face_load_tables (face);
  return face;
}

static void face_free_accels (hb_face_t *face);

void
hb_face_destroy (hb_face_t *face)
{
  if (!face || --face->ref) return;
  face_free_accels (face);
  free (face->cmap_accel.cache);
  hb_blob_destroy (face->blob);
  free (face);
}

unsigned int
hb_face_get_glyph_count (const hb_face_t *face)
{
  return face ? face->num_glyphs : 0;
}

static unsigned fvar_axis_count (hb_face_t *face) { return face->fvar.d ? rd16 (face->fvar.d + 8) : 0; }
static const uint8_t *fvar_axis (hb_face_t *face, unsigned i) { return face->fvar.d + rd16 (face->fvar.d + 4) + i * 20; }

static void fvar_axis_coords (const uint8_t *a, float *min, float *def, float *max)
{
  *def = rdfixed (a + 8);
  *min = HB_MIN (*def, rdfixed (a + 4));
  *max = HB_MAX (*def, rdfixed (a + 12));
}

static void fvar_axis_info (const uint8_t *a, unsigned index, hb_ot_var_axis_info_t *info)
{
  info->axis_index = index;
  info->tag = rd32 (a);
  info->name_id = rd16 (a + 18);
  info->flags = rd16 (a + 16);
  fvar_axis_coords (a, &info->min_value, &info->default_value, &info->max_value);
  info->reserved = 0;
}

unsigned int
hb_ot_var_get_axis_count (hb_face_t *face)
{
  return fvar_axis_count (face);
}

unsigned int
hb_ot_var_get_axis_infos (hb_face_t *face, unsigned int start_offset, unsigned int *axes_count, hb_ot_var_axis_info_t *axes_array)
{
  unsigned count = fvar_axis_count (face);
  if (axes_count && axes_array)
  {
    unsigned n = start_offset < count ? HB_MIN (*axes_count, count - start_offset) : 0;
    for (unsigned i = 0; i < n; i++)
      fvar_axis_info (fvar_axis (face, start_offset + i), start_offset + i, &axes_array[i]);
    *axes_count = n;
  }
  return count;
}

static float avar_map_float (const uint8_t *map, unsigned len, float value)
{
#define FROM(i) rdf2dot14 (map + (i) * 4)
#define TO(i) rdf2dot14 (map + (i) * 4 + 2)
  if (len < 2)
  {
    if (!len) return value;
    return value - FROM (0) + TO (0);
  }
  unsigned start = 0, end = len, i;
  if (FROM (start) == -1 && TO (start) == -1 && FROM (start + 1) == -1) start++;
  if (FROM (end - 1) == +1 && TO (end - 1) == +1 && FROM (end - 2) == +1) end--;
  for (i = start; i < end; i++)
    if (value == FROM (i)) break;
  if (i < end)
  {
    unsigned j = i;
    for (; j + 1 < end; j++)
      if (value != FROM (j + 1)) break;
    if (i == j) return TO (i);
    if (i + 2 == j) return TO (i + 1);
    if (value < 0) return TO (j);
    if (value > 0) return TO (i);
    return fabsf (TO (i)) < fabsf (TO (j)) ? TO (i) : TO (j);
  }
  for (i = start; i < end; i++)
    if (value < FROM (i)) break;
  if (i == start) return value - FROM (start) + TO (start);
  if (i == end) return value - FROM (end - 1) + TO (end - 1);
  float denom = FROM (i) - FROM (i - 1);
  return TO (i - 1) + ((TO (i) - TO (i - 1)) * (value - FROM (i - 1))) / denom;
#undef FROM
#undef TO
}

static void var_normalize_coords (hb_face_t *face, unsigned count, const float *design, int *normalized)
{
  for (unsigned i = 0; i < count; i++)
  {
    float min, def, max;
    fvar_axis_coords (fvar_axis (face, i), &min, &def, &max);
    float v = HB_CLAMP (design[i], min, max), n;
    if (v == def) n = 0;
    else if (v < def) n = (v - def) / (def - min);
    else n = (v - def) / (max - def);
    normalized[i] = (int) hbroundf (n * 65536.0f);
  }
  if (face->avar.d)
  {
    unsigned n = HB_MIN (count, rd16 (face->avar.d + 6));
    const uint8_t *map = face->avar.d + 8;
    for (unsigned i = 0; i < n; i++)
    {
      unsigned len = rd16 (map);
      normalized[i] = (int) hbroundf (avar_map_float (map + 2, len, normalized[i] / 65536.f) * 65536.f);
      map += 2 + len * 4;
    }
  }
  for (unsigned i = 0; i < count; i++)
    normalized[i] = (normalized[i] + 2) >> 2;
}

typedef struct { uint32_t values[256]; } u32_cache_t;

struct hb_font_t
{
  int ref;
  unsigned serial, serial_coords;
  hb_face_t *face;
  int32_t x_scale, y_scale;
  int is_synthetic;
  float x_embolden, y_embolden;
  int embolden_in_place;
  int32_t x_strength, y_strength;
  float slant, slant_xy;
  float x_multf, y_multf;
  int64_t x_mult, y_mult;
  unsigned x_ppem, y_ppem;
  float ptem;
  unsigned instance_index;
  int has_nonzero_coords;
  unsigned num_coords;
  int *coords;
  float *design_coords;

  unsigned cached_serial_coords;
  int cached_valid;
  u32_cache_t *h_advance_cache;
  scalar_cache_t *h_ivs_cache;
  scalar_cache_t *gvar_cache;
  int gvar_cache_valid;
  scalar_cache_t *gdef_cache;
  unsigned gdef_cache_serial;
  int gdef_cache_valid;
};

static void font_changed (hb_font_t *font)
{
  float upem = font->face->upem;
  font->x_multf = font->x_scale / upem;
  font->y_multf = font->y_scale / upem;
  font->x_mult = (int64_t) font->x_scale * 0x10000 / upem;
  font->y_mult = (int64_t) font->y_scale * 0x10000 / upem;
  font->is_synthetic = font->x_embolden || font->y_embolden || font->slant;
  font->x_strength = (int32_t) hbroundf (abs (font->x_scale) * font->x_embolden);
  font->y_strength = (int32_t) hbroundf (abs (font->y_scale) * font->y_embolden);
  font->slant_xy = font->y_scale ? font->slant * font->x_scale / font->y_scale : 0.f;
  font->serial++;
}

static inline hb_position_t em_mult (int32_t v, int64_t mult) { return (hb_position_t) ((v * mult + 32768) >> 16); }
static inline hb_position_t em_scale_x (hb_font_t *f, int32_t v) { return em_mult (v, f->x_mult); }
static inline hb_position_t em_scale_y (hb_font_t *f, int32_t v) { return em_mult (v, f->y_mult); }
static inline hb_position_t em_scalef_x (hb_font_t *f, float v) { return (hb_position_t) hbroundf (v * f->x_multf); }
static inline hb_position_t em_scalef_y (hb_font_t *f, float v) { return (hb_position_t) hbroundf (v * f->y_multf); }
static inline float em_fscale_x (hb_font_t *f, int32_t v) { return (float) v * f->x_multf; }
static inline float em_fscale_y (hb_font_t *f, int32_t v) { return (float) v * f->y_multf; }
static inline float em_fscalef_x (hb_font_t *f, float v) { return v * f->x_multf; }
static inline float em_fscalef_y (hb_font_t *f, float v) { return v * f->y_multf; }

static void font_clear_coord_caches (hb_font_t *font)
{
  free (font->h_advance_cache); font->h_advance_cache = NULL;
  free (font->h_ivs_cache); font->h_ivs_cache = NULL;
  free (font->gvar_cache); font->gvar_cache = NULL;
  font->gvar_cache_valid = 0;
}

static void font_check_serial (hb_font_t *font)
{
  if (!font->cached_valid || font->cached_serial_coords != font->serial_coords)
  {
    font_clear_coord_caches (font);
    font->cached_serial_coords = font->serial_coords;
    font->cached_valid = 1;
  }
}

static void font_adopt_var_coords (hb_font_t *font, int *coords, float *design, unsigned len)
{
  free (font->coords);
  free (font->design_coords);
  font->coords = coords;
  font->design_coords = design;
  font->num_coords = len;
  font->has_nonzero_coords = 0;
  for (unsigned i = 0; i < len; i++)
    if (coords[i]) font->has_nonzero_coords = 1;
  font_changed (font);
  font->serial_coords = font->serial;
}

static void font_set_variations (hb_font_t *font, const hb_variation_t *variations, unsigned variations_length, int named)
{
  hb_face_t *face = font->face;
  unsigned count = fvar_axis_count (face);
  int *normalized = count ? (int *) calloc (count, sizeof (int)) : NULL;
  float *design = count ? (float *) calloc (count, sizeof (float)) : NULL;
  if (count && !(normalized && design)) { free (normalized); free (design); return; }
  for (unsigned i = 0; i < count; i++)
  {
    float min, def, max;
    fvar_axis_coords (fvar_axis (face, i), &min, &def, &max);
    design[i] = def;
  }
  if (named >= 0)
  {
    const uint8_t *f = face->fvar.d;
    unsigned ic = rd16 (f + 12), is = rd16 (f + 14);
    if ((unsigned) named < ic)
    {
      const uint8_t *inst = f + rd16 (f + 4) + count * 20 + named * is;
      for (unsigned i = 0; i < count; i++)
        design[i] = rdfixed (inst + 4 + i * 4);
    }
  }
  for (unsigned i = 0; i < variations_length; i++)
    for (unsigned a = 0; a < count; a++)
      if (rd32 (fvar_axis (face, a)) == variations[i].tag)
        design[a] = variations[i].value;
  var_normalize_coords (face, count, design, normalized);
  font_adopt_var_coords (font, normalized, design, count);
}

hb_font_t *
hb_font_create (hb_face_t *face)
{
  if (!face) return NULL;
  hb_font_t *font = (hb_font_t *) calloc (1, sizeof (hb_font_t));
  if (!font) return NULL;
  font->ref = 1;
  face->ref++;
  font->face = face;
  font->x_scale = font->y_scale = face->upem;
  font->embolden_in_place = 1;
  font->x_multf = font->y_multf = 1.f;
  font->x_mult = font->y_mult = 1 << 16;
  font->instance_index = 0xFFFFFFFFu;
  if (face->index >> 16)
  {
    font->instance_index = (face->index >> 16) - 1;
    font_set_variations (font, NULL, 0, (int) font->instance_index);
  }
  else
    font_set_variations (font, NULL, 0, -1);
  return font;
}

static hb_font_t *font_create_unsynthetic (hb_font_t *parent)
{
  hb_font_t *font = hb_font_create (parent->face);
  if (!font) { parent->ref++; return parent; }
  font->x_scale = parent->x_scale;
  font->y_scale = parent->y_scale;
  font->x_ppem = parent->x_ppem;
  font->y_ppem = parent->y_ppem;
  font->ptem = parent->ptem;
  unsigned n = parent->num_coords;
  if (n)
  {
    int *c = (int *) calloc (n, sizeof (int));
    float *d = (float *) calloc (n, sizeof (float));
    if (c && d)
    {
      memcpy (c, parent->coords, n * sizeof (int));
      memcpy (d, parent->design_coords, n * sizeof (float));
      font_adopt_var_coords (font, c, d, n);
    }
    else { free (c); free (d); }
  }
  font_changed (font);
  font->serial_coords = font->serial;
  return font;
}

void
hb_font_destroy (hb_font_t *font)
{
  if (!font || --font->ref) return;
  font_clear_coord_caches (font);
  free (font->gdef_cache);
  hb_face_destroy (font->face);
  free (font->coords);
  free (font->design_coords);
  free (font);
}

void
hb_font_set_scale (hb_font_t *font, int x_scale, int y_scale)
{
  if (font->x_scale == x_scale && font->y_scale == y_scale) return;
  font->x_scale = x_scale;
  font->y_scale = y_scale;
  font_changed (font);
}

void
hb_font_set_ppem (hb_font_t *font, unsigned int x_ppem, unsigned int y_ppem)
{
  if (font->x_ppem == x_ppem && font->y_ppem == y_ppem) return;
  font->x_ppem = x_ppem;
  font->y_ppem = y_ppem;
  font_changed (font);
}

void
hb_font_set_variations (hb_font_t *font, const hb_variation_t *variations, unsigned int variations_length)
{
  font_set_variations (font, variations, variations_length, font->instance_index != 0xFFFFFFFFu ? (int) font->instance_index : -1);
}

void
hb_font_set_synthetic_bold (hb_font_t *font, float x_embolden, float y_embolden, hb_bool_t in_place)
{
  if (font->x_embolden == x_embolden && font->y_embolden == y_embolden && font->embolden_in_place == !!in_place) return;
  font->x_embolden = x_embolden;
  font->y_embolden = y_embolden;
  font->embolden_in_place = !!in_place;
  font_changed (font);
}

void
hb_font_get_synthetic_bold (hb_font_t *font, float *x_embolden, float *y_embolden, hb_bool_t *in_place)
{
  if (x_embolden) *x_embolden = font->x_embolden;
  if (y_embolden) *y_embolden = font->y_embolden;
  if (in_place) *in_place = font->embolden_in_place;
}

void
hb_font_set_synthetic_slant (hb_font_t *font, float slant)
{
  if (font->slant == slant) return;
  font->slant = slant;
  font_changed (font);
}

float
hb_style_get_value (hb_font_t *font, hb_style_tag_t style_tag)
{
  hb_face_t *face = font->face;
  unsigned count = fvar_axis_count (face);
  for (unsigned i = 0; i < count; i++)
    if (rd32 (fvar_axis (face, i)) == (hb_tag_t) style_tag)
    {
      if (i < font->num_coords) return font->design_coords[i];
      float min, def, max;
      fvar_axis_coords (fvar_axis (face, i), &min, &def, &max);
      return def;
    }
  if (style_tag != HB_STYLE_TAG_WEIGHT) return 0;
  const uint8_t *o = face->os2.d;
  if (o && (rd16 (o + 4) || rd16 (o + 6) || rd16 (o + 64) || rd16 (o + 66)))
    return rd16 (o + 4);
  return face->head.d && (rd16 (face->head.d + 44) & 1) ? 700 : 400;
}

static int mvar_cmp (const void *key, const uint8_t *item, const void *ud)
{
  hb_tag_t a = *(const hb_tag_t *) key, b = rd32 (item);
  return a < b ? -1 : a > b ? 1 : 0;
}

static float mvar_get_var (hb_font_t *font, hb_tag_t tag)
{
  hb_face_t *face = font->face;
  if (!face->mvar.d) return 0.f;
  unsigned size = rd16 (face->mvar.d + 6), count = rd16 (face->mvar.d + 8);
  int i = hb_bsearch (&tag, face->mvar.d + 12, count, size, mvar_cmp, NULL);
  if (i < 0) return 0.f;
  return ivs_get_delta (face->mvar_ivs, rd32 (face->mvar.d + 12 + i * size + 4), font->coords, font->num_coords, NULL);
}

static int metric_y (hb_font_t *font, tab_t t, int has, unsigned off, hb_tag_t tag, int sign, hb_position_t *pos)
{
  if (!has) return 0;
  float v = rds16 (t.d + off) + mvar_get_var (font, tag);
  if (sign > 0) v = (float) fabs ((double) v);
  else if (sign < 0) v = (float) -fabs ((double) v);
  *pos = em_scalef_y (font, v);
  return 1;
}

static int font_metric (hb_font_t *font, int which, hb_position_t *pos)
{
  hb_face_t *face = font->face;
  const uint8_t *o = face->os2.d;
  int os2_has = o && (rd16 (o + 4) || rd16 (o + 6) || rd16 (o + 64) || rd16 (o + 66));
  int typo = o && (rd16 (o + 62) & (1u << 7));
  int hhea_has = face->hhea.d && rd16 (face->hhea.d);
  static const unsigned os2_off[3] = {68, 70, 72}, hhea_off[3] = {4, 6, 8};
  static const hb_tag_t tags[3] = {HB_TAG('h','a','s','c'), HB_TAG('h','d','s','c'), HB_TAG('h','l','g','p')};
  static const int signs[3] = {1, -1, 0};
  return (typo && metric_y (font, face->os2, os2_has, os2_off[which], tags[which], signs[which], pos)) ||
	 metric_y (font, face->hhea, hhea_has, hhea_off[which], tags[which], signs[which], pos);
}

hb_bool_t
hb_font_get_h_extents (hb_font_t *font, hb_font_extents_t *extents)
{
  memset (extents, 0, sizeof (*extents));
  int ret = font_metric (font, 0, &extents->ascender) &&
	    font_metric (font, 1, &extents->descender) &&
	    font_metric (font, 2, &extents->line_gap);
  if (ret)
    extents->ascender += font->y_scale < 0 ? -font->y_strength : font->y_strength;
  return ret;
}

static int cmap_subtable_valid (const uint8_t *st, unsigned len)
{
  if (len < 2) return 0;
  switch (rd16 (st))
  {
  case 0: return len >= 262;
  case 4: return len >= 14 && len - 14 >= 2 + 4u * rd16 (st + 6);
  case 6: return len >= 10 && len - 10 >= 2u * rd16 (st + 8);
  case 10: return len >= 20 && (len - 20) / 2 >= rd32 (st + 16);
  case 12: case 13: return len >= 16 && (len - 16) / 12 >= rd32 (st + 12);
  case 14:
  {
    if (len < 10) return 0;
    uint32_t n = rd32 (st + 6);
    if ((len - 10) / 11 < n) return 0;
    return 1;
  }
  default: return 1;
  }
}

static int enc_cmp (const void *key, const uint8_t *item, const void *ud)
{
  const unsigned *k = (const unsigned *) key;
  unsigned p = rd16 (item), e = rd16 (item + 2);
  if (k[0] != p) return k[0] < p ? -1 : 1;
  if (k[1] != 0xFFFF && k[1] != e) return k[1] < e ? -1 : 1;
  return 0;
}

static const uint8_t *cmap_find_subtable (tab_t cmap, unsigned platform, unsigned encoding, unsigned *len)
{
  unsigned key[2] = {platform, encoding};
  int i = hb_bsearch (key, cmap.d + 4, rd16 (cmap.d + 2), 8, enc_cmp, NULL);
  if (i < 0) return NULL;
  uint32_t off = rd32 (cmap.d + 4 + i * 8 + 4);
  if (!off || off >= cmap.len) return NULL;
  const uint8_t *st = cmap.d + off;
  *len = cmap.len - off;
  if (!cmap_subtable_valid (st, *len)) return NULL;
  return st;
}

static void cmap_accel_init (hb_face_t *face)
{
  cmap_accel_t *a = &face->cmap_accel;
  a->valid = 1;
  tab_t cmap = face->cmap;
  if (!cmap.d) return;
  static const unsigned order[][2] = {{3,0},{3,10},{0,6},{0,4},{3,1},{0,3},{0,2},{0,1},{0,0},{1,0},{1,0xFFFF}};
  for (unsigned i = 0; i < sizeof (order) / sizeof (order[0]); i++)
  {
    unsigned len;
    const uint8_t *st = cmap_find_subtable (cmap, order[i][0], order[i][1], &len);
    if (st) { a->subtable = st; a->subtable_len = len; break; }
  }
  {
    unsigned len;
    const uint8_t *st = cmap_find_subtable (cmap, 0, 5, &len);
    if (st && rd16 (st) == 14) { a->uvs = st; a->uvs_len = len; }
  }
  a->cache = (uint32_t *) malloc (256 * sizeof (uint32_t));
  if (!a->cache) { a->subtable = NULL; return; }
  memset (a->cache, 0xFF, 256 * sizeof (uint32_t));
  if (a->subtable)
  {
    a->format = rd16 (a->subtable);
    if (a->format == 4)
    {
      a->seg_count = rd16 (a->subtable + 6) / 2;
      unsigned values_offset = 16 + 8 * a->seg_count;
      a->glyph_id_array_len = a->subtable_len > values_offset ? (a->subtable_len - values_offset) / 2 : 0;
    }
  }
}

static int long_group_cmp (const void *key, const uint8_t *item, const void *ud)
{
  uint32_t c = *(const uint32_t *) key;
  if (c < rd32 (item)) return -1;
  if (c > rd32 (item + 4)) return +1;
  return 0;
}

typedef struct { const uint8_t *end; unsigned dist; } fmt4_ud_t;
static int fmt4_cmp (const void *key, const uint8_t *item, const void *ud)
{
  uint32_t k = *(const uint32_t *) key;
  unsigned dist = *(const unsigned *) ud;
  if (k > rd16 (item)) return +1;
  if (k < rd16 (item + dist * 2)) return -1;
  return 0;
}

static int cmap_subtable_get (cmap_accel_t *a, uint32_t cp, uint32_t *glyph)
{
  const uint8_t *st = a->subtable;
  uint32_t gid;
  switch (a->format)
  {
  case 0:
    gid = cp < 256 ? st[6 + cp] : 0;
    break;
  case 4:
  {
    unsigned seg = a->seg_count;
    const uint8_t *endc = st + 14, *startc = endc + (seg + 1) * 2, *delta = startc + seg * 2, *rangeo = delta + seg * 2, *gia = rangeo + seg * 2;
    unsigned dist = seg + 1;
    int i = hb_bsearch (&cp, endc, seg, 2, fmt4_cmp, &dist);
    if (i < 0) return 0;
    unsigned ro = rd16 (rangeo + i * 2);
    if (ro == 0)
      gid = cp + rd16 (delta + i * 2);
    else
    {
      unsigned index = ro / 2 + (cp - rd16 (startc + i * 2)) + i - seg;
      if (index >= a->glyph_id_array_len) return 0;
      gid = rd16 (gia + index * 2);
      if (!gid) return 0;
      gid += rd16 (delta + i * 2);
    }
    gid &= 0xFFFFu;
    break;
  }
  case 6: case 10:
  {
    int l = a->format == 10;
    uint32_t start = l ? rd32 (st + 12) : rd16 (st + 6);
    uint32_t count = l ? rd32 (st + 16) : rd16 (st + 8);
    uint32_t idx = cp - start;
    gid = idx < count ? rd16 (st + (l ? 20 : 10) + idx * 2) : 0;
    break;
  }
  case 12: case 13:
  {
    int i = hb_bsearch (&cp, st + 16, rd32 (st + 12), 12, long_group_cmp, NULL);
    if (i < 0) return 0;
    const uint8_t *g = st + 16 + i * 12;
    uint32_t s = rd32 (g), e = rd32 (g + 4);
    if (a->format == 12) gid = s <= e ? rd32 (g + 8) + (cp - s) : 0;
    else gid = rd32 (g + 8);
    break;
  }
  default:
    return 0;
  }
  if (!gid) return 0;
  *glyph = gid;
  return 1;
}

static int cmap_get_nominal (hb_face_t *face, uint32_t cp, uint32_t *glyph)
{
  cmap_accel_t *a = &face->cmap_accel;
  if (!a->valid) cmap_accel_init (face);
  if (!a->cache) return 0;
  if (!a->subtable) return 0;
  uint32_t v = a->cache[cp & 255];
  if (v != 0xFFFFFFFFu && (v >> 19) == (cp >> 8)) { *glyph = v & ((1u << 19) - 1); return 1; }
  if (!cmap_subtable_get (a, cp, glyph)) return 0;
  if (!(cp >> 21) && !(*glyph >> 19))
    a->cache[cp & 255] = ((cp >> 8) << 19) | *glyph;
  return 1;
}

static int uvr_cmp (const void *key, const uint8_t *item, const void *ud)
{
  uint32_t c = *(const uint32_t *) key, s = rd24 (item);
  if (c < s) return -1;
  if (c > s + item[3]) return +1;
  return 0;
}
static int u24_cmp (const void *key, const uint8_t *item, const void *ud)
{
  uint32_t c = *(const uint32_t *) key, s = rd24 (item);
  return c < s ? -1 : c > s ? 1 : 0;
}

static int cmap_get_variation (hb_face_t *face, uint32_t cp, uint32_t vs, uint32_t *glyph)
{
  cmap_accel_t *a = &face->cmap_accel;
  if (!a->valid) cmap_accel_init (face);
  if (a->uvs)
  {
    const uint8_t *t = a->uvs;
    unsigned len = a->uvs_len;
    int i = hb_bsearch (&vs, t + 10, rd32 (t + 6), 11, u24_cmp, NULL);
    if (i >= 0)
    {
      const uint8_t *rec = t + 10 + i * 11;
      uint32_t doff = rd32 (rec + 3), ndoff = rd32 (rec + 7);
      if (doff && doff < len && len - doff >= 4 && (len - doff - 4) / 4 >= rd32 (t + doff) &&
	  hb_bsearch (&cp, t + doff + 4, rd32 (t + doff), 4, uvr_cmp, NULL) >= 0)
	return cmap_get_nominal (face, cp, glyph);
      if (ndoff && ndoff < len && len - ndoff >= 4 && (len - ndoff - 4) / 5 >= rd32 (t + ndoff))
      {
	int j = hb_bsearch (&cp, t + ndoff + 4, rd32 (t + ndoff), 5, u24_cmp, NULL);
	if (j >= 0 && rd16 (t + ndoff + 4 + j * 5 + 3))
	{
	  *glyph = rd16 (t + ndoff + 4 + j * 5 + 3);
	  return 1;
	}
      }
      return 0;
    }
    return 0;
  }
  return 0;
}

hb_bool_t
hb_font_get_nominal_glyph (hb_font_t *font, hb_codepoint_t unicode, hb_codepoint_t *glyph)
{
  *glyph = 0;
  return cmap_get_nominal (font->face, unicode, glyph);
}

static void hmtx_accel_init (hb_face_t *face)
{
  hmtx_accel_t *h = &face->hmtx_accel;
  h->init = 1;
  h->default_advance = face->upem / 2;
  unsigned len = face->hmtx.len;
  if (len & 1) len--;
  h->num_long_metrics = face->hhea.d ? rd16 (face->hhea.d + 34) : 0;
  if (h->num_long_metrics * 4 > len) h->num_long_metrics = len / 4;
  len -= h->num_long_metrics * 4;
  h->num_bearings = face->num_glyphs;
  if (h->num_bearings < h->num_long_metrics) h->num_bearings = h->num_long_metrics;
  if ((h->num_bearings - h->num_long_metrics) * 2 > len) h->num_bearings = h->num_long_metrics + len / 2;
  len -= (h->num_bearings - h->num_long_metrics) * 2;
  if (!h->num_long_metrics) h->num_bearings = h->num_long_metrics = 0;
  h->num_advances = h->num_bearings + len / 2;
  h->num_glyphs = face->num_glyphs;
  if (h->num_glyphs < h->num_advances) h->num_glyphs = h->num_advances;

  tab_t v = face_table (face, HB_TAG('H','V','A','R'));
  if (tab_has (v, 0, 20) && rd16 (v.d) == 1)
  {
    h->hvar = v;
    h->ivs = tab_sub (v, rd32 (v.d + 4));
    if (!ivs_valid (h->ivs)) h->ivs.d = NULL, h->ivs.len = 0;
    h->adv_map = tab_sub (v, rd32 (v.d + 8));
  }
}

static hmtx_accel_t *face_hmtx (hb_face_t *face)
{
  if (!face->hmtx_accel.init) hmtx_accel_init (face);
  return &face->hmtx_accel;
}

static unsigned hmtx_advance_unscaled (hb_face_t *face, hb_codepoint_t glyph)
{
  hmtx_accel_t *h = face_hmtx (face);
  if (glyph < h->num_bearings)
    return rd16 (face->hmtx.d + HB_MIN (glyph, h->num_long_metrics - 1) * 4);
  if (!h->num_advances) return h->default_advance;
  return 0;
}

static int hmtx_lsb_unscaled (hb_face_t *face, hb_codepoint_t glyph)
{
  hmtx_accel_t *h = face_hmtx (face);
  if (glyph < h->num_long_metrics) return rds16 (face->hmtx.d + glyph * 4 + 2);
  if (glyph >= h->num_bearings) return 0;
  return rds16 (face->hmtx.d + h->num_long_metrics * 4 + (glyph - h->num_long_metrics) * 2);
}

static unsigned glyf_advance_with_var_unscaled (hb_font_t *font, hb_codepoint_t glyph, int vertical);
static int glyf_can_vary_advance (hb_face_t *face);

static hb_position_t font_glyph_h_advance_raw (hb_font_t *font, hb_codepoint_t glyph)
{
  hb_face_t *face = font->face;
  hmtx_accel_t *h = face_hmtx (face);
  if (!h->num_bearings)
    return em_scale_x (font, face->upem / 2);
  if (!font->has_nonzero_coords)
    return em_scale_x (font, hmtx_advance_unscaled (face, glyph));
  font_check_serial (font);
  if (!font->h_advance_cache)
  {
    font->h_advance_cache = (u32_cache_t *) malloc (sizeof (u32_cache_t));
    if (!font->h_advance_cache) return em_scale_x (font, hmtx_advance_unscaled (face, glyph));
    memset (font->h_advance_cache, 0xFF, sizeof (u32_cache_t));
  }
  u32_cache_t *c = font->h_advance_cache;
  uint32_t cv = c->values[glyph & 255];
  if (cv != 0xFFFFFFFFu && (cv >> 16) == (glyph >> 8))
    return em_scale_x (font, (int32_t) (cv & 0xFFFF));
  int32_t v;
  if (h->hvar.d)
  {
    if (!font->h_ivs_cache) font->h_ivs_cache = scalar_cache_create (ivs_region_count (h->ivs));
    unsigned adv = hmtx_advance_unscaled (face, glyph);
    uint32_t varidx = delta_set_index_map (h->adv_map, glyph);
    float d = ivs_get_delta (h->ivs, varidx, font->coords, font->num_coords, font->h_ivs_cache);
    v = (int32_t) HB_MAX (0.0f, adv + hbroundf (d));
  }
  else if (glyf_can_vary_advance (face))
    v = (int32_t) glyf_advance_with_var_unscaled (font, glyph, 0);
  else
    return em_scale_x (font, hmtx_advance_unscaled (face, glyph));
  if (!(glyph >> 24) && !((uint32_t) v >> 16))
    c->values[glyph & 255] = ((glyph >> 8) << 16) | (uint32_t) v;
  return em_scale_x (font, v);
}

hb_position_t
hb_font_get_glyph_h_advance (hb_font_t *font, hb_codepoint_t glyph)
{
  hb_position_t advance = font_glyph_h_advance_raw (font, glyph);
  if (font->x_strength && !font->embolden_in_place)
  {
    hb_position_t strength = font->x_scale >= 0 ? font->x_strength : -font->x_strength;
    advance += advance ? strength : 0;
  }
  return advance;
}

#define BUDGET_GLYPH ((int64_t) 1 << 24)

static inline int budget_spend (int64_t *budget, unsigned cost, unsigned mult)
{
  *budget -= (int64_t) cost * mult;
  return *budget >= 0;
}

typedef struct { int path_open; float sx, sy, cx, cy; } dstate_t;

typedef struct
{
  void (*move_to) (void *d, dstate_t *st, float x, float y);
  void (*line_to) (void *d, dstate_t *st, float x, float y);
  void (*quadratic_to) (void *d, dstate_t *st, float cx, float cy, float x, float y);
  void (*cubic_to) (void *d, dstate_t *st, float c1x, float c1y, float c2x, float c2y, float x, float y);
  void (*close_path) (void *d, dstate_t *st);
  int64_t *(*budget) (void *d);
} draw_funcs_t;

typedef struct
{
  const draw_funcs_t *f;
  void *d;
  dstate_t st;
  int64_t local_budget;
  int64_t *budget;
} dsess_t;

static void emit_cubic_to (dsess_t *s, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
  if (s->f->cubic_to) s->f->cubic_to (s->d, &s->st, c1x, c1y, c2x, c2y, x, y);
}

static void emit_quadratic_to (dsess_t *s, float cx, float cy, float x, float y)
{
  if (s->f->quadratic_to) { s->f->quadratic_to (s->d, &s->st, cx, cy, x, y); return; }
  double ccx = s->st.cx, ccy = s->st.cy, qx = cx, qy = cy, tx = x, ty = y;
  const double t = 0.66666666666666666666666667;
  emit_cubic_to (s, (float) (ccx + (qx - ccx) * t), (float) (ccy + (qy - ccy) * t),
		 (float) (tx + (qx - tx) * t), (float) (ty + (qy - ty) * t), x, y);
}

static void emit_line_to (dsess_t *s, float x, float y)
{
  if (s->f->line_to) s->f->line_to (s->d, &s->st, x, y);
}

static void dsess_init (dsess_t *s, const draw_funcs_t *f, void *d)
{
  memset (s, 0, sizeof (*s));
  s->f = f;
  s->d = d;
  s->local_budget = BUDGET_GLYPH;
  s->budget = f->budget ? f->budget (d) : NULL;
  if (!s->budget) s->budget = &s->local_budget;
}

static void ds_start_path (dsess_t *s)
{
  if (s->f->move_to) s->f->move_to (s->d, &s->st, s->st.cx, s->st.cy);
  s->st.path_open = 1;
  s->st.sx = s->st.cx;
  s->st.sy = s->st.cy;
}

static void ds_close_path (dsess_t *s)
{
  if (s->st.path_open)
  {
    if (s->st.sx != s->st.cx || s->st.sy != s->st.cy)
      emit_line_to (s, s->st.sx, s->st.sy);
    if (s->f->close_path) s->f->close_path (s->d, &s->st);
  }
  s->st.path_open = 0;
  s->st.sx = s->st.cx = s->st.sy = s->st.cy = 0;
}

static void ds_move_to (dsess_t *s, float x, float y)
{
  if (s->st.path_open) ds_close_path (s);
  s->st.cx = x;
  s->st.cy = y;
}

static void ds_line_to (dsess_t *s, float x, float y)
{
  if (!s->st.path_open) ds_start_path (s);
  emit_line_to (s, x, y);
  s->st.cx = x;
  s->st.cy = y;
}

static void ds_quadratic_to (dsess_t *s, float cx, float cy, float x, float y)
{
  if (!s->st.path_open) ds_start_path (s);
  emit_quadratic_to (s, cx, cy, x, y);
  s->st.cx = x;
  s->st.cy = y;
}

static void ds_cubic_to (dsess_t *s, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
  if (!s->st.path_open) ds_start_path (s);
  emit_cubic_to (s, c1x, c1y, c2x, c2y, x, y);
  s->st.cx = x;
  s->st.cy = y;
}

typedef struct { float xmin, ymin, xmax, ymax; } extents_t;

static inline void extents_init (extents_t *e) { e->xmin = e->ymin = 0; e->xmax = e->ymax = -1; }
static inline int extents_empty (const extents_t *e) { return e->xmin >= e->xmax || e->ymin >= e->ymax; }

static inline void extents_add_point (extents_t *e, float x, float y)
{
  if (e->xmin > e->xmax)
  {
    e->xmin = e->xmax = x;
    e->ymin = e->ymax = y;
    return;
  }
  e->xmin = HB_MIN (e->xmin, x);
  e->ymin = HB_MIN (e->ymin, y);
  e->xmax = HB_MAX (e->xmax, x);
  e->ymax = HB_MAX (e->ymax, y);
}

static void extents_union (extents_t *e, const extents_t *o)
{
  if (extents_empty (o)) return;
  if (extents_empty (e)) { *e = *o; return; }
  e->xmin = HB_MIN (e->xmin, o->xmin);
  e->ymin = HB_MIN (e->ymin, o->ymin);
  e->xmax = HB_MAX (e->xmax, o->xmax);
  e->ymax = HB_MAX (e->ymax, o->ymax);
}

static void extents_intersect (extents_t *e, const extents_t *o)
{
  if (extents_empty (o) || extents_empty (e)) { extents_init (e); return; }
  e->xmin = HB_MAX (e->xmin, o->xmin);
  e->ymin = HB_MAX (e->ymin, o->ymin);
  e->xmax = HB_MIN (e->xmax, o->xmax);
  e->ymax = HB_MIN (e->ymax, o->ymax);
}

static void extents_from_glyph (extents_t *e, const hb_glyph_extents_t *g)
{
  double x0 = g->x_bearing, y0 = g->y_bearing, x1 = x0 + g->width, y1 = y0 + g->height;
  e->xmin = (float) HB_MIN (x0, x1);
  e->ymin = (float) HB_MIN (y0, y1);
  e->xmax = (float) HB_MAX (x0, x1);
  e->ymax = (float) HB_MAX (y0, y1);
}

static inline hb_position_t clamp_pos (double v) { return (hb_position_t) HB_CLAMP (v, (double) INT32_MIN, (double) INT32_MAX); }

static hb_glyph_extents_t extents_to_glyph (const extents_t *e, int xneg, int yneg)
{
  hb_glyph_extents_t r = {0, 0, 0, 0};
  double x0 = round ((double) e->xmin), y0 = round ((double) e->ymin);
  double x1 = round ((double) e->xmax), y1 = round ((double) e->ymax);
  if (!isfinite (x0) || !isfinite (y0) || !isfinite (x1) || !isfinite (y1)) return r;
  r.x_bearing = clamp_pos (xneg ? x1 : x0);
  r.y_bearing = clamp_pos (yneg ? y0 : y1);
  r.width = clamp_pos (xneg ? x0 - x1 : x1 - x0);
  r.height = clamp_pos (yneg ? y1 - y0 : y0 - y1);
  return r;
}

static void draw_extents_point (void *d, dstate_t *st, float x, float y) { extents_add_point ((extents_t *) d, x, y); }
static void draw_extents_quadratic_to (void *d, dstate_t *st, float cx, float cy, float x, float y)
{
  extents_add_point ((extents_t *) d, cx, cy);
  extents_add_point ((extents_t *) d, x, y);
}
static void draw_extents_cubic_to (void *d, dstate_t *st, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
  extents_add_point ((extents_t *) d, c1x, c1y);
  extents_add_point ((extents_t *) d, c2x, c2y);
  extents_add_point ((extents_t *) d, x, y);
}

static const draw_funcs_t draw_extents_funcs = {
  draw_extents_point, draw_extents_point, draw_extents_quadratic_to, draw_extents_cubic_to, NULL, NULL
};

#define MAX_NESTING_LEVEL 64
#define MAX_GRAPH_EDGE_COUNT 16384
#define GLYF_MAX_POINTS 200000

enum { PHANTOM_LEFT, PHANTOM_RIGHT, PHANTOM_TOP, PHANTOM_BOTTOM, PHANTOM_COUNT };
enum { FLAG_ON_CURVE = 0x01, FLAG_X_SHORT = 0x02, FLAG_Y_SHORT = 0x04, FLAG_REPEAT = 0x08, FLAG_X_SAME = 0x10, FLAG_Y_SAME = 0x20 };
enum {
  ARG_1_AND_2_ARE_WORDS = 0x0001,
  ARGS_ARE_XY_VALUES = 0x0002,
  WE_HAVE_A_SCALE = 0x0008,
  MORE_COMPONENTS = 0x0020,
  WE_HAVE_AN_X_AND_Y_SCALE = 0x0040,
  WE_HAVE_A_TWO_BY_TWO = 0x0080,
  USE_MY_METRICS = 0x0200,
  SCALED_COMPONENT_OFFSET = 0x0800,
  UNSCALED_COMPONENT_OFFSET = 0x1000
};

typedef struct { float x, y; uint8_t flag; uint8_t is_end_point; } cpoint_t;

DEFINE_VEC (cpvec_t, cpoint_t)
DEFINE_VEC (ivec_t, int)
DEFINE_VEC (uvec_t, unsigned)

typedef struct dnode_t dnode_t;
typedef struct { int awake; dnode_t *tortoise, *hare; } decycler_t;
struct dnode_t { union { decycler_t *decycler; dnode_t *next; } u; dnode_t *prev; uintptr_t value; };

static void dnode_init (dnode_t *n, decycler_t *d)
{
  memset (n, 0, sizeof (*n));
  n->u.decycler = d;
  d->awake = !d->awake;
  if (!d->tortoise) { d->tortoise = d->hare = n; return; }
  if (d->awake) d->tortoise = d->tortoise->u.next;
  n->prev = d->hare;
  d->hare->u.next = n;
  d->hare = n;
}

static void dnode_fini (dnode_t *n)
{
  decycler_t *d = n->u.decycler;
  d->hare = n->prev;
  if (n->prev) n->prev->u.decycler = d;
  if (d->awake) d->tortoise = d->tortoise->prev;
  d->awake = !d->awake;
}

static int dnode_visit (dnode_t *n, uintptr_t value)
{
  n->value = value;
  decycler_t *d = n->u.decycler;
  if (d->tortoise == n) return 1;
  return d->tortoise->value != value;
}

typedef struct glyf_scratch_t
{
  cpvec_t all_points, comp_points, orig_points, deltas;
  decycler_t decycler;
  ivec_t x_deltas, y_deltas;
  uvec_t shared_indices, private_indices;
} glyf_scratch_t;

static void glyf_scratch_free (glyf_scratch_t *s)
{
  if (!s) return;
  free (s->all_points.a); free (s->comp_points.a); free (s->orig_points.a); free (s->deltas.a);
  free (s->x_deltas.a); free (s->y_deltas.a); free (s->shared_indices.a); free (s->private_indices.a);
  free (s);
}

static int scache_get (const scalar_cache_t *c, unsigned i, float *v)
{
  if (!c || i >= c->length) { *v = 0.f; return 1; }
  int x = c->values[i];
  if (!x) { *v = 0.f; return 1; }
  if (x == SCALAR_INVALID) return 0;
  *v = x * (1.f / SCALAR_MULT);
  return 1;
}

static void scache_set (scalar_cache_t *c, unsigned i, float v)
{
  if (!c || i >= c->length) return;
  c->values[i] = (int) hbroundf (v * SCALAR_MULT);
}

static void gvar_init (hb_face_t *face)
{
  face->gvar_init = 1;
  tab_t g = face->gvar;
  int ok = tab_has (g, 0, 20) && rd16 (g.d) == 1;
  if (ok)
  {
    uint32_t st = rd32 (g.d + 8);
    unsigned n = rd16 (g.d + 4) * rd16 (g.d + 6);
    ok = tab_has (g, st, n * 2) && tab_has (g, 20, (face->num_glyphs + 1) * ((rd16 (g.d + 14) & 1) ? 4 : 2));
  }
  if (!ok) { face->gvar.d = NULL; face->gvar.len = 0; }
  face->gvar_glyph_count = face->gvar.d && rd32 (face->gvar.d) ? face->num_glyphs : 0;
}

static int gvar_has_data (hb_face_t *face)
{
  if (!face->gvar_init) gvar_init (face);
  return face->gvar.d && rd32 (face->gvar.d);
}

static scalar_cache_t *gvar_create_cache (hb_face_t *face)
{
  if (!gvar_has_data (face)) return NULL;
  return scalar_cache_create (HB_MIN ((unsigned) rd16 (face->gvar.d + 6), 0x1000u));
}

static unsigned gvar_offset (const uint8_t *g, unsigned glyph_count, unsigned i)
{
  if (i > glyph_count) return 0;
  return (rd16 (g + 14) & 1) ? rd32 (g + 20 + i * 4) : rd16 (g + 20 + i * 2) * 2u;
}

static double tuple_scalar (const uint8_t *hdr, const int *coords, unsigned ncoords, unsigned axis_count,
			    const uint8_t *shared, unsigned shared_len, scalar_cache_t *cache)
{
  unsigned ti = rd16 (hdr + 2), index = ti & 0x0FFF;
  unsigned clen = cache ? cache->length : 0;
  int plain = (ti & ~0x2000u) < clen;
  float s;
  if (plain && scache_get (cache, index, &s)) return (double) s;
  const uint8_t *peak;
  int interm = (ti & 0x4000) != 0;
  if (ti & 0x8000)
  {
    peak = hdr + 4;
    cache = NULL;
  }
  else
  {
    if (!plain && clen && scache_get (cache, index, &s))
    {
      if (interm && (s != 0 && s != 1.f)) cache = NULL;
      else return (double) s;
    }
    if ((index + 1) * axis_count > shared_len) return 0.0;
    peak = shared + axis_count * index * 2;
  }
  const uint8_t *start = NULL, *end = NULL;
  if (interm)
  {
    start = hdr + 4 + ((ti & 0x8000) ? axis_count * 2 : 0);
    end = start + axis_count * 2;
  }
  double scalar = 1.0;
  for (unsigned i = 0; i < axis_count; i++)
  {
    int pk = rds16 (peak + i * 2);
    if (!pk) continue;
    int v = i < ncoords ? coords[i] : 0;
    if (!v) { scalar = 0.0; break; }
    if (v == pk) continue;
    if (interm)
    {
      cache = NULL;
      int st = rds16 (start + i * 2), en = rds16 (end + i * 2);
      if (st > pk || pk > en || (st < 0 && en > 0 && pk)) continue;
      if (v < st || v > en) { scalar = 0.0; break; }
      if (v < pk) { if (pk != st) scalar *= (double) (v - st) / (pk - st); }
      else { if (pk != en) scalar *= (double) (en - v) / (en - pk); }
    }
    else if (v < HB_MIN (0, pk) || v > HB_MAX (0, pk)) { scalar = 0.0; break; }
    else
      scalar *= (double) v / pk;
  }
  if (cache) scache_set (cache, index, (float) scalar);
  return scalar;
}

static int decompile_points (const uint8_t **pp, uvec_t *points, const uint8_t *end)
{
  const uint8_t *p = *pp;
  if (p + 1 > end) return 0;
  unsigned count = *p++;
  if (count & 0x80)
  {
    if (p + 1 > end) return 0;
    count = ((count & 0x7F) << 8) | *p++;
  }
  if (!uvec_t_resize (points, count, 0)) return 0;
  unsigned n = 0, i = 0;
  while (i < count)
  {
    if (p + 1 > end) return 0;
    unsigned control = *p++;
    unsigned run = (control & 0x7F) + 1, stop = i + run;
    if (stop > count) return 0;
    if (control & 0x80)
    {
      if (p + run * 2 > end) return 0;
      for (; i < stop; i++, p += 2) { n += rd16 (p); points->a[i] = n; }
    }
    else
    {
      if (p + run > end) return 0;
      for (; i < stop; i++) { n += *p++; points->a[i] = n; }
    }
  }
  *pp = p;
  return 1;
}

static int decompile_deltas (const uint8_t **pp, int *values, unsigned count, const uint8_t *end, unsigned start)
{
  const uint8_t *p = *pp;
  unsigned i = 0;
  while (i < count)
  {
    if (p + 1 > end) { *pp = p; return 0; }
    unsigned control = *p++;
    unsigned run = (control & 0x3F) + 1, stop = i + run;
    if (stop > count) { *pp = p; return 0; }
    unsigned skip = i < start ? HB_MIN (start - i, run) : 0;
    i += skip;
    switch (control & 0xC0)
    {
    case 0x80:
      memset (values + i, 0, (stop - i) * sizeof (int));
      i = stop;
      break;
    case 0x40:
      if (p + run * 2 > end) { *pp = p; return 0; }
      p += skip * 2;
      for (; i < stop; i++, p += 2) values[i] = rds16 (p);
      break;
    case 0xC0:
      if (p + run * 4 > end) { *pp = p; return 0; }
      p += skip * 4;
      for (; i < stop; i++, p += 4) values[i] = rds32 (p);
      break;
    default:
      if (p + run > end) { *pp = p; return 0; }
      p += skip;
      for (; i < stop; i++) values[i] = (int8_t) *p++;
      break;
    }
  }
  *pp = p;
  return 1;
}

static int decompile_deltas_add (const uint8_t **pp, cpoint_t *points, unsigned count, float scalar, const uint8_t *end, unsigned start, int is_x)
{
  const uint8_t *p = *pp;
  unsigned i = 0;
  int ok = 1;
  while (i < count)
  {
    if (p + 1 > end) { ok = 0; break; }
    unsigned control = *p++;
    unsigned run = (control & 0x3F) + 1, stop = i + run;
    if (stop > count) { ok = 0; break; }
    unsigned skip = i < start ? HB_MIN (start - i, run) : 0;
    i += skip;
    switch (control & 0xC0)
    {
    case 0x80:
      i = stop;
      break;
    case 0x40:
      if (p + run * 2 > end) { ok = 0; goto done; }
      p += skip * 2;
      for (; i < stop; i++, p += 2) { float v = rds16 (p) * scalar; if (is_x) points[i].x += v; else points[i].y += v; }
      break;
    case 0xC0:
      if (p + run * 4 > end) { ok = 0; goto done; }
      p += skip * 4;
      for (; i < stop; i++, p += 4) { float v = rds32 (p) * scalar; if (is_x) points[i].x += v; else points[i].y += v; }
      break;
    default:
      if (p + run > end) { ok = 0; goto done; }
      p += skip;
      for (; i < stop; i++) { float v = (int8_t) *p++ * scalar; if (is_x) points[i].x += v; else points[i].y += v; }
      break;
    }
  }
done:
  *pp = p;
  return ok;
}

static float infer_delta (const cpoint_t *points, const cpoint_t *deltas, unsigned target, unsigned prev, unsigned next, int is_x)
{
#define M(p) (is_x ? (p).x : (p).y)
  float target_val = M (points[target]), prev_val = M (points[prev]), next_val = M (points[next]);
  float prev_delta = M (deltas[prev]), next_delta = M (deltas[next]);
#undef M
  if (prev_val == next_val) return prev_delta == next_delta ? prev_delta : 0.f;
  else if (target_val <= HB_MIN (prev_val, next_val)) return prev_val < next_val ? prev_delta : next_delta;
  else if (target_val >= HB_MAX (prev_val, next_val)) return prev_val > next_val ? prev_delta : next_delta;
  float r = (target_val - prev_val) / (next_val - prev_val);
  return prev_delta + r * (next_delta - prev_delta);
}

static inline unsigned next_index (unsigned i, unsigned start, unsigned end) { return i >= end ? start : i + 1; }

typedef struct
{
  const uint8_t *vd;
  unsigned vd_len;
  int tuples_left;
  unsigned axis_count, data_offset, cur_size;
  const uint8_t *data_base;
  const uint8_t *cur;
} tuple_iter_t;

static unsigned tuple_header_size (const uint8_t *h, unsigned axis_count)
{
  unsigned ti = rd16 (h + 2);
  if (ti & 0xC000)
    return (((ti & 0x8000) != 0) + ((ti & 0x4000) != 0) * 2) * axis_count * 2 + 4;
  return 4;
}

static int tuple_iter_valid (tuple_iter_t *it)
{
  if (it->tuples_left <= 0) return 0;
  it->cur_size = 4;
  if ((uintptr_t) (it->cur + 4 - it->vd) > it->vd_len) return 0;
  it->cur_size = tuple_header_size (it->cur, it->axis_count);
  if ((uintptr_t) (it->cur + it->cur_size - it->vd) > it->vd_len) return 0;
  return 1;
}

static int tuple_iter_next (tuple_iter_t *it)
{
  it->data_offset += rd16 (it->cur);
  it->cur += it->cur_size;
  it->tuples_left--;
  return tuple_iter_valid (it);
}

static int gvar_apply_deltas (hb_face_t *face, hb_codepoint_t glyph, const int *coords, unsigned ncoords,
			      cpoint_t *points, unsigned count, glyf_scratch_t *scratch,
			      scalar_cache_t *gvar_cache, int phantom_only, int64_t *budget)
{
  if (!face->gvar_init) gvar_init (face);
  unsigned glyph_count = face->gvar_glyph_count;
  if (glyph >= glyph_count) return 1;
  const uint8_t *g = face->gvar.d;
  unsigned so = gvar_offset (g, glyph_count, glyph), eo = gvar_offset (g, glyph_count, glyph + 1);
  if (eo < so) return 1;
  unsigned start = rd32 (g + 16) + so, length = eo - so;
  if (start > face->gvar.len) length = 0;
  else length = HB_MIN (length, face->gvar.len - start);
  if (length < 4) return 1;
  const uint8_t *vd = g + start;
  if (!rd16 (vd)) return 1;

  unsigned axis_count = rd16 (g + 4);
  uvec_t *shared_indices = &scratch->shared_indices;
  shared_indices->len = 0;

  tuple_iter_t it;
  it.vd = vd;
  it.vd_len = length;
  it.tuples_left = rd16 (vd) & 0x0FFF;
  it.axis_count = axis_count;
  it.cur = vd + 4;
  it.data_offset = 0;
  it.data_base = rd16 (vd + 2) ? vd + rd16 (vd + 2) : NULL;
  if (rd16 (vd) & 0x8000)
  {
    if (it.data_base)
    {
      const uint8_t *p = it.data_base;
      if (!decompile_points (&p, shared_indices, vd + length)) return 1;
      it.data_offset = p - it.data_base;
    }
    else
      it.data_offset = 1;
  }
  if (!tuple_iter_valid (&it)) return 1;

  int any_private_points = 0, private_points_checked = 0;
  cpvec_t *orig_vec = &scratch->orig_points, *deltas_vec = &scratch->deltas;
  orig_vec->len = 0;
  deltas_vec->len = 0;
  cpoint_t *orig_points = NULL, *deltas = NULL;
  unsigned shared_len = rd16 (g + 6) * axis_count;
  const uint8_t *shared = g + rd32 (g + 8);
  uvec_t *private_indices = &scratch->private_indices;
  ivec_t *x_deltas = &scratch->x_deltas, *y_deltas = &scratch->y_deltas;
  int flush = 0;
  unsigned pz = phantom_only ? count - 4 : 0, pn = phantom_only ? 4 : count;

  do
  {
    float scalar = (float) tuple_scalar (it.cur, coords, ncoords, axis_count, shared, shared_len, gvar_cache);
    if (scalar == 0.f) continue;
    if (budget && !budget_spend (budget, 1, phantom_only && count >= 4 ? 4 : count)) return 0;
    if (!private_points_checked)
    {
      tuple_iter_t scan = it;
      do
      {
	if (rd16 (scan.cur + 2) & 0x2000) { any_private_points = 1; break; }
      } while (tuple_iter_next (&scan));
      private_points_checked = 1;
    }
    if (!it.data_base) return 0;
    const uint8_t *p = it.data_base + it.data_offset;
    unsigned dlen = rd16 (it.cur);
    if (!(vd <= p && p <= vd + length && (unsigned) (vd + length - p) >= dlen)) return 0;
    if (!deltas)
    {
      if (!cpvec_t_resize (deltas_vec, count, 0)) return 0;
      deltas = deltas_vec->a;
      memset (deltas + pz, 0, pn * sizeof (cpoint_t));
    }
    const uint8_t *end = p + dlen;
    int has_private = (rd16 (it.cur + 2) & 0x2000) != 0;
    if (has_private && !decompile_points (&p, private_indices, end)) return 0;
    const unsigned *indices = has_private ? private_indices->a : shared_indices->a;
    unsigned nindices = has_private ? private_indices->len : shared_indices->len;
    int apply_to_all = nindices == 0;
    unsigned num_deltas = apply_to_all ? count : nindices;
    unsigned start_deltas = apply_to_all && phantom_only && num_deltas >= 4 ? num_deltas - 4 : 0;

    if (apply_to_all && !any_private_points)
    {
      if (!decompile_deltas_add (&p, points, count, scalar, end, start_deltas, 1)) return 0;
      if (!decompile_deltas_add (&p, points, count, scalar, end, start_deltas, 0)) return 0;
      continue;
    }

    if (!ivec_t_resize (x_deltas, num_deltas, 0)) return 0;
    if (!decompile_deltas (&p, x_deltas->a, num_deltas, end, start_deltas)) return 0;
    if (!ivec_t_resize (y_deltas, num_deltas, 0)) return 0;
    if (!decompile_deltas (&p, y_deltas->a, num_deltas, end, start_deltas)) return 0;

    if (!apply_to_all)
    {
      if (!orig_points && !phantom_only)
      {
	if (!cpvec_t_resize (orig_vec, count, 0)) return 0;
	memcpy (orig_vec->a, points, count * sizeof (cpoint_t));
	orig_points = orig_vec->a;
      }
      if (flush)
	for (unsigned i = pz; i < count; i++) { points[i].x += deltas[i].x; points[i].y += deltas[i].y; }
      memset (deltas + pz, 0, pn * sizeof (cpoint_t));
    }

    if (scalar != 1.0f)
    {
      if (apply_to_all)
	for (unsigned i = pz; i < count; i++)
	{
	  deltas[i].x += x_deltas->a[i] * scalar;
	  deltas[i].y += y_deltas->a[i] * scalar;
	}
      else
	for (unsigned i = 0; i < num_deltas; i++)
	{
	  unsigned pt = indices[i];
	  if (pt >= count) continue;
	  if (phantom_only && pt < count - 4) continue;
	  deltas[pt].flag = 1;
	  deltas[pt].x += x_deltas->a[i] * scalar;
	  deltas[pt].y += y_deltas->a[i] * scalar;
	}
    }
    else
    {
      if (apply_to_all)
	for (unsigned i = pz; i < count; i++)
	{
	  deltas[i].x += x_deltas->a[i];
	  deltas[i].y += y_deltas->a[i];
	}
      else
	for (unsigned i = 0; i < num_deltas; i++)
	{
	  unsigned pt = indices[i];
	  if (pt >= count) continue;
	  if (phantom_only && pt < count - 4) continue;
	  deltas[pt].flag = 1;
	  deltas[pt].x += x_deltas->a[i];
	  deltas[pt].y += y_deltas->a[i];
	}
    }

    if (!apply_to_all && !phantom_only)
    {
      unsigned start_point = 0, end_point = 0;
      for (;;)
      {
	while (end_point < count && !points[end_point].is_end_point) end_point++;
	if (end_point == count) break;
	unsigned unref = 0;
	for (unsigned i = start_point; i < end_point + 1; i++) unref += deltas[i].flag;
	unref = (end_point - start_point + 1) - unref;
	unsigned j = start_point;
	if (unref == 0 || unref > end_point - start_point) goto no_more_gaps;
	for (;;)
	{
	  unsigned prev, next, i;
	  for (;;)
	  {
	    i = j;
	    j = next_index (i, start_point, end_point);
	    if (deltas[i].flag && !deltas[j].flag) break;
	  }
	  prev = j = i;
	  for (;;)
	  {
	    i = j;
	    j = next_index (i, start_point, end_point);
	    if (!deltas[i].flag && deltas[j].flag) break;
	  }
	  next = j;
	  i = prev;
	  for (;;)
	  {
	    i = next_index (i, start_point, end_point);
	    if (i == next) break;
	    deltas[i].x = infer_delta (orig_points, deltas, i, prev, next, 1);
	    deltas[i].y = infer_delta (orig_points, deltas, i, prev, next, 0);
	    if (--unref == 0) goto no_more_gaps;
	  }
	}
      no_more_gaps:
	start_point = end_point = end_point + 1;
      }
    }
    flush = 1;
  } while (tuple_iter_next (&it));

  if (flush)
    for (unsigned i = pz; i < count; i++) { points[i].x += deltas[i].x; points[i].y += deltas[i].y; }
  return 1;
}

typedef struct { const uint8_t *d; unsigned len; hb_codepoint_t gid; int type; } glyph_t;
enum { GLYPH_EMPTY, GLYPH_SIMPLE, GLYPH_COMPOSITE };

static void glyf_init (hb_face_t *face)
{
  face->glyf_init = 1;
  const uint8_t *h = face->head.d;
  int loc_format = h ? rds16 (h + 50) : 0, data_format = h ? rds16 (h + 52) : 0;
  if (!(loc_format <= 1 && data_format <= 1)) return;
  face->glyf_short = loc_format == 0;
  unsigned n = face->loca.len / (face->glyf_short ? 2 : 4);
  n = HB_MAX (1u, n) - 1;
  face->glyf_num_glyphs = HB_MIN (n, face->num_glyphs);
}

static unsigned glyf_num_glyphs (hb_face_t *face)
{
  if (!face->glyf_init) glyf_init (face);
  return face->glyf_num_glyphs;
}

static glyph_t glyph_make (const uint8_t *d, unsigned len, hb_codepoint_t gid)
{
  glyph_t g = {d, len, gid, GLYPH_EMPTY};
  int nc = len >= 10 ? rds16 (d) : 0;
  if (nc > 0) g.type = GLYPH_SIMPLE;
  else if (nc < 0) g.type = GLYPH_COMPOSITE;
  return g;
}

static glyph_t glyf_glyph (hb_face_t *face, hb_codepoint_t gid)
{
  glyph_t empty = {NULL, 0, (hb_codepoint_t) -1, GLYPH_EMPTY};
  if (gid >= glyf_num_glyphs (face)) return empty;
  unsigned so, eo;
  const uint8_t *l = face->loca.d;
  if (face->glyf_short) { so = 2 * rd16 (l + gid * 2); eo = 2 * rd16 (l + gid * 2 + 2); }
  else { so = rd32 (l + gid * 4); eo = rd32 (l + gid * 4 + 4); }
  if (so > eo || eo > face->glyf.len) return empty;
  return glyph_make (face->glyf.d + so, eo - so, gid);
}

static inline int gh_xmin (glyph_t *g) { return g->len >= 10 ? rds16 (g->d + 2) : 0; }
static inline int gh_ymin (glyph_t *g) { return g->len >= 10 ? rds16 (g->d + 4) : 0; }
static inline int gh_xmax (glyph_t *g) { return g->len >= 10 ? rds16 (g->d + 6) : 0; }
static inline int gh_ymax (glyph_t *g) { return g->len >= 10 ? rds16 (g->d + 8) : 0; }

static int simple_glyph_points (glyph_t *g, cpvec_t *points, int phantom_only, int64_t *budget)
{
  int nc = rds16 (g->d);
  const uint8_t *endpts = g->d + 10;
  if (10 + nc * 2 + 2 > g->len) return 0;
  unsigned num_points = rd16 (endpts + (nc - 1) * 2) + 1;
  if (num_points < (unsigned) nc) return 0;
  if (budget && !budget_spend (budget, 1, num_points)) return 0;
  unsigned old = points->len;
  if (!cpvec_t_reserve (points, old + num_points + 4)) return 0;
  if (!cpvec_t_resize (points, old + num_points, 0)) return 0;
  cpoint_t *pts = points->a + old;
  if (phantom_only) return 1;
  memset (pts, 0, num_points * sizeof (cpoint_t));
  for (int i = 0; i < nc; i++) { unsigned e = rd16 (endpts + i * 2); if (e < num_points) pts[e].is_end_point = 1; }
  const uint8_t *p = endpts + (nc + 1) * 2 + rd16 (endpts + nc * 2);
  const uint8_t *end = g->d + g->len;
  if (p >= end) return 0;
  for (unsigned i = 0; i < num_points;)
  {
    if (p + 1 > end) return 0;
    uint8_t flag = *p++;
    pts[i++].flag = flag;
    if (flag & FLAG_REPEAT)
    {
      if (p + 1 > end) return 0;
      unsigned repeat = *p++, stop = HB_MIN (i + repeat, num_points);
      for (; i < stop; i++) pts[i].flag = flag;
    }
  }
  for (int axis = 0; axis < 2; axis++)
  {
    unsigned short_flag = axis ? FLAG_Y_SHORT : FLAG_X_SHORT, same_flag = axis ? FLAG_Y_SAME : FLAG_X_SAME;
    int v = 0;
    for (unsigned i = 0; i < num_points; i++)
    {
      unsigned flag = pts[i].flag;
      if (flag & short_flag)
      {
	if (p + 1 > end) return 0;
	v += ((flag & same_flag) ? 1 : -1) * *p++;
      }
      else if (!(flag & same_flag))
      {
	if (p + 2 > end) return 0;
	v += rds16 (p);
	p += 2;
      }
      if (axis) pts[i].y = v; else pts[i].x = v;
    }
  }
  return 1;
}

static unsigned comp_size (unsigned flags)
{
  unsigned size = 4 + ((flags & ARG_1_AND_2_ARE_WORDS) ? 4 : 2);
  if (flags & WE_HAVE_A_SCALE) size += 2;
  else if (flags & WE_HAVE_AN_X_AND_Y_SCALE) size += 4;
  else if (flags & WE_HAVE_A_TWO_BY_TWO) size += 8;
  return size;
}

static const uint8_t *comp_check (glyph_t *g, const uint8_t *c)
{
  if (!(g->d <= c && c <= g->d + g->len && (unsigned) (g->d + g->len - c) >= 4)) return NULL;
  if ((unsigned) (g->d + g->len - c) < comp_size (rd16 (c))) return NULL;
  return c;
}

static const uint8_t *comp_first (glyph_t *g) { return g->type == GLYPH_COMPOSITE ? comp_check (g, g->d + 10) : NULL; }
static const uint8_t *comp_next (glyph_t *g, const uint8_t *c)
{
  if (!(rd16 (c) & MORE_COMPONENTS)) return NULL;
  return comp_check (g, c + comp_size (rd16 (c)));
}

static int comp_transformation (const uint8_t *c, float matrix[4], cpoint_t *trans)
{
  unsigned flags = rd16 (c);
  matrix[0] = matrix[3] = 1.f;
  matrix[1] = matrix[2] = 0.f;
  const uint8_t *p = c + 4;
  int tx, ty;
  if (flags & ARG_1_AND_2_ARE_WORDS) { tx = rds16 (p); ty = rds16 (p + 2); p += 4; }
  else { tx = (int8_t) p[0]; ty = (int8_t) p[1]; p += 2; }
  if (!(flags & ARGS_ARE_XY_VALUES)) tx = ty = 0;
  trans->x = (float) tx; trans->y = (float) ty; trans->flag = 0; trans->is_end_point = 1;
  if (flags & WE_HAVE_A_SCALE) { matrix[0] = matrix[3] = rdf2dot14 (p); return 1; }
  else if (flags & WE_HAVE_AN_X_AND_Y_SCALE) { matrix[0] = rdf2dot14 (p); matrix[3] = rdf2dot14 (p + 2); return 1; }
  else if (flags & WE_HAVE_A_TWO_BY_TWO)
  {
    matrix[0] = rdf2dot14 (p); matrix[1] = rdf2dot14 (p + 2);
    matrix[2] = rdf2dot14 (p + 4); matrix[3] = rdf2dot14 (p + 6);
    return 1;
  }
  return tx || ty;
}

static void points_translate (cpoint_t *pts, unsigned n, float tx, float ty)
{
  if (tx != 0.f && ty != 0.f)
    for (unsigned i = 0; i < n; i++) { pts[i].x += tx; pts[i].y += ty; }
  else if (tx != 0.f)
    for (unsigned i = 0; i < n; i++) pts[i].x += tx;
  else if (ty != 0.f)
    for (unsigned i = 0; i < n; i++) pts[i].y += ty;
}

static void points_transform (cpoint_t *pts, unsigned n, const float m[4])
{
  if (m[0] != 1.f || m[1] != 0.f || m[2] != 0.f || m[3] != 1.f)
    for (unsigned i = 0; i < n; i++)
    {
      float x = pts[i].x * m[0] + pts[i].y * m[2];
      pts[i].y = pts[i].x * m[1] + pts[i].y * m[3];
      pts[i].x = x;
    }
}

static int glyph_get_points (hb_font_t *font, glyph_t *g, cpvec_t *all, glyf_scratch_t *scratch,
			     int phantom_only, const int *coords, unsigned ncoords,
			     scalar_cache_t *gvar_cache, unsigned depth, unsigned *edge_count, int64_t *budget)
{
  hb_face_t *face = font->face;
  if (depth > MAX_NESTING_LEVEL) return 0;
  if (*edge_count > MAX_GRAPH_EDGE_COUNT) return 0;
  (*edge_count)++;
  if (!budget_spend (budget, 16, 1)) return 0;

  cpvec_t *points = g->type == GLYPH_SIMPLE ? all : &scratch->comp_points;
  unsigned old_length = points->len;

  if (g->type == GLYPH_SIMPLE)
  {
    if (!simple_glyph_points (g, all, phantom_only, budget)) return 0;
  }
  else if (g->type == GLYPH_COMPOSITE)
  {
    for (const uint8_t *c = comp_first (g); c; c = comp_next (g, c))
    {
      float m[4];
      cpoint_t trans;
      comp_transformation (c, m, &trans);
      if (!cpvec_t_reserve (points, points->len + 1 + 4)) return 0;
      cpvec_t_push (points, trans);
    }
  }

  if (!cpvec_t_resize (points, points->len + PHANTOM_COUNT, 1)) return 0;
  {
    cpoint_t *ph = points->a + points->len - PHANTOM_COUNT;
    int lsb = hmtx_lsb_unscaled (face, g->gid);
    int h_delta = gh_xmin (g) - lsb;
    int v_orig = gh_ymax (g);
    unsigned h_adv = hmtx_advance_unscaled (face, g->gid);
    unsigned v_adv = -face->upem;
    ph[PHANTOM_LEFT].x = h_delta;
    ph[PHANTOM_RIGHT].x = (int) h_adv + h_delta;
    ph[PHANTOM_TOP].y = v_orig;
    ph[PHANTOM_BOTTOM].y = v_orig - (int) v_adv;
  }

  int any = 0;
  for (unsigned i = 0; i < ncoords; i++) if (coords[i]) { any = 1; break; }
  if (any)
    if (!gvar_apply_deltas (face, g->gid, coords, ncoords, points->a + old_length, points->len - old_length,
			    scratch, gvar_cache, phantom_only && g->type == GLYPH_SIMPLE, budget))
      return 0;

  float shift = 0;
  if (g->type == GLYPH_SIMPLE)
    shift = points->a[points->len - PHANTOM_COUNT + PHANTOM_LEFT].x;
  else if (g->type == GLYPH_COMPOSITE)
  {
    dnode_t node;
    dnode_init (&node, &scratch->decycler);
    unsigned comp_index = 0;
    for (const uint8_t *c = comp_first (g); c; c = comp_next (g, c))
    {
      unsigned flags = rd16 (c);
      hb_codepoint_t item_gid = rd16 (c + 2);
      if (!dnode_visit (&node, item_gid)) { comp_index++; continue; }
      unsigned old_count = all->len;
      int my_metrics = (flags & USE_MY_METRICS) != 0;
      if (!phantom_only || my_metrics)
      {
	glyph_t child = glyf_glyph (face, item_gid);
	if (!glyph_get_points (font, &child, all, scratch, phantom_only, coords, ncoords, gvar_cache, depth + 1, edge_count, budget))
	{
	  points->len = HB_MIN (points->len, old_length);
	  dnode_fini (&node);
	  return 0;
	}
      }
      cpoint_t *ph = points->a + points->len - PHANTOM_COUNT;
      cpoint_t *comp = all->a + old_count;
      unsigned comp_len = all->len - old_count;
      if (my_metrics)
	for (unsigned i = 0; i < PHANTOM_COUNT; i++) ph[i] = comp[comp_len - PHANTOM_COUNT + i];
      if (!budget_spend (budget, 1, comp_len))
      {
	points->len = HB_MIN (points->len, old_length);
	dnode_fini (&node);
	return 0;
      }
      if (comp_len)
      {
	float m[4];
	cpoint_t dt;
	comp_transformation (c, m, &dt);
	cpoint_t t = points->a[old_length + comp_index];
	if ((flags & (SCALED_COMPONENT_OFFSET | UNSCALED_COMPONENT_OFFSET)) == SCALED_COMPONENT_OFFSET)
	{
	  points_translate (comp, comp_len, t.x, t.y);
	  points_transform (comp, comp_len, m);
	}
	else
	{
	  points_transform (comp, comp_len, m);
	  points_translate (comp, comp_len, t.x, t.y);
	}
      }
      if (!(flags & ARGS_ARE_XY_VALUES) && !phantom_only)
      {
	const uint8_t *p = c + 4;
	unsigned p1, p2;
	if (flags & ARG_1_AND_2_ARE_WORDS) { p1 = rd16 (p); p2 = rd16 (p + 2); }
	else { p1 = p[0]; p2 = p[1]; }
	if (p1 < all->len && p2 < comp_len)
	  points_translate (comp, comp_len, all->a[p1].x - comp[p2].x, all->a[p1].y - comp[p2].y);
      }
      if (all->len >= PHANTOM_COUNT) all->len -= PHANTOM_COUNT;
      if (all->len > GLYF_MAX_POINTS)
      {
	points->len = HB_MIN (points->len, old_length);
	dnode_fini (&node);
	return 0;
      }
      comp_index++;
    }
    dnode_fini (&node);
    if (!cpvec_t_reserve (all, all->len + PHANTOM_COUNT)) return 0;
    memcpy (all->a + all->len, points->a + points->len - PHANTOM_COUNT, PHANTOM_COUNT * sizeof (cpoint_t));
    all->len += PHANTOM_COUNT;
    shift = all->a[all->len - PHANTOM_COUNT + PHANTOM_LEFT].x;
    points->len = old_length;
  }
  else
  {
    if (!cpvec_t_reserve (all, all->len + PHANTOM_COUNT)) return 0;
    memcpy (all->a + all->len, points->a + points->len - PHANTOM_COUNT, PHANTOM_COUNT * sizeof (cpoint_t));
    all->len += PHANTOM_COUNT;
    shift = all->a[all->len - PHANTOM_COUNT + PHANTOM_LEFT].x;
    points->len = old_length;
  }

  if (depth == 0 && shift)
    for (unsigned i = 0; i < all->len; i++) all->a[i].x -= shift;
  return 1;
}

static glyf_scratch_t *glyf_scratch (hb_face_t *face)
{
  if (!face->glyf_scratch) face->glyf_scratch = (glyf_scratch_t *) calloc (1, sizeof (glyf_scratch_t));
  return face->glyf_scratch;
}

typedef struct
{
  int consume;
  void (*point) (void *d, const cpoint_t *p);
  void (*contour_end) (void *d);
  void *d;
  cpoint_t *phantoms;
} points_sink_t;

static int glyf_points (hb_font_t *font, hb_codepoint_t gid, points_sink_t *sink, const int *coords, unsigned ncoords,
			glyf_scratch_t *scratch, scalar_cache_t *gvar_cache, int64_t *budget)
{
  hb_face_t *face = font->face;
  if (gid >= glyf_num_glyphs (face)) return 0;
  int64_t stack_budget = BUDGET_GLYPH;
  if (!budget) budget = &stack_budget;
  cpvec_t *all = &scratch->all_points;
  all->len = 0;
  glyph_t g = glyf_glyph (face, gid);
  unsigned edge_count = 0;
  if (!glyph_get_points (font, &g, all, scratch, !sink->consume, coords, ncoords, gvar_cache, 0, &edge_count, budget))
    return 0;
  if (!budget_spend (budget, 1, all->len)) return 0;
  unsigned count = all->len - PHANTOM_COUNT;
  cpoint_t *points = all->a;
  if (sink->consume)
  {
    for (unsigned i = 0; i < count; i++)
    {
      if (points[i].flag & FLAG_ON_CURVE)
      {
	for (; i < count; i++)
	{
	  sink->point (sink->d, &points[i]);
	  if (points[i].is_end_point) { sink->contour_end (sink->d); break; }
	}
      }
      else
      {
	unsigned start = i;
	for (; i < count; i++) if (points[i].is_end_point) break;
	unsigned end = i;
	if (end < count) sink->point (sink->d, &points[end]);
	for (i = start; i < end; i++) sink->point (sink->d, &points[i]);
	sink->contour_end (sink->d);
      }
    }
  }
  if (sink->phantoms)
    memcpy (sink->phantoms, points + count, PHANTOM_COUNT * sizeof (cpoint_t));
  return 1;
}

static const int *font_var_coords (hb_font_t *font, unsigned *n)
{
  *n = font->has_nonzero_coords ? font->num_coords : 0;
  return font->coords;
}

static scalar_cache_t *font_gvar_cache (hb_font_t *font)
{
  font_check_serial (font);
  if (!font->gvar_cache_valid)
  {
    font->gvar_cache = gvar_create_cache (font->face);
    font->gvar_cache_valid = 1;
  }
  return font->gvar_cache;
}

static void sink_nop_contour_end (void *d) {}

static unsigned glyf_advance_with_var_unscaled (hb_font_t *font, hb_codepoint_t glyph, int vertical)
{
  hb_face_t *face = font->face;
  if (glyph >= glyf_num_glyphs (face)) return 0;
  glyf_scratch_t *scratch = glyf_scratch (face);
  cpoint_t phantoms[PHANTOM_COUNT];
  points_sink_t sink = {0, NULL, sink_nop_contour_end, NULL, phantoms};
  unsigned n;
  const int *coords = font_var_coords (font, &n);
  if (!glyf_points (font, glyph, &sink, coords, n, scratch, font_gvar_cache (font), NULL))
    return vertical ? face->upem : face->upem / 2;
  float r = vertical ? phantoms[PHANTOM_TOP].y - phantoms[PHANTOM_BOTTOM].y : phantoms[PHANTOM_RIGHT].x - phantoms[PHANTOM_LEFT].x;
  return (unsigned) HB_CLAMP (hbroundf (r), 0.f, (float) UINT_MAX / 2);
}

static int glyf_can_vary_advance (hb_face_t *face)
{
  return gvar_has_data (face) && glyf_num_glyphs (face) && glyf_scratch (face);
}

static void scale_glyph_extents (hb_font_t *font, hb_glyph_extents_t *e)
{
  float x1 = em_fscale_x (font, e->x_bearing);
  float y1 = em_fscale_y (font, e->y_bearing);
  float x2 = em_fscale_x (font, e->x_bearing + e->width);
  float y2 = em_fscale_y (font, e->y_bearing + e->height);
  e->x_bearing = (hb_position_t) floorf (x1);
  e->y_bearing = (hb_position_t) floorf (y1);
  e->width = (hb_position_t) ceilf (x2) - e->x_bearing;
  e->height = (hb_position_t) ceilf (y2) - e->y_bearing;
}

typedef struct { float min_x, min_y, max_x, max_y; } bounds_t;

static void bounds_point (void *d, const cpoint_t *p)
{
  bounds_t *b = (bounds_t *) d;
  b->min_x = HB_MIN (b->min_x, p->x);
  b->min_y = HB_MIN (b->min_y, p->y);
  b->max_x = HB_MAX (b->max_x, p->x);
  b->max_y = HB_MAX (b->max_y, p->y);
}

static int glyf_get_extents (hb_font_t *font, hb_codepoint_t gid, hb_glyph_extents_t *extents)
{
  hb_face_t *face = font->face;
  if (gid >= glyf_num_glyphs (face)) return 0;
  unsigned n;
  const int *coords = font_var_coords (font, &n);
  if (n)
  {
    glyf_scratch_t *scratch = glyf_scratch (face);
    if (!scratch) return 0;
    bounds_t b = {FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
    points_sink_t sink = {1, bounds_point, sink_nop_contour_end, &b, NULL};
    if (!glyf_points (font, gid, &sink, coords, n, scratch, NULL, NULL)) return 0;
    if (b.min_x >= b.max_x || b.min_y >= b.max_y)
    {
      extents->width = extents->x_bearing = extents->height = extents->y_bearing = 0;
      return 1;
    }
    double xb = (double) hbroundf (b.min_x), yb = (double) hbroundf (b.max_y);
    extents->x_bearing = clamp_pos (xb);
    extents->width = clamp_pos ((double) hbroundf (b.max_x) - xb);
    extents->y_bearing = clamp_pos (yb);
    extents->height = clamp_pos ((double) hbroundf (b.min_y) - yb);
    scale_glyph_extents (font, extents);
    return 1;
  }
  glyph_t g = glyf_glyph (face, gid);
  if (g.type == GLYPH_EMPTY)
  {
    extents->x_bearing = extents->y_bearing = extents->width = extents->height = 0;
    return 1;
  }
  int xmin = gh_xmin (&g), xmax = gh_xmax (&g), ymin = gh_ymin (&g), ymax = gh_ymax (&g);
  extents->x_bearing = hmtx_lsb_unscaled (face, gid);
  extents->y_bearing = HB_MAX (ymin, ymax);
  extents->width = HB_MAX (xmin, xmax) - HB_MIN (xmin, xmax);
  extents->height = HB_MIN (ymin, ymax) - HB_MAX (ymin, ymax);
  scale_glyph_extents (font, extents);
  return 1;
}

typedef struct { int has; float x, y; } opt_point_t;

typedef struct
{
  hb_font_t *font;
  dsess_t *ds;
  opt_point_t first_oncurve, first_offcurve, last_offcurve;
} path_builder_t;

static inline opt_point_t opt_mid (opt_point_t a, opt_point_t b)
{
  opt_point_t r = {1, (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
  return r;
}

static void path_point (void *d, const cpoint_t *point)
{
  path_builder_t *pb = (path_builder_t *) d;
  int on = point->flag & FLAG_ON_CURVE;
  opt_point_t p = {1, em_fscalef_x (pb->font, point->x), em_fscalef_y (pb->font, point->y)};
  if (!pb->first_oncurve.has)
  {
    if (on)
    {
      pb->first_oncurve = p;
      ds_move_to (pb->ds, p.x, p.y);
    }
    else if (pb->first_offcurve.has)
    {
      opt_point_t mid = opt_mid (pb->first_offcurve, p);
      pb->first_oncurve = mid;
      pb->last_offcurve = p;
      ds_move_to (pb->ds, mid.x, mid.y);
    }
    else
      pb->first_offcurve = p;
  }
  else if (pb->last_offcurve.has)
  {
    if (on)
    {
      ds_quadratic_to (pb->ds, pb->last_offcurve.x, pb->last_offcurve.y, p.x, p.y);
      pb->last_offcurve.has = 0;
    }
    else
    {
      opt_point_t mid = opt_mid (pb->last_offcurve, p);
      ds_quadratic_to (pb->ds, pb->last_offcurve.x, pb->last_offcurve.y, mid.x, mid.y);
      pb->last_offcurve = p;
    }
  }
  else if (on)
    ds_line_to (pb->ds, p.x, p.y);
  else
    pb->last_offcurve = p;
}

static void path_contour_end (void *d)
{
  path_builder_t *pb = (path_builder_t *) d;
  if (pb->first_offcurve.has && pb->last_offcurve.has)
  {
    opt_point_t mid = opt_mid (pb->last_offcurve, pb->first_offcurve);
    ds_quadratic_to (pb->ds, pb->last_offcurve.x, pb->last_offcurve.y, mid.x, mid.y);
    pb->last_offcurve.has = 0;
  }
  if (pb->first_offcurve.has && pb->first_oncurve.has)
    ds_quadratic_to (pb->ds, pb->first_offcurve.x, pb->first_offcurve.y, pb->first_oncurve.x, pb->first_oncurve.y);
  else if (pb->last_offcurve.has && pb->first_oncurve.has)
    ds_quadratic_to (pb->ds, pb->last_offcurve.x, pb->last_offcurve.y, pb->first_oncurve.x, pb->first_oncurve.y);
  else if (pb->first_oncurve.has)
    ds_line_to (pb->ds, pb->first_oncurve.x, pb->first_oncurve.y);
  else if (pb->first_offcurve.has)
  {
    float x = pb->first_offcurve.x, y = pb->first_offcurve.y;
    ds_move_to (pb->ds, x, y);
    ds_quadratic_to (pb->ds, x, y, x, y);
  }
  pb->first_oncurve.has = pb->first_offcurve.has = pb->last_offcurve.has = 0;
  ds_close_path (pb->ds);
}

static int glyf_get_path (hb_font_t *font, hb_codepoint_t gid, dsess_t *ds, scalar_cache_t *gvar_cache, int64_t *budget)
{
  hb_face_t *face = font->face;
  if (!glyf_num_glyphs (face)) return 0;
  glyf_scratch_t *scratch = glyf_scratch (face);
  if (!scratch) return 1;
  path_builder_t pb;
  memset (&pb, 0, sizeof (pb));
  pb.font = font;
  pb.ds = ds;
  points_sink_t sink = {1, path_point, path_contour_end, &pb, NULL};
  unsigned n;
  const int *coords = font_var_coords (font, &n);
  return glyf_points (font, gid, &sink, coords, n, scratch, gvar_cache, budget);
}

#define CFF_MAX_OPS 200000
#define CFF_UNDEF_SID 0xFFFFFFFFu
#define CFF_ARG_LIMIT 513
#define CFF_CALL_LIMIT 10

typedef struct { const uint8_t *d; unsigned len, off; } strref_t;

static inline int sr_avail (const strref_t *s, unsigned n) { return s->off + n <= s->len; }
static inline int sr_error (const strref_t *s) { return s->off > s->len; }
static inline void sr_set_error (strref_t *s) { s->off = s->len + 1; }
static inline unsigned sr_at (strref_t *s, unsigned i)
{
  if (s->off + i >= s->len) { sr_set_error (s); return 0; }
  return s->d[s->off + i];
}

typedef struct
{
  strref_t str;
  double args[CFF_ARG_LIMIT];
  unsigned nargs;
  int arg_error;
} cff_env_t;

static inline int env_error (const cff_env_t *e) { return sr_error (&e->str) || e->arg_error; }
static inline void push_num (cff_env_t *e, double v)
{
  if (e->nargs < CFF_ARG_LIMIT) e->args[e->nargs++] = v;
  else e->arg_error = 1;
}
static inline double pop_num (cff_env_t *e)
{
  if (e->nargs) return e->args[--e->nargs];
  e->arg_error = 1;
  return 0.;
}
static inline double eval_arg (cff_env_t *e, unsigned i)
{
  if (i < e->nargs) return e->args[i];
  e->arg_error = 1;
  return 0.;
}
static inline int num_to_int (double v, int *ok)
{
  if (!(v >= INT_MIN && v <= INT_MAX)) { *ok = 0; return 0; }
  *ok = 1;
  return (int) v;
}
static inline int num_to_int_sat (double v)
{
  int ok, r = num_to_int (v, &ok);
  if (ok) return r;
  return v < 0 ? INT_MIN : v > 0 ? INT_MAX : 0;
}
static int pop_int (cff_env_t *e)
{
  int ok, v = num_to_int (pop_num (e), &ok);
  if (!ok) e->arg_error = 1;
  return v;
}
static unsigned pop_uint (cff_env_t *e)
{
  int i = pop_int (e);
  if (i < 0) { i = 0; e->arg_error = 1; }
  return (unsigned) i;
}

static unsigned fetch_op (strref_t *s)
{
  if (!sr_avail (s, 1)) return 0xFFFFu;
  unsigned op = s->d[s->off++];
  if (op == 12)
  {
    if (!sr_avail (s, 1)) return 0xFFFFu;
    op = 256 + s->d[s->off++];
  }
  return op;
}

static void process_number_op (unsigned op, cff_env_t *e)
{
  if (op == 28)
  {
    push_num (e, (int16_t) ((sr_at (&e->str, 0) << 8) | sr_at (&e->str, 1)));
    e->str.off += 2;
  }
  else if (op >= 247 && op <= 250)
  {
    push_num (e, (int16_t) ((op - 247) * 256 + sr_at (&e->str, 0) + 108));
    e->str.off++;
  }
  else if (op >= 251 && op <= 254)
  {
    push_num (e, -(int16_t) (op - 251) * 256 - (int) sr_at (&e->str, 0) - 108);
    e->str.off++;
  }
  else if (op >= 32 && op <= 246)
    push_num (e, (int) op - 139);
  else
    e->nargs = 0;
}

static double pow10_hb (unsigned exponent)
{
  static const double powers[] = {1.0e+256, 1.0e+128, 1.0e+64, 1.0e+32, 1.0e+16, 1.0e+8, 10000., 100., 10.};
  unsigned mask = 1 << 8;
  double result = 1;
  for (const double *p = powers; mask; ++p, mask >>= 1)
    if (exponent & mask) result *= *p;
  return result;
}

static int parse_double (const char *p, const char *pe, double *pv)
{
  const char *start = p;
  double value = 0, frac = 0, frac_count = 0;
  unsigned exp = 0;
  int neg = 0, exp_neg = 0, exp_overflow = 0, st = 0;
  for (; p < pe; p++)
  {
    char c = *p;
    int digit = c >= '0' && c <= '9';
    switch (st)
    {
    case 0:
    case 1:
      if (st == 0 && (c == '+' || c == '-')) { neg = c == '-'; st = 1; continue; }
      if (digit) { value = value * 10. + (c - '0'); st = 2; continue; }
      if (c == '.') { st = 3; continue; }
      goto out;
    case 2:
      if (digit) { value = value * 10. + (c - '0'); continue; }
      if (c == '.') { st = 3; continue; }
      if (c == 'e' || c == 'E') { st = 5; continue; }
      goto out;
    case 3:
    case 4:
      if (digit)
      {
	if (frac <= 0xFFFFFFFFFFFFFull / 10) { frac = frac * 10. + (c - '0'); ++frac_count; }
	st = 4;
	continue;
      }
      if (st == 4 && (c == 'e' || c == 'E')) { st = 5; continue; }
      goto out;
    case 5:
    case 6:
      if (st == 5 && (c == '+' || c == '-')) { exp_neg = c == '-'; st = 6; continue; }
      if (digit) { st = 7; goto exp_digit; }
      goto out;
    case 7:
      if (!digit) goto out;
    exp_digit:
      if (exp * 10 + (c - '0') <= 0x7FFu) exp = exp * 10 + (c - '0');
      else exp_overflow = 1;
      continue;
    }
  }
out:
  if (p == start) return 0;
  if (frac_count) value += frac / pow10_hb ((unsigned) frac_count);
  if (neg) value *= -1.;
  if (exp_overflow)
  {
    if (value != 0)
    {
      if (exp_neg) value = neg ? -DBL_MIN : DBL_MIN;
      else value = neg ? -DBL_MAX : DBL_MAX;
    }
  }
  else if (exp)
  {
    if (exp_neg) value /= pow10_hb (exp);
    else value *= pow10_hb (exp);
  }
  *pv = value;
  return p == pe;
}

static double parse_bcd (strref_t *s)
{
  if (sr_error (s)) return .0;
  char buf[32] = {0};
  unsigned char byte = 0;
  for (unsigned i = 0, count = 0; count < sizeof (buf); ++i, ++count)
  {
    unsigned nibble;
    if (!(i & 1))
    {
      if (!sr_avail (s, 1)) break;
      byte = sr_at (s, 0);
      s->off++;
      nibble = byte >> 4;
    }
    else
      nibble = byte & 0x0F;
    if (nibble == 13) break;
    else if (nibble == 15)
    {
      double pv;
      if (!parse_double (buf, buf + count, &pv)) break;
      return pv;
    }
    else
    {
      buf[count] = "0123456789.EE?-?"[nibble];
      if (nibble == 12)
      {
	++count;
	if (count == sizeof (buf)) break;
	buf[count] = '-';
      }
    }
  }
  sr_set_error (s);
  return .0;
}

static void process_dict_number_op (unsigned op, cff_env_t *e)
{
  if (op == 29)
  {
    push_num (e, (int) ((sr_at (&e->str, 0) << 24) | (sr_at (&e->str, 1) << 16) | (sr_at (&e->str, 2) << 8) | sr_at (&e->str, 3)));
    e->str.off += 4;
  }
  else if (op == 30)
    push_num (e, parse_bcd (&e->str));
  else
    process_number_op (op, e);
}

typedef struct { const uint8_t *start, *end; unsigned num_glyphs; } cff_sc_t;

static inline int sc_check_range (const cff_sc_t *sc, const uint8_t *p, uint64_t len)
{
  return (uintptr_t) (p - sc->start) <= (uintptr_t) (sc->end - sc->start) && (uint64_t) (sc->end - p) >= len;
}

static inline unsigned cff_index_count (const uint8_t *p) { return p ? rd16 (p) : 0; }

static unsigned cff_index_offset_at (const uint8_t *p, unsigned i)
{
  unsigned size = p[2];
  const uint8_t *o = p + 3 + i * size;
  switch (size)
  {
  case 1: return o[0];
  case 2: return rd16 (o);
  case 3: return rd24 (o);
  case 4: return rd32 (o);
  default: return 0;
  }
}

static int cff_index_valid (const cff_sc_t *sc, const uint8_t *p)
{
  if (!sc_check_range (sc, p, 2)) return 0;
  unsigned count = rd16 (p);
  if (!count) return 1;
  if (!sc_check_range (sc, p + 2, 1)) return 0;
  unsigned off_size = p[2];
  if (off_size < 1 || off_size > 4) return 0;
  if (!sc_check_range (sc, p + 3, (uint64_t) off_size * (count + 1))) return 0;
  return sc_check_range (sc, p + 2 + off_size * (count + 1), cff_index_offset_at (p, count));
}

static uint64_t cff_index_size (const uint8_t *p)
{
  unsigned count = rd16 (p);
  if (!count) return 2;
  return 3 + (uint64_t) p[2] * (count + 1) + (uint32_t) (cff_index_offset_at (p, count) - 1);
}

static tab_t cff_index_get (const uint8_t *p, unsigned i)
{
  tab_t r = {NULL, 0};
  if (!p || i >= rd16 (p)) return r;
  unsigned o0 = cff_index_offset_at (p, i), o1 = cff_index_offset_at (p, i + 1);
  if (o1 < o0 || o1 > cff_index_offset_at (p, rd16 (p))) return r;
  r.d = p + 2 + p[2] * (rd16 (p) + 1) + o0;
  r.len = o1 - o0;
  return r;
}

static const uint8_t *sc_at (const cff_sc_t *sc, const uint8_t *base, int64_t offset)
{
  if (!offset) return NULL;
  const uint8_t *p = base + offset;
  if (!((uintptr_t) (p - sc->start) <= (uintptr_t) (sc->end - sc->start))) return NULL;
  return p;
}

static const uint8_t *cff_index_at (const cff_sc_t *sc, const uint8_t *base, int64_t offset)
{
  const uint8_t *p = sc_at (sc, base, offset);
  return p && cff_index_valid (sc, p) ? p : NULL;
}

static int charset_valid (const cff_sc_t *sc, const uint8_t *p)
{
  if (!sc_check_range (sc, p, 1)) return 0;
  unsigned num_glyphs = sc->num_glyphs;
  switch (p[0])
  {
  case 0: return sc_check_range (sc, p + 1, (uint64_t) (num_glyphs - 1) * 2);
  case 1:
  case 2:
  {
    unsigned rs = p[0] == 1 ? 3 : 4;
    if (!num_glyphs) return 0;
    num_glyphs--;
    for (unsigned i = 0; num_glyphs > 0; i++)
    {
      const uint8_t *r = p + 1 + i * rs;
      if (!sc_check_range (sc, r, rs)) return 0;
      unsigned n = (rs == 3 ? r[2] : rd16 (r + 2)) + 1;
      if (num_glyphs < n) return 0;
      num_glyphs -= n;
    }
    return 1;
  }
  default: return 0;
  }
}

static unsigned charset_get_glyph (const uint8_t *p, unsigned sid, unsigned num_glyphs)
{
  if (!sid) return 0;
  if (p[0] == 0)
  {
    for (unsigned g = 1; g < num_glyphs; g++)
      if (rd16 (p + 1 + (g - 1) * 2) == sid) return g;
    return 0;
  }
  unsigned rs = p[0] == 1 ? 3 : 4, glyph = 1;
  for (unsigned i = 0;; i++)
  {
    if (glyph >= num_glyphs) return 0;
    const uint8_t *r = p + 1 + i * rs;
    unsigned first = rd16 (r), left = rs == 3 ? r[2] : rd16 (r + 2);
    if (first <= sid && sid <= first + left) return glyph + (sid - first);
    glyph += left + 1;
  }
}

static int encoding_valid (const cff_sc_t *sc, const uint8_t *p)
{
  if (!sc_check_range (sc, p, 1)) return 0;
  unsigned fmt = p[0] & 0x7F;
  if (fmt > 1 || !sc_check_range (sc, p + 1, 1)) return 0;
  unsigned n = p[1], size = fmt ? 2 : 1;
  if (!sc_check_range (sc, p + 2, n * size)) return 0;
  if (!(p[0] & 0x80)) return 1;
  if (!n) return 0;
  const uint8_t *s = p + 2 + n * size;
  return sc_check_range (sc, s, 1) && sc_check_range (sc, s + 1, s[0] * 3);
}

static int fdselect_valid (const cff_sc_t *sc, const uint8_t *p, unsigned fdcount)
{
  if (!sc_check_range (sc, p, 1)) return 0;
  if (p[0] == 0) return sc_check_range (sc, p + 1, sc->num_glyphs);
  if (p[0] != 3) return 0;
  if (!sc_check_range (sc, p + 1, 2)) return 0;
  unsigned n = rd16 (p + 1);
  for (unsigned i = 0; i < n; i++)
  {
    const uint8_t *r = p + 3 + i * 3;
    if (!sc_check_range (sc, r, 3) || !(rd16 (r) < sc->num_glyphs && r[2] < fdcount)) return 0;
  }
  if (!n || rd16 (p + 3) != 0) return 0;
  for (unsigned i = 1; i < n; i++)
    if (rd16 (p + 3 + (i - 1) * 3) >= rd16 (p + 3 + i * 3)) return 0;
  const uint8_t *sentinel = p + 3 + n * 3;
  return sc_check_range (sc, sentinel, 2) && rd16 (sentinel) == sc->num_glyphs;
}

static unsigned fdselect_get_fd (const uint8_t *p, hb_codepoint_t glyph)
{
  if (!p) return 0;
  if (p[0] == 0) return p[1 + glyph];
  if (p[0] != 3) return 0;
  unsigned n = rd16 (p + 1);
  const uint8_t *ranges = p + 3;
  int min = 0, max = (int) n - 2;
  while (min <= max)
  {
    int mid = ((unsigned) min + (unsigned) max) / 2;
    const uint8_t *r = ranges + mid * 3;
    if (glyph < rd16 (r)) max = mid - 1;
    else if (glyph < rd16 (r + 3)) return r[2];
    else min = mid + 1;
  }
  return ranges[(n - 1) * 3 + 2];
}

typedef struct
{
  int valid;
  cff_sc_t sc;
  const uint8_t *cff;
  const uint8_t *charset;
  int charset_offset;
  const uint8_t *fdselect;
  const uint8_t *global_subrs;
  const uint8_t *charstrings;
  unsigned fd_count;
  const uint8_t **local_subrs;
  unsigned num_glyphs;
} cff_accel_t;

typedef struct
{
  int charstrings, fdarray, encoding, charset, fdselect, is_cid;
  int64_t private_offset;
  unsigned private_size;
} cff_dict_t;

enum { DICT_TOP, DICT_FONT, DICT_PRIVATE };

static int cff_parse_dict (tab_t str, int kind, cff_dict_t *dv)
{
  cff_env_t *e = (cff_env_t *) malloc (sizeof (cff_env_t));
  if (!e) return 0;
  e->str.d = str.d; e->str.len = str.len; e->str.off = 0;
  e->nargs = 0; e->arg_error = 0;
  int ok = 1;
  while (sr_avail (&e->str, 1))
  {
    unsigned op = fetch_op (&e->str);
    if (kind == DICT_TOP)
      switch (op)
      {
      case 0: case 1: case 256: case 2: case 256 + 38: case 3: case 4: case 256 + 21: case 256 + 22:
	pop_uint (e); e->nargs = 0; break;
      case 256 + 1: case 256 + 2: case 256 + 3: case 256 + 4: case 256 + 5: case 256 + 6: case 13:
      case 256 + 8: case 256 + 20: case 256 + 31: case 256 + 32: case 256 + 33: case 256 + 35: case 5:
      case 14: case 256 + 23: case 256 + 7:
	e->nargs = 0; break;
      case 256 + 34: pop_uint (e); e->nargs = 0; break;
      case 256 + 30: pop_uint (e); pop_uint (e); pop_uint (e); e->nargs = 0; dv->is_cid = 1; break;
      case 16: dv->encoding = pop_int (e); e->nargs = 0; break;
      case 15: dv->charset = pop_int (e); e->nargs = 0; break;
      case 256 + 37: dv->fdselect = pop_int (e); e->nargs = 0; break;
      case 18: dv->private_offset = pop_int (e); dv->private_size = pop_uint (e); e->nargs = 0; break;
      case 17: dv->charstrings = pop_int (e); e->nargs = 0; break;
      case 256 + 36: dv->fdarray = pop_int (e); e->nargs = 0; break;
      default: process_dict_number_op (op, e); break;
      }
    else if (kind == DICT_FONT)
      switch (op)
      {
      case 256 + 38: pop_uint (e); e->nargs = 0; break;
      case 256 + 7: case 256 + 5: e->nargs = 0; break;
      case 18: dv->private_offset = pop_uint (e); dv->private_size = pop_uint (e); e->nargs = 0; break;
      default: process_dict_number_op (op, e); break;
      }
    else
      switch (op)
      {
      case 6: case 7: case 8: case 9: case 256 + 12: case 256 + 13: case 10: case 11: case 256 + 9:
      case 256 + 10: case 256 + 11: case 256 + 14: case 256 + 17: case 256 + 18: case 256 + 19: case 20: case 21:
	e->nargs = 0; break;
      case 19: dv->private_offset = pop_int (e); e->nargs = 0; break;
      default: process_dict_number_op (op, e); break;
      }
    if (env_error (e)) { ok = 0; break; }
  }
  free (e);
  return ok;
}

static void cff_accel_init (hb_face_t *face, cff_accel_t *a)
{
  memset (a, 0, sizeof (*a));
  tab_t t = face->cff;
  if (!(tab_has (t, 0, 4) && t.d[0] == 1)) return;
  const uint8_t *cff = t.d;
  cff_sc_t *sc = &a->sc;
  sc->start = cff; sc->end = cff + t.len; sc->num_glyphs = face->num_glyphs;
  a->cff = cff;
  const uint8_t *name_index = cff + cff[2];
  if (!cff_index_valid (sc, name_index)) return;
  const uint8_t *top_index = cff_index_at (sc, name_index, (int) cff_index_size (name_index));
  if (!top_index || !rd16 (top_index)) return;
  tab_t top_str = cff_index_get (top_index, 0);
  if (!sc_check_range (sc, top_str.d, top_str.len)) return;
  cff_dict_t top;
  memset (&top, 0, sizeof (top));
  if (!cff_parse_dict (top_str, DICT_TOP, &top)) return;
  a->charset_offset = top.charset;
  if (top.charset > 2)
  {
    a->charset = sc_at (sc, cff, top.charset);
    if (!a->charset || !charset_valid (sc, a->charset)) return;
  }
  a->fd_count = 1;
  const uint8_t *fdarray = NULL;
  if (top.is_cid)
  {
    fdarray = cff_index_at (sc, cff, top.fdarray);
    a->fdselect = sc_at (sc, cff, top.fdselect);
    if (a->fdselect && !fdselect_valid (sc, a->fdselect, cff_index_count (fdarray))) a->fdselect = NULL;
    if (!fdarray || !a->fdselect) return;
    a->fd_count = rd16 (fdarray);
    if (!a->charset) return;
  }
  else if (top.encoding > 1)
  {
    const uint8_t *enc = sc_at (sc, cff, top.encoding);
    if (!enc || !encoding_valid (sc, enc)) return;
  }
  const uint8_t *string_index = cff_index_at (sc, top_index, (int) cff_index_size (top_index));
  if (!string_index) return;
  a->global_subrs = cff_index_at (sc, string_index, (int) cff_index_size (string_index));
  a->charstrings = cff_index_at (sc, cff, top.charstrings);
  if (!a->charstrings) return;
  a->num_glyphs = rd16 (a->charstrings);
  if (a->num_glyphs != sc->num_glyphs) return;
  a->local_subrs = (const uint8_t **) calloc (a->fd_count ? a->fd_count : 1, sizeof (const uint8_t *));
  if (!a->local_subrs) return;
  for (unsigned i = 0; i < (top.is_cid ? a->fd_count : 1); i++)
  {
    cff_dict_t font;
    memset (&font, 0, sizeof (font));
    if (top.is_cid)
    {
      tab_t font_str = cff_index_get (fdarray, i);
      if (!sc_check_range (sc, font_str.d, font_str.len)) goto fail;
      if (!cff_parse_dict (font_str, DICT_FONT, &font)) goto fail;
    }
    else
      font = top;
    tab_t priv_str = {NULL, font.private_size};
    const uint8_t *pp = sc_at (sc, cff, font.private_offset);
    if (pp && sc_check_range (sc, pp, font.private_size)) priv_str.d = pp;
    if (font.private_size && !priv_str.d) goto fail;
    cff_dict_t priv;
    memset (&priv, 0, sizeof (priv));
    if (priv_str.d && !cff_parse_dict (priv_str, DICT_PRIVATE, &priv)) goto fail;
    if (priv_str.d) a->local_subrs[i] = cff_index_at (sc, priv_str.d, priv.private_offset);
  }
  a->valid = 1;
  return;
fail:
  free (a->local_subrs);
  a->local_subrs = NULL;
}

static cff_accel_t *face_cff (hb_face_t *face)
{
  if (!face->cff_accel)
  {
    face->cff_accel = calloc (1, sizeof (cff_accel_t));
    if (!face->cff_accel) return NULL;
    cff_accel_init (face, (cff_accel_t *) face->cff_accel);
  }
  return (cff_accel_t *) face->cff_accel;
}

static void cff_accel_free (hb_face_t *face)
{
  cff_accel_t *a = (cff_accel_t *) face->cff_accel;
  if (!a) return;
  free (a->local_subrs);
  free (a);
}

static const uint8_t standard_encoding_to_sid[] =
{
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    1,    2,    3,    4,    5,    6,    7,    8,    9,   10,   11,   12,   13,   14,   15,   16,
   17,   18,   19,   20,   21,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,   32,
   33,   34,   35,   36,   37,   38,   39,   40,   41,   42,   43,   44,   45,   46,   47,   48,
   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,   59,   60,   61,   62,   63,   64,
   65,   66,   67,   68,   69,   70,   71,   72,   73,   74,   75,   76,   77,   78,   79,   80,
   81,   82,   83,   84,   85,   86,   87,   88,   89,   90,   91,   92,   93,   94,   95,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,   96,   97,   98,   99,  100,  101,  102,  103,  104,  105,  106,  107,  108,  109,  110,
    0,  111,  112,  113,  114,    0,  115,  116,  117,  118,  119,  120,  121,  122,    0,  123,
    0,  124,  125,  126,  127,  128,  129,  130,  131,    0,  132,  133,    0,  134,  135,  136,
  137,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,  138,    0,  139,    0,    0,    0,    0,  140,  141,  142,  143,    0,    0,    0,    0,
    0,  144,    0,    0,    0,  145,    0,    0,  146,  147,  148,  149,    0,    0,    0,    0
};

static unsigned cff_std_code_to_glyph (cff_accel_t *a, int code)
{
  if ((unsigned) code >= 256) return 0;
  unsigned sid = standard_encoding_to_sid[code];
  if (a->charset) return charset_get_glyph (a->charset, sid, a->num_glyphs);
  if (a->charset_offset == 0 && code <= 228) return sid;
  return 0;
}

typedef struct { double x, y; } cpt_t;
typedef struct { cpt_t min, max; } cbounds_t;

typedef struct
{
  int extents;
  int path_open;
  cbounds_t bounds;
  hb_font_t *font;
  dsess_t *ds;
  const cpt_t *delta;
  cff_accel_t *cff;
} cs_param_t;

typedef struct
{
  cff_env_t e;
  strref_t call_stack[CFF_CALL_LIMIT];
  unsigned ncalls;
  int call_error;
  int endchar, seen_hintmask, in_seac;
  unsigned hstem_count, vstem_count, hintmask_size;
  int64_t *budget;
  cpt_t pt;
  const uint8_t *gsubrs, *lsubrs;
} cs_env_t;

static void cbounds_init (cbounds_t *b) { b->min.x = b->min.y = INT_MAX; b->max.x = b->max.y = INT_MIN; }
static int cbounds_empty (const cbounds_t *b) { return b->min.x >= b->max.x || b->min.y >= b->max.y; }
static void cbounds_update (cbounds_t *b, cpt_t p)
{
  if (p.x < b->min.x) b->min.x = p.x;
  if (p.x > b->max.x) b->max.x = p.x;
  if (p.y < b->min.y) b->min.y = p.y;
  if (p.y > b->max.y) b->max.y = p.y;
}
static void cbounds_merge (cbounds_t *b, const cbounds_t *o)
{
  if (cbounds_empty (b)) *b = *o;
  else if (!cbounds_empty (o))
  {
    if (o->min.x < b->min.x) b->min.x = o->min.x;
    if (o->max.x > b->max.x) b->max.x = o->max.x;
    if (o->min.y < b->min.y) b->min.y = o->min.y;
    if (o->max.y > b->max.y) b->max.y = o->max.y;
  }
}

static inline cpt_t cpt_dx (cpt_t p, double dx) { p.x += dx; return p; }
static inline cpt_t cpt_dy (cpt_t p, double dy) { p.y += dy; return p; }
static inline cpt_t cpt_d (cpt_t p, double dx, double dy) { p.x += dx; p.y += dy; return p; }

static cpt_t cs_delta (const cs_param_t *p, cpt_t pt)
{
  if (p->delta) { pt.x += p->delta->x; pt.y += p->delta->y; }
  return pt;
}

static void cs_moveto (cs_env_t *env, cs_param_t *p, cpt_t pt)
{
  if (p->extents) p->path_open = 0;
  else
  {
    cpt_t q = cs_delta (p, pt);
    ds_move_to (p->ds, em_fscalef_x (p->font, (float) q.x), em_fscalef_y (p->font, (float) q.y));
  }
  env->pt = pt;
}

static void cs_line (cs_env_t *env, cs_param_t *p, cpt_t pt)
{
  if (p->extents)
  {
    if (!p->path_open) { p->path_open = 1; cbounds_update (&p->bounds, env->pt); }
    env->pt = pt;
    cbounds_update (&p->bounds, env->pt);
    return;
  }
  cpt_t q = cs_delta (p, pt);
  ds_line_to (p->ds, em_fscalef_x (p->font, (float) q.x), em_fscalef_y (p->font, (float) q.y));
  env->pt = pt;
}

static void cs_curve (cs_env_t *env, cs_param_t *p, cpt_t p1, cpt_t p2, cpt_t p3)
{
  if (p->extents)
  {
    if (!p->path_open) { p->path_open = 1; cbounds_update (&p->bounds, env->pt); }
    cbounds_update (&p->bounds, p1);
    cbounds_update (&p->bounds, p2);
    env->pt = p3;
    cbounds_update (&p->bounds, env->pt);
    return;
  }
  cpt_t a = cs_delta (p, p1), b = cs_delta (p, p2), c = cs_delta (p, p3);
  hb_font_t *f = p->font;
  ds_cubic_to (p->ds, em_fscalef_x (f, (float) a.x), em_fscalef_y (f, (float) a.y),
	       em_fscalef_x (f, (float) b.x), em_fscalef_y (f, (float) b.y),
	       em_fscalef_x (f, (float) c.x), em_fscalef_y (f, (float) c.y));
  env->pt = p3;
}

static unsigned subrs_bias (const uint8_t *subrs)
{
  unsigned n = cff_index_count (subrs);
  return n < 1240 ? 107 : n < 33900 ? 1131 : 32768;
}

static void cs_call_subr (cs_env_t *env, const uint8_t *subrs)
{
  int n = pop_int (&env->e) + (int) subrs_bias (subrs);
  if (n < 0 || (unsigned) n >= cff_index_count (subrs) || env->ncalls >= CFF_CALL_LIMIT)
  {
    sr_set_error (&env->e.str);
    return;
  }
  env->call_stack[env->ncalls++] = env->e.str;
  tab_t s = cff_index_get (subrs, n);
  env->e.str.d = s.d; env->e.str.len = s.len; env->e.str.off = 0;
}

static void cs_return (cs_env_t *env)
{
  if (sr_error (&env->e.str)) sr_set_error (&env->e.str);
  if (env->ncalls) env->e.str = env->call_stack[--env->ncalls];
  else
  {
    env->call_error = 1;
    memset (&env->e.str, 0, sizeof (env->e.str));
  }
}

static void cs_hintmask_size (cs_env_t *env)
{
  if (!env->seen_hintmask)
  {
    env->vstem_count += env->e.nargs / 2;
    env->hintmask_size = (env->hstem_count + env->vstem_count + 7) >> 3;
    env->seen_hintmask = 1;
  }
}

static int cff_get (cff_accel_t *cff, hb_font_t *font, hb_codepoint_t glyph, cs_param_t *param, int in_seac, int64_t *budget);

static void cs_seac (cs_env_t *env, cs_param_t *param)
{
  cff_env_t *e = &env->e;
  unsigned n = e->nargs;
  cpt_t delta = {eval_arg (e, n - 4), eval_arg (e, n - 3)};
  if (!param->extents) ds_close_path (param->ds);
  unsigned base = cff_std_code_to_glyph (param->cff, num_to_int_sat (eval_arg (e, n - 2)));
  unsigned accent = cff_std_code_to_glyph (param->cff, num_to_int_sat (eval_arg (e, n - 1)));
  if (param->extents)
  {
    cs_param_t pb = *param, pa = *param;
    if (!env->in_seac && base && accent &&
	cff_get (param->cff, param->font, base, &pb, 1, env->budget) &&
	cff_get (param->cff, param->font, accent, &pa, 1, env->budget))
    {
      cbounds_merge (&param->bounds, &pb.bounds);
      if (!cbounds_empty (&pa.bounds))
      {
	pa.bounds.min = cpt_d (pa.bounds.min, delta.x, delta.y);
	pa.bounds.max = cpt_d (pa.bounds.max, delta.x, delta.y);
      }
      cbounds_merge (&param->bounds, &pa.bounds);
    }
    else
      sr_set_error (&e->str);
  }
  else
  {
    cs_param_t pb = *param, pa = *param;
    pb.delta = NULL;
    pa.delta = &delta;
    if (!(!env->in_seac && base && accent &&
	  cff_get (param->cff, param->font, base, &pb, 1, env->budget) &&
	  cff_get (param->cff, param->font, accent, &pa, 1, env->budget)))
      sr_set_error (&e->str);
  }
}

static void cs_path_op (unsigned op, cs_env_t *env, cs_param_t *param)
{
  cff_env_t *e = &env->e;
  unsigned n = e->nargs, i = 0;
  cpt_t pt1, pt2, pt3;
#define A(k) eval_arg (e, (k))
  switch (op)
  {
  case 21: { cpt_t p = env->pt; double dy = pop_num (e), dx = pop_num (e); cs_moveto (env, param, cpt_d (p, dx, dy)); break; }
  case 22: cs_moveto (env, param, cpt_dx (env->pt, pop_num (e))); break;
  case 4: cs_moveto (env, param, cpt_dy (env->pt, pop_num (e))); break;
  case 5:
    for (; i + 2 <= e->nargs; i += 2) cs_line (env, param, cpt_d (env->pt, A (i), A (i + 1)));
    break;
  case 6:
  case 7:
  {
    int h = op == 6;
    for (; i + 2 <= e->nargs; i += 2)
    {
      pt1 = env->pt;
      pt1 = h ? cpt_dx (pt1, A (i)) : cpt_dy (pt1, A (i));
      cs_line (env, param, pt1);
      pt1 = h ? cpt_dy (pt1, A (i + 1)) : cpt_dx (pt1, A (i + 1));
      cs_line (env, param, pt1);
    }
    if (i < e->nargs)
    {
      pt1 = env->pt;
      pt1 = h ? cpt_dx (pt1, A (i)) : cpt_dy (pt1, A (i));
      cs_line (env, param, pt1);
    }
    break;
  }
  case 8:
    for (; i + 6 <= e->nargs; i += 6)
    {
      pt1 = cpt_d (env->pt, A (i), A (i + 1));
      pt2 = cpt_d (pt1, A (i + 2), A (i + 3));
      pt3 = cpt_d (pt2, A (i + 4), A (i + 5));
      cs_curve (env, param, pt1, pt2, pt3);
    }
    break;
  case 24:
    if (n < 8) break;
    for (; i + 6 <= n - 2; i += 6)
    {
      pt1 = cpt_d (env->pt, A (i), A (i + 1));
      pt2 = cpt_d (pt1, A (i + 2), A (i + 3));
      pt3 = cpt_d (pt2, A (i + 4), A (i + 5));
      cs_curve (env, param, pt1, pt2, pt3);
    }
    cs_line (env, param, cpt_d (env->pt, A (i), A (i + 1)));
    break;
  case 25:
    if (n < 8) break;
    for (; i + 2 <= n - 6; i += 2) cs_line (env, param, cpt_d (env->pt, A (i), A (i + 1)));
    pt1 = cpt_d (env->pt, A (i), A (i + 1));
    pt2 = cpt_d (pt1, A (i + 2), A (i + 3));
    pt3 = cpt_d (pt2, A (i + 4), A (i + 5));
    cs_curve (env, param, pt1, pt2, pt3);
    break;
  case 26:
  case 27:
  {
    int v = op == 26;
    pt1 = env->pt;
    if (e->nargs & 1) { pt1 = v ? cpt_dx (pt1, A (i)) : cpt_dy (pt1, A (i)); i++; }
    for (; i + 4 <= e->nargs; i += 4)
    {
      pt1 = v ? cpt_dy (pt1, A (i)) : cpt_dx (pt1, A (i));
      pt2 = cpt_d (pt1, A (i + 1), A (i + 2));
      pt3 = v ? cpt_dy (pt2, A (i + 3)) : cpt_dx (pt2, A (i + 3));
      cs_curve (env, param, pt1, pt2, pt3);
      pt1 = env->pt;
    }
    break;
  }
  case 30:
  case 31:
  {
    int v = op == 30;
#define FIRST(p, k) (v ? cpt_dy (p, A (k)) : cpt_dx (p, A (k)))
#define SECOND(p, k) (v ? cpt_dx (p, A (k)) : cpt_dy (p, A (k)))
    if ((e->nargs % 8) >= 4)
    {
      pt1 = FIRST (env->pt, i);
      pt2 = cpt_d (pt1, A (i + 1), A (i + 2));
      pt3 = SECOND (pt2, i + 3);
      i += 4;
      for (; i + 8 <= e->nargs; i += 8)
      {
	cs_curve (env, param, pt1, pt2, pt3);
	pt1 = SECOND (env->pt, i);
	pt2 = cpt_d (pt1, A (i + 1), A (i + 2));
	pt3 = FIRST (pt2, i + 3);
	cs_curve (env, param, pt1, pt2, pt3);
	pt1 = FIRST (pt3, i + 4);
	pt2 = cpt_d (pt1, A (i + 5), A (i + 6));
	pt3 = SECOND (pt2, i + 7);
      }
      if (i < e->nargs) pt3 = FIRST (pt3, i);
      cs_curve (env, param, pt1, pt2, pt3);
    }
    else
      for (; i + 8 <= e->nargs; i += 8)
      {
	pt1 = FIRST (env->pt, i);
	pt2 = cpt_d (pt1, A (i + 1), A (i + 2));
	pt3 = SECOND (pt2, i + 3);
	cs_curve (env, param, pt1, pt2, pt3);
	pt1 = SECOND (pt3, i + 4);
	pt2 = cpt_d (pt1, A (i + 5), A (i + 6));
	pt3 = FIRST (pt2, i + 7);
	if ((e->nargs - i < 16) && (e->nargs & 1)) pt3 = SECOND (pt3, i + 8);
	cs_curve (env, param, pt1, pt2, pt3);
      }
#undef FIRST
#undef SECOND
    break;
  }
  case 256 + 34:
    if (n != 7) { sr_set_error (&e->str); break; }
    {
      cpt_t p1 = cpt_dx (env->pt, A (0)), p2 = cpt_d (p1, A (1), A (2)), p3 = cpt_dx (p2, A (3));
      cpt_t p4 = cpt_dx (p3, A (4)), p5 = cpt_dx (p4, A (5));
      p5.y = p1.y;
      cpt_t p6 = cpt_dx (p5, A (6));
      cs_curve (env, param, p1, p2, p3);
      cs_curve (env, param, p4, p5, p6);
    }
    break;
  case 256 + 35:
    if (n != 13) { sr_set_error (&e->str); break; }
    {
      cpt_t p1 = cpt_d (env->pt, A (0), A (1)), p2 = cpt_d (p1, A (2), A (3)), p3 = cpt_d (p2, A (4), A (5));
      cpt_t p4 = cpt_d (p3, A (6), A (7)), p5 = cpt_d (p4, A (8), A (9)), p6 = cpt_d (p5, A (10), A (11));
      cs_curve (env, param, p1, p2, p3);
      cs_curve (env, param, p4, p5, p6);
    }
    break;
  case 256 + 36:
    if (n != 9) { sr_set_error (&e->str); break; }
    {
      cpt_t p1 = cpt_d (env->pt, A (0), A (1)), p2 = cpt_d (p1, A (2), A (3)), p3 = cpt_dx (p2, A (4));
      cpt_t p4 = cpt_dx (p3, A (5)), p5 = cpt_d (p4, A (6), A (7)), p6 = cpt_dx (p5, A (8));
      p6.y = env->pt.y;
      cs_curve (env, param, p1, p2, p3);
      cs_curve (env, param, p4, p5, p6);
    }
    break;
  case 256 + 37:
    if (n != 11) { sr_set_error (&e->str); break; }
    {
      cpt_t d = {0, 0};
      for (unsigned k = 0; k < 10; k += 2) d = cpt_d (d, A (k), A (k + 1));
      cpt_t p1 = cpt_d (env->pt, A (0), A (1)), p2 = cpt_d (p1, A (2), A (3)), p3 = cpt_d (p2, A (4), A (5));
      cpt_t p4 = cpt_d (p3, A (6), A (7)), p5 = cpt_d (p4, A (8), A (9)), p6 = p5;
      if (fabs (d.x) > fabs (d.y)) { p6 = cpt_dx (p6, A (10)); p6.y = env->pt.y; }
      else { p6.x = env->pt.x; p6 = cpt_dy (p6, A (10)); }
      cs_curve (env, param, p1, p2, p3);
      cs_curve (env, param, p4, p5, p6);
    }
    break;
  }
#undef A
  e->nargs = 0;
}

static void cs_process_op (unsigned op, cs_env_t *env, cs_param_t *param)
{
  cff_env_t *e = &env->e;
  switch (op)
  {
  case 256 + 0: e->nargs = 0; break;
  case 14:
    if (e->nargs >= 4) cs_seac (env, param);
    e->nargs = 0;
    env->endchar = 1;
    break;
  case 11: cs_return (env); break;
  case 255:
    if (sr_avail (&e->str, 4))
    {
      push_num (e, (int32_t) rd32 (e->str.d + e->str.off) / 65536.0);
      e->str.off += 4;
    }
    break;
  case 10: cs_call_subr (env, env->lsubrs); break;
  case 29: cs_call_subr (env, env->gsubrs); break;
  case 1: case 18: env->hstem_count += e->nargs / 2; e->nargs = 0; break;
  case 3: case 23: env->vstem_count += e->nargs / 2; e->nargs = 0; break;
  case 19: case 20:
    cs_hintmask_size (env);
    if (sr_avail (&e->str, env->hintmask_size))
    {
      e->nargs = 0;
      e->str.off += env->hintmask_size;
    }
    break;
  case 21: case 22: case 4: case 5: case 6: case 7: case 8: case 24: case 25: case 26: case 27: case 30: case 31:
  case 256 + 34: case 256 + 35: case 256 + 36: case 256 + 37:
    cs_path_op (op, env, param);
    break;
  default:
    process_number_op (op, e);
    break;
  }
}

static int cff_get (cff_accel_t *cff, hb_font_t *font, hb_codepoint_t glyph, cs_param_t *param, int in_seac, int64_t *budget)
{
  if (param->extents) cbounds_init (&param->bounds);
  param->path_open = 0;
  if (!cff->valid || glyph >= cff->num_glyphs) return 0;
  unsigned fd = fdselect_get_fd (cff->fdselect, glyph);
  tab_t str = cff_index_get (cff->charstrings, glyph);
  cs_env_t *env = (cs_env_t *) calloc (1, sizeof (cs_env_t));
  if (!env) return 0;
  env->e.str.d = str.d; env->e.str.len = str.len;
  env->in_seac = in_seac;
  env->budget = budget;
  env->gsubrs = cff->global_subrs;
  env->lsubrs = fd < cff->fd_count ? cff->local_subrs[fd] : NULL;
  int64_t remaining = budget ? *budget : (int64_t) CFF_MAX_OPS;
  unsigned max_ops = (unsigned) HB_CLAMP (remaining, (int64_t) 0, (int64_t) CFF_MAX_OPS), start_ops = max_ops;
  int ret = 1;
  if (max_ops)
    for (;;)
    {
      cs_process_op (fetch_op (&env->e.str), env, param);
      if (env_error (&env->e) || env->call_error || !--max_ops)
      {
	ret = 0;
	break;
      }
      if (env->endchar) break;
    }
  else
    ret = 0;
  if (budget) *budget -= (int64_t) (start_ops - max_ops);
  free (env);
  if (ret && !param->extents) ds_close_path (param->ds);
  return ret;
}

static int cff_get_extents (hb_font_t *font, hb_codepoint_t glyph, hb_glyph_extents_t *extents)
{
  cff_accel_t *cff = face_cff (font->face);
  if (!cff) return 0;
  int64_t budget = BUDGET_GLYPH;
  cs_param_t p;
  memset (&p, 0, sizeof (p));
  p.extents = 1;
  p.cff = cff;
  p.font = font;
  if (!cff_get (cff, font, glyph, &p, 0, &budget)) return 0;
  cbounds_t *b = &p.bounds;
  if (b->min.x >= b->max.x) extents->width = extents->x_bearing = 0;
  else
  {
    double xb = hbround (b->min.x);
    extents->x_bearing = clamp_pos (xb);
    extents->width = clamp_pos (hbround (b->max.x) - xb);
  }
  if (b->min.y >= b->max.y) extents->height = extents->y_bearing = 0;
  else
  {
    double yb = hbround (b->max.y);
    extents->y_bearing = clamp_pos (yb);
    extents->height = clamp_pos (hbround (b->min.y) - yb);
  }
  scale_glyph_extents (font, extents);
  return 1;
}

static int cff_get_path (hb_font_t *font, hb_codepoint_t glyph, dsess_t *ds, int64_t *budget)
{
  cff_accel_t *cff = face_cff (font->face);
  if (!cff) return 0;
  int64_t stack_budget = BUDGET_GLYPH;
  if (!budget) budget = &stack_budget;
  cs_param_t p;
  memset (&p, 0, sizeof (p));
  p.cff = cff;
  p.font = font;
  p.ds = ds;
  return cff_get (cff, font, glyph, &p, 0, budget);
}

enum { OP_MOVE_TO, OP_LINE_TO, OP_QUADRATIC_TO, OP_CUBIC_TO };

typedef struct { float x, y; int type; } opoint_t;

DEFINE_VEC (opvec_t, opoint_t)

typedef struct
{
  opvec_t points;
  uvec_t contours;
  int64_t *budget_remaining;
} outline_t;

static void outline_push (outline_t *o, float x, float y, int type)
{
  opoint_t p = {x, y, type};
  opvec_t_push (&o->points, p);
}

static void rec_move_to (void *d, dstate_t *st, float x, float y) { outline_push ((outline_t *) d, x, y, OP_MOVE_TO); }
static void rec_line_to (void *d, dstate_t *st, float x, float y) { outline_push ((outline_t *) d, x, y, OP_LINE_TO); }
static void rec_quadratic_to (void *d, dstate_t *st, float cx, float cy, float x, float y)
{
  outline_push ((outline_t *) d, cx, cy, OP_QUADRATIC_TO);
  outline_push ((outline_t *) d, x, y, OP_QUADRATIC_TO);
}
static void rec_cubic_to (void *d, dstate_t *st, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
  outline_push ((outline_t *) d, c1x, c1y, OP_CUBIC_TO);
  outline_push ((outline_t *) d, c2x, c2y, OP_CUBIC_TO);
  outline_push ((outline_t *) d, x, y, OP_CUBIC_TO);
}
static void rec_close_path (void *d, dstate_t *st)
{
  outline_t *o = (outline_t *) d;
  uvec_t_push (&o->contours, o->points.len);
}
static int64_t *rec_budget (void *d) { return ((outline_t *) d)->budget_remaining; }

static const draw_funcs_t outline_recording_funcs = {
  rec_move_to, rec_line_to, rec_quadratic_to, rec_cubic_to, rec_close_path, rec_budget
};

static void outline_free (outline_t *o)
{
  free (o->points.a);
  free (o->contours.a);
}

static void outline_replay (const outline_t *o, const draw_funcs_t *f, void *d)
{
  dsess_t s;
  dsess_init (&s, f, d);
  unsigned first = 0;
  for (unsigned c = 0; c < o->contours.len; c++)
  {
    unsigned end = o->contours.a[c];
    for (unsigned i = first; i < end;)
    {
      const opoint_t *p = &o->points.a[i++];
      switch (p->type)
      {
      case OP_MOVE_TO: ds_move_to (&s, p->x, p->y); break;
      case OP_LINE_TO: ds_line_to (&s, p->x, p->y); break;
      case OP_QUADRATIC_TO:
      {
	const opoint_t *p2 = &o->points.a[i++];
	ds_quadratic_to (&s, p->x, p->y, p2->x, p2->y);
	break;
      }
      case OP_CUBIC_TO:
      {
	const opoint_t *p2 = &o->points.a[i++], *p3 = &o->points.a[i++];
	ds_cubic_to (&s, p->x, p->y, p2->x, p2->y, p3->x, p3->y);
	break;
      }
      }
    }
    ds_close_path (&s);
    first = end;
  }
}

static void outline_translate (outline_t *o, float dx, float dy)
{
  for (unsigned i = 0; i < o->points.len; i++) { o->points.a[i].x += dx; o->points.a[i].y += dy; }
}

static void outline_slant (outline_t *o, float slant_xy)
{
  for (unsigned i = 0; i < o->points.len; i++) o->points.a[i].x += slant_xy * o->points.a[i].y;
}

static float outline_control_area (const outline_t *o)
{
  float a = 0;
  unsigned first = 0;
  for (unsigned c = 0; c < o->contours.len; c++)
  {
    unsigned contour = o->contours.a[c];
    for (unsigned i = first; i < contour; i++)
    {
      unsigned j = i + 1 < contour ? i + 1 : first;
      const opoint_t *pi = &o->points.a[i], *pj = &o->points.a[j];
      a += pi->x * pj->y - pi->y * pj->x;
    }
    first = contour;
  }
  return a * .5f;
}

typedef struct { float x, y; } ovec_t;

static float ovec_normalize_len (ovec_t *v)
{
  float len = hypotf (v->x, v->y);
  if (len) { v->x /= len; v->y /= len; }
  return len;
}

static void outline_embolden (outline_t *o, float x_strength, float y_strength, float x_shift, float y_shift)
{
  if (!x_strength && !y_strength) return;
  if (!o->points.len) return;
  x_strength /= 2.f;
  y_strength /= 2.f;
  int orientation_negative = outline_control_area (o) < 0;
  opoint_t *points = o->points.a;
  int first = 0;
  for (unsigned c = 0; c < o->contours.len; c++)
  {
    ovec_t in, out, anchor, shift;
    float l_in = 0, l_out = 0, l_anchor = 0, l, q, d;
    int last = (int) o->contours.a[c] - 1;
    in.x = in.y = anchor.x = anchor.y = 0;
    out.x = out.y = 0;
    for (int i = last, j = first, k = -1; j != i && i != k; j = j < last ? j + 1 : first)
    {
      if (j != k)
      {
	out.x = points[j].x - points[i].x;
	out.y = points[j].y - points[i].y;
	l_out = ovec_normalize_len (&out);
	if (l_out == 0) continue;
      }
      else
      {
	out = anchor;
	l_out = l_anchor;
      }
      if (l_in != 0)
      {
	if (k < 0)
	{
	  k = i;
	  anchor = in;
	  l_anchor = l_in;
	}
	d = in.x * out.x + in.y * out.y;
	if (d > -15.f / 16.f)
	{
	  d = d + 1.f;
	  shift.x = in.y + out.y;
	  shift.y = in.x + out.x;
	  if (orientation_negative) shift.x = -shift.x;
	  else shift.y = -shift.y;
	  q = out.x * in.y - out.y * in.x;
	  if (orientation_negative) q = -q;
	  l = HB_MIN (l_in, l_out);
	  if (x_strength * q <= l * d) shift.x = shift.x * x_strength / d;
	  else shift.x = shift.x * l / q;
	  if (y_strength * q <= l * d) shift.y = shift.y * y_strength / d;
	  else shift.y = shift.y * l / q;
	}
	else
	  shift.x = shift.y = 0;
	for (; i != j; i = i < last ? i + 1 : first)
	{
	  points[i].x += x_shift + shift.x;
	  points[i].y += y_shift + shift.y;
	}
      }
      else
	i = j;
      in = out;
      l_in = l_out;
    }
    first = last + 1;
  }
}

static int colr_get_extents (hb_font_t *font, hb_codepoint_t glyph, hb_glyph_extents_t *extents);
static int font_paint_extents (hb_font_t *font, hb_codepoint_t glyph, hb_glyph_extents_t *extents);

static int ot_draw_glyph (hb_font_t *font, hb_codepoint_t glyph, const draw_funcs_t *f, void *d)
{
  dsess_t ds;
  dsess_init (&ds, f, d);
  scalar_cache_t *gvar_cache = font->num_coords ? font_gvar_cache (font) : NULL;
  int ret = glyf_get_path (font, glyph, &ds, gvar_cache, ds.budget) ||
	    cff_get_path (font, glyph, &ds, ds.budget);
  ds_close_path (&ds);
  return ret;
}

static int font_draw_glyph (hb_font_t *font, hb_codepoint_t glyph, const draw_funcs_t *f, void *d)
{
  int embolden = font->x_strength || font->y_strength;
  int slanted = font->slant_xy != 0.f;
  if (!embolden && !slanted) return ot_draw_glyph (font, glyph, f, d);
  outline_t o;
  memset (&o, 0, sizeof (o));
  o.budget_remaining = f->budget ? f->budget (d) : NULL;
  if (!ot_draw_glyph (font, glyph, &outline_recording_funcs, &o)) { outline_free (&o); return 0; }
  if (slanted)
  {
    outline_translate (&o, 0.f, 0.f);
    outline_slant (&o, font->slant_xy);
    outline_translate (&o, 0.f, 0.f);
  }
  if (embolden)
  {
    float x_shift = font->embolden_in_place ? 0 : (float) font->x_strength / 2;
    float y_shift = (float) font->y_strength / 2;
    if (font->x_scale < 0) x_shift = -x_shift;
    if (font->y_scale < 0) y_shift = -y_shift;
    outline_embolden (&o, font->x_strength, font->y_strength, x_shift, y_shift);
  }
  outline_replay (&o, f, d);
  outline_free (&o);
  return 1;
}

static int cbdt_has_data (hb_face_t *face)
{
  tab_t t = face->cbdt;
  return tab_has (t, 0, 4) && (rd16 (t.d) == 2 || rd16 (t.d) == 3);
}

static int sbix_has_data (hb_face_t *face)
{
  tab_t t = face->sbix;
  return tab_has (t, 0, 8) && rd16 (t.d) >= 1 && rd32 (t.d + 4) <= (t.len - 8) / 4;
}

static int cbdt_image (hb_font_t *font, hb_codepoint_t glyph, const uint8_t **rec, unsigned *len, unsigned *format, unsigned *ppem_x, unsigned *ppem_y)
{
  tab_t t = font->face->cblc, d = font->face->cbdt;
  if (!cbdt_has_data (font->face) || !tab_has (t, 0, 8) || (rd16 (t.d) != 2 && rd16 (t.d) != 3)) return 0;
  unsigned count = rd32 (t.d + 4);
  if (!count || count > (t.len - 8) / 48) return 0;
  unsigned requested = HB_MAX (font->x_ppem, font->y_ppem);
  if (!requested) requested = 1u << 30;
  const uint8_t *s = t.d + 8;
  unsigned best = HB_MAX (s[44], s[45]);
  for (unsigned i = 1; i < count; i++)
  {
    const uint8_t *si = t.d + 8 + i * 48;
    unsigned ppem = HB_MAX (si[44], si[45]);
    if ((requested <= ppem && ppem < best) || (requested > best && ppem > best)) { s = si; best = ppem; }
  }
  unsigned arr = rd32 (s), n = rd32 (s + 8);
  if (!tab_has (t, arr, 0) || n > (t.len - arr) / 8) return 0;
  const uint8_t *r = NULL;
  for (unsigned i = 0; i < n && !r; i++)
  {
    const uint8_t *ri = t.d + arr + i * 8;
    if (rd16 (ri) <= glyph && glyph <= rd16 (ri + 2)) r = ri;
  }
  *ppem_x = s[44]; *ppem_y = s[45];
  if (!r || !s[44] || !s[45]) return 0;
  unsigned sub = arr + rd32 (r + 4), idx = glyph - rd16 (r);
  if (!tab_has (t, sub, 8)) return 0;
  const uint8_t *h = t.d + sub;
  unsigned o0, o1;
  switch (rd16 (h))
  {
  case 1: if (!tab_has (t, sub + 8, (idx + 2) * 4)) return 0; o0 = rd32 (h + 8 + idx * 4); o1 = rd32 (h + 12 + idx * 4); break;
  case 3: if (!tab_has (t, sub + 8, (idx + 2) * 2)) return 0; o0 = rd16 (h + 8 + idx * 2); o1 = rd16 (h + 10 + idx * 2); break;
  default: return 0;
  }
  if (o1 <= o0) return 0;
  unsigned off = rd32 (h + 4) + o0;
  *len = o1 - o0;
  *format = rd16 (h + 2);
  if (off > d.len || d.len - off < *len) return 0;
  *rec = d.d + off;
  return 1;
}

static int cbdt_get_extents (hb_font_t *font, hb_codepoint_t glyph, hb_glyph_extents_t *extents, int scale)
{
  const uint8_t *p;
  unsigned len, format, px, py;
  if (!cbdt_image (font, glyph, &p, &len, &format, &px, &py)) return 0;
  if ((format != 17 || len < 9) && (format != 18 || len < 12)) return 0;
  extents->x_bearing = (int8_t) p[2];
  extents->y_bearing = (int8_t) p[3];
  extents->width = p[1];
  extents->height = -(int) p[0];
  if (scale)
  {
    scale_glyph_extents (font, extents);
    float xs = font->face->upem / (float) px, ys = font->face->upem / (float) py;
    extents->x_bearing = clamp_pos (hbroundf (extents->x_bearing * xs));
    extents->y_bearing = clamp_pos (hbroundf (extents->y_bearing * ys));
    extents->width = clamp_pos (hbroundf (extents->width * xs));
    extents->height = clamp_pos (hbroundf (extents->height * ys));
  }
  return 1;
}

static int cbdt_png (hb_font_t *font, hb_codepoint_t glyph, const uint8_t **png, unsigned *png_len)
{
  const uint8_t *p;
  unsigned len, format, px, py, hdr;
  if (!cbdt_image (font, glyph, &p, &len, &format, &px, &py)) return 0;
  switch (format)
  {
  case 17: hdr = 9; break;
  case 18: hdr = 12; break;
  case 19: hdr = 4; break;
  default: return 0;
  }
  if (len < hdr) return 0;
  tab_t d = font->face->cbdt;
  unsigned start = (unsigned) (p - d.d) + hdr;
  *png = p + hdr;
  *png_len = start < d.len ? HB_MIN (rd32 (p + hdr - 4), d.len - start) : 0;
  return *png_len != 0;
}

static int sbix_png (hb_font_t *font, hb_codepoint_t glyph, const uint8_t **png, unsigned *png_len, int *x_off, int *y_off, unsigned *strike_ppem)
{
  if (!sbix_has_data (font->face)) return 0;
  tab_t t = font->face->sbix;
  unsigned count = rd32 (t.d + 4);
  if (!count) return 0;
  unsigned requested = HB_MAX (font->x_ppem, font->y_ppem);
  if (!requested) requested = 1u << 30;
  unsigned so = rd32 (t.d + 8), num_glyphs = font->face->num_glyphs;
  unsigned best = tab_has (t, so, 2) ? rd16 (t.d + so) : 0;
  for (unsigned i = 1; i < count; i++)
  {
    unsigned o = rd32 (t.d + 8 + i * 4), ppem = tab_has (t, o, 2) ? rd16 (t.d + o) : 0;
    if ((requested <= ppem && ppem < best) || (requested > best && ppem > best)) { so = o; best = ppem; }
  }
  if (!best || !tab_has (t, so, 4) || num_glyphs + 1 > (t.len - so - 4) / 4) return 0;
  const uint8_t *offs = t.d + so + 4;
  unsigned avail = t.len - so;
  for (unsigned retry = 9; retry; retry--)
  {
    if (glyph >= num_glyphs) return 0;
    unsigned o0 = rd32 (offs + glyph * 4), o1 = rd32 (offs + glyph * 4 + 4);
    if (o1 <= o0 || o1 - o0 <= 8 || o1 > avail) return 0;
    const uint8_t *g = t.d + so + o0;
    unsigned len = o1 - o0 - 8;
    if (rd32 (g + 4) == HB_TAG ('d','u','p','e'))
    {
      if (len < 2) return 0;
      glyph = rd16 (g + 8);
      continue;
    }
    if (rd32 (g + 4) != HB_TAG ('p','n','g',' ')) return 0;
    *strike_ppem = best;
    *x_off = (int16_t) rd16 (g);
    *y_off = (int16_t) rd16 (g + 2);
    *png = g + 8;
    *png_len = len;
    return 1;
  }
  return 0;
}

static int sbix_get_extents (hb_font_t *font, hb_codepoint_t glyph, hb_glyph_extents_t *extents, int scale)
{
  const uint8_t *png;
  unsigned len, ppem = 0;
  int x_off = 0, y_off = 0;
  if (!sbix_png (font, glyph, &png, &len, &x_off, &y_off, &ppem)) return 0;
  unsigned w = len >= 29 ? rd32 (png + 16) : 0, h = len >= 29 ? rd32 (png + 20) : 0;
  if (w >= 65536 || h >= 65536) return 0;
  extents->x_bearing = x_off;
  extents->y_bearing = clamp_pos ((double) h + y_off);
  extents->width = (hb_position_t) w;
  extents->height = -(hb_position_t) h;
  if (scale)
  {
    float s = font->face->upem / (float) ppem;
    extents->x_bearing = clamp_pos (hbroundf (extents->x_bearing * s));
    extents->y_bearing = clamp_pos (hbroundf (extents->y_bearing * s));
    extents->width = clamp_pos (hbroundf (extents->width * s));
    extents->height = clamp_pos (hbroundf (extents->height * s));
    scale_glyph_extents (font, extents);
  }
  return 1;
}

hb_bool_t
hb_ot_color_has_png (hb_face_t *face)
{
  return cbdt_has_data (face) || sbix_has_data (face);
}

static int ot_glyph_extents (hb_font_t *font, hb_codepoint_t glyph, hb_glyph_extents_t *extents)
{
  return sbix_get_extents (font, glyph, extents, 1) ||
	 cbdt_get_extents (font, glyph, extents, 1) ||
	 colr_get_extents (font, glyph, extents) ||
	 glyf_get_extents (font, glyph, extents) ||
	 cff_get_extents (font, glyph, extents);
}

static void synthetic_glyph_extents (hb_font_t *font, hb_glyph_extents_t *e)
{
  if (font->slant_xy)
  {
    hb_position_t x1 = e->x_bearing, y1 = e->y_bearing;
    hb_position_t x2 = e->x_bearing + e->width, y2 = e->y_bearing + e->height;
    x1 += floorf (HB_MIN (y1 * font->slant_xy, y2 * font->slant_xy));
    x2 += ceilf (HB_MAX (y1 * font->slant_xy, y2 * font->slant_xy));
    e->x_bearing = x1;
    e->width = x2 - e->x_bearing;
  }
  if (font->x_strength || font->y_strength)
  {
    int y_shift = font->y_strength;
    if (font->y_scale < 0) y_shift = -y_shift;
    e->y_bearing += y_shift;
    e->height -= y_shift;
    int x_shift = font->x_strength;
    if (font->x_scale < 0) x_shift = -x_shift;
    if (font->embolden_in_place) e->x_bearing -= x_shift / 2;
    e->width += x_shift;
  }
}

hb_bool_t
hb_font_get_glyph_extents (hb_font_t *font, hb_codepoint_t glyph, hb_glyph_extents_t *extents)
{
  memset (extents, 0, sizeof (*extents));
  if (!font->is_synthetic && ot_glyph_extents (font, glyph, extents)) return 1;
  if (font_paint_extents (font, glyph, extents)) return 1;
  extents_t de;
  extents_init (&de);
  if (font_draw_glyph (font, glyph, &draw_extents_funcs, &de))
  {
    *extents = extents_to_glyph (&de, 0, 0);
    return 1;
  }
  int ret = ot_glyph_extents (font, glyph, extents);
  if (ret) synthetic_glyph_extents (font, extents);
  return ret;
}

#define HB_PI 3.14159265358979323846f
#define NO_VARIATION 0xFFFFFFFFu

enum {
  MODE_CLEAR, MODE_SRC, MODE_DEST, MODE_SRC_OVER, MODE_DEST_OVER, MODE_SRC_IN, MODE_DEST_IN,
  MODE_SRC_OUT, MODE_DEST_OUT, MODE_SRC_ATOP, MODE_DEST_ATOP, MODE_XOR, MODE_PLUS, MODE_SCREEN,
  MODE_OVERLAY, MODE_DARKEN, MODE_LIGHTEN, MODE_COLOR_DODGE, MODE_COLOR_BURN, MODE_HARD_LIGHT,
  MODE_SOFT_LIGHT, MODE_DIFFERENCE, MODE_EXCLUSION, MODE_MULTIPLY, MODE_HSL_HUE, MODE_HSL_SATURATION,
  MODE_HSL_COLOR, MODE_HSL_LUMINOSITY
};
enum { EXTEND_PAD, EXTEND_REPEAT, EXTEND_REFLECT };

#define HB_COLOR(b,g,r,a) ((hb_color_t) HB_TAG ((b),(g),(r),(a)))
#define color_blue(c) (((c) >> 24) & 0xFF)
#define color_green(c) (((c) >> 16) & 0xFF)
#define color_red(c) (((c) >> 8) & 0xFF)
#define color_alpha_byte(c) ((c) & 0xFF)

static const uint8_t null_pool[64];

typedef struct { float xx, yx, xy, yy, x0, y0; } xform_t;

static const xform_t xform_identity = {1, 0, 0, 1, 0, 0};

static void xform_multiply (xform_t *t, const xform_t *o, int before)
{
  const xform_t a = before ? *o : *t, b = before ? *t : *o;
  t->xx = a.xx * b.xx + a.xy * b.yx;
  t->yx = a.yx * b.xx + a.yy * b.yx;
  t->xy = a.xx * b.xy + a.xy * b.yy;
  t->yy = a.yx * b.xy + a.yy * b.yy;
  t->x0 = a.xx * b.x0 + a.xy * b.y0 + a.x0;
  t->y0 = a.yx * b.x0 + a.yy * b.y0 + a.y0;
}

static inline void xform_point (const xform_t *t, float *x, float *y)
{
  float nx = t->x0 + t->xx * *x + t->xy * *y;
  float ny = t->y0 + t->yx * *x + t->yy * *y;
  *x = nx;
  *y = ny;
}

static void xform_extents (const xform_t *t, extents_t *e)
{
  float qx[4] = {e->xmin, e->xmin, e->xmax, e->xmax}, qy[4] = {e->ymin, e->ymax, e->ymin, e->ymax};
  extents_init (e);
  for (unsigned i = 0; i < 4; i++)
  {
    xform_point (t, &qx[i], &qy[i]);
    extents_add_point (e, qx[i], qy[i]);
  }
}

typedef struct color_line_t color_line_t;

typedef struct
{
  float offset;
  int is_foreground;
  hb_color_t color;
} color_stop_t;

typedef struct
{
  void (*push_transform) (void *d, float xx, float yx, float xy, float yy, float dx, float dy);
  void (*pop_transform) (void *d);
  int (*color_glyph) (void *d, hb_codepoint_t glyph, hb_font_t *font);
  void (*push_clip_glyph) (void *d, hb_codepoint_t glyph, hb_font_t *font);
  void (*push_clip_rectangle) (void *d, float xmin, float ymin, float xmax, float ymax);
  void (*pop_clip) (void *d);
  void (*color) (void *d, int is_foreground, hb_color_t color);
  void (*linear_gradient) (void *d, color_line_t *cl, float x0, float y0, float x1, float y1, float x2, float y2);
  void (*radial_gradient) (void *d, color_line_t *cl, float x0, float y0, float r0, float x1, float y1, float r1);
  void (*sweep_gradient) (void *d, color_line_t *cl, float cx, float cy, float start_angle, float end_angle);
  void (*push_group) (void *d);
  void (*push_group_for) (void *d, int mode);
  void (*pop_group) (void *d, int mode);
  void (*fill_glyph) (void *d, hb_codepoint_t glyph, hb_font_t *font, int is_foreground, hb_color_t color);
  int (*image) (void *d, const uint8_t *png, unsigned len, unsigned width, unsigned height, hb_glyph_extents_t *extents);
} paint_funcs_t;

static void pf_push_transform (const paint_funcs_t *f, void *d, float xx, float yx, float xy, float yy, float dx, float dy)
{
  if (dx == -0.f) dx = 0.f;
  if (dy == -0.f) dy = 0.f;
  if (f->push_transform) f->push_transform (d, xx, yx, xy, yy, dx, dy);
}
static void pf_push_xform (const paint_funcs_t *f, void *d, xform_t t) { pf_push_transform (f, d, t.xx, t.yx, t.xy, t.yy, t.x0, t.y0); }
static void pf_pop_transform (const paint_funcs_t *f, void *d) { if (f->pop_transform) f->pop_transform (d); }
static void pf_push_clip_glyph (const paint_funcs_t *f, void *d, hb_codepoint_t g, hb_font_t *font) { if (f->push_clip_glyph) f->push_clip_glyph (d, g, font); }
static void pf_push_clip_rectangle (const paint_funcs_t *f, void *d, float a, float b, float c, float e) { if (f->push_clip_rectangle) f->push_clip_rectangle (d, a, b, c, e); }
static void pf_pop_clip (const paint_funcs_t *f, void *d) { if (f->pop_clip) f->pop_clip (d); }
static void pf_color (const paint_funcs_t *f, void *d, int fg, hb_color_t c) { if (f->color) f->color (d, fg, c); }
static void pf_push_group_for (const paint_funcs_t *f, void *d, int mode)
{
  if (f->push_group_for) f->push_group_for (d, mode);
  else if (f->push_group) f->push_group (d);
}
static void pf_pop_group (const paint_funcs_t *f, void *d, int mode) { if (f->pop_group) f->pop_group (d, mode); }
static int pf_image (const paint_funcs_t *f, void *d, const uint8_t *png, unsigned len, unsigned w, unsigned h, hb_glyph_extents_t *e)
{
  return f->image ? f->image (d, png, len, w, h, e) : 0;
}
static void pf_fill_glyph (const paint_funcs_t *f, void *d, hb_codepoint_t g, hb_font_t *font, int fg, hb_color_t c)
{
  if (f->fill_glyph) { f->fill_glyph (d, g, font, fg, c); return; }
  pf_push_clip_glyph (f, d, g, font);
  pf_color (f, d, fg, c);
  pf_pop_clip (f, d);
}

static void pf_push_font_transform (const paint_funcs_t *f, void *d, const hb_font_t *font)
{
  float upem = font->face->upem;
  int xs = font->x_scale, ys = font->y_scale;
  pf_push_transform (f, d, xs / upem, 0, 0, ys / upem, 0, 0);
}

static void pf_push_inverse_font_transform (const paint_funcs_t *f, void *d, const hb_font_t *font)
{
  float upem = font->face->upem;
  int xs = font->x_scale ? font->x_scale : upem;
  int ys = font->y_scale ? font->y_scale : upem;
  pf_push_transform (f, d, upem / xs, 0, 0, upem / ys, 0, 0);
}

static void sincos_hb (float r, float *s, float *c) { *c = cosf (r); *s = sinf (r); }

static unsigned cpal_palette_count (hb_face_t *face)
{
  tab_t t = face->cpal;
  if (!tab_has (t, 0, 12)) return 0;
  unsigned version = rd16 (t.d), num_colors = rd16 (t.d + 2), num_palettes = rd16 (t.d + 4), num_records = rd16 (t.d + 6);
  if (!tab_has (t, rd32 (t.d + 8), num_records * 4) || !tab_has (t, 12, num_palettes * 2)) return 0;
  if (version)
  {
    unsigned tail = 12 + num_palettes * 2;
    if (!tab_has (t, tail, 12)) return 0;
    uint32_t pf = rd32 (t.d + tail), pl = rd32 (t.d + tail + 4), cl = rd32 (t.d + tail + 8);
    if ((pf && !tab_has (t, pf, num_palettes * 4)) || (pl && !tab_has (t, pl, num_palettes * 2)) ||
	(cl && !tab_has (t, cl, num_colors * 2))) return 0;
  }
  return num_palettes;
}

static void cpal_palette (hb_face_t *face, unsigned palette, const uint8_t **colors, unsigned *count)
{
  tab_t t = face->cpal;
  *colors = NULL;
  *count = 0;
  if (palette >= cpal_palette_count (face)) return;
  unsigned num_colors = rd16 (t.d + 2), num_records = rd16 (t.d + 6), start = rd16 (t.d + 12 + palette * 2);
  if (start > num_records) return;
  *colors = t.d + rd32 (t.d + 8) + start * 4;
  *count = HB_MIN (num_colors, num_records - start);
}

typedef struct
{
  int init, valid;
  tab_t t;
  unsigned version, num_base, num_layers;
  const uint8_t *base_glyphs, *layers;
  const uint8_t *base_list, *layer_list, *clip_list;
  tab_t var_idx_map, ivs;
} colr_t;

static int colr_check (const colr_t *c, const uint8_t *p, uint64_t n)
{
  return p >= c->t.d && p <= c->t.d + c->t.len && (uint64_t) (c->t.d + c->t.len - p) >= n;
}

static const uint8_t *colr_off (const colr_t *c, const uint8_t *base, uint32_t off)
{
  if (!off) return NULL;
  const uint8_t *p = base + off;
  return p <= c->t.d + c->t.len ? p : NULL;
}

static colr_t *face_colr (hb_face_t *face)
{
  if (!face->colr_accel) face->colr_accel = calloc (1, sizeof (colr_t));
  colr_t *c = (colr_t *) face->colr_accel;
  if (!c || c->init) return c;
  c->init = 1;
  tab_t t = face->colr;
  c->t = t;
  if (!tab_has (t, 0, 14)) return c;
  c->version = rd16 (t.d);
  c->num_base = rd16 (t.d + 2);
  c->num_layers = rd16 (t.d + 12);
  uint32_t bo = rd32 (t.d + 4), lo = rd32 (t.d + 8);
  if (!tab_has (t, bo, c->num_base * 6) || !tab_has (t, lo, c->num_layers * 4)) return c;
  c->base_glyphs = t.d + bo;
  c->layers = t.d + lo;
  if (c->version)
  {
    if (!tab_has (t, 14, 20)) return c;
    const uint8_t *p;
    if ((p = colr_off (c, t.d, rd32 (t.d + 14))) && colr_check (c, p, 4) && colr_check (c, p + 4, (uint64_t) rd32 (p) * 6))
      c->base_list = p;
    if ((p = colr_off (c, t.d, rd32 (t.d + 18))) && colr_check (c, p, 4) && colr_check (c, p + 4, (uint64_t) rd32 (p) * 4))
      c->layer_list = p;
    if ((p = colr_off (c, t.d, rd32 (t.d + 22))) && colr_check (c, p, 5) && colr_check (c, p + 5, (uint64_t) rd32 (p + 1) * 7))
      c->clip_list = p;
    uint32_t mo = rd32 (t.d + 26), vo = rd32 (t.d + 30);
    if (mo && mo <= t.len)
    {
      tab_t m = tab_at (t, mo);
      int ok = m.len >= 2;
      if (ok && m.d[0] == 0) ok = m.len >= 4 && tab_has (m, 4, (uint64_t) rd16 (m.d + 2) * (((m.d[1] >> 4) & 3) + 1));
      else if (ok && m.d[0] == 1) ok = m.len >= 6 && tab_has (m, 6, (uint64_t) rd32 (m.d + 2) * (((m.d[1] >> 4) & 3) + 1));
      if (ok) c->var_idx_map = m;
    }
    if (vo && vo <= t.len)
    {
      tab_t v = tab_at (t, vo);
      if (ivs_valid (v)) c->ivs = v;
    }
  }
  c->valid = 1;
  return c;
}

static int colr_has_data (colr_t *c) { return c && c->valid && (c->num_base || c->version); }

static const uint8_t *colr_base_glyph_paint (colr_t *c, hb_codepoint_t glyph)
{
  if (c->version < 1) return NULL;
  const uint8_t *l = c->base_list;
  if (!l) return NULL;
  int min = 0, max = (int) rd32 (l) - 1;
  while (min <= max)
  {
    int mid = ((unsigned) min + (unsigned) max) / 2;
    const uint8_t *r = l + 4 + mid * 6;
    unsigned g = rd16 (r);
    if (glyph < g) max = mid - 1;
    else if (glyph > g) min = mid + 1;
    else
    {
      const uint8_t *p = colr_off (c, l, rd32 (r + 2));
      return p ? p : null_pool;
    }
  }
  return NULL;
}

hb_bool_t
hb_ot_color_glyph_has_paint (hb_face_t *face, hb_codepoint_t glyph)
{
  colr_t *c = face_colr (face);
  return c && c->valid && colr_base_glyph_paint (c, glyph) != NULL;
}

typedef struct
{
  colr_t *colr;
  const paint_funcs_t *f;
  void *d;
  hb_font_t *font;
  const uint8_t *palette;
  unsigned palette_len;
  hb_color_t foreground;
  const int *coords;
  unsigned ncoords;
  decycler_t glyphs_decycler, layers_decycler;
  int depth_left, edge_count;
} paint_ctx_t;

struct color_line_t
{
  const uint8_t *p;
  int var;
  paint_ctx_t *c;
};

static float colr_inst (paint_ctx_t *c, uint32_t base, unsigned offset)
{
  if (!c->ncoords || base == NO_VARIATION) return 0.f;
  uint32_t idx = base + offset;
  if (c->colr->var_idx_map.d) idx = delta_set_index_map (c->colr->var_idx_map, idx);
  return ivs_get_delta (c->colr->ivs, idx, c->coords, c->ncoords, NULL);
}

static unsigned color_alpha (hb_color_t color, float alpha)
{
  return (unsigned) hbroundf (color_alpha_byte (color) * HB_CLAMP (alpha, 0.f, 1.f));
}

static hb_color_t paint_get_color (paint_ctx_t *c, unsigned index, float alpha, int *is_foreground)
{
  hb_color_t color = c->foreground;
  *is_foreground = 1;
  if (index != 0xFFFF)
  {
    color = index < c->palette_len ? rd32 (c->palette + index * 4) : 0;
    *is_foreground = 0;
  }
  return HB_COLOR (color_blue (color), color_green (color), color_red (color), color_alpha (color, alpha));
}

static inline float f2dot14 (const uint8_t *p, float offset) { return ((int32_t) rds16 (p) + offset) * (1.f / (1 << 14)); }
static inline float f16dot16 (const uint8_t *p, float offset) { return ((int32_t) rds32 (p) + offset) * (1.f / (1 << 16)); }

static unsigned color_line_get_stops (color_line_t *cl, unsigned start, unsigned *count, color_stop_t *stops)
{
  const uint8_t *p = cl->p;
  unsigned len = rd16 (p + 1), size = cl->var ? 10 : 6;
  if (count && stops)
  {
    unsigned i;
    for (i = 0; i < *count && start + i < len; i++)
    {
      const uint8_t *s = p + 3 + (start + i) * size;
      uint32_t vb = cl->var ? rd32 (s + 6) : NO_VARIATION;
      stops[i].offset = f2dot14 (s, colr_inst (cl->c, vb, 0));
      stops[i].color = paint_get_color (cl->c, rd16 (s + 2), f2dot14 (s + 4, colr_inst (cl->c, vb, 1)), &stops[i].is_foreground);
    }
    *count = i;
  }
  return len;
}

static int color_line_get_extend (color_line_t *cl)
{
  switch (cl->p[0])
  {
  case EXTEND_REPEAT: return EXTEND_REPEAT;
  case EXTEND_REFLECT: return EXTEND_REFLECT;
  default: return EXTEND_PAD;
  }
}

static const uint8_t *colr_color_line (colr_t *c, const uint8_t *paint, int var)
{
  const uint8_t *p = colr_off (c, paint, rd24 (paint + 1));
  if (p && colr_check (c, p, 3) && colr_check (c, p + 3, (uint64_t) rd16 (p + 1) * (var ? 10 : 6))) return p;
  return null_pool;
}

static const unsigned paint_sizes[33] = {
  1, 6, 5, 9, 16, 20, 16, 20, 12, 16, 6, 3, 7, 7, 8, 12, 8, 12, 12, 16, 6, 10, 10, 14, 6, 10, 10, 14, 8, 12, 12, 16, 8
};

static const uint8_t *colr_paint_at (colr_t *c, const uint8_t *base, uint32_t off)
{
  const uint8_t *p = colr_off (c, base, off);
  if (!p) return null_pool;
  if (!colr_check (c, p, 1)) return null_pool;
  unsigned fmt = p[0];
  if (fmt <= 32 && !colr_check (c, p, paint_sizes[fmt])) return null_pool;
  return p;
}

static int colr_get_clip (colr_t *c, hb_codepoint_t glyph, hb_glyph_extents_t *extents, const int *coords, unsigned ncoords)
{
  if (c->version < 1 || !c->clip_list) return 0;
  const uint8_t *l = c->clip_list;
  int min = 0, max = (int) rd32 (l + 1) - 1;
  const uint8_t *rec = NULL;
  while (min <= max)
  {
    int mid = ((unsigned) min + (unsigned) max) / 2;
    const uint8_t *r = l + 5 + mid * 7;
    if (glyph < rd16 (r)) max = mid - 1;
    else if (glyph > rd16 (r + 2)) min = mid + 1;
    else { rec = r; break; }
  }
  if (!rec) return 0;
  const uint8_t *b = colr_off (c, l, rd24 (rec + 4));
  if (!b || !colr_check (c, b, 1)) return 0;
  unsigned fmt = b[0];
  if ((fmt == 1 && !colr_check (c, b, 9)) || (fmt == 2 && !colr_check (c, b, 13))) return 0;
  if (fmt != 1 && fmt != 2) return 0;
  int xmin = rds16 (b + 1), ymin = rds16 (b + 3), xmax = rds16 (b + 5), ymax = rds16 (b + 7);
  if (fmt == 2 && ncoords)
  {
    paint_ctx_t pc;
    memset (&pc, 0, sizeof (pc));
    pc.colr = c; pc.coords = coords; pc.ncoords = ncoords;
    uint32_t vb = rd32 (b + 9);
    int v[4];
    for (unsigned i = 0; i < 4; i++) v[i] = (int) HB_CLAMP ((double) hbroundf (colr_inst (&pc, vb, i)), (double) INT_MIN, (double) INT_MAX);
    xmin = sat_add (xmin, v[0]); ymin = sat_add (ymin, v[1]);
    xmax = sat_add (xmax, v[2]); ymax = sat_add (ymax, v[3]);
  }
  extents->x_bearing = xmin;
  extents->y_bearing = ymax;
  extents->width = sat_sub (xmax, xmin);
  extents->height = sat_sub (ymin, ymax);
  return 1;
}

static void paint_recurse (paint_ctx_t *c, const uint8_t *paint);

static int paint_get_solid (paint_ctx_t *c, const uint8_t *p, int *is_foreground, hb_color_t *color)
{
  if (p[0] != 2 && p[0] != 3) return 0;
  uint32_t vb = p[0] == 3 ? rd32 (p + 5) : NO_VARIATION;
  *color = paint_get_color (c, rd16 (p + 1), f2dot14 (p + 3, colr_inst (c, vb, 0)), is_foreground);
  return 1;
}

static void paint_dispatch (paint_ctx_t *c, const uint8_t *p)
{
  colr_t *colr = c->colr;
  const paint_funcs_t *f = c->f;
  void *d = c->d;
  unsigned fmt = p[0];
  int var = (fmt & 1) && fmt >= 3 && fmt <= 31;
  uint32_t vb = NO_VARIATION;
  if (var && fmt != 13) vb = rd32 (p + paint_sizes[fmt] - 4);
#define I(k) colr_inst (c, vb, (k))
#define SRC colr_paint_at (colr, p, rd24 (p + 1))
  switch (fmt)
  {
  case 1:
  {
    unsigned n = p[1];
    uint32_t first = rd32 (p + 2);
    dnode_t node;
    dnode_init (&node, &c->layers_decycler);
    const uint8_t *ll = colr->layer_list;
    unsigned count = ll ? rd32 (ll) : 0;
    for (uint32_t i = first; i < first + n; i++)
    {
      if (!dnode_visit (&node, i)) break;
      uint32_t off = i < count ? rd32 (ll + 4 + i * 4) : 0;
      paint_recurse (c, off ? colr_paint_at (colr, ll, off) : null_pool);
    }
    dnode_fini (&node);
    break;
  }
  case 2: case 3:
  {
    int fg;
    hb_color_t color;
    paint_get_solid (c, p, &fg, &color);
    pf_color (f, d, fg, color);
    break;
  }
  case 4: case 5:
  {
    color_line_t cl = {colr_color_line (colr, p, var), var, c};
    if (f->linear_gradient)
      f->linear_gradient (d, &cl, rds16 (p + 4) + I (0), rds16 (p + 6) + I (1), rds16 (p + 8) + I (2),
			  rds16 (p + 10) + I (3), rds16 (p + 12) + I (4), rds16 (p + 14) + I (5));
    break;
  }
  case 6: case 7:
  {
    color_line_t cl = {colr_color_line (colr, p, var), var, c};
    if (f->radial_gradient)
      f->radial_gradient (d, &cl, rds16 (p + 4) + I (0), rds16 (p + 6) + I (1), rd16 (p + 8) + I (2),
			  rds16 (p + 10) + I (3), rds16 (p + 12) + I (4), rd16 (p + 14) + I (5));
    break;
  }
  case 8: case 9:
  {
    color_line_t cl = {colr_color_line (colr, p, var), var, c};
    if (f->sweep_gradient)
      f->sweep_gradient (d, &cl, rds16 (p + 4) + I (0), rds16 (p + 6) + I (1),
			 (f2dot14 (p + 8, I (2)) + 1) * HB_PI, (f2dot14 (p + 10, I (3)) + 1) * HB_PI);
    break;
  }
  case 10:
  {
    hb_codepoint_t gid = rd16 (p + 4);
    const uint8_t *sub = SRC;
    int fg;
    hb_color_t color;
    if (c->depth_left > 0 && c->edge_count > 0 && paint_get_solid (c, sub, &fg, &color))
    {
      c->edge_count--;
      pf_push_inverse_font_transform (f, d, c->font);
      pf_fill_glyph (f, d, gid, c->font, fg, color);
      pf_pop_transform (f, d);
      break;
    }
    pf_push_inverse_font_transform (f, d, c->font);
    pf_push_clip_glyph (f, d, gid, c->font);
    pf_push_font_transform (f, d, c->font);
    paint_recurse (c, sub);
    pf_pop_transform (f, d);
    pf_pop_clip (f, d);
    pf_pop_transform (f, d);
    break;
  }
  case 11:
  {
    hb_codepoint_t gid = rd16 (p + 1);
    dnode_t node;
    dnode_init (&node, &c->glyphs_decycler);
    if (!dnode_visit (&node, gid)) { dnode_fini (&node); break; }
    pf_push_inverse_font_transform (f, d, c->font);
    if (f->color_glyph && f->color_glyph (d, gid, c->font))
    {
      pf_pop_transform (f, d);
      dnode_fini (&node);
      break;
    }
    pf_pop_transform (f, d);
    const uint8_t *paint = colr_base_glyph_paint (colr, gid);
    hb_glyph_extents_t e = {0, 0, 0, 0};
    int has_clip = colr_get_clip (colr, gid, &e, c->coords, c->ncoords);
    if (has_clip)
      pf_push_clip_rectangle (f, d, (float) e.x_bearing, (float) e.y_bearing + e.height,
			      (float) e.x_bearing + e.width, (float) e.y_bearing);
    if (paint) paint_recurse (c, paint);
    if (has_clip) pf_pop_clip (f, d);
    dnode_fini (&node);
    break;
  }
  case 12: case 13:
  {
    const uint8_t *t = colr_off (colr, p, rd24 (p + 4));
    if (!t || !colr_check (colr, t, fmt == 13 ? 28 : 24)) t = null_pool;
    uint32_t tvb = fmt == 13 ? rd32 (t + 24) : NO_VARIATION;
#define TI(k) colr_inst (c, tvb, (k))
    pf_push_transform (f, d, f16dot16 (t, TI (0)), f16dot16 (t + 4, TI (1)), f16dot16 (t + 8, TI (2)),
		       f16dot16 (t + 12, TI (3)), f16dot16 (t + 16, TI (4)), f16dot16 (t + 20, TI (5)));
#undef TI
    paint_recurse (c, SRC);
    pf_pop_transform (f, d);
    break;
  }
  case 14: case 15:
  {
    float dx = rds16 (p + 4) + I (0), dy = rds16 (p + 6) + I (1);
    xform_t t = {1, 0, 0, 1, dx, dy};
    pf_push_xform (f, d, t);
    paint_recurse (c, SRC);
    pf_pop_transform (f, d);
    break;
  }
  case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23:
  {
    int uniform = fmt >= 20, center = fmt == 18 || fmt == 19 || fmt >= 22;
    float sx = f2dot14 (p + 4, I (0)), sy;
    unsigned k = 1, o = 6;
    if (uniform) sy = sx;
    else { sy = f2dot14 (p + 6, I (1)); k = 2; o = 8; }
    xform_t t = {sx, 0, 0, sy, 0, 0};
    if (center)
    {
      float cx = rds16 (p + o) + I (k), cy = rds16 (p + o + 2) + I (k + 1);
      t.x0 = cx ? (1 - sx) * cx : 0;
      t.y0 = cy ? (1 - sy) * cy : 0;
    }
    pf_push_xform (f, d, t);
    paint_recurse (c, SRC);
    pf_pop_transform (f, d);
    break;
  }
  case 24: case 25: case 26: case 27:
  {
    float a = f2dot14 (p + 4, I (0));
    float s, co;
    sincos_hb (a * HB_PI, &s, &co);
    xform_t t = {co, s, -s, co, 0, 0};
    if (fmt >= 26)
    {
      float cx = rds16 (p + 6) + I (1), cy = rds16 (p + 8) + I (2);
      t.x0 = (1 - co) * cx + s * cy;
      t.y0 = -s * cx + (1 - co) * cy;
    }
    pf_push_xform (f, d, t);
    paint_recurse (c, SRC);
    pf_pop_transform (f, d);
    break;
  }
  case 28: case 29: case 30: case 31:
  {
    float sx = f2dot14 (p + 4, I (0)), sy = f2dot14 (p + 6, I (1));
    float kx = -sx * HB_PI, ky = sy * HB_PI;
    xform_t t;
    if (fmt >= 30)
    {
      float cx = rds16 (p + 8) + I (2), cy = rds16 (p + 10) + I (3);
      kx = kx ? tanf (kx) : 0;
      ky = ky ? tanf (ky) : 0;
      t.xx = 1; t.yx = ky; t.xy = kx; t.yy = 1;
      t.x0 = cy ? -kx * cy : 0;
      t.y0 = cx ? -ky * cx : 0;
    }
    else
    {
      t.xx = 1; t.yx = ky ? tanf (ky) : 0; t.xy = kx ? tanf (kx) : 0; t.yy = 1; t.x0 = 0; t.y0 = 0;
    }
    pf_push_xform (f, d, t);
    paint_recurse (c, SRC);
    pf_pop_transform (f, d);
    break;
  }
  case 32:
  {
    unsigned m = p[4];
    int mode = m <= MODE_HSL_LUMINOSITY ? (int) m : MODE_CLEAR;
    pf_push_group_for (f, d, MODE_SRC_OVER);
    paint_recurse (c, colr_paint_at (colr, p, rd24 (p + 5)));
    pf_push_group_for (f, d, mode);
    paint_recurse (c, SRC);
    pf_pop_group (f, d, mode);
    pf_pop_group (f, d, MODE_SRC_OVER);
    break;
  }
  }
#undef I
#undef SRC
}

static void paint_recurse (paint_ctx_t *c, const uint8_t *paint)
{
  if (c->depth_left <= 0 || c->edge_count <= 0) return;
  c->depth_left--;
  c->edge_count--;
  paint_dispatch (c, paint);
  c->depth_left++;
}

static int colr_paint_glyph (hb_font_t *font, hb_codepoint_t glyph, const paint_funcs_t *f, void *d,
			     unsigned palette, hb_color_t foreground, int clip);

typedef struct { int bounded; unsigned clips; uvec_t groups; } paint_bounded_t;

static void pb_push_clip_glyph (void *d, hb_codepoint_t g, hb_font_t *font) { ((paint_bounded_t *) d)->clips++; }
static void pb_push_clip_rectangle (void *d, float a, float b, float c, float e) { ((paint_bounded_t *) d)->clips++; }
static void pb_pop_clip (void *d)
{
  paint_bounded_t *c = (paint_bounded_t *) d;
  if (c->clips) c->clips--;
}
static void pb_push_group (void *d)
{
  paint_bounded_t *c = (paint_bounded_t *) d;
  uvec_t_push (&c->groups, c->bounded);
  c->bounded = 1;
}
static void pb_pop_group (void *d, int mode)
{
  paint_bounded_t *c = (paint_bounded_t *) d;
  int src = c->bounded;
  c->bounded = c->groups.len ? (int) c->groups.a[--c->groups.len] : 0;
  switch (mode)
  {
  case MODE_CLEAR: c->bounded = 1; break;
  case MODE_SRC: case MODE_SRC_OUT: c->bounded = src; break;
  case MODE_DEST: case MODE_DEST_OUT: break;
  case MODE_SRC_IN: case MODE_DEST_IN: c->bounded = c->bounded && src; break;
  default: c->bounded = c->bounded || src; break;
  }
}
static void pb_paint (void *d)
{
  paint_bounded_t *c = (paint_bounded_t *) d;
  if (!c->clips) c->bounded = 0;
}
static void pb_color (void *d, int fg, hb_color_t col) { pb_paint (d); }
static void pb_linear (void *d, color_line_t *cl, float a, float b, float c, float e, float g, float h) { pb_paint (d); }
static void pb_radial (void *d, color_line_t *cl, float a, float b, float c, float e, float g, float h) { pb_paint (d); }
static void pb_sweep (void *d, color_line_t *cl, float a, float b, float c, float e) { pb_paint (d); }
static int pb_image (void *d, const uint8_t *png, unsigned len, unsigned w, unsigned h, hb_glyph_extents_t *g)
{
  pb_push_clip_rectangle (d, 0, 0, 0, 0);
  pb_paint (d);
  pb_pop_clip (d);
  return 1;
}

static const paint_funcs_t paint_bounded_funcs = {
  NULL, NULL, NULL, pb_push_clip_glyph, pb_push_clip_rectangle, pb_pop_clip, pb_color,
  pb_linear, pb_radial, pb_sweep, pb_push_group, NULL, pb_pop_group, NULL, pb_image
};

static int colr_paint_glyph (hb_font_t *font, hb_codepoint_t glyph, const paint_funcs_t *f, void *d,
			     unsigned palette, hb_color_t foreground, int clip)
{
  colr_t *colr = face_colr (font->face);
  if (!colr_has_data (colr)) return 0;
  paint_ctx_t c;
  memset (&c, 0, sizeof (c));
  c.colr = colr;
  c.f = f;
  c.d = d;
  unsigned pcount = cpal_palette_count (font->face);
  cpal_palette (font->face, palette < pcount ? palette : 0, &c.palette, &c.palette_len);
  c.foreground = foreground;
  c.coords = font->coords;
  c.ncoords = font->has_nonzero_coords ? font->num_coords : 0;
  c.depth_left = MAX_NESTING_LEVEL;
  c.edge_count = MAX_GRAPH_EDGE_COUNT;
  if (font->is_synthetic)
  {
    c.font = font_create_unsynthetic (font);
    hb_font_set_synthetic_bold (c.font, 0, 0, 1);
    hb_font_set_synthetic_slant (c.font, 0);
  }
  else
  {
    font->ref++;
    c.font = font;
  }
  int ret = 0;
  dnode_t node;
  dnode_init (&node, &c.glyphs_decycler);
  dnode_visit (&node, glyph);
  const uint8_t *paint = colr_base_glyph_paint (colr, glyph);
  if (paint)
  {
    int is_bounded = 1;
    hb_glyph_extents_t e = {0, 0, 0, 0};
    if (clip)
    {
      if (!colr_get_clip (colr, glyph, &e, c.coords, c.ncoords))
      {
	clip = 0;
	is_bounded = 0;
      }
      if (!is_bounded)
      {
	paint_bounded_t pb;
	memset (&pb, 0, sizeof (pb));
	pb.bounded = 1;
	colr_paint_glyph (font, glyph, &paint_bounded_funcs, &pb, palette, foreground, 0);
	is_bounded = pb.bounded;
	free (pb.groups.a);
      }
    }
    pf_push_font_transform (f, d, font);
    if (clip)
      pf_push_clip_rectangle (f, d, (float) e.x_bearing, (float) e.y_bearing + e.height,
			      (float) e.x_bearing + e.width, (float) e.y_bearing);
    if (is_bounded) paint_recurse (&c, paint);
    if (clip) pf_pop_clip (f, d);
    pf_pop_transform (f, d);
    ret = 1;
  }
  else
  {
    const uint8_t *bg = colr->base_glyphs;
    int min = 0, max = (int) colr->num_base - 1;
    const uint8_t *rec = NULL;
    while (min <= max)
    {
      int mid = ((unsigned) min + (unsigned) max) / 2;
      const uint8_t *r = bg + mid * 6;
      if (glyph < rd16 (r)) max = mid - 1;
      else if (glyph > rd16 (r)) min = mid + 1;
      else { rec = r; break; }
    }
    if (rec)
    {
      unsigned first = rd16 (rec + 2), n = rd16 (rec + 4);
      if (first > colr->num_layers) n = 0;
      else n = HB_MIN (n, colr->num_layers - first);
      for (unsigned i = 0; i < n; i++)
      {
	const uint8_t *l = colr->layers + (first + i) * 4;
	int fg;
	hb_color_t color = paint_get_color (&c, rd16 (l + 2), 1.f, &fg);
	pf_fill_glyph (f, d, rd16 (l), c.font, fg, color);
      }
      ret = 1;
    }
  }
  dnode_fini (&node);
  hb_font_destroy (c.font);
  return ret;
}

enum { BOUNDS_UNBOUNDED, BOUNDS_BOUNDED, BOUNDS_EMPTY };
typedef struct { int status; extents_t e; } pbounds_t;

static pbounds_t pbounds_from_extents (const extents_t *e)
{
  pbounds_t b;
  b.status = extents_empty (e) ? BOUNDS_EMPTY : BOUNDS_BOUNDED;
  b.e = *e;
  return b;
}
static pbounds_t pbounds_status (int s) { pbounds_t b; b.status = s; extents_init (&b.e); return b; }

static void pbounds_union (pbounds_t *b, const pbounds_t *o)
{
  if (o->status == BOUNDS_UNBOUNDED) b->status = BOUNDS_UNBOUNDED;
  else if (o->status == BOUNDS_BOUNDED)
  {
    if (b->status == BOUNDS_EMPTY) *b = *o;
    else if (b->status == BOUNDS_BOUNDED) extents_union (&b->e, &o->e);
  }
}

static void pbounds_intersect (pbounds_t *b, const pbounds_t *o)
{
  if (o->status == BOUNDS_EMPTY) b->status = BOUNDS_EMPTY;
  else if (o->status == BOUNDS_BOUNDED)
  {
    if (b->status == BOUNDS_UNBOUNDED) *b = *o;
    else if (b->status == BOUNDS_BOUNDED)
    {
      extents_intersect (&b->e, &o->e);
      if (extents_empty (&b->e)) b->status = BOUNDS_EMPTY;
    }
  }
}

DEFINE_VEC (xvec_t, xform_t)
DEFINE_VEC (pbvec_t, pbounds_t)

typedef struct
{
  xvec_t transforms;
  pbvec_t clips, groups;
  int64_t budget_remaining;
} paint_extents_t;

static void pe_clear (paint_extents_t *c)
{
  c->transforms.len = c->clips.len = c->groups.len = 0;
  c->budget_remaining = BUDGET_GLYPH;
  xvec_t_push (&c->transforms, xform_identity);
  pbvec_t_push (&c->clips, pbounds_status (BOUNDS_UNBOUNDED));
  pbvec_t_push (&c->groups, pbounds_status (BOUNDS_EMPTY));
}

static void pe_free (paint_extents_t *c)
{
  free (c->transforms.a);
  free (c->clips.a);
  free (c->groups.a);
}

static void pe_push_transform (void *d, float xx, float yx, float xy, float yy, float dx, float dy)
{
  paint_extents_t *c = (paint_extents_t *) d;
  xform_t t = c->transforms.a[c->transforms.len - 1], o = {xx, yx, xy, yy, dx, dy};
  xform_multiply (&t, &o, 0);
  xvec_t_push (&c->transforms, t);
}
static void pe_pop_transform (void *d)
{
  paint_extents_t *c = (paint_extents_t *) d;
  if (c->transforms.len > 1) c->transforms.len--;
}
static void pe_push_clip (paint_extents_t *c, extents_t e)
{
  xform_extents (&c->transforms.a[c->transforms.len - 1], &e);
  pbounds_t b = pbounds_from_extents (&e);
  pbounds_intersect (&b, &c->clips.a[c->clips.len - 1]);
  pbvec_t_push (&c->clips, b);
}

typedef struct { extents_t e; int64_t *budget; } pe_sink_t;

static void pes_point (void *d, dstate_t *st, float x, float y)
{
  pe_sink_t *s = (pe_sink_t *) d;
  if (!budget_spend (s->budget, 1, 1)) return;
  extents_add_point (&s->e, x, y);
}
static void pes_quadratic (void *d, dstate_t *st, float cx, float cy, float x, float y)
{
  pe_sink_t *s = (pe_sink_t *) d;
  if (!budget_spend (s->budget, 1, 1)) return;
  extents_add_point (&s->e, cx, cy);
  extents_add_point (&s->e, x, y);
}
static void pes_cubic (void *d, dstate_t *st, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
  pe_sink_t *s = (pe_sink_t *) d;
  if (!budget_spend (s->budget, 1, 1)) return;
  extents_add_point (&s->e, c1x, c1y);
  extents_add_point (&s->e, c2x, c2y);
  extents_add_point (&s->e, x, y);
}
static int64_t *pes_budget (void *d) { return ((pe_sink_t *) d)->budget; }

static const draw_funcs_t pe_sink_funcs = { pes_point, pes_point, pes_quadratic, pes_cubic, NULL, pes_budget };

static void pe_push_clip_glyph (void *d, hb_codepoint_t glyph, hb_font_t *font)
{
  paint_extents_t *c = (paint_extents_t *) d;
  pe_sink_t s;
  extents_init (&s.e);
  s.budget = &c->budget_remaining;
  if (c->budget_remaining >= 0) font_draw_glyph (font, glyph, &pe_sink_funcs, &s);
  pe_push_clip (c, s.e);
}
static void pe_push_clip_rectangle (void *d, float xmin, float ymin, float xmax, float ymax)
{
  extents_t e = {xmin, ymin, xmax, ymax};
  pe_push_clip ((paint_extents_t *) d, e);
}
static void pe_pop_clip (void *d)
{
  paint_extents_t *c = (paint_extents_t *) d;
  if (c->clips.len > 1) c->clips.len--;
}
static void pe_push_group (void *d)
{
  pbvec_t_push (&((paint_extents_t *) d)->groups, pbounds_status (BOUNDS_EMPTY));
}
static void pe_pop_group (void *d, int mode)
{
  paint_extents_t *c = (paint_extents_t *) d;
  if (c->groups.len < 2) return;
  pbounds_t src = c->groups.a[--c->groups.len];
  pbounds_t *b = &c->groups.a[c->groups.len - 1];
  switch (mode)
  {
  case MODE_CLEAR: b->status = BOUNDS_EMPTY; break;
  case MODE_SRC: case MODE_SRC_OUT: *b = src; break;
  case MODE_DEST: case MODE_DEST_OUT: break;
  case MODE_SRC_IN: case MODE_DEST_IN: pbounds_intersect (b, &src); break;
  default: pbounds_union (b, &src); break;
  }
}
static void pe_paint (void *d)
{
  paint_extents_t *c = (paint_extents_t *) d;
  pbounds_union (&c->groups.a[c->groups.len - 1], &c->clips.a[c->clips.len - 1]);
}
static void pe_color (void *d, int fg, hb_color_t col) { pe_paint (d); }
static void pe_linear (void *d, color_line_t *cl, float a, float b, float c, float e, float g, float h) { pe_paint (d); }
static void pe_radial (void *d, color_line_t *cl, float a, float b, float c, float e, float g, float h) { pe_paint (d); }
static void pe_sweep (void *d, color_line_t *cl, float a, float b, float c, float e) { pe_paint (d); }
static int pe_image (void *d, const uint8_t *png, unsigned len, unsigned w, unsigned h, hb_glyph_extents_t *g)
{
  extents_t e = {(float) g->x_bearing, (float) g->y_bearing + g->height, (float) g->x_bearing + g->width, (float) g->y_bearing};
  pe_push_clip ((paint_extents_t *) d, e);
  pe_paint (d);
  pe_pop_clip (d);
  return 1;
}

static const paint_funcs_t paint_extents_funcs = {
  pe_push_transform, pe_pop_transform, NULL, pe_push_clip_glyph, pe_push_clip_rectangle, pe_pop_clip, pe_color,
  pe_linear, pe_radial, pe_sweep, pe_push_group, NULL, pe_pop_group, NULL, pe_image
};

static int colr_get_extents (hb_font_t *font, hb_codepoint_t glyph, hb_glyph_extents_t *extents)
{
  colr_t *colr = face_colr (font->face);
  if (!colr_has_data (colr)) return 0;
  if (colr_get_clip (colr, glyph, extents, font->coords, font->num_coords))
  {
    scale_glyph_extents (font, extents);
    return 1;
  }
  paint_extents_t pe;
  memset (&pe, 0, sizeof (pe));
  pe_clear (&pe);
  int ret = colr_paint_glyph (font, glyph, &paint_extents_funcs, &pe, 0, HB_COLOR (0, 0, 0, 0), 1);
  extents_t e = pe.groups.a[pe.groups.len - 1].e;
  if (e.xmin > e.xmax) extents->x_bearing = extents->y_bearing = extents->width = extents->height = 0;
  else *extents = extents_to_glyph (&e, 0, 0);
  pe_free (&pe);
  return ret;
}

static int bitmap_paint_glyph (hb_font_t *font, hb_codepoint_t glyph, const paint_funcs_t *f, void *d)
{
  hb_glyph_extents_t extents, pixel;
  const uint8_t *png;
  unsigned len, ppem;
  int x_off, y_off;
  if (cbdt_has_data (font->face))
  {
    memset (&extents, 0, sizeof (extents));
    if (ot_glyph_extents (font, glyph, &extents) && cbdt_get_extents (font, glyph, &pixel, 0) &&
	cbdt_png (font, glyph, &png, &len) &&
	pf_image (f, d, png, len, (unsigned) pixel.width, (unsigned) -pixel.height, &extents))
      return 1;
  }
  if (sbix_has_data (font->face))
  {
    memset (&extents, 0, sizeof (extents));
    if (ot_glyph_extents (font, glyph, &extents) && sbix_get_extents (font, glyph, &pixel, 0) &&
	sbix_png (font, glyph, &png, &len, &x_off, &y_off, &ppem))
      return pf_image (f, d, png, len, (unsigned) pixel.width, (unsigned) -pixel.height, &extents);
  }
  return 0;
}

static int font_paint_glyph (hb_font_t *font, hb_codepoint_t glyph, const paint_funcs_t *f, void *d,
			     unsigned palette, hb_color_t foreground)
{
  if (font->slant_xy) pf_push_transform (f, d, 1.f, 0.f, font->slant_xy, 1.f, 0.f, 0.f);
  int ret = colr_paint_glyph (font, glyph, f, d, palette, foreground, 1) || bitmap_paint_glyph (font, glyph, f, d);
  if (font->slant_xy) pf_pop_transform (f, d);
  return ret;
}

static int font_paint_extents (hb_font_t *font, hb_codepoint_t glyph, hb_glyph_extents_t *extents)
{
  paint_extents_t pe;
  memset (&pe, 0, sizeof (pe));
  pe_clear (&pe);
  int ret = font_paint_glyph (font, glyph, &paint_extents_funcs, &pe, 0, 0);
  if (ret) *extents = extents_to_glyph (&pe.groups.a[pe.groups.len - 1].e, 0, 0);
  pe_free (&pe);
  return ret;
}

#define RASTER_PIXEL_BITS 8
#define RASTER_ONE_PIXEL (1 << RASTER_PIXEL_BITS)
#define RASTER_PIXEL_MASK (RASTER_ONE_PIXEL - 1)
#define RASTER_FULL_COVERAGE (2 * RASTER_ONE_PIXEL * RASTER_ONE_PIXEL)
#define RASTER_MAX_BUFFER_SIZE ((size_t) 1 << 30)
#define RASTER_MAX_AUTO_DIMENSION 4096
#define RASTER_MAX_DRAW_EDGES ((int64_t) 1 << 20)
#define BUDGET_RASTER_PIXELS ((int64_t) 1 << 26)
#define BUDGET_RASTER_PAINT_PASSES 4
#define BUDGET_DEFAULT ((int64_t) (-0x7FFFFFFFFFFFFFFF - 1))
#define BUDGET_UNLIMITED ((int64_t) 0x7FFFFFFFFFFFFFFF)
#define GRADIENT_LUT_SIZE 256
#define GRADIENT_LUT_MIN_PIXELS (64u * 64u)

enum { RASTER_FORMAT_A8, RASTER_FORMAT_BGRA32 };

static inline uint8_t div255 (unsigned a) { return (a + 255) >> 8; }

static inline uint32_t pack_pixel (uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
  return (uint32_t) b | ((uint32_t) g << 8) | ((uint32_t) r << 16) | ((uint32_t) a << 24);
}

static inline uint32_t src_over (uint32_t src, uint32_t dst)
{
  uint8_t sa = (uint8_t) (src >> 24);
  if (sa == 255) return src;
  if (sa == 0) return dst;
  unsigned inv = 255 - sa;
  return pack_pixel (div255 ((dst & 0xFF) * inv) + (uint8_t) (src & 0xFF),
		     div255 (((dst >> 8) & 0xFF) * inv) + (uint8_t) ((src >> 8) & 0xFF),
		     div255 (((dst >> 16) & 0xFF) * inv) + (uint8_t) ((src >> 16) & 0xFF),
		     div255 (((dst >> 24) & 0xFF) * inv) + sa);
}

static inline uint32_t alpha_mul (uint32_t px, unsigned a)
{
  if (a == 255) return px;
  if (a == 0) return 0;
  return pack_pixel (div255 ((px & 0xFF) * a), div255 (((px >> 8) & 0xFF) * a),
		     div255 (((px >> 16) & 0xFF) * a), div255 (((px >> 24) & 0xFF) * a));
}

DEFINE_VEC (bvec_t, uint8_t)

struct hb_raster_image_t
{
  bvec_t buffer;
  hb_raster_extents_t extents;
  int format;
};

static hb_raster_image_t *image_create (void) { return (hb_raster_image_t *) calloc (1, sizeof (hb_raster_image_t)); }

static void image_destroy (hb_raster_image_t *img)
{
  if (!img) return;
  free (img->buffer.a);
  free (img);
}

static int image_configure (hb_raster_image_t *img, int format, hb_raster_extents_t extents)
{
  unsigned bpp = format == RASTER_FORMAT_BGRA32 ? 4u : 1u;
  if (extents.width > UINT_MAX / bpp) return 0;
  unsigned min_stride = extents.width * bpp;
  if (extents.stride == 0 || extents.stride < min_stride) extents.stride = min_stride;
  if (extents.height && extents.stride > (size_t) -1 / extents.height) return 0;
  size_t size = (size_t) extents.stride * extents.height;
  if (size > RASTER_MAX_BUFFER_SIZE) return 0;
  if (!bvec_t_resize (&img->buffer, (unsigned) size, 0)) return 0;
  img->format = format;
  img->extents = extents;
  return 1;
}

static void image_clear (hb_raster_image_t *img)
{
  if (img->buffer.a) memset (img->buffer.a, 0, (size_t) img->extents.stride * img->extents.height);
}

const uint8_t *hb_raster_image_get_buffer (const hb_raster_image_t *image) { return image->buffer.a; }

void hb_raster_image_get_extents (const hb_raster_image_t *image, hb_raster_extents_t *extents)
{
  if (extents) *extents = image->extents;
}

static inline void unpack_float (uint32_t px, float *r, float *g, float *b, float *a)
{
  *b = (px & 0xFF) / 255.f;
  *g = ((px >> 8) & 0xFF) / 255.f;
  *r = ((px >> 16) & 0xFF) / 255.f;
  *a = (px >> 24) / 255.f;
}

static inline uint32_t pack_float (float r, float g, float b, float a)
{
  return pack_pixel ((uint8_t) (HB_CLAMP (b, 0.f, 1.f) * 255.f + 0.5f), (uint8_t) (HB_CLAMP (g, 0.f, 1.f) * 255.f + 0.5f),
		     (uint8_t) (HB_CLAMP (r, 0.f, 1.f) * 255.f + 0.5f), (uint8_t) (HB_CLAMP (a, 0.f, 1.f) * 255.f + 0.5f));
}

static float blend (int mode, float sc, float dc)
{
  switch (mode)
  {
  case MODE_MULTIPLY: return sc * dc;
  case MODE_SCREEN: return sc + dc - sc * dc;
  case MODE_OVERLAY: return dc <= 0.5f ? 2.f * sc * dc : 1.f - 2.f * (1.f - sc) * (1.f - dc);
  case MODE_DARKEN: return HB_MIN (sc, dc);
  case MODE_LIGHTEN: return HB_MAX (sc, dc);
  case MODE_COLOR_DODGE:
    if (dc <= 0.f) return 0.f;
    if (sc >= 1.f) return 1.f;
    return HB_MIN (1.f, dc / (1.f - sc));
  case MODE_COLOR_BURN:
    if (dc >= 1.f) return 1.f;
    if (sc <= 0.f) return 0.f;
    return 1.f - HB_MIN (1.f, (1.f - dc) / sc);
  case MODE_HARD_LIGHT: return sc <= 0.5f ? 2.f * sc * dc : 1.f - 2.f * (1.f - sc) * (1.f - dc);
  case MODE_SOFT_LIGHT:
  {
    if (sc <= 0.5f) return dc - (1.f - 2.f * sc) * dc * (1.f - dc);
    float d = (dc <= 0.25f) ? ((16.f * dc - 12.f) * dc + 4.f) * dc : sqrtf (dc);
    return dc + (2.f * sc - 1.f) * (d - dc);
  }
  case MODE_DIFFERENCE: return fabsf (sc - dc);
  default: return sc + dc - 2.f * sc * dc;
  }
}

static inline float hsl_lum (float r, float g, float b) { return 0.299f * r + 0.587f * g + 0.114f * b; }
static inline float hsl_sat (float r, float g, float b) { return HB_MAX (HB_MAX (r, g), b) - HB_MIN (HB_MIN (r, g), b); }

static void hsl_clip_color (float *r, float *g, float *b)
{
  float l = hsl_lum (*r, *g, *b);
  float mn = HB_MIN (HB_MIN (*r, *g), *b), mx = HB_MAX (HB_MAX (*r, *g), *b);
  if (mn < 0.f)
  {
    float d = l - mn;
    if (d > 0.f) { *r = l + (*r - l) * l / d; *g = l + (*g - l) * l / d; *b = l + (*b - l) * l / d; }
  }
  if (mx > 1.f)
  {
    float d = mx - l;
    if (d > 0.f) { *r = l + (*r - l) * (1.f - l) / d; *g = l + (*g - l) * (1.f - l) / d; *b = l + (*b - l) * (1.f - l) / d; }
  }
}

static void hsl_set_lum (float *r, float *g, float *b, float l)
{
  float d = l - hsl_lum (*r, *g, *b);
  *r += d; *g += d; *b += d;
  hsl_clip_color (r, g, b);
}

static void hsl_set_sat_inner (float *mn, float *mid, float *mx, float s)
{
  if (*mx > *mn)
  {
    *mid = (*mid - *mn) * s / (*mx - *mn);
    *mx = s;
  }
  else
    *mid = *mx = 0.f;
  *mn = 0.f;
}

static void hsl_set_sat (float *r, float *g, float *b, float s)
{
  if (*r <= *g)
  {
    if (*g <= *b) hsl_set_sat_inner (r, g, b, s);
    else if (*r <= *b) hsl_set_sat_inner (r, b, g, s);
    else hsl_set_sat_inner (b, r, g, s);
  }
  else
  {
    if (*r <= *b) hsl_set_sat_inner (g, r, b, s);
    else if (*g <= *b) hsl_set_sat_inner (g, b, r, s);
    else hsl_set_sat_inner (b, g, r, s);
  }
}

static uint32_t blend_pixel (uint32_t src, uint32_t dst, int mode)
{
  float sr, sg, sb, sa, dr, dg, db, da;
  unpack_float (src, &sr, &sg, &sb, &sa);
  unpack_float (dst, &dr, &dg, &db, &da);
  float usr = sa > 0.f ? sr / sa : 0.f, usg = sa > 0.f ? sg / sa : 0.f, usb = sa > 0.f ? sb / sa : 0.f;
  float udr = da > 0.f ? dr / da : 0.f, udg = da > 0.f ? dg / da : 0.f, udb = da > 0.f ? db / da : 0.f;
  float br, bg, bb;
  if (mode < MODE_HSL_HUE)
  {
    br = blend (mode, usr, udr);
    bg = blend (mode, usg, udg);
    bb = blend (mode, usb, udb);
  }
  else if (mode == MODE_HSL_HUE)
  {
    br = usr; bg = usg; bb = usb;
    hsl_set_sat (&br, &bg, &bb, hsl_sat (udr, udg, udb));
    hsl_set_lum (&br, &bg, &bb, hsl_lum (udr, udg, udb));
  }
  else if (mode == MODE_HSL_SATURATION)
  {
    br = udr; bg = udg; bb = udb;
    hsl_set_sat (&br, &bg, &bb, hsl_sat (usr, usg, usb));
    hsl_set_lum (&br, &bg, &bb, hsl_lum (udr, udg, udb));
  }
  else if (mode == MODE_HSL_COLOR)
  {
    br = usr; bg = usg; bb = usb;
    hsl_set_lum (&br, &bg, &bb, hsl_lum (udr, udg, udb));
  }
  else
  {
    br = udr; bg = udg; bb = udb;
    hsl_set_lum (&br, &bg, &bb, hsl_lum (usr, usg, usb));
  }
  float ra = sa + da - sa * da;
  float rr = sa * da * br + sa * (1.f - da) * usr + (1.f - sa) * da * udr;
  float rg = sa * da * bg + sa * (1.f - da) * usg + (1.f - sa) * da * udg;
  float rb = sa * da * bb + sa * (1.f - da) * usb + (1.f - sa) * da * udb;
  return pack_float (rr, rg, rb, ra);
}

static inline uint32_t add_sat (uint32_t a, uint32_t b)
{
  return pack_pixel ((uint8_t) HB_MIN (255u, (a & 0xFF) + (b & 0xFF)), (uint8_t) HB_MIN (255u, ((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)),
		     (uint8_t) HB_MIN (255u, ((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)), (uint8_t) HB_MIN (255u, (a >> 24) + (b >> 24)));
}

static uint32_t composite_pixel (uint32_t src, uint32_t dst, int mode)
{
  uint8_t sa = (uint8_t) (src >> 24), da = (uint8_t) (dst >> 24);
  switch (mode)
  {
  case MODE_CLEAR: return 0;
  case MODE_SRC: return src;
  case MODE_DEST: return dst;
  case MODE_SRC_OVER: return src_over (src, dst);
  case MODE_DEST_OVER: return src_over (dst, src);
  case MODE_SRC_IN: return alpha_mul (src, da);
  case MODE_DEST_IN: return alpha_mul (dst, sa);
  case MODE_SRC_OUT: return alpha_mul (src, 255 - da);
  case MODE_DEST_OUT: return alpha_mul (dst, 255 - sa);
  case MODE_SRC_ATOP: return add_sat (alpha_mul (src, da), alpha_mul (dst, 255 - sa));
  case MODE_DEST_ATOP: return add_sat (alpha_mul (dst, sa), alpha_mul (src, 255 - da));
  case MODE_XOR: return add_sat (alpha_mul (src, 255 - da), alpha_mul (dst, 255 - sa));
  case MODE_PLUS: return add_sat (src, dst);
  default:
    if (mode >= MODE_SCREEN && mode <= MODE_HSL_LUMINOSITY) return blend_pixel (src, dst, mode);
    return src_over (src, dst);
  }
}

static void image_composite (hb_raster_image_t *dst, const hb_raster_image_t *src, int mode)
{
  unsigned w = dst->extents.width, h = dst->extents.height, stride = dst->extents.stride;
  for (unsigned y = 0; y < h; y++)
  {
    uint32_t *dp = (uint32_t *) (void *) (dst->buffer.a + y * stride);
    const uint32_t *sp = (const uint32_t *) (const void *) (src->buffer.a + y * stride);
    for (unsigned x = 0; x < w; x++) dp[x] = composite_pixel (sp[x], dp[x], mode);
  }
}

typedef struct { int32_t xL, yL, xH, yH; int64_t slope; int32_t wind; } redge_t;

DEFINE_VEC (evec_t, redge_t)
DEFINE_VEC (i32vec_t, int32_t)
DEFINE_VEC (i16vec_t, int16_t)

typedef struct
{
  xform_t transform;
  float x_scale_factor, y_scale_factor;
  int has_clip_box;
  float clip_x0, clip_y0, clip_x1, clip_y1;
  int flatten_clip_active;
  float fc_x0, fc_y0, fc_x1, fc_y1;
  int64_t budget, budget_remaining, edges_left;
  evec_t edges;
  i32vec_t row_area;
  i16vec_t row_cover;
  uvec_t *buckets;
  unsigned nbuckets;
  uvec_t active;
  hb_raster_image_t *recycled;
} rdraw_t;

static void rdraw_recharge (rdraw_t *d) { d->budget_remaining = d->budget == BUDGET_DEFAULT ? BUDGET_GLYPH : d->budget; }

static void rdraw_clear (rdraw_t *d)
{
  d->has_clip_box = 0;
  d->flatten_clip_active = 0;
  rdraw_recharge (d);
  d->edges_left = RASTER_MAX_DRAW_EDGES;
  d->edges.len = 0;
  d->active.len = 0;
}

static void rdraw_reset (rdraw_t *d)
{
  d->transform = xform_identity;
  d->x_scale_factor = d->y_scale_factor = 1.f;
  d->budget = BUDGET_DEFAULT;
  rdraw_clear (d);
}

static void rdraw_free (rdraw_t *d)
{
  image_destroy (d->recycled);
  free (d->edges.a); free (d->row_area.a); free (d->row_cover.a); free (d->active.a);
  for (unsigned i = 0; i < d->nbuckets; i++) free (d->buckets[i].a);
  free (d->buckets);
}

static void rdraw_set_clip_box (rdraw_t *d, float x0, float y0, float x1, float y1)
{
  d->has_clip_box = 1;
  d->clip_x0 = x0; d->clip_y0 = y0; d->clip_x1 = x1; d->clip_y1 = y1;
  d->flatten_clip_active = 1;
  d->fc_x0 = x0 - 1.f; d->fc_y0 = y0 - 1.f;
  d->fc_x1 = x1 + 1.f; d->fc_y1 = y1 + 1.f;
}

static int64_t rdraw_pixel_work (const rdraw_t *d, unsigned max_rows, unsigned max_cols)
{
  int64_t work = 0;
  for (unsigned i = 0; i < d->edges.len; i++)
  {
    const redge_t *e = &d->edges.a[i];
    int64_t dx = (int64_t) e->xH - e->xL;
    if (dx < 0) dx = -dx;
    work += 1 + HB_MIN (((int64_t) e->yH - e->yL) >> RASTER_PIXEL_BITS, (int64_t) max_rows)
	      + HB_MIN (dx >> RASTER_PIXEL_BITS, (int64_t) max_cols);
  }
  return work;
}

static inline void rdraw_point (const rdraw_t *d, float x, float y, float *tx, float *ty)
{
  *tx = x; *ty = y;
  xform_point (&d->transform, tx, ty);
  *tx /= d->x_scale_factor;
  *ty /= d->y_scale_factor;
}

static inline int32_t clamp_i32 (double v) { return (int32_t) HB_CLAMP (v, (double) INT32_MIN, (double) INT32_MAX); }

static void emit_segment (rdraw_t *d, float x0, float y0, float x1, float y1)
{
  if (d->edges_left <= 0) return;
  int32_t X0 = clamp_i32 (hbroundf (x0 * RASTER_ONE_PIXEL)), Y0 = clamp_i32 (hbroundf (y0 * RASTER_ONE_PIXEL));
  int32_t X1 = clamp_i32 (hbroundf (x1 * RASTER_ONE_PIXEL)), Y1 = clamp_i32 (hbroundf (y1 * RASTER_ONE_PIXEL));
  if (Y0 == Y1) return;
  redge_t e;
  if (Y0 < Y1) { e.xL = X0; e.yL = Y0; e.xH = X1; e.yH = Y1; e.wind = +1; }
  else { e.xL = X1; e.yL = Y1; e.xH = X0; e.yH = Y0; e.wind = -1; }
  e.slope = (((int64_t) e.xH - (int64_t) e.xL) * (int64_t) 65536) / ((int64_t) e.yH - (int64_t) e.yL);
  if (evec_t_push (&d->edges, e)) d->edges_left--;
  else d->edges_left = 0;
}

static int flatten_clip_test (const rdraw_t *d, float bx0, float bx1, float by0, float by1, int *check_clip)
{
  if (bx1 < d->fc_x0 || bx0 > d->fc_x1 || by1 < d->fc_y0 || by0 > d->fc_y1) return 1;
  if (bx0 >= d->fc_x0 && bx1 <= d->fc_x1 && by0 >= d->fc_y0 && by1 <= d->fc_y1) *check_clip = 0;
  return 0;
}

static void flatten_quadratic (rdraw_t *d, float x0, float y0, float x1, float y1, float x2, float y2)
{
  struct { float x0, y0, x1, y1, x2, y2; int depth, check_clip; } stack[16];
  unsigned top = 0;
  int depth = 0, check_clip = d->flatten_clip_active;
  int64_t work_left = d->budget_remaining;
  for (;;)
  {
    int emit = depth >= 16;
    if (!emit && check_clip)
      emit = flatten_clip_test (d, HB_MIN (x0, HB_MIN (x1, x2)), HB_MAX (x0, HB_MAX (x1, x2)),
				HB_MIN (y0, HB_MIN (y1, y2)), HB_MAX (y0, HB_MAX (y1, y2)), &check_clip);
    if (!emit)
    {
      float dx = x0 + x2 - 2.f * x1, dy = y0 + y2 - 2.f * y1;
      if (dx < 0) dx = -dx;
      if (dy < 0) dy = -dy;
      emit = dx <= 0.25f && dy <= 0.25f;
    }
    if (!emit)
    {
      if (work_left <= 0) emit = 1;
      else work_left--;
    }
    if (emit)
    {
      emit_segment (d, x0, y0, x2, y2);
      if (!top) { d->budget_remaining = work_left; return; }
      top--;
      x0 = stack[top].x0; y0 = stack[top].y0; x1 = stack[top].x1; y1 = stack[top].y1;
      x2 = stack[top].x2; y2 = stack[top].y2; depth = stack[top].depth; check_clip = stack[top].check_clip;
      continue;
    }
    float x01 = (x0 + x1) * 0.5f, y01 = (y0 + y1) * 0.5f;
    float x12 = (x1 + x2) * 0.5f, y12 = (y1 + y2) * 0.5f;
    float xm = (x01 + x12) * 0.5f, ym = (y01 + y12) * 0.5f;
    stack[top].x0 = xm; stack[top].y0 = ym; stack[top].x1 = x12; stack[top].y1 = y12;
    stack[top].x2 = x2; stack[top].y2 = y2; stack[top].depth = depth + 1; stack[top].check_clip = check_clip;
    top++;
    x2 = xm; y2 = ym; x1 = x01; y1 = y01;
    depth++;
  }
}

static void flatten_cubic (rdraw_t *d, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3)
{
  struct { float x0, y0, x1, y1, x2, y2, x3, y3; int depth, check_clip; } stack[16];
  unsigned top = 0;
  int depth = 0, check_clip = d->flatten_clip_active;
  int64_t work_left = d->budget_remaining;
  for (;;)
  {
    int emit = depth >= 16;
    if (!emit && check_clip)
      emit = flatten_clip_test (d, HB_MIN (HB_MIN (x0, x1), HB_MIN (x2, x3)), HB_MAX (HB_MAX (x0, x1), HB_MAX (x2, x3)),
				HB_MIN (HB_MIN (y0, y1), HB_MIN (y2, y3)), HB_MAX (HB_MAX (y0, y1), HB_MAX (y2, y3)), &check_clip);
    if (!emit)
    {
      float d10x = 2.f * x0 - 3.f * x1 + x3, d10y = 2.f * y0 - 3.f * y1 + y3;
      float d20x = x0 - 3.f * x2 + 2.f * x3, d20y = y0 - 3.f * y2 + 2.f * y3;
      if (d10x < 0) d10x = -d10x;
      if (d10y < 0) d10y = -d10y;
      if (d20x < 0) d20x = -d20x;
      if (d20y < 0) d20y = -d20y;
      emit = d10x <= 0.5f && d10y <= 0.5f && d20x <= 0.5f && d20y <= 0.5f;
    }
    if (!emit)
    {
      if (work_left <= 0) emit = 1;
      else work_left--;
    }
    if (emit)
    {
      emit_segment (d, x0, y0, x3, y3);
      if (!top) { d->budget_remaining = work_left; return; }
      top--;
      x0 = stack[top].x0; y0 = stack[top].y0; x1 = stack[top].x1; y1 = stack[top].y1;
      x2 = stack[top].x2; y2 = stack[top].y2; x3 = stack[top].x3; y3 = stack[top].y3;
      depth = stack[top].depth; check_clip = stack[top].check_clip;
      continue;
    }
    float x01 = (x0 + x1) * 0.5f, y01 = (y0 + y1) * 0.5f;
    float x12 = (x1 + x2) * 0.5f, y12 = (y1 + y2) * 0.5f;
    float x23 = (x2 + x3) * 0.5f, y23 = (y2 + y3) * 0.5f;
    float x012 = (x01 + x12) * 0.5f, y012 = (y01 + y12) * 0.5f;
    float x123 = (x12 + x23) * 0.5f, y123 = (y12 + y23) * 0.5f;
    float xm = (x012 + x123) * 0.5f, ym = (y012 + y123) * 0.5f;
    stack[top].x0 = xm; stack[top].y0 = ym; stack[top].x1 = x123; stack[top].y1 = y123;
    stack[top].x2 = x23; stack[top].y2 = y23; stack[top].x3 = x3; stack[top].y3 = y3;
    stack[top].depth = depth + 1; stack[top].check_clip = check_clip;
    top++;
    x3 = xm; y3 = ym; x2 = x012; y2 = y012; x1 = x01; y1 = y01;
    depth++;
  }
}

static void rd_line_to (void *dd, dstate_t *st, float x, float y)
{
  rdraw_t *d = (rdraw_t *) dd;
  if (d->edges_left <= 0) return;
  float tx0, ty0, tx1, ty1;
  rdraw_point (d, st->cx, st->cy, &tx0, &ty0);
  rdraw_point (d, x, y, &tx1, &ty1);
  emit_segment (d, tx0, ty0, tx1, ty1);
}

static void rd_quadratic_to (void *dd, dstate_t *st, float cx, float cy, float x, float y)
{
  rdraw_t *d = (rdraw_t *) dd;
  if (d->edges_left <= 0) return;
  float tx0, ty0, tx1, ty1, tx2, ty2;
  rdraw_point (d, st->cx, st->cy, &tx0, &ty0);
  rdraw_point (d, cx, cy, &tx1, &ty1);
  rdraw_point (d, x, y, &tx2, &ty2);
  flatten_quadratic (d, tx0, ty0, tx1, ty1, tx2, ty2);
}

static void rd_cubic_to (void *dd, dstate_t *st, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
  rdraw_t *d = (rdraw_t *) dd;
  if (d->edges_left <= 0) return;
  float tx0, ty0, tx1, ty1, tx2, ty2, tx3, ty3;
  rdraw_point (d, st->cx, st->cy, &tx0, &ty0);
  rdraw_point (d, c1x, c1y, &tx1, &ty1);
  rdraw_point (d, c2x, c2y, &tx2, &ty2);
  rdraw_point (d, x, y, &tx3, &ty3);
  flatten_cubic (d, tx0, ty0, tx1, ty1, tx2, ty2, tx3, ty3);
}

static int64_t *rd_budget (void *dd) { return &((rdraw_t *) dd)->budget_remaining; }

static const draw_funcs_t rdraw_funcs = { NULL, rd_line_to, rd_quadratic_to, rd_cubic_to, NULL, rd_budget };

static inline void cell_add (int32_t *area, int16_t *cover, unsigned width, int col,
			     int32_t fx0, int32_t fy0, int32_t fx1, int32_t fy1, int32_t wind,
			     unsigned *x_min, unsigned *x_max)
{
  int32_t dy = fy1 - fy0;
  if ((unsigned) col >= width)
  {
    if (col < 0)
    {
      cover[0] += (int16_t) (dy * wind);
      *x_min = 0;
      *x_max = HB_MAX (*x_max, 0u);
    }
    return;
  }
  area[col] += (fx0 + fx1) * dy * wind;
  cover[col] += (int16_t) (dy * wind);
  *x_min = HB_MIN (*x_min, (unsigned) col);
  *x_max = HB_MAX (*x_max, (unsigned) col);
}

static inline int32_t clamp64_32 (int64_t v) { return (int32_t) HB_CLAMP (v, (int64_t) INT32_MIN, (int64_t) INT32_MAX); }

static void edge_sweep_row (int32_t *area, int16_t *cover, unsigned width, int x_org, int32_t y_top,
			    const redge_t *edge, unsigned *x_min, unsigned *x_max)
{
  int32_t y_bot = (int32_t) HB_MIN ((int64_t) y_top + RASTER_ONE_PIXEL, (int64_t) INT32_MAX);
  int32_t ey0 = HB_MAX (edge->yL, y_top), ey1 = HB_MIN (edge->yH, y_bot);
  if (ey0 >= ey1) return;
  int32_t x0 = clamp64_32 ((int64_t) edge->xL + ((((int64_t) ey0 - (int64_t) edge->yL) * edge->slope) >> 16));
  int32_t x1 = clamp64_32 ((int64_t) edge->xL + ((((int64_t) ey1 - (int64_t) edge->yL) * edge->slope) >> 16));
  int32_t fy0 = ey0 - y_top, fy1 = ey1 - y_top;
  int32_t cx0 = x0 >> RASTER_PIXEL_BITS, fx0 = x0 & RASTER_PIXEL_MASK;
  int32_t cx1 = x1 >> RASTER_PIXEL_BITS, fx1 = x1 & RASTER_PIXEL_MASK;
  int32_t wind = edge->wind;
  if (cx0 == cx1)
  {
    cell_add (area, cover, width, cx0 - x_org, fx0, fy0, fx1, fy1, wind, x_min, x_max);
    return;
  }
  int64_t total_dx = (int64_t) x1 - (int64_t) x0, total_dy = (int64_t) fy1 - (int64_t) fy0;
  int32_t delta_fy = (int32_t) ((int64_t) RASTER_ONE_PIXEL * total_dy / total_dx);
  int64_t col_min = (int64_t) x_org, col_max = (int64_t) x_org + (int64_t) width - 1;
  int inside = (unsigned) (cx0 - x_org) < width && (unsigned) (cx1 - x_org) < width;
  if (total_dx > 0)
  {
    if (!inside)
    {
      if (cx1 < col_min) { cell_add (area, cover, width, -1, 0, fy0, 0, fy1, wind, x_min, x_max); return; }
      if (cx0 > col_max) return;
    }
    int32_t x_b = clamp64_32 (((int64_t) cx0 + 1) * RASTER_ONE_PIXEL);
    int32_t fy_b = fy0 + (int32_t) ((((int64_t) x_b - (int64_t) x0) * total_dy) / total_dx);
    int32_t cx = cx0 + 1, fy_prev = fy_b;
    if (inside || cx0 >= col_min)
      cell_add (area, cover, width, cx0 - x_org, fx0, fy0, RASTER_ONE_PIXEL, fy_b, wind, x_min, x_max);
    else
    {
      int32_t cx_skip = (int32_t) col_min;
      fy_prev = fy_b + (int32_t) ((int64_t) (cx_skip - cx) * delta_fy);
      cell_add (area, cover, width, -1, 0, fy0, 0, fy_prev, wind, x_min, x_max);
      cx = cx_skip;
    }
    int32_t cx_end = inside ? cx1 : (int32_t) HB_MIN ((int64_t) cx1, col_max + 1);
    for (; cx < cx_end; cx++)
    {
      fy_b = fy_prev + delta_fy;
      cell_add (area, cover, width, cx - x_org, 0, fy_prev, RASTER_ONE_PIXEL, fy_b, wind, x_min, x_max);
      fy_prev = fy_b;
    }
    if (inside || cx1 <= col_max)
      cell_add (area, cover, width, cx1 - x_org, 0, fy_prev, fx1, fy1, wind, x_min, x_max);
  }
  else
  {
    if (!inside)
    {
      if (cx0 < col_min) { cell_add (area, cover, width, -1, 0, fy0, 0, fy1, wind, x_min, x_max); return; }
      if (cx1 > col_max) return;
    }
    int32_t x_b = clamp64_32 ((int64_t) cx0 * RASTER_ONE_PIXEL);
    int32_t fy_b = fy0 + (int32_t) ((((int64_t) x_b - (int64_t) x0) * total_dy) / total_dx);
    cell_add (area, cover, width, cx0 - x_org, fx0, fy0, 0, fy_b, wind, x_min, x_max);
    int32_t cx = cx0 - 1, fy_prev = fy_b;
    if (!inside && cx > col_max)
    {
      int32_t cx_skip = (int32_t) col_max;
      fy_prev = fy_b - (int32_t) ((int64_t) (cx - cx_skip) * delta_fy);
      cx = cx_skip;
    }
    int32_t cx_stop = inside ? cx1 : (int32_t) HB_MAX ((int64_t) cx1, col_min - 1);
    for (; cx > cx_stop; cx--)
    {
      fy_b = fy_prev - delta_fy;
      cell_add (area, cover, width, cx - x_org, RASTER_ONE_PIXEL, fy_prev, 0, fy_b, wind, x_min, x_max);
      fy_prev = fy_b;
    }
    if (inside || cx1 >= col_min)
      cell_add (area, cover, width, cx1 - x_org, RASTER_ONE_PIXEL, fy_prev, fx1, fy1, wind, x_min, x_max);
    else
      cell_add (area, cover, width, -1, 0, fy_prev, 0, fy1, wind, x_min, x_max);
  }
}

static int32_t sweep_row_to_alpha (uint8_t *row, int32_t *area, int16_t *cover, unsigned x_min, unsigned x_max)
{
  int32_t accum = 0;
  for (unsigned x = x_min; x <= x_max; x++)
  {
    accum += cover[x];
    int32_t val = accum * (2 * RASTER_ONE_PIXEL) - area[x];
    int32_t a = val < 0 ? -val : val;
    if (a > RASTER_FULL_COVERAGE) a = RASTER_FULL_COVERAGE;
    row[x] = (uint8_t) (((unsigned) a * 255 + RASTER_FULL_COVERAGE / 2) >> (2 * RASTER_PIXEL_BITS + 1));
    area[x] = 0;
    cover[x] = 0;
  }
  return accum;
}

static hb_raster_image_t *rdraw_render (rdraw_t *d)
{
  hb_raster_extents_t ext = {0, 0, 0, 0, 0};
  if (d->edges.len)
  {
    int32_t xmin = d->edges.a[0].xL, xmax = d->edges.a[0].xL, ymin = d->edges.a[0].yL, ymax = d->edges.a[0].yH;
    for (unsigned i = 0; i < d->edges.len; i++)
    {
      const redge_t *e = &d->edges.a[i];
      xmin = HB_MIN (xmin, HB_MIN (e->xL, e->xH));
      xmax = HB_MAX (xmax, HB_MAX (e->xL, e->xH));
      ymin = HB_MIN (ymin, e->yL);
      ymax = HB_MAX (ymax, e->yH);
    }
    int x0 = xmin >> RASTER_PIXEL_BITS, y0 = ymin >> RASTER_PIXEL_BITS;
    int x1 = (int) (((int64_t) xmax + RASTER_PIXEL_MASK) >> RASTER_PIXEL_BITS);
    int y1 = (int) (((int64_t) ymax + RASTER_PIXEL_MASK) >> RASTER_PIXEL_BITS);
    ext.x_origin = x0;
    ext.y_origin = y0;
    ext.width = (unsigned) HB_MIN (HB_MAX (0, x1 - x0), RASTER_MAX_AUTO_DIMENSION);
    ext.height = (unsigned) HB_MIN (HB_MAX (0, y1 - y0), RASTER_MAX_AUTO_DIMENSION);
  }
  ext.stride = (ext.width + 3u) & ~3u;
  hb_raster_image_t *image = d->recycled;
  d->recycled = NULL;
  if (!image) image = image_create ();
  if (!image) { rdraw_clear (d); return NULL; }
  if (!image_configure (image, RASTER_FORMAT_A8, ext)) { image_destroy (image); rdraw_clear (d); return NULL; }
  image_clear (image);
  if (d->edges.len && ext.width && ext.height)
  {
    if (!i32vec_t_resize (&d->row_area, ext.width, 0) || !i16vec_t_resize (&d->row_cover, ext.width, 0))
    { image_destroy (image); rdraw_clear (d); return NULL; }
    memset (d->row_area.a, 0, ext.width * sizeof (int32_t));
    memset (d->row_cover.a, 0, ext.width * sizeof (int16_t));
    if (ext.height > d->nbuckets)
    {
      uvec_t *b = (uvec_t *) realloc (d->buckets, ext.height * sizeof (uvec_t));
      if (!b) { image_destroy (image); rdraw_clear (d); return NULL; }
      memset (b + d->nbuckets, 0, (ext.height - d->nbuckets) * sizeof (uvec_t));
      d->buckets = b;
      d->nbuckets = ext.height;
    }
    for (unsigned i = 0; i < ext.height; i++) d->buckets[i].len = 0;
    for (unsigned i = 0; i < d->edges.len; i++)
    {
      int row = (d->edges.a[i].yL >> RASTER_PIXEL_BITS) - ext.y_origin;
      if (row < 0) row = 0;
      if ((unsigned) row >= ext.height) continue;
      uvec_t_push (&d->buckets[row], i);
    }
    d->active.len = 0;
    for (unsigned row = 0; row < ext.height; row++)
    {
      int32_t y_top = clamp64_32 (((int64_t) ext.y_origin + (int64_t) row) * RASTER_ONE_PIXEL);
      uvec_t *bk = &d->buckets[row];
      if (bk->len && uvec_t_reserve (&d->active, d->active.len + bk->len))
      {
	memcpy (d->active.a + d->active.len, bk->a, bk->len * sizeof (unsigned));
	d->active.len += bk->len;
      }
      unsigned x_min = ext.width, x_max = 0, write = 0;
      for (unsigned j = 0; j < d->active.len; j++)
      {
	unsigned idx = d->active.a[j];
	const redge_t *e = &d->edges.a[idx];
	if (e->yH <= y_top) continue;
	edge_sweep_row (d->row_area.a, d->row_cover.a, ext.width, ext.x_origin, y_top, e, &x_min, &x_max);
	d->active.a[write++] = idx;
      }
      d->active.len = write;
      if (x_min <= x_max)
      {
	uint8_t *row_buf = image->buffer.a + row * ext.stride;
	int32_t accum = sweep_row_to_alpha (row_buf, d->row_area.a, d->row_cover.a, x_min, x_max);
	if (accum)
	{
	  int32_t a = accum * (2 * RASTER_ONE_PIXEL);
	  a = a < 0 ? -a : a;
	  if (a > RASTER_FULL_COVERAGE) a = RASTER_FULL_COVERAGE;
	  memset (row_buf + x_max + 1, (uint8_t) (((unsigned) a * 255 + RASTER_FULL_COVERAGE / 2) >> (2 * RASTER_PIXEL_BITS + 1)), ext.width - 1 - x_max);
	}
      }
    }
  }
  rdraw_clear (d);
  return image;
}

static void rdraw_recycle (rdraw_t *d, hb_raster_image_t *img)
{
  image_destroy (d->recycled);
  d->recycled = img;
}

typedef struct
{
  bvec_t alpha;
  unsigned width, height, stride;
  int is_rect;
  int rect_x0, rect_y0, rect_x1, rect_y1;
  unsigned min_x, min_y, max_x, max_y;
} rclip_t;

DEFINE_VEC (clipvec_t, rclip_t)
DEFINE_VEC (imgvec_t, hb_raster_image_t *)
DEFINE_VEC (stopvec_t, color_stop_t)

struct hb_raster_paint_t
{
  xform_t base_transform;
  float x_scale_factor, y_scale_factor;
  hb_raster_extents_t fixed_extents;
  int has_extents;
  hb_color_t foreground;
  xvec_t transforms;
  clipvec_t clips, clip_cache;
  imgvec_t surfaces, surface_cache;
  stopvec_t stops;
  rdraw_t rdr;
  int64_t budget, budget_remaining, pixel_remaining;
};

static void clip_init_full (rclip_t *c, unsigned w, unsigned h)
{
  c->width = w; c->height = h;
  c->stride = (w + 3u) & ~3u;
  c->is_rect = 1;
  c->rect_x0 = c->rect_y0 = 0;
  c->rect_x1 = (int) w; c->rect_y1 = (int) h;
  c->min_x = c->min_y = 0;
  c->max_x = w; c->max_y = h;
}

static int clip_valid_mask (const rclip_t *c)
{
  if (c->is_rect) return 1;
  if (!c->alpha.a || c->stride < c->width) return 0;
  if (c->height && c->stride > (size_t) -1 / c->height) return 0;
  return (size_t) c->stride * c->height <= c->alpha.len;
}

static void rp_recharge (hb_raster_paint_t *c)
{
  c->budget_remaining = c->budget == BUDGET_DEFAULT ? BUDGET_GLYPH : c->budget;
  if (c->budget == BUDGET_UNLIMITED) c->pixel_remaining = c->budget;
  else if (c->surfaces.len)
    c->pixel_remaining = HB_MAX ((int64_t) BUDGET_RASTER_PIXELS,
				 (int64_t) BUDGET_RASTER_PAINT_PASSES * c->fixed_extents.width * c->fixed_extents.height);
  else c->pixel_remaining = BUDGET_RASTER_PIXELS;
}

static int rp_charge (hb_raster_paint_t *c, int64_t work)
{
  if (c->pixel_remaining < 0) return 0;
  c->pixel_remaining -= work;
  return 1;
}

static int rp_precharge (hb_raster_paint_t *c, int64_t work)
{
  if (c->pixel_remaining < work) { c->pixel_remaining = -1; return 0; }
  c->pixel_remaining -= work;
  return 1;
}

static hb_raster_image_t *rp_acquire_surface (hb_raster_paint_t *c)
{
  hb_raster_image_t *img = c->surface_cache.len ? c->surface_cache.a[--c->surface_cache.len] : image_create ();
  if (!img) return NULL;
  if (!image_configure (img, RASTER_FORMAT_BGRA32, c->fixed_extents)) { image_destroy (img); return NULL; }
  image_clear (img);
  return img;
}

static void rp_release_surface (hb_raster_paint_t *c, hb_raster_image_t *img)
{
  if (!imgvec_t_push (&c->surface_cache, img)) image_destroy (img);
}

static rclip_t rp_acquire_clip (hb_raster_paint_t *c, unsigned w, unsigned h)
{
  rclip_t clip;
  memset (&clip, 0, sizeof (clip));
  if (c->clip_cache.len) clip = c->clip_cache.a[--c->clip_cache.len];
  clip.width = w; clip.height = h;
  clip.stride = (w + 3u) & ~3u;
  clip.is_rect = 0;
  clip.rect_x0 = clip.rect_y0 = clip.rect_x1 = clip.rect_y1 = 0;
  clip.min_x = clip.min_y = clip.max_x = clip.max_y = 0;
  return clip;
}

static void rp_release_clip (hb_raster_paint_t *c, rclip_t clip)
{
  if (clip.alpha.a && !clipvec_t_push (&c->clip_cache, clip)) free (clip.alpha.a);
}

static void rp_push_clip (hb_raster_paint_t *c, rclip_t clip)
{
  if (!clipvec_t_push (&c->clips, clip)) free (clip.alpha.a);
}

static xform_t rp_effective_transform (hb_raster_paint_t *c)
{
  xform_t t = c->transforms.len ? c->transforms.a[c->transforms.len - 1] : xform_identity;
  t.xx /= c->x_scale_factor; t.xy /= c->x_scale_factor; t.x0 /= c->x_scale_factor;
  t.yx /= c->y_scale_factor; t.yy /= c->y_scale_factor; t.y0 /= c->y_scale_factor;
  return t;
}

static hb_raster_image_t *rp_surface (hb_raster_paint_t *c) { return c->surfaces.len ? c->surfaces.a[c->surfaces.len - 1] : NULL; }
static const rclip_t null_clip;
static const rclip_t *rp_clip (hb_raster_paint_t *c) { return c->clips.len ? &c->clips.a[c->clips.len - 1] : &null_clip; }

static void rp_ensure_initialized (hb_raster_paint_t *c)
{
  if (c->surfaces.len) return;
  if (!rp_charge (c, (int64_t) c->fixed_extents.width * c->fixed_extents.height)) return;
  hb_raster_image_t *root = rp_acquire_surface (c);
  if (!root) return;
  if (!imgvec_t_push (&c->surfaces, root)) { rp_release_surface (c, root); return; }
  if (c->budget != BUDGET_UNLIMITED) rp_recharge (c);
  rclip_t clip;
  memset (&clip, 0, sizeof (clip));
  clip_init_full (&clip, c->fixed_extents.width, c->fixed_extents.height);
  if (!clipvec_t_push (&c->clips, clip)) rp_release_surface (c, c->surfaces.a[--c->surfaces.len]);
}

static void rp_push_transform (void *d, float xx, float yx, float xy, float yy, float dx, float dy)
{
  hb_raster_paint_t *c = (hb_raster_paint_t *) d;
  rp_ensure_initialized (c);
  xform_t t = c->transforms.len ? c->transforms.a[c->transforms.len - 1] : xform_identity, o = {xx, yx, xy, yy, dx, dy};
  xform_multiply (&t, &o, 0);
  xvec_t_push (&c->transforms, t);
}

static void rp_pop_transform (void *d)
{
  hb_raster_paint_t *c = (hb_raster_paint_t *) d;
  if (c->transforms.len) c->transforms.len--;
}

static void rp_push_empty_clip (hb_raster_paint_t *c, unsigned w, unsigned h)
{
  rclip_t clip = rp_acquire_clip (c, w, h);
  clip_init_full (&clip, w, h);
  clip.rect_x1 = clip.rect_y1 = 0;
  clip.max_x = clip.max_y = 0;
  rp_push_clip (c, clip);
}

static int rp_intersect_mask (const hb_raster_image_t *surf, const hb_raster_extents_t *m, const rclip_t *clip,
			      unsigned w, unsigned h, int64_t *mx0_out, int64_t *my0_out,
			      unsigned *ix0, unsigned *iy0, unsigned *ix1, unsigned *iy1)
{
  int64_t mx0 = (int64_t) m->x_origin - surf->extents.x_origin, my0 = (int64_t) m->y_origin - surf->extents.y_origin;
  int64_t x0 = HB_MAX ((int64_t) clip->min_x, HB_MAX (mx0, (int64_t) 0));
  int64_t y0 = HB_MAX ((int64_t) clip->min_y, HB_MAX (my0, (int64_t) 0));
  int64_t x1 = HB_MIN ((int64_t) clip->max_x, HB_MIN (mx0 + m->width, (int64_t) w));
  int64_t y1 = HB_MIN ((int64_t) clip->max_y, HB_MIN (my0 + m->height, (int64_t) h));
  if (x0 >= x1 || y0 >= y1) return 0;
  *mx0_out = mx0; *my0_out = my0;
  *ix0 = (unsigned) x0; *iy0 = (unsigned) y0; *ix1 = (unsigned) x1; *iy1 = (unsigned) y1;
  return 1;
}

static void rp_draw_glyph (hb_raster_paint_t *c, hb_raster_image_t *surf, hb_codepoint_t glyph, hb_font_t *font)
{
  rdraw_t *r = &c->rdr;
  xform_t t = rp_effective_transform (c);
  r->transform = t;
  rdraw_set_clip_box (r, (float) surf->extents.x_origin, (float) surf->extents.y_origin,
		      (float) surf->extents.x_origin + (float) surf->extents.width,
		      (float) surf->extents.y_origin + (float) surf->extents.height);
  int64_t b = c->budget_remaining;
  if (b < 0 && b != BUDGET_DEFAULT) b = 0;
  r->budget = b;
  rdraw_recharge (r);
  font_draw_glyph (font, glyph, &rdraw_funcs, r);
  c->budget_remaining = r->budget_remaining;
}

static void rp_push_clip_glyph (void *d, hb_codepoint_t glyph, hb_font_t *font)
{
  hb_raster_paint_t *c = (hb_raster_paint_t *) d;
  rp_ensure_initialized (c);
  hb_raster_image_t *surf = rp_surface (c);
  if (!surf) return;
  unsigned w = surf->extents.width, h = surf->extents.height;
  if (c->budget_remaining < 0) { rp_push_empty_clip (c, w, h); return; }
  rp_draw_glyph (c, surf, glyph, font);
  rdraw_t *r = &c->rdr;
  if (!rp_precharge (c, rdraw_pixel_work (r, h, w))) { rdraw_clear (r); rp_push_empty_clip (c, w, h); return; }
  hb_raster_image_t *mask = rdraw_render (r);
  if (!mask) { rp_push_empty_clip (c, w, h); return; }
  hb_raster_extents_t me = mask->extents;
  if (!rp_charge (c, (int64_t) me.width * me.height + (int64_t) w * h))
  {
    rdraw_recycle (r, mask);
    rp_push_empty_clip (c, w, h);
    return;
  }
  rclip_t nc = rp_acquire_clip (c, w, h);
  size_t size = (size_t) nc.stride * h;
  if (size > RASTER_MAX_BUFFER_SIZE || !bvec_t_resize (&nc.alpha, (unsigned) size, 1))
  {
    rp_release_clip (c, nc);
    rdraw_recycle (r, mask);
    rp_push_empty_clip (c, w, h);
    return;
  }
  memset (nc.alpha.a, 0, size);
  const rclip_t *old = rp_clip (c);
  int64_t mx0, my0;
  unsigned ix0, iy0, ix1, iy1;
  if (!clip_valid_mask (old) || !rp_intersect_mask (surf, &me, old, w, h, &mx0, &my0, &ix0, &iy0, &ix1, &iy1))
  {
    rp_release_clip (c, nc);
    rdraw_recycle (r, mask);
    rp_push_empty_clip (c, w, h);
    return;
  }
  nc.min_x = w; nc.min_y = h; nc.max_x = 0; nc.max_y = 0;
  const uint8_t *mbuf = mask->buffer.a;
  for (unsigned y = iy0; y < iy1; y++)
  {
    const uint8_t *mrow = mbuf + (unsigned) ((int64_t) y - my0) * me.stride;
    const uint8_t *orow = old->is_rect ? NULL : old->alpha.a + y * old->stride;
    uint8_t *out = nc.alpha.a + y * nc.stride;
    unsigned row_min = ix1, row_max = ix0;
    for (unsigned x = ix0; x < ix1; x++)
    {
      uint8_t a = mrow[(unsigned) ((int64_t) x - mx0)];
      if (orow) a = div255 (a * orow[x]);
      out[x] = a;
      if (a)
      {
	row_min = HB_MIN (row_min, x);
	row_max = x + 1;
      }
    }
    if (row_min < row_max)
    {
      nc.min_x = HB_MIN (nc.min_x, row_min);
      nc.min_y = HB_MIN (nc.min_y, y);
      nc.max_x = HB_MAX (nc.max_x, row_max);
      nc.max_y = HB_MAX (nc.max_y, y + 1);
    }
  }
  rdraw_recycle (r, mask);
  rp_push_clip (c, nc);
}

static void rp_push_clip_rectangle (void *d, float xmin, float ymin, float xmax, float ymax)
{
  hb_raster_paint_t *c = (hb_raster_paint_t *) d;
  rp_ensure_initialized (c);
  if (!c->surfaces.len) return;
  xform_t t = rp_effective_transform (c);
  hb_raster_image_t *surf = rp_surface (c);
  unsigned w = surf->extents.width, h = surf->extents.height;
  int axis = t.xy == 0.f && t.yx == 0.f;
  float cx[4] = {xmin, xmax, xmax, xmin}, cy[4] = {ymin, ymin, ymax, ymax};
  for (unsigned i = 0; i < 4; i++) xform_point (&t, &cx[i], &cy[i]);
  float fx0 = cx[0], fy0 = cy[0], fx1 = cx[0], fy1 = cy[0];
  for (unsigned i = 1; i < 4; i++)
  {
    fx0 = HB_MIN (fx0, cx[i]); fy0 = HB_MIN (fy0, cy[i]);
    fx1 = HB_MAX (fx1, cx[i]); fy1 = HB_MAX (fy1, cy[i]);
  }
  int px0 = clamp_i32 ((double) floorf (fx0) - surf->extents.x_origin);
  int py0 = clamp_i32 ((double) floorf (fy0) - surf->extents.y_origin);
  int px1 = clamp_i32 ((double) ceilf (fx1) - surf->extents.x_origin);
  int py1 = clamp_i32 ((double) ceilf (fy1) - surf->extents.y_origin);
  px0 = HB_MAX (px0, 0); py0 = HB_MAX (py0, 0);
  px1 = HB_MIN (px1, (int) w); py1 = HB_MIN (py1, (int) h);
  const rclip_t *old = rp_clip (c);
  if (!clip_valid_mask (old)) { rp_push_empty_clip (c, w, h); return; }
  if (!(axis && old->is_rect) && !rp_charge (c, (int64_t) w * h)) { rp_push_empty_clip (c, w, h); return; }
  rclip_t nc = rp_acquire_clip (c, w, h);
  old = rp_clip (c);
  if (axis && old->is_rect)
  {
    nc.is_rect = 1;
    nc.rect_x0 = HB_MAX (px0, old->rect_x0);
    nc.rect_y0 = HB_MAX (py0, old->rect_y0);
    nc.rect_x1 = HB_MIN (px1, old->rect_x1);
    nc.rect_y1 = HB_MIN (py1, old->rect_y1);
    nc.min_x = (unsigned) HB_MAX (nc.rect_x0, 0);
    nc.min_y = (unsigned) HB_MAX (nc.rect_y0, 0);
    nc.max_x = (unsigned) HB_MAX (HB_MIN (nc.rect_x1, (int) w), 0);
    nc.max_y = (unsigned) HB_MAX (HB_MIN (nc.rect_y1, (int) h), 0);
    rp_push_clip (c, nc);
    return;
  }
  size_t size = (size_t) nc.stride * h;
  if (size > RASTER_MAX_BUFFER_SIZE || !bvec_t_resize (&nc.alpha, (unsigned) size, 1))
  {
    rp_release_clip (c, nc);
    rp_push_empty_clip (c, w, h);
    return;
  }
  memset (nc.alpha.a, 0, size);
  int iy0 = HB_MAX (py0, (int) old->min_y), iy1 = HB_MIN (py1, (int) old->max_y);
  int ix0 = HB_MAX (px0, (int) old->min_x), ix1 = HB_MIN (px1, (int) old->max_x);
  if (axis)
  {
    if (ix0 < ix1 && iy0 < iy1)
    {
      for (int y = iy0; y < iy1; y++)
	memcpy (nc.alpha.a + y * nc.stride + ix0, old->alpha.a + y * old->stride + ix0, ix1 - ix0);
      nc.min_x = ix0; nc.min_y = iy0; nc.max_x = ix1; nc.max_y = iy1;
    }
    rp_push_clip (c, nc);
    return;
  }
  nc.min_x = w; nc.min_y = h; nc.max_x = 0; nc.max_y = 0;
  if (ix0 < ix1 && iy0 < iy1)
  {
    float qx[4], qy[4], enx[4], eny[4], ed[4], area2 = 0.f;
    for (unsigned i = 0; i < 4; i++) { qx[i] = cx[i] - surf->extents.x_origin; qy[i] = cy[i] - surf->extents.y_origin; }
    for (unsigned i = 0; i < 4; i++)
    {
      unsigned j = (i + 1) & 3;
      enx[i] = qy[j] - qy[i];
      eny[i] = -(qx[j] - qx[i]);
      ed[i] = enx[i] * qx[i] + eny[i] * qy[i];
    }
    for (unsigned i = 0; i < 4; i++)
    {
      unsigned j = (i + 1) & 3;
      area2 += qx[i] * qy[j] - qx[j] * qy[i];
    }
    int ccw = area2 >= 0.f;
    for (unsigned y = (unsigned) iy0; y < (unsigned) iy1; y++)
    {
      const uint8_t *orow = old->is_rect ? NULL : old->alpha.a + y * old->stride;
      for (unsigned x = (unsigned) ix0; x < (unsigned) ix1; x++)
      {
	float pxf = x + 0.5f, pyf = y + 0.5f;
	int inside = 1;
	for (unsigned i = 0; i < 4; i++)
	{
	  float dd = enx[i] * pxf + eny[i] * pyf;
	  if (ccw ? dd < ed[i] : dd > ed[i]) { inside = 0; break; }
	}
	uint8_t a = inside ? (orow ? orow[x] : 255) : 0;
	nc.alpha.a[y * nc.stride + x] = a;
	if (a)
	{
	  nc.min_x = HB_MIN (nc.min_x, x);
	  nc.min_y = HB_MIN (nc.min_y, y);
	  nc.max_x = HB_MAX (nc.max_x, x + 1);
	  nc.max_y = HB_MAX (nc.max_y, y + 1);
	}
      }
    }
  }
  rp_push_clip (c, nc);
}

static void rp_pop_clip (void *d)
{
  hb_raster_paint_t *c = (hb_raster_paint_t *) d;
  if (!c->clips.len) return;
  rp_release_clip (c, c->clips.a[--c->clips.len]);
}

static void rp_push_group (void *d)
{
  hb_raster_paint_t *c = (hb_raster_paint_t *) d;
  rp_ensure_initialized (c);
  if (!rp_charge (c, (int64_t) c->fixed_extents.width * c->fixed_extents.height)) return;
  hb_raster_image_t *s = rp_acquire_surface (c);
  if (!s) return;
  if (!imgvec_t_push (&c->surfaces, s)) rp_release_surface (c, s);
}

static void rp_pop_group (void *d, int mode)
{
  hb_raster_paint_t *c = (hb_raster_paint_t *) d;
  if (c->surfaces.len < 2) return;
  hb_raster_image_t *src = c->surfaces.a[--c->surfaces.len], *dst = rp_surface (c);
  if (dst && src && rp_charge (c, (int64_t) dst->extents.width * dst->extents.height))
    image_composite (dst, src, mode);
  rp_release_surface (c, src);
}

static inline uint32_t premul_pixel (hb_color_t color)
{
  uint8_t a = color_alpha_byte (color);
  return pack_pixel (div255 (color_blue (color) * a), div255 (color_green (color) * a), div255 (color_red (color) * a), a);
}

static void rp_solid (hb_raster_paint_t *c, hb_raster_image_t *surf, hb_color_t color, hb_raster_image_t *mask)
{
  uint32_t premul = premul_pixel (color);
  uint8_t pa = (uint8_t) (premul >> 24);
  const rclip_t *clip = rp_clip (c);
  unsigned stride = surf->extents.stride;
  if (clip->min_x >= clip->max_x || clip->min_y >= clip->max_y) return;
  if (pa == 0) return;
  unsigned ix0 = clip->min_x, iy0 = clip->min_y, ix1 = clip->max_x, iy1 = clip->max_y;
  const uint8_t *mbuf = NULL;
  hb_raster_extents_t me;
  int64_t mx0 = 0, my0 = 0;
  if (mask)
  {
    mbuf = mask->buffer.a;
    me = mask->extents;
    if (!rp_intersect_mask (surf, &me, clip, surf->extents.width, surf->extents.height, &mx0, &my0, &ix0, &iy0, &ix1, &iy1))
      return;
  }
  if (!rp_charge (c, (int64_t) (ix1 - ix0) * (iy1 - iy0))) return;
  for (unsigned y = iy0; y < iy1; y++)
  {
    uint32_t *row = (uint32_t *) (void *) (surf->buffer.a + y * stride);
    const uint8_t *crow = clip->is_rect ? NULL : clip->alpha.a + y * clip->stride;
    const uint8_t *mrow = mbuf ? mbuf + (unsigned) ((int64_t) y - my0) * me.stride : NULL;
    unsigned mx = (unsigned) ((int64_t) ix0 - mx0);
    for (unsigned x = ix0; x < ix1; x++)
    {
      unsigned a = crow ? crow[x] : 255;
      if (mrow) a = crow ? div255 (mrow[mx++] * a) : mrow[mx++];
      if (a == 0) continue;
      if (a == 255 && pa == 255) row[x] = premul;
      else row[x] = src_over (alpha_mul (premul, a), row[x]);
    }
  }
}

static void rp_color (void *d, int fg, hb_color_t color)
{
  hb_raster_paint_t *c = (hb_raster_paint_t *) d;
  rp_ensure_initialized (c);
  hb_raster_image_t *surf = rp_surface (c);
  if (surf) rp_solid (c, surf, color, NULL);
}

static void rp_fill_glyph (void *d, hb_codepoint_t glyph, hb_font_t *font, int fg, hb_color_t color)
{
  hb_raster_paint_t *c = (hb_raster_paint_t *) d;
  rp_ensure_initialized (c);
  hb_raster_image_t *surf = rp_surface (c);
  if (!surf) return;
  if (c->budget_remaining < 0) return;
  rp_draw_glyph (c, surf, glyph, font);
  rdraw_t *r = &c->rdr;
  if (!rp_precharge (c, rdraw_pixel_work (r, surf->extents.height, surf->extents.width))) { rdraw_clear (r); return; }
  hb_raster_image_t *mask = rdraw_render (r);
  if (!mask) return;
  if (rp_charge (c, (int64_t) mask->extents.width * mask->extents.height)) rp_solid (c, surf, color, mask);
  rdraw_recycle (r, mask);
}

static float reflect_t (float t)
{
  t = fmodf (fabsf (t), 2.f);
  return t > 1.f ? 2.f - t : t;
}

static uint32_t eval_color_line (const color_stop_t *stops, unsigned len, float t, int extend)
{
  if (!len) return 0;
  if (len == 1) return premul_pixel (stops[0].color);
  if (!isfinite (t)) t = 0.f;
  if (extend == EXTEND_PAD) t = HB_CLAMP (t, 0.f, 1.f);
  else if (extend == EXTEND_REPEAT) t = t - floorf (t);
  else t = reflect_t (t);
  if (t <= stops[0].offset) return premul_pixel (stops[0].color);
  if (t >= stops[len - 1].offset) return premul_pixel (stops[len - 1].color);
  unsigned i;
  for (i = 0; i < len - 1; i++)
    if (t < stops[i + 1].offset) break;
  float range = stops[i + 1].offset - stops[i].offset;
  float k = range > 0.f ? (t - stops[i].offset) / range : 0.f;
  hb_color_t c0 = stops[i].color, c1 = stops[i + 1].color;
  float a0 = color_alpha_byte (c0) / 255.f, r0 = color_red (c0) / 255.f * a0, g0 = color_green (c0) / 255.f * a0, b0 = color_blue (c0) / 255.f * a0;
  float a1 = color_alpha_byte (c1) / 255.f, r1 = color_red (c1) / 255.f * a1, g1 = color_green (c1) / 255.f * a1, b1 = color_blue (c1) / 255.f * a1;
  float a = a0 + k * (a1 - a0), r = r0 + k * (r1 - r0), g = g0 + k * (g1 - g0), b = b0 + k * (b1 - b0);
  return pack_pixel ((uint8_t) (b * 255.f + 0.5f), (uint8_t) (g * 255.f + 0.5f), (uint8_t) (r * 255.f + 0.5f), (uint8_t) (a * 255.f + 0.5f));
}

static uint32_t lookup_lut (const uint32_t *lut, float t, int extend)
{
  float u;
  if (!isfinite (t)) u = 0.f;
  else if (extend == EXTEND_PAD) u = HB_CLAMP (t, 0.f, 1.f);
  else if (extend == EXTEND_REPEAT) { u = t - floorf (t); if (u < 0.f) u += 1.f; }
  else u = reflect_t (t);
  if (!isfinite (u)) u = 0.f;
  else u = HB_CLAMP (u, 0.f, 1.f);
  return lut[(unsigned) (u * (GRADIENT_LUT_SIZE - 1) + 0.5f)];
}

static int stop_cmp (const color_stop_t *a, const color_stop_t *b) { return (a->offset > b->offset) - (a->offset < b->offset); }
static void stop_swap (color_stop_t *a, color_stop_t *b) { color_stop_t t = *a; *a = *b; *b = t; }

static void stops_qsort_loop (color_stop_t *base, size_t nel)
{
  while (nel > 24)
  {
    color_stop_t *last = base + nel - 1, *mid = base + nel / 2;
    if (stop_cmp (base, mid) > 0) stop_swap (base, mid);
    if (stop_cmp (mid, last) > 0)
    {
      stop_swap (mid, last);
      if (stop_cmp (base, mid) > 0) stop_swap (base, mid);
    }
    stop_swap (mid, last - 1);
    color_stop_t *pivot = last - 1, *i = base, *j = last - 1;
    for (;;)
    {
      while (stop_cmp (++i, pivot) < 0) {}
      while (stop_cmp (--j, pivot) > 0) {}
      if (i >= j) break;
      stop_swap (i, j);
    }
    stop_swap (i, last - 1);
    size_t left = (size_t) (i - base), right = nel - left - 1;
    if (left < right) { stops_qsort_loop (base, left); base = i + 1; nel = right; }
    else { stops_qsort_loop (i + 1, right); nel = left; }
  }
}

static void normalize_color_line (color_stop_t *stops, unsigned len, float *min, float *max)
{
  if (!len) { *min = *max = 0.f; return; }
  stops_qsort_loop (stops, len);
  for (color_stop_t *pi = stops + 1; pi < stops + len; pi++)
    for (color_stop_t *pj = pi; pj > stops && stop_cmp (pj - 1, pj) > 0; pj--) stop_swap (pj - 1, pj);
  float mn = stops[0].offset, mx = stops[0].offset;
  for (unsigned i = 1; i < len; i++) { mn = HB_MIN (mn, stops[i].offset); mx = HB_MAX (mx, stops[i].offset); }
  if (mn != mx)
    for (unsigned i = 0; i < len; i++) stops[i].offset = (stops[i].offset - mn) / (mx - mn);
  *min = mn;
  *max = mx;
}

enum { GRAD_LINEAR, GRAD_RADIAL, GRAD_SWEEP };

typedef struct
{
  int kind, extend, use_lut;
  const color_stop_t *stops;
  unsigned len;
  uint32_t lut[GRADIENT_LUT_SIZE];
  float gx0, gy0, dx, dy, inv_denom;
  float cx0, cy0, cr0, cdx, cdy, dr, A;
  float cx, cy, a0, inv_angle_range;
} gradient_t;

static int gradient_t_at (const gradient_t *g, float gx, float gy, float *t)
{
  switch (g->kind)
  {
  case GRAD_LINEAR:
    *t = ((gx - g->gx0) * g->dx + (gy - g->gy0) * g->dy) * g->inv_denom;
    return 1;
  case GRAD_RADIAL:
  {
    float dpx = gx - g->cx0, dpy = gy - g->cy0;
    float B = -2.f * (dpx * g->cdx + dpy * g->cdy + g->cr0 * g->dr);
    float C = dpx * dpx + dpy * dpy - g->cr0 * g->cr0;
    if (fabsf (g->A) > 1e-10f)
    {
      float disc = B * B - 4.f * g->A * C;
      if (disc < 0.f) return 0;
      float sq = sqrtf (disc);
      float t1 = (-B + sq) / (2.f * g->A), t2 = (-B - sq) / (2.f * g->A);
      *t = (g->cr0 + t1 * g->dr >= 0.f) ? t1 : t2;
      return 1;
    }
    if (fabsf (B) < 1e-10f) return 0;
    *t = -C / B;
    return 1;
  }
  default:
  {
    float angle = atan2f (gy - g->cy, gx - g->cx);
    if (angle < 0) angle += (float) (2.f * HB_PI);
    *t = (angle - g->a0) * g->inv_angle_range;
    return 1;
  }
  }
}

static int rp_gradient_begin (hb_raster_paint_t *c, color_line_t *cl, gradient_t *g, float *mn, float *mx)
{
  rp_ensure_initialized (c);
  if (!rp_surface (c)) return 0;
  unsigned count = color_line_get_stops (cl, 0, NULL, NULL);
  if (!count || !stopvec_t_resize (&c->stops, count, 1)) { c->stops.len = 0; return 0; }
  color_line_get_stops (cl, 0, &count, c->stops.a);
  g->stops = c->stops.a;
  g->len = c->stops.len;
  normalize_color_line (c->stops.a, g->len, mn, mx);
  g->extend = color_line_get_extend (cl);
  const rclip_t *clip = rp_clip (c);
  unsigned cw = clip->max_x > clip->min_x ? clip->max_x - clip->min_x : 0;
  unsigned ch = clip->max_y > clip->min_y ? clip->max_y - clip->min_y : 0;
  if (!rp_charge (c, (int64_t) cw * ch)) return 0;
  g->use_lut = (uint64_t) cw * ch >= GRADIENT_LUT_MIN_PIXELS;
  if (g->use_lut)
    for (unsigned i = 0; i < GRADIENT_LUT_SIZE; i++)
      g->lut[i] = eval_color_line (g->stops, g->len, (float) i / (GRADIENT_LUT_SIZE - 1), EXTEND_PAD);
  return 1;
}

static void rp_gradient_fill (hb_raster_paint_t *c, const gradient_t *g)
{
  hb_raster_image_t *surf = rp_surface (c);
  const rclip_t *clip = rp_clip (c);
  xform_t t = rp_effective_transform (c);
  float det = t.xx * t.yy - t.xy * t.yx;
  if (fabsf (det) < 1e-10f) return;
  if (g->kind == GRAD_LINEAR && g->dx * g->dx + g->dy * g->dy < 1e-10f) return;
  float inv_det = 1.f / det;
  float inv_xx = t.yy * inv_det, inv_xy = -t.xy * inv_det, inv_yx = -t.yx * inv_det, inv_yy = t.xx * inv_det;
  float inv_x0 = (t.xy * t.y0 - t.yy * t.x0) * inv_det, inv_y0 = (t.yx * t.x0 - t.xx * t.y0) * inv_det;
  unsigned stride = surf->extents.stride;
  int ox = surf->extents.x_origin, oy = surf->extents.y_origin;
  for (unsigned py = clip->min_y; py < clip->max_y; py++)
  {
    uint32_t *row = (uint32_t *) (void *) (surf->buffer.a + py * stride);
    const uint8_t *crow = clip->is_rect ? NULL : clip->alpha.a + py * clip->stride;
    float gx = inv_xx * ((float) ((int) clip->min_x + ox) + 0.5f) + inv_xy * ((float) ((int) py + oy) + 0.5f) + inv_x0;
    float gy = inv_yx * ((float) ((int) clip->min_x + ox) + 0.5f) + inv_yy * ((float) ((int) py + oy) + 0.5f) + inv_y0;
    for (unsigned px = clip->min_x; px < clip->max_x; px++, gx += inv_xx, gy += inv_yx)
    {
      uint8_t ca = crow ? crow[px] : 255;
      float gt;
      if (!ca || !gradient_t_at (g, gx, gy, &gt)) continue;
      uint32_t src = g->use_lut ? lookup_lut (g->lut, gt, g->extend) : eval_color_line (g->stops, g->len, gt, g->extend);
      if (crow) src = alpha_mul (src, ca);
      row[px] = src_over (src, row[px]);
    }
  }
}

static void rp_linear_gradient (void *d, color_line_t *cl, float x0, float y0, float x1, float y1, float x2, float y2)
{
  hb_raster_paint_t *c = (hb_raster_paint_t *) d;
  gradient_t g;
  float mn, mx;
  if (!rp_gradient_begin (c, cl, &g, &mn, &mx)) return;
  g.kind = GRAD_LINEAR;
  float lx0 = x0, ly0 = y0, lx1 = x1, ly1 = y1;
  float q2x = x2 - x0, q2y = y2 - y0, s = q2x * q2x + q2y * q2y;
  if (s >= 1e-6f)
  {
    float k = (q2x * (x1 - x0) + q2y * (y1 - y0)) / s;
    lx1 = x1 - k * q2x;
    ly1 = y1 - k * q2y;
  }
  g.gx0 = lx0 + mn * (lx1 - lx0);
  g.gy0 = ly0 + mn * (ly1 - ly0);
  float gx1 = lx0 + mx * (lx1 - lx0), gy1 = ly0 + mx * (ly1 - ly0);
  g.dx = gx1 - g.gx0;
  g.dy = gy1 - g.gy0;
  g.inv_denom = 1.f / (g.dx * g.dx + g.dy * g.dy);
  rp_gradient_fill (c, &g);
}

static void rp_radial_gradient (void *d, color_line_t *cl, float x0, float y0, float r0, float x1, float y1, float r1)
{
  hb_raster_paint_t *c = (hb_raster_paint_t *) d;
  gradient_t g;
  float mn, mx;
  if (!rp_gradient_begin (c, cl, &g, &mn, &mx)) return;
  g.kind = GRAD_RADIAL;
  g.cx0 = x0 + mn * (x1 - x0);
  g.cy0 = y0 + mn * (y1 - y0);
  g.cr0 = r0 + mn * (r1 - r0);
  float cx1 = x0 + mx * (x1 - x0), cy1 = y0 + mx * (y1 - y0), cr1 = r0 + mx * (r1 - r0);
  g.cdx = cx1 - g.cx0;
  g.cdy = cy1 - g.cy0;
  g.dr = cr1 - g.cr0;
  g.A = g.cdx * g.cdx + g.cdy * g.cdy - g.dr * g.dr;
  rp_gradient_fill (c, &g);
}

static void rp_sweep_gradient (void *d, color_line_t *cl, float cx, float cy, float start_angle, float end_angle)
{
  hb_raster_paint_t *c = (hb_raster_paint_t *) d;
  gradient_t g;
  float mn, mx;
  if (!rp_gradient_begin (c, cl, &g, &mn, &mx)) return;
  g.kind = GRAD_SWEEP;
  g.cx = cx;
  g.cy = cy;
  g.a0 = start_angle + mn * (end_angle - start_angle);
  float a1 = start_angle + mx * (end_angle - start_angle), range = a1 - g.a0;
  if (fabsf (range) < 1e-10f) return;
  g.inv_angle_range = 1.f / range;
  rp_gradient_fill (c, &g);
}

typedef struct { const uint8_t *p, *end; uint32_t bits; int n, err; } zin_t;
typedef struct { uint16_t count[16], sym[320]; } zhuff_t;

static unsigned z_bits (zin_t *z, int n)
{
  while (z->n < n)
  {
    if (z->p >= z->end) { z->err = 1; return 0; }
    z->bits |= (uint32_t) *z->p++ << z->n;
    z->n += 8;
  }
  unsigned v = z->bits & ((1u << n) - 1);
  z->bits >>= n;
  z->n -= n;
  return v;
}

static int z_build (zhuff_t *h, const uint8_t *len, unsigned n)
{
  uint16_t offs[16];
  memset (h->count, 0, sizeof (h->count));
  for (unsigned i = 0; i < n; i++) h->count[len[i]]++;
  h->count[0] = 0;
  int left = 1;
  for (unsigned i = 1; i < 16; i++)
  {
    left = (left << 1) - h->count[i];
    if (left < 0) return 0;
  }
  offs[1] = 0;
  for (unsigned i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
  for (unsigned i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (uint16_t) i;
  return 1;
}

static int z_decode (zin_t *z, const zhuff_t *h)
{
  int code = 0, first = 0, index = 0;
  for (unsigned len = 1; len < 16; len++)
  {
    code |= (int) z_bits (z, 1);
    int count = h->count[len];
    if (code - count < first) return h->sym[index + (code - first)];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  z->err = 1;
  return 0;
}

static int z_inflate (const uint8_t *src, size_t src_len, uint8_t *out, size_t out_len)
{
  static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
  static const uint8_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
  static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
  static const uint8_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
  static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
  if (src_len < 2 || (src[0] & 15) != 8 || (src[0] >> 4) > 7 || (src[1] & 32) || ((src[0] << 8) | src[1]) % 31) return 0;
  zin_t z = {src + 2, src + src_len, 0, 0, 0};
  size_t pos = 0;
  zhuff_t lh, dh;
  uint8_t lens[320];
  int last;
  do
  {
    last = (int) z_bits (&z, 1);
    unsigned type = z_bits (&z, 2);
    if (z.err) return 0;
    if (type == 0)
    {
      z.bits = 0; z.n = 0;
      if (z.end - z.p < 4) return 0;
      unsigned len = z.p[0] | (z.p[1] << 8);
      if ((len ^ (z.p[2] | (z.p[3] << 8))) != 0xFFFF) return 0;
      z.p += 4;
      if ((size_t) (z.end - z.p) < len || out_len - pos < len) return 0;
      memcpy (out + pos, z.p, len);
      z.p += len; pos += len;
      continue;
    }
    if (type == 1)
    {
      unsigned i = 0;
      for (; i < 144; i++) lens[i] = 8;
      for (; i < 256; i++) lens[i] = 9;
      for (; i < 280; i++) lens[i] = 7;
      for (; i < 288; i++) lens[i] = 8;
      for (; i < 318; i++) lens[i] = 5;
      z_build (&lh, lens, 288);
      z_build (&dh, lens + 288, 30);
    }
    else if (type == 2)
    {
      unsigned nlen = z_bits (&z, 5) + 257, ndist = z_bits (&z, 5) + 1, ncode = z_bits (&z, 4) + 4;
      if (nlen > 286 || ndist > 30) return 0;
      memset (lens, 0, 19);
      for (unsigned i = 0; i < ncode; i++) lens[order[i]] = (uint8_t) z_bits (&z, 3);
      if (z.err || !z_build (&lh, lens, 19)) return 0;
      for (unsigned i = 0; i < nlen + ndist;)
      {
	int sym = z_decode (&z, &lh);
	if (z.err) return 0;
	if (sym < 16) { lens[i++] = (uint8_t) sym; continue; }
	unsigned rep = 0, val = 0;
	if (sym == 16) { if (!i) return 0; val = lens[i - 1]; rep = 3 + z_bits (&z, 2); }
	else if (sym == 17) rep = 3 + z_bits (&z, 3);
	else rep = 11 + z_bits (&z, 7);
	if (i + rep > nlen + ndist) return 0;
	while (rep--) lens[i++] = (uint8_t) val;
      }
      if (!lens[256] || !z_build (&lh, lens, nlen) || !z_build (&dh, lens + nlen, ndist)) return 0;
    }
    else return 0;
    for (;;)
    {
      int sym = z_decode (&z, &lh);
      if (z.err) return 0;
      if (sym < 256)
      {
	if (pos >= out_len) return 0;
	out[pos++] = (uint8_t) sym;
	continue;
      }
      if (sym == 256) break;
      sym -= 257;
      if (sym >= 29) return 0;
      unsigned len = lbase[sym] + z_bits (&z, lext[sym]);
      int ds = z_decode (&z, &dh);
      if (z.err || ds >= 30) return 0;
      size_t dist = dbase[ds] + z_bits (&z, dext[ds]);
      if (z.err || dist > pos || out_len - pos < len) return 0;
      for (; len; len--, pos++) out[pos] = out[pos - dist];
    }
  } while (!last);
  return pos == out_len;
}

static int png_decode (hb_raster_image_t *img, const uint8_t *d, size_t len)
{
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  static const uint8_t ax0[7] = {0, 4, 0, 2, 0, 1, 0}, ay0[7] = {0, 0, 4, 0, 2, 0, 1};
  static const uint8_t adx[7] = {8, 8, 4, 4, 2, 2, 1}, ady[7] = {8, 8, 8, 4, 4, 2, 2};
  if (len < 33 || memcmp (d, sig, 8) || rd32 (d + 12) != HB_TAG ('I','H','D','R')) return 0;
  uint32_t w = rd32 (d + 16), h = rd32 (d + 20);
  unsigned depth = d[24], type = d[25], interlace = d[28];
  static const uint8_t chans[7] = {1, 0, 3, 1, 2, 0, 4};
  if (!w || !h || w > INT_MAX || h > INT_MAX || type > 6 || !chans[type] || d[26] || d[27] || interlace > 1) return 0;
  if (!(depth == 8 || depth == 16 || (depth < 8 && (depth & (depth - 1)) == 0 && (type == 0 || type == 3))) ||
      (type == 3 && depth == 16)) return 0;
  if ((size_t) w * h > RASTER_MAX_BUFFER_SIZE / 4) return 0;
  unsigned bpp = chans[type] * depth;
  uint8_t pal[256 * 4];
  unsigned npal = 0, has_trns = 0, trns[3] = {0, 0, 0};
  memset (pal, 0, sizeof (pal));
  uint8_t *idat = NULL, *raw = NULL;
  size_t idat_len = 0, raw_len = 0;
  int ok = 0;
  for (size_t p = 8; p + 12 <= len;)
  {
    uint32_t n = rd32 (d + p), tag = rd32 (d + p + 4);
    if (n > len - p - 12) goto done;
    const uint8_t *c = d + p + 8;
    if (tag == HB_TAG ('P','L','T','E'))
    {
      npal = HB_MIN (n / 3, 256u);
      for (unsigned i = 0; i < npal; i++) { pal[i * 4] = c[i * 3]; pal[i * 4 + 1] = c[i * 3 + 1]; pal[i * 4 + 2] = c[i * 3 + 2]; pal[i * 4 + 3] = 255; }
    }
    else if (tag == HB_TAG ('t','R','N','S'))
    {
      has_trns = 1;
      if (type == 3) for (unsigned i = 0; i < n && i < 256; i++) pal[i * 4 + 3] = c[i];
      else if (type == 0 && n >= 2) trns[0] = rd16 (c);
      else if (type == 2 && n >= 6) { trns[0] = rd16 (c); trns[1] = rd16 (c + 2); trns[2] = rd16 (c + 4); }
      else has_trns = 0;
    }
    else if (tag == HB_TAG ('I','D','A','T'))
    {
      uint8_t *t = (uint8_t *) realloc (idat, idat_len + n + 1);
      if (!t) goto done;
      idat = t;
      memcpy (idat + idat_len, c, n);
      idat_len += n;
    }
    else if (tag == HB_TAG ('I','E','N','D')) break;
    p += 12 + (size_t) n;
  }
  if (!idat || (type == 3 && !npal)) goto done;
  for (unsigned pass = interlace ? 0 : 6; pass < 7; pass++)
  {
    size_t pw = interlace ? (w + adx[pass] - 1 - ax0[pass]) / adx[pass] : w;
    size_t ph = interlace ? (h + ady[pass] - 1 - ay0[pass]) / ady[pass] : h;
    if (pw && ph) raw_len += ph * (1 + (pw * bpp + 7) / 8);
  }
  raw = (uint8_t *) malloc (raw_len);
  if (!raw || !z_inflate (idat, idat_len, raw, raw_len)) goto done;
  hb_raster_extents_t ext = {0, 0, w, h, 0};
  if (!image_configure (img, RASTER_FORMAT_BGRA32, ext)) goto done;
  {
    unsigned fb = (bpp + 7) / 8, gscale = depth < 8 ? 255 / ((1 << depth) - 1) : 1;
    uint8_t *r = raw;
    for (unsigned pass = interlace ? 0 : 6; pass < 7; pass++)
    {
      unsigned x0 = interlace ? ax0[pass] : 0, y0 = interlace ? ay0[pass] : 0;
      unsigned dx = interlace ? adx[pass] : 1, dy = interlace ? ady[pass] : 1;
      size_t pw = (w + dx - 1 - x0) / dx, ph = (h + dy - 1 - y0) / dy;
      if (!pw || !ph) continue;
      size_t stride = (pw * bpp + 7) / 8;
      uint8_t *prev = NULL;
      for (size_t y = 0; y < ph; y++, prev = r + 1, r += stride + 1)
      {
	uint8_t *row = r + 1;
	unsigned f = r[0];
	for (size_t i = 0; i < stride; i++)
	{
	  unsigned a = i >= fb ? row[i - fb] : 0, b = prev ? prev[i] : 0, c = prev && i >= fb ? prev[i - fb] : 0;
	  switch (f)
	  {
	  case 0: break;
	  case 1: row[i] += a; break;
	  case 2: row[i] += b; break;
	  case 3: row[i] += (a + b) >> 1; break;
	  case 4:
	  {
	    int pa = abs ((int) b - (int) c), pb = abs ((int) a - (int) c), pc = abs ((int) a + (int) b - 2 * (int) c);
	    row[i] += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
	    break;
	  }
	  default: goto done;
	  }
	}
	uint32_t *dst = (uint32_t *) (void *) (img->buffer.a + (size_t) (h - 1 - (y0 + y * dy)) * img->extents.stride);
	for (size_t x = 0; x < pw; x++)
	{
	  unsigned R, G, B, A = 255, v[4];
	  if (depth < 8)
	  {
	    size_t bit = x * depth;
	    v[0] = (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
	  }
	  else
	    for (unsigned k = 0; k < chans[type]; k++)
	      v[k] = depth == 16 ? rd16 (row + (x * chans[type] + k) * 2) : row[x * chans[type] + k];
	  unsigned sh = depth == 16 ? 8 : 0;
	  switch (type)
	  {
	  case 0:
	    R = G = B = depth < 8 ? v[0] * gscale : v[0] >> sh;
	    if (has_trns && v[0] == trns[0]) A = 0;
	    break;
	  case 2:
	    R = v[0] >> sh; G = v[1] >> sh; B = v[2] >> sh;
	    if (has_trns && v[0] == trns[0] && v[1] == trns[1] && v[2] == trns[2]) A = 0;
	    break;
	  case 3:
	    R = pal[v[0] * 4]; G = pal[v[0] * 4 + 1]; B = pal[v[0] * 4 + 2]; A = v[0] < npal ? pal[v[0] * 4 + 3] : 255;
	    break;
	  case 4: R = G = B = v[0] >> sh; A = v[1] >> sh; break;
	  default: R = v[0] >> sh; G = v[1] >> sh; B = v[2] >> sh; A = v[3] >> sh; break;
	  }
	  dst[x0 + x * dx] = pack_pixel (div255 (B * A), div255 (G * A), div255 (R * A), (uint8_t) A);
	}
      }
    }
  }
  ok = 1;
done:
  free (idat);
  free (raw);
  return ok;
}

static uint32_t sample_bilinear (const uint32_t *src, unsigned width, unsigned height, float x, float y)
{
  int x0 = (int) floorf (x), y0 = (int) floorf (y), x1 = x0 + 1, y1 = y0 + 1;
  if (x1 >= (int) width) x1 = (int) width - 1;
  if (y1 >= (int) height) y1 = (int) height - 1;
  float tx = x - x0, ty = y - y0;
  float w00 = (1.f - tx) * (1.f - ty), w10 = tx * (1.f - ty), w01 = (1.f - tx) * ty, w11 = tx * ty;
  uint32_t p00 = src[(size_t) y0 * width + x0], p10 = src[(size_t) y0 * width + x1];
  uint32_t p01 = src[(size_t) y1 * width + x0], p11 = src[(size_t) y1 * width + x1];
  uint32_t r = 0;
  for (unsigned s = 0; s < 32; s += 8)
  {
    float v = ((p00 >> s) & 0xff) * w00 + ((p10 >> s) & 0xff) * w10 + ((p01 >> s) & 0xff) * w01 + ((p11 >> s) & 0xff) * w11;
    r |= (uint32_t) (v + 0.5f) << s;
  }
  return r;
}

static int rp_image (void *d, const uint8_t *png, unsigned len, unsigned width, unsigned height, hb_glyph_extents_t *extents)
{
  hb_raster_paint_t *c = (hb_raster_paint_t *) d;
  rp_ensure_initialized (c);
  if (c->pixel_remaining < 0) return 0;
  hb_raster_image_t src;
  memset (&src, 0, sizeof (src));
  int ret = 0;
  if (!png_decode (&src, png, len)) goto done;
  unsigned sw = src.extents.width, sh = src.extents.height;
  const uint32_t *sd = (const uint32_t *) (const void *) src.buffer.a;
  hb_raster_image_t *surf = rp_surface (c);
  if (!surf || !extents) goto done;
  const rclip_t *clip = rp_clip (c);
  xform_t t = rp_effective_transform (c);
  float det = t.xx * t.yy - t.xy * t.yx;
  if (fabsf (det) < 1e-10f) goto done;
  float inv_det = 1.f / det;
  float inv_xx = t.yy * inv_det, inv_xy = -t.xy * inv_det, inv_yx = -t.yx * inv_det, inv_yy = t.xx * inv_det;
  float inv_x0 = (t.xy * t.y0 - t.yy * t.x0) * inv_det, inv_y0 = (t.yx * t.x0 - t.xx * t.y0) * inv_det;
  int ox = surf->extents.x_origin, oy = surf->extents.y_origin;
  float img_x = extents->x_bearing, img_y = extents->y_bearing;
  float img_sx = (float) extents->width / sw, img_sy = (float) extents->height / sh;
  if (fabsf (img_sx) < 1e-10f || fabsf (img_sy) < 1e-10f) goto done;
  unsigned clip_w = clip->max_x > clip->min_x ? clip->max_x - clip->min_x : 0;
  unsigned clip_h = clip->max_y > clip->min_y ? clip->max_y - clip->min_y : 0;
  if (!rp_charge (c, (int64_t) clip_w * clip_h + (int64_t) sw * sh)) goto done;
  for (unsigned py = clip->min_y; py < clip->max_y; py++)
  {
    uint32_t *row = (uint32_t *) (void *) (surf->buffer.a + py * surf->extents.stride);
    const uint8_t *crow = clip->is_rect ? NULL : clip->alpha.a + py * clip->stride;
    float gx = inv_xx * (float) ((int) clip->min_x + ox) + inv_xy * (float) ((int) py + oy) + inv_x0;
    float gy = inv_yx * (float) ((int) clip->min_x + ox) + inv_yy * (float) ((int) py + oy) + inv_y0;
    for (unsigned px = clip->min_x; px < clip->max_x; px++, gx += inv_xx, gy += inv_yx)
    {
      unsigned a = crow ? crow[px] : 255;
      if (!a) continue;
      float ix = (gx - img_x) / img_sx, iy = (float) (sh - 1) - (gy - img_y) / img_sy;
      if (!isfinite (ix) || !isfinite (iy) || ix < 0.f || iy < 0.f || ix > (float) (sw - 1) || iy > (float) (sh - 1)) continue;
      uint32_t p = sample_bilinear (sd, sw, sh, ix, iy);
      if (crow) p = alpha_mul (p, a);
      row[px] = src_over (p, row[px]);
    }
  }
  ret = 1;
done:
  free (src.buffer.a);
  return ret;
}

static int rp_color_glyph (void *d, hb_codepoint_t glyph, hb_font_t *font) { return 0; }

static const paint_funcs_t raster_paint_funcs = {
  rp_push_transform, rp_pop_transform, rp_color_glyph, rp_push_clip_glyph, rp_push_clip_rectangle, rp_pop_clip, rp_color,
  rp_linear_gradient, rp_radial_gradient, rp_sweep_gradient, rp_push_group, NULL, rp_pop_group, rp_fill_glyph, rp_image
};

hb_raster_paint_t *
hb_raster_paint_create_or_fail (void)
{
  hb_raster_paint_t *p = (hb_raster_paint_t *) calloc (1, sizeof (hb_raster_paint_t));
  if (!p) return NULL;
  p->base_transform = xform_identity;
  p->x_scale_factor = p->y_scale_factor = 1.f;
  p->foreground = HB_COLOR (0, 0, 0, 255);
  p->budget = BUDGET_DEFAULT;
  p->budget_remaining = BUDGET_GLYPH;
  p->pixel_remaining = BUDGET_RASTER_PIXELS;
  rdraw_reset (&p->rdr);
  return p;
}

void
hb_raster_paint_destroy (hb_raster_paint_t *p)
{
  if (!p) return;
  rdraw_free (&p->rdr);
  for (unsigned i = 0; i < p->surfaces.len; i++) image_destroy (p->surfaces.a[i]);
  for (unsigned i = 0; i < p->surface_cache.len; i++) image_destroy (p->surface_cache.a[i]);
  for (unsigned i = 0; i < p->clips.len; i++) free (p->clips.a[i].alpha.a);
  for (unsigned i = 0; i < p->clip_cache.len; i++) free (p->clip_cache.a[i].alpha.a);
  free (p->surfaces.a); free (p->surface_cache.a); free (p->clips.a); free (p->clip_cache.a);
  free (p->transforms.a); free (p->stops.a);
  free (p);
}

static void rp_clear (hb_raster_paint_t *p)
{
  memset (&p->fixed_extents, 0, sizeof (p->fixed_extents));
  p->has_extents = 0;
  p->transforms.len = 0;
  while (p->clips.len) rp_release_clip (p, p->clips.a[--p->clips.len]);
  for (unsigned i = 0; i < p->surfaces.len; i++) rp_release_surface (p, p->surfaces.a[i]);
  p->surfaces.len = 0;
  rp_recharge (p);
  rdraw_reset (&p->rdr);
}

void
hb_raster_paint_reset (hb_raster_paint_t *p)
{
  p->base_transform = xform_identity;
  p->x_scale_factor = p->y_scale_factor = 1.f;
  p->foreground = HB_COLOR (0, 0, 0, 255);
  p->budget = BUDGET_DEFAULT;
  rp_clear (p);
}

void
hb_raster_paint_set_transform (hb_raster_paint_t *p, float xx, float yx, float xy, float yy, float dx, float dy)
{
  xform_t t = {xx, yx, xy, yy, dx, dy};
  p->base_transform = t;
}

void
hb_raster_paint_set_scale_factor (hb_raster_paint_t *p, float x_scale_factor, float y_scale_factor)
{
  p->x_scale_factor = x_scale_factor > 0.f ? x_scale_factor : 1.f;
  p->y_scale_factor = y_scale_factor > 0.f ? y_scale_factor : 1.f;
}

hb_bool_t
hb_raster_paint_set_glyph_extents (hb_raster_paint_t *p, const hb_glyph_extents_t *ge)
{
  float x0 = (float) ge->x_bearing, y0 = (float) ge->y_bearing;
  float x1 = (float) ge->x_bearing + ge->width, y1 = (float) ge->y_bearing + ge->height;
  float xmin = HB_MIN (x0, x1), xmax = HB_MAX (x0, x1), ymin = HB_MIN (y0, y1), ymax = HB_MAX (y0, y1);
  float px[4] = {xmin, xmin, xmax, xmax}, py[4] = {ymin, ymax, ymin, ymax};
  xform_t t = p->base_transform;
  t.xx /= p->x_scale_factor; t.xy /= p->x_scale_factor; t.x0 /= p->x_scale_factor;
  t.yx /= p->y_scale_factor; t.yy /= p->y_scale_factor; t.y0 /= p->y_scale_factor;
  xform_point (&t, &px[0], &py[0]);
  float tx0 = px[0], tx1 = px[0], ty0 = py[0], ty1 = py[0];
  for (unsigned i = 1; i < 4; i++)
  {
    xform_point (&t, &px[i], &py[i]);
    tx0 = HB_MIN (tx0, px[i]); tx1 = HB_MAX (tx1, px[i]);
    ty0 = HB_MIN (ty0, py[i]); ty1 = HB_MAX (ty1, py[i]);
  }
  int32_t ex0 = clamp_i32 (floorf (tx0)), ey0 = clamp_i32 (floorf (ty0));
  int32_t ex1 = clamp_i32 (ceilf (tx1)), ey1 = clamp_i32 (ceilf (ty1));
  if (ex1 <= ex0 || ey1 <= ey0)
  {
    memset (&p->fixed_extents, 0, sizeof (p->fixed_extents));
    p->has_extents = 0;
    return 0;
  }
  p->fixed_extents.x_origin = ex0;
  p->fixed_extents.y_origin = ey0;
  p->fixed_extents.width = (unsigned) HB_MIN ((int64_t) ex1 - ex0, (int64_t) RASTER_MAX_AUTO_DIMENSION);
  p->fixed_extents.height = (unsigned) HB_MIN ((int64_t) ey1 - ey0, (int64_t) RASTER_MAX_AUTO_DIMENSION);
  p->fixed_extents.stride = p->fixed_extents.width * 4;
  p->has_extents = 1;
  return 1;
}

void
hb_raster_paint_set_foreground (hb_raster_paint_t *p, hb_color_t foreground)
{
  p->foreground = foreground;
}

void
hb_raster_paint_glyph (hb_raster_paint_t *p, hb_font_t *font, hb_codepoint_t glyph)
{
  if (!p->has_extents)
  {
    hb_glyph_extents_t ge;
    if (hb_font_get_glyph_extents (font, glyph, &ge)) hb_raster_paint_set_glyph_extents (p, &ge);
  }
  xform_t b = p->base_transform;
  pf_push_transform (&raster_paint_funcs, p, b.xx, b.yx, b.xy, b.yy, b.x0, b.y0);
  if (!font_paint_glyph (font, glyph, &raster_paint_funcs, p, 0, p->foreground))
    rp_fill_glyph (p, glyph, font, 1, p->foreground);
  rp_pop_transform (p);
}

hb_raster_image_t *
hb_raster_paint_render (hb_raster_paint_t *p)
{
  hb_raster_image_t *result = NULL;
  if (p->has_extents)
  {
    if (p->surfaces.len)
    {
      result = p->surfaces.a[0];
      for (unsigned i = 1; i < p->surfaces.len; i++) rp_release_surface (p, p->surfaces.a[i]);
      p->surfaces.len = 0;
    }
    else
      result = rp_acquire_surface (p);
  }
  rp_clear (p);
  return result;
}

void
hb_raster_paint_recycle_image (hb_raster_paint_t *p, hb_raster_image_t *image)
{
  rp_release_surface (p, image);
}

#define ENCODE3(x,y,z) (((uint64_t) (x) << 42) | ((uint64_t) (y) << 21) | (uint64_t) (z))
#define ENCODE3_11_7_14(x,y,z) (((uint32_t) ((x) & 0x07FFu) << 21) | (((uint32_t) (y) & 0x007Fu) << 14) | (uint32_t) ((z) & 0x3FFFu))

typedef enum
{
  HB_SCRIPT_COMMON			= HB_TAG ('Z','y','y','y'),
  HB_SCRIPT_INHERITED			= HB_TAG ('Z','i','n','h'),
  HB_SCRIPT_UNKNOWN			= HB_TAG ('Z','z','z','z'),

  HB_SCRIPT_ARABIC			= HB_TAG ('A','r','a','b'),
  HB_SCRIPT_ARMENIAN			= HB_TAG ('A','r','m','n'),
  HB_SCRIPT_BENGALI			= HB_TAG ('B','e','n','g'),
  HB_SCRIPT_CYRILLIC			= HB_TAG ('C','y','r','l'),
  HB_SCRIPT_DEVANAGARI			= HB_TAG ('D','e','v','a'),
  HB_SCRIPT_GEORGIAN			= HB_TAG ('G','e','o','r'),
  HB_SCRIPT_GREEK			= HB_TAG ('G','r','e','k'),
  HB_SCRIPT_GUJARATI			= HB_TAG ('G','u','j','r'),
  HB_SCRIPT_GURMUKHI			= HB_TAG ('G','u','r','u'),
  HB_SCRIPT_HANGUL			= HB_TAG ('H','a','n','g'),
  HB_SCRIPT_HAN				= HB_TAG ('H','a','n','i'),
  HB_SCRIPT_HEBREW			= HB_TAG ('H','e','b','r'),
  HB_SCRIPT_HIRAGANA			= HB_TAG ('H','i','r','a'),
  HB_SCRIPT_KANNADA			= HB_TAG ('K','n','d','a'),
  HB_SCRIPT_KATAKANA			= HB_TAG ('K','a','n','a'),
  HB_SCRIPT_LAO				= HB_TAG ('L','a','o','o'),
  HB_SCRIPT_LATIN			= HB_TAG ('L','a','t','n'),
  HB_SCRIPT_MALAYALAM			= HB_TAG ('M','l','y','m'),
  HB_SCRIPT_ORIYA			= HB_TAG ('O','r','y','a'),
  HB_SCRIPT_TAMIL			= HB_TAG ('T','a','m','l'),
  HB_SCRIPT_TELUGU			= HB_TAG ('T','e','l','u'),
  HB_SCRIPT_THAI			= HB_TAG ('T','h','a','i'),

  HB_SCRIPT_TIBETAN			= HB_TAG ('T','i','b','t'),

  HB_SCRIPT_BOPOMOFO			= HB_TAG ('B','o','p','o'),
  HB_SCRIPT_BRAILLE			= HB_TAG ('B','r','a','i'),
  HB_SCRIPT_CANADIAN_SYLLABICS		= HB_TAG ('C','a','n','s'),
  HB_SCRIPT_CHEROKEE			= HB_TAG ('C','h','e','r'),
  HB_SCRIPT_ETHIOPIC			= HB_TAG ('E','t','h','i'),
  HB_SCRIPT_KHMER			= HB_TAG ('K','h','m','r'),
  HB_SCRIPT_MONGOLIAN			= HB_TAG ('M','o','n','g'),
  HB_SCRIPT_MYANMAR			= HB_TAG ('M','y','m','r'),
  HB_SCRIPT_OGHAM			= HB_TAG ('O','g','a','m'),
  HB_SCRIPT_RUNIC			= HB_TAG ('R','u','n','r'),
  HB_SCRIPT_SINHALA			= HB_TAG ('S','i','n','h'),
  HB_SCRIPT_SYRIAC			= HB_TAG ('S','y','r','c'),
  HB_SCRIPT_THAANA			= HB_TAG ('T','h','a','a'),
  HB_SCRIPT_YI				= HB_TAG ('Y','i','i','i'),

  HB_SCRIPT_DESERET			= HB_TAG ('D','s','r','t'),
  HB_SCRIPT_GOTHIC			= HB_TAG ('G','o','t','h'),
  HB_SCRIPT_OLD_ITALIC			= HB_TAG ('I','t','a','l'),

  HB_SCRIPT_BUHID			= HB_TAG ('B','u','h','d'),
  HB_SCRIPT_HANUNOO			= HB_TAG ('H','a','n','o'),
  HB_SCRIPT_TAGALOG			= HB_TAG ('T','g','l','g'),
  HB_SCRIPT_TAGBANWA			= HB_TAG ('T','a','g','b'),

  HB_SCRIPT_CYPRIOT			= HB_TAG ('C','p','r','t'),
  HB_SCRIPT_LIMBU			= HB_TAG ('L','i','m','b'),
  HB_SCRIPT_LINEAR_B			= HB_TAG ('L','i','n','b'),
  HB_SCRIPT_OSMANYA			= HB_TAG ('O','s','m','a'),
  HB_SCRIPT_SHAVIAN			= HB_TAG ('S','h','a','w'),
  HB_SCRIPT_TAI_LE			= HB_TAG ('T','a','l','e'),
  HB_SCRIPT_UGARITIC			= HB_TAG ('U','g','a','r'),

  HB_SCRIPT_BUGINESE			= HB_TAG ('B','u','g','i'),
  HB_SCRIPT_COPTIC			= HB_TAG ('C','o','p','t'),
  HB_SCRIPT_GLAGOLITIC			= HB_TAG ('G','l','a','g'),
  HB_SCRIPT_KHAROSHTHI			= HB_TAG ('K','h','a','r'),
  HB_SCRIPT_NEW_TAI_LUE			= HB_TAG ('T','a','l','u'),
  HB_SCRIPT_OLD_PERSIAN			= HB_TAG ('X','p','e','o'),
  HB_SCRIPT_SYLOTI_NAGRI		= HB_TAG ('S','y','l','o'),
  HB_SCRIPT_TIFINAGH			= HB_TAG ('T','f','n','g'),

  HB_SCRIPT_BALINESE			= HB_TAG ('B','a','l','i'),
  HB_SCRIPT_CUNEIFORM			= HB_TAG ('X','s','u','x'),
  HB_SCRIPT_NKO				= HB_TAG ('N','k','o','o'),
  HB_SCRIPT_PHAGS_PA			= HB_TAG ('P','h','a','g'),
  HB_SCRIPT_PHOENICIAN			= HB_TAG ('P','h','n','x'),

  HB_SCRIPT_CARIAN			= HB_TAG ('C','a','r','i'),
  HB_SCRIPT_CHAM			= HB_TAG ('C','h','a','m'),
  HB_SCRIPT_KAYAH_LI			= HB_TAG ('K','a','l','i'),
  HB_SCRIPT_LEPCHA			= HB_TAG ('L','e','p','c'),
  HB_SCRIPT_LYCIAN			= HB_TAG ('L','y','c','i'),
  HB_SCRIPT_LYDIAN			= HB_TAG ('L','y','d','i'),
  HB_SCRIPT_OL_CHIKI			= HB_TAG ('O','l','c','k'),
  HB_SCRIPT_REJANG			= HB_TAG ('R','j','n','g'),
  HB_SCRIPT_SAURASHTRA			= HB_TAG ('S','a','u','r'),
  HB_SCRIPT_SUNDANESE			= HB_TAG ('S','u','n','d'),
  HB_SCRIPT_VAI				= HB_TAG ('V','a','i','i'),

  HB_SCRIPT_AVESTAN			= HB_TAG ('A','v','s','t'),
  HB_SCRIPT_BAMUM			= HB_TAG ('B','a','m','u'),
  HB_SCRIPT_EGYPTIAN_HIEROGLYPHS	= HB_TAG ('E','g','y','p'),
  HB_SCRIPT_IMPERIAL_ARAMAIC		= HB_TAG ('A','r','m','i'),
  HB_SCRIPT_INSCRIPTIONAL_PAHLAVI	= HB_TAG ('P','h','l','i'),
  HB_SCRIPT_INSCRIPTIONAL_PARTHIAN	= HB_TAG ('P','r','t','i'),
  HB_SCRIPT_JAVANESE			= HB_TAG ('J','a','v','a'),
  HB_SCRIPT_KAITHI			= HB_TAG ('K','t','h','i'),
  HB_SCRIPT_LISU			= HB_TAG ('L','i','s','u'),
  HB_SCRIPT_MEETEI_MAYEK		= HB_TAG ('M','t','e','i'),
  HB_SCRIPT_OLD_SOUTH_ARABIAN		= HB_TAG ('S','a','r','b'),
  HB_SCRIPT_OLD_TURKIC			= HB_TAG ('O','r','k','h'),
  HB_SCRIPT_SAMARITAN			= HB_TAG ('S','a','m','r'),
  HB_SCRIPT_TAI_THAM			= HB_TAG ('L','a','n','a'),
  HB_SCRIPT_TAI_VIET			= HB_TAG ('T','a','v','t'),

  HB_SCRIPT_BATAK			= HB_TAG ('B','a','t','k'),
  HB_SCRIPT_BRAHMI			= HB_TAG ('B','r','a','h'),
  HB_SCRIPT_MANDAIC			= HB_TAG ('M','a','n','d'),

  HB_SCRIPT_CHAKMA			= HB_TAG ('C','a','k','m'),
  HB_SCRIPT_MEROITIC_CURSIVE		= HB_TAG ('M','e','r','c'),
  HB_SCRIPT_MEROITIC_HIEROGLYPHS	= HB_TAG ('M','e','r','o'),
  HB_SCRIPT_MIAO			= HB_TAG ('P','l','r','d'),
  HB_SCRIPT_SHARADA			= HB_TAG ('S','h','r','d'),
  HB_SCRIPT_SORA_SOMPENG		= HB_TAG ('S','o','r','a'),
  HB_SCRIPT_TAKRI			= HB_TAG ('T','a','k','r'),

  HB_SCRIPT_BASSA_VAH			= HB_TAG ('B','a','s','s'),
  HB_SCRIPT_CAUCASIAN_ALBANIAN		= HB_TAG ('A','g','h','b'),
  HB_SCRIPT_DUPLOYAN			= HB_TAG ('D','u','p','l'),
  HB_SCRIPT_ELBASAN			= HB_TAG ('E','l','b','a'),
  HB_SCRIPT_GRANTHA			= HB_TAG ('G','r','a','n'),
  HB_SCRIPT_KHOJKI			= HB_TAG ('K','h','o','j'),
  HB_SCRIPT_KHUDAWADI			= HB_TAG ('S','i','n','d'),
  HB_SCRIPT_LINEAR_A			= HB_TAG ('L','i','n','a'),
  HB_SCRIPT_MAHAJANI			= HB_TAG ('M','a','h','j'),
  HB_SCRIPT_MANICHAEAN			= HB_TAG ('M','a','n','i'),
  HB_SCRIPT_MENDE_KIKAKUI		= HB_TAG ('M','e','n','d'),
  HB_SCRIPT_MODI			= HB_TAG ('M','o','d','i'),
  HB_SCRIPT_MRO				= HB_TAG ('M','r','o','o'),
  HB_SCRIPT_NABATAEAN			= HB_TAG ('N','b','a','t'),
  HB_SCRIPT_OLD_NORTH_ARABIAN		= HB_TAG ('N','a','r','b'),
  HB_SCRIPT_OLD_PERMIC			= HB_TAG ('P','e','r','m'),
  HB_SCRIPT_PAHAWH_HMONG		= HB_TAG ('H','m','n','g'),
  HB_SCRIPT_PALMYRENE			= HB_TAG ('P','a','l','m'),
  HB_SCRIPT_PAU_CIN_HAU			= HB_TAG ('P','a','u','c'),
  HB_SCRIPT_PSALTER_PAHLAVI		= HB_TAG ('P','h','l','p'),
  HB_SCRIPT_SIDDHAM			= HB_TAG ('S','i','d','d'),
  HB_SCRIPT_TIRHUTA			= HB_TAG ('T','i','r','h'),
  HB_SCRIPT_WARANG_CITI			= HB_TAG ('W','a','r','a'),

  HB_SCRIPT_AHOM			= HB_TAG ('A','h','o','m'),
  HB_SCRIPT_ANATOLIAN_HIEROGLYPHS	= HB_TAG ('H','l','u','w'),
  HB_SCRIPT_HATRAN			= HB_TAG ('H','a','t','r'),
  HB_SCRIPT_MULTANI			= HB_TAG ('M','u','l','t'),
  HB_SCRIPT_OLD_HUNGARIAN		= HB_TAG ('H','u','n','g'),
  HB_SCRIPT_SIGNWRITING			= HB_TAG ('S','g','n','w'),

  HB_SCRIPT_ADLAM			= HB_TAG ('A','d','l','m'),
  HB_SCRIPT_BHAIKSUKI			= HB_TAG ('B','h','k','s'),
  HB_SCRIPT_MARCHEN			= HB_TAG ('M','a','r','c'),
  HB_SCRIPT_OSAGE			= HB_TAG ('O','s','g','e'),
  HB_SCRIPT_TANGUT			= HB_TAG ('T','a','n','g'),
  HB_SCRIPT_NEWA			= HB_TAG ('N','e','w','a'),

  HB_SCRIPT_MASARAM_GONDI		= HB_TAG ('G','o','n','m'),
  HB_SCRIPT_NUSHU			= HB_TAG ('N','s','h','u'),
  HB_SCRIPT_SOYOMBO			= HB_TAG ('S','o','y','o'),
  HB_SCRIPT_ZANABAZAR_SQUARE		= HB_TAG ('Z','a','n','b'),

  HB_SCRIPT_DOGRA			= HB_TAG ('D','o','g','r'),
  HB_SCRIPT_GUNJALA_GONDI		= HB_TAG ('G','o','n','g'),
  HB_SCRIPT_HANIFI_ROHINGYA		= HB_TAG ('R','o','h','g'),
  HB_SCRIPT_MAKASAR			= HB_TAG ('M','a','k','a'),
  HB_SCRIPT_MEDEFAIDRIN			= HB_TAG ('M','e','d','f'),
  HB_SCRIPT_OLD_SOGDIAN			= HB_TAG ('S','o','g','o'),
  HB_SCRIPT_SOGDIAN			= HB_TAG ('S','o','g','d'),

  HB_SCRIPT_ELYMAIC			= HB_TAG ('E','l','y','m'),
  HB_SCRIPT_NANDINAGARI			= HB_TAG ('N','a','n','d'),
  HB_SCRIPT_NYIAKENG_PUACHUE_HMONG	= HB_TAG ('H','m','n','p'),
  HB_SCRIPT_WANCHO			= HB_TAG ('W','c','h','o'),

  HB_SCRIPT_CHORASMIAN			= HB_TAG ('C','h','r','s'),
  HB_SCRIPT_DIVES_AKURU			= HB_TAG ('D','i','a','k'),
  HB_SCRIPT_KHITAN_SMALL_SCRIPT		= HB_TAG ('K','i','t','s'),
  HB_SCRIPT_YEZIDI			= HB_TAG ('Y','e','z','i'),

  HB_SCRIPT_CYPRO_MINOAN		= HB_TAG ('C','p','m','n'),
  HB_SCRIPT_OLD_UYGHUR			= HB_TAG ('O','u','g','r'),
  HB_SCRIPT_TANGSA			= HB_TAG ('T','n','s','a'),
  HB_SCRIPT_TOTO			= HB_TAG ('T','o','t','o'),
  HB_SCRIPT_VITHKUQI			= HB_TAG ('V','i','t','h'),

  HB_SCRIPT_MATH			= HB_TAG ('Z','m','t','h'),

  HB_SCRIPT_KAWI			= HB_TAG ('K','a','w','i'),
  HB_SCRIPT_NAG_MUNDARI			= HB_TAG ('N','a','g','m'),

  HB_SCRIPT_GARAY			= HB_TAG ('G','a','r','a'),
  HB_SCRIPT_GURUNG_KHEMA		= HB_TAG ('G','u','k','h'),
  HB_SCRIPT_KIRAT_RAI			= HB_TAG ('K','r','a','i'),
  HB_SCRIPT_OL_ONAL			= HB_TAG ('O','n','a','o'),
  HB_SCRIPT_SUNUWAR			= HB_TAG ('S','u','n','u'),
  HB_SCRIPT_TODHRI			= HB_TAG ('T','o','d','r'),
  HB_SCRIPT_TULU_TIGALARI		= HB_TAG ('T','u','t','g'),

  HB_SCRIPT_BERIA_ERFE			= HB_TAG ('B','e','r','f'),
  HB_SCRIPT_SIDETIC			= HB_TAG ('S','i','d','t'),
  HB_SCRIPT_TAI_YO			= HB_TAG ('T','a','y','o'),
  HB_SCRIPT_TOLONG_SIKI			= HB_TAG ('T','o','l','s'),

  HB_SCRIPT_JURCHEN			= HB_TAG ('J','u','r','c'),
  HB_SCRIPT_PROTO_CUNEIFORM		= HB_TAG ('P','c','u','n'),
  HB_SCRIPT_SEAL				= HB_TAG ('S','e','a','l'),

  HB_SCRIPT_INVALID			= 0

} hb_script_t;

static const hb_script_t _hb_ucd_sc_map[179]=
{
                   HB_SCRIPT_COMMON,              HB_SCRIPT_INHERITED,
                  HB_SCRIPT_UNKNOWN,                 HB_SCRIPT_ARABIC,
                 HB_SCRIPT_ARMENIAN,                HB_SCRIPT_BENGALI,
                 HB_SCRIPT_CYRILLIC,             HB_SCRIPT_DEVANAGARI,
                 HB_SCRIPT_GEORGIAN,                  HB_SCRIPT_GREEK,
                 HB_SCRIPT_GUJARATI,               HB_SCRIPT_GURMUKHI,
                   HB_SCRIPT_HANGUL,                    HB_SCRIPT_HAN,
                   HB_SCRIPT_HEBREW,               HB_SCRIPT_HIRAGANA,
                  HB_SCRIPT_KANNADA,               HB_SCRIPT_KATAKANA,
                      HB_SCRIPT_LAO,                  HB_SCRIPT_LATIN,
                HB_SCRIPT_MALAYALAM,                  HB_SCRIPT_ORIYA,
                    HB_SCRIPT_TAMIL,                 HB_SCRIPT_TELUGU,
                     HB_SCRIPT_THAI,                HB_SCRIPT_TIBETAN,
                 HB_SCRIPT_BOPOMOFO,                HB_SCRIPT_BRAILLE,
       HB_SCRIPT_CANADIAN_SYLLABICS,               HB_SCRIPT_CHEROKEE,
                 HB_SCRIPT_ETHIOPIC,                  HB_SCRIPT_KHMER,
                HB_SCRIPT_MONGOLIAN,                HB_SCRIPT_MYANMAR,
                    HB_SCRIPT_OGHAM,                  HB_SCRIPT_RUNIC,
                  HB_SCRIPT_SINHALA,                 HB_SCRIPT_SYRIAC,
                   HB_SCRIPT_THAANA,                     HB_SCRIPT_YI,
                  HB_SCRIPT_DESERET,                 HB_SCRIPT_GOTHIC,
               HB_SCRIPT_OLD_ITALIC,                  HB_SCRIPT_BUHID,
                  HB_SCRIPT_HANUNOO,                HB_SCRIPT_TAGALOG,
                 HB_SCRIPT_TAGBANWA,                HB_SCRIPT_CYPRIOT,
                    HB_SCRIPT_LIMBU,               HB_SCRIPT_LINEAR_B,
                  HB_SCRIPT_OSMANYA,                HB_SCRIPT_SHAVIAN,
                   HB_SCRIPT_TAI_LE,               HB_SCRIPT_UGARITIC,
                 HB_SCRIPT_BUGINESE,                 HB_SCRIPT_COPTIC,
               HB_SCRIPT_GLAGOLITIC,             HB_SCRIPT_KHAROSHTHI,
              HB_SCRIPT_NEW_TAI_LUE,            HB_SCRIPT_OLD_PERSIAN,
             HB_SCRIPT_SYLOTI_NAGRI,               HB_SCRIPT_TIFINAGH,
                 HB_SCRIPT_BALINESE,              HB_SCRIPT_CUNEIFORM,
                      HB_SCRIPT_NKO,               HB_SCRIPT_PHAGS_PA,
               HB_SCRIPT_PHOENICIAN,                 HB_SCRIPT_CARIAN,
                     HB_SCRIPT_CHAM,               HB_SCRIPT_KAYAH_LI,
                   HB_SCRIPT_LEPCHA,                 HB_SCRIPT_LYCIAN,
                   HB_SCRIPT_LYDIAN,               HB_SCRIPT_OL_CHIKI,
                   HB_SCRIPT_REJANG,             HB_SCRIPT_SAURASHTRA,
                HB_SCRIPT_SUNDANESE,                    HB_SCRIPT_VAI,
                  HB_SCRIPT_AVESTAN,                  HB_SCRIPT_BAMUM,
     HB_SCRIPT_EGYPTIAN_HIEROGLYPHS,       HB_SCRIPT_IMPERIAL_ARAMAIC,
    HB_SCRIPT_INSCRIPTIONAL_PAHLAVI, HB_SCRIPT_INSCRIPTIONAL_PARTHIAN,
                 HB_SCRIPT_JAVANESE,                 HB_SCRIPT_KAITHI,
                     HB_SCRIPT_LISU,           HB_SCRIPT_MEETEI_MAYEK,
        HB_SCRIPT_OLD_SOUTH_ARABIAN,             HB_SCRIPT_OLD_TURKIC,
                HB_SCRIPT_SAMARITAN,               HB_SCRIPT_TAI_THAM,
                 HB_SCRIPT_TAI_VIET,                  HB_SCRIPT_BATAK,
                   HB_SCRIPT_BRAHMI,                HB_SCRIPT_MANDAIC,
                   HB_SCRIPT_CHAKMA,       HB_SCRIPT_MEROITIC_CURSIVE,
     HB_SCRIPT_MEROITIC_HIEROGLYPHS,                   HB_SCRIPT_MIAO,
                  HB_SCRIPT_SHARADA,           HB_SCRIPT_SORA_SOMPENG,
                    HB_SCRIPT_TAKRI,              HB_SCRIPT_BASSA_VAH,
       HB_SCRIPT_CAUCASIAN_ALBANIAN,               HB_SCRIPT_DUPLOYAN,
                  HB_SCRIPT_ELBASAN,                HB_SCRIPT_GRANTHA,
                   HB_SCRIPT_KHOJKI,              HB_SCRIPT_KHUDAWADI,
                 HB_SCRIPT_LINEAR_A,               HB_SCRIPT_MAHAJANI,
               HB_SCRIPT_MANICHAEAN,          HB_SCRIPT_MENDE_KIKAKUI,
                     HB_SCRIPT_MODI,                    HB_SCRIPT_MRO,
                HB_SCRIPT_NABATAEAN,      HB_SCRIPT_OLD_NORTH_ARABIAN,
               HB_SCRIPT_OLD_PERMIC,           HB_SCRIPT_PAHAWH_HMONG,
                HB_SCRIPT_PALMYRENE,            HB_SCRIPT_PAU_CIN_HAU,
          HB_SCRIPT_PSALTER_PAHLAVI,                HB_SCRIPT_SIDDHAM,
                  HB_SCRIPT_TIRHUTA,            HB_SCRIPT_WARANG_CITI,
                     HB_SCRIPT_AHOM,  HB_SCRIPT_ANATOLIAN_HIEROGLYPHS,
                   HB_SCRIPT_HATRAN,                HB_SCRIPT_MULTANI,
            HB_SCRIPT_OLD_HUNGARIAN,            HB_SCRIPT_SIGNWRITING,
                    HB_SCRIPT_ADLAM,              HB_SCRIPT_BHAIKSUKI,
                  HB_SCRIPT_MARCHEN,                  HB_SCRIPT_OSAGE,
                   HB_SCRIPT_TANGUT,                   HB_SCRIPT_NEWA,
            HB_SCRIPT_MASARAM_GONDI,                  HB_SCRIPT_NUSHU,
                  HB_SCRIPT_SOYOMBO,       HB_SCRIPT_ZANABAZAR_SQUARE,
                    HB_SCRIPT_DOGRA,          HB_SCRIPT_GUNJALA_GONDI,
          HB_SCRIPT_HANIFI_ROHINGYA,                HB_SCRIPT_MAKASAR,
              HB_SCRIPT_MEDEFAIDRIN,            HB_SCRIPT_OLD_SOGDIAN,
                  HB_SCRIPT_SOGDIAN,                HB_SCRIPT_ELYMAIC,
              HB_SCRIPT_NANDINAGARI, HB_SCRIPT_NYIAKENG_PUACHUE_HMONG,
                   HB_SCRIPT_WANCHO,             HB_SCRIPT_CHORASMIAN,
              HB_SCRIPT_DIVES_AKURU,    HB_SCRIPT_KHITAN_SMALL_SCRIPT,
                   HB_SCRIPT_YEZIDI,           HB_SCRIPT_CYPRO_MINOAN,
               HB_SCRIPT_OLD_UYGHUR,                 HB_SCRIPT_TANGSA,
                     HB_SCRIPT_TOTO,               HB_SCRIPT_VITHKUQI,
                     HB_SCRIPT_MATH,                   HB_SCRIPT_KAWI,
              HB_SCRIPT_NAG_MUNDARI,                  HB_SCRIPT_GARAY,
             HB_SCRIPT_GURUNG_KHEMA,              HB_SCRIPT_KIRAT_RAI,
                  HB_SCRIPT_OL_ONAL,                HB_SCRIPT_SUNUWAR,
                   HB_SCRIPT_TODHRI,          HB_SCRIPT_TULU_TIGALARI,
               HB_SCRIPT_BERIA_ERFE,                HB_SCRIPT_SIDETIC,
                   HB_SCRIPT_TAI_YO,            HB_SCRIPT_TOLONG_SIKI,
                  HB_SCRIPT_JURCHEN,        HB_SCRIPT_PROTO_CUNEIFORM,
                     HB_SCRIPT_SEAL,
};
static const uint16_t _hb_ucd_dm1_p0_map[825]=
{
   0x003B, 0x004B, 0x0060, 0x00B4, 0x00B7, 0x00C5, 0x02B9, 0x0300,
   0x0301, 0x0313, 0x0385, 0x0386, 0x0388, 0x0389, 0x038A, 0x038C,
   0x038E, 0x038F, 0x0390, 0x03A9, 0x03AC, 0x03AD, 0x03AE, 0x03AF,
   0x03B0, 0x03B9, 0x03CC, 0x03CD, 0x03CE, 0x2002, 0x2003, 0x3008,
   0x3009, 0x349E, 0x34B9, 0x34BB, 0x34DF, 0x3515, 0x36EE, 0x36FC,
   0x3781, 0x382F, 0x3862, 0x387C, 0x38C7, 0x38E3, 0x391C, 0x393A,
   0x3A2E, 0x3A6C, 0x3AE4, 0x3B08, 0x3B19, 0x3B49, 0x3B9D, 0x3C18,
   0x3C4E, 0x3D33, 0x3D96, 0x3EAC, 0x3EB8, 0x3F1B, 0x3FFC, 0x4008,
   0x4018, 0x4039, 0x4046, 0x4096, 0x40E3, 0x412F, 0x4202, 0x4227,
   0x42A0, 0x4301, 0x4334, 0x4359, 0x43D5, 0x43D9, 0x440B, 0x446B,
   0x452B, 0x455D, 0x4561, 0x456B, 0x45D7, 0x45F9, 0x4635, 0x46BE,
   0x46C7, 0x4995, 0x49E6, 0x4A6E, 0x4A76, 0x4AB2, 0x4B33, 0x4BCE,
   0x4CCE, 0x4CED, 0x4CF8, 0x4D56, 0x4E0D, 0x4E26, 0x4E32, 0x4E38,
   0x4E39, 0x4E3D, 0x4E41, 0x4E82, 0x4E86, 0x4EAE, 0x4EC0, 0x4ECC,
   0x4EE4, 0x4F60, 0x4F80, 0x4F86, 0x4F8B, 0x4FAE, 0x4FBB, 0x4FBF,
   0x5002, 0x502B, 0x507A, 0x5099, 0x50CF, 0x50DA, 0x50E7, 0x5140,
   0x5145, 0x514D, 0x5154, 0x5164, 0x5167, 0x5168, 0x5169, 0x516D,
   0x5177, 0x5180, 0x518D, 0x5192, 0x5195, 0x5197, 0x51A4, 0x51AC,
   0x51B5, 0x51B7, 0x51C9, 0x51CC, 0x51DC, 0x51DE, 0x51F5, 0x5203,
   0x5207, 0x5217, 0x5229, 0x523A, 0x523B, 0x5246, 0x5272, 0x5277,
   0x5289, 0x529B, 0x52A3, 0x52B3, 0x52C7, 0x52C9, 0x52D2, 0x52DE,
   0x52E4, 0x52F5, 0x52FA, 0x5305, 0x5306, 0x5317, 0x533F, 0x5349,
   0x5351, 0x535A, 0x5373, 0x5375, 0x537D, 0x537F, 0x53C3, 0x53CA,
   0x53DF, 0x53E5, 0x53EB, 0x53F1, 0x5406, 0x540F, 0x541D, 0x5438,
   0x5442, 0x5448, 0x5468, 0x549E, 0x54A2, 0x54BD, 0x54F6, 0x5510,
   0x5553, 0x5555, 0x5563, 0x5584, 0x5587, 0x5599, 0x559D, 0x55AB,
   0x55B3, 0x55C0, 0x55C2, 0x55E2, 0x5606, 0x5651, 0x5668, 0x5674,
   0x56F9, 0x5716, 0x5717, 0x578B, 0x57CE, 0x57F4, 0x580D, 0x5831,
   0x5832, 0x5840, 0x585A, 0x585E, 0x58A8, 0x58AC, 0x58B3, 0x58D8,
   0x58DF, 0x58EE, 0x58F2, 0x58F7, 0x5906, 0x591A, 0x5922, 0x5944,
   0x5948, 0x5951, 0x5954, 0x5962, 0x5973, 0x59D8, 0x59EC, 0x5A1B,
   0x5A27, 0x5A62, 0x5A66, 0x5AB5, 0x5B08, 0x5B28, 0x5B3E, 0x5B85,
   0x5BC3, 0x5BD8, 0x5BE7, 0x5BEE, 0x5BF3, 0x5BFF, 0x5C06, 0x5C22,
   0x5C3F, 0x5C60, 0x5C62, 0x5C64, 0x5C65, 0x5C6E, 0x5C8D, 0x5CC0,
   0x5D19, 0x5D43, 0x5D50, 0x5D6B, 0x5D6E, 0x5D7C, 0x5DB2, 0x5DBA,
   0x5DE1, 0x5DE2, 0x5DFD, 0x5E28, 0x5E3D, 0x5E69, 0x5E74, 0x5EA6,
   0x5EB0, 0x5EB3, 0x5EB6, 0x5EC9, 0x5ECA, 0x5ED2, 0x5ED3, 0x5ED9,
   0x5EEC, 0x5EFE, 0x5F04, 0x5F22, 0x5F53, 0x5F62, 0x5F69, 0x5F6B,
   0x5F8B, 0x5F9A, 0x5FA9, 0x5FAD, 0x5FCD, 0x5FD7, 0x5FF5, 0x5FF9,
   0x6012, 0x601C, 0x6075, 0x6081, 0x6094, 0x60C7, 0x60D8, 0x60E1,
   0x6108, 0x6144, 0x6148, 0x614C, 0x614E, 0x6160, 0x6168, 0x617A,
   0x618E, 0x6190, 0x61A4, 0x61AF, 0x61B2, 0x61DE, 0x61F2, 0x61F6,
   0x6200, 0x6210, 0x621B, 0x622E, 0x6234, 0x625D, 0x62B1, 0x62C9,
   0x62CF, 0x62D3, 0x62D4, 0x62FC, 0x62FE, 0x633D, 0x6350, 0x6368,
   0x637B, 0x6383, 0x63A0, 0x63A9, 0x63C4, 0x63C5, 0x63E4, 0x641C,
   0x6422, 0x6452, 0x6469, 0x6477, 0x647E, 0x649A, 0x649D, 0x64C4,
   0x654F, 0x6556, 0x656C, 0x6578, 0x6599, 0x65C5, 0x65E2, 0x65E3,
   0x6613, 0x6649, 0x6674, 0x6688, 0x6691, 0x669C, 0x66B4, 0x66C6,
   0x66F4, 0x66F8, 0x6700, 0x6717, 0x671B, 0x6721, 0x674E, 0x6753,
   0x6756, 0x675E, 0x677B, 0x6785, 0x6797, 0x67F3, 0x67FA, 0x6817,
   0x681F, 0x6852, 0x6881, 0x6885, 0x688E, 0x68A8, 0x6914, 0x6942,
   0x69A3, 0x69EA, 0x6A02, 0x6A13, 0x6AA8, 0x6AD3, 0x6ADB, 0x6B04,
   0x6B21, 0x6B54, 0x6B72, 0x6B77, 0x6B79, 0x6B9F, 0x6BAE, 0x6BBA,
   0x6BBB, 0x6C4E, 0x6C67, 0x6C88, 0x6CBF, 0x6CCC, 0x6CCD, 0x6CE5,
   0x6D16, 0x6D1B, 0x6D1E, 0x6D34, 0x6D3E, 0x6D41, 0x6D69, 0x6D6A,
   0x6D77, 0x6D78, 0x6D85, 0x6DCB, 0x6DDA, 0x6DEA, 0x6DF9, 0x6E1A,
   0x6E2F, 0x6E6E, 0x6E9C, 0x6EBA, 0x6EC7, 0x6ECB, 0x6ED1, 0x6EDB,
   0x6F0F, 0x6F22, 0x6F23, 0x6F6E, 0x6FC6, 0x6FEB, 0x6FFE, 0x701B,
   0x701E, 0x7039, 0x704A, 0x7070, 0x7077, 0x707D, 0x7099, 0x70AD,
   0x70C8, 0x70D9, 0x7145, 0x7149, 0x716E, 0x719C, 0x71CE, 0x71D0,
   0x7210, 0x721B, 0x7228, 0x722B, 0x7235, 0x7250, 0x7262, 0x7280,
   0x7295, 0x72AF, 0x72C0, 0x72FC, 0x732A, 0x7375, 0x737A, 0x7387,
   0x738B, 0x73A5, 0x73B2, 0x73DE, 0x7406, 0x7409, 0x7422, 0x7447,
   0x745C, 0x7469, 0x7471, 0x7485, 0x7489, 0x7498, 0x74CA, 0x7506,
   0x7524, 0x753B, 0x753E, 0x7559, 0x7565, 0x7570, 0x75E2, 0x7610,
   0x761D, 0x761F, 0x7642, 0x7669, 0x76CA, 0x76DB, 0x76E7, 0x76F4,
   0x7701, 0x771E, 0x771F, 0x7740, 0x774A, 0x778B, 0x77A7, 0x784E,
   0x786B, 0x788C, 0x7891, 0x78CA, 0x78CC, 0x78FB, 0x792A, 0x793C,
   0x793E, 0x7948, 0x7949, 0x7950, 0x7956, 0x795D, 0x795E, 0x7965,
   0x797F, 0x798D, 0x798E, 0x798F, 0x79AE, 0x79CA, 0x79EB, 0x7A1C,
   0x7A40, 0x7A4A, 0x7A4F, 0x7A81, 0x7AB1, 0x7ACB, 0x7AEE, 0x7B20,
   0x7BC0, 0x7BC6, 0x7BC9, 0x7C3E, 0x7C60, 0x7C7B, 0x7C92, 0x7CBE,
   0x7CD2, 0x7CD6, 0x7CE3, 0x7CE7, 0x7CE8, 0x7D00, 0x7D10, 0x7D22,
   0x7D2F, 0x7D5B, 0x7D63, 0x7DA0, 0x7DBE, 0x7DC7, 0x7DF4, 0x7E02,
   0x7E09, 0x7E37, 0x7E41, 0x7E45, 0x7F3E, 0x7F72, 0x7F79, 0x7F7A,
   0x7F85, 0x7F95, 0x7F9A, 0x7FBD, 0x7FFA, 0x8001, 0x8005, 0x8046,
   0x8060, 0x806F, 0x8070, 0x807E, 0x808B, 0x80AD, 0x80B2, 0x8103,
   0x813E, 0x81D8, 0x81E8, 0x81ED, 0x8201, 0x8204, 0x8218, 0x826F,
   0x8279, 0x828B, 0x8291, 0x829D, 0x82B1, 0x82B3, 0x82BD, 0x82E5,
   0x82E6, 0x831D, 0x8323, 0x8336, 0x8352, 0x8353, 0x8363, 0x83AD,
   0x83BD, 0x83C9, 0x83CA, 0x83CC, 0x83DC, 0x83E7, 0x83EF, 0x83F1,
   0x843D, 0x8449, 0x8457, 0x84EE, 0x84F1, 0x84F3, 0x84FC, 0x8516,
   0x8564, 0x85CD, 0x85FA, 0x8606, 0x8612, 0x862D, 0x863F, 0x8650,
   0x865C, 0x8667, 0x8669, 0x8688, 0x86A9, 0x86E2, 0x870E, 0x8728,
   0x876B, 0x8779, 0x8786, 0x87BA, 0x87E1, 0x8801, 0x881F, 0x884C,
   0x8860, 0x8863, 0x88C2, 0x88CF, 0x88D7, 0x88DE, 0x88E1, 0x88F8,
   0x88FA, 0x8910, 0x8941, 0x8964, 0x8986, 0x898B, 0x8996, 0x8AA0,
   0x8AAA, 0x8ABF, 0x8ACB, 0x8AD2, 0x8AD6, 0x8AED, 0x8AF8, 0x8AFE,
   0x8B01, 0x8B39, 0x8B58, 0x8B80, 0x8B8A, 0x8C48, 0x8C55, 0x8CAB,
   0x8CC1, 0x8CC2, 0x8CC8, 0x8CD3, 0x8D08, 0x8D1B, 0x8D77, 0x8DBC,
   0x8DCB, 0x8DEF, 0x8DF0, 0x8ECA, 0x8ED4, 0x8F26, 0x8F2A, 0x8F38,
   0x8F3B, 0x8F62, 0x8F9E, 0x8FB0, 0x8FB6, 0x9023, 0x9038, 0x9072,
   0x907C, 0x908F, 0x9094, 0x90CE, 0x90DE, 0x90F1, 0x90FD, 0x9111,
   0x911B, 0x916A, 0x9199, 0x91B4, 0x91CC, 0x91CF, 0x91D1, 0x9234,
   0x9238, 0x9276, 0x927C, 0x92D7, 0x92D8, 0x9304, 0x934A, 0x93F9,
   0x9415, 0x958B, 0x95AD, 0x95B7, 0x962E, 0x964B, 0x964D, 0x9675,
   0x9678, 0x967C, 0x9686, 0x96A3, 0x96B7, 0x96B8, 0x96C3, 0x96E2,
   0x96E3, 0x96F6, 0x96F7, 0x9723, 0x9732, 0x9748, 0x9756, 0x97DB,
   0x97E0, 0x97FF, 0x980B, 0x9818, 0x9829, 0x983B, 0x985E, 0x98E2,
   0x98EF, 0x98FC, 0x9928, 0x9929, 0x99A7, 0x99C2, 0x99F1, 0x99FE,
   0x9A6A, 0x9B12, 0x9B6F, 0x9C40, 0x9C57, 0x9CFD, 0x9D67, 0x9DB4,
   0x9DFA, 0x9E1E, 0x9E7F, 0x9E97, 0x9E9F, 0x9EBB, 0x9ECE, 0x9EF9,
   0x9EFE, 0x9F05, 0x9F0F, 0x9F16, 0x9F3B, 0x9F43, 0x9F8D, 0x9F8E,
   0x9F9C,
};
static const uint16_t _hb_ucd_dm1_p2_map[110]=
{
   0x0122, 0x051C, 0x0525, 0x054B, 0x063A, 0x0804, 0x08DE, 0x0A2C,
   0x0B63, 0x14E4, 0x16A8, 0x16EA, 0x19C8, 0x1B18, 0x1D0B, 0x1DE4,
   0x1DE6, 0x2183, 0x219F, 0x2331, 0x26D4, 0x2844, 0x284A, 0x2B0C,
   0x2BF1, 0x300A, 0x32B8, 0x335F, 0x3393, 0x339C, 0x33C3, 0x33D5,
   0x346D, 0x36A3, 0x38A7, 0x3A8D, 0x3AFA, 0x3CBC, 0x3D1E, 0x3ED1,
   0x3F5E, 0x3F8E, 0x4263, 0x42EE, 0x43AB, 0x4608, 0x4735, 0x4814,
   0x4C36, 0x4C92, 0x4FA1, 0x4FB8, 0x5044, 0x50F2, 0x50F3, 0x5119,
   0x5133, 0x5249, 0x541D, 0x5626, 0x569A, 0x56C5, 0x597C, 0x5AA7,
   0x5BAB, 0x5C80, 0x5CD0, 0x5F86, 0x61DA, 0x6228, 0x6247, 0x62D9,
   0x633E, 0x64DA, 0x6523, 0x65A8, 0x67A7, 0x67B5, 0x6B3C, 0x6C36,
   0x6CD5, 0x6D6B, 0x6F2C, 0x6FB1, 0x70D2, 0x73CA, 0x7667, 0x78AE,
   0x7966, 0x7CA8, 0x7ED3, 0x7F2F, 0x85D2, 0x85ED, 0x872E, 0x8BFA,
   0x8D77, 0x9145, 0x91DF, 0x921A, 0x940A, 0x9496, 0x95B6, 0x9B30,
   0xA0CE, 0xA105, 0xA20E, 0xA291, 0xA392, 0xA600,
};
static const uint32_t _hb_ucd_dm2_u32_map[638]=
{
  ENCODE3_11_7_14 (0x003C, 0x0338, 0x226E),ENCODE3_11_7_14 (0x003D, 0x0338, 0x2260),
  ENCODE3_11_7_14 (0x003E, 0x0338, 0x226F),ENCODE3_11_7_14 (0x0041, 0x0300, 0x00C0),
  ENCODE3_11_7_14 (0x0041, 0x0301, 0x00C1),ENCODE3_11_7_14 (0x0041, 0x0302, 0x00C2),
  ENCODE3_11_7_14 (0x0041, 0x0303, 0x00C3),ENCODE3_11_7_14 (0x0041, 0x0304, 0x0100),
  ENCODE3_11_7_14 (0x0041, 0x0306, 0x0102),ENCODE3_11_7_14 (0x0041, 0x0307, 0x0226),
  ENCODE3_11_7_14 (0x0041, 0x0308, 0x00C4),ENCODE3_11_7_14 (0x0041, 0x0309, 0x1EA2),
  ENCODE3_11_7_14 (0x0041, 0x030A, 0x00C5),ENCODE3_11_7_14 (0x0041, 0x030C, 0x01CD),
  ENCODE3_11_7_14 (0x0041, 0x030F, 0x0200),ENCODE3_11_7_14 (0x0041, 0x0311, 0x0202),
  ENCODE3_11_7_14 (0x0041, 0x0323, 0x1EA0),ENCODE3_11_7_14 (0x0041, 0x0325, 0x1E00),
  ENCODE3_11_7_14 (0x0041, 0x0328, 0x0104),ENCODE3_11_7_14 (0x0042, 0x0307, 0x1E02),
  ENCODE3_11_7_14 (0x0042, 0x0323, 0x1E04),ENCODE3_11_7_14 (0x0042, 0x0331, 0x1E06),
  ENCODE3_11_7_14 (0x0043, 0x0301, 0x0106),ENCODE3_11_7_14 (0x0043, 0x0302, 0x0108),
  ENCODE3_11_7_14 (0x0043, 0x0307, 0x010A),ENCODE3_11_7_14 (0x0043, 0x030C, 0x010C),
  ENCODE3_11_7_14 (0x0043, 0x0327, 0x00C7),ENCODE3_11_7_14 (0x0044, 0x0307, 0x1E0A),
  ENCODE3_11_7_14 (0x0044, 0x030C, 0x010E),ENCODE3_11_7_14 (0x0044, 0x0323, 0x1E0C),
  ENCODE3_11_7_14 (0x0044, 0x0327, 0x1E10),ENCODE3_11_7_14 (0x0044, 0x032D, 0x1E12),
  ENCODE3_11_7_14 (0x0044, 0x0331, 0x1E0E),ENCODE3_11_7_14 (0x0045, 0x0300, 0x00C8),
  ENCODE3_11_7_14 (0x0045, 0x0301, 0x00C9),ENCODE3_11_7_14 (0x0045, 0x0302, 0x00CA),
  ENCODE3_11_7_14 (0x0045, 0x0303, 0x1EBC),ENCODE3_11_7_14 (0x0045, 0x0304, 0x0112),
  ENCODE3_11_7_14 (0x0045, 0x0306, 0x0114),ENCODE3_11_7_14 (0x0045, 0x0307, 0x0116),
  ENCODE3_11_7_14 (0x0045, 0x0308, 0x00CB),ENCODE3_11_7_14 (0x0045, 0x0309, 0x1EBA),
  ENCODE3_11_7_14 (0x0045, 0x030C, 0x011A),ENCODE3_11_7_14 (0x0045, 0x030F, 0x0204),
  ENCODE3_11_7_14 (0x0045, 0x0311, 0x0206),ENCODE3_11_7_14 (0x0045, 0x0323, 0x1EB8),
  ENCODE3_11_7_14 (0x0045, 0x0327, 0x0228),ENCODE3_11_7_14 (0x0045, 0x0328, 0x0118),
  ENCODE3_11_7_14 (0x0045, 0x032D, 0x1E18),ENCODE3_11_7_14 (0x0045, 0x0330, 0x1E1A),
  ENCODE3_11_7_14 (0x0046, 0x0307, 0x1E1E),ENCODE3_11_7_14 (0x0047, 0x0301, 0x01F4),
  ENCODE3_11_7_14 (0x0047, 0x0302, 0x011C),ENCODE3_11_7_14 (0x0047, 0x0304, 0x1E20),
  ENCODE3_11_7_14 (0x0047, 0x0306, 0x011E),ENCODE3_11_7_14 (0x0047, 0x0307, 0x0120),
  ENCODE3_11_7_14 (0x0047, 0x030C, 0x01E6),ENCODE3_11_7_14 (0x0047, 0x0327, 0x0122),
  ENCODE3_11_7_14 (0x0048, 0x0302, 0x0124),ENCODE3_11_7_14 (0x0048, 0x0307, 0x1E22),
  ENCODE3_11_7_14 (0x0048, 0x0308, 0x1E26),ENCODE3_11_7_14 (0x0048, 0x030C, 0x021E),
  ENCODE3_11_7_14 (0x0048, 0x0323, 0x1E24),ENCODE3_11_7_14 (0x0048, 0x0327, 0x1E28),
  ENCODE3_11_7_14 (0x0048, 0x032E, 0x1E2A),ENCODE3_11_7_14 (0x0049, 0x0300, 0x00CC),
  ENCODE3_11_7_14 (0x0049, 0x0301, 0x00CD),ENCODE3_11_7_14 (0x0049, 0x0302, 0x00CE),
  ENCODE3_11_7_14 (0x0049, 0x0303, 0x0128),ENCODE3_11_7_14 (0x0049, 0x0304, 0x012A),
  ENCODE3_11_7_14 (0x0049, 0x0306, 0x012C),ENCODE3_11_7_14 (0x0049, 0x0307, 0x0130),
  ENCODE3_11_7_14 (0x0049, 0x0308, 0x00CF),ENCODE3_11_7_14 (0x0049, 0x0309, 0x1EC8),
  ENCODE3_11_7_14 (0x0049, 0x030C, 0x01CF),ENCODE3_11_7_14 (0x0049, 0x030F, 0x0208),
  ENCODE3_11_7_14 (0x0049, 0x0311, 0x020A),ENCODE3_11_7_14 (0x0049, 0x0323, 0x1ECA),
  ENCODE3_11_7_14 (0x0049, 0x0328, 0x012E),ENCODE3_11_7_14 (0x0049, 0x0330, 0x1E2C),
  ENCODE3_11_7_14 (0x004A, 0x0302, 0x0134),ENCODE3_11_7_14 (0x004B, 0x0301, 0x1E30),
  ENCODE3_11_7_14 (0x004B, 0x030C, 0x01E8),ENCODE3_11_7_14 (0x004B, 0x0323, 0x1E32),
  ENCODE3_11_7_14 (0x004B, 0x0327, 0x0136),ENCODE3_11_7_14 (0x004B, 0x0331, 0x1E34),
  ENCODE3_11_7_14 (0x004C, 0x0301, 0x0139),ENCODE3_11_7_14 (0x004C, 0x030C, 0x013D),
  ENCODE3_11_7_14 (0x004C, 0x0323, 0x1E36),ENCODE3_11_7_14 (0x004C, 0x0327, 0x013B),
  ENCODE3_11_7_14 (0x004C, 0x032D, 0x1E3C),ENCODE3_11_7_14 (0x004C, 0x0331, 0x1E3A),
  ENCODE3_11_7_14 (0x004D, 0x0301, 0x1E3E),ENCODE3_11_7_14 (0x004D, 0x0307, 0x1E40),
  ENCODE3_11_7_14 (0x004D, 0x0323, 0x1E42),ENCODE3_11_7_14 (0x004E, 0x0300, 0x01F8),
  ENCODE3_11_7_14 (0x004E, 0x0301, 0x0143),ENCODE3_11_7_14 (0x004E, 0x0303, 0x00D1),
  ENCODE3_11_7_14 (0x004E, 0x0307, 0x1E44),ENCODE3_11_7_14 (0x004E, 0x030C, 0x0147),
  ENCODE3_11_7_14 (0x004E, 0x0323, 0x1E46),ENCODE3_11_7_14 (0x004E, 0x0327, 0x0145),
  ENCODE3_11_7_14 (0x004E, 0x032D, 0x1E4A),ENCODE3_11_7_14 (0x004E, 0x0331, 0x1E48),
  ENCODE3_11_7_14 (0x004F, 0x0300, 0x00D2),ENCODE3_11_7_14 (0x004F, 0x0301, 0x00D3),
  ENCODE3_11_7_14 (0x004F, 0x0302, 0x00D4),ENCODE3_11_7_14 (0x004F, 0x0303, 0x00D5),
  ENCODE3_11_7_14 (0x004F, 0x0304, 0x014C),ENCODE3_11_7_14 (0x004F, 0x0306, 0x014E),
  ENCODE3_11_7_14 (0x004F, 0x0307, 0x022E),ENCODE3_11_7_14 (0x004F, 0x0308, 0x00D6),
  ENCODE3_11_7_14 (0x004F, 0x0309, 0x1ECE),ENCODE3_11_7_14 (0x004F, 0x030B, 0x0150),
  ENCODE3_11_7_14 (0x004F, 0x030C, 0x01D1),ENCODE3_11_7_14 (0x004F, 0x030F, 0x020C),
  ENCODE3_11_7_14 (0x004F, 0x0311, 0x020E),ENCODE3_11_7_14 (0x004F, 0x031B, 0x01A0),
  ENCODE3_11_7_14 (0x004F, 0x0323, 0x1ECC),ENCODE3_11_7_14 (0x004F, 0x0328, 0x01EA),
  ENCODE3_11_7_14 (0x0050, 0x0301, 0x1E54),ENCODE3_11_7_14 (0x0050, 0x0307, 0x1E56),
  ENCODE3_11_7_14 (0x0052, 0x0301, 0x0154),ENCODE3_11_7_14 (0x0052, 0x0307, 0x1E58),
  ENCODE3_11_7_14 (0x0052, 0x030C, 0x0158),ENCODE3_11_7_14 (0x0052, 0x030F, 0x0210),
  ENCODE3_11_7_14 (0x0052, 0x0311, 0x0212),ENCODE3_11_7_14 (0x0052, 0x0323, 0x1E5A),
  ENCODE3_11_7_14 (0x0052, 0x0327, 0x0156),ENCODE3_11_7_14 (0x0052, 0x0331, 0x1E5E),
  ENCODE3_11_7_14 (0x0053, 0x0301, 0x015A),ENCODE3_11_7_14 (0x0053, 0x0302, 0x015C),
  ENCODE3_11_7_14 (0x0053, 0x0307, 0x1E60),ENCODE3_11_7_14 (0x0053, 0x030C, 0x0160),
  ENCODE3_11_7_14 (0x0053, 0x0323, 0x1E62),ENCODE3_11_7_14 (0x0053, 0x0326, 0x0218),
  ENCODE3_11_7_14 (0x0053, 0x0327, 0x015E),ENCODE3_11_7_14 (0x0054, 0x0307, 0x1E6A),
  ENCODE3_11_7_14 (0x0054, 0x030C, 0x0164),ENCODE3_11_7_14 (0x0054, 0x0323, 0x1E6C),
  ENCODE3_11_7_14 (0x0054, 0x0326, 0x021A),ENCODE3_11_7_14 (0x0054, 0x0327, 0x0162),
  ENCODE3_11_7_14 (0x0054, 0x032D, 0x1E70),ENCODE3_11_7_14 (0x0054, 0x0331, 0x1E6E),
  ENCODE3_11_7_14 (0x0055, 0x0300, 0x00D9),ENCODE3_11_7_14 (0x0055, 0x0301, 0x00DA),
  ENCODE3_11_7_14 (0x0055, 0x0302, 0x00DB),ENCODE3_11_7_14 (0x0055, 0x0303, 0x0168),
  ENCODE3_11_7_14 (0x0055, 0x0304, 0x016A),ENCODE3_11_7_14 (0x0055, 0x0306, 0x016C),
  ENCODE3_11_7_14 (0x0055, 0x0308, 0x00DC),ENCODE3_11_7_14 (0x0055, 0x0309, 0x1EE6),
  ENCODE3_11_7_14 (0x0055, 0x030A, 0x016E),ENCODE3_11_7_14 (0x0055, 0x030B, 0x0170),
  ENCODE3_11_7_14 (0x0055, 0x030C, 0x01D3),ENCODE3_11_7_14 (0x0055, 0x030F, 0x0214),
  ENCODE3_11_7_14 (0x0055, 0x0311, 0x0216),ENCODE3_11_7_14 (0x0055, 0x031B, 0x01AF),
  ENCODE3_11_7_14 (0x0055, 0x0323, 0x1EE4),ENCODE3_11_7_14 (0x0055, 0x0324, 0x1E72),
  ENCODE3_11_7_14 (0x0055, 0x0328, 0x0172),ENCODE3_11_7_14 (0x0055, 0x032D, 0x1E76),
  ENCODE3_11_7_14 (0x0055, 0x0330, 0x1E74),ENCODE3_11_7_14 (0x0056, 0x0303, 0x1E7C),
  ENCODE3_11_7_14 (0x0056, 0x0323, 0x1E7E),ENCODE3_11_7_14 (0x0057, 0x0300, 0x1E80),
  ENCODE3_11_7_14 (0x0057, 0x0301, 0x1E82),ENCODE3_11_7_14 (0x0057, 0x0302, 0x0174),
  ENCODE3_11_7_14 (0x0057, 0x0307, 0x1E86),ENCODE3_11_7_14 (0x0057, 0x0308, 0x1E84),
  ENCODE3_11_7_14 (0x0057, 0x0323, 0x1E88),ENCODE3_11_7_14 (0x0058, 0x0307, 0x1E8A),
  ENCODE3_11_7_14 (0x0058, 0x0308, 0x1E8C),ENCODE3_11_7_14 (0x0059, 0x0300, 0x1EF2),
  ENCODE3_11_7_14 (0x0059, 0x0301, 0x00DD),ENCODE3_11_7_14 (0x0059, 0x0302, 0x0176),
  ENCODE3_11_7_14 (0x0059, 0x0303, 0x1EF8),ENCODE3_11_7_14 (0x0059, 0x0304, 0x0232),
  ENCODE3_11_7_14 (0x0059, 0x0307, 0x1E8E),ENCODE3_11_7_14 (0x0059, 0x0308, 0x0178),
  ENCODE3_11_7_14 (0x0059, 0x0309, 0x1EF6),ENCODE3_11_7_14 (0x0059, 0x0323, 0x1EF4),
  ENCODE3_11_7_14 (0x005A, 0x0301, 0x0179),ENCODE3_11_7_14 (0x005A, 0x0302, 0x1E90),
  ENCODE3_11_7_14 (0x005A, 0x0307, 0x017B),ENCODE3_11_7_14 (0x005A, 0x030C, 0x017D),
  ENCODE3_11_7_14 (0x005A, 0x0323, 0x1E92),ENCODE3_11_7_14 (0x005A, 0x0331, 0x1E94),
  ENCODE3_11_7_14 (0x0061, 0x0300, 0x00E0),ENCODE3_11_7_14 (0x0061, 0x0301, 0x00E1),
  ENCODE3_11_7_14 (0x0061, 0x0302, 0x00E2),ENCODE3_11_7_14 (0x0061, 0x0303, 0x00E3),
  ENCODE3_11_7_14 (0x0061, 0x0304, 0x0101),ENCODE3_11_7_14 (0x0061, 0x0306, 0x0103),
  ENCODE3_11_7_14 (0x0061, 0x0307, 0x0227),ENCODE3_11_7_14 (0x0061, 0x0308, 0x00E4),
  ENCODE3_11_7_14 (0x0061, 0x0309, 0x1EA3),ENCODE3_11_7_14 (0x0061, 0x030A, 0x00E5),
  ENCODE3_11_7_14 (0x0061, 0x030C, 0x01CE),ENCODE3_11_7_14 (0x0061, 0x030F, 0x0201),
  ENCODE3_11_7_14 (0x0061, 0x0311, 0x0203),ENCODE3_11_7_14 (0x0061, 0x0323, 0x1EA1),
  ENCODE3_11_7_14 (0x0061, 0x0325, 0x1E01),ENCODE3_11_7_14 (0x0061, 0x0328, 0x0105),
  ENCODE3_11_7_14 (0x0062, 0x0307, 0x1E03),ENCODE3_11_7_14 (0x0062, 0x0323, 0x1E05),
  ENCODE3_11_7_14 (0x0062, 0x0331, 0x1E07),ENCODE3_11_7_14 (0x0063, 0x0301, 0x0107),
  ENCODE3_11_7_14 (0x0063, 0x0302, 0x0109),ENCODE3_11_7_14 (0x0063, 0x0307, 0x010B),
  ENCODE3_11_7_14 (0x0063, 0x030C, 0x010D),ENCODE3_11_7_14 (0x0063, 0x0327, 0x00E7),
  ENCODE3_11_7_14 (0x0064, 0x0307, 0x1E0B),ENCODE3_11_7_14 (0x0064, 0x030C, 0x010F),
  ENCODE3_11_7_14 (0x0064, 0x0323, 0x1E0D),ENCODE3_11_7_14 (0x0064, 0x0327, 0x1E11),
  ENCODE3_11_7_14 (0x0064, 0x032D, 0x1E13),ENCODE3_11_7_14 (0x0064, 0x0331, 0x1E0F),
  ENCODE3_11_7_14 (0x0065, 0x0300, 0x00E8),ENCODE3_11_7_14 (0x0065, 0x0301, 0x00E9),
  ENCODE3_11_7_14 (0x0065, 0x0302, 0x00EA),ENCODE3_11_7_14 (0x0065, 0x0303, 0x1EBD),
  ENCODE3_11_7_14 (0x0065, 0x0304, 0x0113),ENCODE3_11_7_14 (0x0065, 0x0306, 0x0115),
  ENCODE3_11_7_14 (0x0065, 0x0307, 0x0117),ENCODE3_11_7_14 (0x0065, 0x0308, 0x00EB),
  ENCODE3_11_7_14 (0x0065, 0x0309, 0x1EBB),ENCODE3_11_7_14 (0x0065, 0x030C, 0x011B),
  ENCODE3_11_7_14 (0x0065, 0x030F, 0x0205),ENCODE3_11_7_14 (0x0065, 0x0311, 0x0207),
  ENCODE3_11_7_14 (0x0065, 0x0323, 0x1EB9),ENCODE3_11_7_14 (0x0065, 0x0327, 0x0229),
  ENCODE3_11_7_14 (0x0065, 0x0328, 0x0119),ENCODE3_11_7_14 (0x0065, 0x032D, 0x1E19),
  ENCODE3_11_7_14 (0x0065, 0x0330, 0x1E1B),ENCODE3_11_7_14 (0x0066, 0x0307, 0x1E1F),
  ENCODE3_11_7_14 (0x0067, 0x0301, 0x01F5),ENCODE3_11_7_14 (0x0067, 0x0302, 0x011D),
  ENCODE3_11_7_14 (0x0067, 0x0304, 0x1E21),ENCODE3_11_7_14 (0x0067, 0x0306, 0x011F),
  ENCODE3_11_7_14 (0x0067, 0x0307, 0x0121),ENCODE3_11_7_14 (0x0067, 0x030C, 0x01E7),
  ENCODE3_11_7_14 (0x0067, 0x0327, 0x0123),ENCODE3_11_7_14 (0x0068, 0x0302, 0x0125),
  ENCODE3_11_7_14 (0x0068, 0x0307, 0x1E23),ENCODE3_11_7_14 (0x0068, 0x0308, 0x1E27),
  ENCODE3_11_7_14 (0x0068, 0x030C, 0x021F),ENCODE3_11_7_14 (0x0068, 0x0323, 0x1E25),
  ENCODE3_11_7_14 (0x0068, 0x0327, 0x1E29),ENCODE3_11_7_14 (0x0068, 0x032E, 0x1E2B),
  ENCODE3_11_7_14 (0x0068, 0x0331, 0x1E96),ENCODE3_11_7_14 (0x0069, 0x0300, 0x00EC),
  ENCODE3_11_7_14 (0x0069, 0x0301, 0x00ED),ENCODE3_11_7_14 (0x0069, 0x0302, 0x00EE),
  ENCODE3_11_7_14 (0x0069, 0x0303, 0x0129),ENCODE3_11_7_14 (0x0069, 0x0304, 0x012B),
  ENCODE3_11_7_14 (0x0069, 0x0306, 0x012D),ENCODE3_11_7_14 (0x0069, 0x0308, 0x00EF),
  ENCODE3_11_7_14 (0x0069, 0x0309, 0x1EC9),ENCODE3_11_7_14 (0x0069, 0x030C, 0x01D0),
  ENCODE3_11_7_14 (0x0069, 0x030F, 0x0209),ENCODE3_11_7_14 (0x0069, 0x0311, 0x020B),
  ENCODE3_11_7_14 (0x0069, 0x0323, 0x1ECB),ENCODE3_11_7_14 (0x0069, 0x0328, 0x012F),
  ENCODE3_11_7_14 (0x0069, 0x0330, 0x1E2D),ENCODE3_11_7_14 (0x006A, 0x0302, 0x0135),
  ENCODE3_11_7_14 (0x006A, 0x030C, 0x01F0),ENCODE3_11_7_14 (0x006B, 0x0301, 0x1E31),
  ENCODE3_11_7_14 (0x006B, 0x030C, 0x01E9),ENCODE3_11_7_14 (0x006B, 0x0323, 0x1E33),
  ENCODE3_11_7_14 (0x006B, 0x0327, 0x0137),ENCODE3_11_7_14 (0x006B, 0x0331, 0x1E35),
  ENCODE3_11_7_14 (0x006C, 0x0301, 0x013A),ENCODE3_11_7_14 (0x006C, 0x030C, 0x013E),
  ENCODE3_11_7_14 (0x006C, 0x0323, 0x1E37),ENCODE3_11_7_14 (0x006C, 0x0327, 0x013C),
  ENCODE3_11_7_14 (0x006C, 0x032D, 0x1E3D),ENCODE3_11_7_14 (0x006C, 0x0331, 0x1E3B),
  ENCODE3_11_7_14 (0x006D, 0x0301, 0x1E3F),ENCODE3_11_7_14 (0x006D, 0x0307, 0x1E41),
  ENCODE3_11_7_14 (0x006D, 0x0323, 0x1E43),ENCODE3_11_7_14 (0x006E, 0x0300, 0x01F9),
  ENCODE3_11_7_14 (0x006E, 0x0301, 0x0144),ENCODE3_11_7_14 (0x006E, 0x0303, 0x00F1),
  ENCODE3_11_7_14 (0x006E, 0x0307, 0x1E45),ENCODE3_11_7_14 (0x006E, 0x030C, 0x0148),
  ENCODE3_11_7_14 (0x006E, 0x0323, 0x1E47),ENCODE3_11_7_14 (0x006E, 0x0327, 0x0146),
  ENCODE3_11_7_14 (0x006E, 0x032D, 0x1E4B),ENCODE3_11_7_14 (0x006E, 0x0331, 0x1E49),
  ENCODE3_11_7_14 (0x006F, 0x0300, 0x00F2),ENCODE3_11_7_14 (0x006F, 0x0301, 0x00F3),
  ENCODE3_11_7_14 (0x006F, 0x0302, 0x00F4),ENCODE3_11_7_14 (0x006F, 0x0303, 0x00F5),
  ENCODE3_11_7_14 (0x006F, 0x0304, 0x014D),ENCODE3_11_7_14 (0x006F, 0x0306, 0x014F),
  ENCODE3_11_7_14 (0x006F, 0x0307, 0x022F),ENCODE3_11_7_14 (0x006F, 0x0308, 0x00F6),
  ENCODE3_11_7_14 (0x006F, 0x0309, 0x1ECF),ENCODE3_11_7_14 (0x006F, 0x030B, 0x0151),
  ENCODE3_11_7_14 (0x006F, 0x030C, 0x01D2),ENCODE3_11_7_14 (0x006F, 0x030F, 0x020D),
  ENCODE3_11_7_14 (0x006F, 0x0311, 0x020F),ENCODE3_11_7_14 (0x006F, 0x031B, 0x01A1),
  ENCODE3_11_7_14 (0x006F, 0x0323, 0x1ECD),ENCODE3_11_7_14 (0x006F, 0x0328, 0x01EB),
  ENCODE3_11_7_14 (0x0070, 0x0301, 0x1E55),ENCODE3_11_7_14 (0x0070, 0x0307, 0x1E57),
  ENCODE3_11_7_14 (0x0072, 0x0301, 0x0155),ENCODE3_11_7_14 (0x0072, 0x0307, 0x1E59),
  ENCODE3_11_7_14 (0x0072, 0x030C, 0x0159),ENCODE3_11_7_14 (0x0072, 0x030F, 0x0211),
  ENCODE3_11_7_14 (0x0072, 0x0311, 0x0213),ENCODE3_11_7_14 (0x0072, 0x0323, 0x1E5B),
  ENCODE3_11_7_14 (0x0072, 0x0327, 0x0157),ENCODE3_11_7_14 (0x0072, 0x0331, 0x1E5F),
  ENCODE3_11_7_14 (0x0073, 0x0301, 0x015B),ENCODE3_11_7_14 (0x0073, 0x0302, 0x015D),
  ENCODE3_11_7_14 (0x0073, 0x0307, 0x1E61),ENCODE3_11_7_14 (0x0073, 0x030C, 0x0161),
  ENCODE3_11_7_14 (0x0073, 0x0323, 0x1E63),ENCODE3_11_7_14 (0x0073, 0x0326, 0x0219),
  ENCODE3_11_7_14 (0x0073, 0x0327, 0x015F),ENCODE3_11_7_14 (0x0074, 0x0307, 0x1E6B),
  ENCODE3_11_7_14 (0x0074, 0x0308, 0x1E97),ENCODE3_11_7_14 (0x0074, 0x030C, 0x0165),
  ENCODE3_11_7_14 (0x0074, 0x0323, 0x1E6D),ENCODE3_11_7_14 (0x0074, 0x0326, 0x021B),
  ENCODE3_11_7_14 (0x0074, 0x0327, 0x0163),ENCODE3_11_7_14 (0x0074, 0x032D, 0x1E71),
  ENCODE3_11_7_14 (0x0074, 0x0331, 0x1E6F),ENCODE3_11_7_14 (0x0075, 0x0300, 0x00F9),
  ENCODE3_11_7_14 (0x0075, 0x0301, 0x00FA),ENCODE3_11_7_14 (0x0075, 0x0302, 0x00FB),
  ENCODE3_11_7_14 (0x0075, 0x0303, 0x0169),ENCODE3_11_7_14 (0x0075, 0x0304, 0x016B),
  ENCODE3_11_7_14 (0x0075, 0x0306, 0x016D),ENCODE3_11_7_14 (0x0075, 0x0308, 0x00FC),
  ENCODE3_11_7_14 (0x0075, 0x0309, 0x1EE7),ENCODE3_11_7_14 (0x0075, 0x030A, 0x016F),
  ENCODE3_11_7_14 (0x0075, 0x030B, 0x0171),ENCODE3_11_7_14 (0x0075, 0x030C, 0x01D4),
  ENCODE3_11_7_14 (0x0075, 0x030F, 0x0215),ENCODE3_11_7_14 (0x0075, 0x0311, 0x0217),
  ENCODE3_11_7_14 (0x0075, 0x031B, 0x01B0),ENCODE3_11_7_14 (0x0075, 0x0323, 0x1EE5),
  ENCODE3_11_7_14 (0x0075, 0x0324, 0x1E73),ENCODE3_11_7_14 (0x0075, 0x0328, 0x0173),
  ENCODE3_11_7_14 (0x0075, 0x032D, 0x1E77),ENCODE3_11_7_14 (0x0075, 0x0330, 0x1E75),
  ENCODE3_11_7_14 (0x0076, 0x0303, 0x1E7D),ENCODE3_11_7_14 (0x0076, 0x0323, 0x1E7F),
  ENCODE3_11_7_14 (0x0077, 0x0300, 0x1E81),ENCODE3_11_7_14 (0x0077, 0x0301, 0x1E83),
  ENCODE3_11_7_14 (0x0077, 0x0302, 0x0175),ENCODE3_11_7_14 (0x0077, 0x0307, 0x1E87),
  ENCODE3_11_7_14 (0x0077, 0x0308, 0x1E85),ENCODE3_11_7_14 (0x0077, 0x030A, 0x1E98),
  ENCODE3_11_7_14 (0x0077, 0x0323, 0x1E89),ENCODE3_11_7_14 (0x0078, 0x0307, 0x1E8B),
  ENCODE3_11_7_14 (0x0078, 0x0308, 0x1E8D),ENCODE3_11_7_14 (0x0079, 0x0300, 0x1EF3),
  ENCODE3_11_7_14 (0x0079, 0x0301, 0x00FD),ENCODE3_11_7_14 (0x0079, 0x0302, 0x0177),
  ENCODE3_11_7_14 (0x0079, 0x0303, 0x1EF9),ENCODE3_11_7_14 (0x0079, 0x0304, 0x0233),
  ENCODE3_11_7_14 (0x0079, 0x0307, 0x1E8F),ENCODE3_11_7_14 (0x0079, 0x0308, 0x00FF),
  ENCODE3_11_7_14 (0x0079, 0x0309, 0x1EF7),ENCODE3_11_7_14 (0x0079, 0x030A, 0x1E99),
  ENCODE3_11_7_14 (0x0079, 0x0323, 0x1EF5),ENCODE3_11_7_14 (0x007A, 0x0301, 0x017A),
  ENCODE3_11_7_14 (0x007A, 0x0302, 0x1E91),ENCODE3_11_7_14 (0x007A, 0x0307, 0x017C),
  ENCODE3_11_7_14 (0x007A, 0x030C, 0x017E),ENCODE3_11_7_14 (0x007A, 0x0323, 0x1E93),
  ENCODE3_11_7_14 (0x007A, 0x0331, 0x1E95),ENCODE3_11_7_14 (0x00A8, 0x0300, 0x1FED),
  ENCODE3_11_7_14 (0x00A8, 0x0301, 0x0385),ENCODE3_11_7_14 (0x00A8, 0x0342, 0x1FC1),
  ENCODE3_11_7_14 (0x00C2, 0x0300, 0x1EA6),ENCODE3_11_7_14 (0x00C2, 0x0301, 0x1EA4),
  ENCODE3_11_7_14 (0x00C2, 0x0303, 0x1EAA),ENCODE3_11_7_14 (0x00C2, 0x0309, 0x1EA8),
  ENCODE3_11_7_14 (0x00C4, 0x0304, 0x01DE),ENCODE3_11_7_14 (0x00C5, 0x0301, 0x01FA),
  ENCODE3_11_7_14 (0x00C6, 0x0301, 0x01FC),ENCODE3_11_7_14 (0x00C6, 0x0304, 0x01E2),
  ENCODE3_11_7_14 (0x00C7, 0x0301, 0x1E08),ENCODE3_11_7_14 (0x00CA, 0x0300, 0x1EC0),
  ENCODE3_11_7_14 (0x00CA, 0x0301, 0x1EBE),ENCODE3_11_7_14 (0x00CA, 0x0303, 0x1EC4),
  ENCODE3_11_7_14 (0x00CA, 0x0309, 0x1EC2),ENCODE3_11_7_14 (0x00CF, 0x0301, 0x1E2E),
  ENCODE3_11_7_14 (0x00D4, 0x0300, 0x1ED2),ENCODE3_11_7_14 (0x00D4, 0x0301, 0x1ED0),
  ENCODE3_11_7_14 (0x00D4, 0x0303, 0x1ED6),ENCODE3_11_7_14 (0x00D4, 0x0309, 0x1ED4),
  ENCODE3_11_7_14 (0x00D5, 0x0301, 0x1E4C),ENCODE3_11_7_14 (0x00D5, 0x0304, 0x022C),
  ENCODE3_11_7_14 (0x00D5, 0x0308, 0x1E4E),ENCODE3_11_7_14 (0x00D6, 0x0304, 0x022A),
  ENCODE3_11_7_14 (0x00D8, 0x0301, 0x01FE),ENCODE3_11_7_14 (0x00DC, 0x0300, 0x01DB),
  ENCODE3_11_7_14 (0x00DC, 0x0301, 0x01D7),ENCODE3_11_7_14 (0x00DC, 0x0304, 0x01D5),
  ENCODE3_11_7_14 (0x00DC, 0x030C, 0x01D9),ENCODE3_11_7_14 (0x00E2, 0x0300, 0x1EA7),
  ENCODE3_11_7_14 (0x00E2, 0x0301, 0x1EA5),ENCODE3_11_7_14 (0x00E2, 0x0303, 0x1EAB),
  ENCODE3_11_7_14 (0x00E2, 0x0309, 0x1EA9),ENCODE3_11_7_14 (0x00E4, 0x0304, 0x01DF),
  ENCODE3_11_7_14 (0x00E5, 0x0301, 0x01FB),ENCODE3_11_7_14 (0x00E6, 0x0301, 0x01FD),
  ENCODE3_11_7_14 (0x00E6, 0x0304, 0x01E3),ENCODE3_11_7_14 (0x00E7, 0x0301, 0x1E09),
  ENCODE3_11_7_14 (0x00EA, 0x0300, 0x1EC1),ENCODE3_11_7_14 (0x00EA, 0x0301, 0x1EBF),
  ENCODE3_11_7_14 (0x00EA, 0x0303, 0x1EC5),ENCODE3_11_7_14 (0x00EA, 0x0309, 0x1EC3),
  ENCODE3_11_7_14 (0x00EF, 0x0301, 0x1E2F),ENCODE3_11_7_14 (0x00F4, 0x0300, 0x1ED3),
  ENCODE3_11_7_14 (0x00F4, 0x0301, 0x1ED1),ENCODE3_11_7_14 (0x00F4, 0x0303, 0x1ED7),
  ENCODE3_11_7_14 (0x00F4, 0x0309, 0x1ED5),ENCODE3_11_7_14 (0x00F5, 0x0301, 0x1E4D),
  ENCODE3_11_7_14 (0x00F5, 0x0304, 0x022D),ENCODE3_11_7_14 (0x00F5, 0x0308, 0x1E4F),
  ENCODE3_11_7_14 (0x00F6, 0x0304, 0x022B),ENCODE3_11_7_14 (0x00F8, 0x0301, 0x01FF),
  ENCODE3_11_7_14 (0x00FC, 0x0300, 0x01DC),ENCODE3_11_7_14 (0x00FC, 0x0301, 0x01D8),
  ENCODE3_11_7_14 (0x00FC, 0x0304, 0x01D6),ENCODE3_11_7_14 (0x00FC, 0x030C, 0x01DA),
  ENCODE3_11_7_14 (0x0102, 0x0300, 0x1EB0),ENCODE3_11_7_14 (0x0102, 0x0301, 0x1EAE),
  ENCODE3_11_7_14 (0x0102, 0x0303, 0x1EB4),ENCODE3_11_7_14 (0x0102, 0x0309, 0x1EB2),
  ENCODE3_11_7_14 (0x0103, 0x0300, 0x1EB1),ENCODE3_11_7_14 (0x0103, 0x0301, 0x1EAF),
  ENCODE3_11_7_14 (0x0103, 0x0303, 0x1EB5),ENCODE3_11_7_14 (0x0103, 0x0309, 0x1EB3),
  ENCODE3_11_7_14 (0x0112, 0x0300, 0x1E14),ENCODE3_11_7_14 (0x0112, 0x0301, 0x1E16),
  ENCODE3_11_7_14 (0x0113, 0x0300, 0x1E15),ENCODE3_11_7_14 (0x0113, 0x0301, 0x1E17),
  ENCODE3_11_7_14 (0x014C, 0x0300, 0x1E50),ENCODE3_11_7_14 (0x014C, 0x0301, 0x1E52),
  ENCODE3_11_7_14 (0x014D, 0x0300, 0x1E51),ENCODE3_11_7_14 (0x014D, 0x0301, 0x1E53),
  ENCODE3_11_7_14 (0x015A, 0x0307, 0x1E64),ENCODE3_11_7_14 (0x015B, 0x0307, 0x1E65),
  ENCODE3_11_7_14 (0x0160, 0x0307, 0x1E66),ENCODE3_11_7_14 (0x0161, 0x0307, 0x1E67),
  ENCODE3_11_7_14 (0x0168, 0x0301, 0x1E78),ENCODE3_11_7_14 (0x0169, 0x0301, 0x1E79),
  ENCODE3_11_7_14 (0x016A, 0x0308, 0x1E7A),ENCODE3_11_7_14 (0x016B, 0x0308, 0x1E7B),
  ENCODE3_11_7_14 (0x017F, 0x0307, 0x1E9B),ENCODE3_11_7_14 (0x01A0, 0x0300, 0x1EDC),
  ENCODE3_11_7_14 (0x01A0, 0x0301, 0x1EDA),ENCODE3_11_7_14 (0x01A0, 0x0303, 0x1EE0),
  ENCODE3_11_7_14 (0x01A0, 0x0309, 0x1EDE),ENCODE3_11_7_14 (0x01A0, 0x0323, 0x1EE2),
  ENCODE3_11_7_14 (0x01A1, 0x0300, 0x1EDD),ENCODE3_11_7_14 (0x01A1, 0x0301, 0x1EDB),
  ENCODE3_11_7_14 (0x01A1, 0x0303, 0x1EE1),ENCODE3_11_7_14 (0x01A1, 0x0309, 0x1EDF),
  ENCODE3_11_7_14 (0x01A1, 0x0323, 0x1EE3),ENCODE3_11_7_14 (0x01AF, 0x0300, 0x1EEA),
  ENCODE3_11_7_14 (0x01AF, 0x0301, 0x1EE8),ENCODE3_11_7_14 (0x01AF, 0x0303, 0x1EEE),
  ENCODE3_11_7_14 (0x01AF, 0x0309, 0x1EEC),ENCODE3_11_7_14 (0x01AF, 0x0323, 0x1EF0),
  ENCODE3_11_7_14 (0x01B0, 0x0300, 0x1EEB),ENCODE3_11_7_14 (0x01B0, 0x0301, 0x1EE9),
  ENCODE3_11_7_14 (0x01B0, 0x0303, 0x1EEF),ENCODE3_11_7_14 (0x01B0, 0x0309, 0x1EED),
  ENCODE3_11_7_14 (0x01B0, 0x0323, 0x1EF1),ENCODE3_11_7_14 (0x01B7, 0x030C, 0x01EE),
  ENCODE3_11_7_14 (0x01EA, 0x0304, 0x01EC),ENCODE3_11_7_14 (0x01EB, 0x0304, 0x01ED),
  ENCODE3_11_7_14 (0x0226, 0x0304, 0x01E0),ENCODE3_11_7_14 (0x0227, 0x0304, 0x01E1),
  ENCODE3_11_7_14 (0x0228, 0x0306, 0x1E1C),ENCODE3_11_7_14 (0x0229, 0x0306, 0x1E1D),
  ENCODE3_11_7_14 (0x022E, 0x0304, 0x0230),ENCODE3_11_7_14 (0x022F, 0x0304, 0x0231),
  ENCODE3_11_7_14 (0x0292, 0x030C, 0x01EF),ENCODE3_11_7_14 (0x0308, 0x0301, 0x0000),
  ENCODE3_11_7_14 (0x0391, 0x0300, 0x1FBA),ENCODE3_11_7_14 (0x0391, 0x0301, 0x0386),
  ENCODE3_11_7_14 (0x0391, 0x0304, 0x1FB9),ENCODE3_11_7_14 (0x0391, 0x0306, 0x1FB8),
  ENCODE3_11_7_14 (0x0391, 0x0313, 0x1F08),ENCODE3_11_7_14 (0x0391, 0x0314, 0x1F09),
  ENCODE3_11_7_14 (0x0391, 0x0345, 0x1FBC),ENCODE3_11_7_14 (0x0395, 0x0300, 0x1FC8),
  ENCODE3_11_7_14 (0x0395, 0x0301, 0x0388),ENCODE3_11_7_14 (0x0395, 0x0313, 0x1F18),
  ENCODE3_11_7_14 (0x0395, 0x0314, 0x1F19),ENCODE3_11_7_14 (0x0397, 0x0300, 0x1FCA),
  ENCODE3_11_7_14 (0x0397, 0x0301, 0x0389),ENCODE3_11_7_14 (0x0397, 0x0313, 0x1F28),
  ENCODE3_11_7_14 (0x0397, 0x0314, 0x1F29),ENCODE3_11_7_14 (0x0397, 0x0345, 0x1FCC),
  ENCODE3_11_7_14 (0x0399, 0x0300, 0x1FDA),ENCODE3_11_7_14 (0x0399, 0x0301, 0x038A),
  ENCODE3_11_7_14 (0x0399, 0x0304, 0x1FD9),ENCODE3_11_7_14 (0x0399, 0x0306, 0x1FD8),
  ENCODE3_11_7_14 (0x0399, 0x0308, 0x03AA),ENCODE3_11_7_14 (0x0399, 0x0313, 0x1F38),
  ENCODE3_11_7_14 (0x0399, 0x0314, 0x1F39),ENCODE3_11_7_14 (0x039F, 0x0300, 0x1FF8),
  ENCODE3_11_7_14 (0x039F, 0x0301, 0x038C),ENCODE3_11_7_14 (0x039F, 0x0313, 0x1F48),
  ENCODE3_11_7_14 (0x039F, 0x0314, 0x1F49),ENCODE3_11_7_14 (0x03A1, 0x0314, 0x1FEC),
  ENCODE3_11_7_14 (0x03A5, 0x0300, 0x1FEA),ENCODE3_11_7_14 (0x03A5, 0x0301, 0x038E),
  ENCODE3_11_7_14 (0x03A5, 0x0304, 0x1FE9),ENCODE3_11_7_14 (0x03A5, 0x0306, 0x1FE8),
  ENCODE3_11_7_14 (0x03A5, 0x0308, 0x03AB),ENCODE3_11_7_14 (0x03A5, 0x0314, 0x1F59),
  ENCODE3_11_7_14 (0x03A9, 0x0300, 0x1FFA),ENCODE3_11_7_14 (0x03A9, 0x0301, 0x038F),
  ENCODE3_11_7_14 (0x03A9, 0x0313, 0x1F68),ENCODE3_11_7_14 (0x03A9, 0x0314, 0x1F69),
  ENCODE3_11_7_14 (0x03A9, 0x0345, 0x1FFC),ENCODE3_11_7_14 (0x03AC, 0x0345, 0x1FB4),
  ENCODE3_11_7_14 (0x03AE, 0x0345, 0x1FC4),ENCODE3_11_7_14 (0x03B1, 0x0300, 0x1F70),
  ENCODE3_11_7_14 (0x03B1, 0x0301, 0x03AC),ENCODE3_11_7_14 (0x03B1, 0x0304, 0x1FB1),
  ENCODE3_11_7_14 (0x03B1, 0x0306, 0x1FB0),ENCODE3_11_7_14 (0x03B1, 0x0313, 0x1F00),
  ENCODE3_11_7_14 (0x03B1, 0x0314, 0x1F01),ENCODE3_11_7_14 (0x03B1, 0x0342, 0x1FB6),
  ENCODE3_11_7_14 (0x03B1, 0x0345, 0x1FB3),ENCODE3_11_7_14 (0x03B5, 0x0300, 0x1F72),
  ENCODE3_11_7_14 (0x03B5, 0x0301, 0x03AD),ENCODE3_11_7_14 (0x03B5, 0x0313, 0x1F10),
  ENCODE3_11_7_14 (0x03B5, 0x0314, 0x1F11),ENCODE3_11_7_14 (0x03B7, 0x0300, 0x1F74),
  ENCODE3_11_7_14 (0x03B7, 0x0301, 0x03AE),ENCODE3_11_7_14 (0x03B7, 0x0313, 0x1F20),
  ENCODE3_11_7_14 (0x03B7, 0x0314, 0x1F21),ENCODE3_11_7_14 (0x03B7, 0x0342, 0x1FC6),
  ENCODE3_11_7_14 (0x03B7, 0x0345, 0x1FC3),ENCODE3_11_7_14 (0x03B9, 0x0300, 0x1F76),
  ENCODE3_11_7_14 (0x03B9, 0x0301, 0x03AF),ENCODE3_11_7_14 (0x03B9, 0x0304, 0x1FD1),
  ENCODE3_11_7_14 (0x03B9, 0x0306, 0x1FD0),ENCODE3_11_7_14 (0x03B9, 0x0308, 0x03CA),
  ENCODE3_11_7_14 (0x03B9, 0x0313, 0x1F30),ENCODE3_11_7_14 (0x03B9, 0x0314, 0x1F31),
  ENCODE3_11_7_14 (0x03B9, 0x0342, 0x1FD6),ENCODE3_11_7_14 (0x03BF, 0x0300, 0x1F78),
  ENCODE3_11_7_14 (0x03BF, 0x0301, 0x03CC),ENCODE3_11_7_14 (0x03BF, 0x0313, 0x1F40),
  ENCODE3_11_7_14 (0x03BF, 0x0314, 0x1F41),ENCODE3_11_7_14 (0x03C1, 0x0313, 0x1FE4),
  ENCODE3_11_7_14 (0x03C1, 0x0314, 0x1FE5),ENCODE3_11_7_14 (0x03C5, 0x0300, 0x1F7A),
  ENCODE3_11_7_14 (0x03C5, 0x0301, 0x03CD),ENCODE3_11_7_14 (0x03C5, 0x0304, 0x1FE1),
  ENCODE3_11_7_14 (0x03C5, 0x0306, 0x1FE0),ENCODE3_11_7_14 (0x03C5, 0x0308, 0x03CB),
  ENCODE3_11_7_14 (0x03C5, 0x0313, 0x1F50),ENCODE3_11_7_14 (0x03C5, 0x0314, 0x1F51),
  ENCODE3_11_7_14 (0x03C5, 0x0342, 0x1FE6),ENCODE3_11_7_14 (0x03C9, 0x0300, 0x1F7C),
  ENCODE3_11_7_14 (0x03C9, 0x0301, 0x03CE),ENCODE3_11_7_14 (0x03C9, 0x0313, 0x1F60),
  ENCODE3_11_7_14 (0x03C9, 0x0314, 0x1F61),ENCODE3_11_7_14 (0x03C9, 0x0342, 0x1FF6),
  ENCODE3_11_7_14 (0x03C9, 0x0345, 0x1FF3),ENCODE3_11_7_14 (0x03CA, 0x0300, 0x1FD2),
  ENCODE3_11_7_14 (0x03CA, 0x0301, 0x0390),ENCODE3_11_7_14 (0x03CA, 0x0342, 0x1FD7),
  ENCODE3_11_7_14 (0x03CB, 0x0300, 0x1FE2),ENCODE3_11_7_14 (0x03CB, 0x0301, 0x03B0),
  ENCODE3_11_7_14 (0x03CB, 0x0342, 0x1FE7),ENCODE3_11_7_14 (0x03CE, 0x0345, 0x1FF4),
  ENCODE3_11_7_14 (0x03D2, 0x0301, 0x03D3),ENCODE3_11_7_14 (0x03D2, 0x0308, 0x03D4),
  ENCODE3_11_7_14 (0x0406, 0x0308, 0x0407),ENCODE3_11_7_14 (0x0410, 0x0306, 0x04D0),
  ENCODE3_11_7_14 (0x0410, 0x0308, 0x04D2),ENCODE3_11_7_14 (0x0413, 0x0301, 0x0403),
  ENCODE3_11_7_14 (0x0415, 0x0300, 0x0400),ENCODE3_11_7_14 (0x0415, 0x0306, 0x04D6),
  ENCODE3_11_7_14 (0x0415, 0x0308, 0x0401),ENCODE3_11_7_14 (0x0416, 0x0306, 0x04C1),
  ENCODE3_11_7_14 (0x0416, 0x0308, 0x04DC),ENCODE3_11_7_14 (0x0417, 0x0308, 0x04DE),
  ENCODE3_11_7_14 (0x0418, 0x0300, 0x040D),ENCODE3_11_7_14 (0x0418, 0x0304, 0x04E2),
  ENCODE3_11_7_14 (0x0418, 0x0306, 0x0419),ENCODE3_11_7_14 (0x0418, 0x0308, 0x04E4),
  ENCODE3_11_7_14 (0x041A, 0x0301, 0x040C),ENCODE3_11_7_14 (0x041E, 0x0308, 0x04E6),
  ENCODE3_11_7_14 (0x0423, 0x0304, 0x04EE),ENCODE3_11_7_14 (0x0423, 0x0306, 0x040E),
  ENCODE3_11_7_14 (0x0423, 0x0308, 0x04F0),ENCODE3_11_7_14 (0x0423, 0x030B, 0x04F2),
  ENCODE3_11_7_14 (0x0427, 0x0308, 0x04F4),ENCODE3_11_7_14 (0x042B, 0x0308, 0x04F8),
  ENCODE3_11_7_14 (0x042D, 0x0308, 0x04EC),ENCODE3_11_7_14 (0x0430, 0x0306, 0x04D1),
  ENCODE3_11_7_14 (0x0430, 0x0308, 0x04D3),ENCODE3_11_7_14 (0x0433, 0x0301, 0x0453),
  ENCODE3_11_7_14 (0x0435, 0x0300, 0x0450),ENCODE3_11_7_14 (0x0435, 0x0306, 0x04D7),
  ENCODE3_11_7_14 (0x0435, 0x0308, 0x0451),ENCODE3_11_7_14 (0x0436, 0x0306, 0x04C2),
  ENCODE3_11_7_14 (0x0436, 0x0308, 0x04DD),ENCODE3_11_7_14 (0x0437, 0x0308, 0x04DF),
  ENCODE3_11_7_14 (0x0438, 0x0300, 0x045D),ENCODE3_11_7_14 (0x0438, 0x0304, 0x04E3),
  ENCODE3_11_7_14 (0x0438, 0x0306, 0x0439),ENCODE3_11_7_14 (0x0438, 0x0308, 0x04E5),
  ENCODE3_11_7_14 (0x043A, 0x0301, 0x045C),ENCODE3_11_7_14 (0x043E, 0x0308, 0x04E7),
  ENCODE3_11_7_14 (0x0443, 0x0304, 0x04EF),ENCODE3_11_7_14 (0x0443, 0x0306, 0x045E),
  ENCODE3_11_7_14 (0x0443, 0x0308, 0x04F1),ENCODE3_11_7_14 (0x0443, 0x030B, 0x04F3),
  ENCODE3_11_7_14 (0x0447, 0x0308, 0x04F5),ENCODE3_11_7_14 (0x044B, 0x0308, 0x04F9),
  ENCODE3_11_7_14 (0x044D, 0x0308, 0x04ED),ENCODE3_11_7_14 (0x0456, 0x0308, 0x0457),
  ENCODE3_11_7_14 (0x0474, 0x030F, 0x0476),ENCODE3_11_7_14 (0x0475, 0x030F, 0x0477),
  ENCODE3_11_7_14 (0x04D8, 0x0308, 0x04DA),ENCODE3_11_7_14 (0x04D9, 0x0308, 0x04DB),
  ENCODE3_11_7_14 (0x04E8, 0x0308, 0x04EA),ENCODE3_11_7_14 (0x04E9, 0x0308, 0x04EB),
};
static const uint64_t _hb_ucd_dm2_u64_map[408]=
{
     ENCODE3 (0x05D0, 0x05B7, 0x0000),   ENCODE3 (0x05D0, 0x05B8, 0x0000),
     ENCODE3 (0x05D0, 0x05BC, 0x0000),   ENCODE3 (0x05D1, 0x05BC, 0x0000),
     ENCODE3 (0x05D1, 0x05BF, 0x0000),   ENCODE3 (0x05D2, 0x05BC, 0x0000),
     ENCODE3 (0x05D3, 0x05BC, 0x0000),   ENCODE3 (0x05D4, 0x05BC, 0x0000),
     ENCODE3 (0x05D5, 0x05B9, 0x0000),   ENCODE3 (0x05D5, 0x05BC, 0x0000),
     ENCODE3 (0x05D6, 0x05BC, 0x0000),   ENCODE3 (0x05D8, 0x05BC, 0x0000),
     ENCODE3 (0x05D9, 0x05B4, 0x0000),   ENCODE3 (0x05D9, 0x05BC, 0x0000),
     ENCODE3 (0x05DA, 0x05BC, 0x0000),   ENCODE3 (0x05DB, 0x05BC, 0x0000),
     ENCODE3 (0x05DB, 0x05BF, 0x0000),   ENCODE3 (0x05DC, 0x05BC, 0x0000),
     ENCODE3 (0x05DE, 0x05BC, 0x0000),   ENCODE3 (0x05E0, 0x05BC, 0x0000),
     ENCODE3 (0x05E1, 0x05BC, 0x0000),   ENCODE3 (0x05E3, 0x05BC, 0x0000),
     ENCODE3 (0x05E4, 0x05BC, 0x0000),   ENCODE3 (0x05E4, 0x05BF, 0x0000),
     ENCODE3 (0x05E6, 0x05BC, 0x0000),   ENCODE3 (0x05E7, 0x05BC, 0x0000),
     ENCODE3 (0x05E8, 0x05BC, 0x0000),   ENCODE3 (0x05E9, 0x05BC, 0x0000),
     ENCODE3 (0x05E9, 0x05C1, 0x0000),   ENCODE3 (0x05E9, 0x05C2, 0x0000),
     ENCODE3 (0x05EA, 0x05BC, 0x0000),   ENCODE3 (0x05F2, 0x05B7, 0x0000),
     ENCODE3 (0x0627, 0x0653, 0x0622),   ENCODE3 (0x0627, 0x0654, 0x0623),
     ENCODE3 (0x0627, 0x0655, 0x0625),   ENCODE3 (0x0648, 0x0654, 0x0624),
     ENCODE3 (0x064A, 0x0654, 0x0626),   ENCODE3 (0x06C1, 0x0654, 0x06C2),
     ENCODE3 (0x06D2, 0x0654, 0x06D3),   ENCODE3 (0x06D5, 0x0654, 0x06C0),
     ENCODE3 (0x0915, 0x093C, 0x0000),   ENCODE3 (0x0916, 0x093C, 0x0000),
     ENCODE3 (0x0917, 0x093C, 0x0000),   ENCODE3 (0x091C, 0x093C, 0x0000),
     ENCODE3 (0x0921, 0x093C, 0x0000),   ENCODE3 (0x0922, 0x093C, 0x0000),
     ENCODE3 (0x0928, 0x093C, 0x0929),   ENCODE3 (0x092B, 0x093C, 0x0000),
     ENCODE3 (0x092F, 0x093C, 0x0000),   ENCODE3 (0x0930, 0x093C, 0x0931),
     ENCODE3 (0x0933, 0x093C, 0x0934),   ENCODE3 (0x09A1, 0x09BC, 0x0000),
     ENCODE3 (0x09A2, 0x09BC, 0x0000),   ENCODE3 (0x09AF, 0x09BC, 0x0000),
     ENCODE3 (0x09C7, 0x09BE, 0x09CB),   ENCODE3 (0x09C7, 0x09D7, 0x09CC),
     ENCODE3 (0x0A16, 0x0A3C, 0x0000),   ENCODE3 (0x0A17, 0x0A3C, 0x0000),
     ENCODE3 (0x0A1C, 0x0A3C, 0x0000),   ENCODE3 (0x0A2B, 0x0A3C, 0x0000),
     ENCODE3 (0x0A32, 0x0A3C, 0x0000),   ENCODE3 (0x0A38, 0x0A3C, 0x0000),
     ENCODE3 (0x0B21, 0x0B3C, 0x0000),   ENCODE3 (0x0B22, 0x0B3C, 0x0000),
     ENCODE3 (0x0B47, 0x0B3E, 0x0B4B),   ENCODE3 (0x0B47, 0x0B56, 0x0B48),
     ENCODE3 (0x0B47, 0x0B57, 0x0B4C),   ENCODE3 (0x0B92, 0x0BD7, 0x0B94),
     ENCODE3 (0x0BC6, 0x0BBE, 0x0BCA),   ENCODE3 (0x0BC6, 0x0BD7, 0x0BCC),
     ENCODE3 (0x0BC7, 0x0BBE, 0x0BCB),   ENCODE3 (0x0C46, 0x0C56, 0x0C48),
     ENCODE3 (0x0CBF, 0x0CD5, 0x0CC0),   ENCODE3 (0x0CC6, 0x0CC2, 0x0CCA),
     ENCODE3 (0x0CC6, 0x0CD5, 0x0CC7),   ENCODE3 (0x0CC6, 0x0CD6, 0x0CC8),
     ENCODE3 (0x0CCA, 0x0CD5, 0x0CCB),   ENCODE3 (0x0D46, 0x0D3E, 0x0D4A),
     ENCODE3 (0x0D46, 0x0D57, 0x0D4C),   ENCODE3 (0x0D47, 0x0D3E, 0x0D4B),
     ENCODE3 (0x0DD9, 0x0DCA, 0x0DDA),   ENCODE3 (0x0DD9, 0x0DCF, 0x0DDC),
     ENCODE3 (0x0DD9, 0x0DDF, 0x0DDE),   ENCODE3 (0x0DDC, 0x0DCA, 0x0DDD),
     ENCODE3 (0x0F40, 0x0FB5, 0x0000),   ENCODE3 (0x0F42, 0x0FB7, 0x0000),
     ENCODE3 (0x0F4C, 0x0FB7, 0x0000),   ENCODE3 (0x0F51, 0x0FB7, 0x0000),
     ENCODE3 (0x0F56, 0x0FB7, 0x0000),   ENCODE3 (0x0F5B, 0x0FB7, 0x0000),
     ENCODE3 (0x0F71, 0x0F72, 0x0000),   ENCODE3 (0x0F71, 0x0F74, 0x0000),
     ENCODE3 (0x0F71, 0x0F80, 0x0000),   ENCODE3 (0x0F90, 0x0FB5, 0x0000),
     ENCODE3 (0x0F92, 0x0FB7, 0x0000),   ENCODE3 (0x0F9C, 0x0FB7, 0x0000),
     ENCODE3 (0x0FA1, 0x0FB7, 0x0000),   ENCODE3 (0x0FA6, 0x0FB7, 0x0000),
     ENCODE3 (0x0FAB, 0x0FB7, 0x0000),   ENCODE3 (0x0FB2, 0x0F80, 0x0000),
     ENCODE3 (0x0FB3, 0x0F80, 0x0000),   ENCODE3 (0x1025, 0x102E, 0x1026),
     ENCODE3 (0x1B05, 0x1B35, 0x1B06),   ENCODE3 (0x1B07, 0x1B35, 0x1B08),
     ENCODE3 (0x1B09, 0x1B35, 0x1B0A),   ENCODE3 (0x1B0B, 0x1B35, 0x1B0C),
     ENCODE3 (0x1B0D, 0x1B35, 0x1B0E),   ENCODE3 (0x1B11, 0x1B35, 0x1B12),
     ENCODE3 (0x1B3A, 0x1B35, 0x1B3B),   ENCODE3 (0x1B3C, 0x1B35, 0x1B3D),
     ENCODE3 (0x1B3E, 0x1B35, 0x1B40),   ENCODE3 (0x1B3F, 0x1B35, 0x1B41),
     ENCODE3 (0x1B42, 0x1B35, 0x1B43),   ENCODE3 (0x1E36, 0x0304, 0x1E38),
     ENCODE3 (0x1E37, 0x0304, 0x1E39),   ENCODE3 (0x1E5A, 0x0304, 0x1E5C),
     ENCODE3 (0x1E5B, 0x0304, 0x1E5D),   ENCODE3 (0x1E62, 0x0307, 0x1E68),
     ENCODE3 (0x1E63, 0x0307, 0x1E69),   ENCODE3 (0x1EA0, 0x0302, 0x1EAC),
     ENCODE3 (0x1EA0, 0x0306, 0x1EB6),   ENCODE3 (0x1EA1, 0x0302, 0x1EAD),
     ENCODE3 (0x1EA1, 0x0306, 0x1EB7),   ENCODE3 (0x1EB8, 0x0302, 0x1EC6),
     ENCODE3 (0x1EB9, 0x0302, 0x1EC7),   ENCODE3 (0x1ECC, 0x0302, 0x1ED8),
     ENCODE3 (0x1ECD, 0x0302, 0x1ED9),   ENCODE3 (0x1F00, 0x0300, 0x1F02),
     ENCODE3 (0x1F00, 0x0301, 0x1F04),   ENCODE3 (0x1F00, 0x0342, 0x1F06),
     ENCODE3 (0x1F00, 0x0345, 0x1F80),   ENCODE3 (0x1F01, 0x0300, 0x1F03),
     ENCODE3 (0x1F01, 0x0301, 0x1F05),   ENCODE3 (0x1F01, 0x0342, 0x1F07),
     ENCODE3 (0x1F01, 0x0345, 0x1F81),   ENCODE3 (0x1F02, 0x0345, 0x1F82),
     ENCODE3 (0x1F03, 0x0345, 0x1F83),   ENCODE3 (0x1F04, 0x0345, 0x1F84),
     ENCODE3 (0x1F05, 0x0345, 0x1F85),   ENCODE3 (0x1F06, 0x0345, 0x1F86),
     ENCODE3 (0x1F07, 0x0345, 0x1F87),   ENCODE3 (0x1F08, 0x0300, 0x1F0A),
     ENCODE3 (0x1F08, 0x0301, 0x1F0C),   ENCODE3 (0x1F08, 0x0342, 0x1F0E),
     ENCODE3 (0x1F08, 0x0345, 0x1F88),   ENCODE3 (0x1F09, 0x0300, 0x1F0B),
     ENCODE3 (0x1F09, 0x0301, 0x1F0D),   ENCODE3 (0x1F09, 0x0342, 0x1F0F),
     ENCODE3 (0x1F09, 0x0345, 0x1F89),   ENCODE3 (0x1F0A, 0x0345, 0x1F8A),
     ENCODE3 (0x1F0B, 0x0345, 0x1F8B),   ENCODE3 (0x1F0C, 0x0345, 0x1F8C),
     ENCODE3 (0x1F0D, 0x0345, 0x1F8D),   ENCODE3 (0x1F0E, 0x0345, 0x1F8E),
     ENCODE3 (0x1F0F, 0x0345, 0x1F8F),   ENCODE3 (0x1F10, 0x0300, 0x1F12),
     ENCODE3 (0x1F10, 0x0301, 0x1F14),   ENCODE3 (0x1F11, 0x0300, 0x1F13),
     ENCODE3 (0x1F11, 0x0301, 0x1F15),   ENCODE3 (0x1F18, 0x0300, 0x1F1A),
     ENCODE3 (0x1F18, 0x0301, 0x1F1C),   ENCODE3 (0x1F19, 0x0300, 0x1F1B),
     ENCODE3 (0x1F19, 0x0301, 0x1F1D),   ENCODE3 (0x1F20, 0x0300, 0x1F22),
     ENCODE3 (0x1F20, 0x0301, 0x1F24),   ENCODE3 (0x1F20, 0x0342, 0x1F26),
     ENCODE3 (0x1F20, 0x0345, 0x1F90),   ENCODE3 (0x1F21, 0x0300, 0x1F23),
     ENCODE3 (0x1F21, 0x0301, 0x1F25),   ENCODE3 (0x1F21, 0x0342, 0x1F27),
     ENCODE3 (0x1F21, 0x0345, 0x1F91),   ENCODE3 (0x1F22, 0x0345, 0x1F92),
     ENCODE3 (0x1F23, 0x0345, 0x1F93),   ENCODE3 (0x1F24, 0x0345, 0x1F94),
     ENCODE3 (0x1F25, 0x0345, 0x1F95),   ENCODE3 (0x1F26, 0x0345, 0x1F96),
     ENCODE3 (0x1F27, 0x0345, 0x1F97),   ENCODE3 (0x1F28, 0x0300, 0x1F2A),
     ENCODE3 (0x1F28, 0x0301, 0x1F2C),   ENCODE3 (0x1F28, 0x0342, 0x1F2E),
     ENCODE3 (0x1F28, 0x0345, 0x1F98),   ENCODE3 (0x1F29, 0x0300, 0x1F2B),
     ENCODE3 (0x1F29, 0x0301, 0x1F2D),   ENCODE3 (0x1F29, 0x0342, 0x1F2F),
     ENCODE3 (0x1F29, 0x0345, 0x1F99),   ENCODE3 (0x1F2A, 0x0345, 0x1F9A),
     ENCODE3 (0x1F2B, 0x0345, 0x1F9B),   ENCODE3 (0x1F2C, 0x0345, 0x1F9C),
     ENCODE3 (0x1F2D, 0x0345, 0x1F9D),   ENCODE3 (0x1F2E, 0x0345, 0x1F9E),
     ENCODE3 (0x1F2F, 0x0345, 0x1F9F),   ENCODE3 (0x1F30, 0x0300, 0x1F32),
     ENCODE3 (0x1F30, 0x0301, 0x1F34),   ENCODE3 (0x1F30, 0x0342, 0x1F36),
     ENCODE3 (0x1F31, 0x0300, 0x1F33),   ENCODE3 (0x1F31, 0x0301, 0x1F35),
     ENCODE3 (0x1F31, 0x0342, 0x1F37),   ENCODE3 (0x1F38, 0x0300, 0x1F3A),
     ENCODE3 (0x1F38, 0x0301, 0x1F3C),   ENCODE3 (0x1F38, 0x0342, 0x1F3E),
     ENCODE3 (0x1F39, 0x0300, 0x1F3B),   ENCODE3 (0x1F39, 0x0301, 0x1F3D),
     ENCODE3 (0x1F39, 0x0342, 0x1F3F),   ENCODE3 (0x1F40, 0x0300, 0x1F42),
     ENCODE3 (0x1F40, 0x0301, 0x1F44),   ENCODE3 (0x1F41, 0x0300, 0x1F43),
     ENCODE3 (0x1F41, 0x0301, 0x1F45),   ENCODE3 (0x1F48, 0x0300, 0x1F4A),
     ENCODE3 (0x1F48, 0x0301, 0x1F4C),   ENCODE3 (0x1F49, 0x0300, 0x1F4B),
     ENCODE3 (0x1F49, 0x0301, 0x1F4D),   ENCODE3 (0x1F50, 0x0300, 0x1F52),
     ENCODE3 (0x1F50, 0x0301, 0x1F54),   ENCODE3 (0x1F50, 0x0342, 0x1F56),
     ENCODE3 (0x1F51, 0x0300, 0x1F53),   ENCODE3 (0x1F51, 0x0301, 0x1F55),
     ENCODE3 (0x1F51, 0x0342, 0x1F57),   ENCODE3 (0x1F59, 0x0300, 0x1F5B),
     ENCODE3 (0x1F59, 0x0301, 0x1F5D),   ENCODE3 (0x1F59, 0x0342, 0x1F5F),
     ENCODE3 (0x1F60, 0x0300, 0x1F62),   ENCODE3 (0x1F60, 0x0301, 0x1F64),
     ENCODE3 (0x1F60, 0x0342, 0x1F66),   ENCODE3 (0x1F60, 0x0345, 0x1FA0),
     ENCODE3 (0x1F61, 0x0300, 0x1F63),   ENCODE3 (0x1F61, 0x0301, 0x1F65),
     ENCODE3 (0x1F61, 0x0342, 0x1F67),   ENCODE3 (0x1F61, 0x0345, 0x1FA1),
     ENCODE3 (0x1F62, 0x0345, 0x1FA2),   ENCODE3 (0x1F63, 0x0345, 0x1FA3),
     ENCODE3 (0x1F64, 0x0345, 0x1FA4),   ENCODE3 (0x1F65, 0x0345, 0x1FA5),
     ENCODE3 (0x1F66, 0x0345, 0x1FA6),   ENCODE3 (0x1F67, 0x0345, 0x1FA7),
     ENCODE3 (0x1F68, 0x0300, 0x1F6A),   ENCODE3 (0x1F68, 0x0301, 0x1F6C),
     ENCODE3 (0x1F68, 0x0342, 0x1F6E),   ENCODE3 (0x1F68, 0x0345, 0x1FA8),
     ENCODE3 (0x1F69, 0x0300, 0x1F6B),   ENCODE3 (0x1F69, 0x0301, 0x1F6D),
     ENCODE3 (0x1F69, 0x0342, 0x1F6F),   ENCODE3 (0x1F69, 0x0345, 0x1FA9),
     ENCODE3 (0x1F6A, 0x0345, 0x1FAA),   ENCODE3 (0x1F6B, 0x0345, 0x1FAB),
     ENCODE3 (0x1F6C, 0x0345, 0x1FAC),   ENCODE3 (0x1F6D, 0x0345, 0x1FAD),
     ENCODE3 (0x1F6E, 0x0345, 0x1FAE),   ENCODE3 (0x1F6F, 0x0345, 0x1FAF),
     ENCODE3 (0x1F70, 0x0345, 0x1FB2),   ENCODE3 (0x1F74, 0x0345, 0x1FC2),
     ENCODE3 (0x1F7C, 0x0345, 0x1FF2),   ENCODE3 (0x1FB6, 0x0345, 0x1FB7),
     ENCODE3 (0x1FBF, 0x0300, 0x1FCD),   ENCODE3 (0x1FBF, 0x0301, 0x1FCE),
     ENCODE3 (0x1FBF, 0x0342, 0x1FCF),   ENCODE3 (0x1FC6, 0x0345, 0x1FC7),
     ENCODE3 (0x1FF6, 0x0345, 0x1FF7),   ENCODE3 (0x1FFE, 0x0300, 0x1FDD),
     ENCODE3 (0x1FFE, 0x0301, 0x1FDE),   ENCODE3 (0x1FFE, 0x0342, 0x1FDF),
     ENCODE3 (0x2190, 0x0338, 0x219A),   ENCODE3 (0x2192, 0x0338, 0x219B),
     ENCODE3 (0x2194, 0x0338, 0x21AE),   ENCODE3 (0x21D0, 0x0338, 0x21CD),
     ENCODE3 (0x21D2, 0x0338, 0x21CF),   ENCODE3 (0x21D4, 0x0338, 0x21CE),
     ENCODE3 (0x2203, 0x0338, 0x2204),   ENCODE3 (0x2208, 0x0338, 0x2209),
     ENCODE3 (0x220B, 0x0338, 0x220C),   ENCODE3 (0x2223, 0x0338, 0x2224),
     ENCODE3 (0x2225, 0x0338, 0x2226),   ENCODE3 (0x223C, 0x0338, 0x2241),
     ENCODE3 (0x2243, 0x0338, 0x2244),   ENCODE3 (0x2245, 0x0338, 0x2247),
     ENCODE3 (0x2248, 0x0338, 0x2249),   ENCODE3 (0x224D, 0x0338, 0x226D),
     ENCODE3 (0x2261, 0x0338, 0x2262),   ENCODE3 (0x2264, 0x0338, 0x2270),
     ENCODE3 (0x2265, 0x0338, 0x2271),   ENCODE3 (0x2272, 0x0338, 0x2274),
     ENCODE3 (0x2273, 0x0338, 0x2275),   ENCODE3 (0x2276, 0x0338, 0x2278),
     ENCODE3 (0x2277, 0x0338, 0x2279),   ENCODE3 (0x227A, 0x0338, 0x2280),
     ENCODE3 (0x227B, 0x0338, 0x2281),   ENCODE3 (0x227C, 0x0338, 0x22E0),
     ENCODE3 (0x227D, 0x0338, 0x22E1),   ENCODE3 (0x2282, 0x0338, 0x2284),
     ENCODE3 (0x2283, 0x0338, 0x2285),   ENCODE3 (0x2286, 0x0338, 0x2288),
     ENCODE3 (0x2287, 0x0338, 0x2289),   ENCODE3 (0x2291, 0x0338, 0x22E2),
     ENCODE3 (0x2292, 0x0338, 0x22E3),   ENCODE3 (0x22A2, 0x0338, 0x22AC),
     ENCODE3 (0x22A8, 0x0338, 0x22AD),   ENCODE3 (0x22A9, 0x0338, 0x22AE),
     ENCODE3 (0x22AB, 0x0338, 0x22AF),   ENCODE3 (0x22B2, 0x0338, 0x22EA),
     ENCODE3 (0x22B3, 0x0338, 0x22EB),   ENCODE3 (0x22B4, 0x0338, 0x22EC),
     ENCODE3 (0x22B5, 0x0338, 0x22ED),   ENCODE3 (0x2ADD, 0x0338, 0x0000),
     ENCODE3 (0x3046, 0x3099, 0x3094),   ENCODE3 (0x304B, 0x3099, 0x304C),
     ENCODE3 (0x304D, 0x3099, 0x304E),   ENCODE3 (0x304F, 0x3099, 0x3050),
     ENCODE3 (0x3051, 0x3099, 0x3052),   ENCODE3 (0x3053, 0x3099, 0x3054),
     ENCODE3 (0x3055, 0x3099, 0x3056),   ENCODE3 (0x3057, 0x3099, 0x3058),
     ENCODE3 (0x3059, 0x3099, 0x305A),   ENCODE3 (0x305B, 0x3099, 0x305C),
     ENCODE3 (0x305D, 0x3099, 0x305E),   ENCODE3 (0x305F, 0x3099, 0x3060),
     ENCODE3 (0x3061, 0x3099, 0x3062),   ENCODE3 (0x3064, 0x3099, 0x3065),
     ENCODE3 (0x3066, 0x3099, 0x3067),   ENCODE3 (0x3068, 0x3099, 0x3069),
     ENCODE3 (0x306F, 0x3099, 0x3070),   ENCODE3 (0x306F, 0x309A, 0x3071),
     ENCODE3 (0x3072, 0x3099, 0x3073),   ENCODE3 (0x3072, 0x309A, 0x3074),
     ENCODE3 (0x3075, 0x3099, 0x3076),   ENCODE3 (0x3075, 0x309A, 0x3077),
     ENCODE3 (0x3078, 0x3099, 0x3079),   ENCODE3 (0x3078, 0x309A, 0x307A),
     ENCODE3 (0x307B, 0x3099, 0x307C),   ENCODE3 (0x307B, 0x309A, 0x307D),
     ENCODE3 (0x309D, 0x3099, 0x309E),   ENCODE3 (0x30A6, 0x3099, 0x30F4),
     ENCODE3 (0x30AB, 0x3099, 0x30AC),   ENCODE3 (0x30AD, 0x3099, 0x30AE),
     ENCODE3 (0x30AF, 0x3099, 0x30B0),   ENCODE3 (0x30B1, 0x3099, 0x30B2),
     ENCODE3 (0x30B3, 0x3099, 0x30B4),   ENCODE3 (0x30B5, 0x3099, 0x30B6),
     ENCODE3 (0x30B7, 0x3099, 0x30B8),   ENCODE3 (0x30B9, 0x3099, 0x30BA),
     ENCODE3 (0x30BB, 0x3099, 0x30BC),   ENCODE3 (0x30BD, 0x3099, 0x30BE),
     ENCODE3 (0x30BF, 0x3099, 0x30C0),   ENCODE3 (0x30C1, 0x3099, 0x30C2),
     ENCODE3 (0x30C4, 0x3099, 0x30C5),   ENCODE3 (0x30C6, 0x3099, 0x30C7),
     ENCODE3 (0x30C8, 0x3099, 0x30C9),   ENCODE3 (0x30CF, 0x3099, 0x30D0),
     ENCODE3 (0x30CF, 0x309A, 0x30D1),   ENCODE3 (0x30D2, 0x3099, 0x30D3),
     ENCODE3 (0x30D2, 0x309A, 0x30D4),   ENCODE3 (0x30D5, 0x3099, 0x30D6),
     ENCODE3 (0x30D5, 0x309A, 0x30D7),   ENCODE3 (0x30D8, 0x3099, 0x30D9),
     ENCODE3 (0x30D8, 0x309A, 0x30DA),   ENCODE3 (0x30DB, 0x3099, 0x30DC),
     ENCODE3 (0x30DB, 0x309A, 0x30DD),   ENCODE3 (0x30EF, 0x3099, 0x30F7),
     ENCODE3 (0x30F0, 0x3099, 0x30F8),   ENCODE3 (0x30F1, 0x3099, 0x30F9),
     ENCODE3 (0x30F2, 0x3099, 0x30FA),   ENCODE3 (0x30FD, 0x3099, 0x30FE),
     ENCODE3 (0xFB49, 0x05C1, 0x0000),   ENCODE3 (0xFB49, 0x05C2, 0x0000),
   ENCODE3 (0x105D2, 0x0307, 0x105C9), ENCODE3 (0x105DA, 0x0307, 0x105E4),
  ENCODE3 (0x11099, 0x110BA, 0x1109A),ENCODE3 (0x1109B, 0x110BA, 0x1109C),
  ENCODE3 (0x110A5, 0x110BA, 0x110AB),ENCODE3 (0x11131, 0x11127, 0x1112E),
  ENCODE3 (0x11132, 0x11127, 0x1112F),ENCODE3 (0x11347, 0x1133E, 0x1134B),
  ENCODE3 (0x11347, 0x11357, 0x1134C),ENCODE3 (0x11382, 0x113C9, 0x11383),
  ENCODE3 (0x11384, 0x113BB, 0x11385),ENCODE3 (0x1138B, 0x113C2, 0x1138E),
  ENCODE3 (0x11390, 0x113C9, 0x11391),ENCODE3 (0x113C2, 0x113B8, 0x113C7),
  ENCODE3 (0x113C2, 0x113C2, 0x113C5),ENCODE3 (0x113C2, 0x113C9, 0x113C8),
  ENCODE3 (0x114B9, 0x114B0, 0x114BC),ENCODE3 (0x114B9, 0x114BA, 0x114BB),
  ENCODE3 (0x114B9, 0x114BD, 0x114BE),ENCODE3 (0x115B8, 0x115AF, 0x115BA),
  ENCODE3 (0x115B9, 0x115AF, 0x115BB),ENCODE3 (0x11935, 0x11930, 0x11938),
  ENCODE3 (0x1611E, 0x1611E, 0x16121),ENCODE3 (0x1611E, 0x1611F, 0x16123),
  ENCODE3 (0x1611E, 0x16120, 0x16125),ENCODE3 (0x1611E, 0x16129, 0x16122),
  ENCODE3 (0x16121, 0x1611F, 0x16126),ENCODE3 (0x16121, 0x16120, 0x16128),
  ENCODE3 (0x16122, 0x1611F, 0x16127),ENCODE3 (0x16129, 0x1611F, 0x16124),
  ENCODE3 (0x16D63, 0x16D67, 0x16D69),ENCODE3 (0x16D67, 0x16D67, 0x16D68),
  ENCODE3 (0x16D69, 0x16D67, 0x16D6A), ENCODE3 (0x1D157, 0x1D165, 0x0000),
   ENCODE3 (0x1D158, 0x1D165, 0x0000), ENCODE3 (0x1D15F, 0x1D16E, 0x0000),
   ENCODE3 (0x1D15F, 0x1D16F, 0x0000), ENCODE3 (0x1D15F, 0x1D170, 0x0000),
   ENCODE3 (0x1D15F, 0x1D171, 0x0000), ENCODE3 (0x1D15F, 0x1D172, 0x0000),
   ENCODE3 (0x1D1B9, 0x1D165, 0x0000), ENCODE3 (0x1D1BA, 0x1D165, 0x0000),
   ENCODE3 (0x1D1BB, 0x1D16E, 0x0000), ENCODE3 (0x1D1BB, 0x1D16F, 0x0000),
   ENCODE3 (0x1D1BC, 0x1D16E, 0x0000), ENCODE3 (0x1D1BC, 0x1D16F, 0x0000),
};

static const uint8_t _hb_ucd_u8[19991]=
{
    5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
   21, 22, 23, 24, 25, 26, 27, 28, 29, 30,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2, 31,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
   32,  2, 33, 34, 35, 36,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 37,  3,  3,  3,  3,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1, 38, 39, 40, 41,
   42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
    2, 58, 59, 60,  0,  0,  0, 61,  2,  2, 62,  2,  2,  2,  2,  2,
    2, 63,  2, 64,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   65,  0,  0,  0,  2, 66, 67, 68,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2, 69,  2, 70,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0, 71, 72, 73,  0,  0,  0,  0, 74,  0,
    0,  0,  0,  0,  0,  0, 75, 76, 77, 78, 79, 80,  4, 81,  0, 82,
   83, 84, 85, 86, 87,  0, 88, 89, 90, 91,  4, 92, 93, 94,  0,  0,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2, 95,  2,  2,  2,  2,  2,  2,  2,  2, 96,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2, 97,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2, 98,  2, 99,  0,  0,  0,  0,  2,100,  0,  0,
    2,  2,  2,  2,  2,  2,  2,  2,  2,101,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,102,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,103,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  104,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,105,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,
    2,  2,  2,  2,  2,  2,  2,  2,  3,  3,  3,  3,  3,  3,  3,  3,
    4,  4,  4,  4,  4,  4,  4,  4,  6,  6,  6,  6,  6,  6,  6,  6,
    5,  5,  5,  5,  5,  5,  5,  5,  9,  9,  9,  9,  9,  9,  9,  9,
    8,  8,  8,  8,  8,  8,  8,  8,  7,  7,  7,  7,  7,  7,  7,  7,
   12, 12, 12, 12, 12, 12, 12, 12, 11, 11, 11, 11, 11,  0,  0,  0,
   10, 10, 10, 10, 10, 10, 10, 10, 14, 14, 14, 14, 14, 14, 14, 14,
    1,  1,  1,  1,  1,  1,  1, 15,  1,  1,  1,  1,  1,  1,  1,  0,
    1,  1,  1,  1, 15,  1,  1,  1,  1,  1,  1,  1, 15,  0,  0,  0,
   18, 18, 18, 18, 18, 18, 18, 18,  1,  1,  1,  1,  1, 15,  0,  0,
    1,  5,  0, 11, 11, 11, 11, 11,  1,  1,  1,  1,  1,  1, 15,  0,
    6,  6,  6,  6,  7,  7,  7,  7,  3,  3,  3,  3,  3,  3,  0,  0,
   15,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  0,  0,
    1,  1,  1,  1,  1,  0,  0,  0,  1,  1,  1, 15,  0,  0,  0,  0,
    1,  1,  1, 15,  1,  1,  1, 15, 28, 28, 28, 28, 28, 28, 28, 28,
   11, 11, 11, 11, 11,  1,  1,  1, 15, 17,  1,  1,  1,  1,  1,  1,
   15,  1,  1,  1,  1,  1,  1,  1,  3,  3,  3,  0,  0,  0,  0,  0,
    3,  3,  3,  3,  3,  3, 29,  0,  1,  0,  0,  0,  0,  0,  0,  0,
   11, 11, 11, 11, 11, 11, 11, 11,  7,  7,  7,  7,  7,  6,  6,  6,
    0,  0,  0, 11, 11, 11, 11, 11, 17,  1,  1,  1,  1,  1,  1,  1,
   11, 11, 11, 11, 11, 10, 10, 10,  3,  3,  3,  3,  3,  0,  0,  0,
    5, 45,  1,  1,  1,  1,  1,  1,  6,  6,  6,  6, 42, 42, 42, 42,
    3,  3,  0,  0,  0,  0,  0,  0,  3,  3,  3,  3,  3,  3,  3, 29,
   11, 11, 11, 11, 11,  0,  0, 13,  1,  1,  0,  0,  0,  0,  0,  0,
   48, 10, 10, 10, 10, 10, 10, 10,  7,  6,  6,  6,  6,  6,  6,  6,
   54,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  0,  0,  0,  0,
   11, 11, 11, 11, 11, 13,  9, 77, 88,  7,  7,  7,  7,  7,  7,  7,
    7,  7,  7,  7,  7,104,105,106, 89,  6,  6,  6,  6,  6,  6,  6,
   22, 22, 22, 22, 22, 22, 22, 22, 27,  5,  5,  5,  5,  5,  5,  5,
    1,  1,  1,  5,  5,  5,  5,  5, 25,  0,  0,  0,  0,  0,  0,  0,
    5, 20,  1,  1,  1,  1,  1,  1,  1, 17,  1,  1,  1,  1,  1,  1,
   11, 11, 11, 11, 11, 13, 13, 13,  1,  1,  1,  1, 15,  1,  1,  0,
   15,  1,  1,  0,  1,  1,  1,  1,  1,  1,  1, 15,  1,  1,  1,  1,
   10, 10, 10, 10, 10, 10, 51,  0,117,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  0,  0,  0,  0,  0, 23,  0,  0,  0,  0,  0,  0,  0,
    1,  1,  1,  1, 14, 14, 14, 13,  6,  6,  6,  0,  7,  7,  7,  0,
   30, 30, 30, 30, 30, 30, 30, 30,  3,  3,  3,  3,  3, 29,  0,  0,
   10, 10, 10, 10, 10,  3,  3,  3, 75, 10, 10, 10, 10, 10, 10, 10,
    3,  3,  3, 29,  0,  0,  0,  0,  6,  8,  8,  8,  8,  8,  8,  8,
    6,  6,  6, 46,  0,  0,  0,  0,  1,  1,  1,  1,  0,  0,  0,  0,
    7,  7,  7,  7,  7, 32,  7,  7,  0, 10, 10, 10, 10, 10, 10, 10,
   10, 10, 10, 10, 51,  0,  0,  0,  1,  1,  1,  1,  1,  1, 86, 10,
    1,  1,  1,  1,  1,  1, 16, 19,  1,  1,  1,  1,  1,  1,  1, 34,
    1,  1,  1,  1,  1,  1,  1,  5, 16, 16, 16, 16, 16, 16, 16, 16,
    1, 15,  0,  0,  0,  0,  0,  0,  5,  5,  5, 23,  0,  0,  0,  0,
   10, 10,  0,  0,  0,  0,  0,  0,  6,  6,  7,  7,  7,  7,  7,  7,
    7,  7,  7,  7,  7,  7,  7,  6,  6,  6,  6,  6,  6,  6,  7,  7,
    7,  7,  7,  6,  6,  6,  6,  6,  3,  0,  0,  0,  0,  0,  0,  0,
    3,  3,  3,  3,  3,  3,  3,  0, 87, 13,102, 13, 26,103, 76, 13,
    6,  6,  6,  6,  6,107,108,138, 87, 30, 30, 65,109,139,140,110,
   49, 10, 89, 13,141,142, 10, 78,  7,  7,  7, 59,  7,  7,  7,  8,
    6,  6,  6, 60,  6,  6,  6,  6,  8,  8,  8,  8, 21, 21, 21, 21,
   21, 21, 21, 21,  6,  8,  8,  8,  8,  8,  8,  8,  7, 21, 21,  6,
   21,  8,  8,  7, 21,  7,  6,  7,  7, 21,  8,  7,  8,  6,  7, 21,
    8,  8,  8,  7, 21,  6,  8,  7, 21,  7, 21, 21,  8, 66,  8,  6,
    1,  1,111, 21,112,111, 21, 21, 21, 21, 21, 21, 21, 21,  6,  8,
   21,112,  8,  7,  8,  8,  8,  8,  8,  8,  6,  6,  6,  7, 21,  8,
   21, 21,  7,  8,  8,  8,  8,  8,  6,  6,  1,  6,  6,  6,  6,  6,
   14, 22, 22, 14, 14, 14, 14, 14, 14, 22, 22, 22, 22, 22, 22, 22,
   14, 14, 67, 22, 22, 22, 67, 67,  8,  8, 67,  8,  0, 68,  6, 88,
    0,  0, 22,143,  7, 32, 32,  7, 21,  7,  7,  7,  7,  7,  7,  7,
    7, 39,  7,  7,  7,  7,  6,  6,  6,  6,  6,  6,  6,  6,  6, 21,
    6,  7,  8,  6,  8,  8,  8,  8,  6,  6,  8, 90, 21,  8, 21,  7,
    8, 41,  5,  5, 79,  8,  8,  8,  7, 21, 21, 21, 21, 21, 21,  6,
   39,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7, 32, 14, 13, 13, 13,
    6,  6,  6,  6,113, 91,144, 69,  5,  5,  5,  5,  5,  5,  5,145,
   70, 36,  5, 70,  5,  0,  0,  0,  1,  1,  1,  1,  1, 15,  0, 17,
    1, 40, 33,  0,  0,  0,  0,  0, 18, 18, 18,  9, 77, 80, 13,  3,
    5,  5,  5,  5,  5, 36,146, 13, 52,  1,  1,  1,  1, 24,  5,  5,
   11, 11, 11, 11, 11, 13, 13,  1, 25,  1,  1,  1,  1,  1,  1,  1,
    1,  1, 53,  5,  5,  5,147, 41,  5,  5, 81, 71, 50,  5,  5,  1,
   11, 11, 11, 11, 11,  1, 55, 72, 13, 13, 13, 13, 13, 13, 13, 73,
   24,  1,  1,  1,  1,  1,  1,  1,  5,  5,  5,  5,  5, 23, 17,  1,
    1,  1,  1,  1,  1, 24,  5,  5,  5,  5, 14, 65, 13, 82, 27, 30,
    1,  1,  1,  5,  5, 71,  5,  5,  5,  5, 71,  5, 71,  5,  5,  0,
   13, 13, 13, 13, 13, 13, 13, 33,  1,  1,  1,  1, 24,  5,  0, 33,
    1,  1,  1,  1,148,  1,  1,  1, 18,  0,  0, 27,  5,  5,  5,  5,
    1,  1,  1,  1, 44,  5,  5,  5,  5, 92,  5,  5,  5,  5,  5,  5,
    1,  1,  1,  1,  1, 20, 25, 16, 19,  5,  5,  5, 20, 16, 19, 16,
   24,  5,  5,  5,  1,  1,  1,  1,  1,  5, 13, 11, 11, 11, 11, 11,
   61,  1,  1,  1,  1,  1,  1,  1, 24, 16, 17,  1,  1,  1, 15, 17,
   15, 15,  0,  1,  1,  0, 25, 16, 19,  5, 23, 35, 37, 35, 19, 15,
    0,  0,  0, 35,  0,  0,  1, 17,  1, 30, 10, 10, 10, 69, 40, 23,
   27, 20, 17,  1,  1, 15,  0, 17, 15,  1, 17, 15,  1,  0, 23, 16,
   19, 23,  0, 27, 23, 27,  5,  0, 27,  0,  0,  0, 17,  1, 15, 15,
    5,  1, 24, 33,  0,  0,  0,  0, 27, 20, 17,  1,  1,  1,  1, 17,
   15,  1, 17,  1,  1,  0, 25, 16, 19,  5,  5, 27, 20, 35, 19,  0,
   80,  0,  0,  0, 17,  5,  5,  5, 27, 16, 17,  1,  1,  1, 15, 17,
   15,  1, 17,  1,  1,  0, 25, 19, 19,  5, 23, 35, 37, 35, 19,  0,
    0, 27,  5, 20,  0,  0,  1, 17, 72, 10, 10, 10,  0,  0,  0,  0,
    0, 25, 17,  1,  1, 15,  0,  1, 15,  1,  1,  0, 17, 15, 15,  1,
    0, 17, 15,  0,  1, 15,  0,  1,  1,  1,  1,  1,  1,  0,  0, 16,
   20, 37,  0, 16, 37, 16, 19,  0, 15,  0,  0, 35,  0,  0,  0,  0,
   10, 62,  3,  3, 69, 29,  0,  0, 20, 16, 25,  1,  1,  1, 15,  1,
    1,  1,  1,  1,  1,  0, 25,  5, 20, 16, 37,  5, 23,  5,  5,  0,
    0,  0, 27, 23,  1, 15,  1,  0,  0,  0,  0, 38, 10, 10, 10, 62,
   24, 16, 53,  1,  1,  1, 15,  1,  1,  1, 17,  1,  1,  0, 25, 19,
   16, 16, 37, 20, 37, 16,  5,  0,  0,  0, 35, 37,  0,  0,  1, 15,
   17, 34,  0,  0,  0,  0,  0,  0,  5, 16,  1,  1,  1,  1, 15,  1,
    1,  1,  1,  1,  1, 24, 25, 16, 19,  5, 23, 16, 37, 16, 19, 55,
    0,  0,  1, 34, 10, 10, 10,114, 10, 10, 10, 10, 62,  1,  1,  1,
   27, 16, 17,  1,  1,  1,  1,  1,  1,  1,  1, 15,  0,  1,  1,  1,
    1, 17,  1,  1,  1,  1, 17,  0,  1,  1,  1, 15,  0, 23,  0, 35,
   16,  5, 23, 23, 16, 16, 16, 16,  0, 16, 33,  0,  0,  0,  0,  0,
   24,  1,  5,  5,  5, 23,  0,115,  1,  1,  1, 71,  5,  5,  5, 36,
   11, 11, 11, 11, 11, 13,  0,  0, 17, 15, 15,  1,  1, 15,  1,  1,
    1,  1, 17, 17,  1,  1,  1,  1, 24,  1,  5,  5,  5,  5, 25,  0,
    1,  1, 15, 82,  5,  5,  5, 23, 11, 11, 11, 11, 11,  0,  1,  1,
   55,  3, 13, 13, 13, 13, 13, 13, 13, 56, 56,  3,  5,  3,  3,  3,
   10, 10, 41, 41, 41, 26, 26, 16,  1,  1,  1,  1, 17,  1,  1,  1,
   27,  5,  5,  5,  5,  5,  5, 20,  5,  5, 36,  5,  1,  1, 24,  5,
    5,  5,  5,  5, 27,  5,  5,  5,  5,  5,  5,  5,  5,  5, 23,  3,
    3,  3,  3, 50,  3,  3, 29,  3, 13, 13, 56,  3, 65, 33,  0,  0,
    1,  1,  1,  1,  1, 34, 19,  5, 20,  5,  5,  5, 19, 20, 19, 25,
    1,  1,  1, 16,  5,  1,  1,  5, 25, 16, 45, 34, 16, 16, 16,  1,
   24,  5, 25,  1,  1,  1,  1,  1,  1, 20, 19, 20, 16, 16, 19, 34,
   11, 11, 11, 11, 11, 16, 19,  3,  7,  7,  7, 39,  0,  0, 39,  0,
    6,  6,  6,  6,  6,113, 68,  6,  1,  1,  1, 15, 15,  1,  1,  0,
   15,  1,  1,  0,  1,  1,  1, 15,  1,  1,  1,  1,  1, 15, 27,  5,
   13, 13, 13, 13,116, 10, 10, 10,  7,  7,  7,  0,  6,  6,  6,  0,
    1,  1,  1,  1,  1,  1, 55, 53,149,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,150,151,  0,  1,  1,  1,  1,  1, 40, 13, 12,
  152,  1,  1,  1, 15,  0,  0,  0,  1,  5, 20,  0,  0,  0,  0, 17,
    1,  5, 83, 33,  0,  0,  0,  0,  1,  5,  0,  0,  0,  0,  0,  0,
    1,  1,  1,  1,  1,  1, 15,  1, 15,  5,  0,  0,  0,  0,  0,  0,
    1,  1,  5, 19,  5,  5,  5, 16, 16, 16, 16, 20, 19,  5,  5,  5,
    5,  5, 13, 61, 13, 80, 24,  0, 10, 10, 10, 10, 10,  0,  0,  0,
   13, 13, 13, 93, 13, 70,  5, 92,  1, 44,  1,  1,  1,  1,  1,  1,
    1,  1, 24, 25,  1,  1,  1,  1,  1,  1,  1,  1, 24, 15,  0,  0,
    5, 20, 16, 19, 20, 16,  0,  0, 16, 20, 16, 16, 19,  5,  0,  0,
   29,  0, 13, 11, 11, 11, 11, 11,  1,  1, 15,  0,  0,  0,  0,  0,
   11, 11, 11, 11, 11, 51,  0,  3,  1,  1,  1, 24, 20, 19,  0, 13,
    1,  1, 34, 20,  5,  5,  5, 23, 20, 20, 19,  5,  5,  5, 20, 16,
   16, 19,  5,  5,  5,  5, 23, 27, 13, 13, 13, 61, 13, 13, 13,  0,
    5,  5,  5,  5,  5,  5,  5, 94,  5,  5, 45,  1,  1,  1,  1,  1,
    1,  1, 20,  5,  5, 20, 20, 16, 16, 20, 45,  1,  1,  1, 15, 13,
   56,  3,  3,  3,  3, 41,  5,  5,  5,  5,  3,  3,  3,  3, 65, 13,
   34,  5,  5, 16,  5, 19,  5,  1,  1,  1,  1, 20,  5, 16, 19, 19,
    5, 16,  0,  0,  0,  0, 13, 13,  1,  1, 16, 16, 16, 16,  5,  5,
    5,  5, 16,  5,  0, 38, 13, 13, 11, 11, 11, 11, 11,  0, 17,  1,
    6,  6,  6,  6, 21, 46,  0,  0,  7,  7,  7,  7,  7, 32, 39,  7,
   13, 13, 13, 13,  0,  0,  0,  0,  5, 36,  5,  5,  5,  5,  5,  5,
   20,  5,  5,  5, 25,  1, 24,  1,  1,  1, 25, 34,  5, 15,  0,  0,
    6,  6,  6,  6,  6,  6, 14, 14, 14, 14, 14, 14, 14, 68,  6,  6,
    6,  6,  6,  6, 68,  6,  6,  6,  6,  6,  6,  6,  6,118, 14, 14,
    8,  8,  8,  6,  6,  6,  6,  8,  6,  6,  6,  6, 39, 39, 39, 39,
    6,  6,  6,  6,  6,  6,  6,  0,  6,  6, 46,  6,  7,  7, 95,119,
   22,  6, 46,  6,  7,  7, 95, 22,  6,  6,  0,  6,  7,  7,120, 22,
    6,  6,  6,  6,  7,  7,153, 22,  0,  6, 46,  6,  7,  7, 95,154,
   74, 74, 74, 74, 74,155, 18, 18, 84, 84, 84, 13, 57,121, 57,121,
   13, 13, 13, 13,156, 18, 18,157, 13, 13, 13, 13,122,123, 13,124,
  125, 13, 63, 64, 13, 13, 13, 13, 13, 77,125, 13, 13, 13, 13,158,
   18, 18,159, 18, 18, 18, 18, 18,160,  0, 10, 10, 10,  9, 63,126,
   10, 10, 10, 10, 10,  9, 63,126, 30, 30,127,  0,  0,  0,  0,  0,
    5,  5,  5,  5,  5,  5,161, 79, 94, 79, 94,  5,  5,  5,  5,  5,
    3, 85,  3, 96,  3, 21,  7,  6,  7,  8, 96,  3, 90,  7,  7,  3,
    3,  3, 85, 85, 85,  7,  7,162,  7,  7, 66,  1,128,  3,  6,  7,
    9,  9, 90,  6,  6, 49,  3,129, 12,163,164, 12,130,  3,  0,  0,
    9,  9, 47,  3,  3,  9,  3,  3, 47, 49,  3, 47,  3,  3,  3, 47,
    3,  3,  3,  3,  3,  3,  3,  9,  3, 47, 47,  3,  3,  3,  3,  3,
    3,  3,  9,  9,  9,  9,  9,  9,  3,  3,  3,  3, 26, 26,  3,  3,
    9,  3,  3,  3,165,166,  3,  3,  3,  3,  3,  3,  3,  3, 47,  3,
    3,  3,  3,  3,  3, 49,  9,  9,  9,  9,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  9,  9,  9,  3,  3,  3,  3,  3,  3,  3,
   10, 10, 10, 10, 10, 10,  3,  3,  3,  3,  3,  3,  3, 10, 10, 10,
    3,  3,  3, 49,  3,  3,  3,  3, 49,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  9,  9,  9,  9,  3,  3,  3,  3,  3,  3,  3, 49,
    3,  3,  3,  3, 26, 26, 26, 26, 26, 26, 26, 10, 10, 10, 10, 10,
   10, 10,  3,  3,  3,  3,  3,  3,  9,  9, 63,131,  9,  9,  9,  9,
    9,  9,  9, 26, 26, 26, 26, 26,  9, 63, 31, 31, 31, 31, 31, 31,
   31, 31, 31, 31,131,  9,  9,  9,  9,  9,  9,  9, 26, 26,  9,  9,
    9,  9,  9,  9,  9,  9, 26,  9,  9,  9, 47, 49,  9,  9, 47,  3,
    3,  3,  0,  3,  3,  3,  3,  3,  8,  7,  8, 21, 21, 21, 21,  7,
    8,  8, 21,  6,  6,  6, 14,  7,  8,  8,129,  3,  3, 96, 21,167,
    5,  8,  0,  0, 38, 13,116, 13,  6,  6,  6, 43,  0,  0, 43,  0,
    1,  1,  1,  1,  0,  0,  0, 58, 33,  0,  0,  0,  0,  0,  0, 27,
   13, 57, 57, 13,122,123, 57, 13, 13, 13, 13, 76, 13, 93, 57, 13,
   57, 26, 26, 26, 26, 13, 13, 61, 13, 13, 13, 13, 13, 84, 13, 13,
   93,168, 13, 13, 13, 13, 13, 13,  3, 13, 97, 31, 31, 31,169,  0,
   13, 26,  0,  0,  0,  0,  0,  0,  3,  3,  3,  3,  3, 54,  3,  3,
   87, 13,170,132, 26, 26, 26, 26, 26,  3, 26, 26, 26, 26,133,171,
  172, 12, 12, 12, 12,  5,  5, 16, 91, 14, 14,  3, 12,173, 40,  3,
    1,  1,  1, 15, 27,174,134, 52,  1,  1,  1,  1,  1, 40, 14, 52,
    0,  0, 17,  1,  1,  1,  1,  1,  3, 10, 10,  3,  3,  3,  3,  3,
    3,  3,  3,  0,  0,  0,  0, 54,  3,  3,  3,  3, 10, 10, 10, 10,
    1,  1, 44,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,135, 13,
   11, 11, 11, 11, 11,  1,  0,  0,  8,  8,  8,  8,  8,  8,  8, 24,
   79,175,  5,  5,  5,  5,  5, 61,  8,  8,  8,  8,  8,  8, 14,  5,
    1,  1,  1, 12, 12, 12, 12, 12,  5, 13, 13, 13,  0,  0,  0,  0,
   22, 22, 22,134, 14, 14, 14, 14, 22,  8,  8,  8,  8,  8,  8,  8,
   68,  6,  6,  6, 21, 21, 21,  8,  8,  8,  8,  8, 67,176, 21, 66,
    8,  8,  6,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  7,  7,  8,
    7,  7,  8,  8,  8,  8,  8,  8,  8,  8,  7,  7, 21, 21,  8,  8,
    8,  8,  8,  8,  8,  8,  7,  0,  0, 32,  0,  0,  0,  0,  0,  0,
   58, 14,177, 66, 14, 66,  1,  1,  1, 25,  1, 25,  1, 24,  1,  1,
    1, 34, 19, 20,  3,  3, 23,  0, 10, 10, 10,  3, 98,  0,  0,  0,
    1,  1, 13, 13,  0,  0,  0,  0, 16,  1,  1,  1,  1,  1,  1,  1,
    1,  1, 16, 16, 16, 16, 16, 16, 16, 16,  5,  0,  0,  0,  0, 13,
    5,  1,  1,  1, 13, 53, 53, 24,  1,  1,  1,  5,  5,  5,  5, 13,
    1,  1,  1, 24,  5,  5,  5,  5,  5, 16,  0,  0,  0,  0,  0, 38,
    1, 24, 16,  5,  5, 16,  5, 16, 83, 13, 13, 13, 13, 13, 13, 58,
    1,  1, 24, 52,  1,  1,  1,  1, 11, 11, 11, 11, 11,  1,  1, 15,
    1,  1,  1,  1, 24,  5,  5, 20, 19, 20, 19, 23,  0,  0,  0,  0,
    1, 24,  1,  1,  1,  1, 20,  0, 11, 11, 11, 11, 11,  0, 13, 13,
   52,  1,  1, 55,  3, 34, 20,  1, 25,  5, 25, 24, 25,  1,  1,  5,
   24, 15,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 17, 44, 13,
    1,  1,  1,  1,  1, 34,  5, 16, 13, 44,178, 23,  0,  0,  0,  0,
   17,  1,  1, 15, 17,  1,  1, 15, 17,  1,  1, 15,  0,  0,  0,  0,
    6,  6,  6,  6,  6,119, 14, 14,  6,  6,  6,  6,118, 22,  7,  0,
    1, 34, 19, 16, 20, 83, 19,  0,  1,  1,  1, 15,  0, 17,  1,  1,
    0, 43,  6,  6,  0,  0, 17, 25,  1,  1,  1,  1,179,  1,  1,  1,
    1,  1,  1, 15,  1,  1, 15, 15,  1, 17, 15,  1,  1,  1,  1,  1,
    1, 22, 22, 22, 22, 22, 22, 22, 22,109,  3,  3,  3,  3,  3,  3,
    3, 72,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1, 31,
    3,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  3,  3,  3,  3,
    1,  1,  1,  1,  1,  1, 98,  3, 13, 13, 13, 97, 64,  0,  0,  0,
   76,180,181, 31, 31, 31, 31, 31, 31, 31, 64, 97, 64, 13,124,182,
   13, 33, 13, 13,133, 31, 31, 64, 13,183,  9, 99, 80, 13,  0,  0,
    1,  1, 15,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1, 15, 73,
   38, 13,102, 13, 26,103, 76, 13,  6,  6,  6,  6,  6,107,108, 63,
   64, 26, 13,  1,  1,  1,  1,  1, 52,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1, 14,  0,  1,  1,  1,  0,  1,  1,  1,
    0,  1,  1,  1,  0,  1, 15,  0, 30,184, 69,127, 49,  9, 47, 29,
    0,  0,  0,  0, 73, 18,  3,  0,  1,  1,  1,  1,  1,  1, 17,  1,
    1,  1,  1,  1,  1, 15,  1, 17, 13, 33,  0, 48, 10, 10, 10, 10,
   10, 10,  0, 54,  3,  3,  3,  3, 12, 12,130, 10, 62,  3,  3,  3,
    3,  3,  3,  3,  3, 10,  3, 29, 29,  0,  0,  0,  0,  0,  0,  0,
    3,  3,  3,  3,  3,  3, 41,  0,136, 10, 10, 10, 10, 10, 10, 10,
   10, 10, 10, 10, 10, 10,  0,  0, 10, 10,  0,  0,  0,  0, 17,  1,
  132,  1,  1,  1,  1,100,  0,  0,  1,  1,  1,  5,  5, 23,  0,  0,
    1,  1,  1,  1,  1,  1,  1, 38,  1,  1,  0,  0,  1,  1,  1,  1,
  137, 12, 12,  0,  0,  0,  0,  0,  7,  7,  7,  7,  6,  6,  6,  6,
    7,  7,  0,  0,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  0,  0,
    1,  1,  0,  0,  0,  0,  0, 38,  7, 32,  7, 43,  6,  6,  6,  6,
    6, 43,  6,  6,  6,  6,  6,  6,  6, 43,  6,  6,  6, 43, 46,  0,
   14, 14, 14, 58, 14, 14, 14, 14, 82, 14, 14, 14, 14, 14, 14, 14,
    1,  1,  1,  0, 15,  1,  1,  1,  1,  1,  1, 17, 15,  0, 15, 17,
    1,  1,  1, 38, 10, 10, 10, 10,  1,  1,  1, 55, 75, 10, 10, 10,
    0,  0,  0, 48, 10, 10, 10, 10,  1, 15,  1,  0,  0, 48, 10, 10,
    1,  1,  1, 10, 10, 10,  0, 38,  1,  1,  1,  1,  1,  0,  0, 38,
    1,  1,  1,  1,  0,  0, 10,  1, 24,  5, 27, 23,  0,  0,  5,  5,
    1,  1, 17,  1, 17,  1,  1,  1,  1,  1,  1,  0,  5, 23,  0, 27,
   13, 13, 13, 13, 33,  0,  0,  0,  1,  1,  1,  1,  1,  1, 86, 78,
    1,  1,  1,  1, 72,  1,  1,  1,  1,  1, 24, 23,  0, 48, 10, 10,
   13, 13, 13, 33,  0,  0,  0,  0,  1,  1,  1,  0, 38, 13, 13, 13,
    1,  1,  1,  0, 10, 10, 10, 10,  1, 15,  0,  0, 10, 10, 10, 10,
    1,  0,  0,  0, 38, 13, 33,  0,  0,  0,  0,  0, 48, 10, 10, 10,
    7, 32,  0,  0,  0,  0,  0,  0,  6, 46,  0,  0,  0, 10, 10, 10,
    1,  1,  5,  5,  0,  0,  0,  0, 11, 11, 11, 11, 11,  1,  1, 52,
    7,  7,  7,  0, 27,  5,  5, 91,  6,  6,  6,  0,  0,  0,  0,  9,
   10, 10, 10, 10, 10, 10, 10, 51,  1,  1,  1,  1,  1, 27,185,  0,
    0,  1, 44,  1,120,186,  5,  5, 56,  3,  3,  3, 72,  1,  1,  1,
   10, 10, 10,114,  0,  0,  0,  0,136, 10, 78, 13, 13,  0,  0,  0,
    1,  5,  5, 13, 13,  0,  0,  0,  1,  1, 86, 10, 10, 10,  0,  0,
   19, 45,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  5,  5,  5,  5,
    5,  5,  5, 36, 13, 13, 13,  0, 10, 10, 10, 11, 11, 11, 11, 11,
   25, 24, 25,  0,  0,  0,  0, 27, 16, 19,  5, 20, 19, 36,187, 13,
   13, 23,  0,  0,  0,  0, 73,  0,  5, 25,  1,  1,  1,  1,  1,  1,
    1,  1,  1, 24,  5,  5, 19,  5,  5,  5, 23, 11, 11, 11, 11, 11,
   13, 13, 34, 45,  0,  0,  0,  0,  1, 24, 13, 15,  0,  0,  0,  0,
    1, 34, 16,  5,  5,  5,  5, 20, 45,  1, 40, 13, 70,  5, 36, 19,
   11, 11, 11, 11, 11, 40, 40, 13, 10, 10, 51,  0,  0,  0,  0,  0,
    5, 16, 20,  5, 13, 13, 13, 25, 24,  0,  0,  0,  0,  0,  0,  0,
    1,  1,  1, 15, 15,  1,  1, 17,  1,  1,  1,  1,  1,  1,  1, 17,
    1,  1,  1,  1, 40,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1, 24,
   16, 19,  5,  5,  5, 23,  0,  0,  5, 16, 17,  1,  1,  1, 15, 17,
   15,  1, 17,  1,  1, 27, 25, 16, 20, 16, 37, 35, 37, 35, 16,  0,
   15,  0,  0, 35,  0,  0, 17,  1,  1, 16,  0,  5,  5,  5, 23,  0,
    5,  5, 23,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1, 17,  0, 15,
    1,  1,  1, 17, 16, 19,  5,  5, 23, 37, 35, 35, 16, 37, 16, 20,
   25, 25, 13, 38, 33,  0,  0,  0, 27, 23,  0,  0,  0,  0,  0,  0,
    1,  1, 34, 16,  5,  5,  5,  5, 16,  5, 20, 25,  1, 40, 13, 13,
   11, 11, 11, 11, 11, 13, 38, 25, 16, 19,  5,  5, 20, 20, 16, 19,
   20,  5,  1, 53,  0,  0,  0,  0, 16,  5,  5,  0, 16, 16,  5, 19,
   36, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,  1,  1,  5,  0,
   16, 19,  5,  5,  5, 20, 19, 19, 36, 13, 15,  0,  0,  0,  0,  0,
   13, 13, 13, 13, 13, 13, 33,  0,  1,  1,  1,  1,  1, 24, 19, 16,
    5,  5,  5, 19, 40,  0,  0,  0, 11, 11,  0,  0,  0,  0,  0,  0,
    1,  1,  1,  1,  1, 15, 27, 19, 16,  5,  5, 19,  5,  5,  0,  0,
   11, 11, 11, 11, 11, 10, 13, 56,  5,  5,  5,  5, 19, 36,  0,  0,
   10, 51,  0,  0,  0,  0,  0, 17,  1,  1,  1, 15, 17,  0,  1,  1,
    1,  1, 17, 15,  1,  1,  1,  1, 16, 16, 16, 35, 37, 27, 20, 25,
   45, 19, 13, 33,  0,  0,  0,  0,  1,  1,  1,  1,  0,  1,  1,  1,
   34, 16,  5,  5,  0,  5, 16, 16, 25, 53, 37,  0,  0,  0,  0,  0,
   24,  5,  5,  5,  5, 25,  1,  1,  1, 24,  5,  5, 20, 24,  5, 36,
   13, 13, 13, 70,  0,  0,  0,  0, 24,  5,  5, 20, 19,  5,  1,  1,
    1,  1,  1,  1,  1,  5,  5,  5,  5,  5,  5, 20,  5, 13, 53, 13,
   13, 33,  0,  0,  0,  0,  0,  0, 13, 13, 13, 13, 13, 15,  0,  0,
   20,  5, 20, 20,  0,  0,  0,  0, 40,  0,  0,  0,  0,  0,  0,  0,
    5,  5,  5, 23,  5,  5,  5, 19, 40, 13, 13,  0,  0,  0,  0,  0,
   13,  1,  1,  1,  1,  1,  1,  1,  0,  5,  5,  5,  5,  5,  5,  5,
    5,  5,  5,  5, 35,  5,  5,  5, 20,  5, 19, 23,  0,  0,  0,  0,
    1,  1,  1, 15,  1, 17,  1,  1, 24,  5,  5, 23,  0, 23,  5, 27,
    5,  5,  5, 24,  0,  0,  0,  0,  1,  1,  1, 17, 15,  1,  1,  1,
    1,  1,  1,  1,  1, 16, 16, 37,  5, 35, 19, 19, 15,  0,  0,  0,
    1,  1,  1,  1, 44,  1,  0,  0,  1, 24, 20, 83, 33,  0,  0,  0,
    5, 34,  1,  1,  1,  1,  1,  1,  1,  1, 16,  5,  5, 23,  0, 16,
   20, 36, 13, 13, 13, 13, 13, 13, 11, 11, 11, 11, 11, 23,  0,  0,
   10, 10, 62,  3,  3,  3, 69, 30, 98,  3,  3,  3,  3,  3,  3,  3,
    3,  0,  0,  0,  0,  0,  0, 38, 13, 13,137, 12, 12, 12, 12, 12,
   12, 12, 12,100,  0,  0,  0,  0, 40, 33,  0,  0,  0,  0,  0,  0,
   25,  1,  1, 24,  5,  5,  5,  5,  5,  5,  5,  0,  0,  0,  0,  0,
    5,  5,  5,  5,  5, 16, 19,  5,  5,  5, 36,  0,  0,  0,  0,  0,
    5,  5,  5, 36, 13, 13,  3,  3, 14, 14, 56,  0,  0,  0,  0,  0,
   11, 11, 11, 11, 11, 48, 10, 10, 10, 17,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  0,  0, 17,  1, 14, 52,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1, 44,135, 13, 10, 10, 10, 78, 13, 33,  0,  0,
    7,  7,  7,  7, 32, 43,  6,  6,  6,  6,  0,  0,  0,  0,  0,  0,
    1,  1,  1,  1,  1, 15,  0, 27, 34, 16, 16, 16, 16, 16, 16, 16,
   16, 16, 16, 16,  0,  0,  0, 27,  5, 81, 14, 14, 14, 14, 14, 14,
   14, 61, 23,  0,  0,  0,  0,  0, 16, 14, 12,100,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0, 17, 14, 14, 58, 14, 14, 14, 58, 82,
    0, 15,  0,  0,  0,  0,  0,  0,  1, 15, 17,  0,  0,  0,  0,  0,
    0,  0,  1,  1, 15,  0,  0,  0,  1,  1,  1,  1,  1,  0, 41, 36,
   18, 18,  0,  0,  0,  0,  0,  0, 11, 11, 11, 11, 11,  3, 29,  0,
    3,  3,  0,  0,  0,  3,  3,  3, 29,  3, 29,  0,  0,  0,188,  9,
    9,  9,  9,  9,  9,  9,  9,  0,  5,  5,  5,  5,  5,  5,  5,  0,
    3,  3,  3, 41, 50,  3,  3,  3,  3,  3,101, 19,  5,  3,101, 16,
   16,189, 18, 18, 18, 92,  5,  5,  5, 50, 41,  5,  5,  5,  3,  3,
    3,  3,  3,  3,  3,  5,  5,  3,  3,  5, 50,  0,  0,  0,  0,  0,
   16,190,  3,  3,  3, 41, 50,101, 16,  0,  0,  0,  0,  0,  0,  0,
    6,  6, 46,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6, 32,  7,
    0, 32, 39, 32, 39,  7, 32,  7,  7,  7,  7,  6,  6, 43, 43,  6,
    6,  6, 43,  6,  6,  6,  6,  6,  6,  6,  7, 39,  7, 32, 39,  7,
    7,  7, 32,  7,  7,  7, 32,  6,  6,  6,  6,  6,  7, 39,  7, 32,
    7,  7, 32, 32,  0,  7,  7,  7, 32,  6,  6,  6,  6,  6,  6,  6,
    6,  6,  6, 46,  7,  7,  7,  7, 59,  6,  6,  6,  6,  6,  6,  6,
    6,  6,  6,  6,  6, 60,  6,  6,  6,  7,  7,  7,  7,  7,  7,  7,
    7,  7,  7,  7,  7, 59,  6,  6,  6,  6, 60,  6,  6,  6,  7,  7,
    7,  7, 59,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6, 60,
    6,  6,  6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7, 59,
    6,  6,  6,  6, 60,  6,  6,  6,  7,  7,  7,  7, 59,  6,  6,  6,
    6, 60,  6,  6,  6,  8,  0, 11,  5,  5,  5, 50,  3, 41,  5,  5,
    5,  5,  5,  5,  5,  5, 50,  3,  3,  3, 41,  3,  3,  3,  3,  3,
    3,  3, 50, 65, 13, 13,  0,  0,  0,  0,  0,  0,  0, 27,  5,  5,
    9,  9,  9,  9,  9,  9, 99,  0,  6,  6,  6,  6,  6,128,  6,  6,
    8,  6,  6,  6,  8,  8, 21,  6, 21,  6,  6,  6,  6,  6,  6,  6,
    6,  6,  6,  6,  8,  8,  8,  8,  0,  0,  0,  0,  0,  0, 58, 14,
    5,  5,  5, 23,  5,  5,  5,  5,  5,  5,  5,  5, 23, 27,  5,  5,
    5, 27, 23,  5,  5, 23,  0,  0, 14, 14, 14, 14, 14, 14, 14,  0,
    0,  0,  0,  0,  0,  0,  0, 27,  5,  5,  5, 81, 14, 14, 14,  0,
   11, 11, 11, 11, 11,  0,  0, 55,  1,  1,  1,  1,  1,  1,  1, 23,
    1,  1,  1,  1,  1,  1,  5,  5, 11, 11, 11, 11, 11,  0,  0,115,
    1,  1,  1,  1,  1, 44,  5,  5,191, 11, 11, 11, 11,192,  0, 38,
    1, 24,  1, 25,  1,  1,  1,  5,  1,  1, 24,  0,  0,  0,  0, 44,
    1,  1,  1, 15,  1,  1, 17, 15,  1,  1, 15, 48, 10, 10, 10, 10,
    6,  6,  5,  5,  5, 81,  0,  0, 10, 10, 10, 10, 10, 10, 75, 10,
  193, 10, 51,  0,  0,  0,  0,  0, 10, 10, 10, 10, 10, 10, 10, 75,
   10, 10, 10, 10, 10, 10, 10,  0,  1,  1, 17,  1,  1,  1,  1,  1,
   17, 15, 15, 17, 17,  1,  1,  1,  1, 15,  1,  1, 17, 17,  0,  0,
    0, 15,  0, 17, 17, 17, 17,  1, 17, 15, 15, 17, 17, 17, 17, 17,
   17, 15, 15, 17,  1, 15,  1,  1,  1, 15,  1,  1, 17,  1, 15, 15,
    1,  1,  1,  1,  1, 17,  1,  1, 17,  1, 17,  1,  1, 17,  1,  1,
    9,  0,  0,  0,  0,  0,  0,  0, 10, 10, 10, 10, 10, 10, 62,  3,
    0,  0,  0,  3,  3,  3,  3,  3,  3, 29,  0,  0,  0,  0,  0,  0,
    3,  3,  3,  3, 29,  0,  0,  0,  3,  3,  3,  3,  3,110, 22, 22,
    3,  3,  3,  3,  3,  0,  3,  3,  9,  9,  9,  9, 99,  0,  0,  0,
    3,  3,  3, 29, 29,  0,  3,  3,  3,  3,  3,  3,  3,  3,  3, 54,
    3,  3,  3,  3,  3,  3,  0, 54,  3, 29,  3,  3,  3,  3,  3,  3,
   11, 11, 11, 11, 11, 29,  0,  0, 73,  0,  0,  0,  0,  0,  0,  0,
    2,  2,  2,  2,  2,  2,  2,  0,  2,  2,  7,  7,  3,  3, 26, 26,
    4,  4, 12, 12,  5,  5,  9,  9,  9,  5, 25, 25, 15, 15, 13, 13,
   14, 14, 21, 21,  6,  6,  7,  2, 10, 10,  2,  7,  1,  1, 10, 12,
   12, 10,  5,  9, 24, 24, 12,  2,  7, 12, 12,  7, 22, 18,  2, 12,
    0,  0, 26,  2, 23, 23, 18, 22,  9,  2, 21,  2,  7, 10,  2, 10,
   12, 21, 10,  2,  2, 21,  2,  9,  7, 21, 26, 12,  8,  8,  2,  5,
    7,  6, 10,  7,  5,  2, 25, 26,  2, 15, 26, 25, 12, 26, 15,  2,
    6,  7, 21,  7,  2, 26,  7, 26, 21, 26, 20, 19,  2,  6,  9, 25,
    5, 25, 21,  6, 15, 26, 25, 22, 18, 21, 26, 21,  5,  7,  6, 24,
    6,  5, 26, 23, 21, 12,  6, 12, 26,  7,  2,  1, 29, 29, 26, 15,
   21, 17, 25, 21, 15, 21, 11, 11, 21, 23, 12,  6,  6,  2, 10, 21,
   17, 17,  9, 26,  7, 15, 29, 21, 21,  9, 24,  5, 25,  9, 17,  6,
    1, 12, 17, 21, 11, 12,  8, 24, 26,  9, 21, 22, 23, 26, 25,  2,
   14,  2, 26, 10, 23, 21, 21, 25,  9, 22, 21, 18, 24, 16,  5, 22,
   25, 18, 24, 26, 26, 24,  9,  8,  8,  5,  5, 21, 15,  7,  2, 23,
   21, 15, 17,  7,  5,  6,  5, 24,  2, 24, 22, 20, 21, 20, 19, 21,
   21, 16, 16, 21, 18,  6, 23,  2,  7,  5,  5, 26, 14, 15, 18, 25,
    7, 14, 17, 22, 24,  6,  6, 21, 12, 15, 21, 14, 25,  0,  7, 20,
   25,  1, 24, 15,  7, 19,  9, 21,  6, 26, 17, 12,  1, 21, 12,  1,
   24,  7, 29,  7,  7, 22, 18,  2, 14,  7,  9, 24, 24,  2, 29,  1,
   27, 28,  1, 29, 21, 29,  1,  2, 15,  6, 12, 11, 26,  5, 14,  9,
    5, 14, 26, 22, 18, 26,  5, 12, 22, 21, 18, 17, 26,  6, 18, 18,
   26, 14, 14,  6, 12, 24, 11, 21, 24,  9,  6,  9,  6, 10,  7, 25,
   17, 16, 16, 22, 16, 16, 25, 17, 25, 24, 12, 17, 24, 12, 21,  1,
    2, 25, 10,  1, 10, 26,  7, 13, 13,  2, 23, 15,  0,  1,  2,  3,
    4,  5,  6,  7,  8,  9,  0, 10, 11, 12, 13,  0, 14,  0,  0,  0,
    0,  0, 15,  0, 16,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 17,
   18, 19,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0, 20,  0, 21, 22, 23,  0,  0,
    0, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 35,  0,  0,  0,
    0, 36,  0, 37,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 38,  0,  0,  0,  0,  0,
    0,  0,  0,  0, 39, 40,  0,  0,  0,  0,  0,  0, 41, 42, 43, 44,
   45,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0, 11, 12, 13, 14,  0,  0,  0,
    0,  0,  0,  0,  0, 15,  0,  0,  0,  0,  0,  0,  0, 16, 17, 18,
    0, 19,  0, 20, 21,  0,  0, 22, 23, 24, 25, 26,  0,  0,  0,  0,
   27, 28, 29, 30,  0, 31,  0, 32, 33,  0,  2, 34,  0,  0,  2,  1,
   35,  0,  2,  1,  0,  0,  2,  1,  0,  0,  2,  1,  0,  0,  0,  1,
    0,  0,  2, 36,  0,  0,  2,  1,  0,  0, 37,  1,  0,  0,  0, 38,
    0,  0, 39, 40,  0,  0, 41, 42,  0, 43, 44,  0, 45, 46,  0, 47,
    0,  0, 48,  0,  0, 49,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 50,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0, 51,  4,  0,  0,  0,  0, 52,
    0,  0,  0,  0,  0,  0, 53,  0,  0,  0, 54,  0,  0,  0,  0,  0,
    0, 55,  0,  0, 56,  0, 57, 58, 59,  0, 60,  5, 61,  0, 62,  0,
   63,  0, 64,  0,  0,  0,  0, 65, 66,  0,  0,  0,  0,  0,  0, 67,
   68,  0,  0,  0,  0,  0,  0, 69, 70,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0, 71,  0,  0,  0,  3,  0,  0,  0,
   72,  0, 73,  0,  0, 74,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0, 75, 76,  0,  0, 77,  0,  0,  0,  0,  0,  0,  0,
    0,  6, 78,  0,  0,  0,  0,  5, 79,  0, 80, 81,  0,  0,  7,  8,
    0,  0,  0,  0,  0,  0, 82, 83, 84,  0,  0,  0,  0,  0,  0,  0,
    1,  0,  0,  0,  0,  0,  0,  0,  0, 85,  0,  0,  0,  0,  0,  0,
    0,  0, 86,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   87,  0,  0,  0,  0,  0,  0,  0, 88,  0,  0,  0, 89,  0,  0,  0,
    0, 90, 91,  0,  0,  0,  0,  0, 92,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 93,  0, 94,  0,  0,  0,
    0,  0,  0,  0,  0,  0, 95, 96, 97,  0,  0, 98,  0, 99,  0,  0,
    0,  0,  0,  6,100,  0,  9,  0,  0,101,102,  0,  7,  0,  0,103,
    0,  0,104,  0,  0,  0,  0,  0,105,  0,106,  1,107,  0,  0,108,
    0,  0,  0,109,  0,  0,  0,110,  0,  0,  0,  0,  0,  0,  3,111,
    0,  0,  3,  0,  0,  0,112,  0,  0,  0,113,  0,  0,  0,  0,  0,
    0,  0,  9,  0,  0,  0,  0,  0,  0,  0,114,115,  0,  0,  0,  0,
    8,  0,  4,116,  0,117,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  3,  0,  0,  0,  0,  0,  0,  0,  0,118,  0,119,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,120,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,121,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,122,  0, 10,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  123,  0,  0,  0,  0,124,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,125,  0,126,127,128,  0,
    0,  0,  0,129,  0,130,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,131,132,  0,  0,133,  0,  0,  0,  0, 10,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,134,  0,135,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,136,  0,  0,  0,  0,  0,  0,  0,
  137,  0,  0,  0,  0,  0,  0,  0,138,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,139,  0,  0,  0,140,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  4,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  9,  0,  0,  0,  0,  0,  0,  0,  2,  0,  0,  0,  0,
    0,  3,  0,  0,  0,  3,  0,  0,  0,  0,  0,  0,  0, 14,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0, 15,  0,  0,  0,  3,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 35,  0,  0,  0,  0,  0,
    1, 12,  0,  0,  1,  1,  1,  1,  1, 49, 50,  6, 51, 52, 53,  6,
    6, 25, 36, 26,  1, 54, 17, 55, 16, 27, 56, 57, 58,  1,  1,  1,
    0,  0,  0,  0,  5,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   59,  7, 60,  1, 18,  6,  7, 61, 62, 63, 64, 65, 66, 67, 68,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  1,  1, 69,  0,  0,  0, 70, 71,
   72, 37,  1, 28,  0,  0,  0,  0, 73,  0,  0,  0,  0,  0,  0,  0,
    0,  8,  1, 29, 16, 29, 74, 38,  0,  0,  0,  0,  0,  0,  0,  0,
   75,  0,  0,  0,  0,  0,  0,  0, 30, 28, 76, 31,  7, 31, 77,  0,
    0,  0,  0,  0,  0,  0,  5,  1,  7,  0,  0, 19,  0,  0,  0,  0,
    0,  8, 39,  1,  1, 13, 13, 11,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0, 40,  0,  0,  0,  0,  0,  0,  5, 20,  1,  0,  0,  8, 16,
    6,  1,  1,  1, 78,  7, 30, 20, 79,  7, 37,  1,  0,  0,  0,  4,
   80, 10,  0,  0,  0,  0,  0,  0,  0,  0,  0, 21,  0,  0,  0,  4,
    0, 81,  0,  0,  0,  0,  0,  0,  0,  0,  2,  3,  0,  0, 14,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 82,  0,  0,  0, 83,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 84,  0,  0,  0, 85,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 41,  0,  0,  0,  0,  0,
    0, 86, 87,  0,  0,  0,  0,  0, 88, 89, 90, 91, 92, 93,  0,  0,
    0,  0,  0,  0,  0, 94,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0, 15, 32,  0,  0,  0,  0, 19,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0, 13,  0,  0,  0,  0,  0, 42,  0,  0,  0,  0,  0,  0,
   14,  0,  0, 22,  0,  0, 95,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0, 96,  0,  0,  0,  0,  0,  0,  5, 23,  0,  3,  0,  0,  0,
    0, 13,  1, 97,  0,  0,  0,  0,  1, 20, 27, 98, 28, 26,  7,  1,
    1,  1,  1, 30,  1,  7, 99, 18, 10,  0,  0,  0,  0,  0,  0,  0,
    0,  9,  0,  0,  0,  0,  5, 26,  1,  0,  0,  0,  0,  0, 33,  0,
    0,  0,  0,  0,  0, 24,  0,  0, 33,  0,  0,  0,  0,  0,  0,  0,
    0, 15,  0,  0,  0,  0,  0,  0, 12, 36, 17,  6,100, 25, 43, 19,
    0, 10, 11,  0,  7,  1,  7,101,102,  1,  1,  1,  1,  1,  1,  1,
    1,103,104,105,  0,  0,  0,  0,106,  1,107, 10, 22,108,109,  6,
   10,  0,  0,  0,  0,  0,  0,  5, 11,  0,  0,  0,  1,  1,  1,  1,
    1,  1,  1,  1,  0,  0,110,111,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,112,  0,  0,  0,  0,  5,  0,  1,  1, 11,  0,  0,  0,  0,
    0,  0,  0,  8,  0,  0,  0,  0, 11,  0,  0,  0,  0,  0,  0,  3,
    0,  0,  0,  0,  1,  1,  1,  1, 11,  0,  0,  0,  0,  0, 34, 41,
    0,  0,  0,  0,  0,  0,  0,  0,  2,  0,  0,  0,  0,  0,  0,  0,
   44,113, 10,  8, 22,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0, 14,  0,  0,  0,  0,  0,  0,  0,  0,  0,114,  1, 16,  6, 17,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 19, 23,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  8, 12,  0,  0,  0,  0,115,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,116,  2,  0,117,  0,  0,
    0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0,  0, 13, 11,
    0,  0,  0,  0,  0,  0,  5, 10,  0,  0,  0,  0,  0,  0,  5, 17,
    0,  0,  0,  0,  0,  0,  0,  0, 27, 31, 18, 40,  0,118, 16, 20,
   23,  0,  0,  0, 45, 38,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    3,  0,  0,  2, 12,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    2,  3,  0,  0,  3,  0, 24,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0, 35,  0,  0,  0,  0,119,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0, 15,  9,  0,  8,  1, 10,  1, 10,  0,  0,  0,  0,  0, 33,
    3,  0,  0,  0, 14, 24,  0,  0,  0,  0,  0, 21, 46,  0,  0,  0,
    0,  0,  0,  0,  9,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0, 46,  0,  0,  0,  0,  2,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0, 32, 15,  0,  0,  0,  0,  0,  0,  0,  0,  2,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  4,  0, 24, 42,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  2,  0,  0, 32,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  2,  0,  0,  0,  0,  0,  0,  0,  0,
   25, 43,  0,  0,  0,  0,  0,  0,120,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,121,  0, 34, 23,  0,  0,  0,  0,  0,  0,122,123,124,
   47,  0, 34,  6, 48, 13, 18,  0,  0,  0,  0,  0,  0,  0,  8, 11,
    0,  0,  0,  0,  8, 10,  0,  0, 47,  0,125,126,127,  0,  0,  0,
    0,  0,  0,  0,  1, 12,  1,  1,  1,  1, 29,  1, 39, 44, 12,  0,
    0,  0,  0,  0,  0,  0,  0,  5,  0,  0,  0,  0,  0,  0,  0, 21,
    0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0,128,
    0,  0,  0,  0,  0,  0,  0, 45,  0,  0,  0,  0,  5, 21,  0,  8,
    0, 22,  0,  0,  0,  0,  0,  0,  6, 48,  0,  0,  0,  1,129,  0,
    0,  0,  0,  0,230,230,230,230,  0,  0,  0,  9,  9,  0,  0,  0,
    0,  9,  0,  0,  0,  0,  0,230,220,220,220,220,230,230,220,230,
    0,  0,230,230,  7,  0,  0,  0,230,  0,  0,  0,230,230,  0,  0,
  230,230,230,  0,  0,230,230,230,  0,  0,  9,  0,  0,  0,  0,  7,
  230,230,230,220,220,220,230,230,230,230,220,220,  0,220,  0,  0,
  230,220,220,220,  0,  0,230,  0,  0,230,  0,  0,220,  0,  0,  0,
    0,  0,  7,  0,  1,  1,  1,  1,220,230,230,230,220,220,220,230,
  220,230,230,220,230,  0,  0,230,230,220,230,230,220,230,220,230,
    0,  9,  9,  0,  0,  0,  9,  9,  0,  0,  0,220,  0,  9,  7,  0,
    1,220,220,220,230,220,220,230,230,220,  0,  0,230,230,  0,230,
    0,220,220,220,220,220,  0,  0,  9,  9,  0,  0,  1,  0,  0,  0,
  230,  0,230,230,  0,  0,230,220,  0,  0,  9,  7,216,216,216,  0,
  220,220,220,  0,230,232,220,220,220,220,232,216,220,202,202,220,
  220,220,220,202,202,220,220,220,230,240,230,220,230,220,220,  0,
  232,220,220,230,233,234,234,233,234,234,233,230,  0,220,230,230,
  230,230,222,220,230,222,228,230, 10, 11, 12, 13, 14, 15, 16, 17,
   18, 19, 19, 20, 21, 22,  0, 23,  0, 24, 25,  0,230,220,  0, 18,
   10, 21,  0,  0, 30, 31, 32,  0,  0,  0,  0, 27, 28, 29, 30, 31,
   32, 33, 34,230, 35,  0,  0,  0,230,  0,220,230,  0, 36,  0,  0,
  220,220,230,220,220,230,230,  0,230,230,  0,220, 27, 28, 29,230,
    0,230,220,230,  0, 84, 91,  0,103,103,  9,  0,107,107,107,107,
  118,118,  9,  0,122,122,122,122,  0,220,  0,220,  0,216,  0,  0,
    0,129,130,  0,132,  0,  0,  0,  0,  0,130,130,130,130,  0,  0,
  130,  0,230,230,  9,  0,230,230,  0,  0,220,  0,  0,228,  0,  0,
    0,222,230,220,230,  0,  0,220,230,220,  0,220,230,230,230,234,
  230,  0,  1,  1,230,234,214,220,202,230,230,230,230,230,232,228,
  228,220,218,230,233,220,230,220,230,230,  1,  1,  1,  1,  1,230,
    0,  1,  1,230,220,230,  1,  1,  0,  0,218,228,232,222,224,224,
    0,  8,  8,  0,220,  0,  0,230,  0,  0, 26,  0,  0,220,  0,230,
  230,  1,220,  0,  0,230,220,  0,  0,  0,220,220,  0,  7,  9,  0,
    6,  6,  0,  0,  0,  0,  1,  0,  0,216,216,  1,  1,  1,  0,  0,
    0,226,216,216,  0,  0,  0,  1,  1,  0,  0,216,216,216,  0,  0,
  232,232,220,230,230,230,  7,  0,  1,  0,  0, 32,  0, 48,  0,  0,
   84, 96,135,144, 10,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,176,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,192,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    2,  1,  1,  0,  3,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  4,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  5,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    6,  7,  8,  9,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 10,
   11, 12, 13, 14, 15, 16, 17, 18, 19,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 20,  0,  0, 21, 22,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 23, 24, 25, 26,  0,
   27,  0, 28, 29, 30, 31, 32,  0,  0,  0,  0,  0,  0,  0, 33, 34,
   35, 36, 37,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 38,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0, 39, 40,  0,  0,  0,  0,  2,  1, 41, 42,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0, 43,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0, 32, 36,  0,  0,  8,  0,  0,  0,  0, 44,
    0,  0, 52,  0,  0,  0, 56,  0,  0,  0,  0,  0,  0,  0, 16,  8,
    0,  0,  0,  0,  0,  0, 24, 48,  0,  0,  0,  0,  0,  0, 28,  0,
    0, 28,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 28,
    0,  0,  0, 28,  0,  0,  0,  0,  0,  0, 60, 64,  0, 68,  0, 72,
   76, 80,  0,  0,  0,  0,  0,  8, 84, 88,  0, 92, 16,  8,  0,  0,
    0,  4,  4, 16,  4,  4,  4,  4,  4,  4,  4, 24, 20,  0, 96,  0,
   16,100,104,  0,  4,  4,108,  0,  0,  0, 40,112,  8, 16,  4,  4,
    4,  4,  4,  8,116,120,124,128,  0,  0,  4,  0,  0,  0,  0,  0,
    0,  0, 28,  0,  0,  0,  0,  0,  0,  0,  4,  4,  4,  8,  0,  0,
   24, 20,132, 36,  0, 28,  0,136, 16,  4,  4,  4,  0,  0,  0,  0,
   24, 12, 12,140,144, 12,148,  0,152,  8,  4,  4,  0,  0,156,  0,
    8,  8,  0, 24, 20,  8,  4,  0,  0,  0,  8,  0,  0,160,  8,  8,
    0,  0, 24, 20,  0,  8,  0,  8,  0,  8,  0,  0,  0,  0, 40, 12,
   12, 12, 12, 12, 12, 12, 12, 12, 20, 16,  4,164, 12, 12, 12, 12,
   12, 12, 12, 12, 12, 20,  0,168,172,176,  0,180,  0, 24, 20,  0,
    0,  0,  0,  0,  0,  0,  0,184, 16,  8, 28,  8,  0,  0,  0,  8,
    4,  4,  8,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 40, 12, 48,
   16,  0,  0,  0,  0,  0,  0,  0,  0,  0,  4,  4,  8,  4,  4,  0,
    0,  0,  0,  0,  0,  0, 40, 20,  0,  8,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0, 32,188,192,  0,  0,  0,  0,  0,  0,  0,
   24, 20,  8,  0,196,  0,  0,  0, 19, 19, 19, 19, 20, 18, 20, 18,
   20, 18, 19, 19, 18, 20, 18, 20, 19, 19, 20, 18, 18, 20, 18, 19,
   19, 19, 19, 20, 19, 20, 18, 19, 19, 19, 19, 21, 19, 17, 19, 19,
   19, 20, 18, 20, 21, 19, 17, 19, 18, 19, 19, 19, 19, 19, 19, 25,
   19, 19, 19, 13, 22, 22, 22, 16, 16, 16, 19, 19, 19, 32, 19, 19,
   19, 38, 19, 37, 31, 29, 30, 19, 36, 19, 19, 19, 19, 19, 19, 26,
   19, 23, 19, 19, 15, 19, 19, 19, 28, 19, 19, 19, 19, 19, 33, 19,
   35, 34, 19, 34, 27, 19, 19, 19, 18, 12, 19, 19, 20, 18, 24, 24,
   24, 19, 23, 23, 19, 19, 14, 14, 14, 15, 15, 19, 20, 18, 19, 21,
   11, 20, 18, 19, 18, 22, 20, 18, 16, 20, 18, 20, 18, 19, 19,  9,
    8, 19, 19,  7, 10, 19, 19, 19, 19,  6, 19, 19, 20, 18, 19, 20,
   19, 19,  5, 19, 19, 19, 19,  4,  3,  4, 19, 19, 20, 18,  2, 19,
   19, 19,  1, 19, 19, 17, 19, 20, 18, 19, 20, 18,  0, 19, 19, 19,
    6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18,  1,  1,  1,
    1,  1,  1, 19,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1, 20, 21, 22,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3, 23,  0,  0,  0,  0,  0,  0,  0,  0, 24, 25,
   26, 27, 28, 29, 30, 31, 32, 33, 34, 35,  0, 36,  5, 37,  5,  5,
   38, 39,  0,  0,  0,  0,  0,  0, 40,  0, 41, 42,  4,  4,  4,  4,
    4,  4, 43, 44, 45,  0,  0,  0,  0,  0,  0, 46, 47,  0,  0, 48,
    0,  0,  0, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60,  0,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1, 61,  1,  1,  1,  1, 62,  1,
    1,  1,  1, 63,  1,  1,  1,  1,  1,  1, 64, 65,  0,  0, 66,  0,
    1,  1,  1,  1, 67,  1,  1,  1,  1,  1,  1,  1,  1, 68,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 69,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   70,  2,  2,  2,  2,  2,  2,  2,  2, 13, 13, 13, 13, 13, 13, 13,
   13, 12, 12, 12, 12, 12, 12, 12, 12,178,178,178,178,178,178,178,
  178,  0,  0,  0,  0,  0,  0,  0,  0,136,136,136,136,136,136,136,
  136, 80, 80, 80, 80, 80, 80, 80, 80, 19, 19, 19, 19, 19, 19, 19,
   19, 63, 63, 63, 63, 63, 63, 63, 63,  3,  3,  3,  3,  3,  3,  3,
    3, 39, 39, 39, 39, 39, 39, 39, 39,176,176,176,176,176,176,176,
  176, 28, 28, 28, 28, 28, 28, 28, 28, 79, 79, 79, 79, 79, 79, 79,
   79,131,131,131,131,131,131,131,131,  1,  1,  1,  1,  1,  1,  1,
    1,127,127,127,127,127,127,127,127,  6,  6,  6,  6,  6,  6,  6,
    6,155,155,155,155,155,155,155,155,139,139,139,139,139,139,139,
  139,  9,  9,  9,  9,  9,  9,  9,  9, 15, 15, 15, 15, 15, 15, 15,
   15, 30, 30, 30, 30, 30, 30, 30, 30,110,110,110,110,110,110,110,
  110, 77, 77, 77, 77, 77, 77, 77, 77, 17, 17, 17, 17, 17, 17, 17,
   17, 27, 27, 27, 27, 27, 27, 27, 27, 33, 33, 33, 33, 33, 33, 33,
   33,113,113,113,113,113,113,113,113, 49, 49, 49, 49, 49, 49, 49,
   49, 25, 25, 25, 25, 25, 25, 25, 25, 29, 29, 29, 29, 29, 29, 29,
   29,177,177,177,177,177,177,177,177,  7,  7,  7,  7,  7,  7,  7,
    7, 32, 32, 32, 32, 32, 32, 32, 32,  8,  8,  8,  8,  8,  8,  8,
    8, 99, 99, 99, 99, 99, 99, 99, 99, 31, 31, 31, 31, 31, 31, 31,
   31, 30, 30, 30, 30, 30, 30, 30,  2,105,105,105,105,105,105,105,
  105, 55, 55, 55, 55, 55, 55, 55, 55, 62, 62, 62, 62, 62, 62, 62,
   62,  0,  0,  0,  0,  2,  2,  2,  2, 56, 56, 56, 56, 56, 56, 56,
   56, 91, 91, 91, 91, 91, 91, 91, 91,100,100,100,100,100,100,100,
  100,119,119,119,119,119,119,119,119, 14, 14, 14, 14, 14, 14, 14,
   14,130,130,130,130,130,130,130,130, 94, 94, 94, 94, 94, 94, 94,
   94,157,157,157,157,157,157,157,157,137,137,137,137,137,137,137,
  137,146,146,146,146,146,146,146,146,  4,  4,  4,  4,  4,  4,  4,
    4, 20, 20, 20, 20, 20, 20, 20, 20, 35, 35, 35, 35, 35, 35, 35,
   35, 84, 84, 84, 84, 84, 84, 84, 84, 40, 40, 40, 40, 40, 40, 40,
   40, 97, 97, 97, 97, 97, 97, 97, 97,124,124,124,124,124,124,124,
  124,123,123,123,123,123,123,123,123,125,125,125,125,125,125,125,
  125,140,140,140,140,140,140,140,140,159,159,159,159,159,159,159,
  159,132,132,132,132,132,132,132,132, 37, 37, 37, 37, 37, 37, 37,
   37, 22, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24, 24, 24, 24,
   24, 58, 58, 58, 58, 58, 58, 58, 58, 76, 76, 76, 76, 76, 76, 76,
   76,  0,  0,  0,  0,  0,  0,  2,  2, 26, 26, 26, 26, 26, 26, 26,
   26, 75, 75, 75, 75, 75, 75, 75, 75, 89, 89, 89, 89, 89, 89, 89,
   89,114,114,114,114,114,114,114,114,141,141,141,141,141,141,141,
  141,133,133,133,133,133,133,133,133,163,163,163,163,163,163,163,
  163, 70, 70, 70, 70, 70, 70, 70, 70,  0,  0,  0,  0,  0,  2,  2,
    2, 68, 68, 68, 68, 68, 68, 68, 68, 92, 92, 92, 92, 92, 92, 92,
   92, 87, 87, 87, 87, 87, 87, 87, 87,  0,  0,  0,  0,  0,  0,  0,
    2,135,135,135,135,135,135,135,135, 85, 85, 85, 85, 85, 85, 85,
   85, 96, 96, 96, 96, 96, 96, 96, 96,109,109,109,109,109,109,109,
  109,102,102,102,102,102,102,102,102, 64, 64, 64, 64, 64, 64, 64,
   64,  0,  0,  2,  2,  2,  2,  2,  2, 61, 61, 61, 61, 61, 61, 61,
   61, 65, 65, 65, 65, 65, 65, 65, 65,108,108,108,108,108,108,108,
  108,142,142,142,142,142,142,142,142,121,121,121,121,121,121,121,
  121,166,166,166,166,166,166,166,166,167,167,167,167,167,167,167,
  167,151,151,151,151,151,151,151,151,152,152,152,152,152,152,152,
  152, 38, 38, 38, 38, 38, 38, 38, 38, 90, 90, 90, 90, 90, 90, 90,
   90, 23, 23, 23, 23, 23, 23, 23, 23, 36, 36, 36, 36, 36, 36, 36,
   36, 30,  2, 30, 30, 30, 30,  2,  2, 48, 48, 48, 48, 48, 48, 48,
   48, 93, 93, 93, 93, 93, 93, 93, 93, 73, 73, 73, 73, 73, 73, 73,
   73,  9,  9,  9,  9,  9,  9,  2,  2, 86, 86, 86, 86, 86, 86, 86,
   86,  2,  0,  0,  0,  0,  0,  0,  0, 67, 67, 67, 67, 67, 67, 67,
   67, 51, 51, 51, 51, 51, 51, 51, 51,104,104,104,104,104,104,104,
  104,170,170,170,170,170,170,170,170, 78, 78, 78, 78, 78, 78, 78,
   78,144,144,144,144,144,144,144,144,165,165,165,165,165,165,165,
  165,171,171,171,171,171,171,171,171,126,126,126,126,126,126,126,
  126,150,150,150,150,150,150,150,150,134,134,134,134,134,134,134,
  134,138,138,138,138,138,138,138,138,175,175,175,175,175,175,175,
  175, 18, 18, 18, 18, 18, 18, 18, 18,  0,  0,  0,  0,  0,  2,  0,
    0, 13, 13, 13, 13, 13, 13,  2,  2, 60, 60, 60, 60, 60, 60, 60,
   60, 69, 69, 69, 69, 69, 69, 69, 69,118,118,118,118,118,118,118,
  118, 59, 59, 59, 59, 59, 59, 59, 59,106,106,106,106,106,106,106,
  106,156,156,156,156,156,156,156,156,147,147,147,147,147,147,147,
  147,148,148,148,148,148,148,148,148,169,169,169,169,169,169,169,
  169,172,172,172,172,172,172,172,172,164,164,164,164,164,164,164,
  164,168,168,168,168,168,168,168,168,174,174,174,174,174,174,174,
  174,  0, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,  0,  0,  0,  0,
    0,  5,  5,  5,  5,  5,  5,  5,  5, 21, 21, 21, 21, 21, 21, 21,
   21, 16, 16, 16, 16, 16, 16, 16, 16, 12, 12, 12, 12, 12, 12, 12,
    2, 74, 74, 74, 74, 74, 74, 74, 74, 42, 42, 42, 42, 42, 42, 42,
   42, 50, 50, 50, 50, 50, 50, 50, 50,161,161,161,161,161,161,161,
  161, 47, 47, 47, 47, 47, 47, 47, 47,120,120,120,120,120,120,120,
  120,116,116,116,116,116,116,116,116, 98, 98, 98, 98, 98, 98, 98,
   98, 57, 57, 57, 57, 57, 57, 57, 57, 88, 88, 88, 88, 88, 88, 88,
   88,117,117,117,117,117,117,117,117,112,112,112,112,112,112,112,
  112,101,101,101,101,101,101,101,101,111,111,111,111,111,111,111,
  111,154,154,154,154,154,154,154,154,143,143,143,143,143,143,143,
  143,115,115,115,115,115,115,115,115,  0,  0, 19,  0,  0,  0,  0,
    0, 95, 95, 95, 95, 95, 95, 95, 95, 11, 11, 11, 11, 11, 11, 11,
   11, 10, 10, 10, 10, 10, 10, 10, 10, 22, 22, 22,  2,  2,  2, 22,
   22,  2, 25, 25, 25, 25, 25, 25, 25, 34, 34, 34, 34, 34, 34, 34,
   34, 52, 52, 52, 52, 52, 52, 52, 52, 54, 54, 54, 54, 54, 54, 54,
   54,  0,  0,  0,  2,  2,  2,  2,  2,  2, 30, 30, 30, 30, 30, 30,
    2,  2,  2, 12, 12, 12, 12, 12, 12, 71, 71, 71, 71, 71, 71, 71,
   71, 41, 41, 41, 41, 41, 41, 41, 41, 53, 53, 53, 53, 53, 53, 53,
   53, 81, 81, 81, 81, 81, 81, 81, 81, 66, 66, 66, 66, 66, 66, 66,
   66, 72, 72, 72, 72, 72, 72, 72, 72,173,173,173,173,173,173,173,
  173, 83, 83, 83, 83, 83, 83, 83, 83, 82, 82, 82, 82, 82, 82, 82,
   82,158,158,158,158,158,158,158,158,153,153,153,153,153,153,153,
  153,145,145,145,145,145,145,145,145,103,103,103,103,103,103,103,
  103,  0,  2,  2,  2,  2,  2,  2,  2,160,160,160,160,160,160,160,
  160,  2,  3,  3,  2,  3,  2,  2,  3,  3,  3,  3,  2,  3,  3,  3,
    3, 19, 19, 19, 19, 19, 19, 19,  0,  3,  3,  3,  3,  3,  0,  3,
    3,  5,  5,  5,  5,  5,  2,  2,  5, 11, 11, 11,  2,  2,  2,  2,
   11, 10, 10, 10, 10, 10, 10,  2, 10, 21, 21, 21, 21, 21,  2,  2,
   21, 22,  2, 22, 22, 22, 22,  2,  2, 23, 23, 23, 23, 23,  2, 23,
   23, 23,  2, 23, 23, 23, 23, 23, 23, 16, 16, 16, 16, 16,  2, 16,
   16, 16,  2, 16, 16, 16, 16, 16, 16, 20, 20, 20, 20, 20,  2, 20,
   20, 20,  2, 20, 20, 20, 20, 20, 20, 36, 36, 36, 36, 36, 36, 36,
    2, 25, 25, 25, 25, 25,  2, 25, 25,  8,  8,  8,  8,  8,  8,  2,
    8,  2,  2,  2,  2,  2,  8,  2,  2, 29, 29, 29, 29, 29, 29,  2,
    2, 45, 45, 45, 45, 45, 45, 45, 45, 44, 44, 44, 44, 44, 44, 44,
   44, 43, 43, 43, 43, 43, 43, 43, 43, 31, 31,  2,  2,  2,  2,  2,
    2, 48, 48, 48, 48,  2,  2,  2,  2, 91, 91,  2,  2,  2,  2,  2,
    2,  1,  2,  2,  2,  2,  2,  2,  2,  9,  9,  9,  9,  9,  2,  9,
    9,  9,  9,  9,  9,  9,  9,  9,  2,  0,  0,  0,  0,  2,  2,  0,
    0,  2,  2,  2,  2,  2,  2,  2, 61,  0, 17, 17, 17, 17, 17, 17,
   17, 12, 12, 12, 12,  2,  2,  2,  2, 13, 13,  2,  2,  2,  2,  2,
    2, 19, 19, 19, 19, 19, 19, 19,  2,  0,  0,  0,  2,  0,  0,  0,
    0, 49, 49, 49, 49, 49, 49,  2,  2,  0,  0,  0,  0,  2,  2,  2,
    0,  1,  0,  0,  0,  0,  0,  0,  0,135,135,135,135,  2,  2,  2,
    2,161,161,161,  2,161,161,161,161,161,161,  2,161,161,161,161,
  161,128,128,128,128,128,128,128,128, 57,  2,  2,  2,  2,  2,  2,
    2,112,112,112,112,112,112,112,  2,122,122,122,122,122,122,122,
  122,130,130,130,  2,  2,  2,  2,  2,165,165,165,165,165,165,  2,
    2,  3,  3,  3,  3,  3,  3,  3,  2,  2,  3,  3,  3,  3,  3,  3,
    3,149,149,149,149,149,149,149,149, 94, 94, 94, 94, 94, 94,  2,
    2,129,129,129,129,129,129,129,129,107,107,107,107,107,  2,  2,
  107,107,107,107,107,107,107,107,107,107,107,107,107,107,  2,  2,
    2,171,171,171,171,171,171,  2,171,123,123,123,123,123,123,  2,
    2,102,102,  2,  2,  2,  2,  2,  2,154,154,154,154,154,154,154,
    2,  2,  2,150,150,150,150,150,150,169,169,  2,  2,  2,  2,  2,
    2,138,138,138,138,138,138,138,  2,103,103,103,103,103,103,  2,
    2,119,119,  2,119,119,119,119,119, 13, 13, 13, 13, 13, 13, 13,
    2, 17,  2,  2,  2,  2,  2,  2,  2,  2,  2,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  2,
    0,  3,  3,  2,  3,  3,  3,  3,  3,  3,  3,  3,  3,  2,  2,  2,
    2, 19,  0,  0,  0,  0,  0,  0,  0, 19, 19, 19, 19, 19,  0,  0,
    0,  0,  0, 26, 26,  0,  0,  0,  0,  9,  9,  9,  9,  0,  9,  9,
    9,  2,  2,  9,  9,  9,  9,  0,  9,  2,  2,  2,  2,  9,  0,  9,
    0,  9,  9,  9,  2,  9,  2,  9,  9,  9,  9,  2,  9,  9,  9,  9,
    9,  9,  9, 55, 55, 55, 55, 55, 55,  6,  6,  6,  6,  6,  1,  1,
    6,  2,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
    2,  2, 14, 14, 14, 14, 14, 14, 14, 14, 14,  2,  2,  2,  2,  2,
    2, 14, 14, 14,  2,  2,  2,  2, 14, 14, 14, 14, 14, 14,  2,  2,
    2,  3,  3,  3,  3,  0,  3,  3,  3,  3,  3,  3,  0,  3,  3,  3,
    0,  0,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  3,  3,  1,  3,  3,  3,  3,  3,  3,
    3, 37, 37, 37, 37, 37, 37,  2, 37, 37, 37, 37,  2,  2, 37, 37,
   37, 38, 38,  2,  2,  2,  2,  2,  2, 64, 64, 64,  2,  2, 64, 64,
   64, 90, 90, 90, 90, 90, 90,  2,  2, 90, 90, 90, 90, 90, 90, 90,
    2, 95, 95, 95, 95,  2,  2, 95,  2, 37, 37, 37,  2,  2,  2,  2,
    2,  3,  3,  2,  2,  2,  2,  2,  3,  3,  3,  0,  3,  3,  3,  3,
    3,  7,  1,  1,  1,  1,  7,  7,  7,  7,  7,  7,  7,  0,  0,  7,
    7,  5,  5,  5,  5,  2,  5,  5,  5,  5,  2,  2,  5,  5,  5,  5,
    5,  5,  2,  5,  5,  5,  5,  5,  5,  5,  2,  5,  2,  2,  2,  5,
    5,  5,  5,  2,  2,  5,  5,  5,  5,  5,  2,  2,  5,  5,  5,  5,
    2,  2,  2,  2,  2,  2,  2,  2,  5,  2,  2,  2,  2,  5,  5,  2,
    5,  5,  5,  5,  5,  2,  2,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    2,  2, 11, 11, 11,  2, 11, 11, 11, 11,  2,  2, 11, 11, 11, 11,
   11, 11,  2, 11, 11, 11, 11, 11, 11, 11,  2, 11, 11,  2, 11, 11,
    2, 11, 11,  2,  2, 11,  2, 11, 11, 11,  2,  2, 11, 11, 11,  2,
    2,  2, 11,  2,  2,  2,  2,  2,  2,  2, 11, 11, 11, 11,  2, 11,
    2,  2,  2,  2,  2,  2,  2, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    2,  2, 10, 10, 10,  2, 10, 10, 10, 10, 10,  2, 10, 10, 10, 10,
   10, 10,  2, 10, 10, 10, 10, 10, 10, 10,  2, 10, 10,  2, 10, 10,
   10, 10, 10,  2,  2, 10, 10, 10, 10, 10, 10,  2, 10, 10, 10,  2,
    2, 10,  2,  2,  2,  2,  2,  2,  2, 10, 10, 10, 10,  2,  2, 10,
   10, 10, 10,  2,  2,  2,  2,  2,  2,  2, 10, 10, 10, 10, 10, 10,
   10,  2, 21, 21, 21,  2, 21, 21, 21, 21,  2,  2, 21, 21, 21, 21,
   21, 21,  2, 21, 21, 21, 21, 21, 21, 21,  2, 21, 21,  2, 21, 21,
   21, 21, 21,  2,  2, 21, 21, 21, 21, 21,  2,  2, 21, 21, 21,  2,
    2,  2,  2,  2, 21, 21, 21, 21, 21,  2,  2,  2,  2, 21, 21,  2,
   21, 21, 21, 21, 21,  2,  2, 21, 21,  2,  2, 22, 22,  2, 22, 22,
   22,  2, 22, 22,  2, 22,  2, 22, 22,  2,  2,  2, 22, 22,  2,  2,
    2, 22, 22,  2,  2,  2,  2, 22, 22, 22,  2,  2,  2,  2,  2,  2,
   22,  2,  2,  2,  2,  2,  2, 22, 22, 22, 22, 22,  2,  2,  2,  2,
    2, 23, 23,  2,  2, 23, 23, 23, 23, 23,  2, 23, 23, 23, 23,  2,
    2,  2,  2,  2,  2,  2, 23, 23,  2, 23, 23, 23,  2, 23, 23,  2,
    2, 23, 23, 23, 23,  2,  2, 23, 23,  2,  2,  2,  2,  2,  2,  2,
   23, 16, 16, 16, 16,  2, 16, 16, 16, 16, 16,  2,  2, 16, 16, 16,
   16, 16,  2, 16, 16, 16, 16,  2,  2,  2,  2,  2,  2,  2, 16, 16,
    2,  2,  2,  2,  2, 16, 16, 16,  2, 16, 16, 16, 16,  2,  2, 16,
   16,  2, 16, 16, 16,  2,  2,  2,  2,  2,  2,  2,  2, 20, 20, 20,
   20, 20, 20, 20, 20,  2,  2, 20, 20,  2, 36, 36, 36,  2, 36, 36,
   36,  2,  2, 36, 36, 36, 36, 36, 36, 36, 36,  2, 36, 36, 36, 36,
   36, 36, 36, 36, 36,  2, 36,  2,  2,  2,  2, 36,  2,  2,  2,  2,
   36, 36, 36, 36, 36, 36,  2, 36,  2,  2,  2,  2,  2,  2,  2, 36,
   36,  2,  2, 36, 36, 36,  2,  2,  2,  2, 24, 24, 24, 24, 24, 24,
   24, 24, 24, 24,  2,  2,  2,  2,  0, 24, 24, 24, 24,  2,  2,  2,
    2,  2, 18, 18,  2, 18,  2, 18, 18, 18, 18, 18,  2, 18, 18, 18,
   18, 18, 18, 18, 18,  2, 18,  2, 18, 18, 18, 18, 18, 18, 18,  2,
    2, 18, 18, 18, 18, 18,  2, 18,  2, 18, 18, 18, 18, 18, 18, 18,
    2, 18, 18,  2,  2, 18, 18, 18, 18, 25, 25, 25, 25, 25,  2,  2,
    2, 25, 25, 25, 25, 25,  0,  0,  0,  0, 25, 25,  2,  2,  2,  2,
    2,  8,  8,  8,  0,  8,  8,  8,  8, 30, 30, 30,  2,  2, 30, 30,
   30, 30, 30, 30, 30, 30,  2,  2,  2, 30, 30,  2,  2,  2,  2,  2,
    2, 34, 34, 34, 34, 34,  2,  2,  2, 35, 35, 35,  0,  0,  0, 35,
   35, 35,  2,  2,  2,  2,  2,  2,  2, 45, 45, 45, 45, 45, 45,  2,
    2,  2,  2,  2,  2,  2,  2,  2, 45, 44, 44, 44, 44, 44,  0,  0,
    2, 43, 43, 43, 43,  2,  2,  2,  2, 46, 46, 46, 46, 46, 46, 46,
   46, 46, 46, 46, 46, 46,  2, 46, 46, 46,  2, 46, 46,  2,  2,  2,
    2, 31, 31, 31, 31, 31, 31,  2,  2, 32, 32,  0,  0, 32,  0, 32,
   32, 32, 32,  2,  2,  2,  2,  2,  2, 32,  2,  2,  2,  2,  2,  2,
    2, 32, 32, 32,  2,  2,  2,  2,  2, 28, 28, 28, 28, 28, 28,  2,
    2, 48, 48, 48, 48, 48, 48, 48,  2, 48,  2,  2,  2, 48, 48, 48,
   48, 52, 52, 52, 52, 52, 52,  2,  2, 52, 52, 52, 52, 52,  2,  2,
    2, 58, 58, 58, 58,  2,  2,  2,  2, 58, 58,  2,  2,  2,  2,  2,
    2, 58, 58, 58,  2,  2,  2, 58, 58, 54, 54, 54, 54,  2,  2, 54,
   54, 91, 91, 91, 91, 91, 91, 91,  2, 91, 91, 91, 91, 91,  2,  2,
   91, 91, 91, 91, 91, 91, 91,  2,  2, 62, 62, 62, 62, 62,  2, 62,
   62, 93, 93, 93, 93,  2,  2,  2,  2,  2,  2,  2,  2, 93, 93, 93,
   93,  2,  2,  2, 70, 70, 70, 70, 70, 70, 70,  2,  2,  2, 70, 70,
   70,  6,  6,  6,  2,  2,  2,  2,  2,  8,  8,  8,  2,  2,  8,  8,
    8,  1,  1,  1,  0,  1,  1,  1,  1,  1,  0,  1,  1,  1,  1,  1,
    1,  1,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  1,  0,  0,
    0,  1,  1,  0,  2,  2,  2,  2,  2, 19, 19, 19, 19, 19, 19,  9,
    9,  9,  9,  9,  6, 19, 19, 19, 19, 19, 19, 19, 19, 19,  9,  9,
    9,  9,  9, 19, 19, 19, 19,  9,  9,  9,  9,  9, 19, 19, 19, 19,
   19,  6, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    9,  2,  9,  2,  9,  2,  9,  2,  9,  9,  9,  9,  9,  2,  2,  9,
    9,  9,  9,  9,  9,  2,  9,  9,  9,  2,  2,  9,  9,  9,  2,  9,
    9,  0,  0,  0,  0,  1,  1,  0,  0,  0, 19,  2,  2,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0, 19,  0,  0,  0,  0,  0,  0,  9,
    0,  0,  0, 19, 19,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 19,
    0, 19,  0,  0,  0,  2,  2,  2,  2, 55, 55, 55, 55,  2,  2,  2,
    2,  2, 55, 55, 55, 55, 55, 55, 55, 61,  2,  2,  2,  2,  2,  2,
    2, 13, 13,  2, 13, 13, 13, 13, 13, 13, 13, 13, 13,  2,  2,  2,
    2,  0,  0,  0,  0,  0, 13,  0, 13,  0, 13, 13, 13, 13, 13, 13,
   13, 13, 13,  1,  1,  1,  1, 12, 12, 13, 13, 13, 13,  0,  0,  0,
    0,  2, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    2,  2,  1,  1,  0,  0, 15, 15, 15, 17, 17, 17,  0,  0, 17, 17,
   17,  2,  2,  2,  2,  2, 26, 26, 26,  2, 12, 12, 12, 12, 12, 12,
   12,  2,  2,  2,  2,  2,  2,  2,  0, 12, 12, 12, 12, 12, 12, 12,
    0, 17, 17, 17, 17, 17, 17, 17,  0, 39, 39, 39, 39, 39,  2,  2,
    2, 39, 39, 39, 39, 39, 39, 39,  2, 77, 77, 77, 77,  2,  2,  2,
    2,  0,  0, 19, 19, 19, 19, 19, 19,  0,  0,  0, 19, 19, 19, 19,
   19, 19, 19, 19, 19, 19, 19,  2,  2,  2,  2, 19,  2,  2,  2,  2,
    2,  2, 19, 19, 19, 19, 19, 19, 19, 60, 60, 60, 60, 60,  2,  2,
    2, 75, 75, 75, 75, 75, 75,  2,  2,  2,  2,  2,  2,  2,  2, 75,
   75, 75, 75,  2,  2,  2,  2,  2,  2, 69, 69, 69, 69, 69, 69,  0,
   69, 74, 74, 74, 74,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
   74, 12, 12, 12, 12, 12,  2,  2,  2, 84, 84, 84, 84, 84, 84,  2,
    0, 84, 84,  2,  2,  2,  2, 84, 84, 33, 33, 33, 33, 33, 33, 33,
    2, 68, 68, 68, 68, 68, 68, 68,  2, 68, 68, 68, 68, 68, 68,  2,
    2, 68, 68,  2,  2, 68, 68, 68, 68, 92, 92, 92,  2,  2,  2,  2,
    2,  2,  2,  2, 92, 92, 92, 92, 92, 87, 87, 87, 87, 87, 87, 87,
    2, 19, 19, 19,  0, 19, 19, 19, 19, 19, 19, 19, 19, 19,  9, 19,
   19, 19, 19,  0,  0, 19, 19,  2,  2, 87, 87, 87, 87, 87, 87,  2,
    2, 87, 87,  2,  2,  2,  2,  2,  2,  2,  2,  2, 12, 12, 12, 12,
   12,  2,  2,  2,  4,  4,  4,  4,  4,  2,  2,  2,  2,  2, 14, 14,
   14, 14, 14, 14, 14, 14, 14, 14,  2, 14, 14, 14, 14, 14,  2, 14,
    2, 14, 14,  2, 14, 14,  2, 14, 14,  3,  3,  3,  3,  3,  3,  0,
    0,  1,  1,  1,  1,  1,  1,  6,  6,  3,  3,  3,  3,  3,  2,  3,
    3,  3,  3,  3,  3,  3,  2,  2,  0,  0,  0,  0,  0,  0,  0, 17,
   17, 17, 17, 17, 17, 17, 17,  0,  0,  2,  2, 12, 12, 12,  2,  2,
    2,  2,  0,  0,  0,  0,  0,  2,  2, 49, 49, 49, 49,  2, 49, 49,
   49, 49, 49, 49, 49, 49, 49, 49,  2, 49, 49, 49,  2, 49, 49,  2,
   49, 49, 49, 49,  2,  2,  2,  2,  2,  0,  0,  0,  2,  2,  2,  2,
    0,  9,  2,  2,  2,  2,  2,  2,  2,  0,  0,  0,  0,  0,  1,  2,
    2, 71, 71, 71, 71, 71,  2,  2,  2, 67,  2,  2,  2,  2,  2,  2,
    2, 42, 42, 42, 42,  2,  2,  2,  2,  2,  2,  2,  2,  2, 42, 42,
   42, 41, 41, 41,  2,  2,  2,  2,  2,118,118,118,  2,  2,  2,  2,
    2, 53, 53, 53, 53, 53, 53,  2, 53, 59, 59, 59, 59,  2,  2,  2,
    2, 59, 59, 59, 59, 59, 59,  2,  2, 50, 50, 50, 50, 50, 50,  2,
    2, 50, 50,  2,  2,  2,  2,  2,  2,104,104,104,104,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,104,161,161,161,  2,161,161,  2,
  161,161,161,  2,161,161,  2,  2,  2,170,170,170,170,  2,  2,  2,
    2,110,110,110,110,110,110,110,  2,110,110,110,110,110,110,  2,
    2, 19, 19, 19, 19, 19, 19,  2, 19, 19,  2, 19, 19, 19, 19, 19,
   19, 47, 47, 47, 47, 47, 47,  2,  2, 47,  2, 47, 47, 47, 47, 47,
   47, 47, 47, 47, 47, 47, 47,  2, 47, 47,  2,  2,  2, 47,  2,  2,
   47, 81, 81, 81, 81, 81, 81,  2, 81,116,116,116,116,116,116,116,
    2,  2,  2,  2,  2,  2,  2,  2,116,128,128,128,  2,128,128,  2,
    2,  2,  2,  2,128,128,128,128,128, 66, 66, 66, 66,  2,  2,  2,
   66, 72, 72,  2,  2,  2,  2,  2, 72,173,173,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2, 97, 97, 97, 97,  2,  2, 97, 97, 97, 97, 97,
   97, 57, 57, 57, 57,  2, 57, 57,  2,  2,  2,  2,  2, 57, 57, 57,
   57, 57, 57, 57, 57,  2, 57, 57, 57,  2, 57, 57, 57, 57, 57, 57,
   57, 57, 57, 57, 57, 57, 57,  2,  2, 57, 57, 57,  2,  2,  2,  2,
   57,  2,  2,  2,112,112,112,112,112, 78, 78, 78, 78, 78, 78,  2,
    2,  2, 78, 78, 78, 78, 78, 78, 78, 83, 83, 83, 83, 83, 83,  2,
    2, 82, 82, 82,  2,  2,  2,  2,  2,122,122,  2,  2,  2,  2,  2,
    2,  2,122,122,122,122,  2,  2,  2,  2,122,122,122,122,122,122,
  122, 89,  2,  2,  2,  2,  2,  2,  2,  2,  2,130,130,130,130,130,
  130,144,144,  2,  2,  2,  2,  2,  2,  2,165,165,165,165,165,165,
  165,  2,  2,  2,  2,  2,  2,165,165,156,156,  2,156,156,156,  2,
    2,156,156,  2,  2,  2,  2,  2,  2,  2,  2,  3,  3,  3,  3,  3,
    3,148,148,  2,  2,  2,  2,  2,  2,158,158,  2,  2,  2,  2,  2,
    2,153,153,153,153,  2,  2,  2,  2,149,149,149,149,149,149,149,
    2,  2,  2, 94, 94, 94, 94, 94, 94,  2,  2,  2,  2,  2,  2,  2,
   94, 85, 85, 85,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 85,  2,
    2,101,  2,  2,  2,  2,  2,  2,  2,101,101,  2,  2,  2,  2,  2,
    2, 96, 96, 96, 96, 96,  2, 96, 96,111,111,111,111,111,111,111,
    2,  2, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,  2,  2,
    2,108,108,  2,108,108,108,108,108,108,108,  2,  2,  2,  2,  2,
    2,129,129,129,129,129,129,129,  2,129,  2,129,129,129,129,  2,
  129,129,129,129,129,129,129,  2,129,129,129,  2,  2,  2,  2,  2,
    2,109,109,109,  2,  2,  2,  2,  2,109,109,  2,  2,  2,  2,  2,
    2,107,107,107,107,  2,107,107,107,107,  2,  2,107,107,107,107,
  107,107,  2,107,107,107,107,107,107,107,  2,107,107,  2,107,107,
  107,107,107,  2,  1,107,107,107,107,107,  2,  2,107,107,107,  2,
    2,107,  2,  2,  2,  2,  2,  2,107,  2,  2,  2,  2,  2,107,107,
  107,107,107,107,107,  2,  2,107,107,171,171,  2,171,  2,  2,171,
    2,171,  2,171,  2,  2,171,  2,171,171,171,171,  2,171,171,171,
  171,171,  2,  2,  2,  2,  2,  2,  2,  2,171,171,  2,  2,  2,  2,
    2,137,137,137,137,  2,137,137,137,137,137,  2,  2,  2,  2,  2,
    2,124,124,  2,  2,  2,  2,  2,  2,114,114,114,114,114,  2,  2,
    2,114,114,  2,  2,  2,  2,  2,  2, 32, 32, 32, 32, 32,  2,  2,
    2, 33, 33, 33, 33,  2,  2,  2,  2,126,126,126,  2,  2,126,126,
  126,126,126,126,126,  2,  2,  2,  2,126,126,126,126,126,126,126,
    2,142,142,142,142,  2,  2,  2,  2,125,125,125,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,125,  2,154,  2,  2,154,154,154,
  154,154,154,154,154,  2,154,154,  2,154,154,154,154,154,154,  2,
  154,154,  2,  2,154,154,154,154,154,154,154,  2,  2,  2,  2,  2,
    2,150,150,150,150,150,  2,  2,  2,140,140,140,  2,  2,  2,  2,
    2,121,  2,  2,  2,  2,  2,  2,  2,  7,  7,  7,  2,  2,  2,  2,
    2,133,  2,133,133,133,133,133,133,133,133,133,133,133,133,133,
    2,133,133,133,133,133,133,  2,  2,133,133,133,133,133,  2,  2,
    2,  2,  2,134,134,134,134,134,134,  2,134,134,134,134,134,134,
  134,134,134,134,134,134,134,134,  2,138,138,  2,138,138,138,138,
  138,  2,  2,138,  2,138,138,  2,138,138,138,  2,  2,  2,  2,  2,
    2,143,143,143,143,143,143,  2,143,143,  2,143,143,143,143,143,
  143,143,143,143,143,143,143,143,  2,143,143,  2,143,143,143,143,
  143,143,  2,  2,  2,  2,  2,  2,  2,143,143,  2,  2,  2,  2,  2,
    2,175,175,175,175,  2,  2,  2,  2,175,175,  2,  2,  2,  2,  2,
    2,  5,  5,  2,  2,  2,  2,  2,  2,145,  2,  2,  2,  2,  2,  2,
    2,163,  2,163,163,163,163,163,163,163,163,163,  2,  2,  2,163,
  163,163,163,163,  2,  2,  2,  2,  2, 86,  2,  2,  2,  2,  2,  2,
    2, 22, 22,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
   22, 63, 63,  2,  2,  2,  2,  2,  2, 63, 63, 63, 63,  2,  2,  2,
    2,177,177,177,177, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
    2,157,157,157,  2,  2,  2,  2,  2, 80, 80, 80, 80, 80, 80,  2,
    2, 80, 80, 80,  2,  2,  2,  2,  2,127,127,127,127,127,127,127,
    2,166,166,  2,  2,  2,  2,  2,  2, 79,  2,  2,  2,  2,  2,  2,
    2,115,115,115,115,115,115,115,  2,115,115,  2,  2,  2,  2,115,
  115,159,159,159,159,159,159,159,  2,159,159,  2,  2,  2,  2,  2,
    2,119,119,119,119,119,119,  2,  2,  2,  2,  2,  2,  2,119,119,
  119,167,167,  2,  2,  2,  2,  2,  2,146,146,146,  2,  2,  2,  2,
    2,172,  2,  2,172,172,172,172,172,172,172,172,172,  2,  2,  2,
    2, 99, 99, 99,  2,  2,  2,  2, 99,  2,  2,  2,  2,  2,  2,  2,
   99,136,139, 13, 13,155,  2,  2,  2,155,155,155,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,155,136,  2,  2,  2,  2,  2,  2,
    2,136,136,136,  2,  2,  2,  2,  2,176,176,  2,  2,  2,  2,  2,
    2,176,176,176,  2,  2,  2,  2,  2, 17, 17, 17, 17,  2, 17, 17,
   17, 17, 17, 17, 17,  2, 17, 17,  2, 17, 15, 15, 15, 15, 15, 15,
   15, 17, 17, 17, 15, 17, 17, 17, 17,  2,  2, 15,  2,  2,  2,  2,
    2, 15, 15, 15,  2,  2, 17,  2,  2,  2,  2,  2,  2, 17, 17, 17,
   17,139,139,139,139,  2,  2,  2,  2,105,105,105,  2,  2,  2,  2,
    2,105,105,105,105,105,  2,  2,  2,105,  2,  2,  2,  2,  2,  2,
    2,105,105,  2,  2,105,105,105,105,  0,  2,  0,  0,  0,  2,  2,
    2,  2,  2,  2,  2,  2,  0,  0,  0,  1,  1,  1,  1,  1,  1,  2,
    2,  1,  1,  1,  1,  1,  1,  1,  2,  1,  1,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  1,  1,
    1,  1,  1,  1,  1,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  0,
    0,  0,  0,  0,  1,  1,  0,  0,  0,  2,  2,  0,  2,  2,  0,  0,
    2,  2,  0,  0,  0,  0,  2,  0,  0,  0,  0,  2,  0,  2,  0,  0,
    0,  0,  0,  0,  0,  2,  0,  0,  0,  0,  0,  0,  2,  2,  0,  0,
    0,  0,  0,  2,  0,  0,  0,  0,  2,  0,  0,  0,  0,  0,  2,  0,
    2,  0,  2,  0,  0,  0,  0,  0,  0,131,131,131,131,  2,  2,  2,
    2,  2,  2,  2,131,131,131,131,131,  2,131,131,131,131,131,131,
  131, 19, 19,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 19, 19,
   19, 19, 19, 19,  9,  9, 19, 19, 19, 56, 56, 56, 56, 56, 56, 56,
    2, 56,  2,  2, 56, 56, 56, 56, 56, 56, 56,  2, 56, 56,  2, 56,
   56, 56, 56, 56,  2,  2,  2,  2,  2,  6,  6,  6,  6,  6,  6,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  6,151,151,151,151,151,  2,  2,
    2,151,151,151,151,151,151,  2,  2,151,151,  2,  2,  2,  2,151,
  151,160,160,160,160,160,160,160,  2,152,152,  2,  2,  2,  2,  2,
  152,164,164,  2,  2,  2,  2,  2,  2,168,168,168,  2,  2,  2,  2,
  168,174,174,174,174,174,174,174,  2,174,174,174,174,174,174,  2,
    2,  2,  2,  2,  2,  2,  2,174,174, 30, 30, 30, 30,  2, 30, 30,
    2,113,113,113,113,113,  2,  2,113,113,113,113,113,113,113,113,
    2,132,132,132,132,  2,  2,  2,  2,132,132,  2,  2,  2,  2,132,
  132,  3,  3,  3,  3,  2,  3,  3,  3,  2,  3,  2,  3,  2,  2,  2,
    2,  2,  2,  3,  2,  2,  2,  2,  3,  2,  3,  2,  3,  2,  3,  3,
    3,  2,  3,  2,  3,  2,  3,  2,  3,  2,  3,  3,  3,  3,  2,  3,
    2,  2,  3,  3,  3,  2,  3,  3,  3,  3,  3,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  0,  0, 15,  0,  0,  2,  2,  2,  2,
    2,  0,  0,  2,  2,  0,  0,  0,  0,  0,  2,  2,  2,  0,  0,  0,
    0, 13,  2,  2,  2,  2,  2,  2,  2, 13, 13, 13,  2,  2,  2,  2,
    2,  2,  0,  2,  2,  2,  2,  2,  2,  1,  2,  3,  4,  5,  6,  7,
    8,  9,  0,  0,  0,  0, 10,  0, 11, 12, 13,  0,  0,  0, 14,  0,
    0, 15,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0, 16, 17,  0,  0,  0,  0, 18,  0,  0,  0,  0,
    0, 19, 20, 21,  0, 22,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0, 23,  0,  0,  0,  0,  0, 24,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0, 25,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0, 26, 27,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12,  0,
    0, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 23,  0,  0, 24, 25,
   26, 27, 28, 29, 30,  0,  0, 31, 32,  0, 33,  0, 34,  0, 35,  0,
    0,  0,  0, 36, 37, 38, 39,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0, 40,  0,  0,  0,  0,  0,  0,
    0,  0,  0, 41, 42,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0, 43, 44,  0, 45,  0,  0,  0,
    0,  0,  0, 46, 47,  0,  0,  0,  0,  0, 48,  0, 49,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 50, 51,  0,  0,  0,
   52,  0,  0, 53,  0,  0,  0,  0,  0,  0,  0, 54,  0,  0,  0,  0,
    0,  0,  0, 55,  0,  0,  0,  0,  0,  0,  0, 56,  0,  0,  0,  0,
    0,  0,  0,  0, 57,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 58, 59, 60, 61, 62,
   63, 64, 65,  0,  0,  0,  0,  0,  0, 66,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0, 67, 68,  0, 69, 70,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0, 71, 72, 73, 74, 75, 76, 77, 78, 79,
   80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
   96, 97, 98, 99,100,101,102,103,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,104,  0,  0,  0,  0,  0,  0,
  105,106,  0,107,  0,  0,  0,108,  0,109,  0,110,  0,111,112,113,
    0,114,  0,  0,  0,115,  0,  0,  0,116,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,117,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,118,119,120,121,  0,
  122,123,124,125,126,  0,127,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,128,129,130,131,132,133,134,135,136,
  137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,
  153,154,155,156,157,  0,  0,  0,158,159,160,161,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,162,  0,163,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  164,165,  0,  0,  0,  0,  0,  0,  0,166,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,167,  0,  0,  0,168,
  169,  0,  0,170,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,171,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,172,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,173,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,174,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,175,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,176,177,  0,  0,
    0,  0,178,179,  0,  0,  0,180,181,182,183,184,185,186,187,188,
  189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,
  205,206,207,208,209,210,211,212,213,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,
};
static const uint16_t _hb_ucd_u16[11240]=
{
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,
    16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
    32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,
    32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
   232, 232, 776, 416, 424, 432, 440, 784, 232, 232, 792, 800,  72, 808,  40, 816,
    64,  64,  64, 824, 832,  64,  64, 840, 848, 856, 864, 872, 880, 888,  64, 896,
    64,  64,  64, 904, 912,  40,  40,  40,  40, 920,  40, 104, 928, 936, 944, 448,
    48,  48,  48,  48,  48,  48,  48, 952, 960, 968, 976,  40, 984, 992,  64,1000,
    72,  72,  72,  40,  40,  40,  64,  64,1008,  64,  64,  64,1016,  64,  64,  64,
    64,  64,  64,1024,  72,1032,  40,  40,1040, 456,  48,1048,1056,   8,1064,1072,
  1080,1088,   8,   8,1096,  48,1104,1112,   8,   8,   8,   8,   8,1120,1128,1136,
  1144,1152,   8,  48,1160,   8,   8,   8,   8,   8, 464, 472, 240,   8,1168,1176,
     8,1184,1192,1200,   8,1208, 152,   8,1216,1224,   8,   8,1232,  48,1240,  48,
   480,   8,   8,1248,1256,1264,1272,1280,1288, 248, 128,1296,1304,1312, 160,1320,
  1328, 248, 128,1336,1344,1352, 304,1360,1368, 488, 128,1376,1384, 192, 160,1392,
  1400, 248, 128,1408,1416,1424, 160,1432,1440,1448,1456,1464,1472,1480, 304,1488,
  1496, 256, 128,1504,1512,1520, 160,1528,1536, 256, 128,1544,1552,1560, 160,1568,
  1576, 256,   8,1584,1592,1600, 160,1608,1616,1624,   8,1632,1640,1648, 304,1656,
   312,   8,   8,1664,1672,1680,   0,   0,1688,   8,1696,1704,1712,1720,   0,   0,
  1728,1736, 320,1744,1752,   8, 168,1760,1768,1776,  48,1784,1792,1800,   0,   0,
     8,   8,1808,1816, 496,1824,1832,1840,1848,1856,  72,  72,1864,  40,  40,1872,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8, 504,1880,   8,   8, 504,   8,   8,1888, 512, 520,   8,   8,
     8, 512,   8,   8,   8,1896,1904, 528,   8, 328,  72,  72,  72,  72,  72,1912,
   536,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,1920,   8,1928,1936,   8,   8,   8,   8,1944,1952,
     8,1960,   8,1968,   8,1976,1984,1992,   8,   8,   8,2000,2008,2016,  88,2024,
  2032,  88,   8,   8,2040,   8,   8, 136,2048,   8,2056,   8,   8,   8,   8, 544,
     8, 112,2064,2072,2080,   8, 120,2088,   8,   8, 200,   8, 208,2096,  24,  24,
     8,2104,   8,   8,   8,2112,2120,2128,  88,  88,2136,2144,  48,  48,  48, 552,
  2152,   8,   8,2160,2168, 496,2176,2184, 336,   8,2192, 240,   8,   8,2200,2208,
     8,   8,2216,2224,2232, 240,   8, 560,2240,  72,  72,2248,2256,2264,2272,2280,
    40,  40,2288, 104, 104, 104,2296,2304,  40,2312, 104, 104,  48,  48,  48,  48,
    64,  64,  64,  64,  64,  64,  64,  64,  64,2320,  64,  64,  64,  64,  64,  64,
   176, 568, 176, 176, 568,2328, 176,2336, 344, 344, 344,2344,2352,2360,2368,2376,
  2384,2392,2400,2408,2416,2424,2432,2440,2448, 104, 576, 576,2456,2464,2472, 552,
  2480,2488,2496,2504,2512,  96,  80,  80,2520,2528,2536,  24,2544,2552,  24,2560,
    56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,
  2568,  24,2576,  24,  24,  24,  24,2584,  24,2592,  56,2600,  24,2608,2616,  24,
    24,  24, 328,   0, 584,   0,  96,  96,  96,2624,  24,  24,  24,  24,2632,  96,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,2640,2648,  24,  24,2656,
    24,  24,  24,  24,  24,  24,2664,  24,  24,  24,  24,  24,  24,  24,  24,  24,
    24,  24,  24,  24,  24,  24,2672,2680,  96,2688,  24,  24,2696,  56,2704,  56,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
    56,  56,  56,  56,  56,  56,  56,  56,2712,2720,  56,  56,  56,2728,  56,2736,
    56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,
    24,  24,  24,  56,2744,  24,  24,2752,  24,  24,  24,  24,  24,  24,  24,  24,
    72,  72,  72,  40,  40,  40,2760,2768,  64,  64,  64,  64,  64,  64,2776,2784,
    40,  40,2792,   8,   8,   8,2800,2808,   8, 216, 224, 224, 224, 224,  48,  48,
  2816,2824,2832,2840,2848,2856,2864,   0,  24,2872,  24,  24,  24,  24,  24, 352,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24, 264,   0,  24,
  2880,2888,2896,2904, 312,   8,   8,   8,   8,2912, 536,   8,   8,   8,   8,2920,
  2928,   8,   8, 312,   8,   8,   8,   8, 112,2936,   8,   8,  24,  24,2944,   8,
    24, 360, 592,  24,2952, 600,  24,  24, 592,  24,  24, 600,  24,  24,  24,  24,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,  24,  24,  24,  24,
     8,2960,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8, 168,  24,  24,  24, 608,   8,   8, 560,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
  2968,   8,2976,   0,  64,  64,2984,2992,  64,3000,   8,   8,   8,   8,3008,3016,
   448,3024,3032, 616,  64,  64,  64,3040,3048,3056,3064,3072,3080,3088,3096,3104,
  3112,   8,3120,3128,   8,   8,   8,3136,3144,   8,   8,3152,3160,  88,  48,3168,
   240,   8,3176,   8,3184,3192,   8, 168, 480,   8,   8,3200,3208, 368,3216,3224,
     8,   8,3232,3240,3248,3256,   8,3264,   8,   8,   8,3272,3280,3288,3296,3304,
  3312,3320, 224,  40,  40,3328,3336,  40,  40,  40,  40,  40,   8,   8,3344,  88,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8, 376,   8,3352,   8,   8, 200,
    16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8, 120,   8,   8,   8,   8,   8,   8, 208,   0,   0,
   624,3360,3368,3376,3384,   8,   8,   8,   8,   8,   8,3392,3400,3408,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,3416,  24,   8,   8,   8,   8,3424,   8,   8,3432,   0,   0,3440,
    48,3448,  48,3456,3464,3472,3480,3488,   8,   8,   8,   8,   8,   8,   8,3496,
  3504, 416, 424, 432, 440,3512,3520,3528,   8,3536,   8, 112,3544,3552,3560,3568,
  3576,   8, 520,3584, 120, 120,   0,   0,   8,   8,   8,   8,   8,   8,   8, 152,
  3592,  96,  96,3600,  80,  80,  80,3608,3616, 272,3624,   0,   0,  24,  24,3632,
     0,   0,   0,   0,   0,   0,   0,   0,   8, 168,   8,   8,   8, 192,3640,3648,
     8,   8,3656,   8,3664,   8,   8,3672,   8,3680,   8,   8,3688,3696,   0,   0,
    72,  72,3704,  40,  40,   8,   8,   8,   8, 120,  88,  72,  72,3712,  40,3720,
     8,   8, 632,   8,   8,   8,3728, 640, 640,3736,3744,3752,   8,   8,   8, 376,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8, 216,   8, 544, 632,   0,3760, 104, 104,3768,   0,   0,   0,   0,
  3776,   8,   8,3784,   8,3792,   8,3800,   8, 112,3808,   0,   0,   0,   8,3816,
     8,3824,   8,3832,   8, 208,   0,   0,   8,   8,   8,3840,  96, 648,  96,  96,
  3848,3856,   8,3864, 656,3872,   8,3880,   8, 664,   0,   0,3888,   8,3896,3904,
     8,   8,   8,3912,   8,3920,   8,3928,   8,3936,3944,   0,   0,   0,   0,   0,
     8,   8,   8,   8, 136,   0,   0,   0,  72,  72,  72,3952,  40,  40,  40,3960,
     8,   8,3968,  88,3976,  72,3984,  40,3992,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,  96,4000,   8,   8,4008, 280,4016,4024, 112,  48,
     8, 664,4032,   8, 464,4040,   0,   8,4048,   0,   0,   8,4056,   0,   8, 216,
  4064,   8,   8,4072,4080, 648,4088,4096, 336,   8,   8,4104,4112,   8, 136,  88,
  4120,   8,4128,4136,4144,   8,   8,4152, 336,   8,   8,4160,4168,4176, 384,4184,
     8, 488, 672,4192,4200,   0,   0,   0,4208,4216,4224,   8,   8,4232,4240,  88,
  4248, 248, 128,4256,4264,4272,4280,4288,4296,   8,   8,4304,4312,4320,4328,   0,
     8,   8,   8,4336,4344,4352, 280,   0,   8,   8,   8,4360,4368,  88,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   8,   8, 680,4376,4384,4392,   0,   0,
     8,   8,   8,4400,4408,  88,4416,   0,   8,   8,4424,4432,  88, 288,4440,   0,
     8,4448,4456,4464, 216,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     8,   8, 672,4472,   0,   0,   0,   0,   0,   0,  72,  72,  40,  40, 320,4480,
  4488,4496,   8,4504,4512,  88,   0,   0,   0,   0,4520,   8,   8,4528,4536,   0,
  4544,   8,   8,4552,4560,4568,   8,   8,4576,4584,4592,   8,   8,   8,   8, 136,
  4600,   0,   0,   0,   0,   0,4608,   0,   0,   0,   0,   0,   8,   8,4616,  88,
   128,   8, 680,4624,4632, 320, 528,4640,   8,4648,4656,4664,   0,   0,   0,   0,
  4672,   8,   8,4680,4688,  88,4696,   8,4704,4712,  88,   8,   8,4720,  88, 472,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   8,4728,
  4736, 256,   8,4744,4752,4760,   0,   0,   0,   0,   0, 192,  96,4768,4776,4784,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8, 208,   0,   0,   0,   0,   0,   0,
    80,  80,  80,  80,  80,  80,  80,4792,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8, 376,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,
    80,  80,  80,  80,  80,  80,  80,  80,4800,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   8,   8,   8,   8,   8,   8,4808,
     8,   8,   8, 144,4816,4824,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8, 152,
     8,   8,   8,   8, 216,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     8, 688,4832,  88,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     8,   8,   8, 136,   8, 112, 368,   8,   8,   8,   8, 112,  88,   8, 120,4840,
     8,   8,   8,4848,4856,4864,4872,4880,   8,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,4888,   8,4896,  88,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,  72,  72,  40,  40,  96,4904,  72,4912,  40,4920,   0,   0,
     8,   8,   8,   8,4928,4936, 696, 696,4944,4952,   0,   0,   0,   0,4960,4968,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8, 152,   0,4976,
     8,   8, 192,   0,   0,   0,   0,   0,   8,   8,   8,   8,   8,   8,   8, 704,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8, 280,   8,   8,   8, 704,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,4984,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8, 136,4992,   0,5000,5008,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8, 200,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     8,   8,   8,   8,   8,   8, 152, 168, 136,5016,5024,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,5032,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,5040,  24,5048,  24,5056,
    48,  48,5064,  48, 712,  24,  24,  24,  24,  24,  24,  24, 352,   0,   0,   0,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24, 264,
    24,  24,5072,  24,  24,  24,5080,5088,5096,  24,5104,  24,  24,  24,  24,  24,
    24,  24,  24,  24,5112,5120,  24,  24,5128,   0,   0,   0,  96, 720,  96, 720,
    24,  24,  24,  24,  24, 608,  96, 656,   0,   0,   0,   0,   0,   0,   0,   0,
    72, 296,  40, 728, 736,5136, 176,  72, 392,5144,5152,5160,5168,  72, 296,  40,
  5176,5184,  40,5192,5200,5208, 744,  72, 752,  40,  72, 296,  40, 728, 736,  40,
   176,  72, 392, 744,  72, 752,  40,  72, 296,  40,5216,  72,5224,5232,5240,5248,
    40,5256,  72,5264,5272,5280,5288,  40,5296,  72,5304,  40,5312, 288, 288, 288,
    48,  48,  48,5320,  48,  48,5328,5336,5344,5352, 456,   0,   0,   0,   0,   0,
    56,5360,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
  5368,  40,  40,  40,5376,5384,5392, 616, 280, 624,   0,   0,5400, 104, 104, 104,
  5408,5416,5424, 104, 104, 104,5432,   0,5440,   0,   0,   0,   0,   0,   0,   0,
     8,   8, 168,5448,5456,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   8,5464,   0,   8,   8,5472,5480,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   8,5488,  88,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   8, 688,5496,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   8, 112,5504,5512,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,5520, 112,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,5528, 712,   0,   0,
    72,  72, 392,  40,5536, 368,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0, 384,  96,  96,5544,5552,   0,   0,   0,   0,
   384,  96,5560,5568,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
  5576,   8,5584,5592,5600,5608,5616,5624,5632, 200,5640, 200,   0,   0,   0,5648,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    24,  24, 184,  24,  24,  24,  24,  24,  24, 352, 360, 400, 400, 400,  24, 264,
  5656,  24,  24,  24,  24,  24,  24,  24,  24,  24, 360,   0,   0,   0,5664,  24,
  5672,  24,  24, 184,5680, 760, 264,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,5688,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,5696, 272, 272,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24, 184, 184,  24,
   184,  24,  24,  24, 408, 328,  24,  24, 408,  24, 768, 184, 760,5704,   0,   0,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
    24,  24,  24,  24,  24, 408, 768, 272,  24,  24,  24,  24,5712,5720,5728, 584,
    24,  24,  24,  24,  24,  24,  24,  24,  24,5736,  24,  24,  24,  24,  24,5744,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   0,   0,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8, 112,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8, 120,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8, 192,   8,
     8,   8,   8,   8,   8, 120,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     8, 120,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8, 152,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8, 208,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     8,   8,   8,   8,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
  5752,   0, 144, 144, 144, 144, 144, 144,   0,   0,   0,   0,   0,   0,   0,   0,
    48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,   0,
    16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,
    16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,5760,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
    24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
    16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,
    32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,
    48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,
    40, 240, 248, 256,  80,  80,  80,  80,  80,  80, 264, 272, 136, 280, 288, 296,
   152, 152, 304, 152, 312, 320, 328, 336, 344, 352,  88, 360, 368, 376, 384, 392,
   400, 408, 416, 424, 432, 440, 448, 456, 464, 472, 480, 488, 496, 504, 512, 520,
   528, 536, 544, 552, 560, 568, 576, 584, 592, 600, 608, 616, 624, 632, 640, 648,
   200, 200, 656, 664,  16,  16,  16,  16, 672, 680, 688, 696, 704, 712, 720, 728,
    96,  96,  96,  96,  96,  96,  96,  96,  96,  96, 736, 744, 752, 760, 768, 776,
   784, 792, 800, 808, 816, 824, 832, 840, 848, 856, 864, 872, 880, 888, 896, 904,
   912, 920, 928, 936, 944, 952, 960, 136,  80,  80,  80,  80, 968, 976, 984, 992,
  1000,1008,1016,1024,1032,1040,1048,  40,  40,  40,  40,  40,  40,  40,  40,  40,
  1056,1064,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,
   160, 160, 160, 160,  40,  40,  40,  40,  40,  40,  40,  40,  40,1072,  40,  40,
  1080,1088,1096,1104,1112,1120,1128,1136,  40,1144,1152,1160,   8,   8,   8,1168,
  1176,1184,1192,1200,1208,  16,1216,1224,1232,1240,  40,1248,1256,1264,  40,  40,
     8,   8,   8,   8,   8,   8,   8,  40,   8,   8,   8,   8,   8,   8,   8,   8,
    64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,
    64,  64,1272,1280, 168, 168, 168, 168,1288, 152,1296,1304,1312,  80,1320,1328,
  1336,1344,1352,1360,1368,1376,1384,1392,1400,1408,1416,1424,1432,1440,1448,1456,
    16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,1464,1472,
     0,   0,   0,   0,   8,   8,   8,   8,   8, 208,   8,1480,1488,1496,  88,  88,
    88,  88,  88,  88,1504,  88,  88,1512,1520,1528,  88,1536,1544,1552,1560,1568,
  1576,1584,1592,1600,1608, 216,1616,1624,   0,   0,1632,1640,1648,1656,1664,1672,
  1680,1688,1696,1704,1712,1720,1728,1736, 176, 176, 176, 176,1744,1752,1760,   0,
  1768,1776,1784,1792,1800,1808,1816,1824,1832,1840,1848,1856,1864,1872,1880,   0,
  1888,1896,1904,1912,1920,1928,1936,   0,   0,1944,1952,1960,1968,1976,1984,1992,
  2000,2008,2016,2024,2032,2040,2048,2056,2064,2072,2080,2088,2096,2104,2112,2120,
  2128,2136,2144,2152,   0,   0,2160,2168,2176,2184,2192,2200,2208,2216,   0,   0,
  2224,   0,2232,2240,2248,2256,2264,2272,2280,2288,2296,2304,2312,2320,   0,2328,
  2336,2344,2352,   0,2360,2368,2376,2384,   0,   0,   0,2392,2400,2408,2416,2424,
    56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,2432,   0,
    56,  56,  56,  56,  56,2440,2448, 224, 224,2456,2464,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,2472,2480,
    48,2488,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,
    48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,2496,
   112, 112, 112, 112, 112, 112, 112, 112, 112,2504,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,2512,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   120, 120, 120, 120, 120, 120, 120, 120,2520,2528,2536,2544,2552,2560,2568,   0,
     0,   0,   0,   0,   0,2576,   0,   0,   0,2584,2592,2600,2608,2616,2624,2632,
    32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32, 128, 128, 128, 128,
   128, 128, 128,2640,2648,   0,  32,2656,  72,  72,  72,  72,  72,  72,  72,  72,
    72,  72,  72,  72,  72,  72,2664,2672,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,2680,
  2688, 184, 184, 184,2696,2704, 144, 144, 144, 144, 144,2712,   0,   0,   0,   0,
  2720,2728,2736,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    40,  40,  40,2744,  40,  40,  40,  40,  40,  40,2752,2760,2768,2776,  40,2784,
    40,  40,  40,2792,2800,2808,2816,  40, 216,2824,2832,2840,  40,2848,   0,   0,
    40,2856,2864,2872,2880,2888,  40,  40,  40,  40,2896,  40,  40,  40,  40,2904,
   104, 104, 104, 104, 104, 104, 104, 104, 104, 104,2912,   0,2920,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  80,  80,2928,2936,
  2944,2952,2960,   0,2968,2976,   0,   0,   0,   0,2984,2992,   0,   0,   0,   0,
     0,   0,   0,3000,   0,   0,   0,3008,   0,   0,   0,3016,   0,   0,   0,3024,
   192, 192, 192,3032,3040,3048,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,3056,3064,   0,3072,   0,   0,   0,3080,3088,3096,3104,   0,   0,   0,   0,
  3112,  40,3120,3128,  40,  40,3136,3144,3152,3160,   0,   0,  40,  40,  40,  40,
    40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,3168,  40,  40,  40,3176,
  3184,3192,3200,3208,  40,  40,  40,  40,  40,3216,  40,3224,  40,  40,3232,3240,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,3248,   8,   8,   8,   8,
  3256,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8, 208,   8,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,3264,
     8,   8,   8,   8,   8,   8,   8,   8,   8, 232,   0,   0,   0,   0,   0,   0,
     8,   8,   8,   8,   8,   8,   8,   8, 232,   0,   0,   0,   0,   0,   0,   0,
     8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,3272,   8,   8,
     8,3280,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    24,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
  3288,  40,   0,   0, 136, 136, 136, 136,   0,   0,   0,   0,   0,   0,   0,   0,
     8,   8,   8,   8,   8,   8,   8,   8,  16,  16,  16,  16,  16,  16,  16,  16,
    24,  24,  24,  24,  24,  24,  24,  24,  40,  40,  40,  40,  40,  40,  40,  40,
    32,  32,  32,  32,  32,  32,  32,  32,  48,  48,  48,  48,  48,  48,  48,  48,
    64,  64,  64,  64,  64,  64,  64,  64,  80,  80,  80,  80,  80,  80,  80,  80,
    88,  88,  88,  88,  88,  88,  88,  88,  56,  56,  56,  56,  56,  56,  56,  56,
    72,  72,  72,  72,  72,  72,  72,  72,  96,  96,  96,  96,  96,  96,  96,  96,
   112, 112, 112, 112, 112, 112, 112, 112, 128, 128, 128, 128, 128, 128, 128, 128,
   104, 104, 104, 104, 104, 104, 104, 104, 144, 144, 144, 144, 144, 144, 144, 144,
   120, 120, 120, 120, 120, 120, 120, 120, 152, 152, 152, 152, 152, 152, 152, 152,
   136, 136, 136, 136, 136, 136, 136, 136, 208, 208, 208, 208, 208, 208, 208, 208,
   192, 192, 192, 192, 192, 192, 192, 192, 184, 184, 184, 184, 184, 184, 184, 184,
   168, 168, 168, 168, 168, 168, 168, 168, 224, 224, 224, 224, 224, 224, 224, 224,
   216, 216, 216, 216, 216, 216, 216, 216,   8,   8,   8,   8,   8,1008,   8,   8,
   160, 160, 160, 160, 160, 160, 160, 160, 256, 256, 256, 256, 256, 256, 256, 256,
     8,   8,   8,1008,   0,   0,   0,   0,1120,  56,  56,1128,1120,  56,  56,1128,
    32,  32,  32,  32,  32,1304,  32,1304,  56,  56,1536,  56,  56,  56,1536,  56,
    56,  56,  56,  56,  56,  56,  56,2096,  32,  32,  32,  32,2104,2112,  32,  32,
   120, 120, 120, 120, 120, 120,2120,2128,2136,2144, 160, 160,2152, 160, 160, 160,
   160, 160, 160, 160,2160, 320, 160, 160,2168, 136, 136, 136, 136, 136, 136, 136,
   136, 136, 136, 136, 136, 136,2176, 424, 424, 424,2184, 424, 424, 424, 424, 424,
   424, 424,2192, 376, 376, 376, 376, 376, 376,2200, 376, 376, 376,2208,2216,   0,
  1544,2224,  72,2232,  72,  72,  72,  72,2240,2248,2256,  72,  72,  72,2264,  72,
    72,  72,  72,1544,  72,  72,  72,  72, 520,2272, 520, 520, 520, 520, 520, 520,
   520,2280,  72,  72,  72,  72,  72,  72, 800, 800, 800, 800, 800, 800,2288,   0,
   712, 712, 712, 712, 712, 712, 712,2296, 808, 808, 808, 808, 808,2304, 808,2312,
  1312,1312,1312,2320, 520,2328,  72,  72,  72,  72,2336,  72,  72,  72,  72,  72,
    72,  72,  72,  72,2344,  72,  72,  72, 264, 264, 264, 264, 264, 264, 264, 264,
   264, 264,2352, 264,2360, 264, 264, 264,2368,1552,2376,1136,1136,2384,2392,2400,
  1552,2408,2416,2424,2432,1136,1136,2440,2448,1560,2456,1320,1320,2464,2472,2480,
  1560,2488,2496,2504,2512,1320,2520,   0,2528,1568,2536,1328,1328,2544,2552,2560,
  1568,2568,2576,   0,2584,1328,2592,2600,2608,1576,2616,1144,1144,2624,2632,2640,
  1576,2648,2656,2664,2672,1144,1144,   0,2680,1336,1584,2688,2696,1336, 528,2704,
  1336,1584,2712,   0,2720, 528, 528,2728, 816,1592,1600, 816, 816,1600, 816,2736,
  1592,2744,2752,2760,2768, 816,2776, 816,1152,1608,1616,1152,1152,1616,2784,2792,
  1608,2800,2808,2816,2824,1152,2832,   0, 432,1624,1632, 432, 432, 432, 432, 432,
  1624,1632,2840, 432,2848, 432, 432, 432,2856, 824,1640,2864, 824, 824,2872,2880,
  1640,2888,2896, 824,2904, 824,2912,   0,2920, 536, 536, 536, 536, 536, 536,2928,
   536, 536, 536,2936,   0,   0,   0,   0,2944,2952, 992, 992,2960, 992, 992,2968,
  2976,2984, 992,2992,   0,   0,   0,   0, 240, 240, 240, 240, 240, 240, 240, 240,
   240,1344, 240, 240, 240,3000,1344, 240, 240, 240, 240,1344, 240, 240, 240,1648,
   240,1648,3008,3016,   0,   0,   0,   0, 216, 216, 216, 216, 280, 280, 280, 280,
  1656,1664, 280, 280, 280, 280, 280,3024, 176, 176, 176, 176, 176, 176, 176, 176,
   176, 832, 304, 832, 176, 176, 176, 176, 176, 832, 176, 176, 176, 176, 832, 304,
   832, 176, 304, 176, 176, 176, 176, 176, 176, 176, 832, 176, 176, 176, 176, 176,
   176, 176, 176,3032, 176, 176, 176,3040, 176, 176, 176,3048, 248, 248, 248, 248,
   248, 248, 248, 248, 248, 248,1672,1672,1352,1352,1352,3056, 440, 440, 440, 440,
   440, 440, 440, 440, 440,3064, 440,3072,1680,1680,3080,3088,1688,1688,3096,   0,
  1696,1696,3104,   0,3112,3120,3128,   0, 296, 296, 296, 296, 296, 296, 296, 296,
   296, 296, 296,3136, 296,1704, 296,1704,3144, 272, 272,3152, 272, 272, 272, 272,
   272, 272, 272, 272, 272, 272, 272,3160, 272, 272, 272, 272, 272,3168,  96,  96,
    96,  96,  96,  96,  96,  96,3176,   0, 840, 840, 840,3184, 840,1712, 840,1712,
  3192, 840,1360,1360,1360,3200,3208,   0, 544, 544, 544, 544, 544,3216, 544, 544,
   544,3224, 544,3232, 296, 296, 296, 296,1368,1368,1368,3240, 352, 352, 352, 352,
   352, 352, 352,3248, 352, 352, 352,3256, 352,1720, 352,1720, 352,3264, 120, 120,
   120, 120, 120, 120, 120, 120,1728,   0, 328, 328, 328, 328, 328, 328, 328, 328,
   328,3272, 328, 328, 328, 328, 328, 328, 552, 552, 552, 552, 552, 552, 552, 552,
   848, 848, 848, 848, 848, 848,3280,3288, 624, 624, 624, 624, 624, 624, 624,3296,
   624,3304, 856, 856, 856, 856, 856, 856, 136,3312, 280, 280, 280, 280, 280,3320,
   552,   0,3328, 120,3336,3344,3352,3360,  56,  56,  56,  56,3368,3376,  56,  56,
    56,  56,  56,3384,3392,3400,  56,3408,  56,  56,  56,  56,  56,  56,  56,3416,
   160, 160, 864, 864, 160, 160, 160, 160, 864, 864, 160,3424, 160, 160, 160, 864,
   160, 160, 160, 160, 160, 160,1736, 160,1736, 160,3432,3440, 160, 160,3448,1744,
    32,3456,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,1000,  32,3464,3472,
    32,  32,  56,  56,  32,  32,  32,  32, 632,   0, 120, 120, 120, 120,1728,   0,
    32,  32,  32,  32,3480,3488,1304,  32,  32,3496,  32,  32,  56,  56,  56,  56,
    56,3504,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32, 720,   0,   0,
    32,1376,   0,   0,  32,  32,  32,  32,  32,  32,  32,  32,  32,  32,1752,  32,
   344, 344, 344, 344, 344, 344, 344, 344, 344, 344, 344, 344,  56,  56,  56,  56,
   320, 320, 320, 320, 320, 320, 320, 320, 320, 320, 320, 320, 320, 320,3512,3520,
   280, 280, 280, 280,1656,1664, 728, 728, 728, 728, 728, 728, 728,1760,3528,1760,
   176, 176, 304,   0, 304, 304, 304, 304, 304, 304, 304, 304, 136, 136, 136, 136,
    32,  32,  32, 560, 336,   0,   0,   0,   8,   8,   8,3536,   8,   8,   8,   8,
     8,   8,   8,   8,   8,   8,3544,   0,   8,   8,1008,   0,   0,   0,  32,  32,
  3552,  32,  32,  32,3560,3568,  32,3576,3584, 168, 168, 168, 168, 168, 168, 168,
   168, 168,3592,3600,1768, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200,3608,
  3616, 568, 568, 568, 568, 568,3624,  16,  16,1160,  32,  32, 568, 568, 568, 568,
    32,  32,  32,  32, 560,3632, 200, 200,  16,  16,  16,1160,  32,  32,  32,  32,
    32,  32,  32,  32,  16,  16,  16,3640,  32,  32, 200, 200, 200, 200, 200,3648,
   200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200,  32,  32,  32,  32,  32,
    80,3656,  80,  80,  80,  80,  80,  80,3664,   0, 872, 872, 872, 872, 872, 872,
   192, 192, 192, 192, 192,3672,   0,   0, 136, 136, 136, 136, 104, 104, 104, 104,
   104, 104, 104, 104, 104, 104, 104,   0,  32,  32,  32,  32,3680,  56,  56,  56,
    56,3688,  56,  56,  56,  56,  56,  56,  56,  56,  56,3696,3704,   0,3712,  56,
  1016,1016,1016,1016,1016,3720,  32, 720, 736, 736, 736, 736, 736, 736, 736,   0,
   576, 576, 576, 576, 576, 576, 576, 576,3728,3736, 576,3744, 264, 264, 264, 264,
  1024,1024,1024,1024,1024,3752,1168,1168,1168,1168,3760,3768,  16,  16,  16,3776,
   448, 448, 448, 448, 448, 448, 448, 448, 448,3784, 448,3792, 216, 216, 216,3800,
   640, 640, 640, 640, 640, 640,3808,   0, 640,3816, 640,3824, 216, 216, 216, 216,
   648, 648, 648, 648, 648, 648, 648, 648,3832,   0,   0,3840, 656, 656,3848,   0,
  1384,1384,1384,   0, 304, 304,  56,  56,  56,  56,  56,3856,3864,3872, 248, 248,
   248, 248, 248, 248, 248, 248, 248, 248, 656, 656, 656, 656, 656,3880, 656,3888,
    16,  16,  16,  16,1776,   0,  16,  16,1160,3896,  16,  16,  16,  16,  16,1776,
     8,   8,   8,1784,   0,   0,   0,   0,1792,   0,3904,3912, 376, 376,3920,3928,
  3936, 376,  72,  72,  72,  72,  72,  72,  72,  72,  72,  72,  72,  72,  72,3944,
    72,  72,   0,   0,   0,   0,  72,  72, 120, 120,  32, 720, 120,3952,  32,  32,
    32,  32,1800,  32, 664, 336,3960,  72,  72,  72,  72,  72,  72,  72,  72,3968,
   880,  32,  32,  32,1120,  56,  56,1128,1120,  56,  56,1128,3976, 200,1768, 200,
   200, 200, 200,3984,  16,  16,  16,1160,1392,1392,1392,3992, 664, 664,   0,4000,
   232,4008, 232, 232,4016, 232, 232,4024, 232,1808, 232,1808,   0,   0,   0,   0,
   232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232,4032,
  4040,  32,  32,  32,  32,  32,1816,  32, 160,1744,  32, 632,4048,   0,   0,   0,
     0,   0,  32,  32,  32,  32,  32,4056,1400,1400,1400,4064, 888, 888, 888, 888,
   888, 888,4072,   0,1824,  32,  32, 336,1176,1176,1176,1176,4080,4088,1408,1408,
  1408,4096,1032,1032,1032,1032,1032,4104,1416,1416,1416,4112,1040,1040,1040,1040,
  4120,1040,4128,   0,   0,   0,   0,   0, 456, 456, 456, 456, 456, 456, 456, 456,
   456, 456, 896, 896, 896, 896, 896, 896,1184,1184,1184,4136,1184,4144, 672, 672,
   672, 672,1832, 672, 672, 672, 672,1832,1048,1048,1048,1048,1048,   0, 904, 904,
   904, 904, 904, 904,4152,4160,1192,1840,1192,1840,4168,1192,1848,1192,1848,4176,
   912, 912, 912, 912, 912, 912,4184,   0, 184, 184, 184, 184, 184, 184,4192,   0,
   184, 184,4200,   0, 184,   0,   0,   0,4208,  56,  56,  56,  56,  56,4216,  56,
  4224,4232,1200,1200,1200,1200,4240,4248,1424,1424,4256,1424,1208,1208,1208,1208,
  1216,1216,1216,4264,4272,1216,   0,   0,   0,   0,   0,   0,1856,1856,4280,4288,
  1432,1432,1432,4296,1440,1440,1440,4304,1448,1448,1448,4312,   0,   0,   0,   0,
  1224,1224,1224,1224, 464, 464, 464,4320, 464, 464,4328, 464, 464, 464, 464, 464,
  4336,4344,4352,4360,1232,1232,4368,4376,1232,1864,1232,1864,1240,1240,1240,1240,
  1248,1248,1248,1248,   0,   0,   0,   0,1256,1256,1256,1256,1872,4384,1872,   0,
   920, 920, 920, 920, 920, 920,4392,4400,1456,1456,4408,1456,1464,1464,4416,1464,
  1880,1880,4424,4432,   0,4440,   0,   0, 584, 584, 584, 584, 584, 584, 584, 584,
   584,4448,   0,   0,   0,   0,   0,   0, 384, 384, 384, 384, 384, 384,1888,   0,
   384, 384, 384, 384, 384, 384,1888,4456, 928, 928, 928, 928, 928,   0, 928,4464,
   936, 936, 936, 936,1896,4472, 936, 936,1896,4480,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,  72,  72,  72,1904,1056,1056,1056,1056,1056,4488,4496,   0,
  4504,1912,  72,  72,  72,1904,  72,  72,1064,1064,1064,1064,1064,   0,1072,1072,
  1072,1072,1072,4512,   0,   0,1472,1472,1472,4520,   0,   0,   0,   0,1480,1480,
  1480,4528,   0,   0,1920,1920,4536,   0, 392, 392, 392, 392, 392, 392, 392, 392,
   392,1928,4544, 392, 392, 392,1928,4552, 680, 680, 680, 680, 680, 680, 680, 680,
  4560,4568,1264,1264,1264,4576,1264,4584, 688, 688, 688, 688, 688, 688,4592, 688,
   688,   0,1272,1272,1272,1272,4600,   0, 360, 360, 360, 360, 360, 360, 360, 360,
   360, 360, 360, 360,4608, 824,4616,   0, 744, 744,4624, 744, 744, 744, 744, 744,
  4632,   0,   0,   0,   0,   0,   0,   0,4640,4648,1936,4656,1936,4664, 696, 696,
   696, 696, 696, 696, 696,4672, 696,4680,4688,1944,4696,1952,1952,4704,4712,4720,
  1944,4728,4736,4744,4752,1960,1960,   0, 944,4760, 944, 944, 944, 944,1968, 944,
  4768,4776,1968,4784,4792,   0,   0,   0, 408, 408, 408, 408, 408, 408, 408, 408,
   408, 408, 408,4800,4808,   0,   0,   0, 472, 472, 472, 472, 472, 472, 472, 472,
   472,   0, 472,4816,   0,   0,   0,   0, 480, 480, 480, 480, 480, 480,1976, 480,
   480, 480, 480,1976,   0,   0,   0,   0, 592, 592, 592, 592, 592, 592, 592, 592,
  4824,   0, 592,4832, 272,4840,   0,   0, 704, 704, 704, 704, 704, 704, 704,1984,
   704,1984, 216, 216,4848,   0,   0,   0, 952, 952, 952,4856, 952,4864, 952, 952,
  4872,   0,   0,   0,   0,   0,   0,   0, 752, 752, 752, 752, 752, 752, 752,4880,
     0,   0,   0,   0, 488, 488, 488, 488, 488, 488, 488, 488, 488, 488,4888,4896,
  1992,4904,4912,1280,1280,1280,4920,4928,1992,   0,1280,4936,   0,   0,   0,   0,
     0,   0,   0,   0, 960,2000, 960, 960, 960, 960, 960,2000,4944,   0,   0,   0,
   600, 600, 600, 600, 600, 600, 600, 600, 600,   0, 496, 496, 496, 496, 496, 496,
   496, 496, 496, 496,4952,   0,  96,  96, 760, 760, 760, 760, 760, 760, 760,4960,
   264,4968,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 360,   0,   0,   0,
  1080,1080,1080,1080,2008,   0,1080,2008, 608,4976, 608, 608, 608, 608,4984, 608,
  4992,   0, 608, 608, 608,5000, 968, 968, 968, 968,5008, 968, 968,5016,5024,   0,
  2016,5032, 976, 976, 976, 976,2016,5040, 976,   0, 976,5048,5056,5064,1288,1288,
  1288,5072,5080,5088,1288,5096, 984, 984, 984, 984, 984,5104, 984,5112,5120,   0,
     0,   0,   0,   0,1488,1488,1488,5128, 616, 616,5136, 616, 616, 616, 616,5144,
   616, 616, 616,5152,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,5160,   0,
   528, 528, 528, 528, 528, 528,5168,5176,  64,  64,  64,5184,   0,   0,   0,   0,
  5192,   0,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64, 256, 256, 256,
   256,5200,  64,  64,  64,  64,  64,  64,5208,   0,   0,   0,   0,   0,   0,   0,
     0,   0, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400,5216,   0,
    48,  48,5224,   0,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,5232,
  5240,   0,   0,   0,   0,   0,   0,   0, 768, 768, 768, 768, 768, 768, 768,5248,
   104, 104, 104, 104, 104, 104, 104,5256,1296,1296,1296,5264,1296,5272, 504, 504,
   504, 504, 504, 504, 504, 504, 504,5280, 504,5288,1496,1496,1496,2024,2024,   0,
   368, 368, 368, 368, 368, 368, 368, 368,5296,   0, 368,2032,2032, 368, 368,5304,
   368, 368,   0,   0,   0,   0,   0,   0, 776, 776, 776, 776, 776, 776, 776,5312,
   416, 416, 416, 416, 416, 416, 416, 416, 416, 416, 416,5320,1088,1088,1088,5328,
  1088,1088,5336,   0,   0,   0,   0,   0, 288, 288, 288, 288, 288, 288, 288, 288,
   288,5344, 288, 288, 288, 288, 288, 288, 288,5352, 288, 288,   0,   0,   0,   0,
     0,   0,   0,   0,5360,   0,2040,   0, 144, 144, 144,5368,   0,   0,   0,5376,
    40,  40,  40,  40,5384,   0,   0,   0,  40,  40,  40,  40,  40,  40,5392,   0,
    88,  88,5400,   0,  88,  88,  88,  88,  88,  88,5408,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,5416,5424,5432, 168, 168, 168, 168, 168, 168, 168,
   168, 168, 168, 168,5440,2048,5448,   0,   0,   0,5456,   0,5464,2048, 152, 152,
   152, 152, 152, 152, 152, 152, 152,5472, 312, 312, 312, 312, 312, 312, 312, 312,
   312, 312, 312, 312, 312,5480, 312,5488, 312,5496, 312,5504, 336,   0,   0,   0,
    32,  32,  32,  32,  32,  32,  32, 632,  32,  32,  32,  32,  32,  32, 336,2056,
    32,  32,5512,5520,  32,  32,  32, 560, 120, 120, 120, 120, 120,5528, 120, 120,
  5536,   0,  32,  32,  32,  32,  32,  32, 336,   0,   0,   0,   0,   0,   0,   0,
    32,  32,  32,  32,  32,  32, 560,   0,  32,  32,  32,  32,2064,1824,  32,  32,
    32,  32,  32,  32,2064,5544,  32,5552,5560,5568,  32,  32,  32,5576,  32,  32,
   864,   0,  32,5584,  32,  32,  32,  32, 720,   0,   0,   0,   0,   0,   0,   0,
    32,  32, 336,   0,  32,  32, 336,   0,  32,  32, 664,   0,  32,  32,  32,1504,
    32,  32,1000,  32,  32,  32,  32,  32,  32,  32,  32,1000,5592,5600,  32,5608,
  5616,  32,  32,  32,  32,  32,  32,  32,2072,5624,1000,1000,  32,  32,  32,5632,
  5640,2056,5648,  32,  32,  32,  32,  32,  32,  32,  32,  32, 664,  32,  32,  32,
    32,1752,  32,  32,  32,  32,  32,  32, 112,5656,   0,5664,5672, 112,   0,   0,
    32,  32,  32, 632,   0,   0,   0,   0,5680,   0,1792,   0,   0,   0,   0,   0,
     0,5688,  56,  56,  56,  56,5696,  56,5704, 344, 344,5712,5720,5728, 136, 136,
   136, 136, 136, 136, 136,5736,   0,   0,   0,5744,   0,   0,   0,   0,   0,   0,
   784, 784, 784, 784, 784,5752, 784,5760, 784,5768,   0,   0,   0,   0,   0,   0,
     0,   0,1512,1512,1512,5776,   0,   0, 792, 792, 792, 792, 792, 792, 792,5784,
     0,   0,1096,1096,1096,1096,1096,5792,   0,   0,1104,1104,1104,1104,1104,5800,
  1112,1112,1112,5808,1112,1112,5816,5824,   0,   0,   0,   0, 304,5832, 176, 304,
  5840, 224,5848,   0,   0,   0,   0,   0, 512, 512, 512, 512, 512, 512, 512, 512,
   512,5856, 512,5864,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 880,  32,
    32,  32,  32,  32,  32,  32, 632,   0, 880,  32,  32,  32,  32,  32,  32, 560,
  5872,  72,  72,  72,1520,1912,1528,5880,5888,5896,1520,5904,1520,1528,1528,5912,
    72,2080,  72,2088,5920,2080,  72,2088,   0,   0,   0,   0,   0,   0,5928,   0,
    32,  32,  32,  32,  32, 336,  32,  32,  32,  32, 336,   0,  32, 664, 880,  32,
   880,  32, 880,  32,  32,  32, 560,   0,  32,  32,  32,  32,  32, 664,   0,   0,
     0,   0,   0,   0,5936,  32,  32,  32,5944,   0,  32,  32,  32,  32,  32, 336,
    32,1504, 720,   0, 560,   0,   0,   0,  32,  32,  32,5952,  32, 632,  32, 632,
    32,  32,  32, 336,  32, 336,  32,  32,  32, 336,  32,  32,  32,  32,  32,  32,
    32,   0,  32, 720,  32,  32,  32,  32,  32,   0,  32,  32,  32, 560,  32, 336,
   720,   0,  32,1504,   0,   0,   0,   0,  32,  32,  32,   0,  32, 560,  32, 632,
   664,5960,  32,2072,  32,1816,  32,1376,  32,  32,1800,  32,  32,  32,  32,  32,
    32,  32,  32,  32,  32,  32,  32,1376,   8,   8,   8,   8,   0,   0,   0,   0,
     8,   8,   8,2040,   8,   8,   8,   8,   8,   8,   8,   8,5968,   0,   8,   8,
     8,5976,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,1784,
  5984,   0,   0,   0,  32,  32,  32,  32,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0, 939, 940, 941, 942, 946, 948,   0, 962,
   969, 970, 971, 976,1001,1002,1003,1008,   0,1033,1040,1041,1042,1043,1047,   0,
     0,1080,1081,1082,1086,1110,   0,   0,1124,1125,1126,1127,1131,1133,   0,1147,
  1154,1155,1156,1161,1187,1188,1189,1193,   0,1219,1226,1227,1228,1229,1233,   0,
     0,1267,1268,1269,1273,1298,   0,1303, 943,1128, 944,1129, 954,1139, 958,1143,
   959,1144, 960,1145, 961,1146, 964,1149,   0,   0, 973,1158, 974,1159, 975,1160,
   983,1168, 978,1163, 988,1173, 990,1175, 991,1176, 993,1178, 994,1179,   0,   0,
  1004,1190,1005,1191,1006,1192,1014,1199,1007,   0,   0,   0,1016,1201,1020,1206,
     0,1022,1208,1025,1211,1023,1209,   0,   0,   0,   0,1032,1218,1037,1223,1035,
  1221,   0,   0,   0,1044,1230,1045,1231,1049,1235,   0,   0,1058,1244,1064,1250,
  1060,1246,1066,1252,1067,1253,1072,1258,1069,1255,1077,1264,1074,1261,   0,   0,
  1083,1270,1084,1271,1085,1272,1088,1275,1089,1276,1096,1283,1103,1290,1111,1299,
  1115,1118,1307,1120,1309,1121,1310,   0,1053,1239,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,1093,1280,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0, 949,1134,1010,1195,1050,1236,1090,1277,1341,1368,1340,
  1367,1342,1369,1339,1366,   0,1320,1347,1418,1419,1323,1350,   0,   0, 992,1177,
  1018,1204,1055,1241,1416,1417,1415,1424,1202,   0,   0,   0, 987,1172,   0,   0,
  1031,1217,1321,1348,1322,1349,1338,1365, 950,1135, 951,1136, 979,1164, 980,1165,
  1011,1196,1012,1197,1051,1237,1052,1238,1061,1247,1062,1248,1091,1278,1092,1279,
  1071,1257,1076,1263,   0,   0, 997,1182,   0,   0,   0,   0,   0,   0, 945,1130,
   982,1167,1337,1364,1335,1362,1046,1232,1422,1423,1113,1301,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   8,   9,   0,  10,1425,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   7,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   1,   0,   0,   0,   0,   0,   0,1314,1427,   5,
  1434,1438,1443,   0,1450,   0,1455,1461,1514,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,1446,1458,1468,1476,1480,1486,1517,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,1489,1503,1494,1500,1508,   0,   0,   0,   0,1520,1521,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,1526,1528,   0,1525,   0,   0,   0,1522,
     0,   0,   0,   0,1536,1532,1539,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,1534,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,1556,   0,   0,   0,   0,   0,   0,1548,1550,   0,1547,   0,   0,   0,1567,
     0,   0,   0,   0,1558,1554,1561,   0,   0,   0,   0,   0,   0,   0,1568,1569,
     0,   0,   0,   0,   0,   0,   0,   0,   0,1529,1551,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,1523,1545,1524,1546,   0,   0,1527,1549,
     0,   0,1570,1571,1530,1552,1531,1553,   0,   0,1533,1555,1535,1557,1537,1559,
     0,   0,1572,1573,1544,1566,1538,1560,1540,1562,1541,1563,1542,1564,   0,   0,
  1543,1565,   0,   0,   0,   0,   0,   0,   0,   0,1606,1607,1609,1608,1610,   0,
     0,   0,   0,   0,   0,   0,   0,   0,1613,   0,1611,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,1612,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,1620,   0,   0,   0,   0,   0,   0,   0,1623,   0,   0,1624,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
  1614,1615,1616,1617,1618,1619,1621,1622,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,1628,1629,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,1625,1626,   0,1627,   0,   0,   0,1634,   0,   0,1635,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,1630,1631,1632,   0,   0,1633,   0,   0,   0,   0,   0,   0,   0,   0,   0,
  1639,   0,   0,1638,1640,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,1636,1637,   0,   0,   0,   0,   0,   0,1641,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,1642,1644,1643,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
  1645,   0,   0,   0,   0,   0,   0,   0,1646,   0,   0,   0,   0,   0,   0,1648,
  1649,   0,1647,1650,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,1651,1653,1652,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,1654,   0,1655,1657,1656,   0,   0,   0,   0,1659,   0,   0,   0,   0,
     0,   0,   0,   0,   0,1660,   0,   0,   0,   0,1661,   0,   0,   0,   0,1662,
     0,   0,   0,   0,1663,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,1658,   0,   0,   0,   0,   0,   0,   0,   0,   0,1664,   0,1665,1673,   0,
  1674,   0,   0,   0,   0,   0,   0,   0,   0,1666,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,1668,   0,   0,   0,   0,
     0,   0,   0,   0,   0,1669,   0,   0,   0,   0,1670,   0,   0,   0,   0,1671,
     0,   0,   0,   0,1672,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,1667,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,1675,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,1676,   0,
  1677,   0,1678,   0,1679,   0,1680,   0,   0,   0,1681,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,1682,   0,1683,   0,   0,1684,1685,   0,1686,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0, 953,1138, 955,1140, 956,1141, 957,1142,
  1324,1351, 963,1148, 965,1150, 968,1153, 966,1151, 967,1152,1378,1380,1379,1381,
   984,1169, 985,1170,1420,1421, 986,1171, 989,1174, 995,1180, 998,1183, 996,1181,
   999,1184,1000,1185,1015,1200,1329,1356,1017,1203,1019,1205,1021,1207,1024,1210,
  1687,1688,1027,1213,1026,1212,1028,1214,1029,1215,1030,1216,1034,1220,1036,1222,
  1039,1225,1038,1224,1334,1361,1336,1363,1382,1384,1383,1385,1056,1242,1057,1243,
  1059,1245,1063,1249,1689,1690,1065,1251,1068,1254,1070,1256,1386,1387,1388,1389,
  1691,1692,1073,1259,1075,1262,1079,1266,1078,1265,1095,1282,1098,1285,1097,1284,
  1390,1391,1392,1393,1099,1286,1100,1287,1101,1288,1102,1289,1105,1292,1104,1291,
  1106,1294,1107,1295,1108,1296,1114,1302,1119,1308,1122,1311,1123,1312,1186,1260,
  1293,1305,   0,1394,   0,   0,   0,   0, 952,1137, 947,1132,1317,1344,1316,1343,
  1319,1346,1318,1345,1693,1695,1371,1375,1370,1374,1373,1377,1372,1376,1694,1696,
   981,1166, 977,1162, 972,1157,1326,1353,1325,1352,1328,1355,1327,1354,1697,1698,
  1009,1194,1013,1198,1054,1240,1048,1234,1331,1358,1330,1357,1333,1360,1332,1359,
  1699,1700,1396,1401,1395,1400,1398,1403,1397,1402,1399,1404,1094,1281,1087,1274,
  1406,1411,1405,1410,1408,1413,1407,1412,1409,1414,1109,1297,1117,1306,1116,1304,
  1112,1300,   0,   0,   0,   0,   0,   0,1471,1472,1701,1705,1702,1706,1703,1707,
  1430,1431,1715,1719,1716,1720,1717,1721,1477,1478,1729,1731,1730,1732,   0,   0,
  1435,1436,1733,1735,1734,1736,   0,   0,1481,1482,1737,1741,1738,1742,1739,1743,
  1439,1440,1751,1755,1752,1756,1753,1757,1490,1491,1765,1768,1766,1769,1767,1770,
  1447,1448,1771,1774,1772,1775,1773,1776,1495,1496,1777,1779,1778,1780,   0,   0,
  1451,1452,1781,1783,1782,1784,   0,   0,1504,1505,1785,1788,1786,1789,1787,1790,
     0,1459,   0,1791,   0,1792,   0,1793,1509,1510,1794,1798,1795,1799,1796,1800,
  1462,1463,1808,1812,1809,1813,1810,1814,1467,  21,1475,  22,1479,  23,1485,  24,
  1493,  27,1499,  28,1507,  29,   0,   0,1704,1708,1709,1710,1711,1712,1713,1714,
  1718,1722,1723,1724,1725,1726,1727,1728,1740,1744,1745,1746,1747,1748,1749,1750,
  1754,1758,1759,1760,1761,1762,1763,1764,1797,1801,1802,1803,1804,1805,1806,1807,
  1811,1815,1816,1817,1818,1819,1820,1821,1470,1469,1822,1474,1465,   0,1473,1825,
  1429,1428,1426,  12,1432,   0,  26,   0,   0,1315,1823,1484,1466,   0,1483,1829,
  1433,  13,1437,  14,1441,1826,1827,1828,1488,1487,1513,  19,   0,   0,1492,1515,
  1445,1444,1442,  15,   0,1831,1832,1833,1502,1501,1516,  25,1497,1498,1506,1518,
  1457,1456,1454,  17,1453,1313,  11,   3,   0,   0,1824,1512,1519,   0,1511,1830,
  1449,  16,1460,  18,1464,   4,   0,   0,  30,  31,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  20,   0,
     0,   0,   2,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,1834,1835,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,1836,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,1837,1839,1838,   0,   0,   0,   0,1840,   0,   0,   0,
     0,1841,   0,   0,1842,   0,   0,   0,   0,   0,   0,   0,1843,   0,1844,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,1845,   0,   0,1846,   0,   0,1847,
     0,1848,   0,   0,   0,   0,   0,   0, 937,   0,1850,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,1849, 936, 938,1851,1852,   0,   0,1853,1854,   0,   0,
  1855,1856,   0,   0,   0,   0,   0,   0,1857,1858,   0,   0,1861,1862,   0,   0,
  1863,1864,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,1867,1868,1869,1870,1859,1860,1865,1866,   0,   0,   0,   0,
     0,   0,1871,1872,1873,1874,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,  32,  33,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,1875,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,1877,   0,1878,   0,1879,   0,1880,   0,1881,   0,1882,   0,
  1883,   0,1884,   0,1885,   0,1886,   0,1887,   0,1888,   0,   0,1889,   0,1890,
     0,1891,   0,   0,   0,   0,   0,   0,1892,1893,   0,1894,1895,   0,1896,1897,
     0,1898,1899,   0,1900,1901,   0,   0,   0,   0,   0,   0,1876,   0,   0,   0,
     0,   0,   0,   0,   0,   0,1902,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,1904,   0,1905,   0,1906,   0,1907,   0,1908,   0,1909,   0,
  1910,   0,1911,   0,1912,   0,1913,   0,1914,   0,1915,   0,   0,1916,   0,1917,
     0,1918,   0,   0,   0,   0,   0,   0,1919,1920,   0,1921,1922,   0,1923,1924,
     0,1925,1926,   0,1927,1928,   0,   0,   0,   0,   0,   0,1903,   0,   0,1929,
  1930,1931,1932,   0,   0,   0,1933,   0, 710, 385, 724, 715, 455, 103, 186, 825,
   825, 242, 751, 205, 241, 336, 524, 601, 663, 676, 688, 738, 411, 434, 474, 500,
   649, 746, 799, 108, 180, 416, 482, 662, 810, 275, 462, 658, 692, 344, 618, 679,
   293, 388, 440, 492, 740, 116, 146, 168, 368, 414, 481, 527, 606, 660, 665, 722,
   781, 803, 809, 538, 553, 588, 642, 758, 811, 701, 233, 299, 573, 612, 487, 540,
   714, 779, 232, 267, 412, 445, 457, 585, 594, 766, 167, 613, 149, 148, 560, 589,
   648, 768, 708, 345, 411, 704, 105, 259, 313, 496, 518, 174, 542, 120, 307, 101,
   430, 372, 584, 183, 228, 529, 650, 697, 424, 732, 428, 349, 632, 355, 517, 110,
   135, 147, 403, 580, 624, 700, 750, 170, 193, 245, 297, 374, 463, 543, 763, 801,
   812, 815, 162, 384, 420, 730, 287, 330, 337, 366, 459, 476, 509, 558, 591, 610,
   726, 652, 734, 759, 154, 163, 198, 473, 683, 697, 292, 311, 353, 423, 572, 494,
   113, 217, 259, 280, 314, 499, 506, 603, 608, 752, 778, 782, 788, 117, 557, 748,
   774, 320, 109, 126, 260, 265, 373, 411, 479, 523, 655, 737, 823, 380, 765, 161,
   395, 398, 438, 451, 502, 516, 537, 583, 791, 136, 340, 769, 122, 273, 446, 727,
   305, 322, 400, 496, 771, 155, 190, 269, 377, 391, 406, 432, 501, 519, 599, 684,
   687, 749, 776, 175, 452, 191, 480, 510, 659, 772, 805, 813, 397, 444, 619, 566,
   568, 575, 491, 471, 707, 111, 636, 156, 153, 288, 346, 578, 256, 435, 383, 729,
   680, 767, 694, 295, 128, 210,   0,   0, 227,   0, 379,   0,   0, 150, 493, 525,
   544, 551, 552, 556, 783, 576, 604,   0, 661,   0, 703,   0,   0, 735, 743,   0,
     0,   0, 793, 794, 795, 808, 741, 773, 118, 127, 130, 166, 169, 177, 207, 213,
   215, 226, 229, 268, 270, 317, 327, 329, 335, 369, 375, 381, 404, 441, 448, 458,
   477, 484, 503, 539, 545, 547, 546, 548, 549, 550, 554, 555, 561, 564, 569, 591,
   593, 595, 598, 607, 620, 625, 625, 651, 690, 695, 705, 706, 716, 717, 733, 735,
   777, 786, 790, 315, 869, 623,   0,   0, 102, 145, 134, 115, 129, 138, 165, 171,
   207, 202, 206, 212, 227, 231, 240, 243, 250, 254, 294, 296, 303, 308, 319, 325,
   321, 329, 326, 335, 341, 357, 360, 362, 370, 379, 388, 389, 393, 421, 424, 438,
   456, 454, 458, 465, 477, 535, 485, 490, 493, 507, 512, 514, 521, 522, 525, 526,
   528, 533, 532, 541, 565, 569, 574, 586, 591, 597, 607, 637, 647, 674, 691, 693,
   695, 698, 703, 699, 705, 704, 702, 706, 709, 717, 728, 736, 747, 754, 770, 777,
   783, 784, 786, 787, 790, 802, 825, 848, 847, 857,  55,  65,  66, 883, 892, 916,
   822, 824,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,1586,   0,1605,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,1602,1603,1934,1935,1574,1575,1576,1577,1579,1580,1581,1583,1584,   0,
  1585,1587,1588,1589,1591,   0,1592,   0,1593,1594,   0,1595,1596,   0,1598,1599,
  1600,1601,1604,1582,1578,1590,1597,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,1936,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,1937,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,1938,   0,1939,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,1940,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,1941,1942,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,1943,1944,   0,   0,   0,   0,   0,   0,1945,   0,1946,   0,   0,
     0,   0,   0,   0,   0,   0,1947,   0,   0,1948,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,1950,   0,1949,
  1951,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,1953,1952,   0,1954,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,1955,1956,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
  1957,   0,   0,   0,   0,   0,   0,   0,   0,1958,1961,1959,1965,1960,1962,1964,
  1963,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
  1967,1966,1968,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,1969,1970,1971,1972,1973,1974,1975,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,1976,1977,1978,1980,1979,1981,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0, 106, 104, 107, 826, 114, 118, 119, 121,
   123, 124, 127, 125,  34, 830, 130, 131, 132, 137, 827,  35, 133, 139, 829, 142,
   143, 112, 144, 145, 924, 151, 152,  37, 157, 158, 159, 160,  38, 165, 166, 169,
   171, 172, 173, 174, 176, 177, 178, 179, 181, 182, 182, 182, 833, 468, 184, 185,
   834, 187, 188, 189, 196, 192, 194, 195, 197, 199, 200, 201, 203, 204, 204, 206,
   208, 209, 211, 218, 213, 219, 214, 216, 153, 234, 221, 222, 223, 220, 225, 224,
   230, 835, 235, 236, 237, 238, 239, 244, 836, 837, 247, 248, 249, 246, 251,  39,
    40, 253, 255, 255, 838, 257, 258, 259, 261, 839, 262, 263, 301, 264,  41, 266,
   270, 272, 271, 841, 274, 842, 277, 276, 278, 281, 282,  42, 283, 284, 285, 286,
    43, 843,  44, 289, 290, 291, 293, 934, 298, 845, 845, 621, 300, 300,  45, 852,
   894, 302, 304,  46, 306, 309, 310, 312, 316,  48,  47, 317, 846, 318, 323, 324,
   325, 324, 328, 329, 333, 331, 332, 334, 335, 336, 338, 339, 342, 343, 347, 351,
   849, 350, 348, 352, 354, 359, 850, 361, 358, 356,  49, 363, 365, 367, 364,  50,
   369, 371, 851, 376, 386, 378,  53, 381,  52,  51, 140, 141, 387, 382, 614,  78,
   388, 389, 390, 394, 392, 856,  54, 399, 396, 402, 404, 858, 405, 401, 407,  55,
   408, 409, 410, 413, 859, 415,  56, 417, 860, 418,  57, 419, 422, 424, 425, 861,
   840, 862, 426, 863, 429, 431, 427, 433, 437, 441, 438, 439, 442, 443, 864, 436,
   449, 450,  58, 454, 453, 865, 447, 460, 866, 867, 461, 466, 465, 464,  59, 467,
   470, 469, 472, 828, 475, 868, 478, 870, 483, 485, 486, 871, 488, 489, 872, 873,
   495, 497,  60, 498,  61,  61, 504, 505, 507, 508, 511,  62, 513, 874, 515, 875,
   518, 844, 520, 876, 877, 878,  63,  64, 528, 880, 879, 881, 882, 530, 531, 531,
   533,  66, 534,  67,  68, 884, 536, 538, 541,  69, 885, 549, 886, 887, 556, 559,
    70, 561, 562, 563, 888, 889, 889, 567,  71, 890, 570, 571,  72, 891, 577,  73,
   581, 579, 582, 893, 587,  74, 590, 592, 596,  75, 895, 896,  76, 897, 600, 898,
   602, 605, 607, 899, 900, 609, 901, 611, 853,  77, 615, 616,  79, 617, 252, 902,
   903, 854, 855, 621, 622, 731,  80, 627, 626, 628, 164, 629, 630, 631, 633, 904,
   632, 634, 639, 640, 635, 641, 646, 651, 638, 643, 644, 645, 905, 907, 906,  81,
   653, 654, 656, 911, 657, 908,  82,  83, 909, 910,  84, 664, 665, 666, 667, 669,
   668, 671, 670, 674, 672, 673, 675,  85, 677, 678,  86, 681, 682, 912, 685, 686,
    87, 689,  36, 913, 914,  88,  89, 696, 702, 709, 711, 915, 712, 713, 718, 719,
   917, 831, 721, 720, 723, 832, 725, 728, 918, 919, 739, 742, 744, 920, 745, 753,
   756, 757, 755, 760, 761, 921, 762,  90, 764, 922,  91, 775, 279, 780, 923, 925,
    92,  93, 785, 926,  94, 927, 787, 787, 789, 928, 792,  95, 796, 797, 798, 800,
    96, 929, 802, 804, 806,  97,  98, 807, 930,  99, 931, 932, 933, 814, 100, 816,
   817, 818, 819, 820, 821, 935,   0,   0,
};
static const int32_t _hb_ucd_palette[39]=
{
   -112883,   -2527,   -2250,   -2108,   -2106,   -2104,   -2016,   -1923,
     -1918,   -1914,   -1824,   -1316,    -138,     -16,      -8,      -7,
        -3,      -2,      -1,       0,       1,       2,       3,       7,
         8,      16,     138,    1316,    1824,    1914,    1918,    1923,
      2016,    2104,    2106,    2108,    2250,    2527,  112883,
};

static inline uint8_t _hb_ucd_gc (unsigned u)
{
  return u<1114110u ? (uint8_t)(_hb_ucd_u8[7944u+((_hb_ucd_u8[2176u+_hb_ucd_u16[((_hb_ucd_u8[((((((u)>>1))>>3))>>5)])<<5)+((((((u)>>1))>>3))&31)]+((((u)>>1))&7)])<<1)+((u)&1)]) : 2;
}
static inline uint8_t _hb_ucd_ccc (unsigned u)
{
  return u<125259u ? (uint8_t)(_hb_ucd_u8[10432u+((_hb_ucd_u8[9308u+((_hb_ucd_u8[8577u+((_hb_ucd_u8[8332u+((((((u)>>2))>>3))>>4)])<<4)+((((((u)>>2))>>3))&15)])<<3)+((((u)>>2))&7)])<<2)+((u)&3)]) : 0;
}
static inline uint8_t _hb_ucd_b4 (const uint8_t* a, unsigned i)
{
  return (a[i>>1]>>((i&1)<<2))&15;
}
static inline int32_t _hb_ucd_bmg (unsigned u)
{
  return u<121617u ? (int32_t)(_hb_ucd_palette[_hb_ucd_u8[11624u+_hb_ucd_u8[11272u+((_hb_ucd_u8[11071u+((_hb_ucd_b4(_hb_ucd_u8+10952u,((((((u)>>2))>>3))>>4)))<<4)+((((((u)>>2))>>3))&15)])<<3)+((((u)>>2))&7)]+((u)&3)]]) : 0;
}
static inline uint8_t _hb_ucd_sc (unsigned u)
{
  return u<918000u ? (uint8_t)(_hb_ucd_u8[12721u+_hb_ucd_u16[4520u+_hb_ucd_u16[3392u+((_hb_ucd_u8[11824u+((((((u)>>3))>>3))>>4)])<<4)+((((((u)>>3))>>3))&15)]+((((u)>>3))&7)]+((u)&7)]) : 2;
}
static inline uint16_t _hb_ucd_dm (unsigned u)
{
  return u<195102u ? (uint16_t)(_hb_ucd_u16[7816u+((_hb_ucd_u8[19095u+((_hb_ucd_u8[18713u+((((u)>>4))>>5)])<<5)+((((u)>>4))&31)])<<4)+((u)&15)]) : 0;
}

static const uint8_t _hb_emoji_u8[624]=
{
    1,  0,  0,  0, 50,  4,  5,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,118,152,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  2,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    3,  4,  0,  0,  5,  0,  6,  0,  0,  0,  0,  0,  7,  0,  0,  8,
    0,  0,  0,  9,  0,  0, 10, 11, 12, 13, 14, 15, 16, 17, 18,  0,
    0,  0,  0,  0, 19,  0,  0,  0,  0,  0,  0,  0, 20, 21,  0,  0,
   22,  0,  0,  0,  0,  0,  0,  0,  0,  0, 23,  0,  0,  0,  0,  0,
   24,  0, 25, 26,  0, 27, 28, 29, 30, 31,  1,  1, 32,  1, 33, 34,
    1,  1,  1, 35, 36, 37, 38, 39,  1, 40,  1, 41,  0,  0,  0, 42,
   43, 44, 45, 46, 47, 48,  1,  1,  0, 49,  1,  1,  0,  0,  0,  0,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    0,  0,  0,  0,  0,  0,  0,  0,255,255,255,255,255,255,255,255,
    0,  0,  0,  0,  0, 66,  0,  0,  0,  0,  0,  0,  0,  0,  0, 16,
    0,  2,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  4,  0,  0,  2,
    0,  0,240,  3,  0,  6,  0,  0,  0,  0,  0, 12,  0,  1,  0,  0,
    0,128,  0,  0,  0,254, 15,  7,  4,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0, 12, 64,  0,  1,  0,  0,  0,  0,  0,  0,120,
   31, 64, 50, 33, 77,196,  0,  7,  5,255, 15,128,105,  1,  0,200,
    0,  0,252, 26,131, 12,  3, 96, 48,193, 26,  0,  0,  6,191, 39,
   36,191, 84, 32,  2,  1, 24,  0,144, 80,184,  0, 24,  0,  0,  0,
    0,  0,224,  0,  2,  0,  1,128,  0,  0,  0,  0,  0,  0, 48,  0,
  224,  0,  0, 24,  0,  0,  0,  0,  0,  0, 33,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  1, 32,  0,  0,128,  2,  0,  0,  0,  0,
   16,  0,  0,  0,  0,240,  0,  0,  0,  0,240,255,  0,128,  1,  0,
    1,128,  1,  0,  0,  0,192,255,  0,  0,  0,  0,  0,  0,  3,192,
    0, 64,254,  7,  0,128,255,255,255,255,255,255, 63,  0,  0,  0,
  254,255,  0,  4,  0,128,252,247,  0,254,255,255,192,255,255,255,
  255,255,255,255,243,255,255,255,255,255,207,206,255,255,255,255,
  255,255,255,255,255,255,185,  7,255,255,255,255,255,255,255,191,
  255,255,255,255,255,255,255, 63,  0,126,255,255,255,128,249,  7,
  128, 60, 97,  0, 48,  1,  6, 16, 28,  0, 14,112, 10,129,  8,252,
  255,255,  0,  0,  0,  0,  0,  0, 63,248,231,255, 63,250,249,255,
    0,  0,  0,240,255,255,  1,  0,  0,240,  0,  0,  0,  0,  0,  0,
    0,255,  0,252,  0,  0,  0,  0,  0,255,  0,  0,  0,192,  0,240,
  252,255,  0,254,255,255,255,255,  0,240,255,255,255,255,255,247,
  191,255,255,255,255,255,255,255,  0,  0,  0,255,  0,192,255,255,
};

static inline uint8_t _hb_emoji_b4 (const uint8_t* a, unsigned i)
{
  return (a[i>>1]>>((i&1)<<2))&15;
}
static inline uint8_t _hb_emoji_b1 (const uint8_t* a, unsigned i)
{
  return (a[i>>3]>>((i&7)<<0))&1;
}
static inline uint8_t _hb_emoji_is_Extended_Pictographic (unsigned u)
{
  return u<131070u ? (uint8_t)(_hb_emoji_b1(_hb_emoji_u8+224u,((_hb_emoji_u8[64u+((_hb_emoji_b4(_hb_emoji_u8,((((u)>>6))>>4)))<<4)+((((u)>>6))&15)])<<6)+((u)&63))) : 0;
}

static const uint8_t modified_combining_class[256] =
{
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  22,  15,  16,  17,  23,  18,
   19,  20,  21,  14,  24,  12,  25,  13,  10,  11,  26,  28,  29,  30,  31,  32,
   33,  27,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,
   48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
   64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,
   80,  81,  82,  83,   4,  85,  86,  87,  88,  89,  90,   5,  92,  93,  94,  95,
   96,  97,  98,  99, 100, 101, 102,   3, 104, 105, 106, 107, 108, 109, 110, 111,
  112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
  128, 129, 132, 131, 131, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
  144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
  160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
  176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
  192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
  208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
  224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
  240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255,
};

static inline unsigned uni_gc (hb_codepoint_t u) { return _hb_ucd_gc (u); }
static inline unsigned uni_ccc (hb_codepoint_t u) { return _hb_ucd_ccc (u); }
static inline hb_codepoint_t uni_mirroring (hb_codepoint_t u) { return u + _hb_ucd_bmg (u); }
static inline hb_script_t uni_script (hb_codepoint_t u) { return _hb_ucd_sc_map[_hb_ucd_sc (u)]; }

#define SBASE 0xAC00u
#define LBASE 0x1100u
#define VBASE 0x1161u
#define TBASE 0x11A7u
#define SCOUNT 11172u
#define LCOUNT 19u
#define VCOUNT 21u
#define TCOUNT 28u
#define NCOUNT (VCOUNT * TCOUNT)

static int uni_compose (hb_codepoint_t a, hb_codepoint_t b, hb_codepoint_t *ab)
{
  *ab = 0;
  if (!a || !b) return 0;
  if (a >= SBASE && a < SBASE + SCOUNT && b > TBASE && b < TBASE + TCOUNT && !((a - SBASE) % TCOUNT))
  {
    *ab = a + (b - TBASE);
    return 1;
  }
  if (a >= LBASE && a < LBASE + LCOUNT && b >= VBASE && b < VBASE + VCOUNT)
  {
    *ab = SBASE + (a - LBASE) * NCOUNT + (b - VBASE) * TCOUNT;
    return 1;
  }
  hb_codepoint_t u = 0;
  if ((a & 0xFFFFF800u) == 0x0000u && (b & 0xFFFFFF80) == 0x0300u)
  {
    uint32_t k = ENCODE3_11_7_14 (a, b, 0);
    int min = 0, max = (int) (sizeof (_hb_ucd_dm2_u32_map) / sizeof (_hb_ucd_dm2_u32_map[0])) - 1;
    while (min <= max)
    {
      int mid = ((unsigned) min + (unsigned) max) / 2;
      uint32_t v = _hb_ucd_dm2_u32_map[mid], kk = v & ENCODE3_11_7_14 (0x1FFFFFu, 0x1FFFFFu, 0);
      if (k < kk) max = mid - 1;
      else if (k > kk) min = mid + 1;
      else { u = v & 0x3FFFu; break; }
    }
    if (min > max) return 0;
  }
  else
  {
    uint64_t k = ENCODE3 (a, b, 0);
    int min = 0, max = (int) (sizeof (_hb_ucd_dm2_u64_map) / sizeof (_hb_ucd_dm2_u64_map[0])) - 1;
    while (min <= max)
    {
      int mid = ((unsigned) min + (unsigned) max) / 2;
      uint64_t v = _hb_ucd_dm2_u64_map[mid], kk = v & ENCODE3 (0x1FFFFFu, 0x1FFFFFu, 0);
      if (k < kk) max = mid - 1;
      else if (k > kk) min = mid + 1;
      else { u = (hb_codepoint_t) v & 0x1FFFFFu; break; }
    }
    if (min > max) return 0;
  }
  if (!u) return 0;
  *ab = u;
  return 1;
}

static int uni_decompose (hb_codepoint_t ab, hb_codepoint_t *a, hb_codepoint_t *b)
{
  *a = ab;
  *b = 0;
  unsigned si = ab - SBASE;
  if (si < SCOUNT)
  {
    if (si % TCOUNT)
    {
      *a = SBASE + (si / TCOUNT) * TCOUNT;
      *b = TBASE + (si % TCOUNT);
    }
    else
    {
      *a = LBASE + (si / NCOUNT);
      *b = VBASE + (si % NCOUNT) / TCOUNT;
    }
    return 1;
  }
  unsigned i = _hb_ucd_dm (ab);
  if (!i) return 0;
  i--;
  const unsigned n0 = sizeof (_hb_ucd_dm1_p0_map) / 2, n2 = sizeof (_hb_ucd_dm1_p2_map) / 2, n32 = sizeof (_hb_ucd_dm2_u32_map) / 4;
  if (i < n0) { *a = _hb_ucd_dm1_p0_map[i]; *b = 0; return 1; }
  i -= n0;
  if (i < n2) { *a = 0x20000 | _hb_ucd_dm1_p2_map[i]; *b = 0; return 1; }
  i -= n2;
  if (i < n32)
  {
    uint32_t v = _hb_ucd_dm2_u32_map[i];
    *a = v >> 21;
    *b = ((v >> 14) & 0x007Fu) | 0x0300;
    return 1;
  }
  i -= n32;
  uint64_t v = _hb_ucd_dm2_u64_map[i];
  *a = (hb_codepoint_t) (v >> 42);
  *b = (hb_codepoint_t) (v >> 21) & 0x1FFFFFu;
  return 1;
}

static unsigned uni_modified_combining_class (hb_codepoint_t u)
{
  if (u == 0x1A60u) return 254;
  if (u == 0x0FC6u) return 254;
  if (u == 0x0F39u) return 127;
  return modified_combining_class[uni_ccc (u)];
}

#define IN_RANGE(u, lo, hi) ((uint32_t) ((u) - (lo)) <= (uint32_t) ((hi) - (lo)))

static int uni_is_variation_selector (hb_codepoint_t u)
{
  return IN_RANGE (u, 0xFE00u, 0xFE0Fu) || IN_RANGE (u, 0xE0100u, 0xE01EFu);
}

static int uni_is_default_ignorable (hb_codepoint_t ch)
{
  hb_codepoint_t plane = ch >> 16;
  if (plane == 0)
  {
    switch (ch >> 8)
    {
    case 0x00: return ch == 0x00ADu;
    case 0x03: return ch == 0x034Fu;
    case 0x06: return ch == 0x061Cu;
    case 0x17: return IN_RANGE (ch, 0x17B4u, 0x17B5u);
    case 0x18: return IN_RANGE (ch, 0x180Bu, 0x180Fu);
    case 0x20: return IN_RANGE (ch, 0x200Bu, 0x200Fu) || IN_RANGE (ch, 0x202Au, 0x202Eu) || IN_RANGE (ch, 0x2060u, 0x206Fu);
    case 0xFE: return IN_RANGE (ch, 0xFE00u, 0xFE0Fu) || ch == 0xFEFFu;
    case 0xFF: return IN_RANGE (ch, 0xFFF0u, 0xFFF8u);
    default: return 0;
    }
  }
  if (plane == 0x01) return IN_RANGE (ch, 0x1D173u, 0x1D17Au);
  if (plane == 0x0E) return IN_RANGE (ch, 0xE0000u, 0xE0FFFu);
  return 0;
}

enum { NOT_SPACE = 0, SPACE_EM = 1, SPACE_EM_2 = 2, SPACE_EM_3 = 3, SPACE_EM_4 = 4, SPACE_EM_5 = 5, SPACE_EM_6 = 6,
       SPACE_EM_16 = 16, SPACE_4_EM_18, SPACE, SPACE_FIGURE, SPACE_PUNCTUATION, SPACE_NARROW };

static unsigned uni_space_fallback_type (hb_codepoint_t u)
{
  switch (u)
  {
  case 0x0020u: case 0x00A0u: return SPACE;
  case 0x2000u: case 0x2002u: return SPACE_EM_2;
  case 0x2001u: case 0x2003u: case 0x3000u: return SPACE_EM;
  case 0x2004u: return SPACE_EM_3;
  case 0x2005u: return SPACE_EM_4;
  case 0x2006u: return SPACE_EM_6;
  case 0x2007u: return SPACE_FIGURE;
  case 0x2008u: return SPACE_PUNCTUATION;
  case 0x2009u: return SPACE_EM_5;
  case 0x200Au: return SPACE_EM_16;
  case 0x202Fu: return SPACE_NARROW;
  case 0x205Fu: return SPACE_4_EM_18;
  default: return NOT_SPACE;
  }
}

#define FLAG(x) (1u << (x))
#define FLAG_UNSAFE(x) ((unsigned) (x) < 32 ? 1u << (unsigned) (x) : 0)
#define FLAG_RANGE(x,y) (FLAG ((y) + 1) - FLAG (x))
#define FLAG64_UNSAFE(x) ((unsigned) (x) < 64 ? (uint64_t) 1 << (unsigned) (x) : 0)

enum
{
  GC_CONTROL, GC_FORMAT, GC_UNASSIGNED, GC_PRIVATE_USE, GC_SURROGATE,
  GC_LOWERCASE_LETTER, GC_MODIFIER_LETTER, GC_OTHER_LETTER, GC_TITLECASE_LETTER, GC_UPPERCASE_LETTER,
  GC_SPACING_MARK, GC_ENCLOSING_MARK, GC_NON_SPACING_MARK,
  GC_DECIMAL_NUMBER, GC_LETTER_NUMBER, GC_OTHER_NUMBER,
  GC_CONNECT_PUNCTUATION, GC_DASH_PUNCTUATION, GC_CLOSE_PUNCTUATION, GC_FINAL_PUNCTUATION,
  GC_INITIAL_PUNCTUATION, GC_OTHER_PUNCTUATION, GC_OPEN_PUNCTUATION,
  GC_CURRENCY_SYMBOL, GC_MODIFIER_SYMBOL, GC_MATH_SYMBOL, GC_OTHER_SYMBOL,
  GC_LINE_SEPARATOR, GC_PARAGRAPH_SEPARATOR, GC_SPACE_SEPARATOR
};

#define GC_IS_MARK(gc) (FLAG_UNSAFE (gc) & (FLAG (GC_SPACING_MARK) | FLAG (GC_ENCLOSING_MARK) | FLAG (GC_NON_SPACING_MARK)))

enum { DIR_LTR = 4, DIR_RTL = 5 };

#define BUFFER_MAX_LEN_FACTOR 256
#define BUFFER_MAX_LEN_MIN 65536
#define BUFFER_MAX_LEN_DEFAULT 0x3FFFFFFF
#define BUFFER_MAX_OPS_FACTOR 4096
#define BUFFER_MAX_OPS_MIN 65536
#define BUFFER_MAX_OPS_DEFAULT 0x1FFFFFFF
#define CONTEXT_LENGTH 5
#define MAX_SYLLABLE_LENGTH 4096

enum
{
  GLYPH_FLAG_UNSAFE_TO_BREAK = 1, /* also unsafe to concat */
  GLYPH_FLAG_UNSAFE_TO_CONCAT = 2,
  GLYPH_FLAG_SAFE_TO_INSERT_TATWEEL = 4,
  GLYPH_FLAG_DEFINED = 7
};

enum
{
  SCRATCH_HAS_FRACTION_SLASH = 0x01,
  SCRATCH_HAS_DEFAULT_IGNORABLES = 0x02,
  SCRATCH_HAS_SPACE_FALLBACK = 0x04,
  SCRATCH_HAS_GPOS_ATTACHMENT = 0x08,
  SCRATCH_HAS_CGJ = 0x10,
  SCRATCH_HAS_BROKEN_SYLLABLE = 0x20,
  SCRATCH_HAS_VARIATION_SELECTOR_FALLBACK = 0x40,
  SCRATCH_HAS_CONTINUATIONS = 0x80,
  SCRATCH_SHAPER0 = 0x01000000
};

#define DIGEST_N 3
static const unsigned digest_shifts[DIGEST_N] = {4, 0, 6};
typedef struct { uint64_t m[DIGEST_N]; } digest_t;

static void digest_add (digest_t *d, hb_codepoint_t g)
{
  for (unsigned i = 0; i < DIGEST_N; i++) d->m[i] |= (uint64_t) 1 << ((g >> digest_shifts[i]) & 63);
}
static int digest_add_range (digest_t *d, hb_codepoint_t a, hb_codepoint_t b)
{
  int ret = 0;
  for (unsigned i = 0; i < DIGEST_N; i++) if (d->m[i] != (uint64_t) -1) ret = 1;
  if (!ret) return 0;
  ret = 0;
  for (unsigned i = 0; i < DIGEST_N; i++)
  {
    unsigned shift = digest_shifts[i];
    if ((b >> shift) - (a >> shift) >= 63) d->m[i] = (uint64_t) -1;
    else
    {
      uint64_t ma = (uint64_t) 1 << ((a >> shift) & 63), mb = (uint64_t) 1 << ((b >> shift) & 63);
      d->m[i] |= mb + (mb - ma) - (mb < ma);
      ret = 1;
    }
  }
  return ret;
}
static int digest_may_have (const digest_t *d, hb_codepoint_t g)
{
  for (unsigned i = 0; i < DIGEST_N; i++)
    if (!(d->m[i] & ((uint64_t) 1 << ((g >> digest_shifts[i]) & 63)))) return 0;
  return 1;
}
static int digest_may_intersect (const digest_t *a, const digest_t *b)
{
  for (unsigned i = 0; i < DIGEST_N; i++) if (!(a->m[i] & b->m[i])) return 0;
  return 1;
}
static void digest_union (digest_t *a, const digest_t *b)
{
  for (unsigned i = 0; i < DIGEST_N; i++) a->m[i] |= b->m[i];
}

struct hb_buffer_t
{
  int content_unicode;
  hb_script_t script;
  int direction;
  int successful, have_output, have_positions;
  unsigned idx, len, out_len, allocated;
  hb_glyph_info_t *info, *out_info;
  hb_glyph_position_t *pos;
  hb_codepoint_t context[2][CONTEXT_LENGTH];
  unsigned context_len[2];
  digest_t digest;
  uint8_t serial;
  uint32_t random_state;
  unsigned scratch_flags;
  unsigned max_len;
  int max_ops;
};

#define UPROPS(i) ((i)->var2.u16[0])
#define GPROPS(i) ((i)->var1.u16[0])
#define LIGPROPS(i) ((i)->var1.u8[2])
#define SYLLABLE(i) ((i)->var1.u8[3])

enum
{
  UPROPS_MASK_GEN_CAT = 0x001F,
  UPROPS_MASK_IGNORABLE = 0x0020,
  UPROPS_MASK_HIDDEN = 0x0040,
  UPROPS_MASK_CONTINUATION = 0x0080,
  UPROPS_MASK_Cf_ZWJ = 0x0100,
  UPROPS_MASK_Cf_ZWNJ = 0x0200,
  UPROPS_MASK_Cf_VS = 0x0400
};

enum
{
  GLYPH_PROPS_BASE_GLYPH = 0x02,
  GLYPH_PROPS_LIGATURE = 0x04,
  GLYPH_PROPS_MARK = 0x08,
  GLYPH_PROPS_SUBSTITUTED = 0x10,
  GLYPH_PROPS_LIGATED = 0x20,
  GLYPH_PROPS_MULTIPLIED = 0x40,
  GLYPH_PROPS_PRESERVE = 0x70
};

static int buf_enlarge (hb_buffer_t *b, unsigned size)
{
  if (size > b->max_len) { b->successful = 0; return 0; }
  if (!b->successful) return 0;
  unsigned new_allocated = b->allocated;
  hb_glyph_position_t *new_pos = NULL;
  hb_glyph_info_t *new_info = NULL;
  int separate_out = b->out_info != b->info;
  if (size >= UINT32_MAX / sizeof (hb_glyph_info_t)) goto done;
  while (size >= new_allocated) new_allocated += (new_allocated >> 1) + 32;
  if (new_allocated >= UINT32_MAX / sizeof (hb_glyph_info_t)) goto done;
  new_pos = (hb_glyph_position_t *) realloc (b->pos, new_allocated * sizeof (hb_glyph_info_t));
  new_info = (hb_glyph_info_t *) realloc (b->info, new_allocated * sizeof (hb_glyph_info_t));
done:
  if (!new_pos || !new_info) b->successful = 0;
  if (new_pos) b->pos = new_pos;
  if (new_info) b->info = new_info;
  b->out_info = separate_out ? (hb_glyph_info_t *) b->pos : b->info;
  if (b->successful) b->allocated = new_allocated;
  return b->successful;
}

static int buf_ensure (hb_buffer_t *b, unsigned size)
{
  return !size || size < b->allocated ? 1 : buf_enlarge (b, size);
}

static int buf_make_room_for (hb_buffer_t *b, unsigned num_in, unsigned num_out)
{
  if (!buf_ensure (b, b->out_len + num_out)) return 0;
  if (b->out_info == b->info && b->out_len + num_out > b->idx + num_in)
  {
    b->out_info = (hb_glyph_info_t *) b->pos;
    memcpy (b->out_info, b->info, b->out_len * sizeof (hb_glyph_info_t));
  }
  return 1;
}

static int buf_shift_forward (hb_buffer_t *b, unsigned count)
{
  if (!buf_ensure (b, b->len + count)) return 0;
  b->max_ops -= b->len - b->idx;
  if (b->max_ops < 0) { b->successful = 0; return 0; }
  memmove (b->info + b->idx + count, b->info + b->idx, (b->len - b->idx) * sizeof (hb_glyph_info_t));
  if (b->idx + count > b->len)
    memset (b->info + b->len, 0, (b->idx + count - b->len) * sizeof (hb_glyph_info_t));
  b->len += count;
  b->idx += count;
  return 1;
}

static void buf_clear (hb_buffer_t *b)
{
  b->content_unicode = 0;
  b->script = HB_SCRIPT_INVALID;
  b->direction = 0;
  b->successful = 1;
  b->have_output = b->have_positions = 0;
  b->idx = b->len = b->out_len = 0;
  b->out_info = b->info;
  memset (b->context, 0, sizeof b->context);
  memset (b->context_len, 0, sizeof b->context_len);
  b->serial = 0;
  b->random_state = 1;
  b->scratch_flags = 0;
}

static void buf_enter (hb_buffer_t *b)
{
  b->serial = 0;
  b->scratch_flags = 0;
  if (b->len < UINT32_MAX / BUFFER_MAX_LEN_FACTOR)
    b->max_len = HB_MAX (b->len * BUFFER_MAX_LEN_FACTOR, (unsigned) BUFFER_MAX_LEN_MIN);
  if (b->len < UINT32_MAX / BUFFER_MAX_OPS_FACTOR)
  {
    unsigned mul = HB_MAX (b->len * BUFFER_MAX_OPS_FACTOR, (unsigned) BUFFER_MAX_OPS_MIN);
    b->max_ops = (int) mul;
  }
}

static void buf_leave (hb_buffer_t *b)
{
  b->max_len = BUFFER_MAX_LEN_DEFAULT;
  b->max_ops = BUFFER_MAX_OPS_DEFAULT;
  b->serial = 0;
}

static void buf_add (hb_buffer_t *b, hb_codepoint_t cp, unsigned cluster)
{
  if (!buf_ensure (b, b->len + 1)) return;
  hb_glyph_info_t *g = &b->info[b->len];
  memset (g, 0, sizeof (*g));
  g->codepoint = cp;
  g->cluster = cluster;
  b->len++;
}

static void buf_clear_output (hb_buffer_t *b)
{
  b->have_output = 1;
  b->have_positions = 0;
  b->idx = 0;
  b->out_len = 0;
  b->out_info = b->info;
}

static void buf_clear_positions (hb_buffer_t *b)
{
  b->have_output = 0;
  b->have_positions = 1;
  b->out_len = 0;
  b->out_info = b->info;
  memset (b->pos, 0, sizeof (b->pos[0]) * b->len);
}

static inline hb_glyph_info_t *buf_cur (hb_buffer_t *b) { return &b->info[b->idx]; }
static inline hb_glyph_info_t *buf_prev (hb_buffer_t *b) { return &b->out_info[b->out_len ? b->out_len - 1 : 0]; }
static inline uint8_t buf_next_serial (hb_buffer_t *b) { return ++b->serial ? b->serial : ++b->serial; }
static inline unsigned buf_backtrack_len (const hb_buffer_t *b) { return b->have_output ? b->out_len : b->idx; }

static int buf_next_glyphs (hb_buffer_t *b, unsigned n)
{
  if (b->have_output)
  {
    if (b->out_info != b->info || b->out_len != b->idx)
    {
      if (!buf_ensure (b, b->out_len + n)) return 0;
      memmove (b->out_info + b->out_len, b->info + b->idx, n * sizeof (hb_glyph_info_t));
    }
    b->out_len += n;
  }
  b->idx += n;
  return 1;
}

static int buf_next_glyph (hb_buffer_t *b)
{
  if (b->have_output)
  {
    if (b->out_info != b->info || b->out_len != b->idx)
    {
      if (!buf_ensure (b, b->out_len + 1)) return 0;
      b->out_info[b->out_len] = b->info[b->idx];
    }
    b->out_len++;
  }
  b->idx++;
  return 1;
}

static inline void buf_skip_glyph (hb_buffer_t *b) { b->idx++; }

static int buf_sync (hb_buffer_t *b)
{
  int ret = 0;
  if (!b->successful || !buf_next_glyphs (b, b->len - b->idx)) goto reset;
  if (b->out_info != b->info)
  {
    b->pos = (hb_glyph_position_t *) b->info;
    b->info = b->out_info;
  }
  b->len = b->out_len;
  ret = 1;
reset:
  b->have_output = 0;
  b->out_len = 0;
  b->out_info = b->info;
  b->idx = 0;
  return ret;
}

static int buf_move_to (hb_buffer_t *b, unsigned i)
{
  if (!b->have_output) { b->idx = i; return 1; }
  if (!b->successful) return 0;
  if (b->out_len < i)
  {
    unsigned count = i - b->out_len;
    if (!buf_make_room_for (b, count, count)) return 0;
    b->max_ops -= count;
    if (b->max_ops < 0) { b->successful = 0; return 0; }
    memmove (b->out_info + b->out_len, b->info + b->idx, count * sizeof (hb_glyph_info_t));
    b->idx += count;
    b->out_len += count;
  }
  else if (b->out_len > i)
  {
    unsigned count = b->out_len - i;
    if (b->idx < count && !buf_shift_forward (b, count - b->idx)) return 0;
    b->max_ops -= count;
    if (b->max_ops < 0) { b->successful = 0; return 0; }
    b->idx -= count;
    b->out_len -= count;
    memmove (b->info + b->idx, b->out_info + b->out_len, count * sizeof (hb_glyph_info_t));
  }
  return 1;
}

static void buf_set_masks (hb_buffer_t *b, hb_mask_t value, hb_mask_t mask, unsigned cluster_start, unsigned cluster_end)
{
  if (!mask) return;
  hb_mask_t not_mask = ~mask;
  value &= mask;
  b->max_ops -= b->len;
  if (b->max_ops < 0) b->successful = 0;
  if (cluster_start == 0 && cluster_end == (unsigned) -1)
  {
    for (unsigned i = 0; i < b->len; i++) b->info[i].mask = (b->info[i].mask & not_mask) | value;
    return;
  }
  for (unsigned i = 0; i < b->len; i++)
    if (cluster_start <= b->info[i].cluster && b->info[i].cluster < cluster_end)
      b->info[i].mask = (b->info[i].mask & not_mask) | value;
}

static inline void set_cluster (hb_glyph_info_t *inf, unsigned cluster, unsigned mask)
{
  if (inf->cluster != cluster) inf->mask = (inf->mask & ~GLYPH_FLAG_DEFINED) | (mask & GLYPH_FLAG_DEFINED);
  inf->cluster = cluster;
}

static void buf_merge_clusters (hb_buffer_t *b, unsigned start, unsigned end)
{
  if (end - start < 2) return;
  b->max_ops -= end - start;
  if (b->max_ops < 0) b->successful = 0;
  hb_glyph_info_t *info = b->info;
  unsigned cluster = info[start].cluster;
  for (unsigned i = start + 1; i < end; i++) cluster = HB_MIN (cluster, info[i].cluster);
  if (cluster != info[end - 1].cluster)
    while (end < b->len && info[end - 1].cluster == info[end].cluster) end++;
  if (cluster != info[start].cluster)
    while (b->idx < start && info[start - 1].cluster == info[start].cluster) start--;
  if (b->idx == start && info[start].cluster != cluster)
    for (unsigned i = b->out_len; i && b->out_info[i - 1].cluster == info[start].cluster; i--)
      set_cluster (&b->out_info[i - 1], cluster, 0);
  for (unsigned i = start; i < end; i++) set_cluster (&info[i], cluster, 0);
}

static void buf_merge_out_clusters (hb_buffer_t *b, unsigned start, unsigned end)
{
  if (end - start < 2) return;
  b->max_ops -= end - start;
  if (b->max_ops < 0) b->successful = 0;
  hb_glyph_info_t *out = b->out_info;
  unsigned cluster = out[start].cluster;
  for (unsigned i = start + 1; i < end; i++) cluster = HB_MIN (cluster, out[i].cluster);
  while (start && out[start - 1].cluster == out[start].cluster) start--;
  while (end < b->out_len && out[end - 1].cluster == out[end].cluster) end++;
  if (end == b->out_len)
    for (unsigned i = b->idx; i < b->len && b->info[i].cluster == out[end - 1].cluster; i++)
      set_cluster (&b->info[i], cluster, 0);
  for (unsigned i = start; i < end; i++) set_cluster (&out[i], cluster, 0);
}

static int buf_replace_glyphs (hb_buffer_t *b, unsigned num_in, unsigned num_out, const hb_codepoint_t *glyph_data)
{
  if (!buf_make_room_for (b, num_in, num_out)) return 0;
  buf_merge_clusters (b, b->idx, b->idx + num_in);
  hb_glyph_info_t orig = b->idx < b->len ? *buf_cur (b) : *buf_prev (b);
  hb_glyph_info_t *p = &b->out_info[b->out_len];
  for (unsigned i = 0; i < num_out; i++)
  {
    *p = orig;
    p->codepoint = glyph_data[i];
    p++;
  }
  b->idx += num_in;
  b->out_len += num_out;
  return 1;
}

static int buf_replace_glyph (hb_buffer_t *b, hb_codepoint_t g) { return buf_replace_glyphs (b, 1, 1, &g); }
static int buf_output_glyph (hb_buffer_t *b, hb_codepoint_t g) { return buf_replace_glyphs (b, 0, 1, &g); }

static int buf_output_info (hb_buffer_t *b, hb_glyph_info_t gi)
{
  if (!buf_make_room_for (b, 0, 1)) return 0;
  b->out_info[b->out_len++] = gi;
  return 1;
}

static int buf_copy_glyph (hb_buffer_t *b) { return buf_output_info (b, *buf_cur (b)); }

static void buf_delete_glyph (hb_buffer_t *b)
{
  unsigned cluster = b->info[b->idx].cluster;
  if ((b->idx + 1 < b->len && cluster == b->info[b->idx + 1].cluster) ||
      (b->out_len && cluster == b->out_info[b->out_len - 1].cluster))
    goto done;
  if (b->out_len)
  {
    if (cluster < b->out_info[b->out_len - 1].cluster)
    {
      unsigned mask = b->info[b->idx].mask;
      unsigned old_cluster = b->out_info[b->out_len - 1].cluster;
      for (unsigned i = b->out_len; i && b->out_info[i - 1].cluster == old_cluster; i--)
	set_cluster (&b->out_info[i - 1], cluster, mask);
    }
    goto done;
  }
  if (b->idx + 1 < b->len) buf_merge_clusters (b, b->idx, b->idx + 2);
done:
  buf_skip_glyph (b);
}

static void buf_delete_glyphs_inplace (hb_buffer_t *b, int (*filter) (const hb_glyph_info_t *))
{
  unsigned j = 0, count = b->len;
  hb_glyph_info_t *info = b->info;
  for (unsigned i = 0; i < count; i++)
  {
    if (filter (&info[i]))
    {
      unsigned cluster = info[i].cluster;
      if (i + 1 < count && cluster == info[i + 1].cluster) continue;
      if (j)
      {
	if (cluster < info[j - 1].cluster)
	{
	  unsigned mask = info[i].mask;
	  unsigned old_cluster = info[j - 1].cluster;
	  for (unsigned k = j; k && info[k - 1].cluster == old_cluster; k--)
	    set_cluster (&info[k - 1], cluster, mask);
	}
	continue;
      }
      if (i + 1 < count) buf_merge_clusters (b, i, i + 2);
      continue;
    }
    if (j != i)
    {
      info[j] = info[i];
      b->pos[j] = b->pos[i];
    }
    j++;
  }
  b->len = j;
}

static void infos_set_glyph_flags (hb_buffer_t *b, hb_glyph_info_t *infos, unsigned start, unsigned end, unsigned cluster, hb_mask_t mask)
{
  if (start == end) return;
  b->max_ops -= end - start;
  if (b->max_ops < 0) b->successful = 0;
  unsigned cluster_first = infos[start].cluster, cluster_last = infos[end - 1].cluster;
  if (cluster != cluster_first && cluster != cluster_last)
  {
    for (unsigned i = start; i < end; i++)
      if (cluster != infos[i].cluster) infos[i].mask |= mask;
    return;
  }
  if (cluster == cluster_first)
  {
    for (unsigned i = end; start < i && infos[i - 1].cluster != cluster_first; i--) infos[i - 1].mask |= mask;
  }
  else
  {
    for (unsigned i = start; i < end && infos[i].cluster != cluster_last; i++) infos[i].mask |= mask;
  }
}

static unsigned infos_find_min_cluster (const hb_glyph_info_t *infos, unsigned start, unsigned end, unsigned cluster)
{
  if (start == end) return cluster;
  return HB_MIN (cluster, HB_MIN (infos[start].cluster, infos[end - 1].cluster));
}

static void buf_set_glyph_flags (hb_buffer_t *b, hb_mask_t mask, unsigned start, unsigned end, int interior, int from_out_buffer)
{
  if (end != (unsigned) -1 && end - start > 255) return;
  end = HB_MIN (end, b->len);
  if (interior && !from_out_buffer && end - start < 2) return;
  if (!from_out_buffer || !b->have_output)
  {
    if (!interior)
      for (unsigned i = start; i < end; i++) b->info[i].mask |= mask;
    else
    {
      unsigned cluster = infos_find_min_cluster (b->info, start, end, UINT32_MAX);
      infos_set_glyph_flags (b, b->info, start, end, cluster, mask);
    }
  }
  else
  {
    if (!interior)
    {
      for (unsigned i = start; i < b->out_len; i++) b->out_info[i].mask |= mask;
      for (unsigned i = b->idx; i < end; i++) b->info[i].mask |= mask;
    }
    else
    {
      unsigned cluster = infos_find_min_cluster (b->info, b->idx, end, UINT32_MAX);
      cluster = infos_find_min_cluster (b->out_info, start, b->out_len, cluster);
      infos_set_glyph_flags (b, b->out_info, start, b->out_len, cluster, mask);
      infos_set_glyph_flags (b, b->info, b->idx, end, cluster, mask);
    }
  }
}

static void buf_unsafe_to_break (hb_buffer_t *b, unsigned start, unsigned end)
{
  buf_set_glyph_flags (b, GLYPH_FLAG_UNSAFE_TO_BREAK | GLYPH_FLAG_UNSAFE_TO_CONCAT, start, end, 1, 0);
}

static void buf_unsafe_to_break_from_outbuffer (hb_buffer_t *b, unsigned start, unsigned end)
{
  buf_set_glyph_flags (b, GLYPH_FLAG_UNSAFE_TO_BREAK | GLYPH_FLAG_UNSAFE_TO_CONCAT, start, end, 1, 1);
}

static void buf_reverse_range (hb_buffer_t *b, unsigned start, unsigned end)
{
  if (start >= end || end > b->len) return;
  for (unsigned i = start, j = end - 1; i < j; i++, j--)
  {
    hb_glyph_info_t t = b->info[i]; b->info[i] = b->info[j]; b->info[j] = t;
  }
  if (b->have_positions)
    for (unsigned i = start, j = end - 1; i < j; i++, j--)
    {
      hb_glyph_position_t t = b->pos[i]; b->pos[i] = b->pos[j]; b->pos[j] = t;
    }
}

static void buf_reverse (hb_buffer_t *b) { buf_reverse_range (b, 0, b->len); }

static void buf_reverse_groups (hb_buffer_t *b, int (*group) (const hb_glyph_info_t *, const hb_glyph_info_t *), int merge)
{
  if (!b->len) return;
  unsigned start = 0, i;
  for (i = 1; i < b->len; i++)
    if (!group (&b->info[i - 1], &b->info[i]))
    {
      if (merge) buf_merge_clusters (b, start, i);
      buf_reverse_range (b, start, i);
      start = i;
    }
  if (merge) buf_merge_clusters (b, start, i);
  buf_reverse_range (b, start, i);
  buf_reverse (b);
}

static unsigned buf_group_end (const hb_buffer_t *b, unsigned start, int (*group) (const hb_glyph_info_t *, const hb_glyph_info_t *))
{
  while (++start < b->len && group (&b->info[start - 1], &b->info[start]));
  return start;
}

static int cluster_group_func (const hb_glyph_info_t *a, const hb_glyph_info_t *b) { return a->cluster == b->cluster; }

#define foreach_group(buffer, start, end, group_func) \
  for (unsigned _count = (buffer)->len, start = 0, end = _count ? buf_group_end (buffer, 0, group_func) : 0; \
       start < _count; start = end, end = buf_group_end (buffer, start, group_func))

#define foreach_cluster(buffer, start, end) foreach_group (buffer, start, end, cluster_group_func)

static void buf_sort (hb_buffer_t *b, unsigned start, unsigned end, int (*compar) (const hb_glyph_info_t *, const hb_glyph_info_t *))
{
  for (unsigned i = start + 1; i < end; i++)
  {
    unsigned j = i;
    while (j > start && compar (&b->info[j - 1], &b->info[i]) > 0) j--;
    if (i == j) continue;
    buf_merge_clusters (b, j, i + 1);
    hb_glyph_info_t t = b->info[i];
    memmove (&b->info[j + 1], &b->info[j], (i - j) * sizeof (hb_glyph_info_t));
    b->info[j] = t;
  }
}

static void buf_update_digest (hb_buffer_t *b)
{
  memset (&b->digest, 0, sizeof b->digest);
  for (unsigned i = 0; i < b->len; i++) digest_add (&b->digest, b->info[i].codepoint);
}

static void info_set_unicode_props (hb_glyph_info_t *info, hb_buffer_t *buffer)
{
  unsigned u = info->codepoint;
  unsigned gen_cat = uni_gc (u);
  unsigned props = gen_cat;
  if (u >= 0x80u)
  {
    if (uni_is_default_ignorable (u))
    {
      buffer->scratch_flags |= SCRATCH_HAS_DEFAULT_IGNORABLES;
      props |= UPROPS_MASK_IGNORABLE;
      if (u == 0x200Cu) props |= UPROPS_MASK_Cf_ZWNJ;
      else if (u == 0x200Du) props |= UPROPS_MASK_Cf_ZWJ;
      else if (IN_RANGE (u, 0x180Bu, 0x180Du) || u == 0x180Fu) props |= UPROPS_MASK_HIDDEN;
      else if (IN_RANGE (u, 0xE0020u, 0xE007Fu)) props |= UPROPS_MASK_HIDDEN;
      else if (u == 0x034Fu)
      {
	buffer->scratch_flags |= SCRATCH_HAS_CGJ;
	props |= UPROPS_MASK_HIDDEN;
      }
    }
    if (GC_IS_MARK (gen_cat))
    {
      buffer->scratch_flags |= SCRATCH_HAS_CONTINUATIONS;
      props |= UPROPS_MASK_CONTINUATION;
      props |= uni_modified_combining_class (u) << 8;
    }
  }
  UPROPS (info) = props;
}

static inline unsigned info_gc (const hb_glyph_info_t *i) { return UPROPS (i) & UPROPS_MASK_GEN_CAT; }
static inline void info_set_gc (hb_glyph_info_t *i, unsigned gc) { UPROPS (i) = gc | (UPROPS (i) & (0xFF & ~UPROPS_MASK_GEN_CAT)); }
static inline int info_is_unicode_mark (const hb_glyph_info_t *i) { return GC_IS_MARK (info_gc (i)) != 0; }
static inline void info_set_modified_combining_class (hb_glyph_info_t *i, unsigned c)
{
  if (!info_is_unicode_mark (i)) return;
  UPROPS (i) = (c << 8) | (UPROPS (i) & 0xFF);
}
static inline unsigned info_cc (const hb_glyph_info_t *i) { return info_is_unicode_mark (i) ? UPROPS (i) >> 8 : 0; }
static inline int info_is_unicode_space (const hb_glyph_info_t *i) { return info_gc (i) == GC_SPACE_SEPARATOR; }
static inline void info_set_space_fallback_type (hb_glyph_info_t *i, unsigned s)
{
  if (!info_is_unicode_space (i)) return;
  UPROPS (i) = (s << 8) | (UPROPS (i) & 0xFF);
}
static inline unsigned info_space_fallback_type (const hb_glyph_info_t *i) { return info_is_unicode_space (i) ? UPROPS (i) >> 8 : NOT_SPACE; }
static inline int info_is_variation_selector (const hb_glyph_info_t *i) { return info_gc (i) == GC_FORMAT && (UPROPS (i) & UPROPS_MASK_Cf_VS); }
static inline void info_set_variation_selector (hb_glyph_info_t *i, int customize)
{
  if (customize)
  {
    info_set_gc (i, GC_FORMAT);
    UPROPS (i) |= UPROPS_MASK_Cf_VS;
  }
  else
    info_set_gc (i, GC_NON_SPACING_MARK);
}
static inline int info_substituted (const hb_glyph_info_t *i) { return (GPROPS (i) & GLYPH_PROPS_SUBSTITUTED) != 0; }
static inline int info_is_default_ignorable (const hb_glyph_info_t *i) { return (UPROPS (i) & UPROPS_MASK_IGNORABLE) && !info_substituted (i); }
static inline void info_clear_default_ignorable (hb_glyph_info_t *i) { UPROPS (i) &= ~UPROPS_MASK_IGNORABLE; }
static inline int info_is_hidden (const hb_glyph_info_t *i) { return (UPROPS (i) & UPROPS_MASK_HIDDEN) != 0; }
static inline void info_unhide (hb_glyph_info_t *i) { UPROPS (i) &= ~UPROPS_MASK_HIDDEN; }
static inline void info_set_continuation (hb_glyph_info_t *i, hb_buffer_t *b)
{
  b->scratch_flags |= SCRATCH_HAS_CONTINUATIONS;
  UPROPS (i) |= UPROPS_MASK_CONTINUATION;
}
static inline void info_clear_continuation (hb_glyph_info_t *i) { UPROPS (i) &= ~UPROPS_MASK_CONTINUATION; }
static inline int info_is_continuation (const hb_glyph_info_t *i) { return (UPROPS (i) & UPROPS_MASK_CONTINUATION) != 0; }
static int grapheme_group_func (const hb_glyph_info_t *a, const hb_glyph_info_t *b) { return info_is_continuation (b); }
#define foreach_grapheme(buffer, start, end) foreach_group (buffer, start, end, grapheme_group_func)
static inline int info_is_unicode_format (const hb_glyph_info_t *i) { return info_gc (i) == GC_FORMAT; }
static inline int info_is_zwnj (const hb_glyph_info_t *i) { return info_is_unicode_format (i) && (UPROPS (i) & UPROPS_MASK_Cf_ZWNJ); }
static inline int info_is_zwj (const hb_glyph_info_t *i) { return info_is_unicode_format (i) && (UPROPS (i) & UPROPS_MASK_Cf_ZWJ); }

#define IS_LIG_BASE 0x10
static inline void info_clear_lig_props (hb_glyph_info_t *i) { LIGPROPS (i) = 0; }
static inline void info_set_lig_props_for_ligature (hb_glyph_info_t *i, unsigned lig_id, unsigned n) { LIGPROPS (i) = (lig_id << 5) | IS_LIG_BASE | (n & 0x0F); }
static inline void info_set_lig_props_for_mark (hb_glyph_info_t *i, unsigned lig_id, unsigned comp) { LIGPROPS (i) = (lig_id << 5) | (comp & 0x0F); }
static inline void info_set_lig_props_for_component (hb_glyph_info_t *i, unsigned comp) { info_set_lig_props_for_mark (i, 0, comp); }
static inline unsigned info_lig_id (const hb_glyph_info_t *i) { return LIGPROPS (i) >> 5; }
static inline int info_ligated_internal (const hb_glyph_info_t *i) { return (LIGPROPS (i) & IS_LIG_BASE) != 0; }
static inline unsigned info_lig_comp (const hb_glyph_info_t *i) { return info_ligated_internal (i) ? 0 : LIGPROPS (i) & 0x0F; }
static inline unsigned info_lig_num_comps (const hb_glyph_info_t *i)
{
  return (GPROPS (i) & GLYPH_PROPS_LIGATURE) && info_ligated_internal (i) ? LIGPROPS (i) & 0x0F : 1;
}
static uint8_t allocate_lig_id (hb_buffer_t *b)
{
  uint8_t lig_id = buf_next_serial (b) & 0x07;
  if (!lig_id) lig_id = allocate_lig_id (b);
  return lig_id;
}
static inline int info_is_base_glyph (const hb_glyph_info_t *i) { return (GPROPS (i) & GLYPH_PROPS_BASE_GLYPH) != 0; }
static inline int info_is_ligature (const hb_glyph_info_t *i) { return (GPROPS (i) & GLYPH_PROPS_LIGATURE) != 0; }
static inline int info_is_mark (const hb_glyph_info_t *i) { return (GPROPS (i) & GLYPH_PROPS_MARK) != 0; }
static inline int info_ligated (const hb_glyph_info_t *i) { return (GPROPS (i) & GLYPH_PROPS_LIGATED) != 0; }
static inline int info_multiplied (const hb_glyph_info_t *i) { return (GPROPS (i) & GLYPH_PROPS_MULTIPLIED) != 0; }
static inline unsigned info_lig_num_comps_in_ligation (const hb_glyph_info_t *i)
{
  if (info_multiplied (i) && info_lig_comp (i)) return 0;
  return info_lig_num_comps (i);
}
static inline int info_ligated_and_didnt_multiply (const hb_glyph_info_t *i) { return info_ligated (i) && !info_multiplied (i); }
static inline void info_clear_ligated_and_multiplied (hb_glyph_info_t *i) { GPROPS (i) &= ~(GLYPH_PROPS_LIGATED | GLYPH_PROPS_MULTIPLIED); }

static unsigned next_syllable (hb_buffer_t *b, unsigned start)
{
  hb_glyph_info_t *info = b->info;
  unsigned count = start + HB_MIN (b->len - start, (unsigned) MAX_SYLLABLE_LENGTH);
  unsigned syllable = SYLLABLE (&info[start]);
  while (++start < count && syllable == SYLLABLE (&info[start]));
  return start;
}

#define foreach_syllable(buffer, start, end) \
  for (unsigned _count = (buffer)->len, start = 0, end = _count ? next_syllable (buffer, 0) : 0; \
       start < _count; start = end, end = next_syllable (buffer, start))

hb_buffer_t *hb_buffer_create (void)
{
  hb_buffer_t *b = (hb_buffer_t *) calloc (1, sizeof (hb_buffer_t));
  if (!b) return NULL;
  b->max_len = BUFFER_MAX_LEN_DEFAULT;
  b->max_ops = BUFFER_MAX_OPS_DEFAULT;
  buf_clear (b);
  return b;
}

void hb_buffer_destroy (hb_buffer_t *b)
{
  if (!b) return;
  free (b->info);
  free (b->pos);
  free (b);
}

void hb_buffer_clear_contents (hb_buffer_t *b) { buf_clear (b); }

hb_glyph_info_t *hb_buffer_get_glyph_infos (hb_buffer_t *b, unsigned int *length)
{
  if (length) *length = b->len;
  return b->info;
}

hb_glyph_position_t *hb_buffer_get_glyph_positions (hb_buffer_t *b, unsigned int *length)
{
  if (length) *length = b->len;
  if (!b->have_positions) buf_clear_positions (b);
  return b->pos;
}

static int script_is_rtl (hb_script_t s)
{
  switch ((unsigned) s)
  {
  case HB_SCRIPT_ARABIC: case HB_SCRIPT_HEBREW: case HB_SCRIPT_SYRIAC: case HB_SCRIPT_THAANA:
  case HB_SCRIPT_CYPRIOT: case HB_SCRIPT_KHAROSHTHI: case HB_SCRIPT_PHOENICIAN: case HB_SCRIPT_NKO:
  case HB_SCRIPT_LYDIAN: case HB_SCRIPT_AVESTAN: case HB_SCRIPT_IMPERIAL_ARAMAIC: case HB_SCRIPT_INSCRIPTIONAL_PAHLAVI:
  case HB_SCRIPT_INSCRIPTIONAL_PARTHIAN: case HB_SCRIPT_OLD_SOUTH_ARABIAN: case HB_SCRIPT_OLD_TURKIC: case HB_SCRIPT_SAMARITAN:
  case HB_SCRIPT_MANDAIC: case HB_SCRIPT_MEROITIC_CURSIVE: case HB_SCRIPT_MEROITIC_HIEROGLYPHS: case HB_SCRIPT_MANICHAEAN:
  case HB_SCRIPT_MENDE_KIKAKUI: case HB_SCRIPT_NABATAEAN: case HB_SCRIPT_OLD_NORTH_ARABIAN: case HB_SCRIPT_PALMYRENE:
  case HB_SCRIPT_PSALTER_PAHLAVI: case HB_SCRIPT_HATRAN: case HB_SCRIPT_ADLAM: case HB_SCRIPT_HANIFI_ROHINGYA:
  case HB_SCRIPT_OLD_SOGDIAN: case HB_SCRIPT_SOGDIAN: case HB_SCRIPT_ELYMAIC: case HB_SCRIPT_CHORASMIAN:
  case HB_SCRIPT_YEZIDI: case HB_SCRIPT_OLD_UYGHUR: case HB_SCRIPT_GARAY: case HB_SCRIPT_SIDETIC:
    return 1;
  }
  return 0;
}

void hb_buffer_guess_segment_properties (hb_buffer_t *b)
{
  if (b->script == HB_SCRIPT_INVALID)
    for (unsigned i = 0; i < b->len; i++)
    {
      hb_script_t s = uni_script (b->info[i].codepoint);
      if (s != HB_SCRIPT_COMMON && s != HB_SCRIPT_INHERITED && s != HB_SCRIPT_UNKNOWN)
      {
	b->script = s;
	break;
      }
    }
  if (!b->direction) b->direction = script_is_rtl (b->script) ? DIR_RTL : DIR_LTR;
}

static const uint8_t *utf8_next (const uint8_t *text, const uint8_t *end, hb_codepoint_t *unicode)
{
  hb_codepoint_t c = *text++;
  if (c > 0x7Fu)
  {
    unsigned t1, t2, t3;
    if (IN_RANGE (c, 0xC2u, 0xDFu))
    {
      if (text < end && (t1 = text[0] - 0x80u) <= 0x3Fu)
      {
	c = ((c & 0x1Fu) << 6) | t1;
	text++;
      }
      else goto error;
    }
    else if (IN_RANGE (c, 0xE0u, 0xEFu))
    {
      if (1 < end - text && (t1 = text[0] - 0x80u) <= 0x3Fu && (t2 = text[1] - 0x80u) <= 0x3Fu)
      {
	c = ((c & 0xFu) << 12) | (t1 << 6) | t2;
	if (c < 0x0800u || IN_RANGE (c, 0xD800u, 0xDFFFu)) goto error;
	text += 2;
      }
      else goto error;
    }
    else if (IN_RANGE (c, 0xF0u, 0xF4u))
    {
      if (2 < end - text && (t1 = text[0] - 0x80u) <= 0x3Fu && (t2 = text[1] - 0x80u) <= 0x3Fu && (t3 = text[2] - 0x80u) <= 0x3Fu)
      {
	c = ((c & 0x7u) << 18) | (t1 << 12) | (t2 << 6) | t3;
	if (!IN_RANGE (c, 0x10000u, 0x10FFFFu)) goto error;
	text += 3;
      }
      else goto error;
    }
    else goto error;
  }
  *unicode = c;
  return text;
error:
  *unicode = 0xFFFDu;
  return text;
}

static const uint8_t *utf8_prev (const uint8_t *text, const uint8_t *start, hb_codepoint_t *unicode)
{
  const uint8_t *end = text--;
  while (start < text && (*text & 0xc0) == 0x80 && end - text < 4) text--;
  if (utf8_next (text, end, unicode) == end) return text;
  *unicode = 0xFFFDu;
  return end - 1;
}

void hb_buffer_add_utf8 (hb_buffer_t *b, const char *text8, int text_length, unsigned int item_offset, int item_length)
{
  const uint8_t *text = (const uint8_t *) text8;
  if (text_length == -1) text_length = (int) strlen (text8);
  if (item_length == -1) item_length = text_length - item_offset;
  item_offset = HB_MIN (item_offset, (unsigned) text_length);
  item_length = HB_CLAMP (item_length, 0, text_length - (int) item_offset);
  if (item_length < 0 || item_length > INT_MAX / 8 || !buf_ensure (b, b->len + item_length / 4)) return;
  if (!b->len && item_offset > 0)
  {
    b->context_len[0] = 0;
    const uint8_t *prev = text + item_offset, *start = text;
    while (start < prev && b->context_len[0] < CONTEXT_LENGTH)
    {
      hb_codepoint_t u;
      prev = utf8_prev (prev, start, &u);
      b->context[0][b->context_len[0]++] = u;
    }
  }
  const uint8_t *next = text + item_offset, *end = next + item_length;
  while (next < end)
  {
    hb_codepoint_t u;
    const uint8_t *old_next = next;
    next = utf8_next (next, end, &u);
    buf_add (b, u, old_next - text);
  }
  b->context_len[1] = 0;
  end = text + text_length;
  while (next < end && b->context_len[1] < CONTEXT_LENGTH)
  {
    hb_codepoint_t u;
    next = utf8_next (next, end, &u);
    b->context[1][b->context_len[1]++] = u;
  }
  b->content_unicode = 1;
}

#define NOT_COVERED 0xFFFFFFFFu
#define NOT_FOUND_INDEX 0xFFFFu
#define NO_FEATURE_INDEX 0xFFFFu
#define NO_VARIATIONS_INDEX 0xFFFFFFFFu
#define MAX_NESTING_LEVEL 64
#define MAX_CONTEXT_LENGTH 64
#define SANITIZE_MAX_OPS_FACTOR 64
#define SANITIZE_MAX_OPS_MIN 16384
#define SANITIZE_MAX_OPS_MAX 0x3FFFFFFF
#define SANITIZE_MAX_SUBTABLES 0x4000

enum
{
  LOOKUP_RIGHT_TO_LEFT = 0x0001,
  LOOKUP_IGNORE_FLAGS = 0x000E,
  LOOKUP_IGNORE_MARKS = 0x0008,
  LOOKUP_USE_MARK_FILTERING_SET = 0x0010,
  LOOKUP_MARK_ATTACHMENT_TYPE = 0xFF00
};

enum { ATTACH_TYPE_MARK = 1, ATTACH_TYPE_CURSIVE = 2 };
#define ATTACH_CHAIN(p) ((p)->var.i16[0])
#define ATTACH_TYPE(p) ((p)->var.u8[2])

static inline int32_t sat_neg (int32_t a) { return a == INT32_MIN ? INT32_MAX : -a; }

typedef struct
{
  const uint8_t *start;
  unsigned length;
  int max_ops;
  unsigned max_subtables;
  unsigned depth;
  int lazy;
} sc_t;

static void sc_init (sc_t *c, const uint8_t *d, unsigned len)
{
  c->start = d;
  c->length = d ? len : 0;
  uint64_t m = (uint64_t) c->length * SANITIZE_MAX_OPS_FACTOR;
  c->max_ops = m > UINT32_MAX ? SANITIZE_MAX_OPS_MAX : (int) HB_CLAMP ((unsigned) m, (unsigned) SANITIZE_MAX_OPS_MIN, (unsigned) SANITIZE_MAX_OPS_MAX);
  c->max_subtables = 0;
  c->depth = 0;
  c->lazy = 1;
}

static inline int sc_point (const sc_t *c, const uint8_t *p) { return (uintptr_t) p - (uintptr_t) c->start <= c->length; }
static inline int sc_struct (const sc_t *c, const uint8_t *p, unsigned min_size) { return sc_point (c, p + min_size); }
static inline int sc_range (sc_t *c, const uint8_t *p, unsigned len)
{
  return (uintptr_t) p - (uintptr_t) c->start <= c->length &&
	 (unsigned) ((uintptr_t) c->start + c->length - (uintptr_t) p) >= len &&
	 (c->max_ops -= len) > 0;
}
static inline int sc_range2 (sc_t *c, const uint8_t *p, unsigned a, unsigned b)
{
  return !(b && a >= 0x100000000ull / b) && sc_range (c, p, a * b);
}
static int sc_ops (sc_t *c, unsigned count)
{
  if (c->max_ops < 0 || count >= (unsigned) c->max_ops) { c->max_ops = -1; return 0; }
  c->max_ops -= (int) count;
  return 1;
}
static inline int sc_array16 (sc_t *c, const uint8_t *p, unsigned size)
{
  return sc_struct (c, p, 2) && sc_range (c, p + 2, rd16 (p) * size);
}

typedef int (*sc_func_t) (sc_t *c, const uint8_t *p, unsigned arg);

static int sc_off16 (sc_t *c, const uint8_t *p, const uint8_t *base, sc_func_t f, unsigned arg)
{
  if (!sc_struct (c, p, 2)) return 0;
  unsigned o = rd16 (p);
  return !o || f (c, base + o, arg);
}
static int sc_off24 (sc_t *c, const uint8_t *p, const uint8_t *base, sc_func_t f, unsigned arg)
{
  if (!sc_struct (c, p, 3)) return 0;
  unsigned o = rd24 (p);
  return !o || f (c, base + o, arg);
}
static int sc_off32 (sc_t *c, const uint8_t *p, const uint8_t *base, sc_func_t f, unsigned arg)
{
  if (!sc_struct (c, p, 4)) return 0;
  uint32_t o = rd32 (p);
  return !o || f (c, base + o, arg);
}
static int sc_off16_array (sc_t *c, const uint8_t *p, const uint8_t *base, sc_func_t f, unsigned arg)
{
  if (!sc_array16 (c, p, 2)) return 0;
  unsigned n = rd16 (p);
  for (unsigned i = 0; i < n; i++)
    if (!sc_off16 (c, p + 2 + i * 2, base, f, arg)) return 0;
  return 1;
}

static int sc_coverage (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!sc_struct (c, p, 2)) return 0;
  switch (rd16 (p))
  {
  case 1: return sc_array16 (c, p + 2, 2);
  case 2: return sc_array16 (c, p + 2, 6);
  default: return 1;
  }
}

static int sc_classdef (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!sc_struct (c, p, 2)) return 0;
  switch (rd16 (p))
  {
  case 1: return sc_struct (c, p, 6) && sc_array16 (c, p + 4, 2);
  case 2: return sc_array16 (c, p + 2, 6);
  default: return 1;
  }
}

static int sc_device (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!sc_struct (c, p + 4, 2)) return 0;
  if (rd16 (p + 4) == 0x8000) return sc_struct (c, p, 6);
  return 1;
}

static int sc_u16_array (sc_t *c, const uint8_t *p, unsigned arg) { return sc_array16 (c, p, 2); }

static int sc_var_region_list (sc_t *c, const uint8_t *p, unsigned arg)
{
  return sc_struct (c, p, 4) && sc_range2 (c, p + 4, rd16 (p) * (rd16 (p + 2) & 0x7FFF), 6);
}

static int sc_var_data (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!(sc_struct (c, p, 6) && sc_array16 (c, p + 4, 2))) return 0;
  unsigned items = rd16 (p), wsc = rd16 (p + 2), rc = rd16 (p + 4);
  if ((wsc & 0x7FFF) > rc) return 0;
  unsigned row = ((wsc & 0x7FFF) + rc) * (wsc & 0x8000 ? 2 : 1);
  return sc_range2 (c, p + 6 + rc * 2, items, row);
}

static int sc_ivs (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!(sc_struct (c, p, 8) && rd16 (p) == 1 && sc_off32 (c, p + 2, p, sc_var_region_list, 0))) return 0;
  if (!sc_array16 (c, p + 6, 4)) return 0;
  unsigned n = rd16 (p + 6);
  for (unsigned i = 0; i < n; i++)
    if (!sc_off32 (c, p + 8 + i * 4, p, sc_var_data, 0)) return 0;
  return 1;
}

static int sc_caret_value (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!sc_struct (c, p, 2)) return 0;
  switch (rd16 (p))
  {
  case 1: case 2: return sc_struct (c, p, 4);
  case 3: return sc_struct (c, p, 6) && sc_off16 (c, p + 4, p, sc_device, 0);
  default: return 1;
  }
}
static int sc_lig_glyph (sc_t *c, const uint8_t *p, unsigned arg) { return sc_off16_array (c, p, p, sc_caret_value, 0); }
static int sc_attach_list (sc_t *c, const uint8_t *p, unsigned arg)
{
  return sc_off16 (c, p, p, sc_coverage, 0) && sc_off16_array (c, p + 2, p, sc_u16_array, 0);
}
static int sc_lig_caret_list (sc_t *c, const uint8_t *p, unsigned arg)
{
  return sc_off16 (c, p, p, sc_coverage, 0) && sc_off16_array (c, p + 2, p, sc_lig_glyph, 0);
}
static int sc_mark_glyph_sets (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!sc_struct (c, p, 2)) return 0;
  if (rd16 (p) != 1) return 1;
  if (!sc_array16 (c, p + 2, 4)) return 0;
  unsigned n = rd16 (p + 2);
  for (unsigned i = 0; i < n; i++)
    if (!sc_off32 (c, p + 4 + i * 4, p, sc_coverage, 0)) return 0;
  return 1;
}

static int sc_gdef (sc_t *c, const uint8_t *p)
{
  if (!sc_struct (c, p, 4)) return 0;
  if (rd16 (p) != 1) return 1;
  uint32_t v = rd32 (p);
  return sc_off16 (c, p + 4, p, sc_classdef, 0) &&
	 sc_off16 (c, p + 6, p, sc_attach_list, 0) &&
	 sc_off16 (c, p + 8, p, sc_lig_caret_list, 0) &&
	 sc_off16 (c, p + 10, p, sc_classdef, 0) &&
	 (v < 0x00010002u || sc_off16 (c, p + 12, p, sc_mark_glyph_sets, 0)) &&
	 (v < 0x00010003u || sc_off32 (c, p + 14, p, sc_ivs, 0));
}

static int sc_langsys (sc_t *c, const uint8_t *p, unsigned arg) { return sc_struct (c, p, 6) && sc_array16 (c, p + 4, 2); }
static int sc_record_array (sc_t *c, const uint8_t *p, const uint8_t *base, sc_func_t f)
{
  if (!sc_array16 (c, p, 6)) return 0;
  unsigned n = rd16 (p);
  for (unsigned i = 0; i < n; i++)
  {
    const uint8_t *r = p + 2 + i * 6;
    if (!(sc_struct (c, r, 6) && sc_off16 (c, r + 4, base, f, 0))) return 0;
  }
  return 1;
}
static int sc_script (sc_t *c, const uint8_t *p, unsigned arg)
{
  return sc_off16 (c, p, p, sc_langsys, 0) && sc_record_array (c, p + 2, p, sc_langsys);
}
static int sc_script_list (sc_t *c, const uint8_t *p, unsigned arg) { return sc_record_array (c, p, p, sc_script); }
static int sc_feature_params (sc_t *c, const uint8_t *p, unsigned arg) { return 1; }
static int sc_feature (sc_t *c, const uint8_t *p, unsigned arg)
{
  return sc_struct (c, p, 4) && sc_off16 (c, p, p, sc_feature_params, 0) && sc_array16 (c, p + 2, 2);
}
static int sc_feature_list (sc_t *c, const uint8_t *p, unsigned arg) { return sc_record_array (c, p, p, sc_feature); }

static int sc_condition (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (c->depth >= MAX_NESTING_LEVEL) return 0;
  c->depth++;
  int ret = 1;
  if (!sc_struct (c, p, 2)) ret = 0;
  else switch (rd16 (p))
  {
  case 1: case 2: ret = sc_struct (c, p, 8); break;
  case 3: case 4:
  {
    if (!(sc_struct (c, p + 2, 1) && sc_range (c, p + 3, p[2] * 3))) { ret = 0; break; }
    for (unsigned i = 0; i < p[2] && ret; i++) ret = sc_off24 (c, p + 3 + i * 3, p, sc_condition, 0);
    break;
  }
  case 5: ret = sc_off24 (c, p + 2, p, sc_condition, 0); break;
  }
  c->depth--;
  return ret;
}
static int sc_condition_set (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!sc_array16 (c, p, 4)) return 0;
  unsigned n = rd16 (p);
  for (unsigned i = 0; i < n; i++)
    if (!sc_off32 (c, p + 2 + i * 4, p, sc_condition, 0)) return 0;
  return 1;
}
static int sc_feature_table_subst (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!(sc_struct (c, p, 4) && rd16 (p) == 1 && sc_array16 (c, p + 4, 6))) return 0;
  unsigned n = rd16 (p + 4);
  for (unsigned i = 0; i < n; i++)
  {
    const uint8_t *r = p + 6 + i * 6;
    if (!(sc_struct (c, r, 6) && sc_off32 (c, r + 2, p, sc_feature, 0))) return 0;
  }
  return 1;
}
static int sc_feature_variations (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!(sc_struct (c, p, 4) && rd16 (p) == 1 && sc_struct (c, p + 4, 4))) return 0;
  uint32_t n = rd32 (p + 4);
  if (n >= 0x100000000ull / 8 || !sc_range (c, p + 8, n * 8)) return 0;
  for (uint32_t i = 0; i < n; i++)
  {
    const uint8_t *r = p + 8 + i * 8;
    if (!(sc_off32 (c, r, p, sc_condition_set, 0) && sc_off32 (c, r + 4, p, sc_feature_table_subst, 0))) return 0;
  }
  return 1;
}

static int sc_rule (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!sc_struct (c, p, 4)) return 0;
  unsigned ic = rd16 (p), lc = rd16 (p + 2);
  return sc_range (c, p + 4, 2 * (ic ? ic - 1 : 0) + 4 * lc);
}
static int sc_rule_set (sc_t *c, const uint8_t *p, unsigned arg) { return sc_off16_array (c, p, p, sc_rule, 0); }
static int sc_chain_rule (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!sc_struct (c, p, 2)) return 0;
  const uint8_t *in = p + 2 + rd16 (p) * 2;
  if (!sc_struct (c, in, 2)) return 0;
  unsigned ilen = rd16 (in);
  const uint8_t *la = in + 2 + (ilen ? ilen - 1 : 0) * 2;
  if (!sc_struct (c, la, 2)) return 0;
  return sc_array16 (c, la + 2 + rd16 (la) * 2, 4);
}
static int sc_chain_rule_set (sc_t *c, const uint8_t *p, unsigned arg) { return sc_off16_array (c, p, p, sc_chain_rule, 0); }

static int sc_context (sc_t *c, const uint8_t *p)
{
  switch (rd16 (p))
  {
  case 1: return sc_off16 (c, p + 2, p, sc_coverage, 0) && sc_off16_array (c, p + 4, p, sc_rule_set, 0);
  case 2: return sc_off16 (c, p + 2, p, sc_coverage, 0) && sc_off16 (c, p + 4, p, sc_classdef, 0) && sc_off16_array (c, p + 6, p, sc_rule_set, 0);
  case 3:
  {
    if (!sc_struct (c, p, 6)) return 0;
    unsigned n = rd16 (p + 2);
    if (!n || !sc_range2 (c, p + 6, n, 2)) return 0;
    for (unsigned i = 0; i < n; i++)
      if (!sc_off16 (c, p + 6 + i * 2, p, sc_coverage, 0)) return 0;
    return sc_range2 (c, p + 6 + n * 2, rd16 (p + 4), 4);
  }
  default: return 1;
  }
}

static int sc_chain_context (sc_t *c, const uint8_t *p)
{
  switch (rd16 (p))
  {
  case 1: return sc_off16 (c, p + 2, p, sc_coverage, 0) && sc_off16_array (c, p + 4, p, sc_chain_rule_set, 0);
  case 2:
    return sc_off16 (c, p + 2, p, sc_coverage, 0) && sc_off16 (c, p + 4, p, sc_classdef, 0) &&
	   sc_off16 (c, p + 6, p, sc_classdef, 0) && sc_off16 (c, p + 8, p, sc_classdef, 0) &&
	   sc_off16_array (c, p + 10, p, sc_chain_rule_set, 0);
  case 3:
  {
    const uint8_t *bt = p + 2;
    if (!sc_off16_array (c, bt, p, sc_coverage, 0)) return 0;
    const uint8_t *in = bt + 2 + rd16 (bt) * 2;
    if (!sc_off16_array (c, in, p, sc_coverage, 0)) return 0;
    if (!rd16 (in)) return 0;
    const uint8_t *la = in + 2 + rd16 (in) * 2;
    if (!sc_off16_array (c, la, p, sc_coverage, 0)) return 0;
    return sc_array16 (c, la + 2 + rd16 (la) * 2, 4);
  }
  default: return 1;
  }
}

static unsigned cov_population (const uint8_t *p);

static int sc_gsub_subtable (sc_t *c, const uint8_t *p, unsigned type);
static int sc_gpos_subtable (sc_t *c, const uint8_t *p, unsigned type);

static int sc_extension (sc_t *c, const uint8_t *p, unsigned table_index)
{
  if (!sc_struct (c, p, 2)) return 0;
  if (rd16 (p) != 1) return 1;
  unsigned t = rd16 (p + 2);
  if (!(sc_struct (c, p, 8) && t != (table_index ? 9u : 7u))) return 0;
  uint32_t o = rd32 (p + 4);
  if (!o) return table_index ? !(t >= 1 && t <= 9) : !(t >= 1 && t <= 8);
  return table_index ? sc_gpos_subtable (c, p + o, t) : sc_gsub_subtable (c, p + o, t);
}

static int sc_ligature (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!(sc_struct (c, p, 2) && sc_struct (c, p + 2, 2))) return 0;
  unsigned n = rd16 (p + 2);
  return !n || sc_range (c, p + 4, (n - 1) * 2);
}
static int sc_ligature_set (sc_t *c, const uint8_t *p, unsigned arg) { return sc_off16_array (c, p, p, sc_ligature, 0); }

static int sc_gsub_subtable (sc_t *c, const uint8_t *p, unsigned type)
{
  switch (type)
  {
  case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
    if (!sc_struct (c, p, 2)) return 0;
    break;
  default: return 1;
  }
  unsigned fmt = rd16 (p);
  switch (type)
  {
  case 1:
    if (fmt == 1) return sc_struct (c, p, 6) && sc_off16 (c, p + 2, p, sc_coverage, 0) && sc_ops (c, cov_population (rd16 (p + 2) ? p + rd16 (p + 2) : NULL) >> 1);
    if (fmt == 2) return sc_off16 (c, p + 2, p, sc_coverage, 0) && sc_array16 (c, p + 4, 2);
    return 1;
  case 2: case 3:
    if (fmt == 1) return sc_off16 (c, p + 2, p, sc_coverage, 0) && sc_off16_array (c, p + 4, p, sc_u16_array, 0);
    return 1;
  case 4:
    if (fmt == 1) return sc_off16 (c, p + 2, p, sc_coverage, 0) && sc_off16_array (c, p + 4, p, sc_ligature_set, 0);
    return 1;
  case 5: return sc_context (c, p);
  case 6: return sc_chain_context (c, p);
  case 7: return sc_extension (c, p, 0);
  case 8:
  {
    if (fmt != 1) return 1;
    const uint8_t *bt = p + 4;
    if (!(sc_off16 (c, p + 2, p, sc_coverage, 0) && sc_off16_array (c, bt, p, sc_coverage, 0))) return 0;
    const uint8_t *la = bt + 2 + rd16 (bt) * 2;
    if (!sc_off16_array (c, la, p, sc_coverage, 0)) return 0;
    return sc_array16 (c, la + 2 + rd16 (la) * 2, 2);
  }
  }
  return 1;
}

static inline unsigned vf_len (unsigned f) { return __builtin_popcount (f & 0xFF); }

static int sc_anchor (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!sc_struct (c, p, 2)) return 0;
  switch (rd16 (p))
  {
  case 1: return sc_struct (c, p, 6);
  case 2: return sc_struct (c, p, 8);
  case 3: return sc_struct (c, p, 10) && sc_off16 (c, p + 6, p, sc_device, 0) && sc_off16 (c, p + 8, p, sc_device, 0);
  default: return 1;
  }
}
static int sc_anchor_matrix (sc_t *c, const uint8_t *p, unsigned cols)
{
  if (!sc_struct (c, p, 2)) return 0;
  unsigned rows = rd16 (p);
  if (cols && rows >= 0x100000000ull / cols) return 0;
  return sc_range2 (c, p + 2, rows * cols, 2);
}
static int sc_mark_array (sc_t *c, const uint8_t *p, unsigned arg)
{
  if (!sc_array16 (c, p, 4)) return 0;
  unsigned n = rd16 (p);
  for (unsigned i = 0; i < n; i++)
  {
    const uint8_t *r = p + 2 + i * 4;
    if (!(sc_struct (c, r, 4) && sc_off16 (c, r + 2, p, sc_anchor, 0))) return 0;
  }
  return 1;
}
static int sc_lig_array (sc_t *c, const uint8_t *p, unsigned cols) { return sc_off16_array (c, p, p, sc_anchor_matrix, cols); }
static int sc_pair_set (sc_t *c, const uint8_t *p, unsigned stride)
{
  return sc_struct (c, p, 2) && sc_range2 (c, p + 2, rd16 (p), stride);
}

static int sc_gpos_subtable (sc_t *c, const uint8_t *p, unsigned type)
{
  if (type < 1 || type > 9) return 1;
  if (!sc_struct (c, p, 2)) return 0;
  unsigned fmt = rd16 (p);
  switch (type)
  {
  case 1:
    if (fmt == 1)
      return sc_struct (c, p, 6) && sc_off16 (c, p + 2, p, sc_coverage, 0) &&
	     sc_ops (c, cov_population (rd16 (p + 2) ? p + rd16 (p + 2) : NULL) >> 1) &&
	     sc_range (c, p + 6, vf_len (rd16 (p + 4)) * 2);
    if (fmt == 2)
      return sc_struct (c, p, 8) && sc_off16 (c, p + 2, p, sc_coverage, 0) &&
	     sc_range2 (c, p + 8, rd16 (p + 6), vf_len (rd16 (p + 4)) * 2);
    return 1;
  case 2:
    if (fmt == 1)
    {
      if (!sc_struct (c, p, 10)) return 0;
      unsigned stride = 2 + 2 * (vf_len (rd16 (p + 4)) + vf_len (rd16 (p + 6)));
      return sc_off16 (c, p + 2, p, sc_coverage, 0) && sc_off16_array (c, p + 8, p, sc_pair_set, stride);
    }
    if (fmt == 2)
    {
      if (!(sc_struct (c, p, 16) && sc_off16 (c, p + 2, p, sc_coverage, 0) &&
	    sc_off16 (c, p + 8, p, sc_classdef, 0) && sc_off16 (c, p + 10, p, sc_classdef, 0))) return 0;
      unsigned stride = 2 * (vf_len (rd16 (p + 4)) + vf_len (rd16 (p + 6)));
      return sc_range2 (c, p + 16, rd16 (p + 12) * rd16 (p + 14), stride);
    }
    return 1;
  case 3:
    if (fmt == 1) return sc_off16 (c, p + 2, p, sc_coverage, 0) && sc_array16 (c, p + 4, 4);
    return 1;
  case 4: case 6:
    if (fmt == 1)
      return sc_struct (c, p, 12) && sc_off16 (c, p + 2, p, sc_coverage, 0) && sc_off16 (c, p + 4, p, sc_coverage, 0) &&
	     sc_off16 (c, p + 8, p, sc_mark_array, 0) && sc_off16 (c, p + 10, p, sc_anchor_matrix, rd16 (p + 6));
    return 1;
  case 5:
    if (fmt == 1)
      return sc_struct (c, p, 12) && sc_off16 (c, p + 2, p, sc_coverage, 0) && sc_off16 (c, p + 4, p, sc_coverage, 0) &&
	     sc_off16 (c, p + 8, p, sc_mark_array, 0) && sc_off16 (c, p + 10, p, sc_lig_array, rd16 (p + 6));
    return 1;
  case 7: return sc_context (c, p);
  case 8: return sc_chain_context (c, p);
  case 9: return sc_extension (c, p, 1);
  }
  return 1;
}

static unsigned ext_type (const uint8_t *st)
{
  return st && rd16 (st) == 1 ? rd16 (st + 2) : 0;
}

static int sc_lookup (sc_t *c, const uint8_t *p, unsigned table_index)
{
  if (!(sc_struct (c, p, 6) && sc_array16 (c, p + 4, 2))) return 0;
  unsigned n = rd16 (p + 4), type = rd16 (p);
  c->max_subtables += n;
  if (c->max_subtables >= SANITIZE_MAX_SUBTABLES) return 0;
  if ((rd16 (p + 2) & LOOKUP_USE_MARK_FILTERING_SET) && !sc_struct (c, p + 6 + n * 2, 2)) return 0;
  if (!sc_off16_array (c, p + 4, p, table_index ? sc_gpos_subtable : sc_gsub_subtable, type)) return 0;
  if (type == (table_index ? 9u : 7u))
  {
    unsigned o0 = rd16 (p + 6), t = ext_type (o0 ? p + o0 : NULL);
    for (unsigned i = 1; i < n; i++)
    {
      unsigned o = rd16 (p + 6 + i * 2);
      if (ext_type (o ? p + o : NULL) != t) return 0;
    }
  }
  return 1;
}

static int sc_lookup_list (sc_t *c, const uint8_t *p, unsigned table_index) { return sc_off16_array (c, p, p, sc_lookup, table_index); }

static int sc_gsubgpos (sc_t *c, const uint8_t *p, unsigned table_index)
{
  if (!sc_struct (c, p, 4)) return 0;
  if (rd16 (p) != 1) return 1;
  return sc_off16 (c, p + 4, p, sc_script_list, 0) &&
	 sc_off16 (c, p + 6, p, sc_feature_list, 0) &&
	 sc_off16 (c, p + 8, p, sc_lookup_list, table_index) &&
	 (rd32 (p) < 0x00010001u || sc_off32 (c, p + 10, p, sc_feature_variations, 0));
}

static inline const uint8_t *off16 (const uint8_t *base, const uint8_t *field)
{
  unsigned o = rd16 (field);
  return o ? base + o : NULL;
}
static inline const uint8_t *off32 (const uint8_t *base, const uint8_t *field)
{
  uint32_t o = rd32 (field);
  return o ? base + o : NULL;
}
static inline const uint8_t *arr_off16 (const uint8_t *base, const uint8_t *arr, unsigned i)
{
  return arr && i < rd16 (arr) ? off16 (base, arr + 2 + i * 2) : NULL;
}

static int u16_cmp (const void *key, const uint8_t *item, const void *ud)
{
  unsigned g = *(const unsigned *) key, v = rd16 (item);
  return (g > v) - (g < v);
}
static int range_cmp (const void *key, const uint8_t *item, const void *ud)
{
  unsigned g = *(const unsigned *) key;
  return g < rd16 (item) ? -1 : g <= rd16 (item + 2) ? 0 : 1;
}

static unsigned cov_get (const uint8_t *p, hb_codepoint_t g)
{
  if (!p) return NOT_COVERED;
  switch (rd16 (p))
  {
  case 1:
  {
    int i = hb_bsearch (&g, p + 4, rd16 (p + 2), 2, u16_cmp, NULL);
    return i < 0 ? NOT_COVERED : (unsigned) i;
  }
  case 2:
  {
    int i = hb_bsearch (&g, p + 4, rd16 (p + 2), 6, range_cmp, NULL);
    if (i < 0) return NOT_COVERED;
    const uint8_t *r = p + 4 + i * 6;
    return rd16 (r + 4) + (g - rd16 (r));
  }
  default: return NOT_COVERED;
  }
}

static unsigned cov_population (const uint8_t *p)
{
  if (!p) return NOT_COVERED;
  switch (rd16 (p))
  {
  case 1: return rd16 (p + 2);
  case 2:
  {
    uint32_t ret = 0;
    unsigned n = rd16 (p + 2);
    for (unsigned i = 0; i < n; i++)
    {
      unsigned first = rd16 (p + 4 + i * 6), last = rd16 (p + 6 + i * 6);
      if (last >= first) ret += last - first + 1;
    }
    return ret;
  }
  default: return NOT_COVERED;
  }
}

static void cov_collect_digest (const uint8_t *p, digest_t *d)
{
  if (!p) return;
  unsigned n = rd16 (p + 2);
  switch (rd16 (p))
  {
  case 1:
    for (unsigned i = 0; i < n; i++) digest_add (d, rd16 (p + 4 + i * 2));
    break;
  case 2:
    for (unsigned i = 0; i < n; i++)
      if (!digest_add_range (d, rd16 (p + 4 + i * 6), rd16 (p + 6 + i * 6))) return;
    break;
  }
}

static unsigned classdef_get (const uint8_t *p, hb_codepoint_t g)
{
  if (!p) return 0;
  switch (rd16 (p))
  {
  case 1:
  {
    unsigned i = g - rd16 (p + 2);
    return i < rd16 (p + 4) ? rd16 (p + 6 + i * 2) : 0;
  }
  case 2:
  {
    int i = hb_bsearch (&g, p + 4, rd16 (p + 2), 6, range_cmp, NULL);
    return i < 0 ? 0 : rd16 (p + 4 + i * 6 + 4);
  }
  default: return 0;
  }
}

static void classdef_collect_class (const uint8_t *p, unsigned klass, digest_t *d)
{
  if (!p) return;
  switch (rd16 (p))
  {
  case 1:
  {
    unsigned start = rd16 (p + 2), n = rd16 (p + 4);
    for (unsigned i = 0; i < n; i++)
      if (rd16 (p + 6 + i * 2) == klass) digest_add (d, start + i);
    break;
  }
  case 2:
  {
    unsigned n = rd16 (p + 2);
    for (unsigned i = 0; i < n; i++)
    {
      const uint8_t *r = p + 4 + i * 6;
      if (rd16 (r + 4) == klass && !digest_add_range (d, rd16 (r), rd16 (r + 2))) return;
    }
    break;
  }
  }
}

typedef struct { const uint8_t *st; unsigned type; digest_t digest; } lsub_t;

typedef struct
{
  unsigned count;
  digest_t digest, digest_second;
  lsub_t sub[];
} laccel_t;

typedef struct
{
  const uint8_t *table;
  unsigned lookup_count;
  const uint8_t *lookup_list;
  laccel_t **accels;
} gtab_t;

typedef struct
{
  unsigned bias;
  uint64_t page[8];
  digest_t digest;
  const uint8_t *cov;
} mark_set_t;

typedef struct
{
  const uint8_t *gdef;
  const uint8_t *glyph_class_def, *mark_attach_class_def;
  tab_t ivs;
  unsigned mark_set_count;
  mark_set_t *mark_sets;
  gtab_t g[2];
} layout_t;

static const uint8_t *sanitized_table (tab_t t, int (*f) (sc_t *, const uint8_t *, unsigned), unsigned arg, unsigned *len)
{
  if (!t.d) return NULL;
  sc_t c;
  sc_init (&c, t.d, t.len);
  c.lazy = 1;
  if (!f (&c, t.d, arg)) return NULL;
  *len = t.len;
  return t.d;
}

static int sc_gdef_f (sc_t *c, const uint8_t *p, unsigned arg) { return sc_gdef (c, p); }

static void mark_set_init (mark_set_t *s, const uint8_t *cov)
{
  s->cov = cov;
  s->bias = UINT32_MAX;
  memset (s->page, 0, sizeof s->page);
  memset (&s->digest, 0, sizeof s->digest);
  if (!cov) return;
  uint64_t *bits = (uint64_t *) calloc (1024, sizeof (uint64_t));
  if (!bits) return;
  unsigned n = rd16 (cov + 2);
  switch (rd16 (cov))
  {
  case 1:
  {
    unsigned last = 0;
    for (unsigned i = 0; i < n; i++)
    {
      unsigned g = rd16 (cov + 4 + i * 2);
      if (i && g < last) break;
      last = g;
      bits[g >> 6] |= (uint64_t) 1 << (g & 63);
    }
    break;
  }
  case 2:
    for (unsigned i = 0; i < n; i++)
    {
      unsigned a = rd16 (cov + 4 + i * 6), b = rd16 (cov + 6 + i * 6);
      if (a > b) break;
      for (unsigned g = a; g <= b; g++) bits[g >> 6] |= (uint64_t) 1 << (g & 63);
    }
    break;
  }
  unsigned min = UINT32_MAX, max = UINT32_MAX;
  for (unsigned g = 0; g < 65536; g++)
    if (bits[g >> 6] >> (g & 63) & 1)
    {
      if (min == UINT32_MAX) min = g;
      max = g;
    }
  if (max != UINT32_MAX)
  {
    if (max - min >= 512)
    {
      for (unsigned g = min; g <= max; g++)
	if (bits[g >> 6] >> (g & 63) & 1) digest_add (&s->digest, g);
    }
    else
    {
      s->bias = min;
      for (unsigned g = min; g <= max; g++)
	if (bits[g >> 6] >> (g & 63) & 1) s->page[(g - min) >> 6] |= (uint64_t) 1 << ((g - min) & 63);
    }
  }
  free (bits);
}

static int mark_set_covers (const layout_t *l, unsigned i, hb_codepoint_t g)
{
  const mark_set_t *s = &l->mark_sets[i];
  if (s->bias != UINT32_MAX)
  {
    unsigned b = g - s->bias;
    return b < 512 && (s->page[b >> 6] >> (b & 63) & 1);
  }
  return digest_may_have (&s->digest, g) && cov_get (s->cov, g) != NOT_COVERED;
}

static int sc_gsubgpos_f (sc_t *c, const uint8_t *p, unsigned ti) { return sc_gsubgpos (c, p, ti); }

static layout_t *face_layout (hb_face_t *face)
{
  if (face->layout_accel) return (layout_t *) face->layout_accel;
  layout_t *l = (layout_t *) calloc (1, sizeof (layout_t));
  if (!l) return NULL;
  unsigned len;
  const uint8_t *gdef = sanitized_table (face->gdef, sc_gdef_f, 0, &len);
  if (gdef && rd16 (gdef) == 1)
  {
    uint32_t v = rd32 (gdef);
    l->gdef = gdef;
    l->glyph_class_def = off16 (gdef, gdef + 4);
    l->mark_attach_class_def = off16 (gdef, gdef + 10);
    const uint8_t *mgs = v >= 0x00010002u ? off16 (gdef, gdef + 12) : NULL;
    if (mgs && rd16 (mgs) == 1)
    {
      unsigned n = rd16 (mgs + 2);
      l->mark_sets = (mark_set_t *) calloc (n ? n : 1, sizeof (mark_set_t));
      if (l->mark_sets)
      {
	l->mark_set_count = n;
	for (unsigned i = 0; i < n; i++) mark_set_init (&l->mark_sets[i], off32 (mgs, mgs + 4 + i * 4));
      }
    }
    const uint8_t *ivs = v >= 0x00010003u ? off32 (gdef, gdef + 14) : NULL;
    if (ivs)
    {
      l->ivs.d = ivs;
      l->ivs.len = face->gdef.len - (unsigned) (ivs - face->gdef.d);
    }
  }
  tab_t tabs[2] = {face->gsub, face->gpos};
  for (unsigned ti = 0; ti < 2; ti++)
  {
    gtab_t *g = &l->g[ti];
    g->table = sanitized_table (tabs[ti], sc_gsubgpos_f, ti, &len);
    if (g->table && rd16 (g->table) == 1)
    {
      g->lookup_list = off16 (g->table, g->table + 8);
      g->lookup_count = g->lookup_list ? rd16 (g->lookup_list) : 0;
    }
    if (g->lookup_count)
    {
      g->accels = (laccel_t **) calloc (g->lookup_count, sizeof (laccel_t *));
      if (!g->accels) g->lookup_count = 0, g->table = NULL, g->lookup_list = NULL;
    }
  }
  face->layout_accel = l;
  return l;
}

static void layout_free (hb_face_t *face)
{
  layout_t *l = (layout_t *) face->layout_accel;
  if (!l) return;
  for (unsigned ti = 0; ti < 2; ti++)
  {
    for (unsigned i = 0; i < l->g[ti].lookup_count; i++) free (l->g[ti].accels[i]);
    free (l->g[ti].accels);
  }
  free (l->mark_sets);
  free (l);
  face->layout_accel = NULL;
}

static unsigned gdef_glyph_props (const layout_t *l, hb_codepoint_t g)
{
  switch (classdef_get (l->glyph_class_def, g))
  {
  default: return 0;
  case 1: return GLYPH_PROPS_BASE_GLYPH;
  case 2: return GLYPH_PROPS_LIGATURE;
  case 3: return GLYPH_PROPS_MARK | (classdef_get (l->mark_attach_class_def, g) << 8);
  }
}

static const uint8_t *gtab_lookup (const gtab_t *g, unsigned i) { return arr_off16 (g->lookup_list, g->lookup_list, i); }

static unsigned lookup_props (const uint8_t *L)
{
  if (!L) return 0;
  unsigned flag = rd16 (L + 2);
  if (flag & LOOKUP_USE_MARK_FILTERING_SET) flag += rd16 (L + 6 + rd16 (L + 4) * 2) << 16;
  return flag;
}

static void collect_second (const uint8_t *st, unsigned table_index, unsigned type, digest_t *d)
{
  static const digest_t full = {{(uint64_t) -1, (uint64_t) -1, (uint64_t) -1}};
  unsigned fmt = rd16 (st);
  int ctx = table_index ? 7 : 5;
  if (type == (unsigned) ctx)
  {
    if (fmt == 3)
    {
      if (rd16 (st + 2) <= 1) *d = full;
      else cov_collect_digest (off16 (st, st + 8), d);
      return;
    }
    const uint8_t *cd = fmt == 2 ? off16 (st, st + 4) : NULL;
    const uint8_t *sets = st + (fmt == 2 ? 6 : 4);
    for (unsigned i = 0; i < rd16 (sets); i++)
    {
      const uint8_t *rs = off16 (st, sets + 2 + i * 2);
      if (!rs) continue;
      for (unsigned j = 0; j < rd16 (rs); j++)
      {
	const uint8_t *r = off16 (rs, rs + 2 + j * 2);
	unsigned ic = r ? rd16 (r) : 0;
	if (ic <= 1) { *d = full; continue; }
	unsigned second = rd16 (r + 4);
	if (fmt != 2) digest_add (d, second);
	else if (!second) *d = full;
	else classdef_collect_class (cd, second, d);
      }
    }
    return;
  }
  if (type == (unsigned) ctx + 1)
  {
    if (fmt == 3)
    {
      const uint8_t *bt = st + 2, *in = bt + 2 + rd16 (bt) * 2, *la = in + 2 + rd16 (in) * 2;
      if (rd16 (in) > 1) cov_collect_digest (off16 (st, in + 4), d);
      else if (rd16 (la)) cov_collect_digest (off16 (st, la + 2), d);
      else *d = full;
      return;
    }
    const uint8_t *icd = fmt == 2 ? off16 (st, st + 6) : NULL, *lcd = fmt == 2 ? off16 (st, st + 8) : NULL;
    const uint8_t *sets = st + (fmt == 2 ? 10 : 4);
    for (unsigned i = 0; i < rd16 (sets); i++)
    {
      const uint8_t *rs = off16 (st, sets + 2 + i * 2);
      if (!rs) continue;
      for (unsigned j = 0; j < rd16 (rs); j++)
      {
	const uint8_t *r = off16 (rs, rs + 2 + j * 2);
	unsigned second;
	const uint8_t *cd;
	if (!r) { *d = full; continue; }
	const uint8_t *in = r + 2 + rd16 (r) * 2;
	unsigned ilen = rd16 (in);
	const uint8_t *la = in + 2 + (ilen ? ilen - 1 : 0) * 2;
	if (ilen > 1) { second = rd16 (in + 2); cd = icd; }
	else if (rd16 (la)) { second = rd16 (la + 2); cd = lcd; }
	else { *d = full; continue; }
	if (fmt != 2) digest_add (d, second);
	else if (!second) *d = full;
	else classdef_collect_class (cd, second, d);
      }
    }
    return;
  }
  if (!table_index)
  {
    if (type == 4)
    {
      for (unsigned i = 0; i < rd16 (st + 4); i++)
      {
	const uint8_t *ls = off16 (st, st + 6 + i * 2);
	if (!ls) continue;
	for (unsigned j = 0; j < rd16 (ls); j++)
	{
	  const uint8_t *lig = off16 (ls, ls + 2 + j * 2);
	  unsigned n = lig ? rd16 (lig + 2) : 0;
	  if (n <= 1) *d = full;
	  else digest_add (d, rd16 (lig + 4));
	}
      }
      return;
    }
    *d = full;
    return;
  }
  switch (type)
  {
  case 2:
    if (fmt == 1)
    {
      unsigned stride = 2 + 2 * (vf_len (rd16 (st + 4)) + vf_len (rd16 (st + 6)));
      for (unsigned i = 0; i < rd16 (st + 8); i++)
      {
	const uint8_t *ps = off16 (st, st + 10 + i * 2);
	if (!ps) continue;
	for (unsigned j = 0; j < rd16 (ps); j++) digest_add (d, rd16 (ps + 2 + j * stride));
      }
      return;
    }
    *d = full;
    return;
  case 3: cov_collect_digest (off16 (st, st + 2), d); return;
  case 4: case 5: case 6: cov_collect_digest (off16 (st, st + 4), d); return;
  default: *d = full; return;
  }
}

static const uint8_t *subtable_coverage (const uint8_t *st, unsigned table_index, unsigned type)
{
  unsigned fmt = rd16 (st);
  if (type == (table_index ? 7u : 5u) && fmt == 3) return off16 (st, st + 6);
  if (type == (table_index ? 8u : 6u) && fmt == 3)
  {
    const uint8_t *in = st + 4 + rd16 (st + 2) * 2;
    return arr_off16 (st, in, 0);
  }
  return off16 (st, st + 2);
}

static int subtable_dispatched (unsigned table_index, unsigned type, unsigned fmt)
{
  if (!table_index)
    switch (type)
    {
    case 1: return fmt == 1 || fmt == 2;
    case 2: case 3: case 4: case 8: return fmt == 1;
    case 5: case 6: return fmt >= 1 && fmt <= 3;
    default: return 0;
    }
  switch (type)
  {
  case 1: case 2: return fmt == 1 || fmt == 2;
  case 3: case 4: case 5: case 6: return fmt == 1;
  case 7: case 8: return fmt >= 1 && fmt <= 3;
  default: return 0;
  }
}

static void accel_add (laccel_t *a, unsigned *n, digest_t *second, const uint8_t *st, unsigned table_index, unsigned type)
{
  if (!st) return;
  unsigned ext = table_index ? 9 : 7;
  if (type == ext)
  {
    if (rd16 (st) != 1) return;
    accel_add (a, n, second, off32 (st, st + 4), table_index, rd16 (st + 2));
    return;
  }
  if (!subtable_dispatched (table_index, type, rd16 (st))) return;
  collect_second (st, table_index, type, second);
  lsub_t *s = &a->sub[(*n)++];
  s->st = st;
  s->type = type;
  memset (&s->digest, 0, sizeof s->digest);
  cov_collect_digest (subtable_coverage (st, table_index, type), &s->digest);
}

static laccel_t *get_accel (gtab_t *g, unsigned table_index, unsigned i)
{
  if (i >= g->lookup_count) return NULL;
  if (g->accels[i]) return g->accels[i];
  const uint8_t *L = gtab_lookup (g, i);
  unsigned count = L ? rd16 (L + 4) : 0;
  laccel_t *a = (laccel_t *) calloc (1, sizeof (laccel_t) + count * sizeof (lsub_t));
  if (!a) return NULL;
  unsigned n = 0;
  for (unsigned k = 0; k < count; k++)
    accel_add (a, &n, &a->digest_second, off16 (L, L + 6 + k * 2), table_index, rd16 (L));
  a->count = n;
  for (unsigned k = 0; k < n; k++) digest_union (&a->digest, &a->sub[k].digest);
  g->accels[i] = a;
  return a;
}

typedef int (*match_func_t) (hb_glyph_info_t *info, unsigned value, const void *data);

typedef struct
{
  unsigned idx, end;
  unsigned lookup_props;
  hb_mask_t mask;
  int ignore_zwnj, ignore_zwj, ignore_hidden, per_syllable;
  uint8_t syllable;
  match_func_t match_func;
  const void *match_data;
  const uint8_t *glyph_data;
} skippy_t;

typedef struct
{
  unsigned table_index;
  hb_font_t *font;
  hb_buffer_t *buffer;
  layout_t *l;
  gtab_t *g;
  sc_t sanitizer;
  scalar_cache_t *var_store_cache;
  int direction;
  hb_mask_t lookup_mask;
  unsigned lookup_index, lookup_props, nesting_level_left;
  int has_glyph_classes, auto_zwnj, auto_zwj, per_syllable, random;
  int last_base;
  unsigned last_base_until;
  uint32_t *mp;
  unsigned mp_len;
  int mp_heap;
  skippy_t iter_input, iter_context;
} ot_apply_t;

enum { MATCH_NO, MATCH_YES, MATCH_MAYBE };
enum { SKIP_NO, SKIP_YES, SKIP_MAYBE };
enum { IT_MATCH, IT_NOT_MATCH, IT_SKIP };

static scalar_cache_t *font_gdef_cache (hb_font_t *font, layout_t *l)
{
  if (!font->gdef_cache_valid || font->gdef_cache_serial != font->serial_coords)
  {
    free (font->gdef_cache);
    font->gdef_cache = scalar_cache_create (ivs_region_count (l->ivs));
    font->gdef_cache_serial = font->serial_coords;
    font->gdef_cache_valid = 1;
  }
  return font->gdef_cache;
}

static int mp_resize (ot_apply_t *c, unsigned n)
{
  if (n <= c->mp_len) return 1;
  uint32_t *a = (uint32_t *) (c->mp_heap ? realloc (c->mp, n * sizeof (uint32_t)) : malloc (n * sizeof (uint32_t)));
  if (!a) return 0;
  if (!c->mp_heap) memcpy (a, c->mp, c->mp_len * sizeof (uint32_t));
  c->mp = a;
  c->mp_len = n;
  c->mp_heap = 1;
  return 1;
}

static int check_glyph_property (const ot_apply_t *c, const hb_glyph_info_t *info, unsigned match_props)
{
  unsigned glyph_props = GPROPS (info);
  if (glyph_props & match_props & LOOKUP_IGNORE_FLAGS) return 0;
  if (glyph_props & GLYPH_PROPS_MARK)
  {
    if (match_props & LOOKUP_USE_MARK_FILTERING_SET) return mark_set_covers (c->l, match_props >> 16, info->codepoint);
    if (match_props & LOOKUP_MARK_ATTACHMENT_TYPE) return (match_props & LOOKUP_MARK_ATTACHMENT_TYPE) == (glyph_props & LOOKUP_MARK_ATTACHMENT_TYPE);
  }
  return 1;
}

static void skippy_init (ot_apply_t *c, skippy_t *it, int context_match)
{
  it->end = c->buffer->len;
  it->glyph_data = NULL;
  it->match_func = NULL;
  it->match_data = NULL;
  it->lookup_props = c->lookup_props;
  it->ignore_zwnj = c->table_index == 1 || (context_match && c->auto_zwnj);
  it->ignore_zwj = context_match || c->auto_zwj;
  it->ignore_hidden = c->table_index == 1;
  it->mask = context_match ? (hb_mask_t) -1 : c->lookup_mask;
  it->per_syllable = c->table_index == 0 && c->per_syllable;
  it->syllable = 0;
}

static void init_iters (ot_apply_t *c)
{
  skippy_init (c, &c->iter_input, 0);
  skippy_init (c, &c->iter_context, 1);
}

static void set_lookup_props (ot_apply_t *c, unsigned props)
{
  if ((props & LOOKUP_USE_MARK_FILTERING_SET) && (props >> 16) >= c->l->mark_set_count) props &= ~LOOKUP_USE_MARK_FILTERING_SET;
  c->lookup_props = props;
  init_iters (c);
}

static void skippy_reset (ot_apply_t *c, skippy_t *it, unsigned start)
{
  it->idx = start;
  it->end = c->buffer->len;
  it->syllable = SYLLABLE (buf_cur (c->buffer));
}

static void skippy_reset_back (ot_apply_t *c, skippy_t *it, unsigned start)
{
  it->idx = start;
  it->syllable = SYLLABLE (buf_cur (c->buffer));
}

static int may_skip (const ot_apply_t *c, const skippy_t *it, const hb_glyph_info_t *info)
{
  if (!check_glyph_property (c, info, it->lookup_props)) return SKIP_YES;
  if (info_is_default_ignorable (info) &&
      (it->ignore_zwnj || !info_is_zwnj (info)) &&
      (it->ignore_zwj || !info_is_zwj (info)) &&
      (it->ignore_hidden || !info_is_hidden (info)))
    return SKIP_MAYBE;
  return SKIP_NO;
}

static int skippy_match (const ot_apply_t *c, skippy_t *it, hb_glyph_info_t *info)
{
  int skip = may_skip (c, it, info);
  if (skip == SKIP_YES) return IT_SKIP;
  int m;
  if (!(info->mask & it->mask) || (it->per_syllable && it->syllable && it->syllable != SYLLABLE (info))) m = MATCH_NO;
  else if (it->match_func) m = it->match_func (info, it->glyph_data ? rd16 (it->glyph_data) : 0, it->match_data) ? MATCH_YES : MATCH_NO;
  else m = MATCH_MAYBE;
  if (m == MATCH_YES || (m == MATCH_MAYBE && skip == SKIP_NO)) return IT_MATCH;
  if (skip == SKIP_NO) return IT_NOT_MATCH;
  return IT_SKIP;
}

static int skippy_next (ot_apply_t *c, skippy_t *it, unsigned *unsafe_to)
{
  hb_glyph_info_t *info = c->buffer->info;
  int stop = (int) it->end - 1;
  while ((int) it->idx < stop)
  {
    it->idx++;
    switch (skippy_match (c, it, &info[it->idx]))
    {
    case IT_MATCH:
      if (it->glyph_data) it->glyph_data += 2;
      return 1;
    case IT_NOT_MATCH:
      if (unsafe_to) *unsafe_to = it->idx + 1;
      return 0;
    }
  }
  if (unsafe_to) *unsafe_to = it->end;
  return 0;
}

static int skippy_prev (ot_apply_t *c, skippy_t *it, unsigned *unsafe_from)
{
  hb_glyph_info_t *out_info = c->buffer->out_info;
  while (it->idx > 0)
  {
    it->idx--;
    switch (skippy_match (c, it, &out_info[it->idx]))
    {
    case IT_MATCH:
      if (it->glyph_data) it->glyph_data += 2;
      return 1;
    case IT_NOT_MATCH:
      if (unsafe_from) *unsafe_from = HB_MAX (1u, it->idx) - 1u;
      return 0;
    }
  }
  if (unsafe_from) *unsafe_from = 0;
  return 0;
}

static void set_glyph_class (ot_apply_t *c, hb_codepoint_t g, unsigned class_guess, int ligature, int component)
{
  hb_buffer_t *b = c->buffer;
  digest_add (&b->digest, g);
  hb_glyph_info_t *cur = buf_cur (b);
  unsigned props = GPROPS (cur) | GLYPH_PROPS_SUBSTITUTED;
  if (ligature)
  {
    props |= GLYPH_PROPS_LIGATED;
    props &= ~GLYPH_PROPS_MULTIPLIED;
  }
  if (component) props |= GLYPH_PROPS_MULTIPLIED;
  if (c->has_glyph_classes) GPROPS (cur) = (props & GLYPH_PROPS_PRESERVE) | gdef_glyph_props (c->l, g);
  else if (class_guess) GPROPS (cur) = (props & GLYPH_PROPS_PRESERVE) | class_guess;
  else GPROPS (cur) = props;
}

static void replace_glyph (ot_apply_t *c, hb_codepoint_t g)
{
  set_glyph_class (c, g, 0, 0, 0);
  buf_replace_glyph (c->buffer, g);
}

static int match_glyph (hb_glyph_info_t *info, unsigned value, const void *data) { return info->codepoint == value; }
static int match_class (hb_glyph_info_t *info, unsigned value, const void *data) { return classdef_get ((const uint8_t *) data, info->codepoint) == value; }
static int match_coverage (hb_glyph_info_t *info, unsigned value, const void *data)
{
  return cov_get (value ? (const uint8_t *) data + value : NULL, info->codepoint) != NOT_COVERED;
}

static int match_input (ot_apply_t *c, unsigned count, const uint8_t *input, match_func_t match_func, const void *match_data,
			unsigned *end_position, unsigned *p_total_component_count)
{
  hb_buffer_t *buffer = c->buffer;
  if (count == 1)
  {
    *end_position = buffer->idx + 1;
    c->mp[0] = buffer->idx;
    if (p_total_component_count) *p_total_component_count = info_lig_num_comps (buf_cur (buffer));
    return 1;
  }
  if (count > MAX_CONTEXT_LENGTH) return 0;
  skippy_t *it = &c->iter_input;
  skippy_reset (c, it, buffer->idx);
  it->match_func = match_func;
  it->match_data = match_data;
  it->glyph_data = input;
  unsigned total_component_count = 0;
  unsigned first_lig_id = info_lig_id (buf_cur (buffer));
  unsigned first_lig_comp = info_lig_comp (buf_cur (buffer));
  enum { LIGBASE_NOT_CHECKED, LIGBASE_MAY_NOT_SKIP, LIGBASE_MAY_SKIP } ligbase = LIGBASE_NOT_CHECKED;
  for (unsigned i = 1; i < count; i++)
  {
    unsigned unsafe_to;
    if (!skippy_next (c, it, &unsafe_to))
    {
      *end_position = unsafe_to;
      return 0;
    }
    if (i + 1 > c->mp_len && !mp_resize (c, i + 1)) return 0;
    c->mp[i] = it->idx;
    const hb_glyph_info_t *ti = &buffer->info[it->idx];
    unsigned this_lig_id = info_lig_id (ti), this_lig_comp = info_lig_comp (ti);
    if (first_lig_id && first_lig_comp)
    {
      if (first_lig_id != this_lig_id || first_lig_comp != this_lig_comp)
      {
	if (ligbase == LIGBASE_NOT_CHECKED)
	{
	  int found = 0;
	  const hb_glyph_info_t *out = buffer->out_info;
	  unsigned j = buffer->out_len;
	  while (j && info_lig_id (&out[j - 1]) == first_lig_id)
	  {
	    if (info_lig_comp (&out[j - 1]) == 0)
	    {
	      j--;
	      found = 1;
	      break;
	    }
	    j--;
	  }
	  if (found && may_skip (c, it, &out[j]) == SKIP_YES) ligbase = LIGBASE_MAY_SKIP;
	  else ligbase = LIGBASE_MAY_NOT_SKIP;
	}
	if (ligbase == LIGBASE_MAY_NOT_SKIP) return 0;
      }
    }
    else if (this_lig_id && this_lig_comp && this_lig_id != first_lig_id)
      return 0;
    total_component_count += info_lig_num_comps_in_ligation (ti);
  }
  *end_position = it->idx + 1;
  if (p_total_component_count)
  {
    total_component_count += info_lig_num_comps (buf_cur (buffer));
    *p_total_component_count = total_component_count;
  }
  c->mp[0] = buffer->idx;
  return 1;
}

static void ligate_input (ot_apply_t *c, unsigned count, unsigned match_end, hb_codepoint_t lig_glyph, unsigned total_component_count)
{
  hb_buffer_t *buffer = c->buffer;
  buf_merge_clusters (buffer, buffer->idx, match_end);
  int is_base_ligature = info_is_base_glyph (&buffer->info[c->mp[0]]);
  int is_mark_ligature = info_is_mark (&buffer->info[c->mp[0]]);
  for (unsigned i = 1; i < count; i++)
    if (!info_is_mark (&buffer->info[c->mp[i]]))
    {
      is_base_ligature = is_mark_ligature = 0;
      break;
    }
  int is_ligature = !is_base_ligature && !is_mark_ligature;
  unsigned klass = is_ligature ? GLYPH_PROPS_LIGATURE : 0;
  unsigned lig_id = is_ligature ? allocate_lig_id (buffer) : 0;
  unsigned last_lig_id = info_lig_id (buf_cur (buffer));
  unsigned last_num_components = info_lig_num_comps (buf_cur (buffer));
  unsigned components_so_far = last_num_components;
  if (is_ligature)
  {
    info_set_lig_props_for_ligature (buf_cur (buffer), lig_id, total_component_count);
    if (info_gc (buf_cur (buffer)) == GC_NON_SPACING_MARK) info_set_gc (buf_cur (buffer), GC_OTHER_LETTER);
  }
  set_glyph_class (c, lig_glyph, klass, 1, 0);
  buf_replace_glyph (buffer, lig_glyph);
  for (unsigned i = 1; i < count; i++)
  {
    while (buffer->idx < c->mp[i] && buffer->successful)
    {
      if (is_ligature)
      {
	unsigned this_comp = info_lig_comp (buf_cur (buffer));
	if (this_comp == 0) this_comp = last_num_components;
	unsigned new_lig_comp = components_so_far - last_num_components + HB_MIN (this_comp, last_num_components);
	info_set_lig_props_for_mark (buf_cur (buffer), lig_id, new_lig_comp);
      }
      buf_next_glyph (buffer);
    }
    last_lig_id = info_lig_id (buf_cur (buffer));
    last_num_components = info_lig_num_comps_in_ligation (buf_cur (buffer));
    components_so_far += last_num_components;
    buffer->idx++;
  }
  if (!is_mark_ligature && last_lig_id)
    for (unsigned i = buffer->idx; i < buffer->len; ++i)
    {
      if (last_lig_id != info_lig_id (&buffer->info[i])) break;
      unsigned this_comp = info_lig_comp (&buffer->info[i]);
      if (!this_comp) break;
      unsigned new_lig_comp = components_so_far - last_num_components + HB_MIN (this_comp, last_num_components);
      info_set_lig_props_for_mark (&buffer->info[i], lig_id, new_lig_comp);
    }
}

static int match_backtrack (ot_apply_t *c, unsigned count, const uint8_t *backtrack, match_func_t f, const void *data, unsigned *match_start)
{
  if (!count)
  {
    *match_start = buf_backtrack_len (c->buffer);
    return 1;
  }
  skippy_t *it = &c->iter_context;
  skippy_reset_back (c, it, buf_backtrack_len (c->buffer));
  it->match_func = f;
  it->match_data = data;
  it->glyph_data = backtrack;
  for (unsigned i = 0; i < count; i++)
  {
    unsigned unsafe_from;
    if (!skippy_prev (c, it, &unsafe_from))
    {
      *match_start = unsafe_from;
      return 0;
    }
  }
  *match_start = it->idx;
  return 1;
}

static int match_lookahead (ot_apply_t *c, unsigned count, const uint8_t *lookahead, match_func_t f, const void *data, unsigned start_index, unsigned *end_index)
{
  if (!count)
  {
    *end_index = start_index;
    return 1;
  }
  skippy_t *it = &c->iter_context;
  skippy_reset (c, it, start_index - 1);
  it->match_func = f;
  it->match_data = data;
  it->glyph_data = lookahead;
  for (unsigned i = 0; i < count; i++)
  {
    unsigned unsafe_to;
    if (!skippy_next (c, it, &unsafe_to))
    {
      *end_index = unsafe_to;
      return 0;
    }
  }
  *end_index = it->idx + 1;
  return 1;
}

static int recurse (ot_apply_t *c, unsigned lookup_index);

static void apply_lookup (ot_apply_t *c, unsigned count, unsigned lookup_count, const uint8_t *lookup_record, unsigned match_end)
{
  hb_buffer_t *buffer = c->buffer;
  int end;
  {
    unsigned bl = buf_backtrack_len (buffer);
    end = bl + match_end - buffer->idx;
    int delta = bl - buffer->idx;
    for (unsigned j = 0; j < count; j++) c->mp[j] += delta;
  }
  for (unsigned i = 0; i < lookup_count && buffer->successful; i++)
  {
    unsigned idx = rd16 (lookup_record + i * 4);
    if (idx >= count) continue;
    unsigned orig_len = buf_backtrack_len (buffer) + buffer->len - buffer->idx;
    if (c->mp[idx] >= orig_len) continue;
    if (!buf_move_to (buffer, c->mp[idx])) break;
    if (buffer->max_ops <= 0) break;
    if (!recurse (c, rd16 (lookup_record + i * 4 + 2))) continue;
    unsigned new_len = buf_backtrack_len (buffer) + buffer->len - buffer->idx;
    int delta = new_len - orig_len;
    if (!delta) continue;
    end += delta;
    if (end < (int) c->mp[idx])
    {
      delta += c->mp[idx] - end;
      end = c->mp[idx];
    }
    unsigned next = idx + 1;
    if (delta > 0)
    {
      if (delta + count > MAX_CONTEXT_LENGTH) break;
      if (count + delta > c->mp_len && !mp_resize (c, count + delta)) return;
    }
    else
    {
      delta = HB_MAX (delta, (int) next - (int) count);
      next -= delta;
    }
    memmove (c->mp + next + delta, c->mp + next, (count - next) * sizeof (c->mp[0]));
    next += delta;
    count += delta;
    for (unsigned j = idx + 1; j < next; j++) c->mp[j] = c->mp[j - 1] + 1;
    for (; next < count; next++) c->mp[next] += delta;
  }
  buf_move_to (buffer, end);
}

static int context_apply_lookup (ot_apply_t *c, unsigned input_count, const uint8_t *input, unsigned lookup_count, const uint8_t *lookup_record,
				 match_func_t f, const void *data)
{
  if (input_count > MAX_CONTEXT_LENGTH) return 0;
  unsigned match_end = 0;
  if (match_input (c, input_count, input, f, data, &match_end, NULL))
  {
    buf_unsafe_to_break (c->buffer, c->buffer->idx, match_end);
    apply_lookup (c, input_count, lookup_count, lookup_record, match_end);
    return 1;
  }
  return 0;
}

static int chain_context_apply_lookup (ot_apply_t *c,
				       unsigned backtrack_count, const uint8_t *backtrack,
				       unsigned input_count, const uint8_t *input,
				       unsigned lookahead_count, const uint8_t *lookahead,
				       unsigned lookup_count, const uint8_t *lookup_record,
				       match_func_t f[3], const void *data[3])
{
  if (input_count > MAX_CONTEXT_LENGTH) return 0;
  unsigned start_index = c->buffer->out_len, end_index = c->buffer->idx, match_end = 0;
  if (!(match_input (c, input_count, input, f[1], data[1], &match_end, NULL) && (end_index = match_end) &&
	match_lookahead (c, lookahead_count, lookahead, f[2], data[2], match_end, &end_index)))
    return 0;
  if (!match_backtrack (c, backtrack_count, backtrack, f[0], data[0], &start_index)) return 0;
  buf_unsafe_to_break_from_outbuffer (c->buffer, start_index, end_index);
  apply_lookup (c, input_count, lookup_count, lookup_record, match_end);
  return 1;
}

static int apply_context (ot_apply_t *c, const uint8_t *st)
{
  hb_codepoint_t g = buf_cur (c->buffer)->codepoint;
  unsigned fmt = rd16 (st);
  if (fmt == 3)
  {
    if (cov_get (off16 (st, st + 6), g) == NOT_COVERED) return 0;
    unsigned n = rd16 (st + 2);
    return context_apply_lookup (c, n, st + 8, rd16 (st + 4), st + 6 + n * 2, match_coverage, st);
  }
  if (cov_get (off16 (st, st + 2), g) == NOT_COVERED) return 0;
  const uint8_t *cd = NULL, *rs;
  if (fmt == 1) rs = arr_off16 (st, st + 4, cov_get (off16 (st, st + 2), g));
  else
  {
    cd = off16 (st, st + 4);
    rs = arr_off16 (st, st + 6, classdef_get (cd, g));
  }
  if (!rs) return 0;
  unsigned n = rd16 (rs);
  for (unsigned i = 0; i < n; i++)
  {
    const uint8_t *r = off16 (rs, rs + 2 + i * 2);
    unsigned ic = r ? rd16 (r) : 0, lc = r ? rd16 (r + 2) : 0;
    if (!r && context_apply_lookup (c, 0, NULL, 0, NULL, fmt == 1 ? match_glyph : match_class, cd)) return 1;
    if (r && context_apply_lookup (c, ic, r + 4, lc, r + 4 + (ic ? ic - 1 : 0) * 2, fmt == 1 ? match_glyph : match_class, cd)) return 1;
  }
  return 0;
}

static int apply_chain_context (ot_apply_t *c, const uint8_t *st)
{
  hb_codepoint_t g = buf_cur (c->buffer)->codepoint;
  unsigned fmt = rd16 (st);
  if (fmt == 3)
  {
    const uint8_t *bt = st + 2, *in = bt + 2 + rd16 (bt) * 2, *la = in + 2 + rd16 (in) * 2, *lk = la + 2 + rd16 (la) * 2;
    if (cov_get (arr_off16 (st, in, 0), g) == NOT_COVERED) return 0;
    match_func_t f[3] = {match_coverage, match_coverage, match_coverage};
    const void *d[3] = {st, st, st};
    return chain_context_apply_lookup (c, rd16 (bt), bt + 2, rd16 (in), in + 4, rd16 (la), la + 2, rd16 (lk), lk + 2, f, d);
  }
  if (cov_get (off16 (st, st + 2), g) == NOT_COVERED) return 0;
  const uint8_t *rs;
  match_func_t f[3];
  const void *d[3];
  if (fmt == 1)
  {
    rs = arr_off16 (st, st + 4, cov_get (off16 (st, st + 2), g));
    f[0] = f[1] = f[2] = match_glyph;
    d[0] = d[1] = d[2] = NULL;
  }
  else
  {
    const uint8_t *icd = off16 (st, st + 6);
    rs = arr_off16 (st, st + 10, classdef_get (icd, g));
    f[0] = f[1] = f[2] = match_class;
    d[0] = off16 (st, st + 4);
    d[1] = icd;
    d[2] = off16 (st, st + 8);
  }
  if (!rs) return 0;
  unsigned n = rd16 (rs);
  for (unsigned i = 0; i < n; i++)
  {
    const uint8_t *r = off16 (rs, rs + 2 + i * 2);
    if (!r)
    {
      if (chain_context_apply_lookup (c, 0, NULL, 0, NULL, 0, NULL, 0, NULL, f, d)) return 1;
      continue;
    }
    const uint8_t *in = r + 2 + rd16 (r) * 2;
    unsigned ilen = rd16 (in);
    const uint8_t *la = in + 2 + (ilen ? ilen - 1 : 0) * 2, *lk = la + 2 + rd16 (la) * 2;
    if (chain_context_apply_lookup (c, rd16 (r), r + 2, ilen, in + 2, rd16 (la), la + 2, rd16 (lk), lk + 2, f, d)) return 1;
  }
  return 0;
}

static int apply_ligature (ot_apply_t *c, const uint8_t *lig)
{
  unsigned count = rd16 (lig + 2);
  if (!count) return 0;
  if (count == 1)
  {
    replace_glyph (c, rd16 (lig));
    return 1;
  }
  if (count > MAX_CONTEXT_LENGTH) return 0;
  unsigned total_component_count = 0, match_end = 0;
  if (!match_input (c, count, lig + 4, match_glyph, NULL, &match_end, &total_component_count)) return 0;
  ligate_input (c, count, match_end, rd16 (lig), total_component_count);
  return 1;
}

static int apply_gsub (ot_apply_t *c, const uint8_t *st, unsigned type)
{
  hb_buffer_t *buffer = c->buffer;
  hb_codepoint_t g = buf_cur (buffer)->codepoint;
  unsigned fmt = rd16 (st);
  switch (type)
  {
  case 1:
  {
    unsigned index = cov_get (off16 (st, st + 2), g);
    if (index == NOT_COVERED) return 0;
    if (fmt == 1)
    {
      replace_glyph (c, (g + rd16 (st + 4)) & 0xFFFF);
      return 1;
    }
    if (index >= rd16 (st + 4)) return 0;
    replace_glyph (c, rd16 (st + 6 + index * 2));
    return 1;
  }
  case 2:
  {
    unsigned index = cov_get (off16 (st, st + 2), g);
    if (index == NOT_COVERED) return 0;
    const uint8_t *seq = arr_off16 (st, st + 4, index);
    unsigned count = seq ? rd16 (seq) : 0;
    if (count == 1)
    {
      replace_glyph (c, rd16 (seq + 2));
      return 1;
    }
    if (count == 0)
    {
      buf_delete_glyph (buffer);
      return 1;
    }
    unsigned klass = info_is_ligature (buf_cur (buffer)) ? GLYPH_PROPS_BASE_GLYPH : 0;
    unsigned lig_id = info_lig_id (buf_cur (buffer));
    for (unsigned i = 0; i < count; i++)
    {
      if (!lig_id) info_set_lig_props_for_component (buf_cur (buffer), i);
      hb_codepoint_t s = rd16 (seq + 2 + i * 2);
      set_glyph_class (c, s, klass, 0, 1);
      buf_output_glyph (buffer, s);
    }
    buf_skip_glyph (buffer);
    return 1;
  }
  case 3:
  {
    unsigned index = cov_get (off16 (st, st + 2), g);
    if (index == NOT_COVERED) return 0;
    const uint8_t *as = arr_off16 (st, st + 4, index);
    unsigned count = as ? rd16 (as) : 0;
    if (!count) return 0;
    hb_mask_t glyph_mask = buf_cur (buffer)->mask, lookup_mask = c->lookup_mask;
    unsigned shift = __builtin_ctz (lookup_mask);
    unsigned alt_index = (lookup_mask & glyph_mask) >> shift;
    if (alt_index == 255 && c->random)
    {
      buf_unsafe_to_break (buffer, 0, buffer->len);
      buffer->random_state = buffer->random_state * 48271u % 2147483647u;
      alt_index = buffer->random_state % count + 1;
    }
    if (alt_index > count || alt_index == 0) return 0;
    replace_glyph (c, rd16 (as + 2 + (alt_index - 1) * 2));
    return 1;
  }
  case 4:
  {
    unsigned index = cov_get (off16 (st, st + 2), g);
    if (index == NOT_COVERED) return 0;
    const uint8_t *ls = arr_off16 (st, st + 4, index);
    unsigned n = ls ? rd16 (ls) : 0;
    for (unsigned i = 0; i < n; i++)
    {
      const uint8_t *lig = off16 (ls, ls + 2 + i * 2);
      if (lig && apply_ligature (c, lig)) return 1;
    }
    return 0;
  }
  case 5: return apply_context (c, st);
  case 6: return apply_chain_context (c, st);
  case 8:
  {
    unsigned index = cov_get (off16 (st, st + 2), g);
    if (index == NOT_COVERED) return 0;
    if (c->nesting_level_left != MAX_NESTING_LEVEL) return 0;
    const uint8_t *bt = st + 4, *la = bt + 2 + rd16 (bt) * 2, *sub = la + 2 + rd16 (la) * 2;
    if (index >= rd16 (sub)) return 0;
    unsigned start_index = 0, end_index = 0;
    if (match_backtrack (c, rd16 (bt), bt + 2, match_coverage, st, &start_index) &&
	match_lookahead (c, rd16 (la), la + 2, match_coverage, st, buffer->idx + 1, &end_index))
    {
      buf_unsafe_to_break_from_outbuffer (buffer, start_index, end_index);
      set_glyph_class (c, rd16 (sub + 2 + index * 2), 0, 0, 0);
      buf_cur (buffer)->codepoint = rd16 (sub + 2 + index * 2);
      return 1;
    }
    return 0;
  }
  }
  return 0;
}

static const uint8_t *lazy_device (ot_apply_t *c, const uint8_t *field, const uint8_t *base, int *ok)
{
  *ok = sc_off16 (&c->sanitizer, field, base, sc_device, 0);
  return *ok ? off16 (base, field) : NULL;
}

static int device_x_delta (ot_apply_t *c, const uint8_t *d)
{
  hb_font_t *font = c->font;
  if (!d || rd16 (d + 4) != 0x8000 || !font->has_nonzero_coords) return 0;
  return em_scalef_x (font, ivs_get_delta (c->l->ivs, rd32 (d), font->coords, font->num_coords, c->var_store_cache));
}

static int device_y_delta (ot_apply_t *c, const uint8_t *d)
{
  hb_font_t *font = c->font;
  if (!d) return 0;
  switch (rd16 (d + 4))
  {
  case 1: case 2: case 3: case 0x8000: break;
  default: return 0;
  }
  if (!font->has_nonzero_coords) return 0;
  return em_scalef_y (font, ivs_get_delta (c->l->ivs, rd32 (d), font->coords, font->num_coords, c->var_store_cache));
}

static int apply_value (ot_apply_t *c, const uint8_t *base, const uint8_t *v, unsigned format, hb_glyph_position_t *pos)
{
  int ret = 0, ok;
  if (!format) return 0;
  hb_font_t *font = c->font;
  if (format & 0x01) { ret |= rd16 (v) != 0; pos->x_offset = sat_add (pos->x_offset, em_scale_x (font, rds16 (v))); v += 2; }
  if (format & 0x02) { ret |= rd16 (v) != 0; pos->y_offset = sat_add (pos->y_offset, em_scale_y (font, rds16 (v))); v += 2; }
  if (format & 0x04) { ret |= rd16 (v) != 0; pos->x_advance = sat_add (pos->x_advance, em_scale_x (font, rds16 (v))); v += 2; }
  if (format & 0x08) v += 2;
  if (!(format & 0xF0)) return ret;
  int use_x = font->x_ppem || font->has_nonzero_coords;
  int use_y = font->y_ppem || font->has_nonzero_coords;
  if (!use_x && !use_y) return ret;
  if (format & 0x10)
  {
    if (use_x)
    {
      ret |= rd16 (v) != 0;
      const uint8_t *d = lazy_device (c, v, base, &ok);
      pos->x_offset = sat_add (pos->x_offset, device_x_delta (c, d));
    }
    v += 2;
  }
  if (format & 0x20)
  {
    if (use_y)
    {
      ret |= rd16 (v) != 0;
      const uint8_t *d = lazy_device (c, v, base, &ok);
      pos->y_offset = sat_add (pos->y_offset, device_y_delta (c, d));
    }
    v += 2;
  }
  if (format & 0x40)
  {
    if (use_x)
    {
      ret |= rd16 (v) != 0;
      const uint8_t *d = lazy_device (c, v, base, &ok);
      pos->x_advance = sat_add (pos->x_advance, device_x_delta (c, d));
    }
    v += 2;
  }
  return ret;
}

static void get_anchor (ot_apply_t *c, const uint8_t *a, float *x, float *y)
{
  *x = *y = 0;
  if (!a) return;
  hb_font_t *font = c->font;
  switch (rd16 (a))
  {
  case 1: case 2:
    *x = em_fscale_x (font, rds16 (a + 2));
    *y = em_fscale_y (font, rds16 (a + 4));
    return;
  case 3:
    *x = em_fscale_x (font, rds16 (a + 2));
    *y = em_fscale_y (font, rds16 (a + 4));
    if ((font->x_ppem || font->has_nonzero_coords) && sc_off16 (&c->sanitizer, a + 6, a, sc_device, 0))
      *x += device_x_delta (c, off16 (a, a + 6));
    if ((font->y_ppem || font->has_nonzero_coords) && sc_off16 (&c->sanitizer, a + 8, a, sc_device, 0))
      *y += device_y_delta (c, off16 (a, a + 8));
    return;
  }
}

static int pair_record_cmp (const void *key, const uint8_t *item, const void *ud)
{
  unsigned g = *(const unsigned *) key, v = rd16 (item);
  return (g > v) - (g < v);
}

static int apply_pair (ot_apply_t *c, const uint8_t *st)
{
  hb_buffer_t *buffer = c->buffer;
  unsigned index = cov_get (off16 (st, st + 2), buf_cur (buffer)->codepoint);
  if (index == NOT_COVERED) return 0;
  skippy_t *it = &c->iter_input;
  it->idx = buffer->idx;
  if (!skippy_next (c, it, NULL)) return 0;
  unsigned pos = it->idx;
  if (rd16 (st) == 1)
  {
    unsigned f1 = rd16 (st + 4), f2 = rd16 (st + 6), len1 = vf_len (f1), len2 = vf_len (f2);
    const uint8_t *ps = arr_off16 (st, st + 8, index);
    if (!ps) return 0;
    unsigned record_size = 2 + 2 * (len1 + len2);
    hb_codepoint_t g = buffer->info[pos].codepoint;
    int i = hb_bsearch (&g, ps + 2, rd16 (ps), record_size, pair_record_cmp, NULL);
    if (i < 0) return 0;
    const uint8_t *r = ps + 2 + i * record_size;
    int applied_first = len1 && apply_value (c, ps, r + 2, f1, &buffer->pos[buffer->idx]);
    int applied_second = len2 && apply_value (c, ps, r + 2 + len1 * 2, f2, &buffer->pos[pos]);
    if (applied_first || applied_second) buf_unsafe_to_break (buffer, buffer->idx, pos + 1);
    if (len2)
    {
      pos++;
      buf_unsafe_to_break (buffer, buffer->idx, pos + 1);
    }
    buffer->idx = pos;
    return 1;
  }
  unsigned klass1 = classdef_get (off16 (st, st + 8), buf_cur (buffer)->codepoint);
  unsigned klass2 = classdef_get (off16 (st, st + 10), buffer->info[pos].codepoint);
  unsigned c1 = rd16 (st + 12), c2 = rd16 (st + 14);
  if (klass1 >= c1 || klass2 >= c2) return 0;
  unsigned f1 = rd16 (st + 4), f2 = rd16 (st + 6), len1 = vf_len (f1), len2 = vf_len (f2);
  const uint8_t *v = st + 16 + 2 * (len1 + len2) * (klass1 * c2 + klass2);
  int applied_first = len1 && apply_value (c, st, v, f1, &buffer->pos[buffer->idx]);
  int applied_second = len2 && apply_value (c, st, v + len1 * 2, f2, &buffer->pos[pos]);
  if (applied_first || applied_second) buf_unsafe_to_break (buffer, buffer->idx, pos + 1);
  if (len2)
  {
    pos++;
    buf_unsafe_to_break (buffer, buffer->idx, pos + 1);
  }
  buffer->idx = pos;
  return 1;
}

static void reverse_cursive_minor_offset (hb_glyph_position_t *pos, unsigned len, unsigned i, unsigned new_parent, unsigned nesting_level)
{
  if (nesting_level > MAX_NESTING_LEVEL) return;
  int chain = ATTACH_CHAIN (&pos[i]), type = ATTACH_TYPE (&pos[i]);
  if (!chain || !(type & ATTACH_TYPE_CURSIVE)) return;
  ATTACH_CHAIN (&pos[i]) = 0;
  unsigned j = (int) i + chain;
  if (j >= len) return;
  if (j == new_parent) return;
  int16_t reversed_chain = -chain;
  if (reversed_chain != -chain) return;
  reverse_cursive_minor_offset (pos, len, j, new_parent, nesting_level + 1);
  pos[j].y_offset = sat_sub (0, pos[i].y_offset);
  ATTACH_CHAIN (&pos[j]) = reversed_chain;
  ATTACH_TYPE (&pos[j]) = type;
}

static const uint8_t *entry_exit (const uint8_t *st, unsigned index, unsigned which)
{
  if (index >= rd16 (st + 4)) return NULL;
  return st + 6 + index * 4 + which * 2;
}

static int apply_cursive (ot_apply_t *c, const uint8_t *st)
{
  hb_buffer_t *buffer = c->buffer;
  const uint8_t *cov = off16 (st, st + 2);
  const uint8_t *entry = entry_exit (st, cov_get (cov, buf_cur (buffer)->codepoint), 0);
  if (!entry || !rd16 (entry) || !sc_off16 (&c->sanitizer, entry, st, sc_anchor, 0)) return 0;
  skippy_t *it = &c->iter_input;
  it->idx = buffer->idx;
  if (!skippy_prev (c, it, NULL)) return 0;
  const uint8_t *exit = entry_exit (st, cov_get (cov, buffer->info[it->idx].codepoint), 1);
  if (!exit || !rd16 (exit) || !sc_off16 (&c->sanitizer, exit, st, sc_anchor, 0)) return 0;
  unsigned i = it->idx, j = buffer->idx;
  buf_unsafe_to_break (buffer, i, j + 1);
  float entry_x, entry_y, exit_x, exit_y;
  get_anchor (c, off16 (st, exit), &exit_x, &exit_y);
  get_anchor (c, off16 (st, entry), &entry_x, &entry_y);
  hb_glyph_position_t *pos = buffer->pos;
  hb_position_t d;
  if (c->direction == DIR_LTR)
  {
    pos[i].x_advance = sat_add ((hb_position_t) hbroundf (exit_x), pos[i].x_offset);
    d = sat_add ((hb_position_t) hbroundf (entry_x), pos[j].x_offset);
    pos[j].x_advance = sat_sub (pos[j].x_advance, d);
    pos[j].x_offset = sat_sub (pos[j].x_offset, d);
  }
  else
  {
    d = sat_add ((hb_position_t) hbroundf (exit_x), pos[i].x_offset);
    pos[i].x_advance = sat_sub (pos[i].x_advance, d);
    pos[i].x_offset = sat_sub (pos[i].x_offset, d);
    pos[j].x_advance = sat_add ((hb_position_t) hbroundf (entry_x), pos[j].x_offset);
  }
  unsigned child = i, parent = j;
  hb_position_t y_offset = (hb_position_t) hbroundf (entry_y - exit_y);
  if (!(c->lookup_props & LOOKUP_RIGHT_TO_LEFT))
  {
    unsigned k = child;
    child = parent;
    parent = k;
    y_offset = -y_offset;
  }
  reverse_cursive_minor_offset (pos, buffer->len, child, parent, 0);
  ATTACH_CHAIN (&pos[child]) = (int) parent - (int) child;
  if (ATTACH_CHAIN (&pos[child]) != (int) parent - (int) child)
  {
    ATTACH_CHAIN (&pos[child]) = 0;
    goto overflow;
  }
  ATTACH_TYPE (&pos[child]) = ATTACH_TYPE_CURSIVE;
  buffer->scratch_flags |= SCRATCH_HAS_GPOS_ATTACHMENT;
  pos[child].y_offset = y_offset;
  if (ATTACH_CHAIN (&pos[parent]) == -ATTACH_CHAIN (&pos[child]))
  {
    ATTACH_CHAIN (&pos[parent]) = 0;
    pos[parent].y_offset = 0;
  }
overflow:
  buffer->idx++;
  return 1;
}

static hb_position_t resolve_cross_offset (const hb_glyph_position_t *pos, unsigned len, unsigned glyph_pos)
{
  hb_position_t offset = pos[glyph_pos].y_offset;
  while (ATTACH_TYPE (&pos[glyph_pos]) & ATTACH_TYPE_CURSIVE)
  {
    int chain = ATTACH_CHAIN (&pos[glyph_pos]);
    if (!chain) break;
    unsigned parent = (int) glyph_pos + chain;
    if (parent >= len) break;
    glyph_pos = parent;
    offset = sat_add (offset, pos[glyph_pos].y_offset);
  }
  return offset;
}

static int mark_array_apply (ot_apply_t *c, const uint8_t *ma, unsigned mark_index, unsigned glyph_index,
			     const uint8_t *anchors, unsigned class_count, unsigned glyph_pos)
{
  hb_buffer_t *buffer = c->buffer;
  const uint8_t *rec = ma && mark_index < rd16 (ma) ? ma + 2 + mark_index * 4 : NULL;
  unsigned mark_class = rec ? rd16 (rec) : 0;
  const uint8_t *mark_anchor = rec ? off16 (ma, rec + 2) : NULL;
  unsigned rows = anchors ? rd16 (anchors) : 0;
  if (glyph_index >= rows || mark_class >= class_count) return 0;
  const uint8_t *field = anchors + 2 + (glyph_index * class_count + mark_class) * 2;
  if (!sc_off16 (&c->sanitizer, field, anchors, sc_anchor, 0)) return 0;
  if (!rd16 (field)) return 0;
  const uint8_t *glyph_anchor = off16 (anchors, field);
  float mark_x, mark_y, base_x, base_y;
  buf_unsafe_to_break (buffer, glyph_pos, buffer->idx + 1);
  get_anchor (c, mark_anchor, &mark_x, &mark_y);
  get_anchor (c, glyph_anchor, &base_x, &base_y);
  hb_position_t base_offset = resolve_cross_offset (buffer->pos, buffer->len, glyph_pos);
  hb_glyph_position_t *mark = &buffer->pos[buffer->idx];
  ATTACH_CHAIN (mark) = (int) glyph_pos - (int) buffer->idx;
  if (ATTACH_CHAIN (mark) != (int) glyph_pos - (int) buffer->idx)
  {
    ATTACH_CHAIN (mark) = 0;
    goto overflow;
  }
  ATTACH_TYPE (mark) = ATTACH_TYPE_MARK;
  mark->x_offset = (hb_position_t) hbroundf (base_x - mark_x);
  mark->y_offset = (hb_position_t) hbroundf (base_y - mark_y);
  mark->y_offset = sat_add (mark->y_offset, base_offset);
  buffer->scratch_flags |= SCRATCH_HAS_GPOS_ATTACHMENT;
overflow:
  buffer->idx++;
  return 1;
}

static int mark_base_accept (hb_buffer_t *buffer, unsigned idx)
{
  const hb_glyph_info_t *info = buffer->info;
  return !info_multiplied (&info[idx]) || 0 == info_lig_comp (&info[idx]) ||
	 (idx == 0 || info_is_mark (&info[idx - 1]) || !info_multiplied (&info[idx - 1]) ||
	  info_lig_id (&info[idx]) != info_lig_id (&info[idx - 1]) ||
	  info_lig_comp (&info[idx]) != info_lig_comp (&info[idx - 1]) + 1);
}

static int mark_lig_accept (hb_buffer_t *buffer, unsigned idx)
{
  return !info_multiplied (&buffer->info[idx]) || 0 == info_lig_comp (&buffer->info[idx]);
}

static int find_last_base (ot_apply_t *c, const uint8_t *base_cov, int lig)
{
  hb_buffer_t *buffer = c->buffer;
  skippy_t *it = &c->iter_input;
  it->lookup_props = LOOKUP_IGNORE_MARKS;
  if (c->last_base_until > buffer->idx)
  {
    c->last_base_until = 0;
    c->last_base = -1;
  }
  for (unsigned j = buffer->idx; j > c->last_base_until; j--)
  {
    int m = skippy_match (c, it, &buffer->info[j - 1]);
    if (m == IT_MATCH && !(lig ? mark_lig_accept (buffer, j - 1) : mark_base_accept (buffer, j - 1)) &&
	NOT_COVERED == cov_get (base_cov, buffer->info[j - 1].codepoint))
      m = IT_SKIP;
    if (m == IT_MATCH)
    {
      c->last_base = (int) j - 1;
      break;
    }
  }
  c->last_base_until = buffer->idx;
  return c->last_base;
}

static int apply_mark_base (ot_apply_t *c, const uint8_t *st)
{
  hb_buffer_t *buffer = c->buffer;
  unsigned mark_index = cov_get (off16 (st, st + 2), buf_cur (buffer)->codepoint);
  if (mark_index == NOT_COVERED) return 0;
  const uint8_t *base_cov = off16 (st, st + 4);
  int last_base = find_last_base (c, base_cov, 0);
  if (last_base == -1) return 0;
  unsigned idx = (unsigned) last_base;
  unsigned base_index = cov_get (base_cov, buffer->info[idx].codepoint);
  if (base_index == NOT_COVERED) return 0;
  return mark_array_apply (c, off16 (st, st + 8), mark_index, base_index, off16 (st, st + 10), rd16 (st + 6), idx);
}

static int apply_mark_lig (ot_apply_t *c, const uint8_t *st)
{
  hb_buffer_t *buffer = c->buffer;
  unsigned mark_index = cov_get (off16 (st, st + 2), buf_cur (buffer)->codepoint);
  if (mark_index == NOT_COVERED) return 0;
  const uint8_t *lig_cov = off16 (st, st + 4);
  int last_base = find_last_base (c, lig_cov, 1);
  if (last_base == -1) return 0;
  unsigned idx = (unsigned) last_base;
  unsigned lig_index = cov_get (lig_cov, buffer->info[idx].codepoint);
  if (lig_index == NOT_COVERED) return 0;
  const uint8_t *lig_attach = arr_off16 (off16 (st, st + 10), off16 (st, st + 10), lig_index);
  unsigned comp_count = lig_attach ? rd16 (lig_attach) : 0;
  if (!comp_count) return 0;
  unsigned comp_index;
  unsigned lig_id = info_lig_id (&buffer->info[idx]);
  unsigned mark_id = info_lig_id (buf_cur (buffer));
  unsigned mark_comp = info_lig_comp (buf_cur (buffer));
  if (lig_id && lig_id == mark_id && mark_comp > 0) comp_index = HB_MIN (comp_count, mark_comp) - 1;
  else comp_index = comp_count - 1;
  return mark_array_apply (c, off16 (st, st + 8), mark_index, comp_index, lig_attach, rd16 (st + 6), idx);
}

static int apply_mark_mark (ot_apply_t *c, const uint8_t *st)
{
  hb_buffer_t *buffer = c->buffer;
  unsigned mark1_index = cov_get (off16 (st, st + 2), buf_cur (buffer)->codepoint);
  if (mark1_index == NOT_COVERED) return 0;
  skippy_t *it = &c->iter_input;
  it->idx = buffer->idx;
  it->lookup_props = c->lookup_props & ~(uint32_t) LOOKUP_IGNORE_FLAGS;
  if (!skippy_prev (c, it, NULL)) return 0;
  if (!info_is_mark (&buffer->info[it->idx])) return 0;
  unsigned j = it->idx;
  unsigned id1 = info_lig_id (buf_cur (buffer)), id2 = info_lig_id (&buffer->info[j]);
  unsigned comp1 = info_lig_comp (buf_cur (buffer)), comp2 = info_lig_comp (&buffer->info[j]);
  if (id1 == id2)
  {
    if (!(id1 == 0 || comp1 == comp2)) return 0;
  }
  else if (!((id1 > 0 && !comp1) || (id2 > 0 && !comp2)))
    return 0;
  unsigned mark2_index = cov_get (off16 (st, st + 4), buffer->info[j].codepoint);
  if (mark2_index == NOT_COVERED) return 0;
  return mark_array_apply (c, off16 (st, st + 8), mark1_index, mark2_index, off16 (st, st + 10), rd16 (st + 6), j);
}

static int apply_gpos (ot_apply_t *c, const uint8_t *st, unsigned type)
{
  hb_buffer_t *buffer = c->buffer;
  switch (type)
  {
  case 1:
  {
    unsigned index = cov_get (off16 (st, st + 2), buf_cur (buffer)->codepoint);
    if (index == NOT_COVERED) return 0;
    unsigned f = rd16 (st + 4);
    if (rd16 (st) == 1) apply_value (c, st, st + 6, f, &buffer->pos[buffer->idx]);
    else
    {
      if (index >= rd16 (st + 6)) return 0;
      apply_value (c, st, st + 8 + index * vf_len (f) * 2, f, &buffer->pos[buffer->idx]);
    }
    buffer->idx++;
    return 1;
  }
  case 2: return apply_pair (c, st);
  case 3: return apply_cursive (c, st);
  case 4: return apply_mark_base (c, st);
  case 5: return apply_mark_lig (c, st);
  case 6: return apply_mark_mark (c, st);
  case 7: return apply_context (c, st);
  case 8: return apply_chain_context (c, st);
  }
  return 0;
}

static int accel_apply (ot_apply_t *c, const laccel_t *a)
{
  if (a->count == 1)
    return c->table_index ? apply_gpos (c, a->sub[0].st, a->sub[0].type) : apply_gsub (c, a->sub[0].st, a->sub[0].type);
  hb_codepoint_t g = buf_cur (c->buffer)->codepoint;
  for (unsigned i = 0; i < a->count; i++)
    if (digest_may_have (&a->sub[i].digest, g) &&
	(c->table_index ? apply_gpos (c, a->sub[i].st, a->sub[i].type) : apply_gsub (c, a->sub[i].st, a->sub[i].type)))
      return 1;
  return 0;
}

static int recurse (ot_apply_t *c, unsigned lookup_index)
{
  if (c->nesting_level_left == 0)
  {
    c->buffer->successful = 0;
    return 0;
  }
  c->buffer->max_ops--;
  if (c->buffer->max_ops < 0)
  {
    c->buffer->successful = 0;
    return 0;
  }
  c->nesting_level_left--;
  unsigned saved_props = c->lookup_props, saved_index = c->lookup_index;
  c->lookup_index = lookup_index;
  set_lookup_props (c, lookup_props (gtab_lookup (c->g, lookup_index)));
  uint32_t stack[8], *saved_mp = c->mp;
  unsigned saved_len = c->mp_len;
  int saved_heap = c->mp_heap;
  c->mp = stack;
  c->mp_len = 8;
  c->mp_heap = 0;
  laccel_t *a = get_accel (c->g, c->table_index, lookup_index);
  int ret = a && accel_apply (c, a);
  c->lookup_index = saved_index;
  set_lookup_props (c, saved_props);
  if (c->mp_heap) free (c->mp);
  c->mp = saved_mp;
  c->mp_len = saved_len;
  c->mp_heap = saved_heap;
  c->nesting_level_left++;
  return ret;
}

static int lookup_is_reverse (const uint8_t *L)
{
  if (!L) return 0;
  unsigned type = rd16 (L);
  if (type == 7)
  {
    const uint8_t *st = arr_off16 (L, L + 4, 0);
    return ext_type (st) == 8;
  }
  return type == 8;
}

static int apply_forward (ot_apply_t *c, const laccel_t *a)
{
  int ret = 0;
  hb_buffer_t *buffer = c->buffer;
  while (buffer->successful)
  {
    hb_glyph_info_t *info = buffer->info;
    unsigned j = buffer->idx;
    while (j < buffer->len &&
	   !(digest_may_have (&a->digest, info[j].codepoint) &&
	     (info[j].mask & c->lookup_mask) &&
	     check_glyph_property (c, &info[j], c->lookup_props)))
      j++;
    if (j > buffer->idx && !buf_next_glyphs (buffer, j - buffer->idx)) break;
    if (buffer->idx >= buffer->len) break;
    if (accel_apply (c, a)) ret = 1;
    else buf_next_glyph (buffer);
  }
  return ret;
}

static int apply_backward (ot_apply_t *c, const laccel_t *a)
{
  int ret = 0;
  hb_buffer_t *buffer = c->buffer;
  do
  {
    hb_glyph_info_t *cur = buf_cur (buffer);
    if (digest_may_have (&a->digest, cur->codepoint) && (cur->mask & c->lookup_mask) && check_glyph_property (c, cur, c->lookup_props))
      ret |= accel_apply (c, a);
    buffer->idx--;
  }
  while ((int) buffer->idx >= 0);
  return ret;
}

typedef struct
{
  unsigned index;
  hb_mask_t mask;
  hb_tag_t feature_tag;
  uint8_t auto_zwnj, auto_zwj, random, per_syllable;
} lookup_map_t;

struct shape_plan_t;
typedef int (*pause_func_t) (const struct shape_plan_t *plan, hb_font_t *font, hb_buffer_t *buffer);

typedef struct
{
  unsigned last_lookup;
  pause_func_t pause_func;
} stage_map_t;

static void layout_apply (unsigned table_index, const struct shape_plan_t *plan, hb_font_t *font, hb_buffer_t *buffer,
			  const lookup_map_t *lookups, const stage_map_t *stages, unsigned stage_count)
{
  layout_t *l = face_layout (font->face);
  if (!l) return;
  ot_apply_t c;
  memset (&c, 0, sizeof c);
  uint32_t stack[8];
  c.table_index = table_index;
  c.font = font;
  c.buffer = buffer;
  c.l = l;
  c.g = &l->g[table_index];
  sc_init (&c.sanitizer, c.g->table, c.g->table ? (table_index ? font->face->gpos.len : font->face->gsub.len) : 0);
  c.var_store_cache = font_gdef_cache (font, l);
  c.direction = buffer->direction;
  c.lookup_mask = 1;
  c.lookup_index = (unsigned) -1;
  c.nesting_level_left = MAX_NESTING_LEVEL;
  c.has_glyph_classes = l->glyph_class_def != NULL;
  c.auto_zwnj = c.auto_zwj = 1;
  c.last_base = -1;
  c.mp = stack;
  c.mp_len = 8;
  init_iters (&c);
  unsigned i = 0;
  for (unsigned s = 0; s < stage_count; s++)
  {
    for (; i < stages[s].last_lookup; i++)
    {
      const lookup_map_t *lm = &lookups[i];
      laccel_t *a = get_accel (c.g, table_index, lm->index);
      if (!a) continue;
      if (!(digest_may_intersect (&a->digest, &buffer->digest) && digest_may_intersect (&a->digest_second, &buffer->digest))) continue;
      c.lookup_index = lm->index;
      c.lookup_mask = lm->mask;
      c.last_base = -1;
      c.last_base_until = 0;
      c.auto_zwj = lm->auto_zwj;
      c.auto_zwnj = lm->auto_zwnj;
      c.random = lm->random;
      c.per_syllable = lm->per_syllable;
      if (!buffer->len || !c.lookup_mask) continue;
      const uint8_t *L = gtab_lookup (c.g, lm->index);
      set_lookup_props (&c, lookup_props (L));
      if (table_index || !lookup_is_reverse (L))
      {
	if (!table_index) buf_clear_output (buffer);
	buffer->idx = 0;
	apply_forward (&c, a);
	if (!table_index) buf_sync (buffer);
      }
      else
      {
	buffer->idx = buffer->len - 1;
	apply_backward (&c, a);
      }
    }
    if (stages[s].pause_func && stages[s].pause_func (plan, font, buffer)) buf_update_digest (buffer);
  }
  if (c.mp_heap) free (c.mp);
}

static void layout_set_glyph_props (hb_font_t *font, hb_buffer_t *buffer)
{
  layout_t *l = face_layout (font->face);
  memset (&buffer->digest, 0, sizeof buffer->digest);
  for (unsigned i = 0; i < buffer->len; i++)
  {
    digest_add (&buffer->digest, buffer->info[i].codepoint);
    GPROPS (&buffer->info[i]) = l ? gdef_glyph_props (l, buffer->info[i].codepoint) : 0;
    info_clear_lig_props (&buffer->info[i]);
  }
}

static void position_finish_offsets (hb_font_t *font, hb_buffer_t *buffer);

static void propagate_attachment_offsets (hb_glyph_position_t *pos, unsigned len, unsigned i, int direction, unsigned nesting_level)
{
  int chain = ATTACH_CHAIN (&pos[i]), type = ATTACH_TYPE (&pos[i]);
  ATTACH_CHAIN (&pos[i]) = 0;
  unsigned j = (int) i + chain;
  if (j >= len) return;
  if (!nesting_level) return;
  if (ATTACH_CHAIN (&pos[j])) propagate_attachment_offsets (pos, len, j, direction, nesting_level - 1);
  if (type & ATTACH_TYPE_CURSIVE)
  {
    pos[i].y_offset = sat_add (pos[i].y_offset, pos[j].y_offset);
    return;
  }
  pos[i].x_offset = sat_add (pos[i].x_offset, pos[j].x_offset);
  if (j < i)
  {
    if (direction == DIR_LTR)
      for (unsigned k = j; k < i; k++)
      {
	pos[i].x_offset = sat_sub (pos[i].x_offset, pos[k].x_advance);
	pos[i].y_offset = sat_sub (pos[i].y_offset, pos[k].y_advance);
      }
    else
      for (unsigned k = j + 1; k < i + 1; k++)
      {
	pos[i].x_offset = sat_add (pos[i].x_offset, pos[k].x_advance);
	pos[i].y_offset = sat_add (pos[i].y_offset, pos[k].y_advance);
      }
  }
  else
  {
    if (direction == DIR_LTR)
      for (unsigned k = i; k < j; k++)
      {
	pos[i].x_offset = sat_add (pos[i].x_offset, pos[k].x_advance);
	pos[i].y_offset = sat_add (pos[i].y_offset, pos[k].y_advance);
      }
    else
      for (unsigned k = i + 1; k < j + 1; k++)
      {
	pos[i].x_offset = sat_sub (pos[i].x_offset, pos[k].x_advance);
	pos[i].y_offset = sat_sub (pos[i].y_offset, pos[k].y_advance);
      }
  }
}

static void position_finish_offsets (hb_font_t *font, hb_buffer_t *buffer)
{
  unsigned len = buffer->len;
  hb_glyph_position_t *pos = hb_buffer_get_glyph_positions (buffer, NULL);
  if (buffer->scratch_flags & SCRATCH_HAS_GPOS_ATTACHMENT)
  {
    if (buffer->direction == DIR_LTR)
    {
      for (unsigned i = 0; i < len; i++)
	if (ATTACH_CHAIN (&pos[i])) propagate_attachment_offsets (pos, len, i, buffer->direction, MAX_NESTING_LEVEL);
    }
    else
      for (unsigned i = len; i-- > 0;)
	if (ATTACH_CHAIN (&pos[i])) propagate_attachment_offsets (pos, len, i, buffer->direction, MAX_NESTING_LEVEL);
  }
  if (font->slant_xy)
    for (unsigned i = 0; i < len; i++)
      if (pos[i].y_offset)
      {
	double v = (double) pos[i].x_offset + (double) hbroundf (font->slant_xy * pos[i].y_offset);
	pos[i].x_offset = v >= INT32_MAX ? INT32_MAX : v <= INT32_MIN ? INT32_MIN : (hb_position_t) v;
      }
}

#define TAG_DFLT HB_TAG ('D','F','L','T')
#define TAG_dflt HB_TAG ('d','f','l','t')
#define TAG_latn HB_TAG ('l','a','t','n')
#define MAP_MAX_BITS 8u
#define MAP_MAX_VALUE ((1u << MAP_MAX_BITS) - 1u)

enum
{
  F_NONE = 0x0000,
  F_GLOBAL = 0x0001, /* has default value */
  F_HAS_FALLBACK = 0x0002,
  F_MANUAL_ZWNJ = 0x0004,
  F_MANUAL_ZWJ = 0x0008,
  F_MANUAL_JOINERS = F_MANUAL_ZWNJ | F_MANUAL_ZWJ,
  F_GLOBAL_MANUAL_JOINERS = F_GLOBAL | F_MANUAL_JOINERS,
  F_GLOBAL_HAS_FALLBACK = F_GLOBAL | F_HAS_FALLBACK,
  F_GLOBAL_SEARCH = 0x0010,
  F_RANDOM = 0x0020,
  F_PER_SYLLABLE = 0x0040
};

enum
{
  NORM_NONE,
  NORM_DECOMPOSED,
  NORM_COMPOSED_DIACRITICS,
  NORM_COMPOSED_DIACRITICS_NO_SHORT_CIRCUIT,
  NORM_AUTO
};

enum { ZERO_WIDTH_MARKS_NONE, ZERO_WIDTH_MARKS_BY_GDEF_EARLY, ZERO_WIDTH_MARKS_BY_GDEF_LATE };

#define NORMALIZER_GLYPH(i) ((i)->var1.u32)
#define SHAPER_CATEGORY(i) ((i)->var2.u8[2])
#define SHAPER_AUX(i) ((i)->var2.u8[3])

typedef struct shape_plan_t shape_plan_t;
typedef struct map_builder_t map_builder_t;

typedef struct
{
  void (*collect_features) (map_builder_t *map);
  void (*override_features) (map_builder_t *map);
  void *(*data_create) (const shape_plan_t *plan);
  void (*preprocess_text) (const shape_plan_t *plan, hb_buffer_t *buffer, hb_font_t *font);
  void (*postprocess_glyphs) (const shape_plan_t *plan, hb_buffer_t *buffer, hb_font_t *font);
  int (*decompose) (const shape_plan_t *plan, hb_codepoint_t ab, hb_codepoint_t *a, hb_codepoint_t *b);
  int (*compose) (const shape_plan_t *plan, hb_codepoint_t a, hb_codepoint_t b, hb_codepoint_t *ab);
  void (*setup_masks) (const shape_plan_t *plan, hb_buffer_t *buffer, hb_font_t *font);
  void (*reorder_marks) (const shape_plan_t *plan, hb_buffer_t *buffer, unsigned start, unsigned end);
  hb_tag_t gpos_tag;
  int normalization_preference;
  int zero_width_marks;
  int fallback_position;
} shaper_t;

typedef struct
{
  hb_tag_t tag;
  unsigned seq, max_value, flags, default_value, stage[2];
} feature_info_t;

typedef struct
{
  hb_tag_t tag;
  unsigned index[2], stage[2], shift;
  hb_mask_t mask, _1_mask;
  uint8_t needs_fallback, auto_zwnj, auto_zwj, random, per_syllable;
} feature_map_t;

typedef struct { unsigned index; pause_func_t pause_func; } stage_info_t;

DEFINE_VEC (fivec_t, feature_info_t)
DEFINE_VEC (fmvec_t, feature_map_t)
DEFINE_VEC (lmvec_t, lookup_map_t)
DEFINE_VEC (smvec_t, stage_map_t)
DEFINE_VEC (sivec_t, stage_info_t)

struct map_builder_t
{
  hb_face_t *face;
  layout_t *l;
  hb_script_t script;
  int direction;
  int is_simple;
  hb_tag_t chosen_script[2];
  int found_script[2];
  unsigned script_index[2], language_index[2];
  unsigned current_stage[2];
  fivec_t feature_infos;
  sivec_t stages[2];
};

struct shape_plan_t
{
  hb_script_t script;
  int direction;
  unsigned variations_index[2];
  const shaper_t *shaper;
  hb_mask_t global_mask;
  fmvec_t features;
  lmvec_t lookups[2];
  smvec_t stages[2];
  hb_tag_t chosen_script[2];
  int found_script[2];
  void *data;
  hb_mask_t rtlm_mask;
  int has_gpos_mark, zero_marks, fallback_glyph_classes, fallback_mark_positioning;
  int adjust_mark_positioning_when_zeroing, apply_gpos;
};

struct plan_node_t
{
  plan_node_t *next;
  shape_plan_t *plan;
};

static const uint8_t *gg_list (const gtab_t *g, unsigned off)
{
  return g->table && rd16 (g->table) == 1 ? off16 (g->table, g->table + off) : NULL;
}

static int tag_cmp (const void *key, const uint8_t *item, const void *ud)
{
  uint32_t a = *(const uint32_t *) key, b = rd32 (item);
  return (a > b) - (a < b);
}

static unsigned record_find (const uint8_t *list, hb_tag_t tag)
{
  if (!list) return NOT_FOUND_INDEX;
  int i = hb_bsearch (&tag, list + 2, rd16 (list), 6, tag_cmp, NULL);
  return i < 0 ? NOT_FOUND_INDEX : (unsigned) i;
}

static const uint8_t *record_get (const uint8_t *list, const uint8_t *base, unsigned i)
{
  return list && i < rd16 (list) ? off16 (base, list + 2 + i * 6 + 4) : NULL;
}

static const uint8_t *gg_script (const gtab_t *g, unsigned i)
{
  const uint8_t *sl = gg_list (g, 4);
  return record_get (sl, sl, i);
}

static const uint8_t *gg_lang_sys (const uint8_t *script, unsigned i)
{
  if (!script) return NULL;
  if (i == NOT_FOUND_INDEX) return off16 (script, script);
  return record_get (script + 2, script, i);
}

static hb_tag_t gg_feature_tag (const gtab_t *g, unsigned i)
{
  const uint8_t *fl = gg_list (g, 6);
  if (i == NOT_FOUND_INDEX || !fl || i >= rd16 (fl)) return 0;
  return rd32 (fl + 2 + i * 6);
}

static unsigned gg_feature_count (const gtab_t *g)
{
  const uint8_t *fl = gg_list (g, 6);
  return fl ? rd16 (fl) : 0;
}

static const uint8_t *gg_feature_variations (const gtab_t *g)
{
  return g->table && rd32 (g->table) >= 0x00010001u ? off32 (g->table, g->table + 10) : NULL;
}

static const uint8_t null_feature[4] = {0, 0, 0, 0};

static const uint8_t *gg_feature_variation (const gtab_t *g, unsigned feature_index, unsigned variations_index)
{
  const uint8_t *fv = gg_feature_variations (g);
  if (variations_index != NO_VARIATIONS_INDEX && g->table && rd32 (g->table) >= 0x00010001u)
  {
    const uint8_t *rec = fv && variations_index < rd32 (fv + 4) ? fv + 8 + variations_index * 8 : NULL;
    const uint8_t *fts = rec ? off32 (fv, rec + 4) : NULL;
    if (fts)
    {
      unsigned n = rd16 (fts + 4);
      for (unsigned i = 0; i < n; i++)
	if (rd16 (fts + 6 + i * 6) == feature_index)
	{
	  const uint8_t *f = off32 (fts, fts + 6 + i * 6 + 2);
	  return f ? f : null_feature;
	}
    }
  }
  const uint8_t *fl = gg_list (g, 6);
  return record_get (fl, fl, feature_index);
}

static int condition_eval (const uint8_t *c, hb_font_t *font, layout_t *l)
{
  if (!c) return 0;
  switch (rd16 (c))
  {
  case 1:
  {
    unsigned axis = rd16 (c + 2);
    int coord = axis < font->num_coords ? font->coords[axis] : 0;
    return rds16 (c + 4) <= coord && coord <= rds16 (c + 6);
  }
  case 2:
  {
    float value = rds16 (c + 2);
    uint32_t idx = rd32 (c + 4);
    if (font->num_coords && idx != 0xFFFFFFFFu) value += ivs_get_delta (l->ivs, idx, font->coords, font->num_coords, NULL);
    return value > 0;
  }
  case 3:
    for (unsigned i = 0; i < c[2]; i++)
      if (!condition_eval (rd24 (c + 3 + i * 3) ? c + rd24 (c + 3 + i * 3) : NULL, font, l)) return 0;
    return 1;
  case 4:
    for (unsigned i = 0; i < c[2]; i++)
      if (condition_eval (rd24 (c + 3 + i * 3) ? c + rd24 (c + 3 + i * 3) : NULL, font, l)) return 1;
    return 0;
  case 5:
    return !condition_eval (rd24 (c + 2) ? c + rd24 (c + 2) : NULL, font, l);
  default: return 0;
  }
}

static unsigned find_variations_index (const gtab_t *g, hb_font_t *font, layout_t *l)
{
  const uint8_t *fv = gg_feature_variations (g);
  if (!fv) return NO_VARIATIONS_INDEX;
  uint32_t n = rd32 (fv + 4);
  for (uint32_t i = 0; i < n; i++)
  {
    const uint8_t *cs = off32 (fv, fv + 8 + i * 8);
    unsigned count = cs ? rd16 (cs) : 0, ok = 1;
    for (unsigned k = 0; k < count && ok; k++) ok = condition_eval (off32 (cs, cs + 2 + k * 4), font, l);
    if (ok) return i;
  }
  return NO_VARIATIONS_INDEX;
}

static hb_tag_t old_tag_from_script (hb_script_t script)
{
  switch ((unsigned) script)
  {
  case HB_SCRIPT_INVALID: return TAG_DFLT;
  case HB_SCRIPT_MATH: return HB_TAG ('m','a','t','h');
  case HB_SCRIPT_HIRAGANA: return HB_TAG ('k','a','n','a');
  case HB_TAG ('H','r','k','t'): return HB_TAG ('k','a','n','a');
  case HB_SCRIPT_LAO: return HB_TAG ('l','a','o',' ');
  case HB_SCRIPT_YI: return HB_TAG ('y','i',' ',' ');
  case HB_SCRIPT_NKO: return HB_TAG ('n','k','o',' ');
  case HB_SCRIPT_VAI: return HB_TAG ('v','a','i',' ');
  }
  return ((hb_tag_t) script) | 0x20000000u;
}

static hb_tag_t new_tag_from_script (hb_script_t script)
{
  switch ((unsigned) script)
  {
  case HB_SCRIPT_BENGALI: return HB_TAG ('b','n','g','2');
  case HB_SCRIPT_DEVANAGARI: return HB_TAG ('d','e','v','2');
  case HB_SCRIPT_GUJARATI: return HB_TAG ('g','j','r','2');
  case HB_SCRIPT_GURMUKHI: return HB_TAG ('g','u','r','2');
  case HB_SCRIPT_KANNADA: return HB_TAG ('k','n','d','2');
  case HB_SCRIPT_MALAYALAM: return HB_TAG ('m','l','m','2');
  case HB_SCRIPT_ORIYA: return HB_TAG ('o','r','y','2');
  case HB_SCRIPT_TAMIL: return HB_TAG ('t','m','l','2');
  case HB_SCRIPT_TELUGU: return HB_TAG ('t','e','l','2');
  case HB_SCRIPT_MYANMAR: return HB_TAG ('m','y','m','2');
  }
  return TAG_DFLT;
}

static void map_builder_init (map_builder_t *m, hb_face_t *face, layout_t *l, hb_script_t script, int direction)
{
  memset (m, 0, sizeof (*m));
  m->face = face;
  m->l = l;
  m->script = script;
  m->direction = direction;
  hb_tag_t tags[3];
  unsigned count = 0;
  hb_tag_t new_tag = new_tag_from_script (script);
  if (new_tag != TAG_DFLT)
  {
    if (new_tag != HB_TAG ('m','y','m','2')) tags[count++] = new_tag | '3';
    tags[count++] = new_tag;
  }
  hb_tag_t old_tag = old_tag_from_script (script);
  if (old_tag != TAG_DFLT) tags[count++] = old_tag;
  for (unsigned ti = 0; ti < 2; ti++)
  {
    const gtab_t *g = &l->g[ti];
    const uint8_t *sl = gg_list (g, 4);
    unsigned idx = NOT_FOUND_INDEX;
    hb_tag_t chosen = 0;
    int found = 0;
    for (unsigned i = 0; i < count && !found; i++)
      if ((idx = record_find (sl, tags[i])) != NOT_FOUND_INDEX) chosen = tags[i], found = 1;
    if (!found)
    {
      static const hb_tag_t fallbacks[3] = {TAG_DFLT, TAG_dflt, TAG_latn};
      for (unsigned i = 0; i < 3; i++)
	if ((idx = record_find (sl, fallbacks[i])) != NOT_FOUND_INDEX) { chosen = fallbacks[i]; break; }
    }
    m->found_script[ti] = found;
    m->chosen_script[ti] = chosen;
    m->script_index[ti] = idx;
    const uint8_t *s = gg_script (g, idx);
    m->language_index[ti] = s ? record_find (s + 2, TAG_dflt) : NOT_FOUND_INDEX;
  }
}

static void map_add_feature (map_builder_t *m, hb_tag_t tag, unsigned flags, unsigned value)
{
  if (!tag) return;
  feature_info_t f;
  f.tag = tag;
  f.seq = m->feature_infos.len + 1;
  f.max_value = value;
  f.flags = flags;
  f.default_value = (flags & F_GLOBAL) ? value : 0;
  f.stage[0] = m->current_stage[0];
  f.stage[1] = m->current_stage[1];
  fivec_t_push (&m->feature_infos, f);
}

static void map_enable_feature (map_builder_t *m, hb_tag_t tag, unsigned flags, unsigned value) { map_add_feature (m, tag, F_GLOBAL | flags, value); }

static void map_add_pause (map_builder_t *m, unsigned ti, pause_func_t f)
{
  stage_info_t s = {m->current_stage[ti], f};
  sivec_t_push (&m->stages[ti], s);
  m->current_stage[ti]++;
}

static void map_add_gsub_pause (map_builder_t *m, pause_func_t f) { map_add_pause (m, 0, f); }

static int langsys_find_feature (const gtab_t *g, const uint8_t *ls, hb_tag_t tag, unsigned *feature_index)
{
  unsigned n = ls ? rd16 (ls + 4) : 0;
  for (unsigned i = 0; i < n; i++)
  {
    unsigned f = rd16 (ls + 6 + i * 2);
    if (tag == gg_feature_tag (g, f))
    {
      if (feature_index) *feature_index = f;
      return 1;
    }
  }
  return 0;
}

static int map_has_feature (map_builder_t *m, hb_tag_t tag)
{
  for (unsigned ti = 0; ti < 2; ti++)
    if (langsys_find_feature (&m->l->g[ti], gg_lang_sys (gg_script (&m->l->g[ti], m->script_index[ti]), m->language_index[ti]), tag, NULL))
      return 1;
  return 0;
}

static int feature_info_cmp (const feature_info_t *a, const feature_info_t *b)
{
  return (a->tag != b->tag) ? (a->tag < b->tag ? -1 : 1) : (a->seq < b->seq ? -1 : a->seq > b->seq ? 1 : 0);
}
static int feature_map_cmp (const feature_map_t *a, const feature_map_t *b) { return a->tag < b->tag ? -1 : a->tag > b->tag ? 1 : 0; }
static int lookup_map_cmp (const lookup_map_t *a, const lookup_map_t *b) { return a->index < b->index ? -1 : a->index > b->index ? 1 : 0; }

#define DEFINE_QSORT(name, T, cmp) \
static void name##_loop (T *base, size_t nel) \
{ \
  while (nel > 24) \
  { \
    T *last = base + nel - 1, *mid = base + nel / 2, t; \
    if (cmp (base, mid) > 0) { t = *base; *base = *mid; *mid = t; } \
    if (cmp (mid, last) > 0) \
    { \
      t = *mid; *mid = *last; *last = t; \
      if (cmp (base, mid) > 0) { t = *base; *base = *mid; *mid = t; } \
    } \
    t = *mid; *mid = *(last - 1); *(last - 1) = t; \
    T *pivot = last - 1, *i = base, *j = last - 1; \
    for (;;) \
    { \
      while (cmp (++i, pivot) < 0) {} \
      while (cmp (--j, pivot) > 0) {} \
      if (i >= j) break; \
      t = *i; *i = *j; *j = t; \
    } \
    t = *i; *i = *(last - 1); *(last - 1) = t; \
    size_t left = (size_t) (i - base), right = nel - left - 1; \
    if (left < right) { name##_loop (base, left); base = i + 1; nel = right; } \
    else { name##_loop (i + 1, right); nel = left; } \
  } \
} \
static void name (T *base, size_t nel) \
{ \
  name##_loop (base, nel); \
  for (T *pi = base + 1; pi < base + nel; pi++) \
    for (T *pj = pi; pj > base && cmp (pj - 1, pj) > 0; pj--) { T t = pj[-1]; pj[-1] = pj[0]; pj[0] = t; } \
}

DEFINE_QSORT (feature_infos_sort, feature_info_t, feature_info_cmp)
DEFINE_QSORT (feature_maps_sort, feature_map_t, feature_map_cmp)
DEFINE_QSORT (lookup_maps_sort, lookup_map_t, lookup_map_cmp)

static const feature_map_t *plan_feature (const shape_plan_t *p, hb_tag_t tag)
{
  int min = 0, max = (int) p->features.len - 1;
  while (min <= max)
  {
    int mid = ((unsigned) min + (unsigned) max) / 2;
    hb_tag_t t = p->features.a[mid].tag;
    if (tag < t) max = mid - 1;
    else if (tag > t) min = mid + 1;
    else return &p->features.a[mid];
  }
  return NULL;
}

static hb_mask_t plan_get_mask (const shape_plan_t *p, hb_tag_t tag, unsigned *shift)
{
  const feature_map_t *f = plan_feature (p, tag);
  if (shift) *shift = f ? f->shift : 0;
  return f ? f->mask : 0;
}
static hb_mask_t plan_get_1_mask (const shape_plan_t *p, hb_tag_t tag)
{
  const feature_map_t *f = plan_feature (p, tag);
  return f ? f->_1_mask : 0;
}

static void map_add_lookups (map_builder_t *m, shape_plan_t *p, unsigned ti, unsigned feature_index, unsigned variations_index,
			     hb_mask_t mask, int auto_zwnj, int auto_zwj, int random, int per_syllable, hb_tag_t feature_tag)
{
  const gtab_t *g = &m->l->g[ti];
  const uint8_t *f = gg_feature_variation (g, feature_index, variations_index);
  unsigned n = f ? rd16 (f + 2) : 0;
  for (unsigned i = 0; i < n; i++)
  {
    unsigned idx = rd16 (f + 4 + i * 2);
    if (idx >= g->lookup_count) continue;
    lookup_map_t lm = {idx, mask, feature_tag, (uint8_t) auto_zwnj, (uint8_t) auto_zwj, (uint8_t) random, (uint8_t) per_syllable};
    lmvec_t_push (&p->lookups[ti], lm);
  }
}

static void map_compile (map_builder_t *m, shape_plan_t *p)
{
  unsigned global_bit_shift = 31, global_bit_mask = 1u << global_bit_shift;
  p->global_mask = global_bit_mask;
  unsigned required_feature_index[2], required_feature_stage[2] = {0, 0};
  hb_tag_t required_feature_tag[2];
  const uint8_t *langsys[2];
  for (unsigned ti = 0; ti < 2; ti++)
  {
    p->chosen_script[ti] = m->chosen_script[ti];
    p->found_script[ti] = m->found_script[ti];
    langsys[ti] = gg_lang_sys (gg_script (&m->l->g[ti], m->script_index[ti]), m->language_index[ti]);
    unsigned rq = langsys[ti] ? rd16 (langsys[ti] + 2) : 0xFFFFu;
    required_feature_index[ti] = rq == 0xFFFFu ? NOT_FOUND_INDEX : rq;
    required_feature_tag[ti] = gg_feature_tag (&m->l->g[ti], required_feature_index[ti]);
  }
  fivec_t *fi = &m->feature_infos;
  if (fi->len)
  {
    if (!m->is_simple) feature_infos_sort (fi->a, fi->len);
    feature_info_t *f = fi->a;
    unsigned j = 0;
    for (unsigned i = 1; i < fi->len; i++)
      if (f[i].tag != f[j].tag) f[++j] = f[i];
      else
      {
	if (f[i].flags & F_GLOBAL)
	{
	  f[j].flags |= F_GLOBAL;
	  f[j].max_value = f[i].max_value;
	  f[j].default_value = f[i].default_value;
	}
	else
	{
	  if (f[j].flags & F_GLOBAL) f[j].flags ^= F_GLOBAL;
	  f[j].max_value = HB_MAX (f[j].max_value, f[i].max_value);
	}
	f[j].flags |= (f[i].flags & F_HAS_FALLBACK);
	f[j].stage[0] = HB_MIN (f[j].stage[0], f[i].stage[0]);
	f[j].stage[1] = HB_MIN (f[j].stage[1], f[i].stage[1]);
      }
    fi->len = j + 1;
  }
  unsigned next_bit = 4;
  for (unsigned i = 0; i < fi->len; i++)
  {
    const feature_info_t *info = &fi->a[i];
    unsigned bits_needed;
    if ((info->flags & F_GLOBAL) && info->max_value == 1) bits_needed = 0;
    else
    {
      unsigned bs = 0;
      for (unsigned v = info->max_value; v; v >>= 1) bs++;
      bits_needed = HB_MIN (MAP_MAX_BITS, bs);
    }
    if (!info->max_value || next_bit + bits_needed >= global_bit_shift) continue;
    int found = 0;
    unsigned feature_index[2];
    for (unsigned ti = 0; ti < 2; ti++)
    {
      if (required_feature_tag[ti] == info->tag) required_feature_stage[ti] = info->stage[ti];
      if (langsys_find_feature (&m->l->g[ti], langsys[ti], info->tag, &feature_index[ti])) found = 1;
      else feature_index[ti] = NO_FEATURE_INDEX;
    }
    if (!found && (info->flags & F_GLOBAL_SEARCH))
      for (unsigned ti = 0; ti < 2; ti++)
      {
	const gtab_t *g = &m->l->g[ti];
	unsigned n = gg_feature_count (g), k;
	feature_index[ti] = NO_FEATURE_INDEX;
	for (k = 0; k < n; k++)
	  if (gg_feature_tag (g, k) == info->tag) { feature_index[ti] = k; found = 1; break; }
      }
    if (!found && !(info->flags & F_HAS_FALLBACK)) continue;
    feature_map_t map;
    map.tag = info->tag;
    map.index[0] = feature_index[0];
    map.index[1] = feature_index[1];
    map.stage[0] = info->stage[0];
    map.stage[1] = info->stage[1];
    map.auto_zwnj = !(info->flags & F_MANUAL_ZWNJ);
    map.auto_zwj = !(info->flags & F_MANUAL_ZWJ);
    map.random = !!(info->flags & F_RANDOM);
    map.per_syllable = !!(info->flags & F_PER_SYLLABLE);
    if ((info->flags & F_GLOBAL) && info->max_value == 1)
    {
      map.shift = global_bit_shift;
      map.mask = global_bit_mask;
    }
    else
    {
      map.shift = next_bit;
      map.mask = (1u << (next_bit + bits_needed)) - (1u << next_bit);
      next_bit += bits_needed;
      p->global_mask |= (info->default_value << map.shift) & map.mask;
    }
    map._1_mask = (1u << map.shift) & map.mask;
    map.needs_fallback = !found;
    fmvec_t_push (&p->features, map);
  }
  if (m->is_simple) feature_maps_sort (p->features.a, p->features.len);
  map_add_pause (m, 0, NULL);
  map_add_pause (m, 1, NULL);
  for (unsigned ti = 0; ti < 2; ti++)
  {
    lmvec_t *lookups = &p->lookups[ti];
    unsigned stage_index = 0, last_num_lookups = 0;
    for (unsigned stage = 0; stage < m->current_stage[ti]; stage++)
    {
      if (required_feature_index[ti] != NO_FEATURE_INDEX && required_feature_stage[ti] == stage)
	map_add_lookups (m, p, ti, required_feature_index[ti], p->variations_index[ti], global_bit_mask, 1, 1, 0, 0, HB_TAG (' ',' ',' ',' '));
      for (unsigned k = 0; k < p->features.len; k++)
      {
	const feature_map_t *f = &p->features.a[k];
	if (f->stage[ti] == stage)
	  map_add_lookups (m, p, ti, f->index[ti], p->variations_index[ti], f->mask, f->auto_zwnj, f->auto_zwj, f->random, f->per_syllable, f->tag);
      }
      if (last_num_lookups + 1 < lookups->len)
      {
	lookup_maps_sort (lookups->a + last_num_lookups, lookups->len - last_num_lookups);
	unsigned j = last_num_lookups;
	for (unsigned i = j + 1; i < lookups->len; i++)
	  if (lookups->a[i].index != lookups->a[j].index) lookups->a[++j] = lookups->a[i];
	  else
	  {
	    lookups->a[j].mask |= lookups->a[i].mask;
	    lookups->a[j].auto_zwnj &= lookups->a[i].auto_zwnj;
	    lookups->a[j].auto_zwj &= lookups->a[i].auto_zwj;
	  }
	lookups->len = j + 1;
      }
      last_num_lookups = lookups->len;
      if (stage_index < m->stages[ti].len && m->stages[ti].a[stage_index].index == stage)
      {
	stage_map_t sm = {last_num_lookups, m->stages[ti].a[stage_index].pause_func};
	smvec_t_push (&p->stages[ti], sm);
	stage_index++;
      }
    }
  }
}

static int font_has_glyph (hb_font_t *font, hb_codepoint_t u)
{
  hb_codepoint_t g = 0;
  return cmap_get_nominal (font->face, u, &g);
}

static int nominal_glyph (hb_font_t *font, hb_codepoint_t u, hb_codepoint_t *g)
{
  *g = 0;
  return cmap_get_nominal (font->face, u, g);
}

static int norm_decompose (const shape_plan_t *plan, hb_codepoint_t ab, hb_codepoint_t *a, hb_codepoint_t *b)
{
  return plan->shaper->decompose ? plan->shaper->decompose (plan, ab, a, b) : uni_decompose (ab, a, b);
}

static int norm_compose (const shape_plan_t *plan, hb_codepoint_t a, hb_codepoint_t b, hb_codepoint_t *ab)
{
  return plan->shaper->compose ? plan->shaper->compose (plan, a, b, ab) : uni_compose (a, b, ab);
}

static void output_char (hb_buffer_t *buffer, hb_codepoint_t unichar, hb_codepoint_t glyph)
{
  NORMALIZER_GLYPH (buf_cur (buffer)) = glyph;
  buf_output_glyph (buffer, unichar);
  info_set_unicode_props (buf_prev (buffer), buffer);
}

static void next_char (hb_buffer_t *buffer, hb_codepoint_t glyph)
{
  NORMALIZER_GLYPH (buf_cur (buffer)) = glyph;
  buf_next_glyph (buffer);
}

static unsigned decompose (const shape_plan_t *plan, hb_buffer_t *buffer, hb_font_t *font, int shortest, hb_codepoint_t ab)
{
  hb_codepoint_t a = 0, b = 0, a_glyph = 0, b_glyph = 0;
  if (!norm_decompose (plan, ab, &a, &b) || (b && !nominal_glyph (font, b, &b_glyph))) return 0;
  int has_a = nominal_glyph (font, a, &a_glyph);
  if (shortest && has_a)
  {
    output_char (buffer, a, a_glyph);
    if (b)
    {
      output_char (buffer, b, b_glyph);
      return 2;
    }
    return 1;
  }
  unsigned ret = decompose (plan, buffer, font, shortest, a);
  if (ret)
  {
    if (b)
    {
      output_char (buffer, b, b_glyph);
      return ret + 1;
    }
    return ret;
  }
  if (has_a)
  {
    output_char (buffer, a, a_glyph);
    if (b)
    {
      output_char (buffer, b, b_glyph);
      return 2;
    }
    return 1;
  }
  return 0;
}

static void decompose_current_character (const shape_plan_t *plan, hb_buffer_t *buffer, hb_font_t *font, int shortest)
{
  hb_codepoint_t u = buf_cur (buffer)->codepoint, glyph = 0;
  if (shortest && nominal_glyph (font, u, &glyph))
  {
    next_char (buffer, glyph);
    return;
  }
  if (decompose (plan, buffer, font, shortest, u))
  {
    buf_skip_glyph (buffer);
    return;
  }
  if (!shortest && nominal_glyph (font, u, &glyph))
  {
    next_char (buffer, glyph);
    return;
  }
  if (info_is_unicode_space (buf_cur (buffer)))
  {
    hb_codepoint_t space_glyph;
    unsigned space_type = uni_space_fallback_type (u);
    if (space_type != NOT_SPACE && nominal_glyph (font, 0x0020, &space_glyph))
    {
      info_set_space_fallback_type (buf_cur (buffer), space_type);
      next_char (buffer, space_glyph);
      buffer->scratch_flags |= SCRATCH_HAS_SPACE_FALLBACK;
      return;
    }
  }
  if (u == 0x2011u)
  {
    hb_codepoint_t other_glyph;
    if (nominal_glyph (font, 0x2010u, &other_glyph))
    {
      next_char (buffer, other_glyph);
      return;
    }
  }
  next_char (buffer, glyph);
}

static void set_glyph (hb_glyph_info_t *info, hb_font_t *font)
{
  hb_codepoint_t g;
  nominal_glyph (font, info->codepoint, &g);
  NORMALIZER_GLYPH (info) = g;
}

static void handle_variation_selector_cluster (hb_buffer_t *buffer, hb_font_t *font, unsigned end)
{
  for (; buffer->idx < end - 1 && buffer->successful;)
  {
    if (uni_is_variation_selector (buffer->info[buffer->idx + 1].codepoint))
    {
      hb_codepoint_t g = 0;
      if (cmap_get_variation (font->face, buf_cur (buffer)->codepoint, buffer->info[buffer->idx + 1].codepoint, &g))
      {
	NORMALIZER_GLYPH (buf_cur (buffer)) = g;
	hb_codepoint_t unicode = buf_cur (buffer)->codepoint;
	buf_replace_glyphs (buffer, 2, 1, &unicode);
      }
      else
      {
	NORMALIZER_GLYPH (buf_cur (buffer)) = g;
	set_glyph (buf_cur (buffer), font);
	buf_next_glyph (buffer);
	buffer->scratch_flags |= SCRATCH_HAS_VARIATION_SELECTOR_FALLBACK;
	info_set_variation_selector (buf_cur (buffer), 1);
	set_glyph (buf_cur (buffer), font);
	buf_next_glyph (buffer);
      }
      while (buffer->idx < end && buffer->successful && uni_is_variation_selector (buf_cur (buffer)->codepoint))
      {
	set_glyph (buf_cur (buffer), font);
	buf_next_glyph (buffer);
      }
    }
    else
    {
      set_glyph (buf_cur (buffer), font);
      buf_next_glyph (buffer);
    }
  }
  if (buffer->idx < end)
  {
    set_glyph (buf_cur (buffer), font);
    buf_next_glyph (buffer);
  }
}

static int compare_combining_class (const hb_glyph_info_t *pa, const hb_glyph_info_t *pb)
{
  unsigned a = info_cc (pa), b = info_cc (pb);
  return a < b ? -1 : a == b ? 0 : 1;
}

static void normalize (const shape_plan_t *plan, hb_buffer_t *buffer, hb_font_t *font)
{
  if (!buffer->len) return;
  int mode = plan->shaper->normalization_preference;
  if (mode == NORM_AUTO) mode = NORM_COMPOSED_DIACRITICS;
  int always_short_circuit = mode == NORM_NONE;
  int might_short_circuit = always_short_circuit || (mode != NORM_DECOMPOSED && mode != NORM_COMPOSED_DIACRITICS_NO_SHORT_CIRCUIT);
  unsigned count;
  int all_simple = 1;

  buf_clear_output (buffer);
  count = buffer->len;
  buffer->idx = 0;
  do
  {
    unsigned end;
    for (end = buffer->idx + 1; end < count; end++)
      if (info_is_unicode_mark (&buffer->info[end])) break;
    if (end < count) end--;
    if (might_short_circuit)
    {
      unsigned done;
      for (done = 0; buffer->idx + done < end; done++)
      {
	hb_glyph_info_t *in = &buffer->info[buffer->idx + done];
	hb_codepoint_t g;
	if (!cmap_get_nominal (font->face, in->codepoint, &g)) break;
	NORMALIZER_GLYPH (in) = g;
      }
      if (!buf_next_glyphs (buffer, done)) break;
    }
    while (buffer->idx < end && buffer->successful) decompose_current_character (plan, buffer, font, might_short_circuit);
    if (buffer->idx == count || !buffer->successful) break;
    all_simple = 0;
    for (end = buffer->idx + 1; end < count; end++)
      if (!info_is_unicode_mark (&buffer->info[end])) break;
    int has_vs = 0;
    for (unsigned i = buffer->idx; i < end && buffer->successful; i++)
      if (uni_is_variation_selector (buffer->info[i].codepoint)) { has_vs = 1; break; }
    if (has_vs) handle_variation_selector_cluster (buffer, font, end);
    else
      while (buffer->idx < end && buffer->successful) decompose_current_character (plan, buffer, font, always_short_circuit);
  }
  while (buffer->idx < count && buffer->successful);
  buf_sync (buffer);

  if (!all_simple)
  {
    count = buffer->len;
    hb_glyph_info_t *info = buffer->info;
    for (unsigned i = 0; i < count; i++)
    {
      if (info_cc (&info[i]) == 0) continue;
      unsigned end;
      for (end = i + 1; end < count; end++)
	if (info_cc (&info[end]) == 0) break;
      if (end - i > 32)
      {
	i = end;
	continue;
      }
      buf_sort (buffer, i, end, compare_combining_class);
      if (plan->shaper->reorder_marks) plan->shaper->reorder_marks (plan, buffer, i, end);
      i = end;
    }
  }
  if (buffer->scratch_flags & SCRATCH_HAS_CGJ)
  {
    hb_glyph_info_t *info = buffer->info;
    for (unsigned i = 1; i + 1 < buffer->len; i++)
      if (info[i].codepoint == 0x034Fu && (info_cc (&info[i + 1]) == 0 || info_cc (&info[i - 1]) <= info_cc (&info[i + 1])))
	info_unhide (&info[i]);
  }
  if (!all_simple && buffer->successful && (mode == NORM_COMPOSED_DIACRITICS || mode == NORM_COMPOSED_DIACRITICS_NO_SHORT_CIRCUIT))
  {
    buf_clear_output (buffer);
    count = buffer->len;
    unsigned starter = 0;
    buf_next_glyph (buffer);
    while (buffer->idx < count)
    {
      hb_codepoint_t composed, glyph;
      if (info_is_unicode_mark (buf_cur (buffer)) &&
	  (starter == buffer->out_len - 1 || info_cc (buf_prev (buffer)) < info_cc (buf_cur (buffer))) &&
	  norm_compose (plan, buffer->out_info[starter].codepoint, buf_cur (buffer)->codepoint, &composed) &&
	  nominal_glyph (font, composed, &glyph))
      {
	if (!buf_next_glyph (buffer)) break;
	buf_merge_out_clusters (buffer, starter, buffer->out_len);
	buffer->out_len--;
	buffer->out_info[starter].codepoint = composed;
	NORMALIZER_GLYPH (&buffer->out_info[starter]) = glyph;
	info_set_unicode_props (&buffer->out_info[starter], buffer);
	continue;
      }
      if (!buf_next_glyph (buffer)) break;
      if (info_cc (buf_prev (buffer)) == 0) starter = buffer->out_len - 1;
    }
    buf_sync (buffer);
  }
}

static const uint8_t _hb_arabic_joining_u8[749]=
{
    0, 16,  2,  0,  0,  0,  3,  0,  4,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  5,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0, 96,135,  9,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 10,  0,  0,  0,  0,  0,
    0,  0,  0,  8, 16, 24, 32, 40, 48,  0, 56,  0, 64, 72, 80,  0,
    0,  0,  0, 88, 96,104,  0,  0,  0,  0,  0,112,120,  0,  0,  0,
    0,  0,  0,  0,128,  0,  0,  0,  0,  0,  0,  0,  0,  0,136,  0,
    0,144,  0,  0,  0,  0,  0,152,  0,  0,  0,  0,  0,  0,160,168,
  176,184,192,  0,  0,200,208,  0,  0,  0,  0,  0,  0,  0,  0,216,
  224,  0,  0,  0,  0,  0,  0,  0,  0,  8,  9,  0,  0, 10, 11, 12,
    1,  1, 13,  0,  0,  0, 14, 15,  1,  1,  2,  2,  4,  1,  1,  1,
    1, 16, 17, 18,  3,  0, 19,  0, 20,  0, 21, 22, 23,  1, 24,  0,
    0,  0, 25,  1, 26,  1, 27, 28,  4,  0, 29,  1,  1,  1, 30,  0,
   31, 32,  5, 33, 34, 35, 36,  2,  2, 37, 38,  6,  0,  1, 39, 40,
    5,  1,  7,  0,  0, 41,  0,  0,  0, 42, 43,  0,  0,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  7, 44,  1,  1,  1,  1, 45,  0,
    0,  0, 46,  0,  0,  0, 47,  0,  0,  0,  0,  0,  0, 48,  6,  0,
    0,  1,  1,  1,  1,  1,  1, 49,  0, 50, 51, 52, 53, 54, 55,  0,
    0, 56, 57, 58,  0,  0, 59,  0,  0, 60,  1,  1,  1, 61,  0,  0,
    0, 62,  0,  0, 63, 64, 65,  0,  0,  0,  0,  0,  0,  0,  0, 66,
    1, 67,  0, 68,  0,  0,  0, 69,  1, 70,  0,  0,  0,  0,  0, 71,
   72, 73, 74,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    3,  0,  3,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,
    1, 75, 76,  0,  0,  0,  0,  0,  0,119,119,119,119, 51, 51, 51,
   51, 34, 34, 34, 34,119,119,  7,119, 34, 51, 51, 51, 35, 51, 51,
   51,  0,119,119,119,115,119,119,119,  0,  0,  0,119,112,  7,119,
  119,  3, 34, 34, 35, 35, 51, 51, 35, 34, 50, 51, 51, 50,115,119,
  119,119,119,119, 51, 39, 34, 32, 34, 50, 35, 34, 34, 34, 34, 35,
   35, 51, 34, 39,119,119,119,119, 34,119, 51,115, 55,119,119,119,
  103,116, 51, 83, 37, 34, 51, 51, 50, 50, 53, 50, 83,119,119, 39,
   51, 35, 34, 51, 51, 51, 35, 50, 51, 35, 35, 50, 51,119, 51, 51,
   51, 51,115,119,119,119,115,119,119, 50, 51, 51, 34, 51, 51, 50,
   34,114,119,119,119,  3, 51, 51, 32, 35,114,119,119, 34, 50, 51,
    3, 48, 51, 51, 50, 51, 34,  2, 50, 35, 50, 51, 51,119,112,119,
  119,119,119,119, 48,119,115,119,112,  0,  0, 96, 54,115,115,119,
  119,119,119, 48,119,119,119,119,  7,119,119,119,  0, 51,  1,119,
  119, 51, 51, 35, 32, 32,  2, 16, 34, 34, 50, 51, 19, 51, 51, 35,
   51, 35,  0,114,119,119, 55, 51, 35, 35, 35, 34, 51, 35, 51, 50,
   34, 35,119,119,119, 39, 34, 50,  3, 49, 51, 51, 51, 51, 50,119,
  119,119, 50,115, 51, 23, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
  113, 51, 35, 51, 51, 51, 51,  3,119, 55, 51,114,119, 51, 51, 34,
   51, 51,119,119,119,  3, 51, 34,  2, 35, 50, 35, 51, 48, 34,  3,
    0, 32, 19,119,119, 51, 51,119,119,119,103,119,119,
};

static inline uint8_t _hb_arabic_joining_b4 (const uint8_t* a, unsigned i)
{
  return (a[i>>1]>>((i&1)<<2))&15;
}
static inline uint8_t arabic_joining_type (unsigned u)
{

  return u<125260u ? (uint8_t)(_hb_arabic_joining_b4(_hb_arabic_joining_u8+441u,((_hb_arabic_joining_u8[209u+_hb_arabic_joining_u8[123u+((_hb_arabic_joining_b4(_hb_arabic_joining_u8,((((((u)>>3))>>3))>>3)))<<3)+((((((u)>>3))>>3))&7)]+((((u)>>3))&7)])<<3)+((u)&7))) : 7;
}

enum { JOINING_TYPE_U, JOINING_TYPE_L, JOINING_TYPE_R, JOINING_TYPE_D, JOINING_GROUP_ALAPH, JOINING_GROUP_DALATH_RISH, JOINING_TYPE_T = 6, JOINING_TYPE_X = 7 };
enum { ISOL, FINA, FIN2, FIN3, MEDI, MED2, INIT, NONE, ARABIC_NUM_FEATURES = NONE, STCH_FIXED, STCH_REPEATING };

static const hb_tag_t arabic_features[] =
{
  HB_TAG ('i','s','o','l'), HB_TAG ('f','i','n','a'), HB_TAG ('f','i','n','2'), HB_TAG ('f','i','n','3'),
  HB_TAG ('m','e','d','i'), HB_TAG ('m','e','d','2'), HB_TAG ('i','n','i','t')
};

static const struct { uint8_t prev_action, curr_action, next_state; } arabic_state_table[7][6] =
{
  { {NONE,NONE,0}, {NONE,ISOL,2}, {NONE,ISOL,1}, {NONE,ISOL,2}, {NONE,ISOL,1}, {NONE,ISOL,6}, },
  { {NONE,NONE,0}, {NONE,ISOL,2}, {NONE,ISOL,1}, {NONE,ISOL,2}, {NONE,FIN2,5}, {NONE,ISOL,6}, },
  { {NONE,NONE,0}, {NONE,ISOL,2}, {INIT,FINA,1}, {INIT,FINA,3}, {INIT,FINA,4}, {INIT,FINA,6}, },
  { {NONE,NONE,0}, {NONE,ISOL,2}, {MEDI,FINA,1}, {MEDI,FINA,3}, {MEDI,FINA,4}, {MEDI,FINA,6}, },
  { {NONE,NONE,0}, {NONE,ISOL,2}, {MED2,ISOL,1}, {MED2,ISOL,2}, {MED2,FIN2,5}, {MED2,ISOL,6}, },
  { {NONE,NONE,0}, {NONE,ISOL,2}, {ISOL,ISOL,1}, {ISOL,ISOL,2}, {ISOL,FIN2,5}, {ISOL,ISOL,6}, },
  { {NONE,NONE,0}, {NONE,ISOL,2}, {NONE,ISOL,1}, {NONE,ISOL,2}, {NONE,FIN3,5}, {NONE,ISOL,6}, }
};

typedef struct { hb_mask_t mask_array[ARABIC_NUM_FEATURES + 1]; int has_stch; } arabic_plan_t;

static unsigned get_joining_type (hb_codepoint_t u, unsigned gen_cat)
{
  unsigned j = arabic_joining_type (u);
  if (j != JOINING_TYPE_X) return j;
  return (FLAG_UNSAFE (gen_cat) & (FLAG (GC_NON_SPACING_MARK) | FLAG (GC_ENCLOSING_MARK) | FLAG (GC_FORMAT))) ? JOINING_TYPE_T : JOINING_TYPE_U;
}

static int record_stch (const shape_plan_t *plan, hb_font_t *font, hb_buffer_t *buffer)
{
  const arabic_plan_t *ap = (const arabic_plan_t *) plan->data;
  if (!ap->has_stch) return 0;
  for (unsigned i = 0; i < buffer->len; i++)
    if (info_multiplied (&buffer->info[i]))
    {
      SHAPER_AUX (&buffer->info[i]) = info_lig_comp (&buffer->info[i]) % 2 ? STCH_REPEATING : STCH_FIXED;
      buffer->scratch_flags |= SCRATCH_SHAPER0;
    }
  return 0;
}

static void collect_features_arabic (map_builder_t *m)
{
  map_enable_feature (m, HB_TAG ('s','t','c','h'), 0, 1);
  map_add_gsub_pause (m, record_stch);
  map_enable_feature (m, HB_TAG ('c','c','m','p'), F_MANUAL_ZWJ, 1);
  map_enable_feature (m, HB_TAG ('l','o','c','l'), F_MANUAL_ZWJ, 1);
  map_add_gsub_pause (m, NULL);
  for (unsigned i = 0; i < ARABIC_NUM_FEATURES; i++)
  {
    unsigned c = arabic_features[i] & 0xFF;
    int has_fallback = m->script == HB_SCRIPT_ARABIC && !(c >= '2' && c <= '3');
    map_add_feature (m, arabic_features[i], F_MANUAL_ZWJ | (has_fallback ? F_HAS_FALLBACK : F_NONE), 1);
    map_add_gsub_pause (m, NULL);
  }
  map_add_gsub_pause (m, NULL);
  map_enable_feature (m, HB_TAG ('r','l','i','g'), F_MANUAL_ZWJ | F_HAS_FALLBACK, 1);
  if (m->script == HB_SCRIPT_ARABIC) map_add_gsub_pause (m, NULL);
  map_enable_feature (m, HB_TAG ('c','a','l','t'), F_MANUAL_ZWJ, 1);
  if (!map_has_feature (m, HB_TAG ('r','c','l','t'))) map_add_gsub_pause (m, NULL);
  map_enable_feature (m, HB_TAG ('l','i','g','a'), F_MANUAL_ZWJ, 1);
  map_enable_feature (m, HB_TAG ('c','l','i','g'), F_MANUAL_ZWJ, 1);
  map_enable_feature (m, HB_TAG ('m','s','e','t'), F_MANUAL_ZWJ, 1);
}

static void *data_create_arabic (const shape_plan_t *plan)
{
  arabic_plan_t *ap = (arabic_plan_t *) calloc (1, sizeof (arabic_plan_t));
  if (!ap) return NULL;
  ap->has_stch = !!plan_get_1_mask (plan, HB_TAG ('s','t','c','h'));
  for (unsigned i = 0; i < ARABIC_NUM_FEATURES; i++) ap->mask_array[i] = plan_get_1_mask (plan, arabic_features[i]);
  return ap;
}

static void setup_masks_arabic (const shape_plan_t *plan, hb_buffer_t *buffer, hb_font_t *font)
{
  const arabic_plan_t *ap = (const arabic_plan_t *) plan->data;
  hb_glyph_info_t *info = buffer->info;
  unsigned count = buffer->len, prev = UINT32_MAX, state = 0;
  for (unsigned i = 0; i < buffer->context_len[0]; i++)
  {
    unsigned t = get_joining_type (buffer->context[0][i], uni_gc (buffer->context[0][i]));
    if (t == JOINING_TYPE_T) continue;
    state = arabic_state_table[state][t].next_state;
    break;
  }
  for (unsigned i = 0; i < count; i++)
  {
    unsigned t = get_joining_type (info[i].codepoint, info_gc (&info[i]));
    if (t == JOINING_TYPE_T)
    {
      SHAPER_AUX (&info[i]) = NONE;
      continue;
    }
    if (arabic_state_table[state][t].prev_action != NONE && prev != UINT32_MAX)
    {
      SHAPER_AUX (&info[prev]) = arabic_state_table[state][t].prev_action;
      buf_unsafe_to_break (buffer, prev, i + 1);
    }
    SHAPER_AUX (&info[i]) = arabic_state_table[state][t].curr_action;
    prev = i;
    state = arabic_state_table[state][t].next_state;
  }
  for (unsigned i = 0; i < buffer->context_len[1]; i++)
  {
    unsigned t = get_joining_type (buffer->context[1][i], uni_gc (buffer->context[1][i]));
    if (t == JOINING_TYPE_T) continue;
    if (arabic_state_table[state][t].prev_action != NONE && prev != UINT32_MAX)
    {
      SHAPER_AUX (&info[prev]) = arabic_state_table[state][t].prev_action;
      buf_unsafe_to_break (buffer, prev, buffer->len);
    }
    break;
  }
  for (unsigned i = 0; i < count; i++) info[i].mask |= ap->mask_array[SHAPER_AUX (&info[i])];
}

#define ARABIC_GC_IS_WORD(gc) (FLAG_UNSAFE (gc) & (FLAG (GC_UNASSIGNED) | FLAG (GC_PRIVATE_USE) | FLAG (GC_MODIFIER_LETTER) | \
  FLAG (GC_OTHER_LETTER) | FLAG (GC_SPACING_MARK) | FLAG (GC_ENCLOSING_MARK) | FLAG (GC_NON_SPACING_MARK) | FLAG (GC_DECIMAL_NUMBER) | \
  FLAG (GC_LETTER_NUMBER) | FLAG (GC_OTHER_NUMBER) | FLAG (GC_CURRENCY_SYMBOL) | FLAG (GC_MODIFIER_SYMBOL) | FLAG (GC_MATH_SYMBOL) | \
  FLAG (GC_OTHER_SYMBOL)))

static int is_stch (const hb_glyph_info_t *i) { return SHAPER_AUX (i) == STCH_FIXED || SHAPER_AUX (i) == STCH_REPEATING; }

static void postprocess_glyphs_arabic (const shape_plan_t *plan, hb_buffer_t *buffer, hb_font_t *font)
{
  if (!(buffer->scratch_flags & SCRATCH_SHAPER0)) return;
  int rtl = buffer->direction == DIR_RTL;
  if (!rtl) buf_reverse (buffer);
  int sign = font->x_scale < 0 ? -1 : +1;
  unsigned extra_glyphs_needed = 0;
  for (int step = 0; step <= 1; step++)
  {
    unsigned count = buffer->len;
    hb_glyph_info_t *info = buffer->info;
    hb_glyph_position_t *pos = buffer->pos;
    unsigned new_len = count + extra_glyphs_needed, j = new_len;
    for (unsigned i = count; i; i--)
    {
      if (!is_stch (&info[i - 1]))
      {
	if (step == 1)
	{
	  --j;
	  info[j] = info[i - 1];
	  pos[j] = pos[i - 1];
	}
	continue;
      }
      hb_position_t w_total = 0, w_fixed = 0, w_repeating = 0;
      int n_fixed = 0, n_repeating = 0;
      unsigned end = i;
      while (i && is_stch (&info[i - 1]))
      {
	i--;
	hb_position_t width = hb_font_get_glyph_h_advance (font, info[i].codepoint);
	if (SHAPER_AUX (&info[i]) == STCH_FIXED) { w_fixed = sat_add (w_fixed, width); n_fixed++; }
	else { w_repeating = sat_add (w_repeating, width); n_repeating++; }
      }
      unsigned start = i, context = i;
      while (context && !is_stch (&info[context - 1]) &&
	     (info_is_default_ignorable (&info[context - 1]) || ARABIC_GC_IS_WORD (info_gc (&info[context - 1]))))
      {
	context--;
	w_total = sat_add (w_total, pos[context].x_advance);
      }
      i++;
      unsigned n_copies = 0;
      int64_t w_remaining_signed = (int64_t) w_total - w_fixed, w_repeating_signed = w_repeating;
      if (sign < 0)
      {
	w_remaining_signed = -w_remaining_signed;
	w_repeating_signed = -w_repeating_signed;
      }
      hb_position_t w_remaining = sat_sub (w_total, w_fixed);
      if (w_remaining_signed > w_repeating_signed && w_repeating_signed > 0)
	n_copies = w_remaining_signed / w_repeating_signed - 1;
      hb_position_t extra_repeat_overlap = 0;
      int64_t shortfall = w_remaining_signed - w_repeating_signed * (n_copies + 1);
      if (shortfall > 0 && n_repeating > 0)
      {
	++n_copies;
	int64_t excess = (n_copies + 1) * w_repeating_signed - w_remaining_signed;
	if (excess > 0)
	{
	  int64_t v = excess / ((int64_t) n_copies * (int64_t) n_repeating);
	  extra_repeat_overlap = v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (hb_position_t) v;
	  w_remaining = 0;
	}
      }
      unsigned max_copies = 0;
      if (n_repeating > 0)
      {
	unsigned base_glyphs = n_fixed + n_repeating;
	if (base_glyphs < 256) max_copies = (256 - base_glyphs) / n_repeating;
      }
      n_copies = HB_MIN (n_copies, max_copies);
      if (step == 0)
      {
	uint64_t added = (uint64_t) n_copies * n_repeating;
	if (added > UINT32_MAX || (uint64_t) extra_glyphs_needed + added > UINT32_MAX)
	{
	  extra_glyphs_needed = UINT32_MAX;
	  break;
	}
	extra_glyphs_needed += (unsigned) added;
      }
      else
      {
	buf_unsafe_to_break (buffer, context, end);
	hb_position_t x_offset = w_remaining / 2;
	for (unsigned k = end; k > start; k--)
	{
	  hb_position_t width = hb_font_get_glyph_h_advance (font, info[k - 1].codepoint);
	  unsigned repeat = 1;
	  if (SHAPER_AUX (&info[k - 1]) == STCH_REPEATING) repeat += n_copies;
	  pos[k - 1].x_advance = 0;
	  for (unsigned n = 0; n < repeat; n++)
	  {
	    if (rtl)
	    {
	      x_offset = sat_sub (x_offset, width);
	      if (n > 0) x_offset = sat_add (x_offset, extra_repeat_overlap);
	    }
	    pos[k - 1].x_offset = x_offset;
	    --j;
	    info[j] = info[k - 1];
	    pos[j] = pos[k - 1];
	    if (!rtl)
	    {
	      x_offset = sat_add (x_offset, width);
	      if (n > 0) x_offset = sat_sub (x_offset, extra_repeat_overlap);
	    }
	  }
	}
      }
    }
    if (step == 0)
    {
      if ((uint64_t) count + extra_glyphs_needed > UINT32_MAX || !buf_ensure (buffer, count + extra_glyphs_needed)) break;
    }
    else
      buffer->len = new_len;
  }
  if (!rtl) buf_reverse (buffer);
}

static int info_is_mcm (const hb_glyph_info_t *info)
{
  static const hb_codepoint_t marks[] = {0x0654u, 0x0655u, 0x0658u, 0x06DCu, 0x06E3u, 0x06E7u, 0x06E8u, 0x08CAu, 0x08CBu, 0x08CDu, 0x08CEu, 0x08CFu, 0x08D3u, 0x08F3u};
  for (unsigned i = 0; i < sizeof (marks) / sizeof (marks[0]); i++)
    if (info->codepoint == marks[i]) return 1;
  return 0;
}

static void reorder_marks_arabic (const shape_plan_t *plan, hb_buffer_t *buffer, unsigned start, unsigned end)
{
  hb_glyph_info_t *info = buffer->info;
  unsigned i = start;
  for (unsigned cc = 220; cc <= 230; cc += 10)
  {
    while (i < end && info_cc (&info[i]) < cc) i++;
    if (i == end) break;
    if (info_cc (&info[i]) > cc) continue;
    unsigned j = i;
    while (j < end && info_cc (&info[j]) == cc && info_is_mcm (&info[j])) j++;
    if (i == j) continue;
    hb_glyph_info_t temp[32];
    buf_merge_clusters (buffer, start, j);
    memmove (temp, &info[i], (j - i) * sizeof (hb_glyph_info_t));
    memmove (&info[start + j - i], &info[start], (i - start) * sizeof (hb_glyph_info_t));
    memmove (&info[start], temp, (j - i) * sizeof (hb_glyph_info_t));
    unsigned new_start = start + j - i, new_cc = cc == 220 ? 25 : 26;
    while (start < new_start)
    {
      info_set_modified_combining_class (&info[start], new_cc);
      start++;
    }
    i = j;
  }
}

static const shaper_t shaper_arabic = {
  collect_features_arabic, NULL, data_create_arabic, NULL, postprocess_glyphs_arabic, NULL, NULL,
  setup_masks_arabic, reorder_marks_arabic, 0, NORM_AUTO, ZERO_WIDTH_MARKS_BY_GDEF_LATE, 1
};

static const shaper_t shaper_default = {
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NORM_AUTO, ZERO_WIDTH_MARKS_BY_GDEF_LATE, 1
};

enum { LJMO = 1, VJMO, TJMO, HANGUL_FEATURE_COUNT };
static const hb_tag_t hangul_features[HANGUL_FEATURE_COUNT] = {0, HB_TAG ('l','j','m','o'), HB_TAG ('v','j','m','o'), HB_TAG ('t','j','m','o')};
typedef struct { hb_mask_t mask_array[HANGUL_FEATURE_COUNT], calt_mask; } hangul_plan_t;

#define H_LBASE 0x1100u
#define H_VBASE 0x1161u
#define H_TBASE 0x11A7u
#define H_LCOUNT 19u
#define H_VCOUNT 21u
#define H_TCOUNT 28u
#define H_SBASE 0xAC00u
#define H_NCOUNT (H_VCOUNT * H_TCOUNT)
#define H_SCOUNT (H_LCOUNT * H_NCOUNT)
#define isCombiningL(u) IN_RANGE (u, H_LBASE, H_LBASE + H_LCOUNT - 1)
#define isCombiningV(u) IN_RANGE (u, H_VBASE, H_VBASE + H_VCOUNT - 1)
#define isCombiningT(u) IN_RANGE (u, H_TBASE + 1, H_TBASE + H_TCOUNT - 1)
#define isCombinedS(u) IN_RANGE (u, H_SBASE, H_SBASE + H_SCOUNT - 1)
#define isL(u) (IN_RANGE (u, 0x1100u, 0x115Fu) || IN_RANGE (u, 0xA960u, 0xA97Cu))
#define isV(u) (IN_RANGE (u, 0x1160u, 0x11A7u) || IN_RANGE (u, 0xD7B0u, 0xD7C6u))
#define isT(u) (IN_RANGE (u, 0x11A8u, 0x11FFu) || IN_RANGE (u, 0xD7CBu, 0xD7FBu))
#define isHangulTone(u) IN_RANGE (u, 0x302Eu, 0x302Fu)

static void collect_features_hangul (map_builder_t *m)
{
  for (unsigned i = LJMO; i < HANGUL_FEATURE_COUNT; i++) map_add_feature (m, hangul_features[i], F_NONE, 1);
}
static void override_features_hangul (map_builder_t *m) { map_add_feature (m, HB_TAG ('c','a','l','t'), F_NONE, 1); }

static void *data_create_hangul (const shape_plan_t *plan)
{
  hangul_plan_t *hp = (hangul_plan_t *) calloc (1, sizeof (hangul_plan_t));
  if (!hp) return NULL;
  for (unsigned i = 0; i < HANGUL_FEATURE_COUNT; i++) hp->mask_array[i] = plan_get_1_mask (plan, hangul_features[i]);
  hp->calt_mask = plan_get_1_mask (plan, HB_TAG ('c','a','l','t'));
  return hp;
}

static int is_zero_width_char (hb_font_t *font, hb_codepoint_t u)
{
  hb_codepoint_t g;
  return nominal_glyph (font, u, &g) && hb_font_get_glyph_h_advance (font, g) == 0;
}

static void preprocess_text_hangul (const shape_plan_t *plan, hb_buffer_t *buffer, hb_font_t *font)
{
  buf_clear_output (buffer);
  unsigned start = 0, end = 0, count = buffer->len;
  for (buffer->idx = 0; buffer->idx < count && buffer->successful;)
  {
    hb_codepoint_t u = buf_cur (buffer)->codepoint;
    if (isHangulTone (u))
    {
      if (start < end && end == buffer->out_len)
      {
	buf_unsafe_to_break_from_outbuffer (buffer, start, buffer->idx);
	if (!buf_next_glyph (buffer)) break;
	if (!is_zero_width_char (font, u))
	{
	  buf_merge_out_clusters (buffer, start, end + 1);
	  hb_glyph_info_t *info = buffer->out_info;
	  hb_glyph_info_t tone = info[end];
	  memmove (&info[start + 1], &info[start], (end - start) * sizeof (hb_glyph_info_t));
	  info[start] = tone;
	}
      }
      else
      {
	if (font_has_glyph (font, 0x25CCu))
	{
	  hb_codepoint_t chars[2];
	  if (!is_zero_width_char (font, u)) { chars[0] = u; chars[1] = 0x25CCu; }
	  else { chars[0] = 0x25CCu; chars[1] = u; }
	  buf_replace_glyphs (buffer, 1, 2, chars);
	}
	else
	  buf_next_glyph (buffer);
      }
      start = end = buffer->out_len;
      continue;
    }
    start = buffer->out_len;
    if (isL (u) && buffer->idx + 1 < count)
    {
      hb_codepoint_t l = u, v = buffer->info[buffer->idx + 1].codepoint;
      if (isV (v))
      {
	hb_codepoint_t t = 0;
	unsigned tindex = 0;
	if (buffer->idx + 2 < count)
	{
	  t = buffer->info[buffer->idx + 2].codepoint;
	  if (isT (t)) tindex = t - H_TBASE;
	  else t = 0;
	}
	buf_unsafe_to_break (buffer, buffer->idx, buffer->idx + (t ? 3 : 2));
	if (isCombiningL (l) && isCombiningV (v) && (t == 0 || isCombiningT (t)))
	{
	  hb_codepoint_t s = H_SBASE + (l - H_LBASE) * H_NCOUNT + (v - H_VBASE) * H_TCOUNT + tindex;
	  if (font_has_glyph (font, s))
	  {
	    buf_replace_glyphs (buffer, t ? 3 : 2, 1, &s);
	    end = start + 1;
	    continue;
	  }
	}
	SHAPER_AUX (buf_cur (buffer)) = LJMO;
	buf_next_glyph (buffer);
	SHAPER_AUX (buf_cur (buffer)) = VJMO;
	buf_next_glyph (buffer);
	if (t)
	{
	  SHAPER_AUX (buf_cur (buffer)) = TJMO;
	  buf_next_glyph (buffer);
	  end = start + 3;
	}
	else
	  end = start + 2;
	if (!buffer->successful) break;
	buf_merge_out_clusters (buffer, start, end);
	continue;
      }
    }
    else if (isCombinedS (u))
    {
      hb_codepoint_t s = u;
      int has_glyph = font_has_glyph (font, s);
      unsigned lindex = (s - H_SBASE) / H_NCOUNT, nindex = (s - H_SBASE) % H_NCOUNT;
      unsigned vindex = nindex / H_TCOUNT, tindex = nindex % H_TCOUNT;
      if (!tindex && buffer->idx + 1 < count && isCombiningT (buffer->info[buffer->idx + 1].codepoint))
      {
	unsigned new_tindex = buffer->info[buffer->idx + 1].codepoint - H_TBASE;
	hb_codepoint_t new_s = s + new_tindex;
	if (font_has_glyph (font, new_s))
	{
	  buf_replace_glyphs (buffer, 2, 1, &new_s);
	  end = start + 1;
	  continue;
	}
	else
	  buf_unsafe_to_break (buffer, buffer->idx, buffer->idx + 2);
      }
      if (!has_glyph || (!tindex && buffer->idx + 1 < count && isT (buffer->info[buffer->idx + 1].codepoint)))
      {
	hb_codepoint_t decomposed[3] = {H_LBASE + lindex, H_VBASE + vindex, H_TBASE + tindex};
	if (font_has_glyph (font, decomposed[0]) && font_has_glyph (font, decomposed[1]) && (!tindex || font_has_glyph (font, decomposed[2])))
	{
	  unsigned s_len = tindex ? 3 : 2;
	  buf_replace_glyphs (buffer, 1, s_len, decomposed);
	  if (has_glyph && !tindex)
	  {
	    buf_next_glyph (buffer);
	    s_len++;
	  }
	  if (!buffer->successful) break;
	  hb_glyph_info_t *info = buffer->out_info;
	  end = start + s_len;
	  unsigned i = start;
	  SHAPER_AUX (&info[i++]) = LJMO;
	  SHAPER_AUX (&info[i++]) = VJMO;
	  if (i < end) SHAPER_AUX (&info[i++]) = TJMO;
	  buf_merge_out_clusters (buffer, start, end);
	  continue;
	}
	else if (!tindex && buffer->idx + 1 < count && isT (buffer->info[buffer->idx + 1].codepoint))
	  buf_unsafe_to_break (buffer, buffer->idx, buffer->idx + 2);
      }
      if (has_glyph) end = start + 1;
    }
    buf_next_glyph (buffer);
  }
  buf_sync (buffer);
}

static void setup_masks_hangul (const shape_plan_t *plan, hb_buffer_t *buffer, hb_font_t *font)
{
  const hangul_plan_t *hp = (const hangul_plan_t *) plan->data;
  hb_glyph_info_t *info = buffer->info;
  for (unsigned i = 0; i < buffer->len; i++)
  {
    info[i].mask |= hp->mask_array[SHAPER_AUX (&info[i])];
    hb_codepoint_t u = info[i].codepoint;
    if (isL (u) || isV (u) || isT (u)) info[i].mask &= ~hp->calt_mask;
  }
}

static const shaper_t shaper_hangul = {
  collect_features_hangul, override_features_hangul, data_create_hangul, preprocess_text_hangul, NULL, NULL, NULL,
  setup_masks_hangul, NULL, 0, NORM_NONE, ZERO_WIDTH_MARKS_NONE, 1
};

static void reorder_marks_hebrew (const shape_plan_t *plan, hb_buffer_t *buffer, unsigned start, unsigned end)
{
  hb_glyph_info_t *info = buffer->info;
  for (unsigned i = start + 2; i < end; i++)
  {
    unsigned c0 = info_cc (&info[i - 2]), c1 = info_cc (&info[i - 1]), c2 = info_cc (&info[i]);
    if ((c0 == 20 || c0 == 21) && (c1 == 22 || c1 == 23) && (c2 == 25 || c2 == 220))
    {
      buf_merge_clusters (buffer, i - 1, i + 1);
      hb_glyph_info_t t = info[i - 1];
      info[i - 1] = info[i];
      info[i] = t;
      break;
    }
  }
}

static const shaper_t shaper_hebrew = {
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, reorder_marks_hebrew, HB_TAG ('h','e','b','r'), NORM_AUTO, ZERO_WIDTH_MARKS_BY_GDEF_LATE, 1
};

#define IS_SARA_AM(x) (((x) & ~0x0080u) == 0x0E33u)
#define IS_ABOVE_BASE_MARK(x) (IN_RANGE ((x) & ~0x0080u, 0x0E34u, 0x0E37u) || IN_RANGE ((x) & ~0x0080u, 0x0E47u, 0x0E4Eu) || \
			       ((x) & ~0x0080u) == 0x0E31u || ((x) & ~0x0080u) == 0x0E3Bu)

static void preprocess_text_thai (const shape_plan_t *plan, hb_buffer_t *buffer, hb_font_t *font)
{
  buf_clear_output (buffer);
  unsigned count = buffer->len;
  for (buffer->idx = 0; buffer->idx < count;)
  {
    hb_codepoint_t u = buf_cur (buffer)->codepoint;
    if (!IS_SARA_AM (u))
    {
      if (!buf_next_glyph (buffer)) break;
      continue;
    }
    buf_output_glyph (buffer, u - 0x0E33u + 0x0E4Du);
    info_set_continuation (buf_prev (buffer), buffer);
    if (!buf_replace_glyph (buffer, u - 1)) break;
    unsigned end = buffer->out_len;
    info_set_gc (&buffer->out_info[end - 2], GC_NON_SPACING_MARK);
    unsigned start = end - 2;
    while (start > 0 && IS_ABOVE_BASE_MARK (buffer->out_info[start - 1].codepoint)) start--;
    if (start + 2 < end)
    {
      buf_merge_out_clusters (buffer, start, end);
      hb_glyph_info_t t = buffer->out_info[end - 2];
      memmove (buffer->out_info + start + 1, buffer->out_info + start, sizeof (buffer->out_info[0]) * (end - start - 2));
      buffer->out_info[start] = t;
    }
    if (start) buf_merge_out_clusters (buffer, start - 1, end);
  }
  buf_sync (buffer);
}

static const shaper_t shaper_thai = {
  NULL, NULL, NULL, preprocess_text_thai, NULL, NULL, NULL, NULL, NULL, 0, NORM_AUTO, ZERO_WIDTH_MARKS_BY_GDEF_LATE, 1
};

static const shaper_t shaper_indic;

static const shaper_t *shaper_categorize (hb_script_t script, int direction, hb_tag_t gsub_script)
{
  switch ((unsigned) script)
  {
  default: return &shaper_default;
  case HB_SCRIPT_ARABIC: case HB_SCRIPT_SYRIAC:
    return gsub_script != TAG_DFLT || script == HB_SCRIPT_ARABIC ? &shaper_arabic : &shaper_default;
  case HB_SCRIPT_THAI: case HB_SCRIPT_LAO: return &shaper_thai;
  case HB_SCRIPT_HANGUL: return &shaper_hangul;
  case HB_SCRIPT_HEBREW: return &shaper_hebrew;
  case HB_SCRIPT_BENGALI: case HB_SCRIPT_DEVANAGARI: case HB_SCRIPT_GUJARATI: case HB_SCRIPT_GURMUKHI: case HB_SCRIPT_KANNADA:
  case HB_SCRIPT_MALAYALAM: case HB_SCRIPT_ORIYA: case HB_SCRIPT_TAMIL: case HB_SCRIPT_TELUGU:
    return gsub_script == TAG_DFLT || gsub_script == TAG_latn ? &shaper_default : &shaper_indic;
  }
}

static const struct { hb_tag_t tag; unsigned flags; } common_features[] =
{
  {HB_TAG ('a','b','v','m'), F_GLOBAL}, {HB_TAG ('b','l','w','m'), F_GLOBAL}, {HB_TAG ('c','c','m','p'), F_GLOBAL},
  {HB_TAG ('l','o','c','l'), F_GLOBAL}, {HB_TAG ('m','a','r','k'), F_GLOBAL_MANUAL_JOINERS},
  {HB_TAG ('m','k','m','k'), F_GLOBAL_MANUAL_JOINERS}, {HB_TAG ('r','l','i','g'), F_GLOBAL},
  {HB_TAG ('c','a','l','t'), F_GLOBAL}, {HB_TAG ('c','l','i','g'), F_GLOBAL}, {HB_TAG ('c','u','r','s'), F_GLOBAL},
  {HB_TAG ('d','i','s','t'), F_GLOBAL}, {HB_TAG ('k','e','r','n'), F_GLOBAL_HAS_FALLBACK}, {HB_TAG ('l','i','g','a'), F_GLOBAL},
  {HB_TAG ('r','c','l','t'), F_GLOBAL},
};

static void plan_destroy (shape_plan_t *p)
{
  if (!p) return;
  free (p->data);
  free (p->features.a);
  for (unsigned ti = 0; ti < 2; ti++)
  {
    free (p->lookups[ti].a);
    free (p->stages[ti].a);
  }
  free (p);
}

static shape_plan_t *plan_create (hb_face_t *face, layout_t *l, hb_script_t script, int direction, const unsigned vi[2])
{
  shape_plan_t *p = (shape_plan_t *) calloc (1, sizeof (shape_plan_t));
  if (!p) return NULL;
  p->script = script;
  p->direction = direction;
  p->variations_index[0] = vi[0];
  p->variations_index[1] = vi[1];
  map_builder_t m;
  map_builder_init (&m, face, l, script, direction);
  const shaper_t *shaper = shaper_categorize (script, direction, m.chosen_script[0]);
  p->shaper = shaper;

  m.is_simple = 1;
  map_enable_feature (&m, HB_TAG ('r','v','r','n'), 0, 1);
  map_add_gsub_pause (&m, NULL);
  if (direction == DIR_LTR)
  {
    map_enable_feature (&m, HB_TAG ('l','t','r','a'), 0, 1);
    map_enable_feature (&m, HB_TAG ('l','t','r','m'), 0, 1);
  }
  else
  {
    map_enable_feature (&m, HB_TAG ('r','t','l','a'), 0, 1);
    map_add_feature (&m, HB_TAG ('r','t','l','m'), F_NONE, 1);
  }
  map_enable_feature (&m, HB_TAG ('r','a','n','d'), F_RANDOM, MAP_MAX_VALUE);
  map_enable_feature (&m, HB_TAG ('H','a','r','f'), 0, 1);
  map_enable_feature (&m, HB_TAG ('H','A','R','F'), 0, 1);
  if (shaper->collect_features)
  {
    m.is_simple = 0;
    shaper->collect_features (&m);
  }
  map_enable_feature (&m, HB_TAG ('B','u','z','z'), 0, 1);
  map_enable_feature (&m, HB_TAG ('B','U','Z','Z'), 0, 1);
  for (unsigned i = 0; i < sizeof (common_features) / sizeof (common_features[0]); i++)
    map_add_feature (&m, common_features[i].tag, common_features[i].flags, 1);
  if (shaper->override_features) shaper->override_features (&m);

  map_compile (&m, p);
  free (m.feature_infos.a);
  free (m.stages[0].a);
  free (m.stages[1].a);

  p->rtlm_mask = plan_get_1_mask (p, HB_TAG ('r','t','l','m'));
  int disable_gpos = shaper->gpos_tag && shaper->gpos_tag != p->chosen_script[1];
  p->fallback_glyph_classes = !l->glyph_class_def;
  p->apply_gpos = !disable_gpos && l->g[1].table && rd32 (l->g[1].table);
  p->zero_marks = shaper->zero_width_marks != ZERO_WIDTH_MARKS_NONE;
  p->has_gpos_mark = !!plan_get_1_mask (p, HB_TAG ('m','a','r','k'));
  p->adjust_mark_positioning_when_zeroing = !p->apply_gpos;
  p->fallback_mark_positioning = p->adjust_mark_positioning_when_zeroing && shaper->fallback_position;
  if (shaper->data_create)
  {
    p->data = shaper->data_create (p);
    if (!p->data)
    {
      plan_destroy (p);
      return NULL;
    }
  }
  return p;
}

static void face_free_plans (hb_face_t *face)
{
  for (plan_node_t *n = face->plans, *next; n; n = next)
  {
    next = n->next;
    plan_destroy (n->plan);
    free (n);
  }
  face->plans = NULL;
}

static shape_plan_t *plan_get (hb_font_t *font, layout_t *l, hb_script_t script, int direction)
{
  hb_face_t *face = font->face;
  unsigned vi[2];
  for (unsigned ti = 0; ti < 2; ti++) vi[ti] = find_variations_index (&l->g[ti], font, l);
  for (plan_node_t *n = face->plans; n; n = n->next)
    if (n->plan->script == script && n->plan->direction == direction && n->plan->variations_index[0] == vi[0] && n->plan->variations_index[1] == vi[1])
      return n->plan;
  shape_plan_t *p = plan_create (face, l, script, direction, vi);
  if (!p) return NULL;
  plan_node_t *n = (plan_node_t *) calloc (1, sizeof (plan_node_t));
  if (!n)
  {
    plan_destroy (p);
    return NULL;
  }
  n->plan = p;
  n->next = face->plans;
  face->plans = n;
  return p;
}

static int is_regional_indicator (hb_codepoint_t u) { return IN_RANGE (u, 0x1F1E6u, 0x1F1FFu); }

static void set_unicode_props (hb_buffer_t *buffer, hb_mask_t global_mask)
{
  unsigned count = buffer->len;
  hb_glyph_info_t *info = buffer->info;
  for (unsigned i = 0; i < count; i++)
  {
    info[i].mask = global_mask;
    info_set_unicode_props (&info[i], buffer);
    if (info[i].codepoint < 0x80) continue;
    unsigned gen_cat = info_gc (&info[i]);
    if (FLAG_UNSAFE (gen_cat) & (FLAG (GC_LOWERCASE_LETTER) | FLAG (GC_UPPERCASE_LETTER) | FLAG (GC_TITLECASE_LETTER) | FLAG (GC_OTHER_LETTER) | FLAG (GC_SPACE_SEPARATOR)))
      continue;
    if (gen_cat == GC_MODIFIER_SYMBOL && IN_RANGE (info[i].codepoint, 0x1F3FBu, 0x1F3FFu))
      info_set_continuation (&info[i], buffer);
    else if (i && is_regional_indicator (info[i].codepoint))
    {
      if (is_regional_indicator (info[i - 1].codepoint) && !info_is_continuation (&info[i - 1]))
	info_set_continuation (&info[i], buffer);
    }
    else if (info_is_zwj (&info[i]))
    {
      info_set_continuation (&info[i], buffer);
      if (i + 1 < count && _hb_emoji_is_Extended_Pictographic (info[i + 1].codepoint))
      {
	i++;
	info[i].mask = global_mask;
	info_set_unicode_props (&info[i], buffer);
	info_set_continuation (&info[i], buffer);
      }
    }
    else if (IN_RANGE (info[i].codepoint, 0xFF9Eu, 0xFF9Fu) || IN_RANGE (info[i].codepoint, 0xE0020u, 0xE007Fu))
      info_set_continuation (&info[i], buffer);
    else if (info[i].codepoint == 0x2044u)
      buffer->scratch_flags |= SCRATCH_HAS_FRACTION_SLASH;
  }
}

static void zero_mark_widths_by_gdef (hb_buffer_t *buffer, int adjust_offsets)
{
  for (unsigned i = 0; i < buffer->len; i++)
    if (info_is_mark (&buffer->info[i]))
    {
      hb_glyph_position_t *pos = &buffer->pos[i];
      if (adjust_offsets)
      {
	pos->x_offset = sat_sub (pos->x_offset, pos->x_advance);
	pos->y_offset = sat_sub (pos->y_offset, pos->y_advance);
      }
      pos->x_advance = pos->y_advance = 0;
    }
}

static void fallback_spaces (hb_font_t *font, hb_buffer_t *buffer)
{
  hb_glyph_info_t *info = buffer->info;
  hb_glyph_position_t *pos = buffer->pos;
  for (unsigned i = 0; i < buffer->len; i++)
    if (info_is_unicode_space (&info[i]) && !info_ligated (&info[i]))
    {
      unsigned space_type = info_space_fallback_type (&info[i]);
      hb_codepoint_t glyph;
      switch (space_type)
      {
      case SPACE_EM: case SPACE_EM_2: case SPACE_EM_3: case SPACE_EM_4: case SPACE_EM_5: case SPACE_EM_6: case SPACE_EM_16:
	pos[i].x_advance = ((int64_t) font->x_scale + ((int) space_type) / 2) / (int) space_type;
	break;
      case SPACE_4_EM_18:
	pos[i].x_advance = (int64_t) font->x_scale * 4 / 18;
	break;
      case SPACE_FIGURE:
	for (char u = '0'; u <= '9'; u++)
	  if (nominal_glyph (font, u, &glyph))
	  {
	    pos[i].x_advance = hb_font_get_glyph_h_advance (font, glyph);
	    break;
	  }
	break;
      case SPACE_PUNCTUATION:
	if (nominal_glyph (font, '.', &glyph) || nominal_glyph (font, ',', &glyph))
	  pos[i].x_advance = hb_font_get_glyph_h_advance (font, glyph);
	break;
      case SPACE_NARROW:
	pos[i].x_advance /= 2;
	break;
      }
    }
}

static int is_default_ignorable_filter (const hb_glyph_info_t *i) { return info_is_default_ignorable (i); }

static void shape_internal (shape_plan_t *plan, hb_font_t *font, hb_buffer_t *buffer)
{
  int target_direction = buffer->direction;
  set_unicode_props (buffer, plan->global_mask);
  if (buffer->scratch_flags & SCRATCH_HAS_CONTINUATIONS)
    foreach_grapheme (buffer, start, end) buf_merge_clusters (buffer, start, end);
  if (plan->shaper->preprocess_text) plan->shaper->preprocess_text (plan, buffer, font);

  if (target_direction == DIR_RTL)
    for (unsigned i = 0; i < buffer->len; i++)
    {
      hb_codepoint_t m = uni_mirroring (buffer->info[i].codepoint);
      if (m != buffer->info[i].codepoint && font_has_glyph (font, m)) buffer->info[i].codepoint = m;
      else buffer->info[i].mask |= plan->rtlm_mask;
    }
  normalize (plan, buffer, font);
  if (plan->shaper->setup_masks) plan->shaper->setup_masks (plan, buffer, font);
  for (unsigned i = 0; i < buffer->len; i++) buffer->info[i].codepoint = NORMALIZER_GLYPH (&buffer->info[i]);

  layout_set_glyph_props (font, buffer);
  if (plan->fallback_glyph_classes)
    for (unsigned i = 0; i < buffer->len; i++)
      GPROPS (&buffer->info[i]) = info_gc (&buffer->info[i]) != GC_NON_SPACING_MARK || info_is_default_ignorable (&buffer->info[i]) ?
				  GLYPH_PROPS_BASE_GLYPH : GLYPH_PROPS_MARK;
  layout_apply (0, plan, font, buffer, plan->lookups[0].a, plan->stages[0].a, plan->stages[0].len);

  buf_clear_positions (buffer);
  for (unsigned i = 0; i < buffer->len; i++) buffer->pos[i].x_advance = hb_font_get_glyph_h_advance (font, buffer->info[i].codepoint);
  if (buffer->scratch_flags & SCRATCH_HAS_SPACE_FALLBACK) fallback_spaces (font, buffer);
  int adjust_offsets_when_zeroing = plan->adjust_mark_positioning_when_zeroing && buffer->direction == DIR_LTR;
  for (unsigned i = 0; i < buffer->len; i++) ATTACH_CHAIN (&buffer->pos[i]) = ATTACH_TYPE (&buffer->pos[i]) = 0;
  if (plan->zero_marks && plan->shaper->zero_width_marks == ZERO_WIDTH_MARKS_BY_GDEF_EARLY) zero_mark_widths_by_gdef (buffer, adjust_offsets_when_zeroing);
  if (plan->apply_gpos) layout_apply (1, plan, font, buffer, plan->lookups[1].a, plan->stages[1].a, plan->stages[1].len);
  if (plan->zero_marks && plan->shaper->zero_width_marks == ZERO_WIDTH_MARKS_BY_GDEF_LATE) zero_mark_widths_by_gdef (buffer, adjust_offsets_when_zeroing);
  if (buffer->scratch_flags & SCRATCH_HAS_DEFAULT_IGNORABLES)
    for (unsigned i = 0; i < buffer->len; i++)
      if (info_is_default_ignorable (&buffer->info[i]))
	buffer->pos[i].x_advance = buffer->pos[i].y_advance = buffer->pos[i].x_offset = 0;
  position_finish_offsets (font, buffer);
  if (buffer->direction == DIR_RTL) buf_reverse (buffer);

  if (buffer->scratch_flags & SCRATCH_HAS_DEFAULT_IGNORABLES)
  {
    hb_codepoint_t invisible;
    if (nominal_glyph (font, ' ', &invisible))
    {
      for (unsigned i = 0; i < buffer->len; i++)
	if (info_is_default_ignorable (&buffer->info[i])) buffer->info[i].codepoint = invisible;
    }
    else
      buf_delete_glyphs_inplace (buffer, is_default_ignorable_filter);
  }
  if (plan->shaper->postprocess_glyphs) plan->shaper->postprocess_glyphs (plan, buffer, font);
  buffer->direction = target_direction;
}

void hb_shape (hb_font_t *font, hb_buffer_t *buffer, const void *features, unsigned int num_features)
{
  if (!buffer->len) return;
  buf_enter (buffer);
  layout_t *l = face_layout (font->face);
  shape_plan_t *plan = l ? plan_get (font, l, buffer->script, buffer->direction) : NULL;
  if (plan) shape_internal (plan, font, buffer);
  buf_leave (buffer);
}

enum
{
  POS_START, POS_RA_TO_BECOME_REPH, POS_PRE_M, POS_PRE_C, POS_BASE_C, POS_AFTER_MAIN, POS_ABOVE_C,
  POS_BEFORE_SUB, POS_BELOW_C, POS_AFTER_SUB, POS_BEFORE_POST, POS_POST_C, POS_AFTER_POST, POS_SMVD, POS_END
};

#define I_Cat(Cat) indic_syllable_machine_ex_##Cat
#define indic_syllable_machine_ex_A 9u
#define indic_syllable_machine_ex_C 1u
#define indic_syllable_machine_ex_CM 16u
#define indic_syllable_machine_ex_CS 18u
#define indic_syllable_machine_ex_DOTTEDCIRCLE 11u
#define indic_syllable_machine_ex_H 4u
#define indic_syllable_machine_ex_M 7u
#define indic_syllable_machine_ex_MPst 13u
#define indic_syllable_machine_ex_N 3u
#define indic_syllable_machine_ex_PLACEHOLDER 10u
#define indic_syllable_machine_ex_RS 12u
#define indic_syllable_machine_ex_Ra 15u
#define indic_syllable_machine_ex_Repha 14u
#define indic_syllable_machine_ex_SM 8u
#define indic_syllable_machine_ex_SMPst 57u
#define indic_syllable_machine_ex_Symbol 17u
#define indic_syllable_machine_ex_V 2u
#define indic_syllable_machine_ex_VD 9u
#define indic_syllable_machine_ex_X 0u
#define indic_syllable_machine_ex_ZWJ 6u
#define indic_syllable_machine_ex_ZWNJ 5u

enum { indic_consonant_syllable, indic_vowel_syllable, indic_standalone_cluster, indic_symbol_cluster, indic_broken_cluster, indic_non_indic_cluster };

static const unsigned char _indic_syllable_machine_trans_keys[] = {
	8u, 57u, 4u, 57u, 5u, 57u, 5u, 57u, 13u, 13u, 4u, 57u, 4u, 57u, 4u, 57u,
	8u, 57u, 5u, 57u, 5u, 57u, 13u, 13u, 4u, 57u, 4u, 57u, 4u, 57u, 4u, 57u,
	8u, 57u, 5u, 57u, 5u, 57u, 13u, 13u, 4u, 57u, 4u, 57u, 4u, 57u, 8u, 57u,
	5u, 57u, 5u, 57u, 13u, 13u, 4u, 57u, 4u, 57u, 5u, 57u, 8u, 57u, 1u, 57u,
	3u, 57u, 3u, 57u, 4u, 57u, 1u, 57u, 5u, 57u, 5u, 57u, 9u, 9u, 5u, 9u,
	1u, 57u, 1u, 57u, 1u, 57u, 3u, 57u, 4u, 57u, 5u, 57u, 5u, 57u, 4u, 57u,
	5u, 57u, 3u, 57u, 5u, 57u, 3u, 57u, 3u, 57u, 3u, 57u, 3u, 57u, 4u, 57u,
	1u, 57u, 3u, 57u, 3u, 57u, 4u, 57u, 1u, 57u, 5u, 57u, 9u, 9u, 5u, 9u,
	1u, 57u, 1u, 57u, 3u, 57u, 4u, 57u, 5u, 57u, 5u, 57u, 4u, 57u, 5u, 57u,
	5u, 57u, 3u, 57u, 5u, 57u, 3u, 57u, 3u, 57u, 4u, 57u, 3u, 57u, 3u, 57u,
	4u, 57u, 1u, 57u, 3u, 57u, 1u, 57u, 5u, 57u, 9u, 9u, 5u, 9u, 1u, 57u,
	1u, 57u, 3u, 57u, 4u, 57u, 5u, 57u, 5u, 57u, 3u, 57u, 4u, 57u, 5u, 57u,
	5u, 57u, 3u, 57u, 5u, 57u, 3u, 57u, 4u, 57u, 4u, 57u, 3u, 57u, 3u, 57u,
	4u, 57u, 1u, 57u, 3u, 57u, 1u, 57u, 5u, 57u, 9u, 9u, 5u, 9u, 1u, 57u,
	1u, 57u, 3u, 57u, 4u, 57u, 5u, 57u, 5u, 57u, 3u, 57u, 4u, 57u, 5u, 57u,
	5u, 57u, 3u, 57u, 5u, 57u, 1u, 57u, 3u, 57u, 1u, 57u, 4u, 57u, 5u, 57u,
	5u, 57u, 9u, 9u, 5u, 9u, 1u, 57u, 3u, 57u, 5u, 57u, 5u, 57u, 9u, 9u,
	5u, 9u, 1u, 15u, 0
};

static const char _indic_syllable_machine_key_spans[] = {
	50, 54, 53, 53, 1, 54, 54, 54,
	50, 53, 53, 1, 54, 54, 54, 54,
	50, 53, 53, 1, 54, 54, 54, 50,
	53, 53, 1, 54, 54, 53, 50, 57,
	55, 55, 54, 57, 53, 53, 1, 5,
	57, 57, 57, 55, 54, 53, 53, 54,
	53, 55, 53, 55, 55, 55, 55, 54,
	57, 55, 55, 54, 57, 53, 1, 5,
	57, 57, 55, 54, 53, 53, 54, 53,
	53, 55, 53, 55, 55, 54, 55, 55,
	54, 57, 55, 57, 53, 1, 5, 57,
	57, 55, 54, 53, 53, 55, 54, 53,
	53, 55, 53, 55, 54, 54, 55, 55,
	54, 57, 55, 57, 53, 1, 5, 57,
	57, 55, 54, 53, 53, 55, 54, 53,
	53, 55, 53, 57, 55, 57, 54, 53,
	53, 1, 5, 57, 55, 53, 53, 1,
	5, 15
};

static const short _indic_syllable_machine_index_offsets[] = {
	0, 51, 106, 160, 214, 216, 271, 326,
	381, 432, 486, 540, 542, 597, 652, 707,
	762, 813, 867, 921, 923, 978, 1033, 1088,
	1139, 1193, 1247, 1249, 1304, 1359, 1413, 1464,
	1522, 1578, 1634, 1689, 1747, 1801, 1855, 1857,
	1863, 1921, 1979, 2037, 2093, 2148, 2202, 2256,
	2311, 2365, 2421, 2475, 2531, 2587, 2643, 2699,
	2754, 2812, 2868, 2924, 2979, 3037, 3091, 3093,
	3099, 3157, 3215, 3271, 3326, 3380, 3434, 3489,
	3543, 3597, 3653, 3707, 3763, 3819, 3874, 3930,
	3986, 4041, 4099, 4155, 4213, 4267, 4269, 4275,
	4333, 4391, 4447, 4502, 4556, 4610, 4666, 4721,
	4775, 4829, 4885, 4939, 4995, 5050, 5105, 5161,
	5217, 5272, 5330, 5386, 5444, 5498, 5500, 5506,
	5564, 5622, 5678, 5733, 5787, 5841, 5897, 5952,
	6006, 6060, 6116, 6170, 6228, 6284, 6342, 6397,
	6451, 6505, 6507, 6513, 6571, 6627, 6681, 6735,
	6737, 6743
};

static const unsigned char _indic_syllable_machine_indicies[] = {
	1, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 1, 0, 2, 3, 3, 4, 5,
	0, 0, 0, 0, 4, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	5, 0, 3, 3, 4, 6, 0, 0,
	0, 0, 4, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 6, 0,
	3, 3, 4, 5, 0, 0, 0, 0,
	4, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 5, 0, 4, 0,
	7, 3, 3, 4, 5, 0, 0, 0,
	0, 4, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 5, 0, 2,
	3, 3, 4, 5, 0, 0, 0, 8,
	4, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 5, 0, 10, 11,
	11, 12, 13, 9, 9, 9, 9, 12,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 13, 9, 14, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 14, 9,
	11, 11, 12, 15, 9, 9, 9, 9,
	12, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 15, 9, 11, 11,
	12, 13, 9, 9, 9, 9, 12, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 13, 9, 12, 9, 16, 11,
	11, 12, 13, 9, 9, 9, 9, 12,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 13, 9, 10, 11, 11,
	12, 13, 9, 9, 9, 17, 12, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 13, 9, 10, 11, 11, 12,
	13, 9, 9, 9, 18, 12, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9,
	9, 13, 9, 20, 21, 21, 22, 23,
	19, 19, 19, 24, 22, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	23, 19, 25, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 25, 19, 21, 21, 22,
	27, 26, 26, 26, 26, 22, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 26,
	26, 27, 26, 21, 21, 22, 23, 19,
	19, 19, 19, 22, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 23,
	19, 22, 26, 20, 21, 21, 22, 23,
	19, 19, 19, 19, 22, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	23, 19, 28, 21, 21, 22, 23, 19,
	19, 19, 19, 22, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 23,
	19, 30, 31, 31, 32, 33, 29, 29,
	29, 34, 32, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 33, 29,
	35, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 35, 29, 31, 31, 32, 36, 29,
	29, 29, 29, 32, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 36,
	29, 31, 31, 32, 33, 29, 29, 29,
	29, 32, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 33, 29, 32,
	29, 30, 31, 31, 32, 33, 29, 29,
	29, 29, 32, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 33, 29,
	37, 31, 31, 32, 33, 29, 29, 29,
	29, 32, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 33, 29, 21,
	21, 22, 38, 0, 0, 0, 0, 22,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 38, 0, 40, 39, 39,
	39, 39, 39, 39, 39, 39, 39, 39,
	39, 39, 39, 39, 39, 39, 39, 39,
	39, 39, 39, 39, 39, 39, 39, 39,
	39, 39, 39, 39, 39, 39, 39, 39,
	39, 39, 39, 39, 39, 39, 39, 39,
	39, 39, 39, 39, 39, 39, 40, 39,
	42, 43, 44, 45, 46, 47, 22, 23,
	48, 49, 49, 24, 22, 50, 51, 52,
	53, 54, 41, 41, 41, 41, 41, 41,
	41, 41, 41, 41, 41, 41, 41, 41,
	41, 41, 41, 41, 41, 41, 41, 41,
	41, 41, 41, 41, 41, 41, 41, 41,
	41, 41, 41, 41, 41, 41, 41, 41,
	55, 41, 57, 58, 59, 60, 4, 5,
	61, 56, 56, 8, 4, 56, 56, 62,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	5, 56, 63, 58, 64, 64, 4, 5,
	61, 56, 56, 56, 4, 56, 56, 62,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	5, 56, 58, 64, 64, 4, 5, 61,
	56, 56, 56, 4, 56, 56, 62, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 5,
	56, 42, 56, 56, 56, 65, 66, 56,
	1, 61, 56, 56, 56, 56, 56, 42,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 1, 56, 67, 67, 56, 1, 61,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 1,
	56, 61, 56, 56, 68, 61, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 68, 56, 61,
	56, 61, 56, 56, 56, 61, 56, 42,
	56, 69, 56, 67, 67, 56, 1, 61,
	56, 56, 56, 56, 56, 42, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 1,
	56, 42, 56, 56, 56, 67, 67, 56,
	1, 61, 56, 56, 56, 56, 56, 42,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 1, 56, 42, 56, 56, 56, 67,
	66, 56, 1, 61, 56, 56, 56, 56,
	56, 42, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 1, 56, 70, 71, 72,
	72, 4, 5, 61, 56, 56, 56, 4,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 5, 56, 71, 72, 72,
	4, 5, 61, 56, 56, 56, 4, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 5, 56, 72, 72, 4, 5,
	61, 56, 56, 56, 4, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	5, 56, 61, 56, 56, 68, 61, 56,
	56, 56, 4, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 68, 56,
	73, 74, 74, 4, 5, 61, 56, 56,
	56, 4, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 5, 56, 65,
	75, 56, 1, 61, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 1, 56, 65, 56, 67,
	67, 56, 1, 61, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 1, 56, 67, 75, 56,
	1, 61, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 1, 56, 57, 58, 64, 64, 4,
	5, 61, 56, 56, 56, 4, 56, 56,
	62, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 5, 56, 57, 58, 59, 64, 4,
	5, 61, 56, 56, 8, 4, 56, 56,
	62, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 5, 56, 77, 78, 79, 80, 12,
	13, 81, 76, 76, 18, 12, 76, 76,
	82, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 13, 76, 83, 78, 84, 80, 12,
	13, 81, 76, 76, 76, 12, 76, 76,
	82, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 13, 76, 78, 84, 80, 12, 13,
	81, 76, 76, 76, 12, 76, 76, 82,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	13, 76, 85, 76, 76, 76, 86, 87,
	76, 14, 81, 76, 76, 76, 76, 76,
	85, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 14, 76, 88, 78, 89, 90,
	12, 13, 81, 76, 76, 17, 12, 76,
	76, 82, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 13, 76, 91, 78, 84, 84,
	12, 13, 81, 76, 76, 76, 12, 76,
	76, 82, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 13, 76, 78, 84, 84, 12,
	13, 81, 76, 76, 76, 12, 76, 76,
	82, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 13, 76, 85, 76, 76, 76, 92,
	87, 76, 14, 81, 76, 76, 76, 76,
	76, 85, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 14, 76, 81, 76, 76,
	93, 81, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 93, 76, 81, 76, 81, 76, 76,
	76, 81, 76, 85, 76, 94, 76, 92,
	92, 76, 14, 81, 76, 76, 76, 76,
	76, 85, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 14, 76, 85, 76, 76,
	76, 92, 92, 76, 14, 81, 76, 76,
	76, 76, 76, 85, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 14, 76, 95,
	96, 97, 97, 12, 13, 81, 76, 76,
	76, 12, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 13, 76, 96,
	97, 97, 12, 13, 81, 76, 76, 76,
	12, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 13, 76, 97, 97,
	12, 13, 81, 76, 76, 76, 12, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 13, 76, 81, 76, 76, 93,
	81, 76, 76, 76, 12, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	93, 76, 98, 99, 99, 12, 13, 81,
	76, 76, 76, 12, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 13,
	76, 86, 100, 76, 14, 81, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 14, 76, 92,
	92, 76, 14, 81, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 14, 76, 86, 76, 92,
	92, 76, 14, 81, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 14, 76, 92, 100, 76,
	14, 81, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 14, 76, 88, 78, 84, 84, 12,
	13, 81, 76, 76, 76, 12, 76, 76,
	82, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 13, 76, 88, 78, 89, 84, 12,
	13, 81, 76, 76, 17, 12, 76, 76,
	82, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 13, 76, 10, 11, 11, 12, 13,
	76, 76, 76, 76, 12, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	13, 76, 77, 78, 84, 80, 12, 13,
	81, 76, 76, 76, 12, 76, 76, 82,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	76, 76, 76, 76, 76, 76, 76, 76,
	13, 76, 102, 45, 103, 103, 22, 23,
	48, 101, 101, 101, 22, 101, 101, 52,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	23, 101, 45, 103, 103, 22, 23, 48,
	101, 101, 101, 22, 101, 101, 52, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 23,
	101, 104, 101, 101, 101, 105, 106, 101,
	25, 48, 101, 101, 101, 101, 101, 104,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 25, 101, 44, 45, 107, 108, 22,
	23, 48, 101, 101, 24, 22, 101, 101,
	52, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 23, 101, 104, 101, 101, 101, 109,
	106, 101, 25, 48, 101, 101, 101, 101,
	101, 104, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 25, 101, 48, 101, 101,
	110, 48, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 110, 101, 48, 101, 48, 101, 101,
	101, 48, 101, 104, 101, 111, 101, 109,
	109, 101, 25, 48, 101, 101, 101, 101,
	101, 104, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 25, 101, 104, 101, 101,
	101, 109, 109, 101, 25, 48, 101, 101,
	101, 101, 101, 104, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 25, 101, 112,
	113, 114, 114, 22, 23, 48, 101, 101,
	101, 22, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 23, 101, 113,
	114, 114, 22, 23, 48, 101, 101, 101,
	22, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 23, 101, 114, 114,
	22, 23, 48, 101, 101, 101, 22, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 23, 101, 48, 26, 26, 110,
	48, 26, 26, 26, 22, 26, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 26,
	110, 26, 44, 45, 103, 103, 22, 23,
	48, 101, 101, 101, 22, 101, 101, 52,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	23, 101, 115, 116, 116, 22, 23, 48,
	101, 101, 101, 22, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 23,
	101, 105, 117, 101, 25, 48, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 25, 101, 109,
	109, 101, 25, 48, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 25, 101, 105, 101, 109,
	109, 101, 25, 48, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 25, 101, 109, 117, 101,
	25, 48, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 25, 101, 44, 45, 107, 103, 22,
	23, 48, 101, 101, 24, 22, 101, 101,
	52, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 23, 101, 20, 21, 21, 22, 23,
	118, 118, 118, 24, 22, 118, 118, 118,
	118, 118, 118, 118, 118, 118, 118, 118,
	118, 118, 118, 118, 118, 118, 118, 118,
	118, 118, 118, 118, 118, 118, 118, 118,
	118, 118, 118, 118, 118, 118, 118, 118,
	118, 118, 118, 118, 118, 118, 118, 118,
	23, 118, 20, 21, 21, 22, 23, 118,
	118, 118, 118, 22, 118, 118, 118, 118,
	118, 118, 118, 118, 118, 118, 118, 118,
	118, 118, 118, 118, 118, 118, 118, 118,
	118, 118, 118, 118, 118, 118, 118, 118,
	118, 118, 118, 118, 118, 118, 118, 118,
	118, 118, 118, 118, 118, 118, 118, 23,
	118, 120, 121, 122, 123, 32, 33, 124,
	119, 119, 34, 32, 119, 119, 125, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 33,
	119, 126, 121, 123, 123, 32, 33, 124,
	119, 119, 119, 32, 119, 119, 125, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 33,
	119, 121, 123, 123, 32, 33, 124, 119,
	119, 119, 32, 119, 119, 125, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 33, 119,
	127, 119, 119, 119, 128, 129, 119, 35,
	124, 119, 119, 119, 119, 119, 127, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	35, 119, 120, 121, 122, 49, 32, 33,
	124, 119, 119, 34, 32, 119, 119, 125,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	33, 119, 127, 119, 119, 119, 130, 129,
	119, 35, 124, 119, 119, 119, 119, 119,
	127, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 35, 119, 124, 119, 119, 131,
	124, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	131, 119, 124, 119, 124, 119, 119, 119,
	124, 119, 127, 119, 132, 119, 130, 130,
	119, 35, 124, 119, 119, 119, 119, 119,
	127, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 35, 119, 127, 119, 119, 119,
	130, 130, 119, 35, 124, 119, 119, 119,
	119, 119, 127, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 35, 119, 133, 134,
	135, 135, 32, 33, 124, 119, 119, 119,
	32, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 33, 119, 134, 135,
	135, 32, 33, 124, 119, 119, 119, 32,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 33, 119, 135, 135, 32,
	33, 124, 119, 119, 119, 32, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 33, 119, 124, 119, 119, 131, 124,
	119, 119, 119, 32, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 131,
	119, 120, 121, 123, 123, 32, 33, 124,
	119, 119, 119, 32, 119, 119, 125, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 33,
	119, 136, 137, 137, 32, 33, 124, 119,
	119, 119, 32, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 33, 119,
	128, 138, 119, 35, 124, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 35, 119, 130, 130,
	119, 35, 124, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 35, 119, 128, 119, 130, 130,
	119, 35, 124, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 35, 119, 130, 138, 119, 35,
	124, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	119, 119, 119, 119, 119, 119, 119, 119,
	35, 119, 42, 43, 44, 45, 107, 103,
	22, 23, 48, 49, 49, 24, 22, 101,
	42, 52, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 101, 101, 101, 101, 101, 101,
	101, 101, 23, 101, 57, 139, 59, 60,
	4, 5, 61, 56, 56, 8, 4, 56,
	56, 62, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 5, 56, 42, 43, 44, 45,
	140, 141, 22, 142, 143, 56, 49, 24,
	22, 56, 42, 52, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 142, 56, 20, 144,
	144, 22, 142, 61, 56, 56, 24, 22,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 142, 56, 61, 56, 56,
	68, 61, 56, 56, 56, 22, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 68, 56, 143, 56, 56, 145, 143,
	56, 56, 56, 22, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 145,
	56, 143, 56, 143, 56, 56, 56, 143,
	56, 42, 56, 69, 20, 144, 144, 22,
	142, 61, 56, 56, 56, 22, 56, 42,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 56, 56, 56, 56, 56, 56, 56,
	56, 142, 56, 147, 146, 148, 148, 146,
	40, 149, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 40, 146, 148, 148, 146, 40, 149,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 40,
	146, 149, 146, 146, 150, 149, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 146, 146, 146,
	146, 146, 146, 146, 146, 150, 146, 149,
	146, 149, 146, 146, 146, 149, 146, 42,
	118, 118, 118, 118, 118, 118, 118, 118,
	49, 118, 118, 118, 118, 42, 118, 0
};

static const unsigned char _indic_syllable_machine_trans_targs[] = {
	31, 37, 42, 2, 43, 46, 4, 50,
	51, 31, 60, 9, 66, 69, 61, 11,
	74, 75, 78, 31, 83, 17, 89, 92,
	93, 84, 31, 19, 98, 31, 107, 24,
	113, 116, 117, 108, 26, 122, 127, 31,
	134, 31, 32, 53, 79, 81, 100, 101,
	85, 102, 123, 124, 94, 132, 137, 92,
	31, 33, 35, 6, 52, 38, 47, 34,
	1, 36, 40, 0, 39, 41, 44, 45,
	3, 48, 5, 49, 31, 54, 56, 14,
	77, 62, 70, 55, 7, 57, 72, 64,
	58, 13, 76, 59, 8, 63, 65, 67,
	68, 10, 71, 12, 73, 31, 80, 20,
	82, 96, 87, 15, 99, 16, 86, 88,
	90, 91, 18, 95, 21, 97, 31, 31,
	103, 105, 22, 27, 109, 118, 104, 106,
	120, 111, 23, 110, 112, 114, 115, 25,
	119, 28, 121, 125, 126, 131, 128, 129,
	29, 130, 31, 133, 30, 135, 136
};

static const char _indic_syllable_machine_trans_actions[] = {
	1, 0, 2, 0, 2, 0, 0, 2,
	2, 3, 2, 0, 2, 0, 0, 0,
	2, 2, 2, 4, 2, 0, 5, 5,
	5, 0, 6, 0, 2, 7, 2, 0,
	2, 0, 2, 0, 0, 2, 0, 8,
	0, 11, 2, 2, 5, 0, 12, 12,
	0, 2, 5, 2, 5, 2, 0, 13,
	14, 2, 0, 0, 2, 0, 2, 2,
	0, 2, 2, 0, 0, 2, 2, 2,
	0, 0, 0, 2, 15, 2, 0, 0,
	2, 0, 2, 2, 0, 2, 2, 2,
	2, 0, 2, 2, 0, 0, 2, 2,
	2, 0, 0, 0, 2, 16, 5, 0,
	5, 2, 2, 0, 5, 0, 0, 2,
	5, 5, 0, 0, 0, 2, 17, 18,
	2, 0, 0, 0, 0, 2, 2, 2,
	2, 2, 0, 0, 2, 2, 2, 0,
	0, 0, 2, 0, 19, 19, 0, 0,
	0, 0, 20, 2, 0, 0, 0
};

static const char _indic_syllable_machine_to_state_actions[] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 9,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0
};

static const char _indic_syllable_machine_from_state_actions[] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 10,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0
};

static const short _indic_syllable_machine_eof_trans[] = {
	1, 1, 1, 1, 1, 1, 1, 10,
	10, 10, 10, 10, 10, 10, 10, 20,
	20, 27, 20, 27, 20, 20, 30, 30,
	30, 30, 30, 30, 30, 1, 40, 0,
	57, 57, 57, 57, 57, 57, 57, 57,
	57, 57, 57, 57, 57, 57, 57, 57,
	57, 57, 57, 57, 57, 77, 77, 77,
	77, 77, 77, 77, 77, 77, 77, 77,
	77, 77, 77, 77, 77, 77, 77, 77,
	77, 77, 77, 77, 77, 77, 77, 102,
	102, 102, 102, 102, 102, 102, 102, 102,
	102, 102, 102, 102, 27, 102, 102, 102,
	102, 102, 102, 102, 119, 119, 120, 120,
	120, 120, 120, 120, 120, 120, 120, 120,
	120, 120, 120, 120, 120, 120, 120, 120,
	120, 120, 120, 102, 57, 57, 57, 57,
	57, 57, 57, 57, 147, 147, 147, 147,
	147, 119
};

static const int indic_syllable_machine_start = 31;

#define found_syllable(syllable_type) \
  do { \
    for (unsigned int i = ts; i < te; i++) \
      SYLLABLE (&info[i]) = (syllable_serial << 4) | syllable_type; \
    syllable_serial++; \
    if (syllable_serial == 16) syllable_serial = 1; \
  } while (0)

static void
find_syllables_indic (hb_buffer_t *buffer)
{
  unsigned int p, pe, eof, ts, te, act;
  int cs;
  hb_glyph_info_t *info = buffer->info;

	{
	cs = indic_syllable_machine_start;
	ts = 0;
	te = 0;
	act = 0;
	}

  p = 0;
  pe = eof = buffer->len;

  unsigned int syllable_serial = 1;

	{
	int _slen;
	int _trans;
	const unsigned char *_keys;
	const unsigned char *_inds;
	if ( p == pe )
		goto _test_eof;
_resume:
	switch ( _indic_syllable_machine_from_state_actions[cs] ) {
	case 10:
	{ts = p;}
	break;
	}

	_keys = _indic_syllable_machine_trans_keys + (cs<<1);
	_inds = _indic_syllable_machine_indicies + _indic_syllable_machine_index_offsets[cs];

	_slen = _indic_syllable_machine_key_spans[cs];
	_trans = _inds[ _slen > 0 && _keys[0] <=( SHAPER_CATEGORY (&info[p])) &&
		( SHAPER_CATEGORY (&info[p])) <= _keys[1] ?
		( SHAPER_CATEGORY (&info[p])) - _keys[0] : _slen ];

_eof_trans:
	cs = _indic_syllable_machine_trans_targs[_trans];

	if ( _indic_syllable_machine_trans_actions[_trans] == 0 )
		goto _again;

	switch ( _indic_syllable_machine_trans_actions[_trans] ) {
	case 2:
	{te = p+1;}
	break;
	case 11:
	{te = p+1;{ found_syllable (indic_non_indic_cluster); }}
	break;
	case 14:
	{te = p;p--;{ found_syllable (indic_consonant_syllable); }}
	break;
	case 15:
	{te = p;p--;{ found_syllable (indic_vowel_syllable); }}
	break;
	case 18:
	{te = p;p--;{ found_syllable (indic_standalone_cluster); }}
	break;
	case 20:
	{te = p;p--;{ found_syllable (indic_symbol_cluster); }}
	break;
	case 16:
	{te = p;p--;{ found_syllable (indic_broken_cluster); buffer->scratch_flags |= SCRATCH_HAS_BROKEN_SYLLABLE; }}
	break;
	case 17:
	{te = p;p--;{ found_syllable (indic_non_indic_cluster); }}
	break;
	case 1:
	{{p = ((te))-1;}{ found_syllable (indic_consonant_syllable); }}
	break;
	case 3:
	{{p = ((te))-1;}{ found_syllable (indic_vowel_syllable); }}
	break;
	case 7:
	{{p = ((te))-1;}{ found_syllable (indic_standalone_cluster); }}
	break;
	case 8:
	{{p = ((te))-1;}{ found_syllable (indic_symbol_cluster); }}
	break;
	case 4:
	{{p = ((te))-1;}{ found_syllable (indic_broken_cluster); buffer->scratch_flags |= SCRATCH_HAS_BROKEN_SYLLABLE; }}
	break;
	case 6:
	{	switch( act ) {
	case 1:
	{{p = ((te))-1;} found_syllable (indic_consonant_syllable); }
	break;
	case 5:
	{{p = ((te))-1;} found_syllable (indic_non_indic_cluster); }
	break;
	case 6:
	{{p = ((te))-1;} found_syllable (indic_broken_cluster); buffer->scratch_flags |= SCRATCH_HAS_BROKEN_SYLLABLE; }
	break;
	case 7:
	{{p = ((te))-1;} found_syllable (indic_non_indic_cluster); }
	break;
	}
	}
	break;
	case 19:
	{te = p+1;}
	{act = 1;}
	break;
	case 13:
	{te = p+1;}
	{act = 5;}
	break;
	case 5:
	{te = p+1;}
	{act = 6;}
	break;
	case 12:
	{te = p+1;}
	{act = 7;}
	break;
	}

_again:
	switch ( _indic_syllable_machine_to_state_actions[cs] ) {
	case 9:
	{ts = 0;}
	break;
	}

	if ( ++p != pe )
		goto _resume;
	_test_eof: {}
	if ( p == eof )
	{
	if ( _indic_syllable_machine_eof_trans[cs] > 0 ) {
		_trans = _indic_syllable_machine_eof_trans[cs] - 1;
		goto _eof_trans;
	}
	}

	}

}

#undef found_syllable

#define OT_X I_Cat(X)
#define OT_C I_Cat(C)
#define OT_V I_Cat(V)
#define OT_N I_Cat(N)
#define OT_H I_Cat(H)
#define OT_ZWNJ I_Cat(ZWNJ)
#define OT_ZWJ I_Cat(ZWJ)
#define OT_M I_Cat(M)
#define OT_SM I_Cat(SM)
#define OT_A I_Cat(A)
#define OT_VD I_Cat(VD)
#define OT_PLACEHOLDER I_Cat(PLACEHOLDER)
#define OT_DOTTEDCIRCLE I_Cat(DOTTEDCIRCLE)
#define OT_RS I_Cat(RS)
#define OT_MPst I_Cat(MPst)
#define OT_Repha I_Cat(Repha)
#define OT_Ra I_Cat(Ra)
#define OT_CM I_Cat(CM)
#define OT_Symbol I_Cat(Symbol)
#define OT_CS I_Cat(CS)
#define OT_SMPst I_Cat(SMPst)

#define OT_VAbv 20u
#define OT_VBlw 21u
#define OT_VPre 22u
#define OT_VPst 23u
#define OT_Robatic 25u
#define OT_Xgroup 26u
#define OT_Ygroup 27u

#define OT_IV 2u
#define OT_As 32u
#define OT_DB 3u
#define OT_GB 10u
#define OT_MH 35u
#define OT_MR 36u
#define OT_MW 37u
#define OT_MY 38u
#define OT_PT 39u
#define OT_VS 40u
#define OT_ML 41u

#define _OT_A    OT_A
#define _OT_As   OT_As
#define _OT_C    OT_C
#define _OT_CM   OT_CM
#define _OT_CS   OT_CS
#define _OT_DC   OT_DOTTEDCIRCLE
#define _OT_H    OT_H
#define _OT_M    OT_M
#define _OT_MH   OT_MH
#define _OT_ML   OT_ML
#define _OT_MP   OT_MPst
#define _OT_MR   OT_MR
#define _OT_MW   OT_MW
#define _OT_MY   OT_MY
#define _OT_N    OT_N
#define _OT_GB   OT_PLACEHOLDER
#define _OT_PT   OT_PT
#define _OT_R    OT_Ra
#define _OT_Rf   OT_Repha
#define _OT_Rt   OT_Robatic
#define _OT_SM   OT_SM
#define _OT_SP   OT_SMPst
#define _OT_S    OT_Symbol
#define _OT_V    OT_V
#define _OT_VA   OT_VAbv
#define _OT_VB   OT_VBlw
#define _OT_VL   OT_VPre
#define _OT_VR   OT_VPst
#define _OT_VS   OT_VS
#define _OT_X    OT_X
#define _OT_Xg   OT_Xgroup
#define _OT_Yg   OT_Ygroup
#define _OT_ZWJ  OT_ZWJ
#define _OT_ZWNJ OT_ZWNJ

#define _POS_T   POS_ABOVE_C
#define _POS_A   POS_AFTER_MAIN
#define _POS_AP  POS_AFTER_POST
#define _POS_AS  POS_AFTER_SUB
#define _POS_C   POS_BASE_C
#define _POS_BS  POS_BEFORE_SUB
#define _POS_B   POS_BELOW_C
#define _POS_X   POS_END
#define _POS_R   POS_POST_C
#define _POS_L   POS_PRE_C
#define _POS_LM  POS_PRE_M
#define _POS_SM  POS_SMVD

#define INDIC_COMBINE_CATEGORIES(S,M) ((S) | ((M) << 8))

#define _(S,M) INDIC_COMBINE_CATEGORIES (_OT_##S, _POS_##M)

static const uint16_t _hb_indic_values[42]=
{
    _(A,SM),  _(As,X),   _(C,C),  _(CM,C),  _(CS,C),  _(DC,C),  _(GB,C),   _(H,B),
     _(H,T),   _(H,X),   _(M,A),  _(M,AP),  _(M,AS),   _(M,B),  _(M,BS),  _(M,LM),
    _(MH,X),  _(ML,X), _(MP,AP),  _(MR,X),  _(MW,X),  _(MY,X),   _(N,X),  _(PT,X),
     _(R,C),  _(Rf,X),  _(Rt,X),  _(S,SM), _(SM,BS), _(SM,SM), _(SP,SM),   _(V,C),
    _(VA,T),  _(VB,B),  _(VL,L),  _(VR,R),  _(VS,X),   _(X,X),  _(Xg,X),  _(Yg,X),
   _(ZWJ,X),_(ZWNJ,X),
};
static const uint8_t _hb_indic_u8[1224]=
{
    1,  0, 50,  4,  5, 96,  0,  7,  8,  9,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,186,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,192,
    0,  0,  0,  0,208,224,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,
    2,  3,  0,  0,  0,  0,  0,  0,  0,  0,  4,  5,  6,  7,  8,  9,
   10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,  0,  0, 22, 23,
   24,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 25, 26,  0,  0,
    0, 27,  0,  0,  0,  0, 28, 29, 30,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0, 31,  0,  0,  0, 32,  0,  0,  0, 33,  0, 34,
    0,  0,  0,  0,  0,  0, 35,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0, 36,  0,  0,  0,  0,  0,  0, 37,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0, 25,  2, 10,  0,  0,  0,  0, 26,  0,
   27,  0,  0,  0, 28,  0,  0,  0,  0,  0, 29, 11, 30,  1,  1,  1,
    4, 31, 32, 33, 34,  1,  6,  2, 35,  1, 12, 13,  7,  1,  1,  3,
   36, 14, 15, 37, 16, 17,  6,  2, 38, 39, 18, 40,  7,  1,  1,  3,
   41, 42, 43, 44, 45, 46, 19,  2, 47,  0, 18, 48, 49,  1,  1,  3,
   20, 14, 50, 51,  0,  0, 21,  2,  0, 52, 53, 13,  7,  1,  1,  3,
   20, 54, 15, 55, 56, 17,  6,  2, 57,  0, 58, 59, 60, 61, 62, 63,
    4, 64, 65, 66, 16,  0, 19,  2,  0,  0, 67,  8,  9,  1,  1,  3,
    4, 22, 68, 69, 70, 71, 23,  2,  0,  0, 12,  8,  9,  1,  1,  3,
   72, 22, 73, 74, 75, 76, 23,  2, 77,  0, 78,  8,  9,  1,  1,  1,
    4, 79, 80, 81, 82, 83, 21,  2,  0, 84, 85,  1,  1, 86, 87, 88,
   89, 90,  2, 91, 92, 93, 94, 95, 96,  1, 97, 98,  2, 99,  0,  0,
    0,  0,  1,  1,  1,100,101, 11,102,103,104,105,106,107,  2, 10,
    0,  0,  0,  0,108,  5,  5,109,110,111,  0,112,113,  0,114,  0,
    0,  0,  0,  0,  0,  0,  0,  0,115,  0,116,  0,  0,  0,  0,  0,
    0,  0,  0,117,  0,  0,  0,  0,  0,118,  0,  0,  0,  0,  5,  5,
  119,120,  0,  0,  0,  0,121,  1,  2,122,  0,  0,  0,  0,  1,  1,
  123,124, 24, 24,  0,  0,  0,  0,  0,  0,125,  0,  0,  0,  0,  0,
    0,126,  0,  0,  2,  2,  2, 10,  0,  0,  0,  0,  2,  2,  2,  2,
    4,  4,  4,  4, 10,  2,  2,  2, 26,  2,  2,  2,  8,  8,  8,  8,
    6, 18,  0,  4, 20, 14, 24,  2,  6,  6, 20,  6, 20,  6, 24,  2,
    4,  0,  0,  0,  6,  6,  6,  6, 66, 12, 14,  6,  6,  6, 20, 14,
    2,  0, 36, 50, 52, 18, 38, 68,  0,  0,  0, 32,  0,  0,  2, 16,
   56, 12, 14,  6,  0,  0,  0,  4, 44,  2, 16,  2,  6, 22,  0,  4,
    2,  0, 36, 28,  6, 28,  0,  4, 30, 30, 30, 30,  0,  0, 42,  0,
   34,  0,  0,  0,  0, 64,  0,  0,  0,  0,  0, 42, 12, 12,  6,  6,
    6,  6, 24,  2,  2, 18, 36,110, 18, 18, 18, 18, 18, 18,112,114,
  116,118,120, 18,  0,  6,  6,  6, 44, 10,  0,  2, 54, 32, 46, 10,
   26,  0,  0,  0,  0,  0, 34, 70,  6, 20,  0, 14, 44,  2, 16, 10,
    2,  0, 72, 50,122, 48,  0, 32, 48, 32, 46,  0,124,  0,  0,  0,
   16,  2, 10, 10, 12,  2,126,  0,  6,  6,  6, 14,  6, 14, 24,  2,
   22, 22, 52, 74, 76, 32, 46,  0, 16, 58, 58, 78,128, 12, 14,  6,
    2,  0, 36,130,132, 32, 46,  0,  0,134,136,138, 16,  0,  0,  0,
    0, 70, 14,  6,  6, 20,  0,  6, 20,  6, 24,  0, 16, 10, 10,  2,
    0, 16, 10,  0,  2, 10,  0,  2,  2,  0,  0, 22, 76, 48,  0, 80,
   54, 22, 82,  0, 12, 12,140,  6, 28, 60, 38, 28, 84, 28, 86,  0,
    0,  0,142, 84,  2, 10, 16,  0, 26,  2, 16,  2, 28, 60, 38, 60,
   38, 18, 86,  0,  0,  0, 74, 38,  0,  0, 16, 10,144,146,  0,  0,
   12, 12,148,  6,  2,150,152, 22, 22, 22, 48, 80, 54, 22, 82,154,
    0,  0,  2,156,  0,  0,  0, 14,  0,  2,  2,  2,  2,  2, 26,  2,
    2,158,  2,  2, 88,  6,  6,  6,  6,160, 90, 92,162,164, 62, 58,
  166,168,170,172,  4,  4,  0, 10,  2,  6,  6, 94, 96, 26,  2,174,
  176, 98,178,180, 98,100,100,  2,102, 62,182,  2,  2,184,186,188,
   12, 12, 12,190,  4, 12,192,  0,  2, 26,  2,  2,  2, 88,  6,  6,
    6,  6,  0, 90, 62, 92, 96,194,196,198, 94,200,202,104,104,106,
  106,204,  0,  0, 42,  0,206,  0,  8,208,  8,  8,210, 40,212, 40,
   40,  2,214,216,  8, 34,  0,  0,  0,  0,218,  0,  4,  4,  4,  0,
    0, 34,  0,  0,  0,  0,108,  0,  0, 64,108,  0,  0,  0,220,  0,
    0, 42,  4, 34,  8, 40, 40, 40,  0,  0,  0,222,  2,  2,102, 16,
    4,  2,  2, 10, 16,  2,  4, 34,  0,224, 78,  2, 56, 12,  0,  0,
    0,226, 72,  0, 37, 37,  2,  2,  6,  6, 31, 31,  0,  0,  2, 37,
   29, 29, 37, 31, 37,  2, 12, 12, 31, 37, 11, 11, 31,  2, 24,  2,
   14, 14, 36, 36, 37, 11,  6, 37, 22, 27, 12, 37, 27, 27, 37,  6,
   24, 37, 11,  7, 11, 37, 11, 15, 11, 12, 15, 37, 37, 29,  0, 22,
   14, 12, 32, 32, 30, 30,  6, 29, 37, 15, 29, 37, 22, 37, 37, 12,
   12, 11, 22, 22, 15, 15, 11,  8, 14, 37, 14,  8,  2, 31, 35, 32,
   32, 33, 35, 35, 33, 33, 35, 23, 23, 23,  2, 32, 26, 38, 38, 38,
   30, 37, 12, 15, 12,  7, 15, 12, 37,  0,  0, 29, 29, 12, 18, 11,
   37, 13, 37,  3, 37, 28, 11, 10, 10, 37, 37, 10, 10, 22, 10, 11,
   29, 31, 37, 14, 37,  4,  4, 29,  6, 31,  2, 12, 12, 27, 25, 37,
    2, 11,  2, 24, 31, 35, 33, 34,  0, 32, 29,  9,  1, 21, 19, 20,
   16,  2, 21, 21, 17,  2, 23,  2,  2, 35, 32,  2, 20, 35, 34, 32,
   32, 29,  2, 29, 29, 32, 32, 35, 35, 34, 34, 34, 38, 39, 39, 26,
    9, 39, 27, 39,  0, 37,  0, 27, 27,  0,  0,  4,  4,  0, 41, 40,
    5, 37, 31, 12,  2, 23, 37, 22,
};

static inline uint8_t _hb_indic_b4 (const uint8_t* a, unsigned i)
{
  return (a[i>>1]>>((i&1)<<2))&15;
}
static inline uint8_t _hb_indic_get_categories_index (unsigned u)
{

  return u<71396u ? (uint8_t)(_hb_indic_u8[996u+_hb_indic_u8[488u+((_hb_indic_u8[186u+((_hb_indic_u8[70u+((_hb_indic_b4(_hb_indic_u8,((((((((u)>>1))>>2))>>3))>>3)))<<3)+((((((((u)>>1))>>2))>>3))&7)])<<3)+((((((u)>>1))>>2))&7)])<<2)+((((u)>>1))&3)]+((u)&1)]) : 37;
}

static uint16_t
hb_indic_get_categories (hb_codepoint_t u)
{
  return _hb_indic_values[_hb_indic_get_categories_index (u)];
}

#define INDIC_POS(i) SHAPER_AUX (i)

static int would_match_input (const hb_codepoint_t *glyphs, unsigned len, unsigned count, const uint8_t *input, match_func_t f, const void *data)
{
  if (count != len) return 0;
  for (unsigned i = 1; i < count; i++)
  {
    hb_glyph_info_t info;
    info.codepoint = glyphs[i];
    if (!f (&info, rd16 (input + (i - 1) * 2), data)) return 0;
  }
  return 1;
}

static int subtable_would_apply (const uint8_t *st, unsigned type, const hb_codepoint_t *glyphs, unsigned len, int zero_context)
{
  if (!st) return 0;
  unsigned fmt = rd16 (st);
  switch (type)
  {
  case 1: case 2: case 3: case 8:
    if ((type == 1 && fmt != 1 && fmt != 2) || (type != 1 && fmt != 1)) return 0;
    return len == 1 && cov_get (off16 (st, st + 2), glyphs[0]) != NOT_COVERED;
  case 4:
  {
    if (fmt != 1) return 0;
    unsigned index = cov_get (off16 (st, st + 2), glyphs[0]);
    if (index == NOT_COVERED) return 0;
    const uint8_t *ls = arr_off16 (st, st + 4, index);
    unsigned n = ls ? rd16 (ls) : 0;
    for (unsigned i = 0; i < n; i++)
    {
      const uint8_t *lig = off16 (ls, ls + 2 + i * 2);
      unsigned count = lig ? rd16 (lig + 2) : 0;
      if (len != count) continue;
      unsigned k;
      for (k = 1; k < len; k++)
	if (glyphs[k] != rd16 (lig + 4 + (k - 1) * 2)) break;
      if (k >= len) return 1;
    }
    return 0;
  }
  case 5:
  {
    if (fmt == 3) return would_match_input (glyphs, len, rd16 (st + 2), st + 8, match_coverage, st);
    if (fmt != 1 && fmt != 2) return 0;
    const uint8_t *cd = fmt == 2 ? off16 (st, st + 4) : NULL;
    const uint8_t *rs = fmt == 1 ? arr_off16 (st, st + 4, cov_get (off16 (st, st + 2), glyphs[0])) : arr_off16 (st, st + 6, classdef_get (cd, glyphs[0]));
    unsigned n = rs ? rd16 (rs) : 0;
    for (unsigned i = 0; i < n; i++)
    {
      const uint8_t *r = off16 (rs, rs + 2 + i * 2);
      if (would_match_input (glyphs, len, r ? rd16 (r) : 0, r ? r + 4 : NULL, fmt == 1 ? match_glyph : match_class, cd)) return 1;
    }
    return 0;
  }
  case 6:
  {
    if (fmt == 3)
    {
      const uint8_t *bt = st + 2, *in = bt + 2 + rd16 (bt) * 2, *la = in + 2 + rd16 (in) * 2;
      return (zero_context ? !rd16 (bt) && !rd16 (la) : 1) && would_match_input (glyphs, len, rd16 (in), in + 4, match_coverage, st);
    }
    if (fmt != 1 && fmt != 2) return 0;
    const uint8_t *icd = fmt == 2 ? off16 (st, st + 6) : NULL;
    const uint8_t *rs = fmt == 1 ? arr_off16 (st, st + 4, cov_get (off16 (st, st + 2), glyphs[0])) : arr_off16 (st, st + 10, classdef_get (icd, glyphs[0]));
    unsigned n = rs ? rd16 (rs) : 0;
    for (unsigned i = 0; i < n; i++)
    {
      const uint8_t *r = off16 (rs, rs + 2 + i * 2);
      unsigned blen = r ? rd16 (r) : 0;
      const uint8_t *in = r ? r + 2 + blen * 2 : NULL;
      unsigned ilen = in ? rd16 (in) : 0;
      const uint8_t *la = in ? in + 2 + (ilen ? ilen - 1 : 0) * 2 : NULL;
      unsigned llen = la ? rd16 (la) : 0;
      if ((zero_context ? !blen && !llen : 1) && would_match_input (glyphs, len, ilen, in ? in + 2 : NULL, fmt == 1 ? match_glyph : match_class, icd)) return 1;
    }
    return 0;
  }
  case 7:
    if (fmt != 1) return 0;
    return subtable_would_apply (off32 (st, st + 4), rd16 (st + 2), glyphs, len, zero_context);
  }
  return 0;
}

static int lookup_would_substitute (layout_t *l, unsigned lookup_index, const hb_codepoint_t *glyphs, unsigned len, int zero_context)
{
  gtab_t *g = &l->g[0];
  if (lookup_index >= g->lookup_count) return 0;
  laccel_t *a = get_accel (g, 0, lookup_index);
  if (!a || !len || !digest_may_have (&a->digest, glyphs[0])) return 0;
  const uint8_t *L = gtab_lookup (g, lookup_index);
  unsigned n = L ? rd16 (L + 4) : 0;
  for (unsigned i = 0; i < n; i++)
    if (subtable_would_apply (off16 (L, L + 6 + i * 2), rd16 (L), glyphs, len, zero_context)) return 1;
  return 0;
}

enum { REPH_MODE_IMPLICIT, REPH_MODE_EXPLICIT, REPH_MODE_LOG_REPHA };
enum { BLWF_MODE_PRE_AND_POST, BLWF_MODE_POST_ONLY };

typedef struct
{
  hb_script_t script;
  int has_old_spec;
  hb_codepoint_t virama;
  unsigned reph_pos, reph_mode, blwf_mode;
} indic_config_t;

static const indic_config_t indic_configs[] =
{
  {HB_SCRIPT_INVALID, 0, 0, POS_BEFORE_POST, REPH_MODE_IMPLICIT, BLWF_MODE_PRE_AND_POST},
  {HB_SCRIPT_DEVANAGARI, 1, 0x094Du, POS_BEFORE_POST, REPH_MODE_IMPLICIT, BLWF_MODE_PRE_AND_POST},
  {HB_SCRIPT_BENGALI, 1, 0x09CDu, POS_AFTER_SUB, REPH_MODE_IMPLICIT, BLWF_MODE_PRE_AND_POST},
  {HB_SCRIPT_GURMUKHI, 1, 0x0A4Du, POS_BEFORE_SUB, REPH_MODE_IMPLICIT, BLWF_MODE_PRE_AND_POST},
  {HB_SCRIPT_GUJARATI, 1, 0x0ACDu, POS_BEFORE_POST, REPH_MODE_IMPLICIT, BLWF_MODE_PRE_AND_POST},
  {HB_SCRIPT_ORIYA, 1, 0x0B4Du, POS_AFTER_MAIN, REPH_MODE_IMPLICIT, BLWF_MODE_PRE_AND_POST},
  {HB_SCRIPT_TAMIL, 1, 0x0BCDu, POS_AFTER_POST, REPH_MODE_IMPLICIT, BLWF_MODE_PRE_AND_POST},
  {HB_SCRIPT_TELUGU, 1, 0x0C4Du, POS_AFTER_POST, REPH_MODE_EXPLICIT, BLWF_MODE_POST_ONLY},
  {HB_SCRIPT_KANNADA, 1, 0x0CCDu, POS_AFTER_POST, REPH_MODE_IMPLICIT, BLWF_MODE_POST_ONLY},
  {HB_SCRIPT_MALAYALAM, 1, 0x0D4Du, POS_AFTER_MAIN, REPH_MODE_LOG_REPHA, BLWF_MODE_PRE_AND_POST},
};

static const struct { hb_tag_t tag; unsigned flags; } indic_features[] =
{
  {HB_TAG ('n','u','k','t'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('a','k','h','n'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('r','p','h','f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('r','k','r','f'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('p','r','e','f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('b','l','w','f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('a','b','v','f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('h','a','l','f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('p','s','t','f'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('v','a','t','u'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('c','j','c','t'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('i','n','i','t'), F_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('p','r','e','s'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('a','b','v','s'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('b','l','w','s'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('p','s','t','s'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
  {HB_TAG ('h','a','l','n'), F_GLOBAL_MANUAL_JOINERS | F_PER_SYLLABLE},
};

enum
{
  INDIC_NUKT, INDIC_AKHN, INDIC_RPHF, INDIC_RKRF, INDIC_PREF, INDIC_BLWF, INDIC_ABVF, INDIC_HALF, INDIC_PSTF,
  INDIC_VATU, INDIC_CJCT, INDIC_INIT, INDIC_PRES, INDIC_ABVS, INDIC_BLWS, INDIC_PSTS, INDIC_HALN,
  INDIC_NUM_FEATURES, INDIC_BASIC_FEATURES = INDIC_INIT
};

typedef struct { const lookup_map_t *lookups; unsigned count; int zero_context; } would_substitute_t;

typedef struct
{
  const indic_config_t *config;
  int is_old_spec;
  hb_codepoint_t virama_glyph;
  would_substitute_t rphf, pref, blwf, pstf, vatu;
  hb_mask_t mask_array[INDIC_NUM_FEATURES];
} indic_plan_t;

static void would_substitute_init (would_substitute_t *w, const shape_plan_t *plan, hb_tag_t tag, int zero_context)
{
  const feature_map_t *f = plan_feature (plan, tag);
  unsigned stage = f ? f->stage[0] : UINT32_MAX;
  w->zero_context = zero_context;
  w->lookups = NULL;
  w->count = 0;
  if (stage > plan->stages[0].len) return;
  unsigned start = stage ? plan->stages[0].a[stage - 1].last_lookup : 0;
  unsigned end = stage < plan->stages[0].len ? plan->stages[0].a[stage].last_lookup : plan->lookups[0].len;
  w->lookups = plan->lookups[0].a + start;
  w->count = end - start;
}

static int would_substitute (const would_substitute_t *w, const hb_codepoint_t *glyphs, unsigned count, hb_face_t *face)
{
  layout_t *l = face_layout (face);
  for (unsigned i = 0; i < w->count; i++)
    if (lookup_would_substitute (l, w->lookups[i].index, glyphs, count, w->zero_context)) return 1;
  return 0;
}

static int is_one_of (const hb_glyph_info_t *info, unsigned flags)
{
  if (info_ligated (info)) return 0;
  return !!(FLAG_UNSAFE (SHAPER_CATEGORY (info)) & flags);
}

#define CONSONANT_FLAGS_INDIC (FLAG (I_Cat (C)) | FLAG (I_Cat (CS)) | FLAG (I_Cat (Ra)) | FLAG (I_Cat (CM)) | FLAG (I_Cat (V)) | FLAG (I_Cat (PLACEHOLDER)) | FLAG (I_Cat (DOTTEDCIRCLE)))
#define JOINER_FLAGS (FLAG (I_Cat (ZWJ)) | FLAG (I_Cat (ZWNJ)))
static int is_consonant (const hb_glyph_info_t *info) { return is_one_of (info, CONSONANT_FLAGS_INDIC); }
static int is_joiner (const hb_glyph_info_t *info) { return is_one_of (info, JOINER_FLAGS); }
static int is_halant (const hb_glyph_info_t *info) { return is_one_of (info, FLAG (I_Cat (H))); }

static int insert_dotted_circles (hb_font_t *font, hb_buffer_t *buffer, unsigned broken_syllable_type, unsigned dottedcircle_category,
				  int repha_category, int dottedcircle_position)
{
  if (!(buffer->scratch_flags & SCRATCH_HAS_BROKEN_SYLLABLE)) return 0;
  hb_codepoint_t dottedcircle_glyph;
  if (!nominal_glyph (font, 0x25CCu, &dottedcircle_glyph)) return 0;
  hb_glyph_info_t dottedcircle;
  memset (&dottedcircle, 0, sizeof dottedcircle);
  SHAPER_CATEGORY (&dottedcircle) = dottedcircle_category;
  if (dottedcircle_position != -1) SHAPER_AUX (&dottedcircle) = dottedcircle_position;
  dottedcircle.codepoint = dottedcircle_glyph;
  buf_clear_output (buffer);
  buffer->idx = 0;
  unsigned last_syllable = 0;
  while (buffer->idx < buffer->len && buffer->successful)
  {
    unsigned syllable = SYLLABLE (buf_cur (buffer));
    if (last_syllable != syllable && (syllable & 0x0F) == broken_syllable_type)
    {
      last_syllable = syllable;
      hb_glyph_info_t ginfo = dottedcircle;
      ginfo.cluster = buf_cur (buffer)->cluster;
      ginfo.mask = buf_cur (buffer)->mask;
      SYLLABLE (&ginfo) = SYLLABLE (buf_cur (buffer));
      if (repha_category != -1)
	while (buffer->idx < buffer->len && buffer->successful && last_syllable == SYLLABLE (buf_cur (buffer)) &&
	       SHAPER_CATEGORY (buf_cur (buffer)) == (unsigned) repha_category)
	  buf_next_glyph (buffer);
      buf_output_info (buffer, ginfo);
    }
    else
      buf_next_glyph (buffer);
  }
  buf_sync (buffer);
  return 1;
}

static int setup_syllables_indic (const shape_plan_t *plan, hb_font_t *font, hb_buffer_t *buffer)
{
  find_syllables_indic (buffer);
  foreach_syllable (buffer, start, end) buf_unsafe_to_break (buffer, start, end);
  return 0;
}

static int initial_reordering_indic (const shape_plan_t *plan, hb_font_t *font, hb_buffer_t *buffer);
static int final_reordering_indic (const shape_plan_t *plan, hb_font_t *font, hb_buffer_t *buffer);

static void collect_features_indic (map_builder_t *m)
{
  map_add_gsub_pause (m, setup_syllables_indic);
  map_enable_feature (m, HB_TAG ('l','o','c','l'), F_PER_SYLLABLE, 1);
  map_enable_feature (m, HB_TAG ('c','c','m','p'), F_PER_SYLLABLE, 1);
  unsigned i = 0;
  map_add_gsub_pause (m, initial_reordering_indic);
  for (; i < INDIC_BASIC_FEATURES; i++)
  {
    map_add_feature (m, indic_features[i].tag, indic_features[i].flags, 1);
    map_add_gsub_pause (m, NULL);
  }
  map_add_gsub_pause (m, final_reordering_indic);
  for (; i < INDIC_NUM_FEATURES; i++) map_add_feature (m, indic_features[i].tag, indic_features[i].flags, 1);
}

static void override_features_indic (map_builder_t *m)
{
  map_add_feature (m, HB_TAG ('l','i','g','a'), F_GLOBAL, 0);
  map_add_gsub_pause (m, NULL);
}

static void *data_create_indic (const shape_plan_t *plan)
{
  indic_plan_t *ip = (indic_plan_t *) calloc (1, sizeof (indic_plan_t));
  if (!ip) return NULL;
  ip->config = &indic_configs[0];
  for (unsigned i = 1; i < sizeof (indic_configs) / sizeof (indic_configs[0]); i++)
    if (plan->script == indic_configs[i].script)
    {
      ip->config = &indic_configs[i];
      break;
    }
  ip->is_old_spec = ip->config->has_old_spec && ((plan->chosen_script[0] & 0xFFu) != '2');
  ip->virama_glyph = (hb_codepoint_t) -1;
  int zero_context = !ip->is_old_spec && plan->script != HB_SCRIPT_MALAYALAM;
  would_substitute_init (&ip->rphf, plan, HB_TAG ('r','p','h','f'), zero_context);
  would_substitute_init (&ip->pref, plan, HB_TAG ('p','r','e','f'), zero_context);
  would_substitute_init (&ip->blwf, plan, HB_TAG ('b','l','w','f'), zero_context);
  would_substitute_init (&ip->pstf, plan, HB_TAG ('p','s','t','f'), zero_context);
  would_substitute_init (&ip->vatu, plan, HB_TAG ('v','a','t','u'), zero_context);
  for (unsigned i = 0; i < INDIC_NUM_FEATURES; i++)
    ip->mask_array[i] = (indic_features[i].flags & F_GLOBAL) ? 0 : plan_get_1_mask (plan, indic_features[i].tag);
  return ip;
}

static unsigned consonant_position_from_face (const indic_plan_t *ip, hb_codepoint_t consonant, hb_codepoint_t virama, hb_face_t *face)
{
  hb_codepoint_t glyphs[3] = {virama, consonant, virama};
  if (would_substitute (&ip->blwf, glyphs, 2, face) || would_substitute (&ip->blwf, glyphs + 1, 2, face) ||
      would_substitute (&ip->vatu, glyphs, 2, face) || would_substitute (&ip->vatu, glyphs + 1, 2, face))
    return POS_BELOW_C;
  if (would_substitute (&ip->pstf, glyphs, 2, face) || would_substitute (&ip->pstf, glyphs + 1, 2, face)) return POS_POST_C;
  if (would_substitute (&ip->pref, glyphs, 2, face) || would_substitute (&ip->pref, glyphs + 1, 2, face)) return POS_POST_C;
  return POS_BASE_C;
}

static void setup_masks_indic (const shape_plan_t *plan, hb_buffer_t *buffer, hb_font_t *font)
{
  for (unsigned i = 0; i < buffer->len; i++)
  {
    unsigned type = hb_indic_get_categories (buffer->info[i].codepoint);
    SHAPER_CATEGORY (&buffer->info[i]) = type & 0xFF;
    INDIC_POS (&buffer->info[i]) = type >> 8;
  }
}

static int compare_indic_order (const hb_glyph_info_t *pa, const hb_glyph_info_t *pb) { return (int) INDIC_POS (pa) - (int) INDIC_POS (pb); }

static void stable_sort (hb_glyph_info_t *array, unsigned len, int (*compar) (const hb_glyph_info_t *, const hb_glyph_info_t *))
{
  for (unsigned i = 1; i < len; i++)
  {
    unsigned j = i;
    while (j && compar (&array[j - 1], &array[i]) > 0) j--;
    if (i == j) continue;
    hb_glyph_info_t t = array[i];
    memmove (&array[j + 1], &array[j], (i - j) * sizeof (hb_glyph_info_t));
    array[j] = t;
  }
}

static void initial_reordering_consonant_syllable (const shape_plan_t *plan, hb_face_t *face, hb_buffer_t *buffer, unsigned start, unsigned end)
{
  const indic_plan_t *ip = (const indic_plan_t *) plan->data;
  hb_glyph_info_t *info = buffer->info;
  if (buffer->script == HB_SCRIPT_KANNADA && start + 3 <= end &&
      is_one_of (&info[start], FLAG (I_Cat (Ra))) && is_one_of (&info[start + 1], FLAG (I_Cat (H))) && is_one_of (&info[start + 2], FLAG (I_Cat (ZWJ))))
  {
    buf_merge_clusters (buffer, start + 1, start + 3);
    hb_glyph_info_t t = info[start + 1];
    info[start + 1] = info[start + 2];
    info[start + 2] = t;
  }
  unsigned base = end;
  int has_reph = 0;
  {
    unsigned limit = start;
    if (ip->mask_array[INDIC_RPHF] && start + 3 <= end &&
	((ip->config->reph_mode == REPH_MODE_IMPLICIT && !is_joiner (&info[start + 2])) ||
	 (ip->config->reph_mode == REPH_MODE_EXPLICIT && SHAPER_CATEGORY (&info[start + 2]) == I_Cat (ZWJ))))
    {
      hb_codepoint_t glyphs[3] = {info[start].codepoint, info[start + 1].codepoint,
				  ip->config->reph_mode == REPH_MODE_EXPLICIT ? info[start + 2].codepoint : 0};
      if (would_substitute (&ip->rphf, glyphs, 2, face) ||
	  (ip->config->reph_mode == REPH_MODE_EXPLICIT && would_substitute (&ip->rphf, glyphs, 3, face)))
      {
	limit += 2;
	while (limit < end && is_joiner (&info[limit])) limit++;
	base = start;
	has_reph = 1;
      }
    }
    else if (ip->config->reph_mode == REPH_MODE_LOG_REPHA && SHAPER_CATEGORY (&info[start]) == I_Cat (Repha))
    {
      limit += 1;
      while (limit < end && is_joiner (&info[limit])) limit++;
      base = start;
      has_reph = 1;
    }
    {
      unsigned i = end;
      int seen_below = 0;
      do
      {
	i--;
	if (is_consonant (&info[i]))
	{
	  if (INDIC_POS (&info[i]) != POS_BELOW_C && (INDIC_POS (&info[i]) != POS_POST_C || seen_below))
	  {
	    base = i;
	    break;
	  }
	  if (INDIC_POS (&info[i]) == POS_BELOW_C) seen_below = 1;
	  base = i;
	}
	else if (start < i && SHAPER_CATEGORY (&info[i]) == I_Cat (ZWJ) && SHAPER_CATEGORY (&info[i - 1]) == I_Cat (H))
	  break;
      }
      while (i > limit);
    }
    if (has_reph && base == start && limit - base <= 2) has_reph = 0;
  }
  for (unsigned i = start; i < base; i++) INDIC_POS (&info[i]) = HB_MIN ((unsigned) POS_PRE_C, (unsigned) INDIC_POS (&info[i]));
  if (base < end) INDIC_POS (&info[base]) = POS_BASE_C;
  if (has_reph) INDIC_POS (&info[start]) = POS_RA_TO_BECOME_REPH;
  if (ip->is_old_spec)
  {
    int disallow_double_halants = buffer->script == HB_SCRIPT_KANNADA;
    for (unsigned i = base + 1; i < end; i++)
      if (SHAPER_CATEGORY (&info[i]) == I_Cat (H))
      {
	unsigned j;
	for (j = end - 1; j > i; j--)
	  if (is_consonant (&info[j]) || (disallow_double_halants && SHAPER_CATEGORY (&info[j]) == I_Cat (H))) break;
	if (SHAPER_CATEGORY (&info[j]) != I_Cat (H) && j > i)
	{
	  hb_glyph_info_t t = info[i];
	  memmove (&info[i], &info[i + 1], (j - i) * sizeof (info[0]));
	  info[j] = t;
	}
	break;
      }
  }
  {
    unsigned last_pos = POS_START;
    for (unsigned i = start; i < end; i++)
    {
      if (FLAG_UNSAFE (SHAPER_CATEGORY (&info[i])) & (JOINER_FLAGS | FLAG (I_Cat (N)) | FLAG (I_Cat (RS)) | FLAG (I_Cat (CM)) | FLAG (I_Cat (H))))
      {
	INDIC_POS (&info[i]) = last_pos;
	if (SHAPER_CATEGORY (&info[i]) == I_Cat (H) && INDIC_POS (&info[i]) == POS_PRE_M)
	  for (unsigned j = i; j > start; j--)
	    if (INDIC_POS (&info[j - 1]) != POS_PRE_M)
	    {
	      INDIC_POS (&info[i]) = INDIC_POS (&info[j - 1]);
	      break;
	    }
      }
      else if (INDIC_POS (&info[i]) != POS_SMVD)
      {
	if (SHAPER_CATEGORY (&info[i]) == I_Cat (MPst) && i > start && SHAPER_CATEGORY (&info[i - 1]) == I_Cat (SM))
	  INDIC_POS (&info[i - 1]) = INDIC_POS (&info[i]);
	last_pos = INDIC_POS (&info[i]);
      }
    }
  }
  {
    unsigned last = base;
    for (unsigned i = base + 1; i < end; i++)
      if (is_consonant (&info[i]))
      {
	for (unsigned j = last + 1; j < i; j++)
	  if (INDIC_POS (&info[j]) < POS_SMVD) INDIC_POS (&info[j]) = INDIC_POS (&info[i]);
	last = i;
      }
      else if (FLAG_UNSAFE (SHAPER_CATEGORY (&info[i])) & (FLAG (I_Cat (M)) | FLAG (I_Cat (MPst))))
	last = i;
  }
  {
    unsigned syllable = SYLLABLE (&info[start]);
    for (unsigned i = start; i < end; i++) SYLLABLE (&info[i]) = i - start;
    stable_sort (info + start, end - start, compare_indic_order);
    unsigned first_left_matra = end, last_left_matra = end;
    base = end;
    for (unsigned i = start; i < end; i++)
    {
      if (INDIC_POS (&info[i]) == POS_BASE_C)
      {
	base = i;
	break;
      }
      else if (INDIC_POS (&info[i]) == POS_PRE_M)
      {
	if (first_left_matra == end) first_left_matra = i;
	last_left_matra = i;
      }
    }
    if (first_left_matra < last_left_matra)
    {
      buf_reverse_range (buffer, first_left_matra, last_left_matra + 1);
      unsigned i = first_left_matra;
      for (unsigned j = i; j <= last_left_matra; j++)
	if (FLAG_UNSAFE (SHAPER_CATEGORY (&info[j])) & (FLAG (I_Cat (M)) | FLAG (I_Cat (MPst))))
	{
	  buf_reverse_range (buffer, i, j + 1);
	  i = j + 1;
	}
    }
    if (ip->is_old_spec || end - start > 127)
      buf_merge_clusters (buffer, base, end);
    else
      for (unsigned i = base; i < end; i++)
	if (SYLLABLE (&info[i]) != 255)
	{
	  unsigned min = i, max = i, j = start + SYLLABLE (&info[i]);
	  while (j != i)
	  {
	    min = HB_MIN (min, j);
	    max = HB_MAX (max, j);
	    unsigned next = start + SYLLABLE (&info[j]);
	    SYLLABLE (&info[j]) = 255;
	    j = next;
	  }
	  buf_merge_clusters (buffer, HB_MAX (base, min), max + 1);
	}
    for (unsigned i = start; i < end; i++) SYLLABLE (&info[i]) = syllable;
  }
  {
    hb_mask_t mask;
    for (unsigned i = start; i < end && INDIC_POS (&info[i]) == POS_RA_TO_BECOME_REPH; i++) info[i].mask |= ip->mask_array[INDIC_RPHF];
    mask = ip->mask_array[INDIC_HALF];
    if (!ip->is_old_spec && ip->config->blwf_mode == BLWF_MODE_PRE_AND_POST) mask |= ip->mask_array[INDIC_BLWF];
    for (unsigned i = start; i < base; i++) info[i].mask |= mask;
    mask = ip->mask_array[INDIC_BLWF] | ip->mask_array[INDIC_ABVF] | ip->mask_array[INDIC_PSTF];
    for (unsigned i = base + 1; i < end; i++) info[i].mask |= mask;
  }
  if (ip->is_old_spec && buffer->script == HB_SCRIPT_DEVANAGARI)
    for (unsigned i = start; i + 1 < base; i++)
      if (SHAPER_CATEGORY (&info[i]) == I_Cat (Ra) && SHAPER_CATEGORY (&info[i + 1]) == I_Cat (H) &&
	  (i + 2 == base || SHAPER_CATEGORY (&info[i + 2]) != I_Cat (ZWJ)))
      {
	info[i].mask |= ip->mask_array[INDIC_BLWF];
	info[i + 1].mask |= ip->mask_array[INDIC_BLWF];
      }
  unsigned pref_len = 2;
  if (ip->mask_array[INDIC_PREF] && base + pref_len < end)
    for (unsigned i = base + 1; i + pref_len - 1 < end; i++)
    {
      hb_codepoint_t glyphs[2];
      for (unsigned j = 0; j < pref_len; j++) glyphs[j] = info[i + j].codepoint;
      if (would_substitute (&ip->pref, glyphs, pref_len, face))
      {
	for (unsigned j = 0; j < pref_len; j++) info[i++].mask |= ip->mask_array[INDIC_PREF];
	break;
      }
    }
  for (unsigned i = start + 1; i < end; i++)
    if (is_joiner (&info[i]))
    {
      int non_joiner = SHAPER_CATEGORY (&info[i]) == I_Cat (ZWNJ);
      unsigned j = i;
      do
      {
	j--;
	if (non_joiner) info[j].mask &= ~ip->mask_array[INDIC_HALF];
      }
      while (j > start && !is_consonant (&info[j]));
    }
}

static int initial_reordering_indic (const shape_plan_t *plan, hb_font_t *font, hb_buffer_t *buffer)
{
  indic_plan_t *ip = (indic_plan_t *) plan->data;
  int ret = 0;
  hb_codepoint_t virama = ip->virama_glyph;
  if (virama == (hb_codepoint_t) -1)
  {
    if (!ip->config->virama || !nominal_glyph (font, ip->config->virama, &virama)) virama = 0;
    ip->virama_glyph = virama;
  }
  if (virama)
    for (unsigned i = 0; i < buffer->len; i++)
      if (INDIC_POS (&buffer->info[i]) == POS_BASE_C)
	INDIC_POS (&buffer->info[i]) = consonant_position_from_face (ip, buffer->info[i].codepoint, virama, font->face);
  if (insert_dotted_circles (font, buffer, indic_broken_cluster, I_Cat (DOTTEDCIRCLE), I_Cat (Repha), POS_END)) ret = 1;
  foreach_syllable (buffer, start, end)
    switch (SYLLABLE (&buffer->info[start]) & 0x0F)
    {
    case indic_vowel_syllable: case indic_consonant_syllable: case indic_broken_cluster: case indic_standalone_cluster:
      initial_reordering_consonant_syllable (plan, font->face, buffer, start, end);
      break;
    }
  return ret;
}

static void final_reordering_syllable_indic (const shape_plan_t *plan, hb_buffer_t *buffer, unsigned start, unsigned end)
{
  const indic_plan_t *ip = (const indic_plan_t *) plan->data;
  hb_glyph_info_t *info = buffer->info;
  hb_codepoint_t virama_glyph = ip->virama_glyph;
  if (virama_glyph)
    for (unsigned i = start; i < end; i++)
      if (info[i].codepoint == virama_glyph && info_ligated (&info[i]) && info_multiplied (&info[i]))
      {
	SHAPER_CATEGORY (&info[i]) = I_Cat (H);
	info_clear_ligated_and_multiplied (&info[i]);
      }
  int try_pref = !!ip->mask_array[INDIC_PREF];
  unsigned base;
  for (base = start; base < end; base++)
    if (INDIC_POS (&info[base]) >= POS_BASE_C)
    {
      if (try_pref && base + 1 < end)
      {
	for (unsigned i = base + 1; i < end; i++)
	  if ((info[i].mask & ip->mask_array[INDIC_PREF]) != 0)
	  {
	    if (!(info_substituted (&info[i]) && info_ligated_and_didnt_multiply (&info[i])))
	    {
	      base = i;
	      while (base < end && is_halant (&info[base])) base++;
	      if (base < end) INDIC_POS (&info[base]) = POS_BASE_C;
	      try_pref = 0;
	    }
	    break;
	  }
	if (base == end) break;
      }
      if (buffer->script == HB_SCRIPT_MALAYALAM)
	for (unsigned i = base + 1; i < end; i++)
	{
	  while (i < end && is_joiner (&info[i])) i++;
	  if (i == end || !is_halant (&info[i])) break;
	  i++;
	  while (i < end && is_joiner (&info[i])) i++;
	  if (i < end && is_consonant (&info[i]) && INDIC_POS (&info[i]) == POS_BELOW_C)
	  {
	    base = i;
	    INDIC_POS (&info[base]) = POS_BASE_C;
	  }
	}
      if (start < base && INDIC_POS (&info[base]) > POS_BASE_C) base--;
      break;
    }
  if (base == end && start < base && is_one_of (&info[base - 1], FLAG (I_Cat (ZWJ)))) base--;
  if (base < end)
    while (start < base && is_one_of (&info[base], FLAG (I_Cat (N)) | FLAG (I_Cat (H)))) base--;
  if (start + 1 < end && start < base)
  {
    unsigned new_pos = base == end ? base - 2 : base - 1;
    if (buffer->script != HB_SCRIPT_MALAYALAM && buffer->script != HB_SCRIPT_TAMIL)
    {
    search:
      while (new_pos > start && !is_one_of (&info[new_pos], FLAG (I_Cat (M)) | FLAG (I_Cat (MPst)) | FLAG (I_Cat (H)))) new_pos--;
      if (is_halant (&info[new_pos]) && INDIC_POS (&info[new_pos]) != POS_PRE_M)
      {
	if (new_pos + 1 < end && SHAPER_CATEGORY (&info[new_pos + 1]) == I_Cat (ZWJ) && new_pos > start)
	{
	  new_pos--;
	  goto search;
	}
      }
      else
	new_pos = start;
    }
    if (start < new_pos && INDIC_POS (&info[new_pos]) != POS_PRE_M)
    {
      for (unsigned i = new_pos; i > start; i--)
	if (INDIC_POS (&info[i - 1]) == POS_PRE_M)
	{
	  unsigned old_pos = i - 1;
	  if (old_pos < base && base <= new_pos) base--;
	  hb_glyph_info_t tmp = info[old_pos];
	  memmove (&info[old_pos], &info[old_pos + 1], (new_pos - old_pos) * sizeof (info[0]));
	  info[new_pos] = tmp;
	  buf_merge_clusters (buffer, new_pos, HB_MIN (end, base + 1));
	  new_pos--;
	}
    }
    else
      for (unsigned i = start; i < base; i++)
	if (INDIC_POS (&info[i]) == POS_PRE_M)
	{
	  buf_merge_clusters (buffer, i, HB_MIN (end, base + 1));
	  break;
	}
  }
  if (start + 1 < end && INDIC_POS (&info[start]) == POS_RA_TO_BECOME_REPH &&
      ((SHAPER_CATEGORY (&info[start]) == I_Cat (Repha)) ^ info_ligated_and_didnt_multiply (&info[start])))
  {
    unsigned new_reph_pos;
    unsigned reph_pos = ip->config->reph_pos;
    if (reph_pos == POS_AFTER_POST) goto reph_step_5;
    new_reph_pos = start + 1;
    while (new_reph_pos < base && !is_halant (&info[new_reph_pos])) new_reph_pos++;
    if (new_reph_pos < base && is_halant (&info[new_reph_pos]))
    {
      if (new_reph_pos + 1 < base && is_joiner (&info[new_reph_pos + 1])) new_reph_pos++;
      goto reph_move;
    }
    if (reph_pos == POS_AFTER_MAIN)
    {
      new_reph_pos = base;
      while (new_reph_pos + 1 < end && INDIC_POS (&info[new_reph_pos + 1]) <= POS_AFTER_MAIN) new_reph_pos++;
      if (new_reph_pos < end) goto reph_move;
    }
    if (reph_pos == POS_AFTER_SUB)
    {
      new_reph_pos = base;
      while (new_reph_pos + 1 < end && !(FLAG_UNSAFE (INDIC_POS (&info[new_reph_pos + 1])) & (FLAG (POS_POST_C) | FLAG (POS_AFTER_POST) | FLAG (POS_SMVD))))
	new_reph_pos++;
      if (new_reph_pos < end) goto reph_move;
    }
  reph_step_5:
    new_reph_pos = start + 1;
    while (new_reph_pos < base && !is_halant (&info[new_reph_pos])) new_reph_pos++;
    if (new_reph_pos < base && is_halant (&info[new_reph_pos]))
    {
      if (new_reph_pos + 1 < base && is_joiner (&info[new_reph_pos + 1])) new_reph_pos++;
      goto reph_move;
    }
    new_reph_pos = end - 1;
    while (new_reph_pos > start && INDIC_POS (&info[new_reph_pos]) == POS_SMVD) new_reph_pos--;
    if (is_halant (&info[new_reph_pos]))
      for (unsigned i = base + 1; i < new_reph_pos; i++)
	if (FLAG_UNSAFE (SHAPER_CATEGORY (&info[i])) & (FLAG (I_Cat (M)) | FLAG (I_Cat (MPst)))) new_reph_pos--;
  reph_move:
    buf_merge_clusters (buffer, start, new_reph_pos + 1);
    {
      hb_glyph_info_t reph = info[start];
      memmove (&info[start], &info[start + 1], (new_reph_pos - start) * sizeof (info[0]));
      info[new_reph_pos] = reph;
    }
    if (start < base && base <= new_reph_pos) base--;
  }
  if (try_pref && base + 1 < end)
    for (unsigned i = base + 1; i < end; i++)
      if ((info[i].mask & ip->mask_array[INDIC_PREF]) != 0)
      {
	if (info_ligated_and_didnt_multiply (&info[i]))
	{
	  unsigned new_pos = base;
	  if (buffer->script != HB_SCRIPT_MALAYALAM && buffer->script != HB_SCRIPT_TAMIL)
	    while (new_pos > start && !is_one_of (&info[new_pos - 1], FLAG (I_Cat (M)) | FLAG (I_Cat (MPst)) | FLAG (I_Cat (H)))) new_pos--;
	  if (new_pos > start && is_halant (&info[new_pos - 1]) && new_pos < end && is_joiner (&info[new_pos])) new_pos++;
	  unsigned old_pos = i;
	  buf_merge_clusters (buffer, new_pos, old_pos + 1);
	  hb_glyph_info_t tmp = info[old_pos];
	  memmove (&info[new_pos + 1], &info[new_pos], (old_pos - new_pos) * sizeof (info[0]));
	  info[new_pos] = tmp;
	  if (new_pos <= base && base < old_pos) base++;
	}
	break;
      }
  if (INDIC_POS (&info[start]) == POS_PRE_M)
  {
    if (!start || !(FLAG_UNSAFE (info_gc (&info[start - 1])) & FLAG_RANGE (GC_FORMAT, GC_NON_SPACING_MARK)))
      info[start].mask |= ip->mask_array[INDIC_INIT];
    else
      buf_unsafe_to_break (buffer, start - 1, start + 1);
  }
}

static int final_reordering_indic (const shape_plan_t *plan, hb_font_t *font, hb_buffer_t *buffer)
{
  if (!buffer->len) return 0;
  foreach_syllable (buffer, start, end) final_reordering_syllable_indic (plan, buffer, start, end);
  return 0;
}

static int decompose_indic (const shape_plan_t *plan, hb_codepoint_t ab, hb_codepoint_t *a, hb_codepoint_t *b)
{
  switch (ab)
  {
  case 0x0931u: case 0x09DCu: case 0x09DDu: case 0x0B94u: return 0;
  }
  return uni_decompose (ab, a, b);
}

static int compose_indic (const shape_plan_t *plan, hb_codepoint_t a, hb_codepoint_t b, hb_codepoint_t *ab)
{
  if (GC_IS_MARK (uni_gc (a))) return 0;
  if (a == 0x09AFu && b == 0x09BCu)
  {
    *ab = 0x09DFu;
    return 1;
  }
  return uni_compose (a, b, ab);
}

static const shaper_t shaper_indic = {
  collect_features_indic, override_features_indic, data_create_indic, NULL, NULL, decompose_indic, compose_indic,
  setup_masks_indic, NULL, 0, NORM_COMPOSED_DIACRITICS_NO_SHORT_CIRCUIT, ZERO_WIDTH_MARKS_NONE, 0
};

static void face_free_accels (hb_face_t *face)
{
  glyf_scratch_free (face->glyf_scratch);
  cff_accel_free (face);
  free (face->colr_accel);
  face_free_plans (face);
  layout_free (face);
}
