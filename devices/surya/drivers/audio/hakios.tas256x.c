/*
 * The speakers of the POCO X3 NFC (surya): step one, a look. Nothing here plays; nothing here is touched unless boot-arg
 * tas_probe=N asks, and what it does is read, but for the clock votes of step 2.
 *
 * From surya's device tree and Android's mixer paths (devices/surya/README.md, "Audio"):
 *   - two TI TAS256x amplifiers on I2C, the bus i2c@a8c000 (QUPv3 wrapper 1, serial engine 3: GCC vote bits 18-21 for
 *     the wrapper, 25 for the engine, as the ios-sweet port votes wrapper 1's engine 1 with bit 23), at 0x4c (right) and
 *     0x4d (left); reset by GPIO 8 of the PM6150L and GPIO 91 of the SoC, interrupts on GPIO 58 and 30
 *   - fed by TERTIARY MI2S: GPIO 53 bit clock, 54 frame sync, 55 and 56 data (function ter_mi2s), AFE port 0x1004,
 *     two channels of S24_LE at 48 kHz
 *
 * What is not known yet, and why this looks first: in which of the SoC's three pin tiles (west 0x3500000, north
 * 0x3900000, south 0x3d00000; all three inside the one region of qcom,sdmmagpie-pinctrl, 0x3400000 long 0xdc2000, so
 * reading any of them is reading the pin controller) those pins are, and how they are set now; and whether the I2C
 * engine has its firmware (loaded by the boot loader for the engines Android uses) and in what state.
 *
 *   tas_probe=1   every one of those pins' configuration and level, in each of the three tiles
 *   tas_probe=2   ... and the I2C engine: the GCC votes of its wrapper and itself (written: they switch clocks on, as
 *                 the touch screen's driver does for its engine), then its firmware's protocol, status and FIFO sizes
 *   tas_probe=3   ... and the amplifiers asked who they are: the engine's clock at 19.2 MHz (its RCG 0x183a8, as the
 *                 SDM845's wrapper 1 engine 3), its pins GPIO 46 and 47 (north tile) to function 1 (qup13, as Linux's
 *                 pinctrl-sm7150.c has it), the reset GPIO 91 (north) let go
 *                 (output, high), then at 0x4c and 0x4d: book 0 and page 0 chosen (registers 0x7f, 0x00) and the
 *                 revision (0x7d) read. The other amplifier's reset is GPIO 8 of the PM6150L: not touched here (a write
 *                 to a PMIC), so one of the two may not answer.
 *
 *   tas_probe=4   ... and the interface, with the audio DSP up (adsp=1 adsp_sm7150=1, waited for): GPIO 53-56 to function 1
 *                 (ter_mi2s), the DSP asked for TERT MI2S's bit clock (AFE clock 0x104, 3.072 MHz: 48 kHz, two slots of
 *                 32 bits, as Android's machine driver has it for S24_LE), the port 0x1004 set up (I2S, SD0, stereo, 24
 *                 bits, 48 kHz, its word clock ours) and started; then whether GPIO 53 and 54 toggle; held for
 *                 tas_secs (3), stopped, everything put back. Nothing goes to the amplifiers: no sound.
 *
 * The I2C transfers are the ios-sweet port's (xnu/patches/0060 and 0063, sweet_touch.c), run on this SoC's engine 7 there.
 *
 * Seen on the phone (tas_probe=2, 2 November 2026): GPIO 53-56 and 58 are in the west tile, 91 in the north, 30 in the
 * south (each reads 0x1, pull-down, where it is, and 0 elsewhere); 58 and 30 read 1 (the amplifiers' interrupt lines
 * held up: they have power); the engine has its I2C firmware (0x303), FIFOs of 16 and 32 words.
 * tas_probe=3 (2 November 2026): the bus's pins in the north tile, high at rest; with GPIO 91 let go, the amplifier at 0x4c
 * (the right) answers: revision 0x10, a TAS2562; the one at 0x4d (the left) does not: its reset is the PM6150L's GPIO 8.
 * tas_probe=4 (2 November 2026): the DSP took the clock (0x104, 3.072 MHz), the port's I2S configuration and its start; GPIO 53
 * toggled (6962 changes in 20000 reads) and GPIO 54 too (109): TERT MI2S runs.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.audio.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

static inline uint32_t
rd(vm_offset_t a)
{
	return *(volatile uint32_t *)a;
}

static inline void
wr(vm_offset_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

#define GPIO_CFG(n)             (0x1000u * (n))
#define GPIO_IO(n)              (0x1000u * (n) + 4)

static const struct {
	const char *name;
	uint32_t phys;
} tiles[] = { { "west", 0x03500000 }, { "north", 0x03900000 }, { "south", 0x03d00000 } };

static const struct {
	unsigned pin;
	const char *what;
} pins[] = {
	{ 53, "TERT MI2S bit clock" }, { 54, "TERT MI2S frame sync" }, { 55, "TERT MI2S data 0" }, { 56, "TERT MI2S data 1" },
	{ 91, "the amplifiers' reset (reset-gpio2)" }, { 58, "an amplifier's interrupt" }, { 30, "the other's interrupt" },
};

static void
look_at_pins(void)
{
	for (unsigned t = 0; t < sizeof(tiles) / sizeof(tiles[0]); t++) {
		vm_offset_t tile = ml_io_map(tiles[t].phys, 0x100000);

		for (unsigned p = 0; p < sizeof(pins) / sizeof(pins[0]); p++) {
			uint32_t cfg = rd(tile + GPIO_CFG(pins[p].pin)), io = rd(tile + GPIO_IO(pins[p].pin));

			say("tas256x: 1: %s tile, gpio%u (%s): cfg %#x (function %u, %u mA, pull %u%s), in %u out %u\n", tiles[t].name,
			    pins[p].pin, pins[p].what, cfg, cfg >> 2 & 0xf, ((cfg >> 6 & 7) + 1) * 2,
			    cfg & 3, cfg & (1u << 9) ? ", output" : "", io & 1, io >> 1 & 1);
		}
	}
}

/* ---- step 2: the I2C engine -------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000
#define GCC_VOTE                0x5200c
#define GCC_VOTE_WRAP1          (0xfu << 18)    /* the ios-sweet port's mask for wrapper 1 */
#define GCC_VOTE_WRAP1_S3       (1u << 25)      /* engine 3 of wrapper 1, as bit 23 is engine 1 */
#define SE_PHYS                 0x00a8c000      /* dt: i2c@0xa8c000 */
#define SE_STATUS               0x040
#define SE_FW                   0x068           /* [15:8] the protocol of the firmware loaded: 3 is I2C */
#define SE_HW_TX                0xe24           /* [21:16] FIFO depth */
#define SE_HW_RX                0xe28

