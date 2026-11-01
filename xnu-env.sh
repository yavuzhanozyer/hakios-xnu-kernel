# source this (from bash) before building XNU on Linux
# Everything is found relative to this file. THEOS (default ~/theos) provides the cctools and the
# Darwin SDK; XNU_SRC is the kernel tree (xnu/apply.sh creates it).
DARWIN_REDWOOD=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export DARWIN_REDWOOD
export XNU_HOST_OS=$(uname)
# The C locale for everything the build runs. In a Turkish locale (tr_TR) sed's [A-Za-z] does not take a capital I, and
# XNU's assym.s rule (osfmk/conf/Makefile.template) wrote "#define _NUM" for INTSTACK_SIZE, "PAGE_MAX_S_NUM" for
# PAGE_MAX_SIZE ...: locore.s, data.s and start.s then did not assemble. Seen on Arch Linux with tr_TR.UTF-8.
export LC_ALL=C
# On macOS there is no theos: xnu-deps/setup.sh lays out xnu-deps/theos-mac like one (links to the
# cctools of Xcode and to the iPhoneOS 16.5 SDK of github.com/theos/sdks).
if [ "$XNU_HOST_OS" = Darwin ]; then
	export THEOS=${THEOS:-$DARWIN_REDWOOD/xnu-deps/theos-mac}
else
	export THEOS=${THEOS:-$HOME/theos}
fi
export XNU_SRC=${XNU_SRC:-$DARWIN_REDWOOD/xnu-src/xnu-8796.121.2}
export XNUDEPS=$DARWIN_REDWOOD/xnu-deps
export PATH=$XNUDEPS/host/bin:$PATH
export XNU_SDK_PATH=$XNUDEPS/sdk/iPhoneOS16.5.xnu.sdk
export XNU_SDK_VERSION=16.5
export XNU_PLATFORM_PATH=$XNUDEPS/sdk/Platforms/iPhoneOS.platform
