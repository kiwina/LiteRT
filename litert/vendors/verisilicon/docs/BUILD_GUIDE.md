# Build Guide

How to build the LiteRT VeriSilicon plugin from source.

## Prerequisites

### Host machine (x86_64, WSL2 or Linux)

- Docker
- The LiteRT source (this repo): `git clone https://github.com/kiwina/LiteRT.git -b verisilicon-a733`
- VeriSilicon ACUITY toolkit (pegasus.py + VivanteIDE cmdtools)
- SSH access to the Orange Pi Zero 3W (for sysroot)

### Docker image

We use a standard Ubuntu 24.04 container with build tools:

```bash
# Create the build container with the repo mounted
docker run -it --name litert_builder \
  -v /path/to/LiteRT:/litert_build \
  ubuntu:24.04 bash

# Inside the container, install build dependencies:
apt-get update && apt-get install -y \
  build-essential cmake git wget \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  gcc-12-aarch64-linux-gnu g++-12-aarch64-linux-gnu \
  python3.8 python3.8-venv python3.8-dev \
  software-properties-common
```

### ACUITY toolkit (pegasus + VivanteIDE)

The ACUITY toolkit provides `pegasus.py` (model import/export) and VivanteIDE
cmdtools (NBG compiler). Install them inside the Docker container:

```bash
# Install the acuity wheel in a Python 3.8 venv (pegasus requires Python 3.8)
python3.8 -m venv /opt/pegasus_venv
/opt/pegasus_venv/bin/pip install /path/to/acuity-6.30.22-cp38-cp38-manylinux2010_x86_64.whl

# Create a python3 wrapper so the plugin can call pegasus
cat > /usr/local/bin/python3 << 'EOF'
#!/bin/bash
export VIRTUAL_ENV=/opt/pegasus_venv
export PATH=/opt/pegasus_venv/bin:$PATH
exec /opt/pegasus_venv/bin/python3.8 "$@"
EOF
chmod +x /usr/local/bin/python3

# Copy VivanteIDE cmdtools
cp -r /path/to/VivanteIDE5.11.0/cmdtools /opt/VivanteIDE5.11.0/cmdtools
```

### Pi sysroot (for cross-compilation)

The cross-compiler must compile against the Pi's exact glibc 2.36 headers to
avoid GLIBC_2.38 symbol errors. Rsync the Pi's filesystem:

```bash
# From the host (not inside Docker)
mkdir -p /tmp/pi-sysroot/usr /tmp/pi-sysroot/lib
rsync -aL opi:/usr/include/ /tmp/pi-sysroot/usr/include/
rsync -aL opi:/lib/         /tmp/pi-sysroot/lib/
rsync -aL opi:/usr/lib/     /tmp/pi-sysroot/usr/lib/

# Copy into the Docker container
docker cp /tmp/pi-sysroot litert_builder:/pi-sysroot
```

## Building

### Step 1: Configure x86 build (compiler plugin + tools)

```bash
cd /litert_build/litert
mkdir -p cmake_build_x86 && cd cmake_build_x86

cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DLITERT_ENABLE_NPU=ON \
  -DLITERT_ENABLE_GPU=OFF \
  -DLITERT_ENABLE_QUALCOMM=OFF \
  -DLITERT_ENABLE_SAMSUNG=OFF \
  -DLITERT_ENABLE_VERISILICON=ON
```

### Step 2: Build x86 artifacts

```bash
cmake --build . --target verisilicon_compiler_plugin -j$(nproc)
cmake --build . --target apply_plugin_main -j$(nproc)
cmake --build . --target litert_runtime_c_api_shared_lib -j$(nproc)
```

**Outputs:**
- `vendors/verisilicon/compiler/libLiteRtCompilerPluginVerisilicon.so`
- `tools/apply_plugin_main`
- `c/libLiteRt.so`

### Step 3: Configure aarch64 cross-compile

```bash
cd /litert_build/litert
mkdir -p cmake_build_aarch64 && cd cmake_build_aarch64

cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=../litert/vendors/verisilicon/toolchain/aarch64_linux_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DLITERT_ENABLE_NPU=ON \
  -DLITERT_ENABLE_GPU=OFF \
  -DLITERT_ENABLE_QUALCOMM=OFF \
  -DLITERT_ENABLE_SAMSUNG=OFF \
  -DLITERT_ENABLE_VERISILICON=ON \
  -DTFLITE_HOST_TOOLS_DIR=/litert_build/litert/cmake_build_x86/_deps/flatbuffers-build \
  -DTENSORFLOW_SOURCE_DIR=/litert_build/litert/cmake_build_x86/tflite_build/tensorflow-src \
  -DFETCHCONTENT_SOURCE_DIR_PROTOBUF=/litert_build/litert/cmake_build_x86/protobuf \
  -DFETCHCONTENT_SOURCE_DIR_XNNPACK=/litert_build/litert/cmake_build_x86/xnnpack
```

> **Tip:** The `FETCHCONTENT_SOURCE_DIR_*` and `TENSORFLOW_SOURCE_DIR` flags
> point at the x86 build's downloaded sources. This avoids re-downloading
> 1+ GB of dependencies. The `TFLITE_HOST_TOOLS_DIR` provides the x86-compiled
> `flatc` binary needed during cross-compilation.

### Step 4: Build aarch64 artifacts

```bash
cmake --build . --target dispatch_api_verisilicon_so -j$(nproc)
cmake --build . --target litert_runtime_c_api_shared_lib -j$(nproc)
cmake --build . --target run_model -j$(nproc)
```

### Step 5: Verify GLIBC compatibility

The binaries must not require GLIBC newer than 2.36 (Pi runs Debian 12):

```bash
# All three commands should produce EMPTY output:
objdump -T vendors/verisilicon/dispatch/libLiteRtDispatch_Verisilicon.so | grep GLIBC_2.38
objdump -T c/libLiteRt.so | grep GLIBC_2.38
objdump -T tools/run_model | grep GLIBC_2.38

# Check max GLIBC version (should be ≤ 2.34):
objdump -T vendors/verisilicon/dispatch/libLiteRtDispatch_Verisilicon.so | grep -oP 'GLIBC_[0-9.]+' | sort -V | tail -3
```

## Troubleshooting

### "GLIBC_2.38 not found" on Pi

The cross-compiler's default sysroot has glibc 2.39. You must use the Pi
sysroot via the toolchain file's `-isystem` flags. Verify the toolchain file
points at `/pi-sysroot` and uses GCC-12 (not GCC-13+).

### FetchContent re-downloading dependencies

If CMake re-downloads protobuf/xnnpack/tensorflow (slow), pass the
`FETCHCONTENT_SOURCE_DIR_*` flags to point at the x86 build's sources.

### "symbol lookup error: undefined symbol: GpuEnvironment"

The dispatch .so must link against the full LiteRT runtime stack (not just
`litert_runtime_c_api_static`). The CMakeLists.txt uses `--start-group` /
`--end-group` with `litert_cc_api`, `litert_cc_options`, `tensorflow-lite`.

### pegasus "can't open file" or "No module named 'acuitylib'"

Pegasus requires Python 3.8 with the acuitylib package. Create a venv and
install the acuity wheel, then make the `python3` wrapper point at it.
