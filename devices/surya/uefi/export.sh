#!/bin/bash
# Save the surya platform from Mu-Silicium/ into files/ here (uefi/export.sh runs this; HakiOS's own changes are uefi/'s).
# surya.toml and surya.dtb are Mu-Silicium's own at the commit in uefi/BASE and are not copied.
set -e
cd "$(dirname "$(readlink -f "$0")")"
MU=${MU:-../../../Mu-Silicium}
rm -rf files/Platforms
mkdir -p files/Platforms/Xiaomi files/Binaries/surya
cp -r $MU/Platforms/Xiaomi/suryaPkg files/Platforms/Xiaomi/
find files -name __pycache__ -prune -exec rm -rf {} +
echo "devices/surya/uefi/export.sh: $(find files -type f | wc -l) files"
