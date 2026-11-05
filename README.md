# HakiOS on the POCO X3 NFC (`surya`)

English · [Türkçe](#türkçe)

**Apple's open-source Darwin kernel (XNU, xnu-8796.121.2) on the POCO X3 NFC** (Xiaomi `surya`, Qualcomm SM7150-AC
"Snapdragon 732G"): the [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) kernel, loader and drivers, with this phone's own
device layer in [`devices/surya`](devices/surya). A research bench, not a ROM: it runs from RAM, booted with `fastboot boot`, and a reboot
gives the phone back to its own Android. Nothing is flashed.

This is a fork of [oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel), whose reference phone is the POCO X5
Pro 5G (`redwood`, SC7280); it follows upstream's `main` and adds the SM7150 family and `surya`. Upstream's documentation,
[docs/](docs/index.md), holds for everything common.

## What works on surya

As [`devices/surya/device.conf`](devices/surya/device.conf) records it, each row seen on the phone under HakiOS unless it says otherwise
(the dates are the first time). The boot-arguments in brackets are what turns a part on; `make surya_defconfig` sets the ones that work.

| | POCO X3 NFC (`surya`) |
|---|---|
| Boot to the lock screen and the home screen, the stock apps | yes: Darwin 16.5 user space from RAM, drawn by the CPU on UEFI's framebuffer (1080 x 2400) |
| Touch, multitouch | yes: Novatek NT36672C on SPI, its firmware sent over SPI (`touchscreen=1`) |
| Buttons: power, volume up and down | yes: PM6150 / PM6150L (`buttons_pon=0x008 buttons_pon_gen=2 buttons_volup=0x4c1`) |
| All 8 CPU cores and their clocks | yes: the kernel on core 7, the small cores at 1804 MHz, the big ones at 2304 MHz through OSM (`cpus=8 bootcpu=7 cpufreq=1 cpufreq_osm=1`) |
| Temperature sensors | yes: both TSENS blocks, 25 of 32 sensors (`thermal=1 thermal_table=2`) |
| Sound: both speakers, in stereo, iOS's volume | yes: the audio DSP, TERT MI2S, two TAS2562 amplifiers (`adsp=1 adsp_sm7150=1 sound=4`); no speaker protection, 12 dB under the amplifiers' full level |
| Ringtones and alert tones | written (the system image keeps `Library/Ringtones`), not yet heard |
| The battery's level | yes: from UEFI's charger drivers (`battery=1`); charging under this kernel not seen |
| Storage | partly: read through the loader (`ufs=2`), as the modem's file systems; writing not tried |
| Modem | partly: up, linked (GLINK, QRTR), 39 services, its files served (`modem_sm7150=1 modem_step=10 modem_keep=1 ufs=2`); no SIM or network asked yet |
| Wi-Fi | partly: the WCN3990's firmware ready and in mission mode on the modem (`wcn3990=2`); its radio driver (ath10k's copy engines, HTC, WMI, HTT) is being written, so no scan and no connection yet |
| GPU | partly: the Adreno 618's block alive (steps 1-6, `adreno=6 adreno_a618=1`); the GMU's firmware, HFI and the 3D core not yet, so no OpenGL ES and no Metal |
| Microphones, cameras, flashlight, vibration, Bluetooth, cellular data and calls | no |
| Apple's services (activation, Apple ID, iCloud, the App Store) | not supported and not a goal |

The details, part by part, and what each is waiting for: [devices/surya/README.md](devices/surya/README.md).

## Quick start

On a Linux machine with the build set up ([Getting started](docs/getting-started/index.md)), the phone in fastboot, its bootloader
unlocked, and its own firmware pulled once from its Android or a recovery ([`devices/surya/pull-firmware.sh`](devices/surya/pull-firmware.sh)):

```
make surya_defconfig
make kernel
MODEM=1 SYSTEM=1 REST=1 userland/build.sh
./xl reboot
make phone
```

`make phone` uploads the loader and the system image, runs the kernel with what `.config` turns on, and leaves the phone in the loader
for the next run. The log of each run is in `logs/`.

