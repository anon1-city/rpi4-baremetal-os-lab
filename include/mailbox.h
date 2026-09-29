#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdint.h>

int mailbox_get_board_revision(uint32_t *rev);
int mailbox_get_vc_base(uint32_t *vc_base);

#endif
