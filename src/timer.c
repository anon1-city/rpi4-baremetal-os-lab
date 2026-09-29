#include <stdint.h>
#include "timer.h"

volatile unsigned timer_ticks = 0;
volatile unsigned timer_events = 0;

void timer_reload(unsigned interval_ticks){
  __asm__ volatile("msr cntv_tval_el0, %0" :: "r"((uint64_t)interval_ticks));
  __asm__ volatile("isb");
}

void timer_init(unsigned interval_ticks){
  uint64_t ctl = 0;
  __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(ctl));

  __asm__ volatile("msr cntv_tval_el0, %0" :: "r"((uint64_t)interval_ticks));

  ctl = 1; // ENABLE=1, IMASK=0
  __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(ctl));
  __asm__ volatile("isb");
}
