# Architecture

How the VeriSilicon LiteRT plugin works internally.

## Overview

The plugin follows LiteRT's vendor plugin architecture: a **compiler plugin**
(AOT, runs on x86 during compilation) and a **dispatch plugin** (runtime, runs
on aarch64 during inference). This matches the pattern used by Samsung,
Qualcomm, MediaTek, and Google Tensor vendors.

```
┌─── Compiler Plugin (x86) ────────────────────┐    ┌─── Dispatch Plugin (aarch64) ────────────┐
│                                               │    │                                            │
│  LiteRT Framework                             │    │  LiteRT Framework                          │
│    │                                          │    │    │                                       │
│    ├─ apply_plugin_main                       │    │    ├─ run_model                           │
│    │    │                                     │    │    │    │                                  │
│    │    ├─ Load model (.tflite)               │    │    │    ├─ Load compiled model             │
│    │    ├─ Load compiler plugin .so           │    │    │    ├─ Create dispatch delegate        │
│    │    │                                     │    │    │    │                                  │
│    │    ├─ Partition: select supported ops    │    │    │    ├─ libLiteRtDispatch_Verisilicon.so│
│    │    ├─ Compile:                           │    │    │    │    │                             │
│    │    │    ├─ Serialize partition → tflite  │    │    │    │    ├─ LiteRtDispatchGetApi()      │
│    │    │    ├─ pegasus import tflite → json  │    │    │    │    ├─ LiteRtDispatchInitialize() │
│    │    │    ├─ pegasus generate inputmeta   │    │    │    │    │    ├─ dlopen libNBGlinker.so │
│    │    │    └─ pegasus export ovxlib → NBG  │    │    │    │    │    ├─ dlsym vip_* functions  │
│    │    │                                     │    │    │    │    │    └─ vip_init()             │
│    │    └─ Embed NBG into dispatch tflite     │    │    │    │    │                              │
│    │                                          │    │    │    │    ├─ DeviceContextCreate()       │
│    │    → compiled.tflite                     │    │    │    │    ├─ InvocationContextCreate()   │
│    │                                          │    │    │    │    │    ├─ vip_create_network()   │
│    └─ (writes output)                         │    │    │    │    │    │   (loads NBG byte code)  │
│                                               │    │    │    │    │    │                          │
│  libLiteRtCompilerPluginVerisilicon.so        │    │    │    │    ├─ AttachInput/Output()         │
│    18 C exports                               │    │    │    │    ├─ Invoke() → vip_run_network() │
│    Shells out to pegasus subprocess           │    │    │    │    └─ vip_wait_network()           │
│                                               │    │    │    │                                   │
└───────────────────────────────────────────────┘    └───────────────────────────────────────────┘
```

## Compiler Plugin Internals

### Entry Point: `LiteRtCompilerPluginPartition()`

Selects which TFLite ops the NPU supports. Compares each op against the
`kSupportedOps` list in `supported_ops.h`. Returns a list of partition
subgraphs — each becomes one NBG.

### Entry Point: `LiteRtCompilerPluginCompile()`

For each partition:

1. **Serialize** the partition subgraph back to TFLite flatbuffer bytes using
   `LiteRtSerializeModel()`. This is lossless — the partition is a valid
   standalone TFLite model.

