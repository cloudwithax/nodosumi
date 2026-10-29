#ifndef MAPLE_H
#define MAPLE_H

#include "guest/maple/maple_types.h"

struct dreamcast;
struct maple;
struct maple_device;
struct state;

enum {
  MAPLE_DEV_NONE,
  MAPLE_DEV_CONTROLLER,
  MAPLE_DEV_VMU,
  MAPLE_DEV_PURUPURU,
};

struct maple_device {
  struct maple *mp;
  int type;
  void (*destroy)(struct maple_device *);
  int (*input)(struct maple_device *, int, int16_t);
  int (*frame)(struct maple_device *, const union maple_frame *,
               union maple_frame *);
  void (*save_state)(struct maple_device *, struct state *);
  void (*load_state)(struct maple_device *, struct state *);
};

uint8_t maple_encode_addr(int port, int unit);
int maple_decode_addr(uint32_t addr, int *port, int *unit);

struct maple *maple_create(struct dreamcast *dc);
void maple_destroy(struct maple *mp);

struct maple_device *maple_get_device(struct maple *mp, int port, int unit);
void maple_handle_input(struct maple *mp, int port, int button, int16_t value);
int maple_handle_frame(struct maple *mp, int port, union maple_frame *frame,
                       union maple_frame *res);
void maple_vibrate(struct maple *mp, int port, float power, int duration_ms);

struct maple_device *controller_create(struct maple *mp, int port);
struct maple_device *vmu_create(struct maple *mp, int port);
struct maple_device *purupuru_create(struct maple *mp, int port);

#endif
