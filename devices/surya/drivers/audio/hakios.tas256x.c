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
 *
 * Status: written, not yet run.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
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
