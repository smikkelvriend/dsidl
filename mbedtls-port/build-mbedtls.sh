#!/bin/sh
# Builds Mbed TLS for the Nintendo DS(i) ARM9 into ./mbedtls-nds
# Usage: sh mbedtls-port/build-mbedtls.sh   (from the project root)
set -e
VER=v3.6.4
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/mbedtls-src"
OUT="$ROOT/mbedtls-nds"
BLOCKSDS=${BLOCKSDS:-/opt/blocksds/core}
CC=${ARM_NONE_EABI_PATH:-/opt/wonderful/toolchain/gcc-arm-none-eabi/bin/}arm-none-eabi-gcc
AR=${ARM_NONE_EABI_PATH:-/opt/wonderful/toolchain/gcc-arm-none-eabi/bin/}arm-none-eabi-ar

[ -d "$SRC" ] || git clone --depth 1 -b $VER https://github.com/Mbed-TLS/mbedtls.git "$SRC"

rm -rf "$OUT" && mkdir -p "$OUT/lib" "$OUT/include" "$OUT/obj"
cp -r "$SRC/include/mbedtls" "$SRC/include/psa" "$OUT/include/"
cp "$ROOT/mbedtls-port/nds_mbedtls_config.h" "$OUT/include/"

export BLOCKSDS
for f in "$SRC"/library/*.c; do
  "$CC" -mthumb -mcpu=arm946e-s+nofp -specs="$BLOCKSDS/sys/crts/ds_arm9.specs" \
    -O2 -ffunction-sections -fdata-sections -D__NDS__ -D__BLOCKSDS__ -DARM9 \
    -I"$BLOCKSDS/libs/libnds/include" -I"$BLOCKSDS/libs/dswifi/include" \
    -I"$SRC/include" -I"$SRC/library" -I"$OUT/include" \
    -DMBEDTLS_USER_CONFIG_FILE='"nds_mbedtls_config.h"' \
    -c "$f" -o "$OUT/obj/$(basename "$f" .c).o"
done
"$AR" rcs "$OUT/lib/libmbedtls_nds.a" "$OUT"/obj/*.o
rm -rf "$OUT/obj"
echo "Mbed TLS built in $OUT"
