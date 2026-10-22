#ifndef SH4_TMU_H
#define SH4_TMU_H

#include <stdint.h>

struct sh4;

void sh4_tmu_debug_menu(struct sh4 *sh4);

/* save state */
void sh4_tmu_restore_timer(struct sh4 *sh4, int n, int active,
                           int64_t remaining);

#endif
