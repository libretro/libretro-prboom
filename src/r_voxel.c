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

rpatch_t *R_KVX_RasterizeFront(const kvx_model_t *m,
                               const uint8_t *palette_remap)
{
   /* Output dimensions: front view = looking down -Y.  Each output
    * column corresponds to a model X plane; each output row to a
    * model Z plane.  Depth axis is Y (model "into the page").
    *
    * The output sprite is upscaled by a constant factor: each voxel
    * becomes UPSCALE x UPSCALE pixels.  This is purely a sprite-size
    * concern -- a real Doom medikit sprite is ~16-25 pixels tall, but
    * an 8-tall voxel rasterized 1:1 produces a tiny sprite that's
    * barely visible at typical view distances on modern displays.
    * Upscaling here is a placeholder; Turn 3's per-actor scale
    * factor (from VOXELDEF) makes this configurable. */
#define KVX_UPSCALE 4
   int      width;
   int      height;
   int      x, z, y;
   uint8_t *pixels;
   rpatch_t *patch;
   size_t   pixels_size;
   int      num_posts_total;
   size_t   columns_size;
   size_t   posts_size;
   size_t   data_size;
   int      post_index;

   if (!m)
      return NULL;

   width  = m->x_size * KVX_UPSCALE;
   height = m->z_size * KVX_UPSCALE;

   if (width <= 0 || height <= 0)
      return NULL;

   /* First pass: rasterize directly into a column-major scratch
    * buffer.  pixels[col * height + row] = color, or
    * KVX_TRANSPARENT if no opaque voxel at this position.  Each
    * model voxel becomes a KVX_UPSCALE x KVX_UPSCALE block of
    * identical pixels in the output. */
   pixels = (uint8_t *)malloc((size_t)width * (size_t)height);
   if (!pixels)
      return NULL;

   for (x = 0; x < width; x++)
   {
      int mx = x / KVX_UPSCALE;
      for (z = 0; z < height; z++)
      {
         int mz = z / KVX_UPSCALE;
         uint8_t color = KVX_TRANSPARENT;

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

   /* Second pass: count posts (vertical runs of opaque pixels) per
    * column.  This gives us the size of the posts allocation. */
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

   /* Layout the rpatch_t.  We mirror r_patch.c's "single allocation
    * holding pixels + columns + posts" pattern, except we use plain
    * malloc instead of Z_Malloc.  Since voxel-rasterized patches
    * have a different lifetime than WAD patches (we own them and
    * free them with R_KVX_FreeSprite), they live outside the patch
    * cache entirely. */
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

   patch->width        = width;
   patch->height       = height;
   patch->widthmask    = 0;     /* sprites are not tileable */
   patch->isNotTileable= 1;
   /* Sprite origin: center horizontally, bottom at row 0.  This is
    * a reasonable default for a voxel meant to replace a pickup
    * sprite (which sits on the ground).  Real KVX models specify
    * pivot points -- Turn 3 will respect them. */
   patch->leftoffset   = width / 2;
   patch->topoffset    = height;
   patch->locks        = 0;

   patch->pixels  = patch->data;
   patch->columns = (rcolumn_t *)(patch->data + pixels_size);
   patch->posts   = (rpost_t *)(patch->data + pixels_size + columns_size);

   /* Copy the rasterized pixels in. */
   memcpy(patch->pixels, pixels, pixels_size);
   free(pixels);

   /* Third pass: fill in column and post tables. */
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

      /* Close any post still open at column end. */
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

#include "info.h"           /* for sprnames[] */
#include "w_wad.h"          /* W_CheckNumForName */
#include "r_state.h"        /* firstspritelump */
#include "lprintf.h"

/* User-controlled toggle.  Bound to the "voxel_sprites" cvar (see
 * m_misc.c) and the General > Display Options > "Voxel sprites" menu
 * entry (see m_menu.c).  Default is 0 (off) -- voxel rendering is an
 * opt-in feature.
 *
 * When this is 0, R_KVX_Init() leaves the slot empty so
 * R_KVX_LookupSprite() returns NULL for every lump and the engine
 * renders normal sprites.  R_KVX_LookupSprite() additionally checks
 * this flag at lookup time as a defense in depth, so flipping the
 * flag without re-running Init still does the right thing.
 *
 * This is a developer-facing placeholder for Turn 3's VOXELDEF lump
 * system, where users will see voxels only when they explicitly load
 * a voxel mod -- but the menu plumbing established here will carry
 * forward as the on/off toggle for that system. */
int voxel_sprites = 0;

static int           kvx_slot_lump  = -1;     /* absolute sprite lump number */
static rpatch_t     *kvx_slot_patch = NULL;
static kvx_model_t  *kvx_slot_model = NULL;

void R_KVX_Init(void)
{
   /* Tear down any previous slot (libretro reload safety, and
    * also the OFF->ON->OFF->ON path from M_ChangeVoxelSprites). */
   R_KVX_Shutdown();

   if (!voxel_sprites)
   {
      lprintf(LO_INFO, "R_KVX_Init: voxel sprites disabled "
                       "(toggle via Options > General > Display)\n");
      return;
   }

   /* Build the synthetic test model. */
   kvx_slot_model = R_KVX_BuiltinTestVoxel();
   if (!kvx_slot_model)
      return;

   /* Rasterize the front view.  Identity palette mapping: the test
    * voxel's color values are already PLAYPAL indices. */
   kvx_slot_patch = R_KVX_RasterizeFront(kvx_slot_model, NULL);
   if (!kvx_slot_patch)
   {
      R_KVX_Free(kvx_slot_model);
      kvx_slot_model = NULL;
      return;
   }

   /* Bind to the medikit pickup sprite (MEDI), frame A.  The frame-
    * A lump is named "MEDIA0" in the WAD.  Sprite lumps live in the
    * ns_sprites namespace (between S_START / S_END markers); the
    * default ns_global namespace doesn't see them, so we must call
    * the namespace-aware form explicitly.  Returns -1 if not
    * present (e.g. running on a non-Doom IWAD that lacks this
    * sprite). */
   {
      int lump = (W_CheckNumForName)("MEDIA0", ns_sprites);
      if (lump < 0)
      {
         lprintf(LO_INFO, "R_KVX_Init: MEDIA0 lump not found, voxel "
                          "test stub disabled\n");
         R_KVX_FreeSprite(kvx_slot_patch);
         R_KVX_Free(kvx_slot_model);
         kvx_slot_patch = NULL;
         kvx_slot_model = NULL;
         return;
      }
      kvx_slot_lump = lump;
   }

   lprintf(LO_INFO, "R_KVX_Init: voxel test stub bound to "
                    "MEDIA0 (lump %d)\n", kvx_slot_lump);
}

void R_KVX_Shutdown(void)
{
   if (kvx_slot_patch)
   {
      R_KVX_FreeSprite(kvx_slot_patch);
      kvx_slot_patch = NULL;
   }
   if (kvx_slot_model)
   {
      R_KVX_Free(kvx_slot_model);
      kvx_slot_model = NULL;
   }
   kvx_slot_lump = -1;
}

const rpatch_t *R_KVX_LookupSprite(int lump)
{
   /* Defense in depth: if the user disabled voxel sprites without
    * triggering a re-init (shouldn't happen via the menu callback,
    * but cheap to guard), short-circuit here so we never serve a
    * stale slot. */
   if (!voxel_sprites)
      return NULL;
   if (kvx_slot_lump >= 0 && lump == kvx_slot_lump)
      return kvx_slot_patch;
   return NULL;
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
