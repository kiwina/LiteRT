# Docker Environment Setup

Complete guide to set up the build environment from scratch.

## Architecture

We use **two Docker containers**:

```
┌──────────────────────────────────┐     ┌─────────────────────────────────────┐
│  nbg_builder (ACUITY image)      │     │  litert_builder (Ubuntu 24.04)       │
│  Download once, use as source     │     │  All building happens here            │
│                                   │     │                                       │
│  Ubuntu 20.04, GCC 9, Python 3.8  │     │  Ubuntu 24.04, GCC 12/13              │
│  ┌─────────────────────────────┐  │     │  ┌──────────────────────────────┐    │
│  │ pegasus.py + acuitylib      │──┼────┼─▶│ /opt/acuity/pegasus.py       │    │
│  │ VivanteIDE5.11.0/cmdtools   │──┼────┼─▶│ /opt/VivanteIDE5.11.0/...    │    │
│  │ acuity-6.30.22 wheel        │──┼────┼─▶│ /opt/acuity/bin/*.whl        │    │
│  └─────────────────────────────┘  │     │  └──────────────────────────────┘    │
│                                   │     │                                       │
│  ❌ Can't build LiteRT here       │     │  ✅ Repo mounted from WSL             │
│  (GCC 9 = no C++20, cmake 3.16)  │     │  ✅ GCC-12 cross-compiler             │
│                                   │     │  ✅ Pi sysroot at /pi-sysroot         │
│                                   │     │  ✅ CMake 3.28                        │
│                                   │     │  ✅ Python 3.8 venv for pegasus       │
└──────────────────────────────────┘     └─────────────────────────────────────┘
```

**Why two containers?** The ACUITY image (Ubuntu 20.04) has pegasus working
out of the box but its GCC 9 / cmake 3.16 can't build LiteRT (needs C++20 +
cmake 3.20). Its apt repos are also broken (Allwinner mirror unreachable).
We extract pegasus from it and build everything in the Ubuntu 24.04 container.

---

## Step 1: Download the ACUITY Docker Image

Download from Allwinner:
- **Link:** https://netstorage.allwinnertech.com:5001/sharing/Mh23BhPHq
- **Docs:** https://docs.radxa.com/en/cubie/a7s/app-dev/npu-dev/cubie-acuity-env

```bash
# Load the image
docker load -i acuity-toolkit.tar
# Verify (image name will vary - check docker images)
docker images | grep -i acuity
```

## Step 2: Start the ACUITY Container

```bash
# Run the ACUITY container (we call it nbg_builder)
docker run -d --name nbg_builder \
  -v /home/steven/robot/litert-verisilicon/nbg-work:/work \
  <acuity-image-name> sleep infinity

# Verify pegasus works inside it
docker exec nbg_builder python3 /usr/local/acuity_command_line_tools/pegasus.py --help
```

## Step 3: Create the Build Container

```bash
# Create Ubuntu 24.04 container with the repo mounted
docker run -d --name litert_builder \
  -v /home/steven/robot/litert-v2.1.6:/litert_build \
  ubuntu:24.04 sleep infinity

# Enter as root
docker exec -u root -it litert_builder bash
```

## Step 4: Install Build Dependencies

Run inside litert_builder:

```bash
apt-get update && apt-get install -y \
  build-essential cmake git wget \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  gcc-12-aarch64-linux-gnu g++-12-aarch64-linux-gnu \
  python3.8 python3.8-venv python3.8-dev \
  software-properties-common
```

## Step 5: Extract pegasus from ACUITY Container

```bash
# On the WSL host (not inside either container):

mkdir -p /tmp/pegasus-transfer

# Copy pegasus command-line tools
docker cp nbg_builder:/usr/local/acuity_command_line_tools \
  /tmp/pegasus-transfer/acuity_cmd_tools

# Copy the acuity wheel (contains acuitylib + all deps)
docker cp nbg_builder:/root/acuity-toolkit-whl-6.30.22 \
  /tmp/pegasus-transfer/acuity_whl

# Copy VivanteIDE cmdtools (NBG compiler backend)
docker cp nbg_builder:/root/Vivante_IDE/VivanteIDE5.11.0/cmdtools \
  /tmp/pegasus-transfer/viv_cmdtools

# Copy into litert_builder
docker exec -u root litert_builder mkdir -p /opt/acuity /opt/VivanteIDE5.11.0
docker cp /tmp/pegasus-transfer/acuity_cmd_tools/. litert_builder:/opt/acuity/
docker cp /tmp/pegasus-transfer/acuity_whl/. litert_builder:/opt/acuity/
docker cp /tmp/pegasus-transfer/viv_cmdtools/. litert_builder:/opt/VivanteIDE5.11.0/cmdtools/
```

## Step 6: Install acuitylib in litert_builder

Pegasus requires Python 3.8 with the acuitylib package. We create a venv:

