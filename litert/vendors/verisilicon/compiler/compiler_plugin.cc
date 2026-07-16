// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// VeriSilicon LiteRT Compiler Plugin
//
// Partitions supported ops, serializes each partition to a tflite flatbuffer,
// compiles it to NBG bytecode via the ACUITY pegasus toolchain, and returns
// the NBG bytes for embedding in the dispatch tflite.
//
// The runtime dispatch .so (libLiteRtDispatch_Verisilicon.so) loads the NBG
// via vip_create_network(..., VIP_CREATE_NETWORK_FROM_MEMORY, ...) at inference
// time. This plugin only runs at AOT compile time (x86 host / ACUITY Docker).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "litert/c/litert_common.h"
#include "litert/c/litert_model.h"
#include "litert/c/litert_op_code.h"
#include "litert/vendors/c/litert_compiler_plugin.h"
#include "litert/vendors/verisilicon/compiler/nbg_compiler.h"
#include "litert/vendors/verisilicon/compiler/supported_ops.h"

// Use fprintf instead of LITERT_LOG to avoid versioned symbol dependency
// on LiteRtGetMinLoggerSeverity which is not exported by all libLiteRt.so builds.
#define VS_LOG(fmt, ...) fprintf(stderr, "[VeriSilicon] " fmt "\n", ##__VA_ARGS__)
#define VS_LOG_ERR(fmt, ...) fprintf(stderr, "[VeriSilicon ERROR] " fmt "\n", ##__VA_ARGS__)

// =====================================================================
// Plugin state
// =====================================================================
struct LiteRtCompilerPluginT {
  litert::verisilicon::NbgCompiler nbg_compiler;
};

// =====================================================================
// Compiled result (owned by plugin, accessed via accessors below)
// =====================================================================
struct LiteRtCompiledResultT {
  // One NBG bytecode blob per partition. If the plugin produces a single
  // global NBG, all entries point to the same data.
  std::vector<std::vector<uint8_t>> byte_code;

  // Entry-point name per call (one per partition/dispatch op).
  std::vector<std::string> call_names;
};

// =====================================================================
// Identification
// =====================================================================

static constexpr char kPluginManufacturer[] = "Verisilicon";
static constexpr char kPluginSocModel[] = "VIP9000NANODI_PID0X1000003B";
static constexpr char kPluginSDKVersion[] = "1.0.0";

LiteRtStatus LiteRtGetCompilerPluginVersion(LiteRtApiVersion* api_version) {
  if (!api_version) {
    return kLiteRtStatusErrorInvalidArgument;
  }
  api_version->major = LITERT_API_VERSION_MAJOR;
  api_version->minor = LITERT_API_VERSION_MINOR;
  api_version->patch = LITERT_API_VERSION_PATCH;
  return kLiteRtStatusOk;
}

const char* LiteRtGetCompilerPluginSocManufacturer() {
  return kPluginManufacturer;
}

// SDK version (v2.1.6 API: returns status, sets string via out-param)
LiteRtStatus LiteRtGetCompilerPluginSDKVersion(
    LiteRtCompilerPlugin compiler_plugin, const char** sdk_version) {
  if (!compiler_plugin || !sdk_version) {
    return kLiteRtStatusErrorInvalidArgument;
  }
  *sdk_version = kPluginSDKVersion;
  return kLiteRtStatusOk;
}

LiteRtStatus LiteRtGetCompilerPluginSupportedHardware(
    LiteRtCompilerPlugin compiler_plugin,
    LiteRtHwAccelerators* supported_hardware) {
  if (!compiler_plugin || !supported_hardware) {
    return kLiteRtStatusErrorInvalidArgument;
  }
  *supported_hardware = kLiteRtHwAcceleratorNpu;
  return kLiteRtStatusOk;
}

LiteRtStatus LiteRtGetNumCompilerPluginSupportedSocModels(
    LiteRtCompilerPlugin compiler_plugin,
    LiteRtParamIndex* num_supported_soc_models) {
  if (!compiler_plugin || !num_supported_soc_models) {
    return kLiteRtStatusErrorInvalidArgument;
  }
  *num_supported_soc_models = 1;
  return kLiteRtStatusOk;
}

LiteRtStatus LiteRtGetCompilerPluginSupportedSocModel(
    LiteRtCompilerPlugin compiler_plugin, LiteRtParamIndex soc_model_idx,
    const char** soc_model_name) {
  if (!compiler_plugin || !soc_model_name) {
    return kLiteRtStatusErrorInvalidArgument;
  }
  if (soc_model_idx != 0) {
    return kLiteRtStatusErrorUnsupported;
  }
  *soc_model_name = kPluginSocModel;
  return kLiteRtStatusOk;
}

