/*
 * The touch screen of the POCO X3 NFC (surya): a Novatek NT36672C on SPI, as a source of touches
 * for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg touchscreen=1.
 *
 * Where the numbers come from: the surya device tree and Xiaomi's surya kernel sources
 * (drivers/input/touchscreen/nt36xxx: nt36xxx.c, nt36xxx_fw_update.c, nt36xxx_mem_map.h), by way
 * of the ios-sweet port (yavuzhanozyer/ios-sweet, xnu/patches/0082-surya-nvt-touch.patch), whose
 * driver this is, moved onto HakiOS's queue, log and firmware image:
 *   SPI  880000.spi = QUPv3 wrapper 0, serial engine 0, pins GPIO49-52 (WEST tile, function
 *        qup00 = mux 2), mode 0, 8 bits, at most 8 MHz.
 *   GCC  vote 0x5200c bits 6 (m_ahb) 7 (s_ahb) 8 (core) 9 (core_2x) 10 (s0); SE0's clock 0x17034,
 *        its branches 0x17004/08/0c/14/30.
 *   IC   reset GPIO8, interrupt GPIO9 (NORTH tile). Flashless: the firmware is sent over SPI after
 *        every reset (the HW-CRC path of nt36xxx_fw_update.c); SWRST_N8 0x3F0FE, SPI_RD_FAST 0x3F310.
 *   The controller is in the panel's driver chip (TDDI) and powered with the panel, which UEFI has
 *        left on: no supply of its own is switched here.
 *
 * The firmware is the vendor's and not in the repository: the phone's /vendor/firmware/
 * novatek_ts_tianma_fw.bin or novatek_ts_huaxing_fw.bin, whichever panel the phone has, pulled by
 * devices/surya/pull-firmware.sh into fw-touch/, which userland/build.sh puts into the image as
 * fw/novatek_ts_fw.bin.
 *
 * Boot-arguments: touchscreen=1 starts it; touch_delay=<s> waits that long first (default 2);
 * touch_nvt_div=<n> divides 19.2 MHz for the SPI clock (default 4: 4.8 MHz); touch_nvt_fw=0 skips
 * the firmware download (to see only whether the chip answers).
 *
 * Status: the ios-sweet driver this is made of is reported working on this phone; here it is
 * written for HakiOS and compiled for this device, not yet run on the phone.
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/startup.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

/* every line to the loader's log as well, as hakios.buttons.c does: "xl log" shows them during a run */
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

#define NVT_FW_DIR       "fw"
#define NVT_FW_NAME      "novatek_ts_fw.bin"
#define NVT_PANEL_W      1080u
#define NVT_PANEL_H      2400u

#define NVT_SE_PA        0x00880000u
#define NVT_GCC_PA       0x00100000u
#define NVT_TLMM_N_PA    0x03900000u
#define NVT_TLMM_W_PA    0x03500000u
#define NVT_GCC_VOTE     0x5200cu
#define NVT_WRAP0_MASK   0x000007c0u
#define NVT_RCG          0x17034u
#define NVT_GPIO_CFG(n)  ((uint32_t)(n) * 0x1000u)
#define NVT_GPIO_IO(n)   (NVT_GPIO_CFG(n) + 4u)
#define NVT_PIN_RST      8u
#define NVT_PIN_IRQ      9u

/* GENI common */
#define SE_FORCE_DEFAULT 0x020u
#define SE_OUTPUT_CTRL   0x024u
#define SE_CGC_CTRL      0x028u
#define SE_STATUS        0x040u
#define SE_MCLK          0x048u
#define SE_FW            0x068u
#define SE_CLK_SEL       0x07cu
#define SE_BYTE_GRAN     0x254u
#define SE_DMA           0x258u
#define SE_TX_PACK0      0x260u
#define SE_TX_PACK1      0x264u
#define SE_RX_PACK0      0x284u
#define SE_RX_PACK1      0x288u
#define SE_CMD           0x600u
#define SE_CTRL          0x604u
#define SE_IRQ           0x610u
#define SE_IRQ_EN        0x614u
#define SE_IRQ_CLR       0x618u
#define SE_S_IRQ_EN      0x644u
#define SE_S_IRQ_CLR     0x648u
#define SE_TX            0x700u
#define SE_RX            0x780u
#define SE_TX_COUNT      0x800u
#define SE_RX_COUNT      0x804u
#define SE_TX_WM         0x80cu
#define SE_RX_WM         0x810u
#define SE_RX_RFR        0x814u
#define SE_GSI_EVENT_EN  0xe18u
#define SE_IRQ_ROUTE     0xe1cu
#define SE_HW_TX         0xe24u
#define SE_HW_RX         0xe28u
#define SE_DMA_GENERAL   0xe30u
/* GENI SPI */
#define SPI_CPHA         0x224u
#define SPI_LOOPBACK     0x22cu
#define SPI_CPOL         0x230u
#define SPI_DEMUX_INV    0x24cu
#define SPI_DEMUX_SEL    0x250u
#define SPI_TRANS_CFG    0x25cu
#define SPI_WORD_LEN     0x268u
#define SPI_TX_LEN       0x26cu
#define SPI_RX_LEN       0x270u
#define SPI_PRE_POST     0x274u
#define SPI_DELAYS       0x278u
#define SPI_OP_TX        1u
#define SPI_OP_RX        2u

