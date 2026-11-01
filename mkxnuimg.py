#!/usr/bin/env python3
"""
Pack a phone's UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`. The phone: -d NAME, else $DEVICE, else the
.config's CONFIG_DEVICE, else redwood.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see uefi/README.md)
or, when there is none, from devices/<name>/prebuilt/. Where the FD goes and how big it is are the
device's Resources/Configs/<name>.toml (Mu-Silicium's, or the device repository's uefi/files/).

usage: mkxnuimg.py [-d device] [-p payload.bin] [-o out.img]
"""
import argparse, gzip, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')
PAYLOAD_BASE, PAYLOAD_MAX = 0xF0000000, 0x04000000


def config_device():
    try:
        for line in open(os.path.join(HERE, '.config'), encoding='utf-8'):
            m = re.match(r'CONFIG_DEVICE="([a-z0-9_]+)"', line)
            if m:
                return m.group(1)
    except OSError:
        pass
    return None


ap = argparse.ArgumentParser()
ap.add_argument('-d', '--device', default=os.environ.get('DEVICE') or config_device() or 'redwood')
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output')
ap.add_argument('--mode', default='RELEASE')
args = ap.parse_args()
DEV = args.device
if not re.fullmatch(r'[a-z0-9_]+', DEV):
    sys.exit(f'mkxnuimg.py: {DEV!r} is not a device name')
args.output = args.output or os.path.join(HERE, f'xnu-{DEV}.img')
PREBUILT = os.path.join(HERE, 'devices', DEV, 'prebuilt')


def device_toml():
    """[uefi_fd] base, size and [boot_image] header_version of the device's Mu-Silicium configuration."""
    for path in (os.path.join(MU, 'Resources', 'Configs', f'{DEV}.toml'),
                 os.path.join(HERE, 'devices', DEV, 'uefi', 'files', 'Resources', 'Configs', f'{DEV}.toml')):
        if os.path.isfile(path):
            text = open(path, encoding='utf-8').read()
            fd = re.search(r'\[uefi_fd\][^\[]*?base\s*=\s*(0x[0-9A-Fa-f]+)[^\[]*?size\s*=\s*(0x[0-9A-Fa-f]+)', text)
            header = re.search(r'\[boot_image\][^\[]*?header_version\s*=\s*(\d+)', text)
            if fd:
                return int(fd.group(1), 16), int(fd.group(2), 16), int(header.group(1)) if header else 1
    if DEV == 'redwood':                # what the redwood port was made with, before its .toml was looked for
        return 0x9FD00000, 0x200000, 1
    sys.exit(f'mkxnuimg.py: no Resources/Configs/{DEV}.toml in Mu-Silicium/ or devices/{DEV}/uefi/files/ '
             f'(uefi/apply.sh {DEV} makes the tree)')


FD_BASE, FD_SIZE, HEADER_VERSION = device_toml()
print(f'mkxnuimg.py: {DEV}: UEFI FD at {FD_BASE:#x}, {FD_SIZE:#x} bytes; boot image header {HEADER_VERSION}')

fd_path = os.path.join(MU, f'Build/{DEV}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, f'Resources/DTBs/{DEV}.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
else:
    parts = {'shim': os.path.join(PREBUILT, 'BootShim.bin'), 'fd': os.path.join(PREBUILT, 'SILICIUM_UEFI.fd'),
             'dtb': os.path.join(PREBUILT, 'device-tree.dtb'), 'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')}
for name, path in parts.items():
    if not os.path.isfile(path):
        hint = ''
        if name == 'fd':
            hint = ("\n  The UEFI image is not in the repository: it carries the phone vendor's own drivers, which are not\n"
                    "  redistributed here. Build it from your own phone's firmware: uefi/README.md")
        sys.exit(f'mkxnuimg.py: {path} not found{hint}')
shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
assert len(fd) == FD_SIZE, hex(len(fd))

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

dtb = open(parts['dtb'], 'rb').read()
kernel = args.output + '.kernel~'
open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
try:
    subprocess.check_call([sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
                           '--header_version', str(HEADER_VERSION), '-o', args.output])
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