static void
look_at_engine(void)
{
	vm_offset_t gcc = ml_io_map(GCC_PHYS, 0x80000), se;
	uint32_t vote = rd(gcc + GCC_VOTE), fw;

	say("tas256x: 2: GCC's vote register %#x; wrapper 1 %s, its engine 3 %s\n", vote,
	    (vote & GCC_VOTE_WRAP1) == GCC_VOTE_WRAP1 ? "voted for" : "NOT voted for", vote & GCC_VOTE_WRAP1_S3 ? "voted for" : "not");
	if ((vote & (GCC_VOTE_WRAP1 | GCC_VOTE_WRAP1_S3)) != (GCC_VOTE_WRAP1 | GCC_VOTE_WRAP1_S3)) {
		say("tas256x: 2: about to write GCC's vote register: %#x\n", vote | GCC_VOTE_WRAP1 | GCC_VOTE_WRAP1_S3);
		wr(gcc + GCC_VOTE, vote | GCC_VOTE_WRAP1 | GCC_VOTE_WRAP1_S3);
		delay_for_interval(10, NSEC_PER_MSEC);
		say("tas256x: 2: it reads %#x\n", rd(gcc + GCC_VOTE));
	}
	se = ml_io_map(SE_PHYS, 0x4000);
	say("tas256x: 2: about to read the I2C engine at %#x\n", SE_PHYS);
	delay_for_interval(1500, NSEC_PER_MSEC);        /* (for the loader's log to have the line before a read that might hang) */
	fw = rd(se + SE_FW);
	say("tas256x: 2: the engine's firmware %#x: protocol %u (%s), version %u; status %#x; FIFOs %u and %u words\n", fw,
	    fw >> 8 & 0xff, (fw >> 8 & 0xff) == 3 ? "I2C" : (fw >> 8 & 0xff) == 0 ? "NONE: no firmware loaded" : "not I2C", fw & 0xff,
	    rd(se + SE_STATUS), rd(se + SE_HW_TX) >> 16 & 0x3f, rd(se + SE_HW_RX) >> 16 & 0x3f);
}

/* ---- step 3: the amplifiers asked who they are ---------------------------------------------------------- */

#define TLMM_WEST               0x03500000
#define TLMM_NORTH              0x03900000
#define GCC_RCG_WRAP1_S3        0x183a8         /* gcc-sdm845.c: gcc_qupv3_wrap1_s3_clk_src */
#define GCC_BRANCHES_WRAP1      { 0x18004, 0x18008, 0x1800c, 0x18010, 0x183a4 }
#define SE_MCLK                 0x048
#define SE_CLK_SEL              0x07c
#define SE_DMA                  0x258
#define SE_TX_LEN               0x26c
#define SE_RX_LEN               0x270
#define SE_SCL                  0x278
#define SE_CMD                  0x600
#define SE_CTRL                 0x604
#define SE_IRQ                  0x610
#define SE_IRQ_EN               0x614
#define SE_IRQ_CLR              0x618
#define SE_TX                   0x700
#define SE_RX                   0x780
#define SE_TX_COUNT             0x800
#define SE_RX_COUNT             0x804
#define SE_TX_WM                0x80c
#define SE_RX_WM_IRQ            (1u << 26)
#define SE_RX_LAST              (1u << 27)
#define SE_TX_WM_IRQ            (1u << 30)
#define SE_DONE                 (1u << 0)
#define SE_NACK                 (1u << 10)
#define SE_ERRORS               (0x3eu | (1u << 9) | (1u << 10) | (1u << 12) | (3u << 24) | (3u << 28))
#define SE_EVENTS               (SE_DONE | SE_ERRORS | SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ)
#define SE_PACK0                0x0007f8feu
#define SE_PACK1                0x000ffefeu