#define SE_DONE          (1u << 0)
#define SE_ERRORS        (0x0eu | (3u << 24) | (3u << 28))
#define SE_WM_BITS       ((1u << 26) | (1u << 27) | (1u << 30))
/* geni_se_config_packing(8, 4, msb_to_lsb): FIFO bytes LSB first. */
#define SE_PACK0         0x0007f8feu
#define SE_PACK1         0x000ffefeu

/* Novatek */
#define NVT_ENG_RST      0x7FFF80u
#define NVT_SWRST_N8     0x3F0FEu
#define NVT_SPI_RD_FAST  0x3F310u
#define NVT_TRIM_ADDR    0x1F64Eu
#define EV_HOST_CMD      0x50u
#define EV_RESET_DONE    0x60u
#define EV_FWINFO        0x78u
#define NVT_CHUNK        4096u
#define NVT_MAX_PART     24u
#define NVT_POINT_LEN    65u
#define TOUCH_MAX        10u

struct nvt_mmap {
	const char *name;
	uint8_t hw_crc;
	uint32_t event_buf, boot_rdy, bld_crc_en;
	uint32_t ilm_len, dlm_len, ilm_des, dlm_des, g_ilm, g_dlm;
};

static const struct nvt_mmap nvt_675 = {
	"NT36675-map", 2, 0x22D00, 0x3F10D, 0x3F30E,
	0x3F118, 0x3F130, 0x3F128, 0x3F12C, 0x3F100, 0x3F104,
};
static const struct nvt_mmap nvt_672a = {
	"NT36672A-map", 1, 0x21C00, 0x3F10D, 0x3F30E,
	0x3F118, 0x3F130, 0x3F128, 0x3F12C, 0x3F100, 0x3F104,
};

