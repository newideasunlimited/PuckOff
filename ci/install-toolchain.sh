#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

if [[ $# != 1 || "$(uname -s)" != Linux || "$(uname -m)" != x86_64 ]]; then
  echo "Usage: bash ci/install-toolchain.sh NEW_INSTALL_DIRECTORY (Linux x86_64)" >&2
  exit 2
fi
if [[ -e "$1" ]]; then
  echo "Refusing to overwrite existing toolchain directory: $1" >&2
  exit 2
fi
mkdir -p "$1"
cd "$1"

base=https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1
curl --fail --location --retry 3 --silent --show-error \
  "$base/zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz" -o sdk.tar.xz
curl --fail --location --retry 3 --silent --show-error \
  "$base/toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz" -o arm.tar.xz
echo "ca9bc0ff66fafca1dac9d592a36d953cf16d096a9d09b1c0357f021cf9f6a7eb  sdk.tar.xz" | sha256sum -c -
echo "21b85981cb5a1818d9bc53d82af80f208946ec038b982ff1907287572ed3a634  arm.tar.xz" | sha256sum -c -
tar -xf sdk.tar.xz
mkdir -p zephyr-sdk-1.0.1/gnu
tar -xf arm.tar.xz -C zephyr-sdk-1.0.1/gnu
zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc --version