// =====================================================================
// Lifecycle
// =====================================================================

// v2.1.6 API: takes LiteRtCompilerContext* as first arg (ignored for now)
LiteRtStatus LiteRtCreateCompilerPlugin(
    const LiteRtCompilerContext* compiler_context,
    LiteRtCompilerPlugin* compiler_plugin,
    LiteRtEnvironmentOptions env, LiteRtOptions options) {
  if (!compiler_plugin) {
    return kLiteRtStatusErrorInvalidArgument;
  }
  *compiler_plugin = reinterpret_cast<LiteRtCompilerPlugin>(
      new LiteRtCompilerPluginT());
  return kLiteRtStatusOk;
}

void LiteRtDestroyCompilerPlugin(LiteRtCompilerPlugin compiler_plugin) {
  if (compiler_plugin) {
    delete reinterpret_cast<LiteRtCompilerPluginT*>(compiler_plugin);
  }
}

LiteRtStatus LiteRtCompilerPluginCheckCompilerCompatibility(
    LiteRtApiVersion api_version, LiteRtCompilerPlugin compiler_plugin,
    LiteRtEnvironmentOptions env, LiteRtOptions options,
    const char* soc_model_name) {
  return kLiteRtStatusOk;
}

LiteRtStatus LiteRtCompilerPluginRegisterAllTransformations(
    LiteRtCompilerPlugin compiler_plugin,
    LiteRtTransformation** transformations, LiteRtParamIndex* num_patterns) {
  // No graph transformations needed — partition + serialize handles everything.
  *num_patterns = 0;
  return kLiteRtStatusOk;
}

// =====================================================================
// Partition: select which ops to offload to the NPU
// =====================================================================

LiteRtStatus LiteRtCompilerPluginPartition(
    LiteRtCompilerPlugin compiler_plugin, const char* soc_model,
    LiteRtSubgraph subgraph, LiteRtOpList selected_ops) {
  if (!compiler_plugin || !subgraph || !selected_ops) {
    return kLiteRtStatusErrorInvalidArgument;
  }

  // Use the C API to avoid C++ symbol linkage issues.
  LiteRtParamIndex num_ops = 0;
  LiteRtStatus status = LiteRtGetNumSubgraphOps(subgraph, &num_ops);
  if (status != kLiteRtStatusOk) {
    VS_LOG_ERR("LiteRtGetNumSubgraphOps failed: %d",
               static_cast<int>(status));
    return status;
  }

  LiteRtParamIndex pushed = 0;
  for (LiteRtParamIndex i = 0; i < num_ops; ++i) {
    LiteRtOp op;
    status = LiteRtGetSubgraphOp(subgraph, i, &op);
    if (status != kLiteRtStatusOk) {
      VS_LOG_ERR("LiteRtGetSubgraphOp(%u) failed", i);
      return status;
    }

    LiteRtOpCode code;
    status = LiteRtGetOpCode(op, &code);
    if (status != kLiteRtStatusOk) {
      VS_LOG_ERR("LiteRtGetOpCode(%u) failed", i);
      return status;
    }

    if (litert::verisilicon::IsOpSupported(code)) {
      status = LiteRtPushOp(selected_ops, op, /*partition_index=*/0);
      if (status != kLiteRtStatusOk) {
        VS_LOG_ERR("LiteRtPushOp failed for op %u", i);
        return status;
      }
      ++pushed;
    }
  }

  VS_LOG("VeriSilicon Partition: selected %u/%u ops",
             pushed, num_ops);
  return kLiteRtStatusOk;
}

// =====================================================================
// Compile: serialize partitions to tflite, compile to NBG via pegasus
// =====================================================================

