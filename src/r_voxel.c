/* r_voxel.c -- KVX voxel model parser.
 *
 * See r_voxel.h for format documentation and the credit/derivation
 * notes.  This is Turn 1+2 of voxel-renderer integration: the parser
 * plus a sprite-cache prerasterizer.  A self-test main() exercising
 * only the parser can be compiled with -DKVX_TEST_MAIN.
 */

#ifdef KVX_TEST_MAIN
#define KVX_PARSER_ONLY  /* test main has no engine dependency */
#endif

#include "r_voxel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Little-endian readers.  KVX is always little-endian regardless of
 * host -- the format originated on x86 DOS. */

static uint32_t kvx_read_u32(const uint8_t *p)
{
   return ((uint32_t)p[0])
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static uint16_t kvx_read_u16(const uint8_t *p)
{
   return ((uint16_t)p[0])
        | ((uint16_t)p[1] << 8);
}

/* The minimum byte count for a sane KVX file:
 *   - 4 bytes numbytes header
 *   - 24 bytes (xsize..zpivot, 6 * 4)
 *   - (xsize+1) * 4 bytes xoffset[]
 *   - xsize * (ysize+1) * 2 bytes xyoffset[][]
 *   - some slab data (>= 0)
 *   - 768 bytes palette at end of file
 *
 * Smallest meaningful nonempty model: xsize=ysize=zsize=1.  That gives
 * 4 + 24 + 8 + 4 + 0 + 768 = 808 bytes.  We sanity-check against this
 * before reading anything. */
#define KVX_MIN_BYTES (4 + 24 + 8 + 4 + 768)

kvx_model_t *R_KVX_Load(const void *data_v, size_t len)
{
   const uint8_t *data = (const uint8_t *)data_v;
   const uint8_t *p;
   const uint8_t *slab_base;     /* base of the rawslabdata region */
   const uint8_t *slab_limit;    /* one byte past end of the raw slab area */
   uint32_t numbytes;
   uint32_t xsize, ysize, zsize;
   int32_t  xpivot, ypivot, zpivot;
   int      x;
   int      num_offsets;
   int      min_offset;
   int      max_offset;
   int      data_size;
   uint32_t xoffsets[KVX_MAX_SIZE + 1];
   kvx_model_t *m = NULL;

   if (data == NULL || len < KVX_MIN_BYTES)
      return NULL;

   p = data;

   numbytes = kvx_read_u32(p); p += 4;
   xsize    = kvx_read_u32(p); p += 4;
   ysize    = kvx_read_u32(p); p += 4;
   zsize    = kvx_read_u32(p); p += 4;

   /* Sanity: numbytes is the size of this mip level minus its own
    * 4-byte header.  Anything that claims to be larger than the
    * file (minus palette) is malformed. */
   if (numbytes + 4 > len - 768)
      return NULL;

   if (xsize == 0 || ysize == 0 || zsize == 0)
      return NULL;
   if (xsize > KVX_MAX_SIZE || ysize > KVX_MAX_SIZE || zsize > KVX_MAX_SIZE)
      return NULL;

   /* Pivots are stored as 24.8 fixed-point in voxel units.  Doom's
    * fixed_t is 16.16, so shift left by 8 to convert.  Note: the
    * top byte of a 24.8 KVX pivot is rarely nonzero (would mean a
    * voxel pivot beyond 65536 units, far past any real model), but
    * we read all 4 bytes for correctness rather than truncating. */
   xpivot = (int32_t)kvx_read_u32(p); p += 4;
   ypivot = (int32_t)kvx_read_u32(p); p += 4;
   zpivot = (int32_t)kvx_read_u32(p); p += 4;

   /* Read xoffset[xsize+1].  Each is a u32 byte offset into the slab
    * region (relative to start-of-slabs).  Per the KVX spec,
    * xoffset[0] always equals (xsize+1)*4 + xsize*(ysize+1)*2 -- the
    * size of the offset tables themselves -- but we don't validate
    * this; we'll catch any inconsistency via the bounds check on the
    * resolved per-column offsets below. */
   for (x = 0; x <= (int)xsize; x++)
   {
      xoffsets[x] = kvx_read_u32(p);
      p += 4;
   }

   /* Now p points at xyoffset[][].  After we read xyoffset, p will
    * point at the start of rawslabdata.
    *
    * Bounds check: the offset tables have known sizes.  We want to
    * make sure there's enough data left after them for the palette
    * and at least zero bytes of slab data.  numbytes - 24
    * - (xsize+1)*4 - xsize*(ysize+1)*2 = slab_data_bytes.  We
    * require this to be >= 0. */
   {
      uint64_t header_bytes = 24
                            + (uint64_t)(xsize + 1) * 4
                            + (uint64_t)xsize * (ysize + 1) * 2;
      if (header_bytes > numbytes)
         return NULL;
   }

   num_offsets = (int)xsize * ((int)ysize + 1);

   m = (kvx_model_t *)calloc(1, sizeof(*m));
   if (!m)
      return NULL;

   m->x_size = (int)xsize;
   m->y_size = (int)ysize;
   m->z_size = (int)zsize;

   /* Convert 24.8 -> 16.16 fixed-point (Doom's fixed_t).
    * Use signed shift: pivots can be negative for voxels whose
    * centroid lies outside the grid (rare but legal). */
   m->x_pivot_fx = xpivot << 8;
   m->y_pivot_fx = ypivot << 8;
   m->z_pivot_fx = zpivot << 8;

   m->offsets = (int *)malloc(sizeof(int) * num_offsets);
   if (!m->offsets)
   {
      free(m);
      return NULL;
   }

   /* Read xyoffset[x][y] (each 16-bit) and resolve to absolute
    * offsets within the slab data area.  We reorder the indexing
    * here from KVX's column-major to our row-major-ish layout so
    * `offsets[y * x_size + x]` gives the column start. */
   min_offset =  0x7fffffff;
   max_offset = -0x7fffffff;

   for (x = 0; x < (int)xsize; x++)
   {
      int yi;
      for (yi = 0; yi <= (int)ysize; yi++)
      {
         int absolute = (int)kvx_read_u16(p) + (int)xoffsets[x];
         p += 2;

         /* Store at the (yi, x) slot.  Note: yi runs 0..ysize
          * inclusive, so the last entry per X column is the
          * sentinel that gives the "end" offset of the (x, ysize-1)
          * column. */
         m->offsets[yi * (int)xsize + x] = absolute;

         if (absolute < min_offset) min_offset = absolute;
         if (absolute > max_offset) max_offset = absolute;
      }
   }

   /* p now points at rawslabdata in the file.  The absolute offsets
    * we just stored in m->offsets[] are relative to the start of
    * the offset tables (i.e. `data + 4 + 24`), per the KVX spec
    * convention where xoffset[0] equals the size of the offset
    * tables themselves -- see slab6.txt:
    *
    *   "NOTE: xoffset[0] = (xsize+1)*4 + xsize*(ysize+1)*2 (ALWAYS)"
    *
    * That means the smallest legal offset is `table_size` (just past
    * the end of the offset tables), and the largest legal offset is
    * `numbytes - 24` (the byte just past the end of the mip's slab
    * region).  We bounds-check on this range BEFORE doing arithmetic
    * with the offsets to avoid out-of-buffer reads later. */
   {
      uint64_t table_size = (uint64_t)(xsize + 1) * 4
                          + (uint64_t)xsize * (ysize + 1) * 2;
      int      min_legal  = (int)table_size;
      int      max_legal  = (int)(numbytes - 24);
      int      i;

      for (i = 0; i < num_offsets; i++)
      {
         if (m->offsets[i] < min_legal || m->offsets[i] > max_legal)
         {
            free(m->offsets);
            free(m);
            return NULL;
         }
      }
   }

   /* slab_base points at the first byte of rawslabdata in the input.
    * The offset region starts at `data + 4 + 24` and the slab data
    * starts table_size bytes later, which is exactly where p has
    * advanced to after reading the offset tables.  We use slab_base
    * minus min_offset as the source for memcpy, so an absolute
    * offset of e.g. table_size resolves to slab_base + 0 -- the
    * first byte of slab data. */
   slab_base  = p;
   slab_limit = data + 4 + numbytes;

   if (slab_limit > data + len - 768)
   {
      /* Not enough room for the trailing palette. */
      free(m->offsets);
      free(m);
      return NULL;
   }

   data_size = max_offset - min_offset;
   if (data_size < 0)
   {
      free(m->offsets);
      free(m);
      return NULL;
   }

   /* Subtract min_offset from every offset so they index into our
    * (smaller) trimmed slab buffer.  Apted's trick. */
   for (x = 0; x < num_offsets; x++)
      m->offsets[x] -= min_offset;

   /* Allocate and copy the actually-used slab range. */
   m->data_size = (size_t)data_size;
   if (data_size > 0)
   {
      m->data = (uint8_t *)malloc((size_t)data_size);
      if (!m->data)
      {
         free(m->offsets);
         free(m);
         return NULL;
      }
      /* `min_offset` is in offset-tables-relative coordinates, and
       * slab_base sits at byte `table_size` in those same coords.
       * The first slab byte we want to keep is at offset-table-
       * relative position `min_offset`, which is `slab_base +
       * (min_offset - table_size)` in the actual file. */
      {
         uint64_t table_size = (uint64_t)(xsize + 1) * 4
                             + (uint64_t)xsize * (ysize + 1) * 2;
         memcpy(m->data,
                slab_base + (min_offset - (int)table_size),
                (size_t)data_size);
      }
   }
   else
   {
      /* Pathological but legal: a model where every column is empty.
       * Allocate a minimum-sized non-NULL buffer so callers don't
       * have to special-case the empty case. */
      m->data = (uint8_t *)malloc(1);
      if (!m->data)
      {
         free(m->offsets);
         free(m);
         return NULL;
      }
      m->data_size = 0;
   }

   /* Per-column structural validation: walk every column's slab
    * stream and confirm that:
    *   - the [start, end) range is well-ordered
    *   - each slab's (3-byte header + zlen color bytes) fits
    *     entirely within the column's range
    *   - ztop + zlen <= 256 (every voxel Z fits in a byte)
    *
    * This is belt-and-braces: a malformed KVX from an untrusted
    * source could otherwise crash the renderer in Turn 4+ where
    * we'll iterate slabs in the inner loop without per-iteration
    * bounds checks. */
   for (x = 0; x < (int)xsize; x++)
   {
      int yi;
      for (yi = 0; yi < (int)ysize; yi++)
      {
         int A = m->offsets[ yi      * (int)xsize + x];
         int B = m->offsets[(yi + 1) * (int)xsize + x];
         int q;

         if (B < A || B > (int)m->data_size)
            goto bad_slab;

         q = A;
         while (q < B)
         {
            int ztop, zlen;

            if (q + 3 > B)
               goto bad_slab; /* not enough bytes for slab header */

            ztop = m->data[q + 0];
            zlen = m->data[q + 1];
            /* face byte at m->data[q+2]: any 6-bit value is legal */

            if (ztop + zlen > 256)
               goto bad_slab; /* slab extends past max grid Z */
            if (q + 3 + zlen > B)
               goto bad_slab; /* color array overruns column */

            q += 3 + zlen;
         }
      }
   }

   /* Palette: last 768 bytes of the input.  Verified above that
    * slab_limit + 768 <= data + len.  Note we copy from data + len
    * - 768, NOT from slab_limit; this works correctly whether or
    * not extra mip levels are present between slab_limit and the
    * palette. */
   memcpy(m->palette, data + len - 768, 768);

   return m;

bad_slab:
   free(m->offsets);
   free(m->data);
   free(m);
   return NULL;
}

void R_KVX_Free(kvx_model_t *m)
{
   if (!m)
      return;
   free(m->offsets);
   free(m->data);
   free(m);
}

/* ------------------------------------------------------------------
 * Sprite-cache prerasterizer.
 *
 * Excluded from the standalone KVX_TEST_MAIN build (which doesn't
 * link against the rest of the engine) -- the test main only
 * exercises the parser. */

#ifndef KVX_TEST_MAIN
#ifndef KVX_PARSER_ONLY

#include "r_patch.h"

/* Look up whether voxel (x, y, z) is solid in model m.  Returns the
 * KVX-palette color index of the voxel, or 0xff if (x, y, z) is
 * outside the grid or inside an empty space.
 *
 * This walks the model's slab list for column (x, y) -- O(slabs in
 * column) per call, which is fine for the prerasterizer (one-shot
 * per model) but would be too slow for a per-frame inner loop.
 * Turn 4 will likely build a flat occupancy grid for fast access. */
static uint8_t kvx_voxel_at(const kvx_model_t *m, int x, int y, int z)
{
   int A, B, q;

   if (x < 0 || y < 0 || z < 0)
      return 0xff;
   if (x >= m->x_size || y >= m->y_size || z >= m->z_size)
      return 0xff;

   A = m->offsets[ y      * m->x_size + x];
   B = m->offsets[(y + 1) * m->x_size + x];
   q = A;
   while (q < B)
   {
      int ztop = m->data[q + 0];
      int zlen = m->data[q + 1];
      /* face byte at q+2: ignored here; Turn 4 will use it */
      if (z >= ztop && z < ztop + zlen)
         return m->data[q + 3 + (z - ztop)];
      q += 3 + zlen;
   }
   return 0xff;
}

/* Sentinel for transparent pixels in the rpatch_t pixel buffer.
 * Doom's patch format reserves 0xff as the "no voxel" indicator. */
#define KVX_TRANSPARENT 0xff

/* Build an rpatch_t from a column-major pixels buffer.  This is the
 * back half of any rasterizer: counts posts, allocates the rpatch
 * along with its data block, fills column and post tables.  Takes
 * ownership of the pixels buffer (frees it before returning).  On
 * alloc failure, frees pixels and returns NULL. */
static rpatch_t *kvx_pixels_to_rpatch(uint8_t *pixels,
                                      int width, int height)
{
   int       x, z;
   int       num_posts_total;
   size_t    pixels_size;
   size_t    columns_size;
   size_t    posts_size;
   size_t    data_size;
   int       post_index;
   rpatch_t *patch;

   /* Count posts (vertical runs of opaque pixels) per column. */
   num_posts_total = 0;
   for (x = 0; x < width; x++)
   {
      int in_run = 0;
      for (z = 0; z < height; z++)
      {
         int opaque = (pixels[x * height + z] != KVX_TRANSPARENT);
         if (opaque && !in_run)
         {
            num_posts_total++;
            in_run = 1;
         }
         else if (!opaque)
         {
            in_run = 0;
         }
      }
   }

   pixels_size  = (size_t)width * (size_t)height;
   columns_size = (size_t)width * sizeof(rcolumn_t);
   posts_size   = (size_t)num_posts_total * sizeof(rpost_t);
   data_size    = pixels_size + columns_size + posts_size;

   patch = (rpatch_t *)malloc(sizeof(*patch));
   if (!patch)
   {
      free(pixels);
      return NULL;
   }

   patch->data = (unsigned char *)malloc(data_size);
   if (!patch->data)
   {
      free(patch);
      free(pixels);
      return NULL;
   }

   patch->width         = width;
   patch->height        = height;
   patch->widthmask     = 0;
   patch->isNotTileable = 1;
   patch->leftoffset    = width / 2;
   patch->topoffset     = height;
   patch->locks         = 0;

   patch->pixels  = patch->data;
   patch->columns = (rcolumn_t *)(patch->data + pixels_size);
   patch->posts   = (rpost_t *)(patch->data + pixels_size + columns_size);

   memcpy(patch->pixels, pixels, pixels_size);
   free(pixels);

   /* Fill column and post tables. */
   post_index = 0;
   for (x = 0; x < width; x++)
   {
      rcolumn_t *col = &patch->columns[x];
      int        post_start = -1;

      col->numPosts = 0;
      col->pixels   = patch->pixels + x * height;
      col->posts    = &patch->posts[post_index];

      for (z = 0; z < height; z++)
      {
         int opaque = (patch->pixels[x * height + z] != KVX_TRANSPARENT);

         if (opaque && post_start < 0)
         {
            post_start = z;
         }
         else if (!opaque && post_start >= 0)
         {
            patch->posts[post_index].topdelta = post_start;
            patch->posts[post_index].length   = z - post_start;
            patch->posts[post_index].slope    = 0;
            post_index++;
            col->numPosts++;
            post_start = -1;
         }
      }

      if (post_start >= 0)
      {
         patch->posts[post_index].topdelta = post_start;
         patch->posts[post_index].length   = height - post_start;
         patch->posts[post_index].slope    = 0;
         post_index++;
         col->numPosts++;
      }
   }

   return patch;
}

/* Rasterize a voxel front-view at exact target dimensions.  Each
 * output pixel samples the model voxel at
 *   model_x = out_x * model_x_size / target_w
 *   model_z = out_z * model_z_size / target_h
 * which handles both upscale (model smaller than target) and
 * downscale (model larger than target) uniformly.  This lets a
 * voxel be rendered to match an existing sprite's size, even when
 * the voxel resolution doesn't match.
 *
 * Returns NULL on alloc failure or invalid dimensions. */
rpatch_t *R_KVX_RasterizeFrontSized(const kvx_model_t *m,
                                    const uint8_t *palette_remap,
                                    int target_w, int target_h)
{
   int      width;
   int      height;
   int      x, z, y;
   uint8_t *pixels;

   if (!m)
      return NULL;

   width  = target_w;
   height = target_h;

   if (width <= 0 || height <= 0)
      return NULL;

   pixels = (uint8_t *)malloc((size_t)width * (size_t)height);
   if (!pixels)
      return NULL;

   for (x = 0; x < width; x++)
   {
      int mx = (x * m->x_size) / width;
      if (mx >= m->x_size) mx = m->x_size - 1;
      for (z = 0; z < height; z++)
      {
         int mz = (z * m->z_size) / height;
         uint8_t color = KVX_TRANSPARENT;

         if (mz >= m->z_size) mz = m->z_size - 1;

         /* Walk Y from front to back, take the first opaque voxel. */
         for (y = 0; y < m->y_size; y++)
         {
            uint8_t c = kvx_voxel_at(m, mx, y, mz);
            if (c != 0xff)
            {
               color = c;
               break;
            }
         }

         /* Apply palette remap if provided.  Note: 0xff (transparent)
          * is preserved -- a remap table that mapped 0xff to some
          * other index would corrupt the transparent sentinel.  The
          * caller's remap table only has to be valid for indices
          * 0..0xfe; we don't read entry 0xff. */
         if (color != KVX_TRANSPARENT && palette_remap)
            color = palette_remap[color];

         pixels[x * height + z] = color;
      }
   }

   return kvx_pixels_to_rpatch(pixels, width, height);
}

/* Rasterize a voxel from one of 8 view rotations (0=front, 2=right
 * side, 4=back, 6=left, with 1/3/5/7 the 45-degree diagonals).
 * Output target dimensions are independent of rotation -- all 8
 * views are produced at the same target_w x target_h, so the
 * rendering pipeline can swap views by index without resizing.
 *
 * Sampling: for each output column out_x, walk a depth ray through
 * the voxel along the view direction.  The ray's origin in the
 * voxel's local XY plane is determined by out_x's offset from the
 * sprite's centre line, rotated by view angle theta = rotation *
 * 45 degrees.  The first opaque voxel along the ray determines the
 * output pixel's colour.  Output rows map proportionally to the
 * voxel's Z axis, the same as the front-view rasterizer.
 *
 * Sampling resolution: when the voxel is larger than the target
 * sprite (typical for high-res KVX vs Doom sprite sizes), sampling
 * undersamples and we get a "first hit" bias rather than averaging.
 * That's the same bias as the front-view rasterizer; it's not
 * better, but it's also not worse, and it preserves the chunky
 * voxel aesthetic. */
rpatch_t *R_KVX_RasterizeRotated(const kvx_model_t *m,
                                 const uint8_t *palette_remap,
                                 int target_w, int target_h,
                                 int rotation)
{
   /* Trig table for the 8 cardinal+diagonal view angles, scaled by
    * 256 to keep the sampler in fixed-point integer math.
    * theta = rotation * 45deg; entries are (cos(theta)*256,
    * sin(theta)*256).  cos(45deg) = sin(45deg) ~= 181/256. */
   static const int cs8[8][2] =
   {
      { 256,    0 },   /* rot 0:   0 deg */
      { 181,  181 },   /* rot 1:  45 deg */
      {   0,  256 },   /* rot 2:  90 deg */
      {-181,  181 },   /* rot 3: 135 deg */
      {-256,    0 },   /* rot 4: 180 deg */
      {-181, -181 },   /* rot 5: 225 deg */
      {   0, -256 },   /* rot 6: 270 deg */
      { 181, -181 }    /* rot 7: 315 deg */
   };

   int      width;
   int      height;
   int      out_x, out_z;
   int      cx256, cy256;
   int      cos_t, sin_t;
   int      depth_max;
   uint8_t *pixels;

   if (!m)
      return NULL;
   if (rotation < 0 || rotation >= 8)
      return NULL;

   width  = target_w;
   height = target_h;
   if (width <= 0 || height <= 0)
      return NULL;

   /* Voxel centre in fixed-point (.8) for the rotation math. */
   cx256 = (m->x_size * 256) / 2;
   cy256 = (m->y_size * 256) / 2;
   cos_t = cs8[rotation][0];
   sin_t = cs8[rotation][1];

   /* Worst-case depth: half the voxel's diagonal in the XY plane.
    * Add 1 to avoid edge-case under-sampling on diagonal rotations
    * where the ray enters and exits the voxel at off-axis points. */
   {
      int half_x = m->x_size / 2 + 1;
      int half_y = m->y_size / 2 + 1;
      /* sqrt(half_x^2 + half_y^2), conservatively rounded up.  For
       * cubic voxels this is ~1.42 * half_size; we use a coarse
       * approximation that's never less than the true value: take
       * the larger half-size and add the smaller. */
      depth_max = (half_x > half_y ? half_x : half_y) +
                  (half_x > half_y ? half_y : half_x);
   }

   pixels = (uint8_t *)malloc((size_t)width * (size_t)height);
   if (!pixels)
      return NULL;

   for (out_x = 0; out_x < width; out_x++)
   {
      /* Output column's offset from sprite centre line, scaled to
       * model voxel coords.  For rot 0/4 this maps to model X; for
       * rot 2/6 it maps to model Y; for diagonals it's a mix. */
      int local_off256 = ((out_x - width / 2) * m->x_size * 256) /
                         (width > 0 ? width : 1);

      for (out_z = 0; out_z < height; out_z++)
      {
         /* Output row to model Z, proportional. */
         int     mz       = (out_z * m->z_size) / height;
         int     d;
         uint8_t color    = KVX_TRANSPARENT;

         if (mz >= m->z_size) mz = m->z_size - 1;
         if (mz < 0)          mz = 0;

         /* Walk depth from -depth_max to +depth_max, looking for
          * the first opaque voxel.  Negative d puts the sample
          * "behind" the camera (closer to viewer), positive d
          * "in front" (further into the model).  We want first-
          * hit-from-camera, so we walk from -depth_max upwards. */
         for (d = -depth_max; d <= depth_max; d++)
         {
            /* model_x256 = cx + local_off * cos - d * sin
             * model_y256 = cy + local_off * sin + d * cos
             * (all in .8 fixed-point) */
            int model_x256 = cx256
                           + (local_off256 * cos_t) / 256
                           - (d * 256 * sin_t)      / 256;
            int model_y256 = cy256
                           + (local_off256 * sin_t) / 256
                           + (d * 256 * cos_t)      / 256;
            int mx = model_x256 / 256;
            int my = model_y256 / 256;
            uint8_t c;

            if (mx < 0 || mx >= m->x_size) continue;
            if (my < 0 || my >= m->y_size) continue;

            c = kvx_voxel_at(m, mx, my, mz);
            if (c != 0xff)
            {
               color = c;
               break;
            }
         }

         if (color != KVX_TRANSPARENT && palette_remap)
            color = palette_remap[color];

         pixels[out_x * height + out_z] = color;
      }
   }

   return kvx_pixels_to_rpatch(pixels, width, height);
}

/* Rasterize at a default scale.  Used by the fallback test cube and
 * any caller that doesn't have a specific target sprite size in
 * mind.  The default upscale factor exists because the synthetic
 * test cube is 8x8x8 -- rasterized 1:1 it produces an 8x8 pixel
 * sprite which is barely visible.  Real KVX content sized to match
 * its target sprite should call R_KVX_RasterizeFrontSized directly
 * with the sprite's dimensions. */
#define KVX_DEFAULT_UPSCALE 4
rpatch_t *R_KVX_RasterizeFront(const kvx_model_t *m,
                               const uint8_t *palette_remap)
{
   if (!m)
      return NULL;
   return R_KVX_RasterizeFrontSized(m, palette_remap,
                                    m->x_size * KVX_DEFAULT_UPSCALE,
                                    m->z_size * KVX_DEFAULT_UPSCALE);
}

void R_KVX_FreeSprite(rpatch_t *p)
{
   if (!p)
      return;
   free(p->data);
   free(p);
}

/* Build a small in-memory test voxel for end-to-end integration
 * verification without needing a real .kvx file.  Produces an
 * 8x8x8 cube with the corners cut off (so the result is visibly
 * "voxel" rather than a flat sprite-sized square).  Each face is
 * a different color for visual orientation.
 *
 * Color scheme (PLAYPAL indices for Doom):
 *   top    = 176 (bright red, palette index for HUD red)
 *   bottom = 112 (green)
 *   left   =  96 (yellow)
 *   right  = 192 (orange)
 *   front  = 207 (blue)
 *   back   = 251 (white)
 *   interior = 0 (will be hidden anyway)
 *
 * Note these are PLAYPAL indices, not KVX-palette.  Caller passes
 * NULL for palette_remap so they pass through unchanged.
 *
 * The returned model owns its allocations -- free with R_KVX_Free. */
kvx_model_t *R_KVX_BuiltinTestVoxel(void)
{
   const int   N = 8;
   int         x, y, z;
   kvx_model_t *m;
   uint8_t     occupancy[8][8][8];
   uint8_t     color[8][8][8];

   /* Build a flat occupancy first. */
   memset(occupancy, 0, sizeof(occupancy));
   memset(color,     0, sizeof(color));
   for (x = 0; x < N; x++)
      for (y = 0; y < N; y++)
         for (z = 0; z < N; z++)
         {
            /* Cut off corner cubes (1x1x1 at each of 8 corners) for
             * a visibly non-cubical silhouette. */
            int corner_x = (x == 0 || x == N - 1);
            int corner_y = (y == 0 || y == N - 1);
            int corner_z = (z == 0 || z == N - 1);
            if (corner_x && corner_y && corner_z)
               continue;

            occupancy[x][y][z] = 1;

            /* Color: face-dependent.  Pick the dominant axis based
             * on which boundary the voxel is on.  Interior voxels
             * get an arbitrary color; they'll be culled at render
             * time anyway. */
            if      (z == 0)        color[x][y][z] = 176; /* top */
            else if (z == N - 1)    color[x][y][z] = 112; /* bottom */
            else if (x == 0)        color[x][y][z] =  96; /* left */
            else if (x == N - 1)    color[x][y][z] = 192; /* right */
            else if (y == 0)        color[x][y][z] = 207; /* front */
            else if (y == N - 1)    color[x][y][z] = 251; /* back */
            else                    color[x][y][z] =   4; /* interior (gray) */
         }

   /* Synthesize a kvx_model_t from the occupancy grid.  For each
    * column (x,y), produce one slab per contiguous Z-run of opaque
    * voxels.  Compact format. */
   m = (kvx_model_t *)calloc(1, sizeof(*m));
   if (!m)
      return NULL;

   m->x_size = N;
   m->y_size = N;
   m->z_size = N;
   /* Pivot at center bottom (typical for upright actor sprites). */
   m->x_pivot_fx = (N / 2) << 16;
   m->y_pivot_fx = (N / 2) << 16;
   m->z_pivot_fx = (N - 1) << 16;

   /* First pass: compute slab data size. */
   {
      size_t total_bytes = 0;
      for (x = 0; x < N; x++)
         for (y = 0; y < N; y++)
         {
            int run_start = -1;
            for (z = 0; z < N; z++)
            {
               int opaque = occupancy[x][y][z];
               if (opaque && run_start < 0)
                  run_start = z;
               else if (!opaque && run_start >= 0)
               {
                  total_bytes += 3 + (z - run_start);
                  run_start = -1;
               }
            }
            if (run_start >= 0)
               total_bytes += 3 + (N - run_start);
         }

      m->data_size = total_bytes;
      m->data      = (uint8_t *)malloc(total_bytes ? total_bytes : 1);
      if (!m->data)
      {
         free(m);
         return NULL;
      }
   }

   m->offsets = (int *)malloc(sizeof(int) * N * (N + 1));
   if (!m->offsets)
   {
      free(m->data);
      free(m);
      return NULL;
   }

   /* Second pass: fill slab data and offsets.  We layout columns in
    * order (x=0,y=0), (x=0,y=1), ..., which is row-major in (y,x).
    * For each column, write its slabs and record the start byte.
    *
    * The offsets array is sized x_size * (y_size + 1) and indexed as
    * offsets[y * x_size + x], so the sentinel "end of column (x,
    * y_size-1)" lives at offsets[y_size * x_size + x] -- one
    * sentinel per X column. */
   {
      int    cursor = 0;
      for (x = 0; x < N; x++)
      {
         for (y = 0; y < N; y++)
         {
            int run_start = -1;
            m->offsets[y * N + x] = cursor;
            for (z = 0; z < N; z++)
            {
               int opaque = occupancy[x][y][z];
               if (opaque && run_start < 0)
                  run_start = z;
               else if (!opaque && run_start >= 0)
               {
                  int len = z - run_start;
                  int k;
                  m->data[cursor + 0] = (uint8_t)run_start;
                  m->data[cursor + 1] = (uint8_t)len;
                  m->data[cursor + 2] = 0x3f;
                  for (k = 0; k < len; k++)
                     m->data[cursor + 3 + k] = color[x][y][run_start + k];
                  cursor += 3 + len;
                  run_start = -1;
               }
            }
            if (run_start >= 0)
            {
               int len = N - run_start;
               int k;
               m->data[cursor + 0] = (uint8_t)run_start;
               m->data[cursor + 1] = (uint8_t)len;
               m->data[cursor + 2] = 0x3f;
               for (k = 0; k < len; k++)
                  m->data[cursor + 3 + k] = color[x][y][run_start + k];
               cursor += 3 + len;
            }
         }
         /* Write the y=y_size sentinel for this X column.  This is
          * the byte just past the end of (x, y_size-1)'s slabs --
          * equivalently, the cursor at this point. */
         m->offsets[N * N + x] = cursor;
      }
   }

   /* Identity palette (KVX 0-63 ramp).  Caller is responsible for
    * passing palette_remap = NULL since we used PLAYPAL indices
    * directly above. */
   {
      int i;
      for (i = 0; i < 256; i++)
      {
         m->palette[i * 3 + 0] = (uint8_t)((i >> 2) & 0x3f);
         m->palette[i * 3 + 1] = (uint8_t)((i >> 2) & 0x3f);
         m->palette[i * 3 + 2] = (uint8_t)((i >> 2) & 0x3f);
      }
   }

   return m;
}

/* ------------------------------------------------------------------
 * Engine integration (Turn 2 -- single-sprite-replacement stub)
 *
 * Stores one prerasterized voxel rpatch_t and the lump number it
 * replaces.  R_DrawVisSprite calls R_KVX_LookupSprite() before its
 * R_CachePatchNum, and uses the voxel patch when one matches.
 *
 * Limitations of this stub:
 *   - Exactly one replacement at a time (the test voxel for the
 *     medikit's frame A).
 *   - No rotations -- a single front view used for all angles.
 *   - No way for the user to disable it without recompiling
 *     (Turn 3 wires this to a menu option / cvar).
 *
 * The replacement slot lives across libretro session reloads.
 * R_KVX_Init() is idempotent: it tears down any previous slot
 * before populating a new one.  R_KVX_Shutdown() releases it. */

#ifndef KVX_NO_ENGINE_GLUE

#include "info.h"           /* sprnames[], NUMSPRITES */
#include "w_wad.h"          /* W_CheckNumForName, W_CacheLumpNum, ... */
#include "r_state.h"        /* firstspritelump, numsprites */
#include "tables.h"         /* ANG45 (used in spin rate conversion) */
#include "z_zone.h"         /* PU_STATIC, Z_Free */
#include "lprintf.h"

/* User-controlled toggle.  Bound to the "voxel_sprites" cvar (see
 * m_misc.c) and the General > Display Options > "Voxel sprites" menu
 * entry (see m_menu.c).  Default is 0 (off) -- voxel rendering is an
 * opt-in feature.
 *
 * When this is 0, R_KVX_Init() leaves the mapping table empty and
 * R_KVX_LookupSprite() returns NULL for every (sprite, frame) so
 * the engine renders normal sprites.  R_KVX_LookupSprite() also
 * checks this flag at lookup time as defense in depth. */
int voxel_sprites = 0;

/* Per-sprite, per-frame mapping table.  Indexed as
 * kvx_table[sprite_index * KVX_MAX_FRAMES + frame].
 *
 * KVX_MAX_FRAMES covers letters 'A' through one past the practical
 * limit; vanilla Doom uses 'A'..'Y' or so for any single sprite, but
 * DEHACKED-extended frames can go further.  32 is comfortable. */
#define KVX_MAX_FRAMES  32

/* Number of cached view rotations per (sprite, frame).  Matches
 * Doom's 8 sprite rotations (0=front, 1=front-right at 45deg, ...,
 * 7=front-left at 315deg).  Each rotation is an independently
 * rasterized rpatch_t; lookup picks the right one based on the
 * angle from viewer to thing. */
#define KVX_NUM_ROTATIONS 8

typedef struct kvx_voxel_entry_s
{
   kvx_model_t *model;
   /* One rasterized rpatch_t per view rotation.  All 8 share the
    * same model (which is the raw KVX data) and the same target
    * dimensions (matched to the original sprite); they differ only
    * in the angle from which the voxel is sampled. */
   rpatch_t    *patches[KVX_NUM_ROTATIONS];

   /* Pre-rotation in eighth-turn buckets (0..7), applied at lookup
    * time.  Set from the VOXELDEF AngleOffset property: an offset of
    * 90deg (= 2 buckets) means the voxel was authored with its
    * "natural front" 90deg rotated from where Doom expects, so we
    * shift the requested rotation by 2 to compensate.  Most content
    * leaves this at 0. */
   int          angle_offset_bucket;

   /* Spin rates in angle_t units per gametic.  Engine code multiplies
    * by leveltime to get a cumulative rotation that's added to thing
    * ->angle before computing view rotation, producing the spinning-
    * pickup effect Doom voxel mods rely on (Quake-like rotating
    * weapons/health/armor on the floor).  Two slots so a thing can
    * spin differently when initially-placed by the mapper vs dropped
    * by an enemy at runtime; engine selects via MF_DROPPED.  Both
    * default to 0 ("don't spin"). */
   unsigned int placed_spin_per_tic;
   unsigned int dropped_spin_per_tic;
} kvx_voxel_entry_t;

static kvx_voxel_entry_t *kvx_table          = NULL;
static int                kvx_table_sprites  = 0;
static int                kvx_table_count    = 0;  /* populated entries */

/* Parse statistics, accumulated across all voxeldef blocks.
 * Reported in a final summary line so users can see how many
 * mappings landed and why the rest didn't.  Useful when a voxel
 * mod targets sprite names from a different IWAD (e.g. Chex Quest
 * voxels loaded against doom2.wad) and most entries are silently
 * rejected.  Defined here (before the forward decls) so other
 * functions can reference it. */
typedef struct kvx_parse_stats_s
{
   int registered;       /* mappings that landed successfully */
   int unknown_sprite;   /* SPRITE name not in sprnames[] */
   int bad_lump;         /* lump missing, unparseable, or rasterize failed */
   int syntax_error;     /* malformed VOXELDEF block */
} kvx_parse_stats_t;

/* Forward decls for helpers defined further down. */
static void kvx_register_test_voxel(void);
static int  kvx_load_kvx_lump(const char *lumpname,
                              int sprite_idx, int frame,
                              double scale_factor,
                              kvx_model_t **out_model,
                              rpatch_t    *out_patches[KVX_NUM_ROTATIONS]);
static void kvx_register_mapping(int sprite_idx, int frame,
                                 kvx_model_t *model,
                                 rpatch_t    *patches[KVX_NUM_ROTATIONS],
                                 int angle_offset_bucket,
                                 unsigned int placed_spin_per_tic,
                                 unsigned int dropped_spin_per_tic);
static int  kvx_rasterize_all_rotations(const kvx_model_t *m,
                                        int target_w, int target_h,
                                        int target_leftoffset,
                                        int target_topoffset,
                                        int have_offsets,
                                        rpatch_t *out_patches[KVX_NUM_ROTATIONS]);
static int  kvx_parse_voxeldef(const char *src, int len,
                               kvx_parse_stats_t *stats);

void R_KVX_Init(void)
{
   int                voxeldef_lump;
   kvx_parse_stats_t  stats;

   /* Tear down any previous mapping (libretro reload safety,
    * OFF->ON->OFF->ON menu toggle path). */
   R_KVX_Shutdown();

   if (!voxel_sprites)
   {
      lprintf(LO_INFO, "R_KVX_Init: voxel sprites disabled "
                       "(toggle via Options > General > Display)\n");
      return;
   }

   /* Allocate the mapping table.  numsprites is set during
    * R_InitSpriteDefs which has already run by the time R_Init
    * gets to us. */
   kvx_table_sprites = numsprites;
   if (kvx_table_sprites <= 0)
   {
      lprintf(LO_INFO, "R_KVX_Init: numsprites = %d, skipping\n",
              kvx_table_sprites);
      return;
   }

   kvx_table = (kvx_voxel_entry_t *)calloc(
                  (size_t)kvx_table_sprites * KVX_MAX_FRAMES,
                  sizeof(kvx_voxel_entry_t));
   if (!kvx_table)
   {
      lprintf(LO_WARN, "R_KVX_Init: out of memory allocating mapping table\n");
      return;
   }
   kvx_table_count = 0;

   /* Look for a VOXELDEF lump.  If one or more is loaded, parse
    * each in WAD load order so later lumps can override earlier
    * mappings. */
   memset(&stats, 0, sizeof(stats));
   voxeldef_lump = (W_CheckNumForName)("VOXELDEF", ns_global);
   if (voxeldef_lump >= 0)
   {
      const char *src;
      int          len;

      len = W_LumpLength(voxeldef_lump);
      src = (const char *)W_CacheLumpNum(voxeldef_lump);
      if (src && len > 0)
         kvx_parse_voxeldef(src, len, &stats);
      W_UnlockLumpNum(voxeldef_lump);
   }

   if (stats.registered > 0)
   {
      /* Report the breakdown so a user with a partial-success
       * voxel mod (e.g. Chex Quest voxels on doom2.wad, where many
       * Chex sprite names don't exist) can see what was skipped
       * and why. */
      int rejected = stats.unknown_sprite + stats.bad_lump +
                     stats.syntax_error;
      if (rejected > 0)
         lprintf(LO_INFO, "R_KVX_Init: VOXELDEF parsed, %d registered, "
                          "%d skipped (%d unknown sprite, %d bad lump, "
                          "%d syntax error)\n",
                          stats.registered, rejected,
                          stats.unknown_sprite, stats.bad_lump,
                          stats.syntax_error);
      else
         lprintf(LO_INFO, "R_KVX_Init: VOXELDEF parsed, %d voxel "
                          "mappings registered\n", stats.registered);
   }
   else if (voxeldef_lump >= 0)
   {
      /* VOXELDEF was loaded but produced zero mappings.  Likely a
       * complete sprite-name mismatch (wrong IWAD).  Skip the test
       * cube fallback -- the user explicitly tried to load a voxel
       * mod, replacing it with a placeholder cube would be more
       * confusing than helpful. */
      lprintf(LO_WARN, "R_KVX_Init: VOXELDEF parsed, 0 mappings landed "
                       "(%d skipped: %d unknown sprite, %d bad lump, "
                       "%d syntax error).  Wrong IWAD?\n",
                       stats.unknown_sprite + stats.bad_lump +
                          stats.syntax_error,
                       stats.unknown_sprite, stats.bad_lump,
                       stats.syntax_error);
   }
   else
   {
      /* No VOXELDEF in any loaded WAD.  Fall back to the synthetic
       * test cube bound to MEDIA0, so the menu toggle still has a
       * visible effect when no voxel mod is loaded.  This keeps
       * the integration testable on stock IWADs while real voxel
       * mods are being authored. */
      lprintf(LO_INFO, "R_KVX_Init: no VOXELDEF lump, registering "
                       "test cube on MEDI/A as fallback\n");
      kvx_register_test_voxel();
   }
}

void R_KVX_Shutdown(void)
{
   int i, n;

   if (!kvx_table)
   {
      kvx_table_sprites = 0;
      kvx_table_count   = 0;
      return;
   }

   n = kvx_table_sprites * KVX_MAX_FRAMES;
   for (i = 0; i < n; i++)
   {
      int r;
      for (r = 0; r < KVX_NUM_ROTATIONS; r++)
      {
         if (kvx_table[i].patches[r])
         {
            R_KVX_FreeSprite(kvx_table[i].patches[r]);
            kvx_table[i].patches[r] = NULL;
         }
      }
      if (kvx_table[i].model)
      {
         R_KVX_Free(kvx_table[i].model);
         kvx_table[i].model = NULL;
      }
   }
   free(kvx_table);
   kvx_table         = NULL;
   kvx_table_sprites = 0;
   kvx_table_count   = 0;
}

const rpatch_t *R_KVX_LookupSpriteRotated(int sprite, int frame,
                                          int rotation)
{
   const kvx_voxel_entry_t *e;
   int                      shifted;

   /* Defense in depth: if the user disabled voxel sprites without
    * triggering a re-init (shouldn't happen via the menu callback,
    * but cheap to guard). */
   if (!voxel_sprites || !kvx_table)
      return NULL;
   if (sprite < 0 || sprite >= kvx_table_sprites)
      return NULL;
   if (frame < 0 || frame >= KVX_MAX_FRAMES)
      return NULL;
   if (rotation < 0 || rotation >= KVX_NUM_ROTATIONS)
      return NULL;

   /* Apply the per-voxel angle offset.  The engine asks for the view
    * rotation it would normally use for a Doom sprite (rot 0 = front
    * facing camera); if the voxel was authored with a different
    * "natural front", angle_offset_bucket compensates. */
   e = &kvx_table[sprite * KVX_MAX_FRAMES + frame];
   shifted = (rotation + e->angle_offset_bucket) & (KVX_NUM_ROTATIONS - 1);
   return e->patches[shifted];
}

const rpatch_t *R_KVX_LookupSprite(int sprite, int frame)
{
   /* Backwards-compat wrapper: returns the front view (rotation 0).
    * Callers that want view-dependent rotation should use
    * R_KVX_LookupSpriteRotated. */
   return R_KVX_LookupSpriteRotated(sprite, frame, 0);
}

unsigned int R_KVX_GetSpinPerTic(int sprite, int frame, int dropped)
{
   const kvx_voxel_entry_t *e;

   if (!voxel_sprites || !kvx_table)
      return 0;
   if (sprite < 0 || sprite >= kvx_table_sprites)
      return 0;
   if (frame < 0 || frame >= KVX_MAX_FRAMES)
      return 0;
   e = &kvx_table[sprite * KVX_MAX_FRAMES + frame];
   /* If no voxel is registered, both rates will be 0 (calloc'd
    * initial state preserved); returning 0 is the right "no spin"
    * answer regardless. */
   return dropped ? e->dropped_spin_per_tic : e->placed_spin_per_tic;
}

/* Look up a sprite name (4 chars, e.g. "MEDI") in the sprnames[]
 * array and return its index, or -1 if not found.  Sprite names
 * in DOOM are 4 chars; sprnames[i] is a NUL-terminated 4-char string. */
static int kvx_lookup_sprite_name(const char *name)
{
   int i;
   for (i = 0; i < numsprites; i++)
   {
      if (sprnames[i] &&
          name[0] == sprnames[i][0] &&
          name[1] == sprnames[i][1] &&
          name[2] == sprnames[i][2] &&
          name[3] == sprnames[i][3])
         return i;
   }
   return -1;
}

/* Load a KVX from a WAD lump and prerasterize at the target
 * sprite's dimensions.  On success, returns 1 and writes the
 * kvx_model_t and rpatch_t pointers to *out_model and *out_patch.
 * Caller takes ownership.  On failure returns 0 and leaves outputs
 * unchanged.
 *
 * The (sprite_idx, frame) tuple identifies the sprite the voxel
 * will replace; we look up that sprite's first-rotation lump and
 * use its dimensions as the rasterization target.  This makes the
 * voxel a drop-in size match for the original sprite -- a tiny
 * floor splat stays tiny, a tall pickup stays tall -- regardless
 * of whether the KVX is 8x8x8 or 128x128x128 internally.
 *
 * If sprite_idx is invalid or the sprite has no rotation 0 lump
 * loaded (DEHACKED-extended sprite missing graphics, etc.), we
 * fall back to the default upscale rasterizer so we still produce
 * something. */
static int kvx_load_kvx_lump(const char *lumpname,
                             int sprite_idx, int frame,
                             double scale_factor,
                             kvx_model_t **out_model,
                             rpatch_t    *out_patches[KVX_NUM_ROTATIONS])
{
   int          lumpnum;
   int          len;
   const void  *data;
   kvx_model_t *m;
   int          target_w = 0;
   int          target_h = 0;
   int          target_leftoffset = 0;
   int          target_topoffset = 0;
   int          have_offsets = 0;

   lumpnum = (W_CheckNumForName)(lumpname, ns_global);
   if (lumpnum < 0)
   {
      lprintf(LO_WARN, "R_KVX: VOXELDEF references missing lump '%s'\n",
              lumpname);
      return 0;
   }

   len  = W_LumpLength(lumpnum);
   data = W_CacheLumpNum(lumpnum);
   if (!data || len <= 0)
   {
      lprintf(LO_WARN, "R_KVX: lump '%s' is empty\n", lumpname);
      W_UnlockLumpNum(lumpnum);
      return 0;
   }

   m = R_KVX_Load(data, (size_t)len);
   W_UnlockLumpNum(lumpnum);

   if (!m)
   {
      lprintf(LO_WARN, "R_KVX: lump '%s' failed to parse as KVX\n",
              lumpname);
      return 0;
   }

   /* Look up the original sprite's dimensions and offsets so we
    * can rasterize the voxel at matching size and pivot.  All 8
    * rotations are rendered at the same target dimensions (so the
    * rendering pipeline can swap rotations by index without
    * rescaling) -- only the sampling angle differs.  Bounds-check
    * the sprite/frame indices defensively. */
   if (sprite_idx >= 0 && sprite_idx < numsprites &&
       sprites && sprites[sprite_idx].spriteframes &&
       frame >= 0 && frame < sprites[sprite_idx].numframes)
   {
      const spriteframe_t *sf = &sprites[sprite_idx].spriteframes[frame];
      int sprite_lump = sf->lump[0];
      if (sprite_lump >= 0)
      {
         const rpatch_t *orig = R_CachePatchNum(sprite_lump + firstspritelump);
         if (orig)
         {
            target_w = orig->width;
            target_h = orig->height;
            target_leftoffset = orig->leftoffset;
            target_topoffset  = orig->topoffset;
            have_offsets = 1;
         }
         R_UnlockPatchNum(sprite_lump + firstspritelump);
      }
   }

   /* Apply VOXELDEF Scale to target dimensions and offsets.  All
    * four scale together so the voxel positions correctly relative
    * to its (now scaled) bounds.  scale_factor of 1.0 is a no-op;
    * the parser clamps it to a sensible range so we don't have to
    * worry about pathological values here. */
   if (scale_factor != 1.0 && target_w > 0 && target_h > 0)
   {
      int sw = (int)((double)target_w * scale_factor + 0.5);
      int sh = (int)((double)target_h * scale_factor + 0.5);
      if (sw < 1) sw = 1;
      if (sh < 1) sh = 1;
      target_leftoffset = (int)((double)target_leftoffset * scale_factor + 0.5);
      target_topoffset  = (int)((double)target_topoffset  * scale_factor + 0.5);
      target_w = sw;
      target_h = sh;
   }

   if (!kvx_rasterize_all_rotations(m, target_w, target_h,
                                    target_leftoffset, target_topoffset,
                                    have_offsets, out_patches))
   {
      R_KVX_Free(m);
      lprintf(LO_WARN, "R_KVX: rasterization of '%s' failed\n", lumpname);
      return 0;
   }

   *out_model = m;
   return 1;
}

/* Helper: rasterize all 8 view rotations of a voxel at the given
 * target dimensions and offsets, into the supplied output array.
 * On any failure, frees any patches already produced and zeros the
 * array, returning 0.  Returns 1 on full success.  Caller owns the
 * patches on success; the model is not consumed. */
static int kvx_rasterize_all_rotations(const kvx_model_t *m,
                                       int target_w, int target_h,
                                       int target_leftoffset,
                                       int target_topoffset,
                                       int have_offsets,
                                       rpatch_t *out_patches[KVX_NUM_ROTATIONS])
{
   int r;

   for (r = 0; r < KVX_NUM_ROTATIONS; r++)
      out_patches[r] = NULL;

   for (r = 0; r < KVX_NUM_ROTATIONS; r++)
   {
      rpatch_t *p;

      if (target_w > 0 && target_h > 0)
         p = R_KVX_RasterizeRotated(m, NULL, target_w, target_h, r);
      else
         /* No target sizing info -- fall back to default upscale on
          * the front view only.  The other rotations get a copy of
          * the rotated render at the model's native scale. */
         p = R_KVX_RasterizeRotated(m, NULL,
                                    m->x_size * 4, m->z_size * 4, r);

      if (!p)
      {
         /* Roll back: free any earlier rotations and bail. */
         int j;
         for (j = 0; j < r; j++)
         {
            if (out_patches[j])
            {
               R_KVX_FreeSprite(out_patches[j]);
               out_patches[j] = NULL;
            }
         }
         return 0;
      }

      if (have_offsets)
      {
         p->leftoffset = target_leftoffset;
         p->topoffset  = target_topoffset;
      }

      out_patches[r] = p;
   }

   return 1;
}

/* Bind a (sprite, frame) cell to a model and its 8 prerasterized
 * rotation views.  Frees any previous occupant of the cell,
 * transferring ownership of the new model and patches into the
 * table.  Out-of-range arguments cause the new resources to be
 * freed and discarded (defensive; callers should validate first). */
static void kvx_register_mapping(int sprite_idx, int frame,
                                 kvx_model_t *model,
                                 rpatch_t    *patches[KVX_NUM_ROTATIONS],
                                 int angle_offset_bucket,
                                 unsigned int placed_spin_per_tic,
                                 unsigned int dropped_spin_per_tic)
{
   int idx;
   int r;

   if (sprite_idx < 0 || sprite_idx >= kvx_table_sprites ||
       frame < 0 || frame >= KVX_MAX_FRAMES)
   {
      for (r = 0; r < KVX_NUM_ROTATIONS; r++)
         if (patches[r])
            R_KVX_FreeSprite(patches[r]);
      R_KVX_Free(model);
      return;
   }

   idx = sprite_idx * KVX_MAX_FRAMES + frame;
   /* Slot is occupied if any rotation is bound (treat the model
    * pointer as the canonical "occupied" flag).  Free everything
    * before overwriting. */
   if (kvx_table[idx].model)
   {
      for (r = 0; r < KVX_NUM_ROTATIONS; r++)
      {
         if (kvx_table[idx].patches[r])
         {
            R_KVX_FreeSprite(kvx_table[idx].patches[r]);
            kvx_table[idx].patches[r] = NULL;
         }
      }
      R_KVX_Free(kvx_table[idx].model);
   }
   else
   {
      kvx_table_count++;
   }
   kvx_table[idx].model = model;
   for (r = 0; r < KVX_NUM_ROTATIONS; r++)
      kvx_table[idx].patches[r] = patches[r];
   kvx_table[idx].angle_offset_bucket  = angle_offset_bucket;
   kvx_table[idx].placed_spin_per_tic  = placed_spin_per_tic;
   kvx_table[idx].dropped_spin_per_tic = dropped_spin_per_tic;
}

/* Build the synthetic 8x8x8 test cube and bind it to MEDI/A as a
 * fallback when no VOXELDEF lump is present in any loaded WAD.
 * This keeps the menu toggle visibly functional on a vanilla IWAD
 * during development. */
static void kvx_register_test_voxel(void)
{
   kvx_model_t *m;
   rpatch_t    *patches[KVX_NUM_ROTATIONS];
   int          sprite_idx;

   sprite_idx = kvx_lookup_sprite_name("MEDI");
   if (sprite_idx < 0)
      return;  /* not a Doom IWAD; nothing to bind */

   m = R_KVX_BuiltinTestVoxel();
   if (!m)
      return;

   /* Rasterize all 8 view rotations of the test cube at the
    * default upscale.  The cube has differently-coloured faces, so
    * each rotation will look distinct -- a useful sanity test that
    * the rotation lookup is wired through correctly. */
   if (!kvx_rasterize_all_rotations(m,
                                    m->x_size * 4, m->z_size * 4,
                                    0, 0, 0, /* no offsets */
                                    patches))
   {
      R_KVX_Free(m);
      return;
   }

   /* Frame 'A' = index 0. */
   kvx_register_mapping(sprite_idx, 0, m, patches,
                        0 /* no angle offset for test cube */,
                        0 /* no placed spin */,
                        0 /* no dropped spin */);
   lprintf(LO_INFO, "R_KVX: test cube bound to MEDI/A "
                    "(fallback, no VOXELDEF loaded)\n");
}

/* --- VOXELDEF parser --------------------------------------------
 *
 * Compatible with the DelphiDoom voxel-definition format used by
 * existing voxel WADs (e.g. CHEX_QUEST_VOXELS_001):
 *
 *   voxeldef "lumpname.kvx"
 *   {
 *     [property [= value]]*
 *     replaces sprite SPRITEFRAME
 *     [property [= value]]*
 *   }
 *
 * SPRITEFRAME is a 5-character token combining a 4-char sprite name
 * and a 1-char frame letter, e.g. "MEDIA" = sprite MEDI, frame A.
 * It may be quoted ("MEDIA") or bare (medi); case is normalized.
 *
 * The lump name in the quoted string may include a ".kvx" extension;
 * we strip it for WAD lump lookup (WAD lump names have no extension).
 *
 * Properties other than 'replaces' (Scale, droppedspin, placedspin,
 * AngleOffset, etc.) are recognized syntactically but silently
 * ignored.  This lets existing voxel WADs load without errors even
 * though we don't yet implement the corresponding behaviors; those
 * features will be wired up in later commits.
 *
 * Whitespace and case are insensitive on keywords.  Comments via
 * '#', ';', or '//' run to end of line.  Parse errors are reported
 * via lprintf(LO_WARN) with approximate line numbers and recovery
 * skips ahead to the next 'voxeldef' keyword; one bad block doesn't
 * fail the whole load.  Returns the number of mappings successfully
 * registered. */

/* Lightweight scanner state.  Stateful position into the source
 * with a 1-based line counter for error messages. */
typedef struct kvx_scan_s
{
   const char *p;
   const char *end;
   int         line;
} kvx_scan_t;

static int kvx_is_space(int c)
{
   return (c == ' ' || c == '\t' || c == '\r');
}

static int kvx_is_word(int c)
{
   /* alphanumeric, underscore, dot, hyphen, plus -- covers sprite
    * names, lump names (with optional .kvx), and numeric values
    * like "0.9" or "-16". */
   return  (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9')
        || c == '_' || c == '.' || c == '-' || c == '+';
}

static int kvx_to_upper(int c)
{
   if (c >= 'a' && c <= 'z')
      return c - 'a' + 'A';
   return c;
}

/* Skip spaces, tabs, newlines, carriage returns, and comments.
 * Newlines bump the line counter.  This is the only whitespace
 * primitive the parser needs -- everything else (commas between
 * key=value pairs, etc.) is also treated as whitespace.  The
 * original line-statement model from the simpler grammar is
 * gone; tokens float in a free-form sea of whitespace and the
 * structure comes from braces and the 'voxeldef' keyword. */
static void kvx_skip_ws(kvx_scan_t *s)
{
   while (s->p < s->end)
   {
      if (*s->p == '\n')
      {
         s->line++;
         s->p++;
      }
      else if (kvx_is_space((unsigned char)*s->p) || *s->p == ',')
      {
         s->p++;
      }
      else if (*s->p == '#' || *s->p == ';')
      {
         while (s->p < s->end && *s->p != '\n')
            s->p++;
      }
      else if (s->p + 1 < s->end && s->p[0] == '/' && s->p[1] == '/')
      {
         while (s->p < s->end && *s->p != '\n')
            s->p++;
      }
      else
      {
         break;
      }
   }
}

/* Read a quoted string into buf (size buflen including the NUL).
 * Returns 1 on success, 0 on error.  Buf is left NUL-terminated. */
static int kvx_read_quoted(kvx_scan_t *s, char *buf, int buflen)
{
   int n = 0;
   if (s->p >= s->end || *s->p != '"')
      return 0;
   s->p++;
   while (s->p < s->end && *s->p != '"' && *s->p != '\n')
   {
      if (n + 1 >= buflen)
         return 0;
      buf[n++] = *s->p++;
   }
   if (s->p >= s->end || *s->p != '"')
      return 0;
   s->p++;
   buf[n] = '\0';
   return 1;
}

/* Read a bare word (alphanumeric, _, ., -, +) into buf.  Returns 1
 * on success, 0 on error.  Buf is uppercased. */
static int kvx_read_word(kvx_scan_t *s, char *buf, int buflen)
{
   int n = 0;
   if (s->p >= s->end || !kvx_is_word((unsigned char)*s->p))
      return 0;
   while (s->p < s->end && kvx_is_word((unsigned char)*s->p))
   {
      if (n + 1 >= buflen)
         return 0;
      buf[n++] = (char)kvx_to_upper((unsigned char)*s->p);
      s->p++;
   }
   buf[n] = '\0';
   return 1;
}

/* Read either a quoted string or a bare word.  Used for sprite
 * names and lump names which may be either form in the wild. */
static int kvx_read_quoted_or_word(kvx_scan_t *s, char *buf, int buflen)
{
   if (s->p >= s->end)
      return 0;
   if (*s->p == '"')
   {
      if (!kvx_read_quoted(s, buf, buflen))
         return 0;
      /* Uppercase quoted content too, so case handling matches the
       * bare-word path. */
      {
         int i;
         for (i = 0; buf[i]; i++)
            buf[i] = (char)kvx_to_upper((unsigned char)buf[i]);
      }
      return 1;
   }
   return kvx_read_word(s, buf, buflen);
}

/* Try to match a keyword (case-insensitive) at the current scan
 * position.  On match, scanner advances past the keyword and returns
 * 1.  On no match, scanner is rewound and returns 0. */
static int kvx_match_keyword(kvx_scan_t *s, const char *kw)
{
   char buf[32];
   const char *save = s->p;
   int          save_line = s->line;
   if (!kvx_read_word(s, buf, sizeof(buf)))
   {
      s->p    = save;
      s->line = save_line;
      return 0;
   }
   {
      int i;
      for (i = 0; kw[i] && buf[i]; i++)
      {
         if (kvx_to_upper((unsigned char)kw[i]) != buf[i])
         {
            s->p    = save;
            s->line = save_line;
            return 0;
         }
      }
      if (kw[i] || buf[i])
      {
         s->p    = save;
         s->line = save_line;
         return 0;
      }
   }
   return 1;
}

/* Strip a trailing ".KVX" extension from a lump name, in place.
 * WAD lump names have no extensions, but VOXELDEF strings tend to
 * include ".kvx" for readability.  Idempotent if no extension. */
static void kvx_strip_kvx_ext(char *name)
{
   int n = (int)strlen(name);
   if (n >= 4 &&
       name[n-4] == '.' &&
       name[n-3] == 'K' &&
       name[n-2] == 'V' &&
       name[n-1] == 'X')
   {
      name[n-4] = '\0';
   }
}

/* Skip past the next '}' or end of input.  Used to recover from a
 * parse error inside a voxeldef block without re-misreading the
 * tokens that already confused us. */
static void kvx_skip_to_close_brace(kvx_scan_t *s)
{
   while (s->p < s->end && *s->p != '}')
   {
      if (*s->p == '\n')
         s->line++;
      s->p++;
   }
   if (s->p < s->end)
      s->p++;  /* consume the } */
}

/* Skip a property value: a quoted string, a single bare word, or
 * an '= word' pair.  Properties we don't recognize get their value
 * silently consumed. */
static void kvx_skip_value(kvx_scan_t *s)
{
   char dummy[32];
   kvx_skip_ws(s);
   if (s->p < s->end && *s->p == '=')
   {
      s->p++;
      kvx_skip_ws(s);
   }
   if (s->p >= s->end)
      return;
   if (*s->p == '"')
      kvx_read_quoted(s, dummy, sizeof(dummy));
   else if (kvx_is_word((unsigned char)*s->p))
      kvx_read_word(s, dummy, sizeof(dummy));
}

/* Read a numeric property value (with optional leading '=', optional
 * sign, integer or simple decimal).  Stores into *out_value as a
 * double.  Returns 1 on success, 0 if no number could be read.
 *
 * Accepts the forms: "= 1.5", "= -90", "0.875", "180", " = +0.5".
 * Doesn't handle exponent notation (1e3) -- VOXELDEF in the wild
 * doesn't use it, and avoiding it lets us roll our own parser
 * without atof's locale dependence on the decimal separator (atof
 * treats "0,9" as a valid number in some locales, which would
 * silently mis-parse). */
static int kvx_read_number(kvx_scan_t *s, double *out_value)
{
   const char *start;
   const char *p;
   int         sign = 1;
   double      whole = 0.0;
   double      frac = 0.0;
   double      frac_div = 1.0;
   int         have_digit = 0;
   int         in_frac = 0;

   kvx_skip_ws(s);
   if (s->p < s->end && *s->p == '=')
   {
      s->p++;
      kvx_skip_ws(s);
   }

   start = s->p;
   p     = s->p;

   if (p < s->end && (*p == '+' || *p == '-'))
   {
      if (*p == '-') sign = -1;
      p++;
   }

   while (p < s->end)
   {
      char c = *p;
      if (c >= '0' && c <= '9')
      {
         if (!in_frac)
         {
            whole = whole * 10.0 + (double)(c - '0');
         }
         else
         {
            frac = frac * 10.0 + (double)(c - '0');
            frac_div *= 10.0;
         }
         have_digit = 1;
         p++;
      }
      else if (c == '.' && !in_frac)
      {
         in_frac = 1;
         p++;
      }
      else
      {
         break;
      }
   }

   if (!have_digit)
   {
      /* No digits consumed -- restore the scanner and bail. */
      s->p = start;
      return 0;
   }

   *out_value = (double)sign * (whole + frac / frac_div);
   s->p = p;
   return 1;
}

/* Parse a single voxeldef block.  Scanner is positioned just after
 * the 'voxeldef' keyword; we read the lump name, the '{', the
 * properties, and the '}'.  Updates *stats based on outcome.
 * Returns 1 if a mapping was registered, 0 otherwise.  On parse
 * error, recovers to the next '}'. */
static int kvx_parse_voxeldef_block(kvx_scan_t *s,
                                    kvx_parse_stats_t *stats)
{
   char         lump[64];
   char         spriteframe[16];
   char         keyword[32];
   int          sprite_idx = -1;
   int          frame = -1;
   int          have_replaces = 0;
   kvx_model_t *m = NULL;
   rpatch_t    *patches[KVX_NUM_ROTATIONS];
   int          start_line;
   int          r;

   /* Optional VOXELDEF properties.  Defaults: scale=1.0 means "use
    * the original sprite's dimensions verbatim"; angle_offset_deg=0
    * means "voxel was authored with the same front-facing convention
    * Doom expects".  spin rates default to 0 (don't spin). */
   double       scale_factor = 1.0;
   double       angle_offset_deg = 0.0;
   double       placed_spin_deg_per_sec  = 0.0;
   double       dropped_spin_deg_per_sec = 0.0;

   for (r = 0; r < KVX_NUM_ROTATIONS; r++)
      patches[r] = NULL;

   start_line = s->line;

   kvx_skip_ws(s);
   if (!kvx_read_quoted(s, lump, sizeof(lump)))
   {
      lprintf(LO_WARN, "VOXELDEF line %d: expected quoted lump name "
              "after 'voxeldef'\n", start_line);
      stats->syntax_error++;
      return 0;
   }

   /* Uppercase the lump name and strip a trailing .KVX extension
    * for WAD lookup. */
   {
      int i;
      for (i = 0; lump[i]; i++)
         lump[i] = (char)kvx_to_upper((unsigned char)lump[i]);
   }
   kvx_strip_kvx_ext(lump);

   kvx_skip_ws(s);
   if (s->p >= s->end || *s->p != '{')
   {
      lprintf(LO_WARN, "VOXELDEF line %d: expected '{' after lump "
              "name '%s'\n", start_line, lump);
      stats->syntax_error++;
      return 0;
   }
   s->p++;

   /* Property loop: read keyword/value pairs until '}'. */
   for (;;)
   {
      kvx_skip_ws(s);
      if (s->p >= s->end)
      {
         lprintf(LO_WARN, "VOXELDEF line %d: unexpected end of input "
                 "in voxeldef block\n", start_line);
         stats->syntax_error++;
         return 0;
      }
      if (*s->p == '}')
      {
         s->p++;
         break;
      }

      if (!kvx_read_word(s, keyword, sizeof(keyword)))
      {
         lprintf(LO_WARN, "VOXELDEF line %d: malformed property in "
                 "voxeldef '%s'\n", s->line, lump);
         kvx_skip_to_close_brace(s);
         stats->syntax_error++;
         return 0;
      }

      /* The interesting case: 'replaces' takes 'sprite SPRITEFRAME'. */
      if (keyword[0] == 'R' && strcmp(keyword, "REPLACES") == 0)
      {
         kvx_skip_ws(s);
         /* Optional 'sprite' qualifier (DelphiDoom always emits it,
          * but lenient parsers also accept 'replaces SPRITEFRAME'). */
         if (kvx_match_keyword(s, "sprite"))
            kvx_skip_ws(s);

         if (!kvx_read_quoted_or_word(s, spriteframe, sizeof(spriteframe)))
         {
            lprintf(LO_WARN, "VOXELDEF line %d: 'replaces' needs a "
                    "5-char SPRITE+FRAME token\n", s->line);
            kvx_skip_to_close_brace(s);
            stats->syntax_error++;
            return 0;
         }

         if (strlen(spriteframe) != 5)
         {
            lprintf(LO_WARN, "VOXELDEF line %d: 'replaces sprite %s' "
                    "must be exactly 5 characters (sprite name + frame "
                    "letter)\n", s->line, spriteframe);
            kvx_skip_to_close_brace(s);
            stats->syntax_error++;
            return 0;
         }
         if (spriteframe[4] < 'A' ||
             spriteframe[4] >= 'A' + KVX_MAX_FRAMES)
         {
            lprintf(LO_WARN, "VOXELDEF line %d: '%s' frame letter must "
                    "be A..%c\n", s->line, spriteframe,
                    'A' + KVX_MAX_FRAMES - 1);
            kvx_skip_to_close_brace(s);
            stats->syntax_error++;
            return 0;
         }

         {
            char sprite[5];
            sprite[0] = spriteframe[0];
            sprite[1] = spriteframe[1];
            sprite[2] = spriteframe[2];
            sprite[3] = spriteframe[3];
            sprite[4] = '\0';
            sprite_idx = kvx_lookup_sprite_name(sprite);
            if (sprite_idx < 0)
            {
               /* Demoted from LO_WARN to LO_INFO: this is the most
                * common rejection reason (e.g. Chex Quest voxels
                * loaded against doom2.wad name Chex sprites that
                * don't exist).  Not really a warning -- it's
                * normal cross-IWAD content mismatch.  The summary
                * line at end of init shows the total count. */
               lprintf(LO_INFO, "VOXELDEF line %d: unknown sprite '%s' "
                       "for voxel '%s'\n", s->line, sprite, lump);
               kvx_skip_to_close_brace(s);
               stats->unknown_sprite++;
               return 0;
            }
         }
         frame = spriteframe[4] - 'A';
         have_replaces = 1;
         continue;
      }

      /* Scale: voxel size multiplier.  1.0 = match original sprite
       * dimensions exactly; 0.9 = render 10% smaller; 1.1 = 10%
       * larger.  Useful for fine-tuning voxels that look slightly
       * off-scale relative to the sprite they replace. */
      if (keyword[0] == 'S' && strcmp(keyword, "SCALE") == 0)
      {
         double v;
         if (!kvx_read_number(s, &v))
         {
            lprintf(LO_WARN, "VOXELDEF line %d: 'Scale' needs a "
                    "numeric value\n", s->line);
            kvx_skip_to_close_brace(s);
            stats->syntax_error++;
            return 0;
         }
         /* Clamp to a sensible range.  Anything outside [0.1, 10]
          * is almost certainly a typo; silently clamp instead of
          * trusting it and producing a degenerate 1-pixel sprite
          * or a 10000x10000 monstrosity. */
         if (v < 0.1)  v = 0.1;
         if (v > 10.0) v = 10.0;
         scale_factor = v;
         continue;
      }

      /* AngleOffset: per-voxel rotation correction in degrees.
       * Positive values rotate the voxel CW relative to thing
       * angle.  Used when a voxel was authored with a different
       * "natural front" convention than Doom expects (e.g. KVX
       * authored from Build engine which has Y flipped vs Doom). */
      if (keyword[0] == 'A' && strcmp(keyword, "ANGLEOFFSET") == 0)
      {
         double v;
         if (!kvx_read_number(s, &v))
         {
            lprintf(LO_WARN, "VOXELDEF line %d: 'AngleOffset' needs "
                    "a numeric value (degrees)\n", s->line);
            kvx_skip_to_close_brace(s);
            stats->syntax_error++;
            return 0;
         }
         angle_offset_deg = v;
         continue;
      }

      /* DroppedSpin: rotation rate (deg/sec) applied to a thing the
       * engine flagged as MF_DROPPED -- i.e. a pickup left behind
       * when an enemy died.  Voxel mods use this to make dropped
       * weapons/health spin in place like Quake pickups, drawing
       * attention even though the underlying sprite is static. */
      if (keyword[0] == 'D' && strcmp(keyword, "DROPPEDSPIN") == 0)
      {
         double v;
         if (!kvx_read_number(s, &v))
         {
            lprintf(LO_WARN, "VOXELDEF line %d: 'droppedspin' needs "
                    "a numeric value (degrees/sec)\n", s->line);
            kvx_skip_to_close_brace(s);
            stats->syntax_error++;
            return 0;
         }
         dropped_spin_deg_per_sec = v;
         continue;
      }

      /* PlacedSpin: rotation rate (deg/sec) applied to a thing in
       * its initial map placement (everything not flagged
       * MF_DROPPED).  Less commonly used than droppedspin in real
       * voxel mods, but recognised for completeness. */
      if (keyword[0] == 'P' && strcmp(keyword, "PLACEDSPIN") == 0)
      {
         double v;
         if (!kvx_read_number(s, &v))
         {
            lprintf(LO_WARN, "VOXELDEF line %d: 'placedspin' needs "
                    "a numeric value (degrees/sec)\n", s->line);
            kvx_skip_to_close_brace(s);
            stats->syntax_error++;
            return 0;
         }
         placed_spin_deg_per_sec = v;
         continue;
      }

      /* Any other property: known DelphiDoom extras we don't yet
       * handle, or unknown tokens.  Recognized syntactically,
       * ignored semantically.  Consume the '=value' or ' value'
       * that follows. */
      kvx_skip_value(s);
   }

   if (!have_replaces)
   {
      lprintf(LO_WARN, "VOXELDEF line %d: voxeldef '%s' has no "
              "'replaces sprite SPRITEFRAME'\n", start_line, lump);
      stats->syntax_error++;
      return 0;
   }

   /* Load the KVX lump and bind, applying the scale factor to
    * target dimensions. */
   if (!kvx_load_kvx_lump(lump, sprite_idx, frame, scale_factor,
                          &m, patches))
   {
      /* kvx_load_kvx_lump already logged the failure */
      stats->bad_lump++;
      return 0;
   }

   /* Convert degrees to eighth-turn buckets.  Round to nearest
    * bucket, normalise into [0, 7].  Negative angles wrap correctly
    * (-90 deg -> -2 -> 6 = three-quarters turn, equivalent to +270). */
   {
      int bucket;
      double normalised = angle_offset_deg / 45.0;
      /* Round half away from zero. */
      unsigned int placed_per_tic;
      unsigned int dropped_per_tic;

      if (normalised >= 0.0)
         bucket = (int)(normalised + 0.5);
      else
         bucket = -(int)(-normalised + 0.5);
      bucket &= (KVX_NUM_ROTATIONS - 1);  /* &7 wraps negatives */

      /* Convert deg/sec to angle_t units per gametic.  ANG45 covers
       * 45 degrees, so 1 degree = ANG45/45.  Doom runs at TICRATE
       * (35) tics/sec, so per-tic angle = deg/sec * ANG45 / (45 *
       * TICRATE).  Cast through double to avoid integer overflow on
       * large rates -- ANG45 itself fits in unsigned, but the
       * intermediate product wouldn't.  Negative spin rates produce
       * a CCW spin (angle_t arithmetic naturally wraps). */
      placed_per_tic  = (unsigned int)(placed_spin_deg_per_sec  *
                                       ((double)ANG45 / 45.0 / 35.0));
      dropped_per_tic = (unsigned int)(dropped_spin_deg_per_sec *
                                       ((double)ANG45 / 45.0 / 35.0));

      /* Per-mapping success line.  Useful when debugging "voxel X
       * doesn't show up" -- if the line is here, the mapping landed
       * and the issue is downstream (rendering, lookup, etc.).
       * Logged before kvx_register_mapping takes ownership of the
       * patches so we can read width/height from patches[0]. */
      if (scale_factor != 1.0 || bucket != 0 ||
          placed_per_tic != 0 || dropped_per_tic != 0)
         lprintf(LO_INFO, "R_KVX: %s%c -> %s (%dx%d, 8 rotations, "
                          "scale=%.2f, angle_offset=%d/8, "
                          "placed_spin=%g, dropped_spin=%g deg/s)\n",
                 sprnames[sprite_idx], 'A' + frame, lump,
                 patches[0]->width, patches[0]->height,
                 scale_factor, bucket,
                 placed_spin_deg_per_sec, dropped_spin_deg_per_sec);
      else
         lprintf(LO_INFO, "R_KVX: %s%c -> %s (%dx%d, 8 rotations)\n",
                 sprnames[sprite_idx], 'A' + frame, lump,
                 patches[0]->width, patches[0]->height);
      kvx_register_mapping(sprite_idx, frame, m, patches, bucket,
                           placed_per_tic, dropped_per_tic);
   }
   stats->registered++;
   return 1;
}

static int kvx_parse_voxeldef(const char *src, int len,
                              kvx_parse_stats_t *stats)
{
   kvx_scan_t   s;

   s.p    = src;
   s.end  = src + len;
   s.line = 1;

   while (s.p < s.end)
   {
      kvx_skip_ws(&s);
      if (s.p >= s.end)
         break;

      if (kvx_match_keyword(&s, "voxeldef"))
      {
         kvx_parse_voxeldef_block(&s, stats);
         /* On failure, kvx_parse_voxeldef_block has already
          * recovered to past the matching '}' and bumped the
          * appropriate stats counter, so we just continue. */
      }
      else
      {
         /* Stray garbage at top level.  Report once and skip the
          * offending token to make progress. */
         char garbage[32];
         int  err_line = s.line;
         if (kvx_read_word(&s, garbage, sizeof(garbage)))
         {
            lprintf(LO_WARN, "VOXELDEF line %d: unexpected token "
                    "'%s' at top level (expected 'voxeldef')\n",
                    err_line, garbage);
            stats->syntax_error++;
         }
         else
            s.p++;  /* couldn't even read a token; skip 1 char */
      }
   }

   return stats->registered;
}

#endif /* !KVX_NO_ENGINE_GLUE */

#endif /* !KVX_PARSER_ONLY */
#endif /* !KVX_TEST_MAIN */

#ifdef KVX_TEST_MAIN

/* Self-test harness.  Build with:
 *   cc -DKVX_TEST_MAIN -o kvx_test src/r_voxel.c
 * then:
 *   ./kvx_test path/to/file.kvx
 *
 * Prints a summary of the model and the first few slabs.  Used to
 * sanity-check the parser without engine integration. */

#include <inttypes.h>

static void dump_slab(const kvx_model_t *m, int x, int y)
{
   int A = m->offsets[ y      * m->x_size + x];
   int B = m->offsets[(y + 1) * m->x_size + x];
   int q = A;
   int n = 0;

   if (A == B)
   {
      printf("    column (%d,%d): empty\n", x, y);
      return;
   }
   printf("    column (%d,%d): %d bytes\n", x, y, B - A);
   while (q < B && n < 4)
   {
      int ztop = m->data[q + 0];
      int zlen = m->data[q + 1];
      int face = m->data[q + 2];
      printf("      slab %d: ztop=%d zlen=%d face=0x%02x"
             " colors=[", n, ztop, zlen, face);
      {
         int i;
         int dump_count = zlen < 8 ? zlen : 8;
         for (i = 0; i < dump_count; i++)
            printf("%s%d", i ? "," : "", m->data[q + 3 + i]);
         if (zlen > dump_count) printf(",...");
         printf("]\n");
      }
      q += 3 + zlen;
      n++;
   }
   if (q < B) printf("      ... (more slabs)\n");
}

int main(int argc, char **argv)
{
   FILE *f;
   long fsz;
   void *buf;
   kvx_model_t *m;
   int x, y;
   int total_slabs = 0;
   int total_voxels = 0;

   if (argc != 2)
   {
      fprintf(stderr, "usage: %s file.kvx\n", argv[0]);
      return 2;
   }

   f = fopen(argv[1], "rb");
   if (!f) { perror(argv[1]); return 1; }

   fseek(f, 0, SEEK_END);
   fsz = ftell(f);
   fseek(f, 0, SEEK_SET);

   if (fsz < 0)
   {
      fclose(f);
      fprintf(stderr, "ftell failed\n");
      return 1;
   }

   buf = malloc((size_t)fsz);
   if (!buf) { fclose(f); fprintf(stderr, "oom\n"); return 1; }

   if (fread(buf, 1, (size_t)fsz, f) != (size_t)fsz)
   {
      fclose(f);
      free(buf);
      fprintf(stderr, "short read\n");
      return 1;
   }
   fclose(f);

   m = R_KVX_Load(buf, (size_t)fsz);
   free(buf);

   if (!m)
   {
      fprintf(stderr, "R_KVX_Load: failed to parse %s\n", argv[1]);
      return 1;
   }

   printf("model: %dx%dx%d voxels\n", m->x_size, m->y_size, m->z_size);
   printf("pivot: (%.4f, %.4f, %.4f) voxel units\n",
          m->x_pivot_fx / 65536.0,
          m->y_pivot_fx / 65536.0,
          m->z_pivot_fx / 65536.0);
   printf("slab data: %zu bytes\n", m->data_size);

   /* Walk all columns, count slabs and voxels. */
   for (x = 0; x < m->x_size; x++)
   {
      for (y = 0; y < m->y_size; y++)
      {
         int A = m->offsets[ y      * m->x_size + x];
         int B = m->offsets[(y + 1) * m->x_size + x];
         int q = A;
         while (q < B)
         {
            int zlen = m->data[q + 1];
            total_slabs++;
            total_voxels += zlen;
            q += 3 + zlen;
         }
      }
   }
   printf("slabs: %d totaling %d visible voxels\n",
          total_slabs, total_voxels);

   /* Dump first non-empty column found, plus the (0,0) column. */
   printf("first columns:\n");
   dump_slab(m, 0, 0);
   {
      int found = 0;
      for (x = 0; x < m->x_size && !found; x++)
      {
         for (y = 0; y < m->y_size && !found; y++)
         {
            int A = m->offsets[ y      * m->x_size + x];
            int B = m->offsets[(y + 1) * m->x_size + x];
            if (A < B && (x != 0 || y != 0))
            {
               dump_slab(m, x, y);
               found = 1;
            }
         }
      }
   }

   /* Palette spot-check. */
   printf("palette[0..3] (raw 0-63): %d %d %d, %d %d %d, %d %d %d, %d %d %d\n",
      m->palette[0], m->palette[1], m->palette[2],
      m->palette[3], m->palette[4], m->palette[5],
      m->palette[6], m->palette[7], m->palette[8],
      m->palette[9], m->palette[10], m->palette[11]);

   R_KVX_Free(m);
   return 0;
}

#endif /* KVX_TEST_MAIN */
