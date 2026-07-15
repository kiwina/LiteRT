# Deployment Guide

How to deploy the compiled LiteRT artifacts to the Orange Pi Zero 3W and run
NPU inference.

## Pi Prerequisites

The Orange Pi Zero 3W must have:

1. **NPU driver installed** — `/dev/vipcore` device node exists
2. **ViPLite libraries** — `/usr/lib/libNBGlinker.so` and `/usr/lib/libVIPhal.so`
3. **ViPLite headers** — `/usr/include/vip_lite.h` (for reference, not needed at runtime)

Verify:
```bash
ls -la /dev/vipcore           # NPU device
ls -la /usr/lib/libNBGlinker.so  # ViPLite runtime (has vip_* symbols)
ls -la /usr/lib/libVIPhal.so     # HAL layer
```

## Artifacts to Deploy

Copy these three files from the build machine to the Pi:

| File | Built for | Purpose | Size |
|------|-----------|---------|------|
| `libLiteRtDispatch_Verisilicon.so` | aarch64 | Dispatch plugin (loads NBG on NPU) | ~9.5 MB |
| `libLiteRt.so` | aarch64 | LiteRT runtime library | ~16 MB |
| `run_model` | aarch64 | Inference tool (for testing) | ~10 MB |

```bash
# Copy to Pi
ssh opi "mkdir -p /root/litert-v2.1.6"
scp libLiteRtDispatch_Verisilicon.so opi:/root/litert-v2.1.6/
scp libLiteRt.so opi:/root/litert-v2.1.6/
scp run_model opi:/root/litert-v2.1.6/
```

## Compiling a Model (AOT)

Models are compiled on the build machine (x86 Docker), not on the Pi.

### Option A: fp32 (simple, slow)

```bash
export LD_LIBRARY_PATH=/litert_build/litert/cmake_build_x86/c
export LITERT_VERISILICON_PEGASUS=/opt/acuity/pegasus.py
export LITERT_VERISILICON_VIV_SDK=/opt/VivanteIDE5.11.0/cmdtools
export LITERT_VERISILICON_OPTIMIZE=VIP9000NANODI_PID0X1000003B
export LITERT_VERISILICON_DTYPE=float

/litert_build/litert/cmake_build_x86/tools/apply_plugin_main \
  --model=model.tflite \
  --o=model_compiled.tflite \
  --soc_manufacturer=Verisilicon \
  --soc_model=VIP9000NANODI_PID0X1000003B \
  --libs=/litert_build/litert/cmake_build_x86/vendors/verisilicon/compiler \
  --cmd=apply
```

### Option B: int16 quantized (fast, recommended)

**Step 1: Pre-quantize the model**

```bash
export PATH=/opt/pegasus_venv/bin:$PATH

# Import the model
python3 /opt/acuity/pegasus.py import tflite \
  --model model.tflite \
  --output-model model.json \
  --output-data model.data

# Generate input metadata
python3 /opt/acuity/pegasus.py generate inputmeta \
  --model model.json \
  --input-meta-output model_inputmeta.yml

# Create calibration images (dataset.txt lists image paths)
mkdir -p calib && python3 -c "
from PIL import Image; import numpy as np
[Image.fromarray(np.random.randint(0,256,(224,224,3),dtype=np.uint8)).save(f'calib/img_{i}.png') for i in range(10)]
"
echo "calib/img_0.png" > dataset.txt  # ... list all images

# Quantize to int16
python3 /opt/acuity/pegasus.py quantize \
  --model model.json \
  --model-data model.data \
  --with-input-meta model_inputmeta.yml \
  --quantizer dynamic_fixed_point \
  --qtype int16 \
  --device CPU \
  --iterations 10 \
  --output-dir .
```

This produces `model.quantize`.

**Step 2: Compile with quantization**

```bash
export LITERT_VERISILICON_QUANTIZE=/path/to/model.quantize
# (other env vars same as Option A)

apply_plugin_main \
  --model=model.tflite \
  --o=model_int16_compiled.tflite \
  --soc_manufacturer=Verisilicon \
  --soc_model=VIP9000NANODI_PID0X1000003B \
  --libs=/path/to/compiler/plugin/dir \
  --cmd=apply
```

The plugin detects `LITERT_VERISILICON_QUANTIZE` and passes `--dtype quantized`
to pegasus, producing an int16 NBG.

## Running on the Pi

```bash
ssh opi "cd /root/litert-v2.1.6 && \
  LD_LIBRARY_PATH=/root/litert-v2.1.6 ./run_model \
    --graph=model_compiled.tflite \
    --accelerator=npu \
    --dispatch_library_dir=/root/litert-v2.1.6 \
    --iterations=20"
```

**Flags:**
- `--graph` — path to the compiled .tflite model
- `--accelerator=npu` — use the NPU (via dispatch delegate)
- `--dispatch_library_dir` — directory containing `libLiteRtDispatch_Verisilicon.so`
- `--iterations=N` — run N times for benchmarking

### Expected output (int16)

```
INFO: [dispatch_api.cc:82] enter Verisilicon LiteRtInitialize
INFO: [viplite_adapter_api.cc:81] Loading Verisilicon VIPLite adapter .so from: libVIPhal.so
INFO: [viplite_adapter_api.cc:81] Loading Verisilicon VIPLite adapter .so from: .../libNBGlinker.so
INFO: [viplite_adapter_api.cc:93] Loaded VIPLite shared library.
INFO: [dispatch_api.cc:108] Viplite version: 2.0.3
I0000 ... run_model.cc:472] First run took 6739 microseconds
I0000 ... run_model.cc:476] Fastest run took 3611 microseconds
I0000 ... run_model.cc:479] All runs took average 3780 microseconds
```

## Library Setup on Pi

The dispatch .so needs `libNBGlinker.so` accessible. If it's not in the
dispatch directory, symlink it:

```bash
ln -sf /usr/lib/libNBGlinker.so /root/litert-v2.1.6/libNBGlinker.so
```

The adapter tries these paths in order:
1. `libVIPhal.so` (HAL layer, loaded but doesn't have vip_* symbols)
2. `<dispatch_library_dir>/libNBGlinker.so` (has vip_* symbols)
3. `libNBGlinker.so` (fallback via ldconfig)

It loads ALL that exist and uses the last one for symbol resolution.

## Verification

```bash
# Check NPU is accessible
ls -la /dev/vipcore
cat /sys/class/vipcore/version 2>/dev/null || strings /usr/lib/libNBGlinker.so | grep -i version

# Check dispatch .so loads
ldd /root/litert-v2.1.6/libLiteRtDispatch_Verisilicon.so

# Run with verbose logging
LITERT_LOG_LEVEL=VERBOSE LD_LIBRARY_PATH=/root/litert-v2.1.6 \
  ./run_model --graph=model.tflite --accelerator=npu \
  --dispatch_library_dir=/root/litert-v2.1.6
```