2. **Shell out to pegasus** (VeriSilicon's official toolchain):
   ```
   pegasus import tflite  → partition.json + partition.data
   pegasus generate inputmeta → partition_inputmeta.yml
   pegasus export ovxlib --pack-nbg-unify → network_binary.nb
   ```
   The `--pack-nbg-unify` flag produces a single binary NBG file.

3. **Read the NBG** bytes and return them as the compiled result. LiteRT's
   framework embeds the NBG into the output TFLite model as a custom op with
   byte code data.

### Quantization Handling

The plugin **does not quantize**. If the user provides a `.quantize` file via
`LITERT_VERISILICON_QUANTIZE`, the plugin passes `--model-quantize` and
`--dtype quantized` to pegasus. Otherwise it compiles with `--dtype float`.

This respects the user's choice: quantization is a pre-processing step.

## Dispatch Plugin Internals

### Late Binding (dlopen)

The dispatch .so does NOT link against `libNBGlinker.so` at compile time.
Instead, it does `dlopen()` at runtime:

```cpp
// viplite_adapter_api.cc
so_paths.push_back("libVIPhal.so");      // HAL layer
so_paths.push_back(dispatch_dir + "/libNBGlinker.so");  // has vip_* symbols

for (auto& so_path : so_paths) {
    auto maybe_dlib = SharedLibrary::Load(so_path, RtldFlags::Default());
    if (maybe_dlib.HasValue()) {
        dlib_ = std::move(maybe_dlib.Value());
    }
}

// Bind function pointers via dlsym
LOAD_SYMB(vip_create_network, api_->create_network);
LOAD_SYMB(vip_run_network, api_->run_network);
// ... 30+ symbols
```

This means the dispatch .so is self-contained — no vendor NPU libraries needed
at link time. The ViPLite `.so` files are loaded from the Pi at runtime.

### NBG Execution

When `run_model` loads a compiled `.tflite`:

1. **`LiteRtDispatchInitialize()`** — dlopens libNBGlinker.so, loads symbols,
   calls `vip_init()`

2. **`LiteRtDispatchDeviceContextCreate()`** — creates device context, queries
   hardware capabilities

3. **`LiteRtDispatchInvocationContextCreate()`** — calls `vip_create_network()`
   with the NBG byte code from the model. This loads the network onto the NPU.

4. **`Invoke()`** — attaches input/output buffers via `vip_set_input()` /
   `vip_set_output()`, then calls `vip_run_network()` + `vip_wait_network()`.

## Header ABI Matching

The `vip_lite.h` and `vip_lite_common.h` headers in the repo are copied from
the Pi's `/usr/include/` — they are the exact 2024 Vivante headers matching
the Pi's `libNBGlinker.so` (ViPLite 2.0.3.2-AW-2024-08-30).

These are **NOT** the ACUITY toolkit's headers. The ACUITY Docker image
(https://netstorage.allwinnertech.com:5001/sharing/Mh23BhPHq) may ship
different header versions. Always use the headers from the target device.

This is critical: the `VipliteAdapterApi::Api` struct uses `decltype(&vip_init)`
to determine function pointer types at compile time. If the header's function
signature differs from the `.so`, the call convention mismatches → segfault or
data corruption.

To copy the correct headers:
```bash
# From the Pi's /usr/include/ — these are what the device's .so was built against
scp opi:/usr/include/vip_lite.h litert/vendors/verisilicon/dispatch/vip_lite.h
scp opi:/usr/include/vip_lite_common.h litert/vendors/verisilicon/dispatch/vip_lite_common.h
```

**Rule:** Always compile the dispatch .so against the same headers the target
device's libNBGlinker.so was built with.

## Cross-Compilation

The toolchain file (`aarch64_linux_toolchain.cmake`) solves three problems:

1. **Architecture** — GCC-12 cross-compiler targets aarch64
2. **GLIBC compatibility** — `-isystem` flags point at the Pi's glibc 2.36
   headers (not the cross-compiler's 2.39), preventing `__isoc23_*` symbol
   requirements
3. **Link order** — dispatch .so links the full LiteRT runtime stack via
   `--start-group` / `--end-group` to resolve all symbols (including
   `GpuEnvironment`)

## LiteRT API Version

This plugin targets **LiteRT v2.1.6**. Key API details:

- `LiteRtCreateCompilerPlugin` takes 4 args including `const LiteRtCompilerContext*`
- `LiteRtGetCompilerPluginSDKVersion` returns status + `const char**`
- `LiteRtGetCompiledResultHandle` returns `kLiteRtStatusErrorUnsupported` (JIT not implemented)
- Dispatch functions (`LiteRtInitialize`, `LiteRtDeviceContextCreate`,
  `LiteRtInvocationContextCreate`) take `const LiteRtRuntimeContext*` as first arg
