#ifndef SH4_RTC_H
#define SH4_RTC_H

#include <stdint.h>

struct sh4;

/* save state */
void sh4_rtc_restore_timer(struct sh4 *sh4);

#endif
