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
| display | partly | UEFI's framebuffer (1080 x 2400 at 0x9c000000, Mu-Silicium's suryaPkg) through the loader (platform sm7150); drawn by the CPU |
| touch | partly | Novatek NT36672C on SPI, firmware sent over SPI (CONFIG_HAKIOS_NT36672C, touchscreen=1): the ios-sweet driver that works on this phone, moved here; not yet run under HakiOS |
| buttons | partly | power and volume through the PM6150 / PM6150L (CONFIG_HAKIOS_BUTTONS, buttons_pon=0x008 buttons_pon_gen=2 buttons_volup=0x4c1): the addresses ios-sweet reads them at; not yet run under HakiOS |
| cpu | partly | 8 cores, the kernel on core 7, an A76 (cpus=8 bootcpu=7): all eight ran under ios-sweet; the processor clocks (OSM) are not driven |
| storage | partly | UFS read through the loader (ufs=2, UEFI's block devices): the loader's, not yet run on this phone |
| gpu | no | Adreno 618: hakios.adreno.c is the 642L's |
| audio | no | the audio DSP's addresses are the SC7280's |
| battery | no | no charger or fuel gauge driver for the PM6150 / PM7150B |
| haptic | no |  |
| camera | no |  |
| wifi | no | WCN3990-family radio (not the WCN6750 of hakios.wifi.c) |
| bluetooth | no |  |
| cellular | no | soc_* not yet read from this phone's device tree (pull-soc.sh) |
| thermal | no | TSENS at 0xc222000 / 0xc223000, not the SC7280's |

The state is what has been seen on this device, written down in `devices/surya/device.conf`; a part the table does not name has not been looked at.
