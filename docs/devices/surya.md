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
| touch | partly | NT36672C: the chip answers, its firmware runs (reset state 0xa0, 1080x2400), touches reach backboardd; they came 75 s late from a flood of moves, now limited (touch_move_px/_ms): to be seen again |
| buttons | partly | power and volume read from the PM6150 / PM6150L, every press seen in the log (buttons_pon=0x008 buttons_pon_gen=2 buttons_volup=0x4c1); what iOS does with them waits on the same queue as the touches |
| cpu | partly | 8 cores, the kernel on core 7 (cpus=8 bootcpu=7): to the lock screen in 2-3 minutes; the timer is INTID 19 here (vtimer_intid=19, the loader gives it); the clocks (cpufreq_osm=1) not yet run |
| storage | partly | UFS read through the loader (ufs=2, UEFI's block devices): the loader's, not yet run on this phone |
| gpu | no | Adreno 618 (a6xx gen 1, GMU with HFI v1): hakios.adreno.c is the 642L's; firmware pulled (fw-gpu/), no driver yet |
| audio | no | the audio DSP (q6 ADSP, APR, WCD9375 codec) is not the SC7280's; firmware pulled (fw-audio/), no driver yet |
| battery | partly | the level and the charger from UEFI's Qualcomm charger drivers, read by the loader (battery=1); charging is the firmware's: written, not yet run |
| haptic | no |  |
| camera | no |  |
| wifi | no | WCN3990: its firmware runs on the modem (ath10k snoc), so the modem comes first; not the WCN6750 of hakios.wifi.c |
| bluetooth | no |  |
| cellular | no | soc_* not yet read from this phone's device tree (pull-soc.sh) |
| thermal | works | both TSENS v2 blocks, 25 of 32 sensors read, 43 to 51 C (thermal=1 thermal_table=2, 2 November 2026); the stop at 105 C not seen firing |

The state is what has been seen on this device, written down in `devices/surya/device.conf`; a part the table does not name has not been looked at.