static vm_offset_t gcc_base, se_base;
static bool se_dead;

static bool
wait_clear(vm_offset_t base, uint32_t off, uint32_t mask)
{
	for (unsigned i = 0; i < 5000; i++) {
		if (!(rd(base + off) & mask)) {
			return true;
		}
		delay(1);
	}
	return false;
}

static void
gpio_set(vm_offset_t tile, unsigned pin, unsigned function, unsigned pull, unsigned drive_code, bool out, bool high)
{
	uint32_t cfg = rd(tile + GPIO_CFG(pin));

	if (out) {
		wr(tile + GPIO_IO(pin), high ? 2 : 0);
	}
	cfg = (cfg & ~0x3ffu) | function << 2 | pull | drive_code << 6 | (out ? 1u << 9 : 0);
	say("tas256x: 3: about to write gpio%u's configuration: %#x\n", pin, cfg);
	wr(tile + GPIO_CFG(pin), cfg);
}

static bool
engine_clock(void)
{
	static const uint32_t branches[] = GCC_BRANCHES_WRAP1;
	uint32_t cfg = rd(gcc_base + GCC_RCG_WRAP1_S3 + 4), dfs = rd(gcc_base + GCC_RCG_WRAP1_S3 + 0x14);

	say("tas256x: 3: the engine's clock: command %#x, configuration %#x, DFS %#x\n", rd(gcc_base + GCC_RCG_WRAP1_S3), cfg, dfs);
	if (dfs & 1) {
		cfg = rd(gcc_base + GCC_RCG_WRAP1_S3 + 0x1c);
		if ((cfg & 0x371f) != 1) {
			say("tas256x: 3: its DFS level 0 (%#x) is not the crystal undivided: not changed, stopping\n", cfg);
			return false;
		}
	} else {
		cfg = (cfg & ~(0x371fu | 1u << 20)) | 1;        /* the crystal, divided by 1: 19.2 MHz */
		say("tas256x: 3: about to set it to the crystal, 19.2 MHz: %#x\n", cfg);
		wr(gcc_base + GCC_RCG_WRAP1_S3 + 4, cfg);
		wr(gcc_base + GCC_RCG_WRAP1_S3, rd(gcc_base + GCC_RCG_WRAP1_S3) | 1);
		if (!wait_clear(gcc_base, GCC_RCG_WRAP1_S3, 1)) {
			say("tas256x: 3: the clock does not take it\n");
			return false;
		}
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		if (!wait_clear(gcc_base, branches[i], 1u << 31)) {
			say("tas256x: 3: GCC's branch %#x stays off (%#x)\n", branches[i], rd(gcc_base + branches[i]));
			return false;
		}
	}
	return true;
}

static bool
engine_init(void)
{
	uint32_t rx = rd(se_base + SE_HW_RX);

	if ((rd(se_base + SE_FW) >> 8 & 0xff) != 3 || (rd(se_base + SE_STATUS) & 0x1001)) {
		say("tas256x: 3: the engine is not an idle I2C one (firmware %#x, status %#x)\n", rd(se_base + SE_FW),
		    rd(se_base + SE_STATUS));
		return false;
	}
	wr(se_base + 0xe18, 0);                 /* no GSI events */
	wr(se_base + 0xe1c, 0);                 /* its interrupts not routed: its status is looked at */
	wr(se_base + 0x644, 0);
	wr(se_base + SE_IRQ_CLR, 0xffffffffu);
	wr(se_base + 0x648, 0xffffffffu);
	wr(se_base + SE_DMA, 0);
	wr(se_base + 0x28, rd(se_base + 0x28) | 0x7f);
	wr(se_base + 0xe30, rd(se_base + 0xe30) | 0x0f);
	wr(se_base + 0x24, 0x7f);
	wr(se_base + 0x20, 1);
	wr(se_base + SE_CLK_SEL, 0);
	wr(se_base + SE_MCLK, 7u << 4 | 1);     /* 19.2 MHz / 7 */
	wr(se_base + SE_SCL, 10u << 20 | 11u << 10 | 26);       /* 100 kHz */
	wr(se_base + 0x254, 0);
	wr(se_base + 0x260, SE_PACK0);
	wr(se_base + 0x264, SE_PACK1);
	wr(se_base + 0x284, SE_PACK0);
	wr(se_base + 0x288, SE_PACK1);
	wr(se_base + SE_TX_WM, 0);
	wr(se_base + 0x810, 0);
	wr(se_base + 0x814, (rx >> 16 & 0x3f) - 1);
	wr(se_base + SE_IRQ_EN, SE_EVENTS);
	if ((rd(se_base + SE_TX_COUNT) & 0xfffffff) || (rd(se_base + SE_RX_COUNT) & 0x1ffffff)) {
		say("tas256x: 3: the engine's FIFOs hold something already\n");
		return false;
	}
	return true;
}

