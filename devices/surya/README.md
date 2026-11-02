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

## What else is on, and what is not yet

On with this configuration, each written from this SoC's own facts and not yet run on the phone:

| Part | How | Boot-arguments |
|---|---|---|
| processor clocks | both groups (cores 0-5, 6-7) to the top row of their table, through the OSM hardware the firmware switched on | `cpufreq=1 cpufreq_osm=1` |
| memory bandwidth | the BCM vote that comes with cpufreq, its addresses from this SoC's command database | `soc_cmd_db=0x85f20000` (`ddr=0`: no vote) |
| temperature | both TSENS v2 blocks, every sensor by number, the run stopped at 105 C | `thermal=1 thermal_table=2` |
| chip identity | SMEM item 137 | `socinfo=1 soc_smem=0x86000000` |
| PMIC inventory | every block of the PMICs, read only | `pmic=1` |
| battery | the level and the charger from UEFI's charger drivers, read by the loader | `battery=1` (`battery_stop=15` to end a run before it is flat) |

Not yet, and why each needs the phone at hand, a step at a time, rather than code written blind:

- **GPU (Adreno 618)**: `hakios.adreno.c` wakes a 642L in 22 steps, with the 650 family's GMU, its HFI version and its
  RPMh votes; the 618 is the first generation of the a6xx, with another GMU interface and other registers. A wrong write in
  that sequence resets the phone (the driver's head says how it was found out on redwood). The way: a `ADRENO618` driver of
  this device, step by step as the 642L's was, from Linux's `a6xx_gmu.c` / `a6xx_gpu.c` (the a618 paths) and this phone's
  device tree; the firmware is pulled already (`fw-gpu/`: `a630_sqe.fw`, `a618_gmu.bin`, `a615_zap.*`). Until then the
  screen is drawn by the CPU, and hakios-metal (built for the 642L) does not run here.
- **Audio**: the audio DSP runs and is linked (`adsp_step=11 adsp_sm7150=1`: TrustZone starts the relocatable image at
  0x95700000, GLINK with our queue (item 479) made by us, QRTR node 5 with 10 services, the APR audio services up). What is
  left is the speakers' path, from surya's device tree:
  - the amplifiers: two TI TAS256x on I2C (`i2c@a8c000`, 0x4c right and 0x4d left; reset GPIO 8 of one GPIO controller and
    91 of the TLMM, interrupts on TLMM 58 and 30), in place of redwood's Awinic AW88261 (`hakios.speaker.c`)
  - the interface: TERTIARY MI2S (`qcom,msm-dai-q6-mi2s-tert`, one RX line), AFE port 0x1004, pins GPIO 53 (bit clock),
    54 (frame sync), 55 and 56 (data), function ter_mi2s, 8 mA; in place of redwood's PRI_TDM_RX_0 (`hakios.sound.c`)
  - the DSP reads our samples through the SMMU as stream 0x1b21 (qcom,msm-audio-ion, sid mask 0xf), not redwood's 0x1801
  - the codec WCD9375 (bolero, SoundWire) is for the earpiece, headset and microphones, not the loudspeakers
  - Android's mixer paths (`/vendor/etc/mixer_paths_wcd9375.xml`, the card `sm6150-wcd9375-snd-card`): the path "speaker" is
    TERT_MI2S_RX, two channels, S24_LE at 48 kHz; on the amplifiers "TAS256X RX MODE" Speaker, "ASI1 SEL" Left and Right,
    "ASI Left/Right Switch" 1, "PLAYBACK VOLUME" 55 (52 for the factory test of one side), "Profile id" 0; the amplifiers'
    voltage and current sense come back on TERT_MI2S_TX ("tas-iv-capture"). audio_platform_info.xml: the speaker is ACDB
    device 15 (124 protected), 24 bits wide
- **Wi-Fi**: this phone's WCN3990 runs its firmware on the modem (Linux: ath10k over SNOC, the modem's WLAN service), so the
  modem must run first; `hakios.wifi.c` drives a WCN6750 with its own DSP. The modem's image is pulled (`fw-modem/`);
  `tools/soc-args.py` on the phone's own device tree (`adb pull /sys/firmware/fdt`) gives the `soc_*` the modem driver wants.

Check with `tools/hakios-device.py check surya`.
