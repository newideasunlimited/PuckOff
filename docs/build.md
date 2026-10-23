# Default receiver build and CI

## Pinned inputs

`west.yml` locks the default receiver's SDK source dependencies by full commit
ID. This is the subset used from nRF Connect SDK v3.4.1, including Nordic's
Zephyr revision `33fa6a7aac6a4401d16a67cb9f27a3483fa02dd6`.
`ci/requirements.txt` pins the Python/build packages. The compiler is Zephyr SDK
1.0.1; the Linux x86_64 SDK and ARM toolchain archives are SHA-256 checked
before extraction.

The manifest is for the default raw-radio/USB receiver, not a claim that every
optional SDK subsystem can build with this module subset.

## Fresh Linux x86_64 workspace

Install Git, Python 3.12 or newer with virtual-environment support, `curl`,
`xz`, `device-tree-compiler`, and `gperf`. Run these commands in a **new**
directory, not inside another SDK workspace:

```sh
mkdir puckoff-workspace
cd puckoff-workspace
git clone https://github.com/newideasunlimited/PuckOff puckoff
# For repeatability, check out the specific commit you want in puckoff.
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r puckoff/ci/requirements.txt
west init -l puckoff
west update --narrow -o=--depth=1
bash puckoff/ci/install-toolchain.sh "$PWD/toolchain"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$PWD/toolchain/zephyr-sdk-1.0.1"
export SOURCE_DATE_EPOCH="$(git -C puckoff show -s --format=%ct HEAD)"
west build --sysbuild -b promicro_nrf52840/nrf52840/uf2 \
  -d build/receiver puckoff/firmware
python puckoff/ci/check_uf2.py build/receiver/firmware/zephyr/zephyr.uf2
```

The installer refuses to overwrite an existing directory. It does not install
drivers, flash anything, or modify an existing Nordic SDK.

On macOS, use a matching installed NCS v3.4.1 workspace and Zephyr SDK 1.0.1
for the host architecture. Activate that workspace's Python environment, run
`west build` from the SDK workspace, and use absolute application/build paths
as shown in the README. Give each worktree its own build directory; never
share a build directory across concurrent builds or change the shared SDK.

## CI output and reproducibility boundary

`.github/workflows/receiver.yml` runs the sanitizer-enabled host suite and UF2
validator tests before building firmware. It builds the same source twice in
separate clean output directories with the same `SOURCE_DATE_EPOCH`, requires
byte-identical UF2 files, and validates UF2 headers, family ID, sequential
blocks and application addresses (`0x26000` through, but not including,
`0xEC000`). The image must not overwrite the board's settings/bootloader region.

The uploaded commit-specific artifact contains `puckoff.uf2`, its SHA-256,
an ELF for debugging, the generated firmware configuration, frozen manifest
and build version/package information. GitHub Actions retain it for 14 days.
The workflow does not publish releases or write to the main branch.

This checks repeatability for the pinned build on the selected CI runner.
The Ubuntu runner image and host packages can receive updates; byte identity
across every OS or future runner image is not asserted. Build success also
does not establish USB/RF behavior or compatibility with untested hardware.

Run just the host and artifact checks without an SDK:

```sh
cmake -S tests/receiver -B build/receiver-tests
cmake --build build/receiver-tests
ctest --test-dir build/receiver-tests --output-on-failure
python3 -m unittest discover -s ci -p 'test_*.py'
```

## Recovery

Rebooting into the UF2 bootloader can re-enumerate USB without mounting its
mass-storage drive on macOS. Do not assume that seeing a bootloader USB device
means a UF2 copy can proceed.

The hardware owner recovered the tested board using Adafruit **serial DFU**
over its verified bootloader CDC port. This requires a separately validated,
application-only Intel HEX: every data address must be inside
`0x26000 <= address < 0xEC000`. Do not assume an arbitrary `merged.hex` or
vendor package meets that condition. This fallback was exercised on the
matching board/bootloader, not every nRF52840 variant.

With Adafruit's `adafruit-nrfutil` installed and that validated image:

```sh
adafruit-nrfutil dfu genpkg --dev-type 0x0052 \
  --application application-only.hex app-only.zip
adafruit-nrfutil dfu serial --package app-only.zip \
  --port VERIFIED_BOOTLOADER_CDC_PORT --baudrate 115200
```

The placeholder must be replaced with the positively identified bootloader
port, not a guessed serial device. This procedure must contain **no bootloader,
SoftDevice, UICR or settings update**. The observed application-only recovery
preserved stored pairing; it is not a guarantee that unrelated flashing tools
or board layouts will preserve it. CI does not run recovery or flash hardware.
