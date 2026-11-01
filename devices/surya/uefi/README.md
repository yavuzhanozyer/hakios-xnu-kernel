# UEFI for surya

The POCO X3 NFC's platform for HakiOS's UEFI (`uefi/` of the common repository: Mu-Silicium with HakiOS's changes and banner).
Mu-Silicium has the phone already (`Platforms/Xiaomi/suryaPkg`, the `MooreaPkg` SoC package, SM7150-AC); this is that
platform at the commit in `uefi/BASE`, with one change:

- `Library/MemoryMapLib/MemoryMapLib.c`: **"XNU Payload"**, 64 MB at `0xF0000000` kept out of the RAM UEFI hands out, where
  HakiOS's `BootShim.S` copies the `XNUP` container and `xnuloader` finds the kernel (the same region redwoodPkg has).

`Resources/Configs/surya.toml` and `Resources/DTBs/surya.dtb` are Mu-Silicium's own at that commit and are not copied here.
The framebuffer is UEFI's, 1080 x 2400 at `0x9C000000` ("Display Reserved"); the loader knows the SoC by that address
(`platform sm7150` in its log).

- `Binaries/surya`: the vendor's UEFI drivers, **not tracked**: make this directory yourself from your own phone's `xbl`
  (`uefi/README.md` says how), or use Mu-Silicium's binaries for surya where you may.

```sh
devices/surya/uefi/apply.sh       # = uefi/apply.sh surya
cd Mu-Silicium && python3 build_uefi.py -d surya -r RELEASE
```

Status: the platform is Mu-Silicium's, which boots on this phone; the payload region and the XNU loader on it have not been
run on the phone yet.
