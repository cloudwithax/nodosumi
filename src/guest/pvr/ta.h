#ifndef TA_H
#define TA_H

#include "guest/pvr/ta_types.h"
#include "guest/pvr/tex.h"

struct dreamcast;
struct ta;

struct ta *ta_create(struct dreamcast *dc);
void ta_destroy(struct ta *ta);

void ta_soft_reset(struct ta *ta);
void ta_start_render(struct ta *ta);
void ta_end_renders(struct ta *ta);
void ta_list_init(struct ta *ta);
void ta_list_cont(struct ta *ta);
void ta_yuv_init(struct ta *ta);
void ta_texture_info(struct ta *ta, union tsp tsp, union tcw tcw,
                     const uint8_t **texture, int *texture_size,
                     const uint8_t **palette, int *palette_size);

void ta_poly_write(struct ta *ta, uint32_t dst, const uint8_t *src, int size);
void ta_poly_write32(struct ta *ta, const uint8_t *src);
void ta_yuv_write(struct ta *ta, uint32_t dst, const uint8_t *src, int size);
void ta_texture_write(struct ta *ta, uint32_t dst, const uint8_t *src,
                      int size);

/*
 * parameter stream processing helpers, shared by both the ta and tr
 */
/* the pcw's list type is 3 bits, so the types are looked up for the 3 past
   the lists too, as polygons like flycast sizes them */
#define TA_NUM_LIST_TYPES 8

extern int ta_param_sizes[0x100 * TA_NUM_PARAMS * TA_NUM_VERTS];
extern int ta_poly_types[0x100 * TA_NUM_PARAMS * TA_NUM_LIST_TYPES];
extern int ta_vert_types[0x100 * TA_NUM_PARAMS * TA_NUM_LIST_TYPES];

/* the list type is only read from the first global param of a list. the
   params after it are in that list whatever their own bits say, as on the
   hardware, so they're looked up as in it */
static inline union pcw ta_pcw_in_list(union pcw pcw, int list_type) {
  if (list_type != TA_NUM_LISTS) {
    pcw.list_type = list_type;
  }
  return pcw;
}

static inline int ta_param_size(union pcw pcw, int vert_type) {
  /* a modifier volume's global param is 32 bytes, whatever its obj_control
     would make a polygon's */
  if (pcw.para_type == TA_PARAM_POLY_OR_VOL &&
      (pcw.list_type == TA_LIST_OPAQUE_MODVOL ||
       pcw.list_type == TA_LIST_TRANSLUCENT_MODVOL)) {
    return 32;
  }
  return ta_param_sizes[pcw.obj_control * TA_NUM_PARAMS * TA_NUM_VERTS +
                        pcw.para_type * TA_NUM_VERTS + vert_type];
}

static inline int ta_poly_type(union pcw pcw) {
  return ta_poly_types[pcw.obj_control * TA_NUM_PARAMS * TA_NUM_LIST_TYPES +
                       pcw.para_type * TA_NUM_LIST_TYPES + pcw.list_type];
}

static inline int ta_vert_type(union pcw pcw) {
  return ta_vert_types[pcw.obj_control * TA_NUM_PARAMS * TA_NUM_LIST_TYPES +
                       pcw.para_type * TA_NUM_LIST_TYPES + pcw.list_type];
}

static inline int ta_pcw_list_type_valid(union pcw pcw, int current_list_type) {
  /* pcw.list_type is only valid for the first global parameter / object list
     set after TA_LIST_INIT or a previous TA_PARAM_END_OF_LIST. its 3 bits go
     past the 5 list types, and a list of the others never starts, as in
     flycast */
  return current_list_type == TA_NUM_LISTS && pcw.list_type < TA_NUM_LISTS &&
         (pcw.para_type == TA_PARAM_OBJ_LIST_SET ||
          pcw.para_type == TA_PARAM_POLY_OR_VOL ||
          pcw.para_type == TA_PARAM_SPRITE);
}

void ta_init_tables();

/*
 * texture info helpers, shared by both the ta and tr
 */
uint32_t ta_texture_addr(union tsp tsp, union tcw tcw, int *size);
uint32_t ta_palette_addr(union tcw tcw, int *size);
int ta_texture_format(union tcw tcw);
int ta_texture_compressed(union tcw tcw);
int ta_texture_twiddled(union tcw tcw);
int ta_texture_mipmaps(union tcw tcw);
int ta_texture_width(union tsp tsp, union tcw tcw);
int ta_texture_height(union tsp tsp, union tcw tcw);
int ta_texture_stride(union tsp tsp, union tcw tcw, int stride);

#endif
