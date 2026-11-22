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

## What the tool says can be turned on now

Only `buttons-pmic`. Everything else needs a section first (`tools/boards.py status surya`).

## To do, in the order that costs least

1. `loader { }`, `uefi { }` (the Mu-Silicium UEFI of `hakios-uefi`, branch `main`), the I/O window, `cpufreq { }`.
2. `buttons=1`, thermal, battery.
3. Touch (NT36672C, needs the pin tiles), the audio DSP and the speakers (needs `ipc { }`, `rpmh { }`).
4. Wi-Fi: Linux's ath10k on the layer in `kernel/lx`, as ath11k is, with `bdf_j20g.bin`.
5. The Adreno 618's GPU.
