#include <stdint.h>
#include "gic.h"

#define GICDBASE 0xFF841000UL
#define GICCBASE 0xFF842000UL

#define REGD(x) (*(volatile uint32_t*)(GICDBASE + (x)))
#define REGC(x) (*(volatile uint32_t*)(GICCBASE + (x)))

#define D_CTLR 0x000
#define D_ISENABLER(n)  (0x100 + (n)*4)
#define D_ICPENDR(n)    (0x280 + (n)*4)
#define D_IPRIORITYR(n) (0x400 + (n)*4)
#define D_ITARGETSR(n)  (0x800 + (n)*4)

#define C_CTLR 0x000
#define C_PMR  0x004
#define C_IAR  0x00C
#define C_EOIR 0x010

void gic_init(void){
  REGC(C_CTLR)=0;
  REGC(C_PMR)=0xFF;
  REGD(D_CTLR)=0;
  REGC(C_CTLR)=1;
  REGD(D_CTLR)=1;
}

void gic_enable_irq(unsigned id, unsigned cpu){
  unsigned n=id/4, sh=(id%4)*8;
  uint32_t p=REGD(D_IPRIORITYR(n));
  p &= ~(0xFFu<<sh);
  REGD(D_IPRIORITYR(n))=p;

  if (id >= 32) {
    p=REGD(D_ITARGETSR(n));
    p &= ~(0xFFu<<sh);
    p |= ((1u<<cpu) << sh);
    REGD(D_ITARGETSR(n))=p;
  }

  REGD(D_ICPENDR(id/32)) = 1u<<(id%32);
  REGD(D_ISENABLER(id/32)) = 1u<<(id%32);
}

unsigned gic_ack(void){ return REGC(C_IAR); }
void gic_eoi(unsigned iar){ REGC(C_EOIR)=iar; }
unsigned gic_irq_id(unsigned iar){ return iar & 0x3FFu; }
