#include "hollywood.h"
#include "nand.h"
#include "utils.h"
#include "string.h"
#include "start.h"
#include "memory.h"
#include "crypto.h"
#include "irq.h"
#include "ipc.h"
#include "gecko.h"

//#define	NAND_DEBUG	1
#undef NAND_SUPPORT_WRITE
#undef NAND_SUPPORT_ERASE

#ifdef NAND_DEBUG
#	include "gecko.h"
#	define	NAND_debug(f, arg...) gecko_printf("NAND: " f "\n", ##arg);
#else
#	define	NAND_debug(f, arg...) 
#endif

#define STACK_ALIGN(type, name, cnt, alignment)         \
u8 _al__##name[((sizeof(type)*(cnt)) + (alignment) + \
(((sizeof(type)*(cnt))%(alignment)) > 0 ? ((alignment) - \
((sizeof(type)*(cnt))%(alignment))) : 0))]; \
type *name = (type*)(((u32)(_al__##name)) + ((alignment) - (( \
(u32)(_al__##name))&((alignment)-1))))


#define NAND_RESET 0xff
#define NAND_CHIPID 0x90
#define NAND_GETSTATUS 0x70
#define NAND_ERASE_PRE 0x60
#define NAND_ERASE_POST 0xd0
#define NAND_READ_PRE 0x00
#define NAND_READ_POST 0x30
#define NAND_WRITE_PRE 0x80
#define NAND_WRITE_POST 0x10

#define NAND_BUSY_MASK 0x80000000

#define NAND_FLAGS_IRQ 	0x40000000
#define NAND_FLAGS_WAIT 0x8000
#define NAND_FLAGS_WR	0x4000
#define NAND_FLAGS_RD	0x2000
#define NAND_FLAGS_ECC	0x1000

#define	PAGE_SIZE			2048
#define PAGE_SPARE_SIZE		64
#define NAND_MAX_PAGE		4096

static int ipc_code = 0;
static int ipc_tag = 0;

void nand_irq(void)
{
	int code, tag;
	if (ipc_code != 0) {
		code = ipc_code;
		tag = ipc_tag;
		ipc_code = ipc_tag = 0;
		ipc_post(code, tag, 0);
	}
}

static inline u32 __nand_read32(u32 addr)
{
	return read32(addr);
}

inline void __nand_write32(u32 addr, u32 data)
{
	write32(addr, data);
}

inline void __nand_wait(void) {
	while(__nand_read32(NAND_CMD) & NAND_BUSY_MASK);
}

void nand_send_command(u32 command, u32 bitmask, u32 flags, u32 num_bytes) {
	u32 cmd = NAND_BUSY_MASK | (bitmask << 24) | (command << 16) | flags | num_bytes;
	
	NAND_debug("nand_send_command(%x, %x, %x, %x) -> %x\n",
		command, bitmask, flags, num_bytes, cmd);
			
	__nand_write32(NAND_CMD, 0x7fffffff);
	__nand_write32(NAND_CMD, 0);
	__nand_write32(NAND_CMD, cmd);
}

void __nand_set_address(s32 page_off, s32 pageno) {
	NAND_debug("nand_set_address: %d, %d\n", page_off, pageno);
	if (page_off != -1) __nand_write32(NAND_ADDR0, page_off);	
	if (pageno != -1) __nand_write32(NAND_ADDR1, pageno);
}

void __nand_setup_dma(u8 *data, u8 *spare) {
	NAND_debug("nand_setup_dma: %p, %p\n", data, spare);
	if (((s32)data) != -1) {
		dc_invalidaterange(data, 0x800);
		__nand_write32(NAND_DATA, (s32)data);
	}
	if (((s32)spare) != -1) {
		dc_invalidaterange(spare, 0x50); // +0x10 for calculated syndrome?
		__nand_write32(NAND_ECC, (s32)spare);
	}
}

int nand_reset(void)
{
	NAND_debug("nand_reset()\n");
	nand_send_command(NAND_RESET, 0, NAND_FLAGS_WAIT, 0);
	__nand_wait();
// yay cargo cult
	__nand_write32(NAND_CONF, 0x08000000);
	__nand_write32(NAND_CONF, 0x4b3e0e7f);
	return 0;
}

void nand_get_id(u8 *idbuf) {
	__nand_set_address(0,0);

  	dc_invalidaterange(idbuf, 0x40);

	__nand_setup_dma(idbuf, (u8 *)-1);
	nand_send_command(NAND_CHIPID, 1, NAND_FLAGS_IRQ | NAND_FLAGS_RD, 0x40);
}

void nand_get_status(u8 *status_buf) {
	status_buf[0]=0;
	dc_invalidaterange(status_buf, 0x40);
	__nand_setup_dma(status_buf, (u8 *)-1);
	nand_send_command(NAND_GETSTATUS, 0, NAND_FLAGS_IRQ | NAND_FLAGS_RD, 0x40);
}

void nand_read_page(u32 pageno, void *data, void *ecc) {
  NAND_debug("nand_read_page(%u, %p, %p)\n", pageno, data, ecc);
  __nand_set_address(0, pageno);
  nand_send_command(NAND_READ_PRE, 0x1f, 0, 0);

  dc_invalidaterange(data, 0x800);
  dc_invalidaterange(ecc, 0x50);

  __nand_setup_dma(data, ecc);
  nand_send_command(NAND_READ_POST, 0, NAND_FLAGS_IRQ | NAND_FLAGS_WAIT | NAND_FLAGS_RD | NAND_FLAGS_ECC, 0x840);
}

void nand_wait() {
	__nand_wait();
}

#ifdef NAND_SUPPORT_WRITE
void nand_write_page(u32 pageno, void *data, void *ecc) {
	NAND_debug("nand_write_page(%u, %p, %p)\n", pageno, data, ecc);
	if ((pageno < 0x200) || (pageno >= NAND_PAGE_MAX)) {
	  printf("Error: nand_write to page %d forbidden\n", pageno);
	  return;
	}
	dc_flushrange(data, 0x800);
	dc_flushrange(ecc, 0x40);	
	__nand_set_address(0, pageno);
	__nand_setup_dma(data, ecc);
	nand_send_command(NAND_WRITE_PRE, 0x1f, NAND_FLAGS_IRQ | NAND_FLAGS_WR | NAND_FLAGS_ECC, 0x840);
	
	nand_send_command(NAND_WRITE_POST, 0, NAND_FLAGS_IRQ | NAND_FLAGS_WAIT, 0);
}
#endif

#ifdef NAND_SUPPORT_ERASE
void nand_erase_block(u32 pageno) {
  	NAND_debug("nand_erase_block(%d)\n", pageno);
	if ((pageno < 0x200) || (pageno >= NAND_PAGE_MAX)) {
	  printf("Error: nand_erase to page %d forbidden\n", pageno);
	  return;
	}
  	__nand_set_address(0, pageno);
  	nand_send_command(NAND_ERASE_PRE, 0x1c, 0, 0);
	nand_send_command(NAND_ERASE_POST, 0, NAND_FLAGS_IRQ | NAND_FLAGS_WAIT, 0);
}
#endif

void nand_initialize(void)
{
	ipc_code = ipc_tag = 0;
	nand_reset();
	irq_enable(IRQ_NAND);
}

void nand_ipc(volatile ipc_request *req)
{
	if (ipc_code != 0 || ipc_tag != 0) {
		gecko_printf("NAND: previous IPC request is not done yet.");
		ipc_post(req->code, req->tag, 1, -1);
		return;
	}

	switch (req->req) {
		case IPC_NAND_RESET:
			nand_reset();
			ipc_post(req->code, req->tag, 0);
			break;

		case IPC_NAND_GETID:
			ipc_code = req->code;
			ipc_tag = req->tag;
			nand_get_id((u8 *)req->args[0]);
			break;

		case IPC_NAND_READ:
			ipc_code = req->code;
			ipc_tag = req->tag;
			nand_read_page(req->args[0], (void *)req->args[1],
				       (void *)req->args[2]);
			break;
#ifdef NAND_SUPPORT_WRITE
		case IPC_NAND_WRITE:
			ipc_code = req->code;
			ipc_tag = req->tag;
			nand_write_page(req->args[0], (void *)req->args[1],
				       (void *)req->args[2]);
			break;
#endif
#ifdef NAND_SUPPORT_ERASE
		case IPC_NAND_ERASE:
			ipc_code = req->code;
			ipc_tag = req->tag;
			nand_erase_block(req->args[0]);
			break;
#endif
		default:
			gecko_printf("IPC: unknown SLOW NAND request %04x\n",
					req->req);

	}
}