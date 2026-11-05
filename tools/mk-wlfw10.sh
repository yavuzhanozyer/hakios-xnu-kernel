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
for f in qmi_wlfw_v01.h qmi_wlfw_v01.c snoc.c ce.h htc.h qmi.h; do
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

# The configuration the driver sends with the mode (ath10k_snoc_wlan_enable()): snoc.c's three tables for the WCN3990 --
# the copy engines' pipes, the map of HTC services to them, the shadow registers -- with the definitions they are
# written in (ce.h, htc.h, qmi.h).
snoc = open(src + '/snoc.c').read()
ce = open(src + '/ce.h').read()
htc = open(src + '/htc.h').read()
qmih = open(src + '/qmi.h').read()
print("/* as Linux has it on a little-endian machine */")
print('#ifndef BIT\n#define BIT(n) (1U << (n))\n#endif')
print('#undef __cpu_to_le16\n#undef __cpu_to_le32\n#define __cpu_to_le16(x) (x)\n#define __cpu_to_le32(x) (x)')
for name in ('CE_ATTR_DIS_INTR', 'CE_ATTR_FLAGS'):
    print(re.search(r'^#define %s\b.*$' % name, ce, re.M).group(0))
print(''.join(re.findall(r'^#define PIPEDIR_\w+.*\n', ce, re.M)))
for name in ('ce_pipe_config', 'ce_service_to_pipe'):
    print(re.search(r'struct %s \{.*?\n\};\n' % name, ce, re.S).group(0))
print(re.search(r'struct ath10k_shadow_reg_cfg \{.*?\n\};\n', qmih, re.S).group(0))
print(re.search(r'enum ath10k_htc_svc_gid \{.*?\n\};\n', htc, re.S).group(0))
print(re.search(r'#define SVC\(group, idx\).*?\n.*?\n', htc).group(0))
print(re.search(r'enum ath10k_htc_svc_id \{.*?\n\};\n', htc, re.S).group(0))
print('#undef SVC')
print(''.join(re.findall(r'^#define WCN3990_\w+_WR_IDX_OFFSET.*\n', snoc, re.M)))
for kind, name in (('ath10k_shadow_reg_cfg', 'target_shadow_reg_cfg_map'), ('ce_pipe_config', 'target_ce_config_wlan'),
                   ('ce_service_to_pipe', 'target_service_to_ce_map_wlan')):
    table = re.search(r'static struct %s %s\[\] = \{.*?\n\};\n' % (kind, name), snoc, re.S).group(0)
    print(table.replace('static struct', 'static const struct', 1))
PY
cmp -s "$OUT/hakios.wlfw_ath10k.h.new" "$OUT/hakios.wlfw_ath10k.h" 2>/dev/null && rm "$OUT/hakios.wlfw_ath10k.h.new" ||
	mv "$OUT/hakios.wlfw_ath10k.h.new" "$OUT/hakios.wlfw_ath10k.h"
echo "mk-wlfw10.sh: $OUT/hakios.wlfw_ath10k.h ($(grep -c 'qmi_elem_info .*\[\] = {' "$OUT/hakios.wlfw_ath10k.h") tables, ath10k $REF)"
