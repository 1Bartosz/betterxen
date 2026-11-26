#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds bx_shader_bridge.dll (this crate for x86_64-pc-windows-gnu) and the guest check
# check_bridge.exe into one directory (default: <repo>/target/bx-shader-bridge), then checks
# that the DLL exports exactly the bx_shader_bridge.h entry points and needs no MinGW runtime
# DLL. Run the check under CrossOver afterwards, for example:
#   target/debug/bx-cli run --prefix /tmp/bx-shader --shims target/bx-shader-bridge \
#       --exe target/bx-shader-bridge/check_bridge.exe
#
# Needs: rustup target x86_64-pc-windows-gnu, and x86_64-w64-mingw32-gcc / -objdump (mingw-w64).
set -eu

crate=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$crate/../../.." && pwd)
out=${1:-$root/target/bx-shader-bridge}
target=x86_64-pc-windows-gnu
cc=${BX_MINGW_CC:-x86_64-w64-mingw32-gcc}
objdump=${BX_MINGW_OBJDUMP:-x86_64-w64-mingw32-objdump}

for tool in cargo "$cc" "$objdump"; do
    command -v "$tool" >/dev/null 2>&1 || { echo "build.sh: $tool not found" >&2; exit 1; }
done
if command -v rustup >/dev/null 2>&1 && ! rustup target list --installed | grep -qx "$target"; then
    echo "build.sh: missing Rust target; run: rustup target add $target" >&2
    exit 1
fi

CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=$cc \
    cargo build --locked --release -p dxbc-accc --target "$target" --manifest-path "$root/Cargo.toml"
mkdir -p "$out"
cp "$root/target/$target/release/dxbc_accc.dll" "$out/bx_shader_bridge.dll"
dll=$out/bx_shader_bridge.dll

# Exactly the three frozen entry points, nothing else.
exports=$("$objdump" -p "$dll" | awk '/\[Ordinal\/Name Pointer\] Table/ {t=1; next} t && NF == 0 {exit} t && $1 ~ /^\[/ {print $NF}' | sort | tr '\n' ' ')
expected="bx_shader_free_buffer bx_shader_inspect bx_shader_prepare_for_d3dmetal "
if [ "$exports" != "$expected" ]; then
    echo "build.sh: unexpected exports: $exports" >&2
    exit 1
fi
# The shims' rule: no MinGW runtime DLL in a guest's import table.
if "$objdump" -p "$dll" | grep -iE "DLL Name: (libgcc|libstdc|libwinpthread)" >&2; then
    echo "build.sh: bx_shader_bridge.dll imports a MinGW runtime DLL" >&2
    exit 1
fi

"$cc" -std=c11 -O2 -Wall -Wextra -Werror -static \
    -I"$root/abi/include" -I"$crate/../d3dmetal-harness/probe/shaders" \
    -o "$out/check_bridge.exe" "$crate/guest/check_bridge.c"

echo "built $dll (exports: $exports)"
echo "built $out/check_bridge.exe"
