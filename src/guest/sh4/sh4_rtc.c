#include "guest/sh4/sh4.h"
#include "core/time.h"
#include "guest/dreamcast.h"
#include "guest/scheduler.h"

/* RCR2.PES -> period of the periodic interrupt */
static const int64_t RTC_PERIODS_NS[] = {
    0,
    0,
    NS_PER_SEC / 256,
    NS_PER_SEC / 64,
    NS_PER_SEC / 16,
    NS_PER_SEC / 4,
    NS_PER_SEC / 2,
    NS_PER_SEC,
};

static void sh4_rtc_expire(void *data);

static void sh4_rtc_cancel(struct sh4 *sh4) {
  if (sh4->rtc_timer) {
    sched_cancel_timer(sh4->dc->sched, sh4->rtc_timer);
    sh4->rtc_timer = NULL;
  }
}

static void sh4_rtc_reschedule(struct sh4 *sh4) {
  sh4_rtc_cancel(sh4);

  uint32_t rcr2 = *sh4->RCR2;
  int pes = (rcr2 >> 4) & 0x7;

  /* the periodic interrupt only runs while the clock is started and a period
     has been selected */
  if (!(rcr2 & 0x1) || !pes) {
    sh4_clear_interrupt(sh4, SH4_INT_PRI);
    return;
  }

  sh4->rtc_timer = sched_start_timer(sh4->dc->sched, &sh4_rtc_expire, sh4,
                                     RTC_PERIODS_NS[pes]);
}

static void sh4_rtc_expire(void *data) {
  struct sh4 *sh4 = data;

  sh4->rtc_timer = NULL;

  sh4_raise_interrupt(sh4, SH4_INT_PRI);

  sh4_rtc_reschedule(sh4);
}

/* save state helper: the timer isn't serialized (that would change the state
   format), so recreate it from the restored RCR2 after a load */
void sh4_rtc_restore_timer(struct sh4 *sh4) {
  sh4_rtc_reschedule(sh4);
}

REG_W32(sh4_cb, RCR2) {
  struct sh4 *sh4 = dc->sh4;

  *sh4->RCR2 = value;
  sh4_rtc_reschedule(sh4);
}