static void
engine_abort(void)
{
	wr(se_base + SE_CTRL, 2);
	for (unsigned i = 0; i < 5000 && !(rd(se_base + SE_IRQ) & (1u << 5)); i++) {
		delay(1);
	}
	for (unsigned i = 0; i < 64 && (rd(se_base + SE_RX_COUNT) & 0x1ffffff); i++) {
		(void)rd(se_base + SE_RX);
	}
	wr(se_base + SE_IRQ_CLR, 0xffffffffu);
	if (!wait_clear(se_base, SE_STATUS, 1)) {
		se_dead = true;
	}
}

/* One message: a write of 1 to 3 bytes or a read of 1 to 32; stretch keeps the bus for a repeated start. */
static int
i2c_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr, bool stretch)
{
	unsigned sent = 0, got = 0;

	if (se_dead || nt > 3 || nr > 32 || (nt && nr) || (!nt && !nr) || !wait_clear(se_base, SE_STATUS, 1)) {
		return EIO;
	}
	wr(se_base + SE_IRQ_CLR, 0xffffffffu);
	wr(se_base + SE_TX_WM, nt ? 1 : 0);
	wr(se_base + (nr ? SE_RX_LEN : SE_TX_LEN), nr ? nr : nt);
	wr(se_base + SE_CMD, (nr ? 2u : 1u) << 27 | (uint32_t)addr << 9 | (stretch ? 4u : 0));
	for (unsigned i = 0; i < 20000; i++) {
		uint32_t irq = rd(se_base + SE_IRQ), count;

		if (irq & SE_ERRORS) {
			int error = (irq & SE_NACK) ? ENXIO : EIO;

			for (unsigned j = 0; j < 5000 && !(irq & SE_DONE); j++) {
				delay(1);
				irq = rd(se_base + SE_IRQ);
			}
			if ((irq & SE_DONE) && !(rd(se_base + SE_STATUS) & 1)) {
				wr(se_base + SE_IRQ_CLR, irq);
				return error;
			}
			engine_abort();
			return error;
		}
		if (nt && !sent && (irq & SE_TX_WM_IRQ)) {
			uint32_t word = 0;

			for (unsigned j = 0; j < nt; j++) {
				word |= (uint32_t)tx[j] << (8 * j);
			}
			wr(se_base + SE_TX, word);
			sent = nt;
			wr(se_base + SE_TX_WM, 0);
		}
		count = rd(se_base + SE_RX_COUNT) & 0x1ffffff;
		while (count-- && got < nr) {
			uint32_t word = rd(se_base + SE_RX);

			for (unsigned j = 0; j < 4 && got < nr; j++) {
				rx[got++] = (uint8_t)(word >> (8 * j));
			}
		}
		if (irq & SE_DONE) {
			wr(se_base + SE_IRQ_CLR, irq);
			return got == nr && sent == nt ? 0 : EIO;
		}
		wr(se_base + SE_IRQ_CLR, irq & (SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ));
		delay(1);
	}
	engine_abort();
	return ETIMEDOUT;
}

static int
tas_write(uint8_t addr, uint8_t reg, uint8_t value)
{
	uint8_t data[2] = { reg, value };

	return i2c_msg(addr, data, 2, NULL, 0, false);
}

static int
tas_read(uint8_t addr, uint8_t reg, uint8_t *value)
{
	int error = i2c_msg(addr, &reg, 1, NULL, 0, true);

	return error ? error : i2c_msg(addr, NULL, 0, value, 1, false);
}

static bool bus_ready;
static uint32_t gpio91_was;
static vm_offset_t north_base;

