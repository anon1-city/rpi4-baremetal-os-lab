#include <stdint.h>
#include "uart.h"
#include "mailbox.h"

#define LOAD_ADDR        0x00200000UL   /* kernels are loaded in [LOAD_ADDR, RX_BUF_ADDR) */
#define RX_BUF_ADDR      0x01000000UL   /* the image is buffered here first */
#define HDR_MAGIC        0x4C424B52u  // RKBL little-endian bytes: 52 4B 42 4C
#define MAX_IMAGE_SIZE   (16u * 1024u * 1024u)
#define MAX_LOAD_SIZE    (RX_BUF_ADDR - LOAD_ADDR)
#define RX_TIMEOUT_MS    1000u          /* silence longer than this aborts a transfer */

#define EI_NIDENT 16
#define PT_LOAD   1
#define ET_EXEC   2

typedef void (*kernel_entry_t)(void);

typedef struct {
  unsigned char e_ident[EI_NIDENT];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} Elf64_Phdr;

/* ---- Timeouts (generic timer) ------------------------------------------- */

static uint64_t timer_ticks_now(void) {
  uint64_t v;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
  return v;
}

static uint64_t timer_ticks_per_ms(void) {
  uint64_t f;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
  return f ? f / 1000u : 1u;
}

/* Wait for a byte; -1 if none arrives within timeout_ms. */
static int getc_timeout(uint8_t *out, uint32_t timeout_ms) {
  uint64_t start = timer_ticks_now();
  uint64_t limit = timer_ticks_per_ms() * timeout_ms;

  while (!uart_rx_ready()) {
    if (timer_ticks_now() - start > limit) return -1;
  }
  *out = (uint8_t)uart_getc();
  return 0;
}

static int get_u32_le(uint32_t *out) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    uint8_t b;
    if (getc_timeout(&b, RX_TIMEOUT_MS) != 0) return -1;
    v |= (uint32_t)b << (8 * i);
  }
  *out = v;
  return 0;
}

/* Hunt for the magic in the byte stream, one byte at a time. Stray bytes (a line
 * glitch at power-up, the tail of an aborted upload) are skipped instead of
 * throwing every later header out of step. Waits as long as it takes. */
static void wait_for_magic(void) {
  uint32_t window = 0;
  while (window != HDR_MAGIC)
    window = (window >> 8) | ((uint32_t)(uint8_t)uart_getc() << 24);
}

/* ---- Integrity ------------------------------------------------------------ */

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320): the same value as Python's
 * zlib.crc32(). Bitwise, so it needs no table. */
static uint32_t crc32(const uint8_t *buf, uint32_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < n; ++i) {
    crc ^= buf[i];
    for (int k = 0; k < 8; ++k)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

static void print_hex_label(const char *label, uint32_t val) {
  uart_puts(label);
  uart_puts("0x");
  uart_hex32(val);
  uart_puts("\n");
}

static void mem_copy(uint8_t *dst, const uint8_t *src, uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) dst[i] = src[i];
}

static void mem_zero(uint8_t *dst, uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) dst[i] = 0;
}

/* Code was just written as data: make instruction fetch see it. */
static void sync_code(void) {
  __asm__ volatile("dsb sy\n\tic iallu\n\tdsb sy\n\tisb" ::: "memory");
}

/* ---- ELF loading ---------------------------------------------------------- */

static int is_elf64_aarch64(const uint8_t *buf, uint32_t size) {
  if (size < sizeof(Elf64_Ehdr)) return 0;
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)(const void *)buf;
  return (eh->e_ident[0] == 0x7F && eh->e_ident[1] == 0x45 && eh->e_ident[2] == 0x4C && eh->e_ident[3] == 0x46 &&
          eh->e_ident[4] == 2 && eh->e_ident[5] == 1 && eh->e_machine == 183);
}

static const char *elf_error(int rc) {
  switch (rc) {
  case -1: return "bad program header offset";
  case -2: return "bad program header size";
  case -3: return "program headers outside the file";
  case -4: return "segment file size larger than memory size";
  case -5: return "segment data outside the file";
  case -6: return "not a fixed-address executable (ET_EXEC)";
  case -7: return "no loadable segment";
  case -8: return "segment outside the load area 0x200000-0x1000000 (wrong linker script?)";
  case -9: return "entry point is not inside a loaded segment";
  default: return "unknown";
  }
}

/* Load the PT_LOAD segments at their physical addresses. Every segment is
 * checked before anything is copied, so a bad file never overwrites the
 * bootloader, the receive buffer or device registers, and a partial load never
 * happens. Arithmetic is written so that 64-bit header fields cannot wrap. */
