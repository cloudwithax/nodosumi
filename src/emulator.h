#ifndef EMULATOR_H
#define EMULATOR_H

#include <stddef.h>
#include <stdint.h>
#include "host/keycode.h"

struct emu;
struct host;
struct render_backend;

struct emu *emu_create(struct host *host);
void emu_destroy(struct emu *emu);

void emu_vid_created(struct emu *emu, struct render_backend *r);
void emu_vid_destroyed(struct emu *emu);
int emu_keydown(struct emu *emu, int port, int key, int16_t value);

int emu_load(struct emu *emu, const char *path);
void emu_open_tray(struct emu *emu);
int emu_close_tray(struct emu *emu, const char *path, int spin_up);
void emu_debug_menu(struct emu *emu);
void emu_render_frame(struct emu *emu);
int emu_begin_frame(struct emu *emu);
void emu_end_frame(struct emu *emu);
void emu_sync(struct emu *emu);

/* expose the 16mb main ram backing store for libretro's memory interface */
uint8_t *emu_ram(struct emu *emu);
size_t emu_ram_size();

/* save states */
size_t emu_state_size(struct emu *emu);
int emu_save_state(struct emu *emu, void *data, size_t size);
int emu_load_state(struct emu *emu, const void *data, size_t size);

#endif
