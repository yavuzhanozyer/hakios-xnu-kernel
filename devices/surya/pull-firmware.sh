#!/bin/bash
# Pull the POCO X3 NFC's own firmware out of its Android into the fw-*/ directories at the top of the repository (never
# committed: it is the vendor's). The phone is on adb, in Android, or in a recovery with root adb (OrangeFox: mount /vendor
# there first; the firmware partition this script mounts itself).
#
#   fw-touch/novatek_ts_fw.bin   the NT36672C's firmware for the panel this phone has: the controller keeps none of its own
#                                and is sent it after every reset (devices/surya/drivers/input/hakios.nt36672c.c);
#                                userland/build.sh puts it into the image as fw/novatek_ts_fw.bin
#   fw-gpu/, fw-audio/, fw-modem/  the Adreno 618's microcode and zap shader, the audio DSP's and the modem's images (the
#                                Wi-Fi of this phone, a WCN3990, runs its firmware on the modem): pulled now so that they are
#                                at hand; no driver of this device uses them yet (README.md, "Next")
#
# surya was made with two panels, and the touch firmware is the panel's: Tianma (novatek_ts_tianma_fw.bin) or Huaxing
# (novatek_ts_huaxing_fw.bin). The panel is named on the kernel's command line (msm_drm.dsi_display0=...); Xiaomi's driver
# chooses by it. This script does the same, and keeps both when it cannot tell (PANEL=tianma or PANEL=huaxing then says).
set -e
cd "$(dirname "$0")/../.."
adb get-state >/dev/null
mkdir -p fw-touch
pull() { # DIR SOURCE...  (missing files are told, not fatal)
	local dir=$1; shift
	mkdir -p "$dir"
	for f in "$@"; do adb pull "$f" "$dir/" >/dev/null 2>&1 || echo "pull-firmware.sh: $f: not on the phone"; done
	echo "pull-firmware.sh: $dir: $(ls -1 "$dir" | wc -l | tr -d ' ') files"
	rmdir "$dir" 2>/dev/null || true        # nothing pulled: no empty directory for userland/build.sh to trip on
}
pull fw-gpu /vendor/firmware/a630_sqe.fw /vendor/firmware/a618_gmu.bin /vendor/firmware/a630_gmu.bin $(for i in mdt b00 b01 b02; do echo /vendor/firmware/a615_zap.$i; done)
# The DSPs' images are on the partition "modem" (FAT, NON-HLOS), which Android mounts at /vendor/firmware_mnt and a
# recovery (OrangeFox) does not: there it is mounted here, read only, in a directory of its own, and unmounted again.
img=/vendor/firmware_mnt/image
mounted=
if ! adb shell "[ -f $img/adsp.mdt ]" 2>/dev/null; then
	part=$(adb shell 'for p in /dev/block/by-name/modem /dev/block/bootdevice/by-name/modem; do [ -e $p ] && { echo $p; break; }; done' | tr -d '\r')
	if [ -n "$part" ] && adb shell "mkdir -p /tmp/hakios-fw && mount -t vfat -o ro $part /tmp/hakios-fw" 2>/dev/null; then
		img=/tmp/hakios-fw/image
		mounted=1
		echo "pull-firmware.sh: the firmware partition ($part) mounted read only at /tmp/hakios-fw"
	else
		echo "pull-firmware.sh: no $img and the firmware partition could not be mounted (root adb, in a recovery?)"
	fi
fi
pull fw-audio $img/adsp.mdt $(for i in $(seq -w 0 40); do echo $img/adsp.b$i; done)
pull fw-modem $img/modem.mdt $(for i in $(seq -w 0 40); do echo $img/modem.b$i; done)
# (the Wi-Fi's board data, beside the modem's image: for the day the WCN3990 has a driver)
pull fw-wifi $img/bdwlan.bin $img/bdwlan.b0b $img/bdwlan.b0c $img/bdwlan.b0d $img/bdwlan.b0e $img/bdwlan.b0f
[ -z "$mounted" ] || adb shell "umount /tmp/hakios-fw; rmdir /tmp/hakios-fw" 2>/dev/null || true
for f in novatek_ts_tianma_fw.bin novatek_ts_huaxing_fw.bin; do
	adb pull "/vendor/firmware/$f" fw-touch/ >/dev/null 2>&1 || echo "pull-firmware.sh: /vendor/firmware/$f: not on the phone"
done
panel=${PANEL:-}
if [ -z "$panel" ]; then
	cmdline=$(adb shell cat /proc/cmdline 2>/dev/null | tr -d '\r' | tr 'A-Z' 'a-z')
	case "$cmdline" in
	*tianma*) panel=tianma ;;
	*huaxing*) panel=huaxing ;;
	esac
	echo "pull-firmware.sh: the panel on the command line: $(echo "$cmdline" | grep -o 'dsi_display0=[^ ]*' || echo '(not said)')"
fi
if [ -n "$panel" ] && [ -f "fw-touch/novatek_ts_${panel}_fw.bin" ]; then
	cp "fw-touch/novatek_ts_${panel}_fw.bin" fw-touch/novatek_ts_fw.bin
	echo "pull-firmware.sh: fw-touch/novatek_ts_fw.bin is the $panel panel's"
else
	echo "pull-firmware.sh: which panel this phone has is not known: run again with PANEL=tianma or PANEL=huaxing" >&2
	exit 1
fi
