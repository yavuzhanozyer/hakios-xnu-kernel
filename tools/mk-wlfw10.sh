#!/bin/bash
# Give the kernel's build ath10k's descriptions of the messages a WCN3990's firmware speaks (its WLFW service, on the
# modem of the SDM845 and SM7150 phones), for devices/surya/drivers/wifi/hakios.wcn3990.c: the structures of ath10k's
# qmi_wlfw_v01.h and the tables of its qmi_wlfw_v01.c, as they are, written as one header into the kernel's build
# directory, where the compiler finds it and git does not (ath10k's text is ISC and is not kept in the repository).
# Without that header hakios.wcn3990.c compiles to a line that says so.
#
# The two files come from Linux's mirror on GitHub at a pinned release (ATH10K_REF) and are kept in
# xnu-deps/build/ath10k; tools/make-kernel.sh runs this when the .config builds CONFIG_HAKIOS_WCN3990.
#
# usage: tools/mk-wlfw10.sh
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
REF=${ATH10K_REF:-v6.6}
SRC=$ROOT/xnu-deps/build/ath10k/$REF
OUT=$ROOT/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_${MACHINE:-BCM2837}/bsd/DEVELOPMENT
mkdir -p "$SRC" "$OUT"
for f in qmi_wlfw_v01.h qmi_wlfw_v01.c; do
	if [ ! -s "$SRC/$f" ]; then
		curl -fsSL -o "$SRC/$f.part" "https://raw.githubusercontent.com/torvalds/linux/$REF/drivers/net/wireless/ath/ath10k/$f" ||
			{ echo "mk-wlfw10.sh: ath10k's $f ($REF) could not be fetched from GitHub" >&2; rm -f "$SRC/$f.part"; exit 1; }
		mv "$SRC/$f.part" "$SRC/$f"
	fi
done
python3 - "$SRC" "$REF" > "$OUT/hakios.wlfw_ath10k.h.new" <<'PY'
import re
import sys

src, ref = sys.argv[1], sys.argv[2]
h = open(src + '/qmi_wlfw_v01.h').read()
c = open(src + '/qmi_wlfw_v01.c').read()
print("/* ath10k's qmi_wlfw_v01.h and the tables of its qmi_wlfw_v01.c (Linux %s, ISC), put together by" % ref)
print(' * tools/mk-wlfw10.sh: not to be edited, not to be kept. */')
print('''#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
''')
# the header without its guard's end, its includes and its extern declarations of the tables (they are defined below)
h = re.sub(r'^#include .*\n', '', h, flags=re.M)
h = re.sub(r'^extern const struct qmi_elem_info \w+\[\];\n', '', h, flags=re.M)
print(h)
# every table, in the file's order (a table is used by the ones after it), each of them static
tables = re.findall(r'^(?:static )?const struct qmi_elem_info \w+\[\] = \{.*?\n\};\n', c, re.S | re.M)
print('\n'.join('static ' + t if not t.startswith('static ') else t for t in tables))
PY
cmp -s "$OUT/hakios.wlfw_ath10k.h.new" "$OUT/hakios.wlfw_ath10k.h" 2>/dev/null && rm "$OUT/hakios.wlfw_ath10k.h.new" ||
	mv "$OUT/hakios.wlfw_ath10k.h.new" "$OUT/hakios.wlfw_ath10k.h"
echo "mk-wlfw10.sh: $OUT/hakios.wlfw_ath10k.h ($(grep -c 'qmi_elem_info .*\[\] = {' "$OUT/hakios.wlfw_ath10k.h") tables, ath10k $REF)"