static int load_elf64_image(const uint8_t *img, uint32_t size, uint64_t *entry_out) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)(const void *)img;

  if (eh->e_type != ET_EXEC) return -6;
  if (eh->e_phentsize != sizeof(Elf64_Phdr)) return -2;
  if (eh->e_phnum == 0) return -7;
  if ((eh->e_phoff & 7u) != 0 || eh->e_phoff > size) return -1;
  if ((uint64_t)eh->e_phnum * eh->e_phentsize > size - eh->e_phoff) return -3;

  const Elf64_Phdr *ph = (const Elf64_Phdr *)(const void *)(img + eh->e_phoff);
  int loadable = 0, entry_ok = 0;

  for (uint16_t i = 0; i < eh->e_phnum; ++i) {
    if (ph[i].p_type != PT_LOAD) continue;

    if (ph[i].p_filesz > ph[i].p_memsz) return -4;
    if (ph[i].p_offset > size || ph[i].p_filesz > size - ph[i].p_offset) return -5;
    if (ph[i].p_paddr < LOAD_ADDR || ph[i].p_paddr >= RX_BUF_ADDR ||
        ph[i].p_memsz > RX_BUF_ADDR - ph[i].p_paddr) return -8;

    loadable++;
    if (eh->e_entry >= ph[i].p_paddr && eh->e_entry - ph[i].p_paddr < ph[i].p_memsz)
      entry_ok = 1;
  }
  if (!loadable) return -7;
  if (!entry_ok) return -9;

  for (uint16_t i = 0; i < eh->e_phnum; ++i) {
    if (ph[i].p_type != PT_LOAD) continue;

    uint8_t *dst = (uint8_t *)(uintptr_t)ph[i].p_paddr;
    mem_copy(dst, img + ph[i].p_offset, ph[i].p_filesz);
    mem_zero(dst + ph[i].p_filesz, ph[i].p_memsz - ph[i].p_filesz);
  }

  *entry_out = eh->e_entry;
  return 0;
}

void main(void) {
  uart_init();
  uart_puts("\n=== UART Bootloader (Pi4/QEMU) ===\n");

  uint32_t board_rev = 0, vc_base = 0;
  if (mailbox_get_board_revision(&board_rev) == 0) {
    print_hex_label("Board revision: ", board_rev);
  } else {
    uart_puts("Board revision: mailbox query failed\n");
  }

  if (mailbox_get_vc_base(&vc_base) == 0) {
    print_hex_label("VC core base address: ", vc_base);
  } else {
    uart_puts("VC core base address: mailbox query failed\n");
  }

  uart_puts("\nSend header [magic,u32 size,u32 crc32] then image bytes...\n");

  for (;;) {
    uart_puts("Waiting for transfer...\n");

    wait_for_magic();

    uint32_t size, expect_crc;
    if (get_u32_le(&size) != 0 || get_u32_le(&expect_crc) != 0) {
      uart_puts("ERR timeout in header\n");
      continue;
    }
    if (size == 0 || size > MAX_IMAGE_SIZE) {
      uart_puts("ERR invalid size: "); uart_hex32(size); uart_puts("\n");
      continue;
    }

    uint8_t *rx = (uint8_t *)(uintptr_t)RX_BUF_ADDR;
    uint32_t got = 0;
    while (got < size) {
      uint8_t b;
      if (getc_timeout(&b, RX_TIMEOUT_MS) != 0) break;
      rx[got++] = b;
    }
    if (got < size) {
      uart_puts("ERR timeout after "); uart_hex32(got); uart_puts(" of "); uart_hex32(size);
      uart_puts(" bytes\n");
      continue;
    }

    uint32_t got_crc = crc32(rx, size);
    if (got_crc != expect_crc) {
      uart_puts("ERR checksum mismatch exp="); uart_hex32(expect_crc);
      uart_puts(" got="); uart_hex32(got_crc); uart_puts("\n");
      uart_putc(0x4E);
      continue;
    }

    uint64_t entry = LOAD_ADDR;
    if (is_elf64_aarch64(rx, size)) {
      uart_puts("ELF image detected, loading PT_LOAD segments...\n");
      int rc = load_elf64_image(rx, size, &entry);
      if (rc != 0) {
        uart_puts("ERR ELF: "); uart_puts(elf_error(rc)); uart_puts("\n");
        uart_putc(0x4E);
        continue;
      }
    } else {
      if (size > MAX_LOAD_SIZE) {
        uart_puts("ERR raw image larger than the load area: "); uart_hex32(size); uart_puts("\n");
        uart_putc(0x4E);
        continue;
      }
      uart_puts("RAW image detected, loading at 0x00200000\n");
      mem_copy((uint8_t *)(uintptr_t)LOAD_ADDR, rx, size);
      entry = LOAD_ADDR;
    }
    sync_code();

    // Strong, machine-parsable confirmation for host:
    uart_puts("OK ");
    uart_hex32(size);
    uart_puts(" ");
    uart_hex32((uint32_t)entry);
    uart_puts("\n");

    // Optional: keep single-byte ACK too (some hosts look for it)
    uart_putc('A');

    uart_puts("Kernel received: "); uart_hex32(size); uart_puts(" bytes\n");
    uart_puts("Jumping to loaded entry at "); uart_hex32((uint32_t)entry); uart_puts("\n");

    kernel_entry_t k = (kernel_entry_t)(uintptr_t)entry;
    k();

    uart_puts("Returned from loaded kernel (unexpected), waiting again...\n");
  }
}

void irq_handler_c(void) {
  // Bootloader does not use interrupts in this lab.
}
