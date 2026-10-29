#ifndef TR_H
#define TR_H

#include "guest/pvr/ta_types.h"
#include "render/render_backend.h"

struct tr;

#define TR_MAX_SURFS (1024 * 64)
#define TR_MAX_MODVOLS 8192

typedef uint64_t tr_texture_key_t;

struct tr_texture {
  union tsp tsp;
  union tcw tcw;
  unsigned frame;
  int dirty;

  /* source info */
  const uint8_t *texture;
  int texture_size;
  const uint8_t *palette;
  int palette_size;
  /* hash of the palette and palette format the handle was decoded with */
  uint64_t palette_hash;

  /* backend info */
  enum filter_mode filter;
  enum wrap_mode wrap_u;
  enum wrap_mode wrap_v;
  int format;
  int width;
  int height;
  texture_handle_t handle;
};

struct tr_param {
  /* offset of parameter in ta_context param stream */
  int offset;

  /* global list and vertex types at time of parsing */
  int list_type;
  int vert_type;

  /* last surf / vert generated for the param */
  int last_surf;
  int last_vert;
};

struct tr_list {
  int surfs[TR_MAX_SURFS];
  int num_surfs;

  /* debug info */
  int num_orig_surfs;
};

struct tr_context {
  /* original video dimensions, needed to project surfaces correctly */
  int width;
  int height;

  /* parsed surfaces and vertices, ready to be passed to the render backend */
  struct ta_surface surfs[TR_MAX_SURFS];
  int num_surfs;

  /* uploaded when the context is drawn, so the convert needs no gl */
  struct ta_vertex verts[TR_MAX_SURFS];
  int num_verts;

  uint16_t indices[TR_MAX_SURFS * 3];
  int num_indices;

  /* sorted list of surfaces corresponding to each of the ta's polygon lists */
  struct tr_list lists[TA_NUM_LISTS];

  /* the opaque modifier volumes, which shadow the opaque and punch through
     surfaces. their triangles are in the vertices, as is the quad at
     shadow_vert that the shadow is drawn with */
  struct ta_modvol modvols[TR_MAX_MODVOLS];
  int num_modvols;
  int shadow_vert;

  /* debug structures for stepping through the param stream in the tracer,
     only filled in when track_params is set */
  struct tr_param params[TA_MAX_PARAMS];
  int num_params;
  int track_params;
};

static inline tr_texture_key_t tr_texture_key(union tsp tsp, union tcw tcw) {
  return ((uint64_t)tsp.full << 32) | tcw.full;
}

static inline uint64_t tr_palette_hash(const uint8_t *palette, int size,
                                       int palette_fmt) {
  /* fnv-1a, seeded with the format the entries are read as */
  uint64_t h = (0xcbf29ce484222325ull ^ palette_fmt) * 0x100000001b3ull;
  for (int i = 0; i < size; i++) {
    h = (h ^ palette[i]) * 0x100000001b3ull;
  }
  return h;
}

typedef struct tr_texture *(*tr_find_texture_cb)(void *, union tsp, union tcw);

void tr_convert_context(struct render_backend *r, void *userdata,
                        tr_find_texture_cb find_texture,
                        const struct ta_context *ctx, struct tr_context *rc);
void tr_render_context(struct render_backend *r, const struct tr_context *rc);
void tr_render_context_until(struct render_backend *r,
                             const struct tr_context *rc, int end_surf);

#endif