```bash
# Inside litert_builder (as root):
python3.8 -m venv /opt/pegasus_venv
/opt/pegasus_venv/bin/pip install /opt/acuity/bin/acuity-6.30.22-cp38-cp38-manylinux2010_x86_64.whl
```

## Step 7: Create python3 Wrapper

The compiler plugin hardcodes `python3 <pegasus_path>` in its subprocess calls.
We need system `python3` to use the venv:

```bash
cat > /usr/local/bin/python3 << 'EOF'
#!/bin/bash
export VIRTUAL_ENV=/opt/pegasus_venv
export PATH=/opt/pegasus_venv/bin:$PATH
exec /opt/pegasus_venv/bin/python3.8 "$@"
EOF
chmod +x /usr/local/bin/python3

# Verify pegasus works via system python3
python3 /opt/acuity/pegasus.py --help
```

## Step 8: Create Pi Sysroot

The cross-compiler's default sysroot has glibc 2.39, but the Pi has glibc 2.36.
We rsync the Pi's filesystem to use as the sysroot:

```bash
# On the WSL host:
mkdir -p /tmp/pi-sysroot/usr /tmp/pi-sysroot/lib
rsync -aL opi:/usr/include/ /tmp/pi-sysroot/usr/include/
rsync -aL opi:/lib/         /tmp/pi-sysroot/lib/
rsync -aL opi:/usr/lib/     /tmp/pi-sysroot/usr/lib/

# Copy into litert_builder
docker cp /tmp/pi-sysroot litert_builder:/pi-sysroot
```

## Step 9: Copy ViPLite Headers

The dispatch plugin must compile against the Pi's exact ViPLite headers.
Copy from the Pi into the repo:

```bash
# On the WSL host:
scp opi:/usr/include/vip_lite.h \
  /home/steven/robot/litert-v2.1.6/litert/vendors/verisilicon/dispatch/vip_lite.h
scp opi:/usr/include/vip_lite_common.h \
  /home/steven/robot/litert-v2.1.6/litert/vendors/verisilicon/dispatch/vip_lite_common.h

# Verify they match
ssh opi "md5sum /usr/include/vip_lite.h /usr/include/vip_lite_common.h"
md5sum /home/steven/robot/litert-v2.1.6/litert/vendors/verisilicon/dispatch/vip_lite.h \
       /home/steven/robot/litert-v2.1.6/litert/vendors/verisilicon/dispatch/vip_lite_common.h
```

## Environment Variables for AOT Compilation

When running `apply_plugin`, set these environment variables:

```bash
export LD_LIBRARY_PATH=/litert_build/litert/cmake_build_x86/c
export LITERT_VERISILICON_PEGASUS=/opt/acuity/pegasus.py
export LITERT_VERISILICON_VIV_SDK=/opt/VivanteIDE5.11.0/cmdtools
export LITERT_VERISILICON_OPTIMIZE=VIP9000NANODI_PID0X1000003B
export LITERT_VERISILICON_DTYPE=float
# For int16, also set:
# export LITERT_VERISILICON_QUANTIZE=/path/to/model.quantize
```

## Verification Checklist

```bash
# 1. pegasus works
python3 /opt/acuity/pegasus.py --help

# 2. VivanteIDE cmdtools present
ls /opt/VivanteIDE5.11.0/cmdtools/vsimulator

# 3. Pi sysroot present
ls /pi-sysroot/usr/include/features.h

# 4. Cross-compiler works
aarch64-linux-gnu-gcc-12 --version

# 5. ViPLite headers match Pi
md5sum /litert_build/litert/vendors/verisilicon/dispatch/vip_lite.h
```

## Saving the Container State

After setup is complete, save the container so you don't have to redo it:

```bash
# Commit the container to a new image
docker commit litert_builder litert_build_env:saved-$(date +%Y%m%d)

# To restore from saved state:
docker run -d --name litert_builder \
  -v /home/steven/robot/litert-v2.1.6:/litert_build \
  litert_build_env:saved-YYYYMMDD sleep infinity
```

## File Locations Summary

| Item | Location | Source |
|------|----------|--------|
| LiteRT source | `/litert_build/` | WSL bind mount |
| pegasus.py | `/opt/acuity/pegasus.py` | ACUITY image |
| acuitylib | `/opt/pegasus_venv/` | ACUITY wheel |
| VivanteIDE cmdtools | `/opt/VivanteIDE5.11.0/cmdtools/` | ACUITY image |
| Pi sysroot | `/pi-sysroot/` | rsync from Pi |
| ViPLite headers | `litert/vendors/verisilicon/dispatch/vip_lite*.h` | scp from Pi |
| python3 wrapper | `/usr/local/bin/python3` | custom script |
| x86 build dir | `/litert_build/litert/cmake_build_x86/` | CMake |
| aarch64 build dir | `/litert_build/litert/cmake_build_aarch64/` | CMake |
