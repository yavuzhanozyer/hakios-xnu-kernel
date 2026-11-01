#!/bin/bash
# Pull the POCO X3 NFC's own firmware out of its Android into the fw-*/ directories at the top of the repository (never
# committed: it is the vendor's). The phone is on adb, in Android (or in a recovery with /vendor mounted).
#
#   fw-touch/novatek_ts_fw.bin   the NT36672C's firmware for the panel this phone has: the controller keeps none of its own
#                                and is sent it after every reset (devices/surya/drivers/input/hakios.nt36672c.c);
#                                userland/build.sh puts it into the image as fw/novatek_ts_fw.bin
#
# surya was made with two panels, and the touch firmware is the panel's: Tianma (novatek_ts_tianma_fw.bin) or Huaxing
# (novatek_ts_huaxing_fw.bin). The panel is named on the kernel's command line (msm_drm.dsi_display0=...); Xiaomi's driver
# chooses by it. This script does the same, and keeps both when it cannot tell (PANEL=tianma or PANEL=huaxing then says).
set -e
cd "$(dirname "$0")/../.."
adb get-state >/dev/null
mkdir -p fw-touch
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
