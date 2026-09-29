#include <stdint.h>
#include "mailbox.h"

// BCM2711 peripheral base (Raspberry Pi 4)
#define MMIO_BASE       0xFE000000UL
#define MBOX_BASE       (MMIO_BASE + 0x0000B880UL)

#define MBOX_READ       (*(volatile uint32_t *)(MBOX_BASE + 0x00))
#define MBOX_STATUS     (*(volatile uint32_t *)(MBOX_BASE + 0x18))
#define MBOX_WRITE      (*(volatile uint32_t *)(MBOX_BASE + 0x20))

#define MBOX_STATUS_FULL  (1u << 31)
#define MBOX_STATUS_EMPTY (1u << 30)

#define MBOX_CH_PROP      8u
#define MBOX_RESPONSE_OK  0x80000000u

#define TAG_GET_BOARD_REV 0x00010002u
#define TAG_GET_VC_BASE   0x00010006u

static volatile uint32_t mbox[9] __attribute__((aligned(16)));

static int mailbox_call(uint8_t ch) {
  uint32_t addr = (uint32_t)((uintptr_t)mbox);
  uint32_t msg = (addr & ~0xFu) | (uint32_t)(ch & 0xFu);

  while (MBOX_STATUS & MBOX_STATUS_FULL) {}
  MBOX_WRITE = msg;

  for (;;) {
    while (MBOX_STATUS & MBOX_STATUS_EMPTY) {}
    if (MBOX_READ == msg) {
      return (mbox[1] == MBOX_RESPONSE_OK) ? 0 : -1;
    }
  }
}

static int mailbox_get_u32_tag(uint32_t tag, uint32_t *out) {
  mbox[0] = sizeof(mbox);
  mbox[1] = 0;
  mbox[2] = tag;
  mbox[3] = 4; // value buffer size
  mbox[4] = 0; // request
  mbox[5] = 0; // response value
  mbox[6] = 0; // end tag
  mbox[7] = 0;
  mbox[8] = 0;

  if (mailbox_call(MBOX_CH_PROP) != 0) return -1;

  // Bit 31 indicates response length/value present.
  if ((mbox[4] & 0x80000000u) == 0) return -1;
  *out = mbox[5];
  return 0;
}

int mailbox_get_board_revision(uint32_t *rev) {
  return mailbox_get_u32_tag(TAG_GET_BOARD_REV, rev);
}

int mailbox_get_vc_base(uint32_t *vc_base) {
  return mailbox_get_u32_tag(TAG_GET_VC_BASE, vc_base);
}
