# LiteRT VeriSilicon Vendor Plugin

This plugin adds **VeriSilicon VIP9000 NPU** support to LiteRT, targeting the
**Allwinner A733** SoC found in the **Orange Pi Zero 3W**.

It enables any TensorFlow Lite model to be compiled for and executed on the NPU
using the standard LiteRT AOT compilation pipeline — no manual ONNX conversion,
no model stitching, no acuitylite Python at inference time.

## What it does

```
┌─────────────────────────────────────────────────────────────────┐
│                    Build Machine (x86 Docker)                    │
│                                                                  │
│  tflite model ──→ apply_plugin ──→ compiled.tflite (with NBG)   │
│                      │                                           │
│                      ├─ Partition: select NPU-supported ops     │
│                      ├─ Serialize: tflite → flatbuffer bytes    │
│                      ├─ pegasus import: tflite → JSON + .data   │
│                      ├─ pegasus generate inputmeta               │
│                      └─ pegasus export ovxlib → NBG byte code    │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ scp compiled.tflite
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│              Orange Pi Zero 3W (aarch64, A733 SoC)               │
│                                                                  │
│  compiled.tflite ──→ run_model ──→ NPU inference                │
│                         │                                        │
│                         ├─ libLiteRt.so loads dispatch .so      │
│                         ├─ dispatch .so dlopens libNBGlinker.so │
│                         └─ ViPLite submits NBG to VIP9000 NPU   │
└─────────────────────────────────────────────────────────────────┘
```

## Performance

Benchmark: MobileNet V2 (1.0, 224×224), 20 iterations on Pi NPU.

| Data Type | NBG Size | First Run | Average | Fastest | Speedup |
|-----------|----------|-----------|---------|---------|---------|
| float (fp32) | 7.36 MB | 162.4 ms | 162.4 ms | 162.4 ms | 1× |
| **int16** | **6.60 MB** | **6.7 ms** | **3.8 ms** | **3.6 ms** | **43×** |

int16 uses the NPU's hardware-optimized datapath. fp32 falls back to a slow
programmable path. **Always pre-quantize models to int16 for production use.**

## Components

### Compiler Plugin (x86, runs in Docker during AOT compile)

- `compiler/compiler_plugin.cc` — 18 C-exported LiteRT compiler plugin functions
- `compiler/nbg_compiler.h` — Wraps pegasus subprocess (import → inputmeta → export NBG)
- `compiler/supported_ops.h` — List of NPU-supported TFLite ops
- `compiler/CMakeLists.txt` — Builds `libLiteRtCompilerPluginVerisilicon.so`

**Output:** `libLiteRtCompilerPluginVerisilicon.so` (69 KB)

### Dispatch Plugin (aarch64, runs on Pi at inference time)

- `dispatch/dispatch_api.cc` — LiteRT dispatch API entry points (v2.1.6)
- `dispatch/viplite_adapter_api.cc` — Late-binding dlopen of libNBGlinker.so
- `dispatch/litert_dispatch_device_context.cc` — Device context management
- `dispatch/litert_dispatch_invocation_context.cc` — Network invocation
- `dispatch/vip_lite.h` / `vip_lite_common.h` — ViPLite C API headers (2024 version)
- `dispatch/CMakeLists.txt` — Builds `libLiteRtDispatch_Verisilicon.so`

**Output:** `libLiteRtDispatch_Verisilicon.so` (9.5 MB)

### Options

- `c/options/litert_verisilicon_options.{h,cc}` — C API options (device index, core index, timeout, etc.)
- `cc/options/litert_verisilicon_options.{h,cc}` — C++ wrapper for the above

### Toolchain

- `toolchain/aarch64_linux_toolchain.cmake` — Cross-compile for Pi (GCC-12 + Pi sysroot)

## Supported Ops

The following TFLite ops are partitioned to the NPU:

```
Conv2D, DepthwiseConv2D, Add, Mul, Sub, Relu, Relu6, Concatenation,
Reshape, AveragePool2D, MaxPool2D, Mean, FullyConnected, Softmax,
Transpose, Pad
```

Ops not in this list fall back to CPU (XNNPACK).

## Environment Variables

The compiler plugin reads configuration from environment variables:

| Variable | Required | Default | Description |
|----------|----------|---------|-------------|
| `LITERT_VERISILICON_PEGASUS` | Yes | — | Path to `pegasus.py` |
| `LITERT_VERISILICON_VIV_SDK` | Yes | — | Path to `VivanteIDE5.11.0/cmdtools` |
| `LITERT_VERISILICON_OPTIMIZE` | No | `VIP9000NANODI_PID0X1000003B` | Target NPU config |
| `LITERT_VERISILICON_DTYPE` | No | `float` | NBG data type |
| `LITERT_VERISILICON_QUANTIZE` | No | — | Path to pre-quantized `.quantize` file |
| `LITERT_VERISILICON_KEEP_TEMP` | No | unset | If set, keeps temp files for debugging |

## Design Principles

1. **Plugin does NOT quantize** — quantization is a pre-processing step done by the user
2. **Late binding** — dispatch .so does `dlopen("libNBGlinker.so")` at runtime, no vendor libs needed at link time
3. **Pegasus subprocess** — NBG compilation uses VeriSilicon's official pegasus toolchain, not a custom compiler
4. **Matches vendor pattern** — CMake structure mirrors Samsung's implementation exactly

## See Also

- [Build Guide](docs/BUILD_GUIDE.md) — How to build from source
- [Deployment Guide](docs/DEPLOYMENT.md) — How to deploy and run on the Pi
- [Architecture](docs/ARCHITECTURE.md) — Technical deep-dive
