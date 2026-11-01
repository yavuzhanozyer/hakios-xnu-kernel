<!-- Made by tools/docs-gen.py from devices/surya/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# POCO X3 NFC (`surya`)

| | |
|---|---|
| Vendor | Xiaomi |
| SoC | `sm7150` |
| Configuration | `make surya_defconfig` |
| Maintainer | yavuzhanozyer |
| Repository | [`devices/surya/`](https://github.com/oosexgang/hakios-xnu-kernel/tree/main/devices/surya) |
| Firmware | `devices/surya/pull-firmware.sh` pulls it from your own phone |

## What works

| Part | State | |
|---|---|---|
| display | works | UEFI's framebuffer (1080 x 2400 at 0x9c000000) through the loader: the lock screen, its clock going on (2 November 2026); drawn by the CPU |
| touch | works | Novatek NT36672C on SPI, its firmware sent over SPI: the lock screen unlocked by a swipe (CONFIG_HAKIOS_NT36672C, touchscreen=1; moves limited by touch_move_px/_ms), 2 November 2026 |
| buttons | works | power and volume through the PM6150 / PM6150L, seen working in iOS (CONFIG_HAKIOS_BUTTONS, buttons_pon=0x008 buttons_pon_gen=2 buttons_volup=0x4c1), 2 November 2026 |
| cpu | works | 8 cores, the kernel on core 7 (cpus=8 bootcpu=7; the timer is INTID 19 here, vtimer_intid=19 from the loader); the clocks through OSM: the small cores at 1804 MHz, the big at 2304 MHz (cpufreq=1 cpufreq_osm=1), 2 November 2026 |
| storage | partly | UFS read through the loader (ufs=2, UEFI's block devices): the loader's, not yet run on this phone |
| gpu | partly | Adreno 618: steps 1 to 6 of hakios.adreno.c ported (adreno=6 adreno_a618=1): CX on, the GMU's clock at 200 MHz, its always-on counter at 19.2 MHz, "THE GPU BLOCK IS ALIVE" (2 November 2026); the GMU's firmware, HFI and the 3D core not yet |
| audio | no | the audio DSP (q6 ADSP, APR, WCD9375 codec) is not the SC7280's; firmware pulled (fw-audio/), no driver yet |
| battery | partly | the level in iOS from UEFI's Qualcomm charger drivers, read by the loader (battery=1), seen 2 November 2026; charging under this kernel is the firmware's, not seen |
| haptic | no |  |
| camera | no |  |
| wifi | no | WCN3990: its firmware runs on the modem (ath10k snoc), so the modem comes first; not the WCN6750 of hakios.wifi.c |
| bluetooth | no |  |
| cellular | no | soc_* not yet read from this phone's device tree (pull-soc.sh) |
| thermal | works | both TSENS v2 blocks, 25 of 32 sensors read, 43 to 51 C (thermal=1 thermal_table=2, 2 November 2026); the stop at 105 C not seen firing |

The state is what has been seen on this device, written down in `devices/surya/device.conf`; a part the table does not name has not been looked at.