LiteRtStatus LiteRtCompilerPluginCompile(
    LiteRtCompilerPlugin compiler_plugin, const char* soc_model,
    LiteRtModel partitions, LiteRtCompiledResult* compiled_result) {
  if (!compiler_plugin || !partitions || !compiled_result) {
    return kLiteRtStatusErrorInvalidArgument;
  }

  auto* plugin = reinterpret_cast<LiteRtCompilerPluginT*>(compiler_plugin);

  // Use C API for model introspection
  LiteRtParamIndex num_partitions = 0;
  LiteRtStatus status = LiteRtGetNumModelSubgraphs(partitions,
                                                    &num_partitions);
  if (status != kLiteRtStatusOk) {
    return status;
  }

  VS_LOG("VeriSilicon Compile: %u partitions",
             num_partitions);

  auto result = std::make_unique<LiteRtCompiledResultT>();
  result->byte_code.resize(num_partitions);
  result->call_names.resize(num_partitions);

  // Serialize the partitions model ONCE, before compiling any partitions.
  // Serializing inside the loop causes the DISPATCH_OP custom op (embedded
  // after each partition compile) to shift operator code indices, breaking
  // later partitions.
  uint8_t* buf = nullptr;
  size_t size = 0, offset = 0;
  LiteRtModelSerializationOptions opts;
  memset(&opts, 0, sizeof(opts));
  opts.bytecode_alignment = 256;  // NPU DMA requirement

  status = LiteRtSerializeModel(partitions, &buf, &size, &offset,
                                 /*destroy=*/false, opts);
  if (status != kLiteRtStatusOk) {
    VS_LOG_ERR("LiteRtSerializeModel failed: %d", static_cast<int>(status));
    return status;
  }

  VS_LOG("Serialized: %zu bytes (offset %zu, valid %zu)",
         size, offset, size - offset);

  for (LiteRtParamIndex i = 0; i < num_partitions; ++i) {
    // Compile to NBG via pegasus
    char partition_name[64];
    snprintf(partition_name, sizeof(partition_name), "partition_%u", i);
    auto nbg = plugin->nbg_compiler.Compile(buf + offset, size - offset,
                                             partition_name);

    if (nbg.empty()) {
      VS_LOG_ERR("NBG compilation failed for partition %u", i);
      free(buf);
      return kLiteRtStatusErrorRuntimeFailure;
    }

    result->byte_code[i] = std::move(nbg);
    result->call_names[i] = partition_name;

    VS_LOG("Partition %u: NBG = %zu bytes", i,
               result->byte_code[i].size());
  }

  free(buf);

  *compiled_result = reinterpret_cast<LiteRtCompiledResult>(result.release());
  return kLiteRtStatusOk;
}

// =====================================================================
// Compiled result accessors
// =====================================================================

// JIT support — our NPU is AOT-only (VIPLite can't compile on-device)
LiteRtStatus LiteRtGetCompiledResultHandle(
    LiteRtCompiledResult compiled_result, LiteRtParamIndex call_idx,
    LiteRtJitExecutable* handle) {
  return kLiteRtStatusErrorUnsupported;
}

LiteRtStatus LiteRtGetCompiledResultByteCode(
    LiteRtCompiledResult compiled_result, LiteRtParamIndex byte_code_idx,
    const void** byte_code, size_t* byte_code_size) {
  if (!compiled_result || !byte_code || !byte_code_size) {
    return kLiteRtStatusErrorInvalidArgument;
  }
  auto* result = reinterpret_cast<LiteRtCompiledResultT*>(compiled_result);
  if (byte_code_idx >= result->byte_code.size()) {
    return kLiteRtStatusErrorInvalidArgument;
  }
  *byte_code = result->byte_code[byte_code_idx].data();
  *byte_code_size = result->byte_code[byte_code_idx].size();
  return kLiteRtStatusOk;
}

LiteRtStatus LiteRtGetCompiledResultCallInfo(
    LiteRtCompiledResult compiled_result, LiteRtParamIndex call_idx,
    const void** call_info, size_t* call_info_size,
    LiteRtParamIndex* byte_code_idx) {
  if (!compiled_result || !call_info || !call_info_size || !byte_code_idx) {
    return kLiteRtStatusErrorInvalidArgument;
  }
  auto* result = reinterpret_cast<LiteRtCompiledResultT*>(compiled_result);
  if (call_idx >= result->call_names.size()) {
    return kLiteRtStatusErrorInvalidArgument;
  }
  *call_info = result->call_names[call_idx].data();
  *call_info_size = result->call_names[call_idx].size();
  *byte_code_idx = call_idx;  // each call uses its own bytecode blob
  return kLiteRtStatusOk;
}

LiteRtStatus LiteRtGetNumCompiledResultCalls(
    LiteRtCompiledResult compiled_result, LiteRtParamIndex* num_calls) {
  if (!compiled_result || !num_calls) {
    return kLiteRtStatusErrorInvalidArgument;
  }
  auto* result = reinterpret_cast<LiteRtCompiledResultT*>(compiled_result);
  *num_calls = result->call_names.size();
  return kLiteRtStatusOk;
}

void LiteRtDestroyCompiledResult(LiteRtCompiledResult compiled_result) {
  if (compiled_result) {
    delete reinterpret_cast<LiteRtCompiledResultT*>(compiled_result);
  }
}

LiteRtStatus LiteRtCompiledResultNumByteCodeModules(
    LiteRtCompiledResult compiled_result, LiteRtParamIndex* num_byte_code) {
  if (!compiled_result || !num_byte_code) {
    return kLiteRtStatusErrorInvalidArgument;
  }
  auto* result = reinterpret_cast<LiteRtCompiledResultT*>(compiled_result);
  *num_byte_code = result->byte_code.size();
  return kLiteRtStatusOk;
}
