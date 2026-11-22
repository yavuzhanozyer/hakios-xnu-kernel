# surya: Xiaomi POCO X3 NFC (SM7150)

The device package of the POCO X3 NFC for this tree. Started with `tools/boards.py new surya --soc sm7150 --dts
surya.dts --panel qcom,mdss_dsi_nt36672c_tianma_fhd_video` from the phone's own device tree; `tools/boards.py check`
passes. **Nothing here has been run on the phone.** What is known to work on this phone is in the other branch of this
repository (`main`, the iOS 16.5 / XNU 8796 line: devices/surya/README.md); none of it has been ported to this tree.

## What is different from the first two phones (SM7325, SM7350)

* **Pins are in three tiles** (west `0x03500000`, north `0x03900000`, south `0x03d00000`), not one block: `pins { tlmm }`
  holds the west tile only. GPIOs outside it (the touch's reset 8 and interrupt 9, the left speaker amplifier's
  reset) need tiles in the pin model first.
* **Wi-Fi is a WCN3990** (Linux's `ath10k`, over the modem's WLFW service), not a WCN6750 (`ath11k`). The feature
  `wifi-wcn6750` does not apply; there is no ath10k feature here. What was learnt on the phone: the board's data is
  Xiaomi's `bdf_j20g.bin`, not any of the firmware partition's `bdwlan.*`.
* **The audio amplifiers are two TAS2562** (reset of the left one on a PMIC GPIO), the touch controller a flashless
  **NT36672C** on SPI, the GPU an **Adreno 618**: none of `speaker-aw88261`, `touch-goodix`, `gpu-a660` is this phone's.
* **cpufreq** is OSM (`cpufreq_osm`), whose block this tree's `cpufreq { }` can describe (SDM845 to SM8250 have the same).
* The modem is the SM7150's: it rings the application processor through the APCS, not IPCC.

## State (22 November 2026)

`tools/boards.py check` passes. **Nothing here has been run on the phone.**

Filled from what the other branch (`main`, the iOS 16.5 / XNU 8796 line) learnt on this phone:

* `loader { }`: UEFI's framebuffer 1080 x 2400 at `0x9C000000` ("Display Reserved"), and Qualcomm's charger protocols
  (`uefi-charger`).
* `uefi { }`: Mu-Silicium's `surya.toml`, FD at `0x9FC00000`, 3 MB (the UEFI is `hakios-uefi`, branch `main`).
* `platform { }`: the I/O window, the GIC, the UFS controller, eight cores (0-5 Cortex-A55, 6-7 Cortex-A76).
* `cpufreq { }`: the OSM block, `0x18323000` (cores 0-5) and `0x18325800` (cores 6-7).
* `boot { bootargs = "vtimer_intid=19" }`: the virtual timer is INTID 19 on this SoC, not 27; the kernel's GIC takes it
  from that argument (`arch/arm64/kernel/gic_v3_irq.c`, branch `common`).

What `tools/boards.py status surya` lets be turned on now: `cpufreq-epss` (reads the OSM block: `cpufreq=1`),
`battery-uefi` (`battery=1`), `buttons-pmic` (`buttons=1`). The first run on the phone is level 1 of each.

## To do, in the order that costs least

1. First run: `cpufreq=1`, `battery=1`, `buttons=1`, then thermal (needs `thermal { }`), and what the log says.
2. Pin tiles in the pin model (a change to the code branch, not to this package), then the touch controller (NT36672C).
3. The audio DSP and the speakers (need `ipc { }`, `rpmh { }`).
4. Wi-Fi: Linux's ath10k on the layer in `kernel/lx`, as ath11k is, with `bdf_j20g.bin`.
5. The Adreno 618's GPU.
