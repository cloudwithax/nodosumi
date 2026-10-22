#ifndef STATE_H
#define STATE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * serialization cursor. when data is NULL only offset moves, which lets
 * callers find the size by running the same code that saves.
 */
struct state {
  uint8_t *data;
  size_t size;
  size_t offset;
};

static inline void state_write(struct state *s, const void *ptr, size_t len) {
  if (s->data && s->offset + len <= s->size) {
    memcpy(s->data + s->offset, ptr, len);
  }
  s->offset += len;
}

static inline void state_read(struct state *s, void *ptr, size_t len) {
  if (s->data && s->offset + len <= s->size) {
    memcpy(ptr, s->data + s->offset, len);
  }
  s->offset += len;
}

#endif