/* The I2C bus to the amplifiers: its clock, its pins, the amplifiers' reset let go, the engine set up (once a boot). */
static bool
bus_up(void)
{
	vm_offset_t north;

	if (bus_ready) {
		return true;
	}
	north = north_base = ml_io_map(TLMM_NORTH, 0x100000);
	gcc_base = ml_io_map(GCC_PHYS, 0x80000);
	se_base = ml_io_map(SE_PHYS, 0x4000);
	if (!engine_clock()) {
		return false;
	}
	/*
	 * qupv3_se9_i2c_active: GPIO 46 and 47, function qup13, 2 mA, no pull. Linux's pinctrl-sm7150.c: both in the NORTH
	 * tile, qup13 their function 1 (and TERT MI2S 53-56 in the west, function 1 ter_mi2s; 91 north; 30 south: as seen).
	 * Taken for west first, a write to 46 there reset the phone (2 November 2026): a tile has no registers for pins
	 * that are not its own.
	 */
	say("tas256x: 3: the bus's pins in the north tile: gpio46 cfg %#x in %u, gpio47 cfg %#x in %u\n", rd(north + GPIO_CFG(46)),
	    rd(north + GPIO_IO(46)) & 1, rd(north + GPIO_CFG(47)), rd(north + GPIO_IO(47)) & 1);
	gpio_set(north, 46, 1, 0, 0, false, false);
	gpio_set(north, 47, 1, 0, 0, false, false);
	delay_for_interval(1, NSEC_PER_MSEC);
	say("tas256x: 3: now: gpio46 cfg %#x in %u, gpio47 cfg %#x in %u (an idle bus with its pull-ups powered reads 1)\n",
	    rd(north + GPIO_CFG(46)), rd(north + GPIO_IO(46)) & 1, rd(north + GPIO_CFG(47)), rd(north + GPIO_IO(47)) & 1);
	say("tas256x: 3: the amplifiers' reset GPIO 91 let go: output, high\n");
	gpio91_was = rd(north + GPIO_CFG(91));
	gpio_set(north, 91, 0, 0, 0, true, true);
	delay_for_interval(5, NSEC_PER_MSEC);
	if (!engine_init()) {
		return false;
	}
	bus_ready = true;
	return true;
}

static void
ask_amplifiers(void)
{
	static const uint8_t addresses[2] = { 0x4c, 0x4d };

	if (!bus_up()) {
		return;
	}
	for (unsigned a = 0; a < 2; a++) {
		uint8_t revision = 0;
		int error = tas_write(addresses[a], 0x00, 0);

		if (!error) {
			error = tas_write(addresses[a], 0x7f, 0);
		}
		if (!error) {
			error = tas_read(addresses[a], 0x7d, &revision);
		}
		if (error) {
			say("tas256x: 3: at %#x: %s (%d)\n", addresses[a], error == ENXIO ? "NO ANSWER (its reset may be the "
			    "PM6150L's GPIO 8)" : "the transfer failed", error);
		} else {
			say("tas256x: 3: AT %#x AN AMPLIFIER ANSWERS: revision register 0x7d = %#x (%s)\n", addresses[a], revision,
			    (revision & 0xf0) == 0x10 ? "TAS2562" : (revision & 0xf0) == 0x20 ? "TAS2564" : "not a revision this knows");
		}
	}
}

/* ---- step 4: TERT MI2S, through the audio DSP -------------------------------------------------------------- */

#define APR_SVC_AFE                     4
#define APR_BASIC_RSP_RESULT            0x000110e8u
#define AFE_PORT_CMD_DEVICE_START       0x000100e5u
#define AFE_PORT_CMD_DEVICE_STOP        0x000100e6u
#define AFE_PORT_CMD_SET_PARAM_V2       0x000100efu
#define AFE_SVC_CMD_SET_PARAM           0x000100f3u
#define AFE_MODULE_AUDIO_DEV_INTERFACE  0x0001020cu
#define AFE_PARAM_ID_I2S_CONFIG         0x0001020du
#define AFE_MODULE_CLOCK_SET            0x0001028fu
#define AFE_PARAM_ID_CLOCK_SET          0x00010290u
#define PORT_TERT_MI2S_RX               0x1004
#define CLOCK_TER_MI2S_IBIT             0x104
#define CLOCK_COUPLE_NO                 1
#define MI2S_RATE                       48000
#define MI2S_BIT_CLOCK                  (MI2S_RATE * 32 * 2)

struct afe_param {
	uint32_t module, param;
	uint16_t size, reserved;
} __attribute__((packed));

struct afe_clock_set {
	uint32_t version, id, hz;
	uint16_t attribute, root;
	uint32_t enable;
} __attribute__((packed));

struct afe_i2s {
	uint32_t version;
	uint16_t bits, channel_mode, mono_stereo, ws_source;
	uint32_t rate;
	uint16_t data_format, reserved;
} __attribute__((packed));

static bool
afe_command(const char *what, uint32_t opcode, const void *payload, uint32_t length)
{
	struct hakios_apr_answer answer;

	if (!hakios_adsp_apr(APR_SVC_AFE, 0, 0, opcode, payload, length, 1000, &answer)) {
		say("tas256x: 4: %s: NO ANSWER from the DSP\n", what);
		return false;
	}
	if (answer.opcode == APR_BASIC_RSP_RESULT && answer.words[0] == opcode) {
		say("tas256x: 4: %s: %s %#x\n", what, answer.words[1] == 0 ? "done" : "REFUSED, error", answer.words[1]);
		return answer.words[1] == 0;
	}
	say("tas256x: 4: %s: an answer not expected: opcode %#x %#x %#x\n", what, answer.opcode, answer.words[0], answer.words[1]);
	return false;
}