## What you bring, and what is never here

Nothing of Apple's and no vendor's firmware is in this repository. The Darwin user space you lay out yourself from an IPSW you get
yourself (`sysimage/unpack.sh`), and the phone's firmware (touch, GPU, audio DSP, modem, Wi-Fi) comes off your own phone; nothing is
downloaded for you, decrypted or circumvented. Two sets of Linux driver tables (ath10k's, for the WCN3990) are fetched from Linux's
sources when the kernel is built and are not kept here.
[What is in the repository, and what never is](docs/index.md#what-is-in-the-repository-and-what-never-is).

## Repositories

| Repository | Contents |
|---|---|
| [yavuzhanozyer/hakios-xnu-kernel](https://github.com/yavuzhanozyer/hakios-xnu-kernel) | this fork: upstream's kernel port, loader, drivers, user space and tools, with `devices/surya` and the SM7150 layer |
| [yavuzhanozyer/hakios-uefi](https://github.com/yavuzhanozyer/hakios-uefi) | the firmware that starts the loader (Mu-Silicium); surya's platform package is in [`devices/surya/uefi`](devices/surya/uefi) |
| [oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) | upstream: the common kernel and its reference phone, the POCO X5 Pro 5G |
| [`hakios-metal/`](hakios-metal) | Metal's command model on the Adreno, in this tree (not yet running on surya's GPU) |

## License and legal

The XNU patch and the files derived from it are under the Apple Public Source License 2.0 ([LICENSE.APSL](LICENSE.APSL)), the rest under
MIT ([LICENSE](LICENSE)), as the file headers say. Third parties: [NOTICE](NOTICE),
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md), [COMPLIANCE.md](COMPLIANCE.md). What is and is not distributed: [LEGAL.md](LEGAL.md).
None of this is legal advice.

HakiOS is an independent research project. It is **not affiliated with or endorsed by Apple**, nor by Xiaomi; "Darwin", "iPhone", "XNU",
"Apple", "POCO" and "Xiaomi" are named only to describe what the code works with.

## Credits

HakiOS is the work of its team (upstream's [README](https://github.com/oosexgang/hakios-xnu-kernel#team)): @oodze, @netusd, @wannqn,
@hxfuxyyXD, Rhaenja Hun, and Sicilium for the sources. The surya touch driver comes from the earlier ios-sweet port; the SM7150 port is
@yavuzhanozyer's.

---

## Türkçe

**POCO X3 NFC (`surya`, SM7150-AC "Snapdragon 732G") üzerinde Apple'ın açık kaynak Darwin çekirdeği (XNU).** Bu depo,
[oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) deposunun bir çatalıdır; upstream'in referans telefonu
POCO X5 Pro 5G'dir (`redwood`). Bu çatal upstream'i takip eder ve SM7150 ailesini ve `surya`yı ekler. Telefona hiçbir şey yazılmaz:
sistem RAM'den çalışır (`fastboot boot`) ve yeniden başlatınca telefon kendi Android'ine döner.

**Çalışanlar:** kilit ekranı ve ana ekran, stok uygulamalar (CPU ile çizim), dokunmatik, tuşlar, 8 çekirdek ve frekansları, sıcaklık
sensörleri, iki hoparlör stereo ve iOS'un ses seviyesi, batarya seviyesi.

**Kısmen:** depolama (sadece okuma), modem (açık, 39 servis; SIM ve şebeke henüz yok), Wi-Fi (firmware hazır ve "mission" modunda;
radyo sürücüsü yazılıyor, tarama ve bağlanma henüz yok), GPU (blok canlı, 1-6. adımlar; OpenGL ES ve Metal henüz yok), zil sesleri
(eklendi, henüz denenmedi).

**Çalışmayanlar:** mikrofonlar, kameralar, flaş, titreşim, Bluetooth, mobil veri ve arama; Apple servisleri hedef değil.

Hızlı başlangıç için yukarıdaki komutlar; ayrıntılar [devices/surya/README.md](devices/surya/README.md) dosyasında.
