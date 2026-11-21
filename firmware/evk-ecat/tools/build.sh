#!/usr/bin/env bash
set -euo pipefail
project="$(cd "$(dirname "$0")/.." && pwd)"
: "${GNURISCV_TOOLCHAIN_PATH:?Set the GCC 16 toolchain directory}"
: "${EVK_ILP32_RUNTIME:?Set xPack RV32 soft-float toolchain directory}"
build="${EVK_BUILD_DIR:-$project/build}"
cmake -S "$project" -B "$build" -GNinja \
    -DBOARD=hpm5e00evk -DHPM_BUILD_TYPE=flash_xip \
    -DRV_ARCH=rv32imac_zicsr_zifencei \
    "-DCMAKE_EXE_LINKER_FLAGS=-L$EVK_ILP32_RUNTIME/lib/gcc/riscv-none-elf/14.2.0 -L$EVK_ILP32_RUNTIME/riscv-none-elf/lib" \
    "$@"
cmake --build "$build"
python3 "$project/tools/export_firmware.py" \
    "$build" "$build/artifacts"