/* Only HW-CRC chips of nt36xxx_mem_map.h; others are reported, not driven. */
static const struct {
	uint8_t id[6], mask[6];
	const struct nvt_mmap *map;
} nvt_trim[] = {
	{{0x0C, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, &nvt_675},
	{{0xFF, 0xFF, 0xFF, 0x26, 0x65, 0x03}, {0, 0, 0, 1, 1, 1}, &nvt_675},
	{{0xFF, 0xFF, 0xFF, 0x75, 0x66, 0x03}, {0, 0, 0, 1, 1, 1}, &nvt_675},
	{{0x0B, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, &nvt_672a},
	{{0x0B, 0xFF, 0xFF, 0x82, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, &nvt_672a},
	{{0x0B, 0xFF, 0xFF, 0x25, 0x65, 0x03}, {1, 0, 0, 1, 1, 1}, &nvt_672a},
	{{0x0A, 0xFF, 0xFF, 0x72, 0x65, 0x03}, {1, 0, 0, 1, 1, 1}, &nvt_672a},
	{{0x0A, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, &nvt_672a},
	{{0x0A, 0xFF, 0xFF, 0x82, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, &nvt_672a},
	{{0x0A, 0xFF, 0xFF, 0x70, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, &nvt_672a},
	{{0x0B, 0xFF, 0xFF, 0x70, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, &nvt_672a},
	{{0x0A, 0xFF, 0xFF, 0x72, 0x67, 0x03}, {1, 0, 0, 1, 1, 1}, &nvt_672a},
};

struct nvt_part {
	uint32_t sram, size, bin, crc;
};

static struct {
	vm_offset_t se, gcc, tn, tw;
	unsigned depth;
	bool started, dead;
	const struct nvt_mmap *map;
	uint8_t rx[96];
	uint32_t active;
	int x[TOUCH_MAX], y[TOUCH_MAX];
	const uint8_t *fw;
	uint32_t fw_size;
	struct nvt_part part[NVT_MAX_PART];
	unsigned nparts;
} nv;
static int nvt_sleep_chan;

static uint32_t
nvt_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
nvt_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
nvt_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&nvt_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static bool
nvt_wait_clear(vm_offset_t base, uint32_t off, uint32_t mask)
{
	for (unsigned i = 0; i < 5000; ++i) {
		if (!(nvt_rd(base, off) & mask)) {
			return true;
		}
		delay(1);
	}
	return false;
}

static uint32_t
nvt_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* pull: 0 none 1 down 3 up; drive code: mA/2 - 1. */
static void
nvt_gpio(vm_offset_t tile, unsigned pin, unsigned func, unsigned pull,
    unsigned drive, bool out, bool high)
{
	uint32_t cfg = nvt_rd(tile, NVT_GPIO_CFG(pin));
	if (out) {
		nvt_wr(tile, NVT_GPIO_IO(pin),
		    (nvt_rd(tile, NVT_GPIO_IO(pin)) & ~2u) | (high ? 2u : 0u));
	}
	cfg &= ~0x3ffu;
	cfg |= (func << 2) | pull | (drive << 6) | (out ? 0x200u : 0u);
	nvt_wr(tile, NVT_GPIO_CFG(pin), cfg);
}

static bool
nvt_clocks(void)
{
	nvt_wr(nv.gcc, NVT_GCC_VOTE, nvt_rd(nv.gcc, NVT_GCC_VOTE) | NVT_WRAP0_MASK);
	uint32_t dfs = nvt_rd(nv.gcc, NVT_RCG + 0x14u);
	uint32_t cfg = nvt_rd(nv.gcc, NVT_RCG + 4u);
	printf("nt36672c: gcc vote=%#x rcg=%#x cfg=%#x dfs=%#x\n",
	    nvt_rd(nv.gcc, NVT_GCC_VOTE), nvt_rd(nv.gcc, NVT_RCG), cfg, dfs);
	if (dfs & 1u) {
		cfg = nvt_rd(nv.gcc, NVT_RCG + 0x1cu);
		if ((cfg & 0x371fu) != 1u) {
			printf("nt36672c: DFS level0=%#x is not XO/1; stopping\n", cfg);
			return false;
		}
	} else {
		/* XO parent 0, divider 1 => 19.2 MHz, as the ios-sweet port did for its SE7. */
		cfg = (cfg & ~(0x371fu | (1u << 20))) | 1u;
		nvt_wr(nv.gcc, NVT_RCG + 4u, cfg);
		nvt_wr(nv.gcc, NVT_RCG, nvt_rd(nv.gcc, NVT_RCG) | 1u);
		if (!nvt_wait_clear(nv.gcc, NVT_RCG, 1u)) {
			printf("nt36672c: RCG update timeout\n");
			return false;
		}
	}
	const uint32_t halts[] = {0x17004, 0x17008, 0x1700c, 0x17014, 0x17030};
	for (unsigned i = 0; i < sizeof(halts) / sizeof(halts[0]); ++i) {
		if (!nvt_wait_clear(nv.gcc, halts[i], 1u << 31)) {
			printf("nt36672c: GCC branch %#x still off (%#x)\n", halts[i],
			    nvt_rd(nv.gcc, halts[i]));
			return false;
		}
	}
	return true;
}

static bool
nvt_se_init(void)
{
	unsigned div = 4;
	(void)PE_parse_boot_argn("touch_nvt_div", &div, sizeof(div));
	if (div < 3 || div > 64) {
		div = 4;
	}
	uint32_t fw = nvt_rd(nv.se, SE_FW);
	uint32_t tx = nvt_rd(nv.se, SE_HW_TX), rx = nvt_rd(nv.se, SE_HW_RX);
	printf("nt36672c: se0 fw=%#x proto=%u status=%#x tx=%#x rx=%#x trans_cfg=%#x\n",
	    fw, (fw >> 8) & 0xffu, nvt_rd(nv.se, SE_STATUS), tx, rx,
	    nvt_rd(nv.se, SPI_TRANS_CFG));
	if (((fw >> 8) & 0xffu) != 1u || ((tx >> 24) & 0x3fu) != 32u ||
	    ((tx >> 16) & 0x3fu) < 4u || ((rx >> 16) & 0x3fu) < 4u) {
		printf("nt36672c: SE0 has no SPI firmware or unexpected FIFOs; stopping\n");
		return false;
	}
	if (nvt_rd(nv.se, SE_STATUS) & 0x1001u) {
		printf("nt36672c: SE0 busy; stopping\n");
		return false;
	}
	nv.depth = (tx >> 16) & 0x3fu;
	nvt_wr(nv.se, SE_GSI_EVENT_EN, 0);
	nvt_wr(nv.se, SE_IRQ_ROUTE, 0); /* poll local status only */
	nvt_wr(nv.se, SE_S_IRQ_EN, 0);
	nvt_wr(nv.se, SE_IRQ_CLR, 0xffffffffu);
	nvt_wr(nv.se, SE_S_IRQ_CLR, 0xffffffffu);
	nvt_wr(nv.se, SE_DMA, 0);
	nvt_wr(nv.se, SE_CGC_CTRL, nvt_rd(nv.se, SE_CGC_CTRL) | 0x7fu);
	nvt_wr(nv.se, SE_DMA_GENERAL, nvt_rd(nv.se, SE_DMA_GENERAL) | 0x0fu);
	nvt_wr(nv.se, SE_OUTPUT_CTRL, 0x7f);
	nvt_wr(nv.se, SE_FORCE_DEFAULT, 1);
	nvt_wr(nv.se, SE_CLK_SEL, 0);
	nvt_wr(nv.se, SE_MCLK, (div << 4) | 1u);
	nvt_wr(nv.se, SPI_LOOPBACK, 0);
	nvt_wr(nv.se, SPI_CPHA, 0);
	nvt_wr(nv.se, SPI_CPOL, 0);
	nvt_wr(nv.se, SPI_DEMUX_INV, 0);
	nvt_wr(nv.se, SPI_DEMUX_SEL, 0);
	nvt_wr(nv.se, SPI_WORD_LEN, 8u - 4u);
	nvt_wr(nv.se, SPI_PRE_POST, 0);
	nvt_wr(nv.se, SPI_DELAYS, 0);
	nvt_wr(nv.se, SE_BYTE_GRAN, 0);
	nvt_wr(nv.se, SE_TX_PACK0, SE_PACK0);
	nvt_wr(nv.se, SE_TX_PACK1, SE_PACK1);
	nvt_wr(nv.se, SE_RX_PACK0, SE_PACK0);
	nvt_wr(nv.se, SE_RX_PACK1, SE_PACK1);
	nvt_wr(nv.se, SE_TX_WM, 1);
	nvt_wr(nv.se, SE_RX_WM, 0);
	nvt_wr(nv.se, SE_RX_RFR, ((rx >> 16) & 0x3fu) - 2u);
	nvt_wr(nv.se, SE_IRQ_EN, SE_DONE | SE_ERRORS | SE_WM_BITS | (1u << 5));
	if ((nvt_rd(nv.se, SE_TX_COUNT) & 0xfffffffu) ||
	    (nvt_rd(nv.se, SE_RX_COUNT) & 0x1ffffffu)) {
		printf("nt36672c: inherited FIFO data; stopping\n");
		return false;
	}
	printf("nt36672c: SPI ready, depth=%u words, sclk=19.2MHz/%u\n", nv.depth, div);
	return true;
}

static void
nvt_abort(void)
{
	nvt_wr(nv.se, SE_CTRL, 2);
	for (unsigned i = 0; i < 5000 && !(nvt_rd(nv.se, SE_IRQ) & (1u << 5)); ++i) {
		delay(1);
	}
	for (unsigned i = 0; i < 64 && (nvt_rd(nv.se, SE_RX_COUNT) & 0x1ffffffu); ++i) {
		(void)nvt_rd(nv.se, SE_RX);
	}
	nvt_wr(nv.se, SE_IRQ_CLR, 0xffffffffu);
	if (!nvt_wait_clear(nv.se, SE_STATUS, 1u)) {
		nv.dead = true;
		printf("nt36672c: abort failed status=%#x; stopping\n", nvt_rd(nv.se, SE_STATUS));
	}
}

/*
 * One chip-select framed transfer of n bytes. TX byte 0 is hdr, bytes
 * 1..plen come from payload, the rest are zero. If rx is set the transfer is
 * full duplex and up to rx_cap received bytes are stored.
 */
static int
nvt_xfer(uint8_t hdr, const uint8_t *payload, uint32_t plen, uint8_t *rx,
    uint32_t rx_cap, uint32_t n)
{
	if (nv.dead || !n || n > 0xffffffu) {
		return EIO;
	}
	if (!nvt_wait_clear(nv.se, SE_STATUS, 1u)) {
		nv.dead = true;
		return EBUSY;
	}
	uint32_t op = SPI_OP_TX | (rx ? SPI_OP_RX : 0u);
	nvt_wr(nv.se, SE_IRQ_CLR, 0xffffffffu);
	nvt_wr(nv.se, SPI_TX_LEN, n);
	nvt_wr(nv.se, SPI_RX_LEN, rx ? n : 0u);
	nvt_wr(nv.se, SE_CMD, op << 27);
	uint32_t sent = 0, got = 0;
	unsigned idle = 0;
	while (idle < 20000) {
		bool progress = false;
		uint32_t irq = nvt_rd(nv.se, SE_IRQ);
		if (irq & SE_ERRORS) {
			printf("nt36672c: spi error irq=%#x sent=%u got=%u n=%u\n", irq, sent, got, n);
			nvt_abort();
			return EIO;
		}
		while (sent < n && (nvt_rd(nv.se, SE_TX_COUNT) & 0xfffffffu) + 1u < nv.depth) {
			uint32_t word = 0;
			for (unsigned j = 0; j < 4 && sent + j < n; ++j) {
				uint32_t i = sent + j;
				uint8_t b = i == 0 ? hdr : (i <= plen ? payload[i - 1] : 0u);
				word |= (uint32_t)b << (8u * j);
			}
			nvt_wr(nv.se, SE_TX, word);
			sent += 4;
			progress = true;
		}
		if (rx) {
			uint32_t count = nvt_rd(nv.se, SE_RX_COUNT) & 0x1ffffffu;
			if (count > 64) {
				nvt_abort();
				return EIO;
			}
			while (count--) {
				uint32_t word = nvt_rd(nv.se, SE_RX);
				for (unsigned j = 0; j < 4 && got < n; ++j, ++got) {
					if (got < rx_cap) {
						rx[got] = (uint8_t)(word >> (8u * j));
					}
				}
				progress = true;
			}
		}
		if (irq & SE_DONE) {
			if (rx) {
				uint32_t count = nvt_rd(nv.se, SE_RX_COUNT) & 0x1ffffffu;
				while (count-- && got < n) {
					uint32_t word = nvt_rd(nv.se, SE_RX);
					for (unsigned j = 0; j < 4 && got < n; ++j, ++got) {
						if (got < rx_cap) {
							rx[got] = (uint8_t)(word >> (8u * j));
						}
					}
				}
			}
			nvt_wr(nv.se, SE_IRQ_CLR, irq);
			if (sent >= n && (!rx || got >= n)) {
				return 0;
			}
			printf("nt36672c: short transfer sent=%u got=%u n=%u\n", sent, got, n);
			return EIO;
		}
		nvt_wr(nv.se, SE_IRQ_CLR, irq & SE_WM_BITS);
		if (progress) {
			idle = 0;
		} else {
			++idle;
			delay(1);
		}
	}
	printf("nt36672c: spi timeout sent=%u got=%u n=%u irq=%#x\n", sent, got, n,
	    nvt_rd(nv.se, SE_IRQ));
	nvt_abort();
	return ETIMEDOUT;
}

/* CTP_SPI_WRITE: buf[0] | 0x80, then data. */
static int
nvt_write(uint8_t addr, const uint8_t *data, uint32_t len)
{
	return nvt_xfer((uint8_t)(addr | 0x80u), data, len, NULL, 0, len + 1u);
}

/* CTP_SPI_READ: addr & 0x7f, one dummy byte, data starts at rx[2]. */
static int
nvt_read(uint8_t addr, uint8_t *out, uint32_t len)
{
	if (len + 2u > sizeof(nv.rx)) {
		return EINVAL;
	}
	int err = nvt_xfer((uint8_t)(addr & 0x7fu), NULL, 0, nv.rx, sizeof(nv.rx), len + 2u);
	if (!err) {
		memcpy(out, nv.rx + 2, len);
	}
	return err;
}

static int
nvt_set_page(uint32_t addr)
{
	const uint8_t b[2] = {(uint8_t)(addr >> 15), (uint8_t)(addr >> 7)};
	return nvt_write(0xFF, b, 2);
}

static int
nvt_write_addr(uint32_t addr, uint8_t value)
{
	int err = nvt_set_page(addr);
	return err ? err : nvt_write((uint8_t)(addr & 0x7fu), &value, 1);
}

static int
nvt_read_addr(uint32_t addr, uint8_t *out, uint32_t len)
{
	int err = nvt_set_page(addr);
	return err ? err : nvt_read((uint8_t)(addr & 0x7fu), out, len);
}

static void
nvt_bootloader_reset(void)
{
	(void)nvt_write_addr(NVT_SWRST_N8, 0x69);
	delay(5000);
	(void)nvt_write_addr(NVT_SPI_RD_FAST, 0x00);
}

static bool
nvt_identify(void)
{
	uint8_t id[6] = {0};
	for (unsigned retry = 0; retry < 5; ++retry) {
		nvt_bootloader_reset();
		int err = nvt_read_addr(NVT_TRIM_ADDR, id, sizeof(id));
		printf("nt36672c: trim err=%d id=%02x %02x %02x %02x %02x %02x\n",
		    err, id[0], id[1], id[2], id[3], id[4], id[5]);
		for (unsigned t = 0; !err && t < sizeof(nvt_trim) / sizeof(nvt_trim[0]); ++t) {
			unsigned i = 0;
			for (; i < 6; ++i) {
				if (nvt_trim[t].mask[i] && id[i] != nvt_trim[t].id[i]) {
					break;
				}
			}
			if (i == 6) {
				nv.map = nvt_trim[t].map;
				printf("nt36672c: chip matched %s hw_crc=%u\n", nv.map->name, nv.map->hw_crc);
				return true;
			}
		}
		nvt_wait(10);
	}
	return false;
}

/* update_firmware_request + nvt_bin_header_parser (HW-CRC variant). */
static bool
nvt_parse_fw(void)
{
	const uint8_t *d = nv.fw;
	uint32_t need = 0;
	for (uint32_t i = nv.fw_size / 4096u; i > 0; --i) {
		const uint8_t *flag = d + i * 4096u - 3u;
		if ((flag[0] == 'N' && flag[1] == 'V' && flag[2] == 'T') ||
		    (flag[0] == 'M' && flag[1] == 'O' && flag[2] == 'D')) {
			need = i * 4096u;
			break;
		}
	}
	if (need < 8192u) {
		printf("nt36672c: firmware end flag not found\n");
		return false;
	}
	if ((uint8_t)(d[need - 4096u] + d[need - 4095u]) != 0xFF) {
		printf("nt36672c: FW_VER %02x + bar %02x != 0xFF\n", d[need - 4096u], d[need - 4095u]);
		return false;
	}
	uint32_t end = nvt_le32(d), info = 0;
	for (uint32_t pos = 0x30; pos < end && pos < nv.fw_size; pos += 0x10) {
		++info;
	}
	uint32_t ovly = (d[0x28] & 0x10u) ? (d[0x28] & 0x0fu) : 0u;
	nv.nparts = 2u + ovly + info;
	if (nv.nparts > NVT_MAX_PART) {
		printf("nt36672c: %u partitions, too many\n", nv.nparts);
		return false;
	}
	for (uint32_t l = 0; l < nv.nparts; ++l) {
		struct nvt_part *p = &nv.part[l];
		uint32_t pos;
		if (l < 2) {
			p->bin = nvt_le32(d + 12u * l);
			p->sram = nvt_le32(d + 4u + 12u * l);
			p->size = nvt_le32(d + 8u + 12u * l);
			p->crc = nvt_le32(d + 0x18u + 4u * l);
			continue;
		}
		pos = (l < 2u + info) ? 0x30u + 0x10u * (l - 2u)
		    : nv.part[1].bin + 0x10u * (l - 2u - info);
		if (pos + 16u > nv.fw_size) {
			printf("nt36672c: header %u out of range\n", l);
			return false;
		}
		p->sram = nvt_le32(d + pos);
		p->size = nvt_le32(d + pos + 4u);
		p->bin = nvt_le32(d + pos + 8u);
		p->crc = nvt_le32(d + pos + 12u);
	}
	for (uint32_t l = 0; l < nv.nparts; ++l) {
		struct nvt_part *p = &nv.part[l];
		if ((uint64_t)p->bin + p->size > nv.fw_size) {
			printf("nt36672c: partition %u exceeds file\n", l);
			return false;
		}
		printf("nt36672c: part %u sram=%#x size=%#x bin=%#x crc=%#x\n",
		    l, p->sram, p->size, p->bin, p->crc);
	}
	printf("nt36672c: firmware ver=%#x need=%#x partitions=%u\n",
	    d[need - 4096u], need, nv.nparts);
	return true;
}

static int
nvt_write_sram(uint32_t sram, uint32_t bin, uint32_t size)
{
	while (size) {
		uint32_t len = size < NVT_CHUNK ? size : NVT_CHUNK;
		uint32_t avail = bin < nv.fw_size ? nv.fw_size - bin : 0u;
		int err = nvt_set_page(sram);
		if (!err) {
			/* The vendor writes size + 1 bytes; never read past the file. */
			err = nvt_xfer((uint8_t)((sram & 0x7fu) | 0x80u), nv.fw + bin,
			    len < avail ? len : avail, NULL, 0, len + 1u);
		}
		if (err) {
			return err;
		}
		sram += len;
		bin += len;
		size -= len;
	}
	return 0;
}

static void
nvt_crc_bank(uint32_t des, uint32_t sram, uint32_t len_addr, uint32_t size,
    uint32_t g_addr, uint32_t crc)
{
	uint8_t b[4];
	(void)nvt_set_page(des);
	b[0] = (uint8_t)sram; b[1] = (uint8_t)(sram >> 8); b[2] = (uint8_t)(sram >> 16);
	(void)nvt_write((uint8_t)(des & 0x7fu), b, 3);
	b[0] = (uint8_t)size; b[1] = (uint8_t)(size >> 8); b[2] = (uint8_t)((size >> 16) & 1u);
	(void)nvt_write((uint8_t)(len_addr & 0x7fu), b, nv.map->hw_crc == 1 ? 2u : 3u);
	b[0] = (uint8_t)crc; b[1] = (uint8_t)(crc >> 8); b[2] = (uint8_t)(crc >> 16);
	b[3] = (uint8_t)(crc >> 24);
	(void)nvt_write((uint8_t)(g_addr & 0x7fu), b, 4);
}

static bool
nvt_reset_state_ok(void)
{
	uint8_t b[5] = {0};
	(void)nvt_set_page(nv.map->event_buf | EV_RESET_DONE);
	for (unsigned retry = 0; retry <= 10; ++retry) {
		int err = nvt_read(EV_RESET_DONE, b, sizeof(b));
		if (!err && b[0] >= 0xA0u && b[0] <= 0xAFu) {
			printf("nt36672c: reset state %#x\n", b[0]);
			return true;
		}
		nvt_wait(10);
	}
	printf("nt36672c: reset state stuck at %02x %02x %02x %02x %02x\n",
	    b[0], b[1], b[2], b[3], b[4]);
	return false;
}

/* nvt_download_firmware_hw_crc */
static bool
nvt_download(void)
{
	for (unsigned retry = 0; retry < 3; ++retry) {
		nvt_bootloader_reset();
		int err = 0;
		for (uint32_t l = 0; !err && l < nv.nparts; ++l) {
			if (nv.part[l].size) {
				err = nvt_write_sram(nv.part[l].sram, nv.part[l].bin, nv.part[l].size + 1u);
			}
		}
		if (err) {
			printf("nt36672c: SRAM write failed %d (try %u)\n", err, retry);
			continue;
		}
		nvt_crc_bank(nv.map->ilm_des, nv.part[0].sram, nv.map->ilm_len, nv.part[0].size,
		    nv.map->g_ilm, nv.part[0].crc);
		nvt_crc_bank(nv.map->dlm_des, nv.part[1].sram, nv.map->dlm_len, nv.part[1].size,
		    nv.map->g_dlm, nv.part[1].crc);
		uint8_t v = 0;
		(void)nvt_read_addr(nv.map->bld_crc_en, &v, 1);
		v |= 0x80u;
		(void)nvt_write((uint8_t)(nv.map->bld_crc_en & 0x7fu), &v, 1);
		(void)nvt_set_page(nv.map->event_buf);
		v = 0x00;
		(void)nvt_write(EV_RESET_DONE, &v, 1);
		v = 0xAE;
		(void)nvt_write(EV_HOST_CMD, &v, 1);
		(void)nvt_write_addr(nv.map->boot_rdy, 1);
		nvt_wait(5);
		if (nvt_reset_state_ok()) {
			return true;
		}
		printf("nt36672c: firmware did not start (try %u)\n", retry);
	}
	return false;
}

static void
nvt_fw_info(void)
{
	uint8_t b[16] = {0};
	int err = nvt_read_addr(nv.map->event_buf | EV_FWINFO, b, sizeof(b));
	printf("nt36672c: fwinfo err=%d ver=%#x/%#x x_num=%u y_num=%u max=%ux%u\n",
	    err, b[0], b[1], b[2], b[3], ((unsigned)b[4] << 8) | b[5],
	    ((unsigned)b[6] << 8) | b[7]);
}

static bool
nvt_load_file(void)
{
	uint64_t size = 0;
	const uint8_t *data = NULL;

	/* the image may not be there yet this early: look for a minute */
	for (unsigned tries = 0; tries < 60 && data == NULL; tries++) {
		data = hakios_image_file(NVT_FW_DIR, NVT_FW_NAME, &size, NULL);
		if (data == NULL) {
			nvt_wait(1000);
		}
	}
	if (data == NULL) {
		printf("nt36672c: no %s/%s in the image: put the phone's /vendor/firmware/novatek_ts_*_fw.bin into "
		    "fw-touch/ (devices/surya/pull-firmware.sh) and run userland/build.sh\n", NVT_FW_DIR, NVT_FW_NAME);
		return false;
	}
	if (size < 0x10000 || size > 0x200000) {
		printf("nt36672c: the firmware is %llu bytes, which is not one of these: refused\n", size);
		return false;
	}
	nv.fw = data;
	nv.fw_size = (uint32_t)size;
	printf("nt36672c: firmware %s/%s, %u bytes\n", NVT_FW_DIR, NVT_FW_NAME, nv.fw_size);
	return true;
}

/* 0..1079 x 0..2399 on the panel to 0..65535 on the display, as the queue wants it */
static uint32_t
nvt_scale(int v, uint32_t size)
{
	return (uint32_t)(((uint64_t)(uint32_t)v * 65535u) / (size - 1u));
}

static void
nvt_report(uint32_t mask, const int *x, const int *y)
{
	for (unsigned id = 0; id < TOUCH_MAX; ++id) {
		uint32_t bit = 1u << id;
		if (mask & bit) {
			if (!(nv.active & bit)) {
				hakios_touch_post(HAKIOS_TOUCH_DOWN, id, nvt_scale(x[id], NVT_PANEL_W), nvt_scale(y[id], NVT_PANEL_H));
			} else if (x[id] != nv.x[id] || y[id] != nv.y[id]) {
				hakios_touch_post(HAKIOS_TOUCH_MOVE, id, nvt_scale(x[id], NVT_PANEL_W), nvt_scale(y[id], NVT_PANEL_H));
			}
			nv.x[id] = x[id];
			nv.y[id] = y[id];
		} else if (nv.active & bit) {
			hakios_touch_post(HAKIOS_TOUCH_UP, id, nvt_scale(nv.x[id], NVT_PANEL_W), nvt_scale(nv.y[id], NVT_PANEL_H));
		}
	}
	nv.active = mask;
}

/* nvt_ts_work_func, protocol B, no palm/gesture handling. */
static int
nvt_poll(unsigned *wdt)
{
	uint8_t d[NVT_POINT_LEN];
	int x[TOUCH_MAX] = {0}, y[TOUCH_MAX] = {0};
	uint32_t mask = 0;
	int err = nvt_read(0x00, d, sizeof(d));
	if (err) {
		return err;
	}
	unsigned fd = 0;
	for (unsigned i = 0; i < 6; ++i) {
		fd += (d[i] == 0xFD || d[i] == 0xFE);
	}
	*wdt = fd == 6 ? *wdt + 1u : 0u;
	if (fd == 6) {
		return 0;
	}
	for (unsigned i = 0; i < TOUCH_MAX; ++i) {
		const uint8_t *p = d + 6u * i;
		unsigned id = p[0] >> 3, status = p[0] & 7u;
		if (id == 0 || id > TOUCH_MAX || (status != 1u && status != 2u)) {
			continue;
		}
		unsigned px = ((unsigned)p[1] << 4) | (p[3] >> 4);
		unsigned py = ((unsigned)p[2] << 4) | (p[3] & 15u);
		if (px >= NVT_PANEL_W || py >= NVT_PANEL_H) {
			continue;
		}
		mask |= 1u << (id - 1u);
		x[id - 1u] = (int)px;
		y[id - 1u] = (int)py;
	}
	nvt_report(mask, x, y);
	return 0;
}

static bool
nvt_bring_up(void)
{
	int use_fw = 1;
	(void)PE_parse_boot_argn("touch_nvt_fw", &use_fw, sizeof(use_fw));
	/* Vendor probe: TP_RESX low, eng reset over SPI, RESX high, 10 ms. */
	nvt_gpio(nv.tn, NVT_PIN_RST, 0, 0, 1, true, false);
	nvt_wait(5);
	(void)nvt_write_addr(NVT_ENG_RST, 0x5A);
	nvt_wait(1);
	nvt_gpio(nv.tn, NVT_PIN_RST, 0, 0, 1, true, true);
	nvt_wait(10);
	if (!nvt_identify()) {
		printf("nt36672c: no supported Novatek chip answered; stopping\n");
		return false;
	}
	if (!use_fw) {
		printf("nt36672c: touch_nvt_fw=0: no firmware sent\n");
		return true;
	}
	if (!nvt_load_file() || !nvt_parse_fw() || !nvt_download()) {
		return false;
	}
	nvt_fw_info();
	return true;
}

static void
nvt_thread(void *arg, wait_result_t result)
{
	unsigned delay_s = 2;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("touch_delay", &delay_s, sizeof(delay_s));
	printf("nt36672c: NT36672C SPI driver starting in %u s\n", delay_s);
	nvt_wait(delay_s * 1000u);
	nv.gcc = ml_io_map(NVT_GCC_PA, 0x60000);
	nv.tn = ml_io_map(NVT_TLMM_N_PA, 0x40000);
	nv.tw = ml_io_map(NVT_TLMM_W_PA, 0x40000);
	nv.se = ml_io_map(NVT_SE_PA, 0x4000);
	if (!nv.gcc || !nv.tn || !nv.tw || !nv.se) {
		printf("nt36672c: MMIO mapping failed\n");
		goto out;
	}
	printf("nt36672c: before rst=%#x/%#x irq=%#x/%#x spi49=%#x\n",
	    nvt_rd(nv.tn, NVT_GPIO_CFG(NVT_PIN_RST)), nvt_rd(nv.tn, NVT_GPIO_IO(NVT_PIN_RST)),
	    nvt_rd(nv.tn, NVT_GPIO_CFG(NVT_PIN_IRQ)), nvt_rd(nv.tn, NVT_GPIO_IO(NVT_PIN_IRQ)),
	    nvt_rd(nv.tw, NVT_GPIO_CFG(49)));
	if (!nvt_clocks()) {
		goto out;
	}
	/* qupv3_se0_spi: GPIO49-52 function qup00 (mux 2), 6 mA, no pull. */
	for (unsigned pin = 49; pin <= 52; ++pin) {
		nvt_gpio(nv.tw, pin, 2, 0, 2, false, false);
	}
	if (!nvt_se_init() || !nvt_bring_up()) {
		goto out;
	}
	(void)nvt_set_page(nv.map->event_buf);
	printf("nt36672c: polling touch every 10 ms\n");
	unsigned errors = 0, wdt = 0;
	while (!nv.dead) {
		int err = nvt_poll(&wdt);
		if (err) {
			if ((errors++ % 100u) == 0) {
				printf("nt36672c: read err=%d\n", err);
			}
			if (errors == 10) {
				nvt_report(0, nv.x, nv.y);
			}
		} else {
			errors = 0;
		}
		if (wdt > 10) {
			printf("nt36672c: firmware watchdog pattern, reloading\n");
			wdt = 0;
			nvt_report(0, nv.x, nv.y);
			if (!nv.fw || !nvt_download()) {
				break;
			}
			(void)nvt_set_page(nv.map->event_buf);
		}
		nvt_wait(10);
	}
	nvt_report(0, nv.x, nv.y);
out:
	printf("nt36672c: stopped\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

static void
nvt_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;                 /* nothing is touched unless a boot-argument asks */
	}
	if (kernel_thread_start(nvt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("nt36672c: no thread\n");
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, nvt_init);
