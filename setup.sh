#!/usr/bin/env bash
# Fetches the two dependencies at the exact commits this firmware was built and
# tested against, then applies the local patches it needs. Safe to re-run.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p lib

PICO_SDK_COMMIT=bddd20f928b75f18a44c98df0bf4be0296828dd6
PICODVI_COMMIT=6ae9e211627937eff8c917ad8685a35376e78c9e

fetch () {   # name url commit [--recursive]
	local name=$1 url=$2 commit=$3
	if [ -d "lib/$name/.git" ]; then
		echo "==> lib/$name already present"
	else
		echo "==> cloning $name"
		git clone "$url" "lib/$name"
	fi
	git -C "lib/$name" fetch --quiet origin "$commit" 2>/dev/null || true
	git -C "lib/$name" checkout --quiet "$commit"
	if [ "${4:-}" = "--recursive" ]; then
		git -C "lib/$name" submodule update --init --recursive
	fi
}

fetch pico-sdk      https://github.com/raspberrypi/pico-sdk.git "$PICO_SDK_COMMIT" --recursive
fetch PicoDVI-audio https://github.com/ikjordan/PicoDVI.git     "$PICODVI_COMMIT"

# --- the patches ------------------------------------------------------------
# Both are load-bearing. Without the TinyUSB one the chip hard-locks within
# seconds of a Playdate connecting, which looks exactly like a hardware fault.
# See patches/README.md for what each one does and why.
apply () {   # dir patchfile
	local dir=$1 patch=$2
	if git -C "$dir" apply --reverse --check "$PWD/$patch" 2>/dev/null; then
		echo "==> $(basename "$patch") already applied"
	else
		echo "==> applying $(basename "$patch")"
		git -C "$dir" apply "$PWD/$patch"
	fi
}

apply lib/pico-sdk/lib/tinyusb patches/tinyusb.patch
apply lib/PicoDVI-audio        patches/picodvi-audio.patch

echo
echo "Done. Now:"
echo "  cmake -S . -B build -DPICO_SDK_PATH=\$PWD/lib/pico-sdk"
echo "  cmake --build build -j8"
