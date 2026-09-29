#ifndef TIMER_H
#define TIMER_H
void timer_init(unsigned interval_ticks);
void timer_reload(unsigned interval_ticks);
extern volatile unsigned timer_ticks;
extern volatile unsigned timer_events;
#endif