/* A parameter: of the AFE as a whole (in band, port 0) or of a port. */
static bool
afe_param(const char *what, bool of_port, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t packet[16 + sizeof(struct afe_param) + 32];
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	if (of_port) {
		*(uint16_t *)packet = PORT_TERT_MI2S_RX;
		*(uint16_t *)(packet + 2) = (uint16_t)(sizeof(*p) + size);
	} else {
		*(uint32_t *)packet = (uint32_t)sizeof(*p) + size;
	}
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return afe_command(what, of_port ? AFE_PORT_CMD_SET_PARAM_V2 : AFE_SVC_CMD_SET_PARAM, packet,
	           16 + (uint32_t)sizeof(*p) + size);
}

static bool
afe_clock(bool on)
{
	struct afe_clock_set clock = { 1, CLOCK_TER_MI2S_IBIT, MI2S_BIT_CLOCK, CLOCK_COUPLE_NO, 0, on };

	return afe_param(on ? "TERT MI2S's bit clock (0x104) at 3.072 MHz, on" : "TERT MI2S's bit clock, off", false,
	           AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, &clock, sizeof(clock));
}

static void
toggles(vm_offset_t tile, unsigned pin, const char *name)
{
	uint32_t last = rd(tile + GPIO_IO(pin)) & 1, changes = 0, ones = 0;

	for (uint32_t i = 0; i < 20000; i++) {
		uint32_t now = rd(tile + GPIO_IO(pin)) & 1;

		changes += now != last;
		ones += now;
		last = now;
	}
	say("tas256x: 4: gpio%u (%s): %u changes and %u ones in 20000 reads%s\n", pin, name, changes, ones,
	    changes > 100 ? ": IT TOGGLES" : "");
}

static void
interface_on(void)
{
	static const unsigned mi2s[] = { 53, 54, 55, 56 };
	vm_offset_t west = ml_io_map(TLMM_WEST, 0x100000);
	uint32_t was[4], secs = 3;
	struct afe_i2s i2s = { .version = 1, .bits = 24, .channel_mode = 1 /* SD0 */, .mono_stereo = 1 /* stereo */,
		               .ws_source = 1 /* ours */, .rate = MI2S_RATE, .data_format = 0 };
	uint16_t start[2] = { PORT_TERT_MI2S_RX, 0 };
	struct hakios_apr_answer answer;
	bool clock_on = false, port_on = false;
	unsigned waited;

	PE_parse_boot_argn("tas_secs", &secs, sizeof(secs));
	if (secs > 30) {
		secs = 30;
	}
	/* the audio DSP's services: asked until they answer (adsp=1 brings them up beside the system's start) */
	for (waited = 0; waited < 120; waited++) {
		uint16_t nothing[2] = { PORT_TERT_MI2S_RX, 0 };      /* (our port, not started: a stop of it changes nothing) */

		if (hakios_adsp_apr(APR_SVC_AFE, 0, 0, AFE_PORT_CMD_DEVICE_STOP, nothing, sizeof(nothing), 500, &answer)) {
			break;
		}
		delay_for_interval(1000, NSEC_PER_MSEC);
	}
	if (waited == 120) {
		say("tas256x: 4: the audio DSP's services do not answer after two minutes (adsp=1 adsp_sm7150=1?): stopping\n");
		return;
	}
	say("tas256x: 4: the audio DSP answers (after %u s)\n", waited);
	for (unsigned i = 0; i < 4; i++) {
		was[i] = rd(west + GPIO_CFG(mi2s[i]));
		/* function 1 (ter_mi2s), 8 mA, no pull; the clock and the frame sync output, as Android's active state */
		gpio_set(west, mi2s[i], 1, 0, 3, i < 2, true);
	}
	clock_on = afe_clock(true);
	if (clock_on && afe_param("the port 0x1004: I2S on SD0, stereo, 24 bits, 48 kHz, the word clock ours", true,
	    AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_I2S_CONFIG, &i2s, sizeof(i2s))) {
		port_on = afe_command("the port 0x1004, started", AFE_PORT_CMD_DEVICE_START, start, sizeof(start));
	}
	if (port_on) {
		toggles(west, 53, "TERT MI2S bit clock");
		toggles(west, 54, "TERT MI2S frame sync");
		say("tas256x: 4: held for %u s\n", secs);
		delay_for_interval(secs * 1000, NSEC_PER_MSEC);
		toggles(west, 53, "TERT MI2S bit clock");
		afe_command("the port 0x1004, stopped", AFE_PORT_CMD_DEVICE_STOP, start, sizeof(start));
	}
	if (clock_on) {
		afe_clock(false);
	}
	for (unsigned i = 0; i < 4; i++) {
		say("tas256x: 4: about to put gpio%u's configuration back: %#x\n", mi2s[i], was[i]);
		wr(west + GPIO_CFG(mi2s[i]), was[i]);
	}
}

