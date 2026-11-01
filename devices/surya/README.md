# POCO X3 NFC (surya)

A HakiOS device: the POCO X3 NFC (Xiaomi `surya`, M2007J20CG), Qualcomm SM7150-AC (Snapdragon 732G). What is true of this
phone and of no other is here; the kernel, the loader, the drivers and the tools are the common repository's; the SM7150
family's facts are in `soc/`, carried with the device. It is kept in the common repository's tree (`devices/manifest.conf`:
`path devices/surya`) until it has a repository of its own (`tools/hakios-device.py split surya DIR`).

It comes from the [ios-sweet](https://github.com/yavuzhanozyer/ios-sweet) port (iOS 16 on the Redmi Note 10 Pro, the same
SoC), on which this phone's touch screen and keys were made to work (its patch `0082-surya-nvt-touch.patch`). What was learnt
there is moved here, onto HakiOS's UEFI start, loader, drivers and user space.

| File | What |
|---|---|
| `device.conf` | what the device is, and how far each part of it works |
| `configs/surya_defconfig` | the common kernel, the SoC's fragment, this device's: `make surya_defconfig` |
| `configs/surya.config` | this device's own: the touch screen, the buttons pointed at its PMICs, every other phone driver left out |
| `soc/` | the SM7150 family: `soc.conf`, `configs/sm7150.config` (eight cores, what is and is not the SC7280's) |
| `drivers/input/hakios.nt36672c.c` | the touch screen: Novatek NT36672C on SPI (QUPv3 SE0), its firmware sent over SPI |
| `pull-firmware.sh` | the touch firmware of the panel this phone has, from its Android, into `fw-touch/` |
| `uefi/` | its UEFI platform: Mu-Silicium's suryaPkg with HakiOS's payload region |

## Running it

```sh
devices/surya/pull-firmware.sh              # the phone in Android, on adb: fw-touch/novatek_ts_fw.bin
devices/surya/uefi/apply.sh                 # Mu-Silicium/ with HakiOS's changes and this platform; then build it:
(cd Mu-Silicium && python3 build_uefi.py -d surya -r RELEASE)   # with the vendor's drivers in Binaries/surya (uefi/README.md)
make surya_defconfig
make kernel && make loader
make phone SECS=120                         # mkxnuimg.py and xl take the device from the .config (CONFIG_DEVICE="surya")
```

What the log should say: `platform sm7150` from the loader, then `hakios-kernel: N drivers built in (... buttons nt36672c ...)`,
`buttons: power-on block 0x8 (generation 2 ...)`, `nt36672c: chip matched ...` and the
firmware's `reset state 0xa?`.

## What differs from redwood, and where

| Part | redwood (SM7325) | surya (SM7150) | In HakiOS |
|---|---|---|---|
| UEFI framebuffer | 0xE1000000 | 0x9C000000, 1080 x 2400 | `xnuloader/loader.c`: `platform_sm7150` |
| UEFI FD | 0x9FD00000, 2 MB | 0x9FC00000, 3 MB | Mu-Silicium's `surya.toml`, read by `mkxnuimg.py -d surya` |
| cores | 4 A55 + 4 A78 | 6 A55 + 2 A76 | `small_cores = 0x3f`; `cpus=8 bootcpu=7` |
| touch | Goodix GT9916 on SPI | Novatek NT36672C on SPI (SE0, GPIO49-52, reset 8, irq 9) | `drivers/input/hakios.nt36672c.c` |
| power, volume down | PMK8350 PON (split, 0x1300) | PM6150 PON (0x800, bits 0 and 1) | `buttons_pon=0x008 buttons_pon_gen=2` |
| volume up | PM7325 GPIO6 (0x18d) | PM6150L GPIO2 (0x4c1) | `buttons_volup=0x4c1` |

## Next

In the order of `docs/porting/index.md`: `SOCINFO` (`soc_smem=0x86000000`), the temperature sensors and the processor clocks
of this SoC, then UFS, then the Adreno 618. `tools/soc-args.py` on the phone's own device tree (`adb pull /sys/firmware/fdt`) gives the `soc_*` the modem wants.

Check with `tools/hakios-device.py check surya`.
