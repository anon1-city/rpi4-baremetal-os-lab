#ifndef GIC_H
#define GIC_H
void gic_init(void);
void gic_enable_irq(unsigned id, unsigned cpu);
unsigned gic_ack(void);
void gic_eoi(unsigned iar);
unsigned gic_irq_id(unsigned iar);
#endif