/*
 * ---- the right amplifier for hakios.sound.c (sound=3 adsp_sm7150=1) ---------------------------------------------------
 *
 * The TAS2562 at 0x4c, set up as Linux's mainline driver does (sound/soc/codecs/tas2562.c): a software reset; I2S
 * (frame start on the word clock's falling edge, data one bit after it, sampled on the rising edge), 48 kHz, 24-bit
 * words in 32-bit slots, the average of left and right (one amplifier, both channels of the tone); the current and
 * voltage sense off (no speaker protection here: it is the DSP's under Android); powered up only while the port's
 * clock runs. Its loudness is the digital volume (page 2, 0x0c-0x0f, 0x40000000 for 0 dB), 6 dB a step; "below" is
 * eighths of a dB under 0 dB of that volume at the amplifier's own default level. The left one at 0x4d is held in
 * reset by the PM6150L's GPIO 8 and stays silent.
 */
#define TAS_RIGHT               0x4c
#define TAS_PAGE                0x00
#define TAS_SW_RESET            0x01
#define TAS_PWR_CTL             0x02
#define TAS_PWR_ACTIVE          0x0c            /* mode 0 (active), the voltage and current sense powered down */
#define TAS_PWR_SHUTDOWN        0x0e            /* mode 2 (shutdown), the same */
#define TAS_TDM_CFG0            0x06
#define TAS_TDM_CFG1            0x07
#define TAS_TDM_CFG2            0x08
#define TAS_INT_LTCH0           0x24            /* bit 0 over temperature, bit 1 over current */
#define TAS_REV_ID              0x7d
#define TAS_BOOK                0x7f
#define TAS_DVC                 0x0c            /* page 2: the digital volume, four bytes, the high one first */
#define TAS_MUTED               960             /* 120 dB under: the volume 0 */

bool hakios_tas256x_prepare(void);
bool hakios_tas256x_start(void);
bool hakios_tas256x_volume(uint32_t below, uint32_t step_ms);
bool hakios_tas256x_healthy(void);
void hakios_tas256x_stop(void);
void hakios_tas256x_quiesce(void);

static bool amp_prepared, amp_on;
static uint32_t amp_below = TAS_MUTED;

static bool
amp_write(const char *what, uint8_t reg, uint8_t value)
{
	int error = tas_write(TAS_RIGHT, reg, value);

	if (error) {
		say("tas256x: the amplifier: %s (%#x <- %#x): THE TRANSFER FAILED (%d)\n", what, reg, value, error);
		return false;
	}
	return true;
}

static bool
amp_read(uint8_t reg, uint8_t *value)
{
	int error = tas_read(TAS_RIGHT, reg, value);

	if (error) {
		say("tas256x: the amplifier: a read of %#x FAILED (%d)\n", reg, error);
		return false;
	}
	return true;
}

static bool
amp_volume_set(uint32_t below_now)
{
	static const uint32_t db[20] = {
		0x40000000, 0x390a4160, 0x32d64618, 0x2d4efbd6, 0x28619aea, 0x23fd6678, 0x2013739e, 0x1c9676c7, 0x197a967f, 0x16b54338,
		0x143d1362, 0x1209a37b, 0x10137988, 0x0e53ebb4, 0x0cc509ac, 0x0b618872, 0x0a24b063, 0x090a4d30, 0x080e9f97, 0x072e50a6,
	};
	uint32_t under = below_now / 8, value = 0;

	if (below_now < TAS_MUTED) {
		value = db[under % 20];
		for (uint32_t tens = under / 20; tens; tens--) {
			value /= 10;
		}
	}
	if (!amp_write("page 2", TAS_PAGE, 2)) {
		return false;
	}
	for (unsigned i = 0; i < 4; i++) {
		if (!amp_write("the digital volume", (uint8_t)(TAS_DVC + i), (uint8_t)(value >> (24 - 8 * i)))) {
			(void)amp_write("page 0", TAS_PAGE, 0);
			return false;
		}
	}
	amp_below = below_now;
	return amp_write("page 0", TAS_PAGE, 0);
}

bool
hakios_tas256x_prepare(void)
{
	uint8_t revision = 0, cfg0 = 0, cfg1 = 0, cfg2 = 0;

	if (amp_prepared) {
		return true;
	}
	if (!bus_up()) {
		return false;
	}
	say("tas256x: the amplifier at %#x: reset, I2S at 48 kHz (24 bits in 32), left and right averaged, shut down, muted\n",
	    TAS_RIGHT);
	if (!amp_write("page 0", TAS_PAGE, 0) || !amp_write("book 0", TAS_BOOK, 0) || !amp_write("software reset", TAS_SW_RESET, 1)) {
		return false;
	}
	delay_for_interval(2, NSEC_PER_MSEC);
	if (!amp_write("page 0", TAS_PAGE, 0) || !amp_read(TAS_REV_ID, &revision)) {
		return false;
	}
	if ((revision & 0xf0) != 0x10) {
		say("tas256x: the amplifier's revision %#x is not a TAS2562's: not set up\n", revision);
		return false;
	}
	if (!amp_write("shut down", TAS_PWR_CTL, TAS_PWR_SHUTDOWN) || !amp_read(TAS_TDM_CFG0, &cfg0) ||
	    !amp_read(TAS_TDM_CFG1, &cfg1) || !amp_read(TAS_TDM_CFG2, &cfg2)) {
		return false;
	}
	say("tas256x: the amplifier after its reset: TDM configuration %#x %#x %#x\n", cfg0, cfg1, cfg2);
	cfg0 = (uint8_t)((cfg0 & ~0x0fu) | 4u << 1 | 1);        /* 44.1/48 kHz; the frame starts high-to-low (I2S) */
	cfg1 = (uint8_t)((cfg1 & ~0x3fu) | 1u << 1);            /* data one bit clock after the frame start, the rising edge */
	cfg2 = (uint8_t)((cfg2 & ~0x3fu) | 3u << 4 | 2u << 2 | 2u);     /* (left + right) / 2; 24-bit words; 32-bit slots */
	if (!amp_write("TDM configuration 0", TAS_TDM_CFG0, cfg0) || !amp_write("TDM configuration 1", TAS_TDM_CFG1, cfg1) ||
	    !amp_write("TDM configuration 2", TAS_TDM_CFG2, cfg2) || !amp_volume_set(TAS_MUTED)) {
		return false;
	}
	(void)amp_read(TAS_TDM_CFG0, &cfg0);
	(void)amp_read(TAS_TDM_CFG1, &cfg1);
	(void)amp_read(TAS_TDM_CFG2, &cfg2);
	say("tas256x: the amplifier set up: TDM configuration %#x %#x %#x, shut down, muted\n", cfg0, cfg1, cfg2);
	amp_prepared = true;
	return true;
}

bool
hakios_tas256x_start(void)
{
	uint8_t power = 0, latch = 0;

	if (!amp_prepared) {
		return false;
	}
	if (!amp_write("powered up, active", TAS_PWR_CTL, TAS_PWR_ACTIVE)) {
		return false;
	}
	amp_on = true;
	delay_for_interval(10, NSEC_PER_MSEC);
	(void)amp_read(TAS_PWR_CTL, &power);
	(void)amp_read(TAS_INT_LTCH0, &latch);
	say("tas256x: the amplifier is on: power %#x, faults latched %#x\n", power, latch);
	return true;
}

bool
hakios_tas256x_volume(uint32_t below, uint32_t step_ms)
{
	if (!amp_prepared) {
		return false;
	}
	if (below > TAS_MUTED) {
		below = TAS_MUTED;
	}
	while (amp_below != below) {
		uint32_t next = amp_below > below ? (amp_below - below > 48 ? amp_below - 48 : below) :
		    (below - amp_below > 48 ? amp_below + 48 : below);

		if (!amp_volume_set(next)) {
			hakios_tas256x_quiesce();
			return false;
		}
		if (amp_below != below) {
			delay_for_interval(step_ms, NSEC_PER_MSEC);
		}
	}
	say("tas256x: the amplifier's volume: %u.%03u dB under 0 dB\n", below / 8, below % 8 * 125);
	return true;
}

bool
hakios_tas256x_healthy(void)
{
	uint8_t latch = 0;

	if (!amp_on) {
		return true;
	}
	if (!amp_read(TAS_INT_LTCH0, &latch)) {
		hakios_tas256x_quiesce();
		return false;
	}
	if (latch & 3) {
		say("tas256x: THE AMPLIFIER REPORTS A FAULT (%#x: %s%s): shut down\n", latch, (latch & 1) ? "over temperature " : "",
		    (latch & 2) ? "over current" : "");
		hakios_tas256x_quiesce();
		return false;
	}
	return true;
}

void
hakios_tas256x_stop(void)
{
	if (!amp_prepared) {
		return;
	}
	if (amp_on) {
		(void)hakios_tas256x_volume(TAS_MUTED, 20);
		(void)amp_write("shut down", TAS_PWR_CTL, TAS_PWR_SHUTDOWN);
		amp_on = false;
	}
	say("tas256x: the amplifier shut down; its reset GPIO 91 put back (%#x)\n", gpio91_was);
	wr(north_base + GPIO_IO(91), 0);
	wr(north_base + GPIO_CFG(91), gpio91_was);
	amp_prepared = false;
	bus_ready = false;
}

void
hakios_tas256x_quiesce(void)
{
	if (amp_on) {
		(void)tas_write(TAS_RIGHT, TAS_PWR_CTL, TAS_PWR_SHUTDOWN);
		amp_on = false;
	}
}

static void
probe_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter;

	say("tas256x: a look at the speakers' amplifiers and their interface (tas_probe=%u): reads only, but GCC's votes in step 2\n",
	    steps);
	look_at_pins();
	if (steps >= 2) {
		look_at_engine();
	}
	if (steps >= 3) {
		ask_amplifiers();
	}
	if (steps >= 4) {
		interface_on();
	}
	say("tas256x: the look is over\n");
	thread_terminate(current_thread());
}

static void
tas256x_init(void)
{
	uint32_t steps = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("tas_probe", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (kernel_thread_start(probe_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, tas256x_init);
