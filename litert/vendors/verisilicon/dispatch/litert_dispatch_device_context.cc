// Copyright 2025 Vivante Corporation.
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

#include "litert/vendors/verisilicon/dispatch/litert_dispatch_device_context.h"

#include <sys/mman.h>

#include <cstddef>
#include <memory>

#include "absl/strings/str_format.h"  // from @com_google_absl
#include "litert/c/litert_common.h"
#include "litert/c/litert_opaque_options.h"
#include "litert/c/litert_tensor_buffer.h"
#include "litert/c/options/litert_verisilicon_options.h"
#include "litert/cc/litert_expected.h"
#include "litert/cc/litert_macros.h"
#include "litert/vendors/c/litert_dispatch.h"
#include "litert/vendors/verisilicon/dispatch/viplite_adapter_api.h"

using litert::Error;

namespace {
// Map LiteRT element types to ViPLite buffer formats.
// This follows the same pattern as TIM-VX's TranslateDataType:
// the format is derived from the tensor type, not queried from the NBG.
// The NBG was compiled with the same format baked in at pegasus export time.
vip_buffer_format_e LiteRtElementTypeToVipFormat(
    LiteRtElementType element_type) {
  switch (element_type) {
    case kLiteRtElementTypeFloat32:
      return VIP_BUFFER_FORMAT_FP32;
    case kLiteRtElementTypeFloat16:
      return VIP_BUFFER_FORMAT_FP16;
    case kLiteRtElementTypeInt8:
      return VIP_BUFFER_FORMAT_INT8;
    case kLiteRtElementTypeInt16:
      return VIP_BUFFER_FORMAT_INT16;
    case kLiteRtElementTypeInt32:
      return VIP_BUFFER_FORMAT_INT32;
    case kLiteRtElementTypeUInt8:
      return VIP_BUFFER_FORMAT_UINT8;
    case kLiteRtElementTypeUInt16:
      return VIP_BUFFER_FORMAT_UINT16;
    case kLiteRtElementTypeInt64:
      return VIP_BUFFER_FORMAT_INT64;
    case kLiteRtElementTypeUInt64:
      return VIP_BUFFER_FORMAT_UINT64;
    case kLiteRtElementTypeBool:
      return VIP_BUFFER_FORMAT_BOOL8;
    case kLiteRtElementTypeBFloat16:
      return VIP_BUFFER_FORMAT_BFP16;
    default:
      LITERT_LOG(LITERT_WARNING,
                 "Unsupported element type %d, defaulting to FP32",
                 element_type);
      return VIP_BUFFER_FORMAT_FP32;
  }
}

// Populate vip_buffer_create_params_t with the correct format and dimensions
// from the LiteRT tensor type. ViPLite expects [w, h, c, n] order (reversed
// from TFLite's NHWC), matching the TIM-VX pattern.
void PopulateBufferParams(vip_buffer_create_params_t& params,
                          const LiteRtRankedTensorType& tensor_type) {
  params.data_format = LiteRtElementTypeToVipFormat(tensor_type.element_type);
  int rank = tensor_type.layout.rank;
  if (rank > 0 && rank <= 6) {
    params.num_of_dims = rank;
    // Reverse dimensions: TFLite NHWC → ViPLite WHCN
    for (int i = 0; i < rank; i++) {
      params.sizes[i] = tensor_type.layout.dimensions[rank - 1 - i];
    }
  } else {
    // Fallback for unusual ranks
    params.num_of_dims = 1;
    LITERT_LOG(LITERT_WARNING,
               "Tensor rank %d is unusual, using 1D buffer", rank);
  }
}
}  // namespace

litert::Expected<LiteRtDispatchDeviceContextT::Ptr>
LiteRtDispatchDeviceContextT::Create(
    const litert::verisilicon::VipliteAdapterApi& viplite_adapter_api,
    const LiteRtOptions options) {
  return std::unique_ptr<LiteRtDispatchDeviceContextT>(
      new LiteRtDispatchDeviceContextT(viplite_adapter_api, options));
}

litert::Expected<void> LiteRtDispatchDeviceContextT::Init() {
  uint32_t deviceCount = 0;
  std::vector<uint32_t> core_count;
  LrtVerisiliconOptions verisilicon_opts = nullptr;
  LiteRtOpaqueOptions opaque_opts_c = nullptr;
  unsigned int time_out = 0;
  unsigned int profile_level = 0;
  bool dump_nbg = 0;

  if (lrt_options_ &&
      LiteRtGetOpaqueOptions(lrt_options_, &opaque_opts_c) == kLiteRtStatusOk) {
    void* vsi_payload = nullptr;
    if (LiteRtFindOpaqueOptionsData(opaque_opts_c,
                                    LrtVerisiliconOptionsGetIdentifier(),
                                    &vsi_payload) == kLiteRtStatusOk &&
        vsi_payload != nullptr) {
      const char* toml_str = static_cast<const char*>(vsi_payload);
      auto create_status =
          LrtCreateVerisiliconOptionsFromToml(toml_str, &verisilicon_opts);
      if (create_status != kLiteRtStatusOk) {
        LITERT_LOG(LITERT_WARNING,
                   "Create Verisilicon options from toml failed, use default "
                   "settings");
        verisilicon_opts = nullptr;
      }
    }
  }
  if (verisilicon_opts) {
    auto status =
        LrtVerisiliconOptionsGetDeviceIndex(verisilicon_opts, &device_index_);
    if (status != kLiteRtStatusOk) {
      LITERT_LOG(
          LITERT_WARNING,
          "Failed to get device index from Verisilicon options,default 0 ");
    }
    LITERT_LOG(LITERT_INFO, "Get Verisilicon option:device_index = %d",
               device_index_);

    status = LrtVerisiliconOptionsGetCoreIndex(verisilicon_opts, &core_index_);
    if (status != kLiteRtStatusOk) {
      LITERT_LOG(LITERT_WARNING,
                 "Failed to get core index from Verisilicon options,default 0");
    }
    LITERT_LOG(LITERT_INFO, "Get Verisilicon option:core_index = %d",
               core_index_);

    status = LrtVerisiliconOptionsGetTimeOut(verisilicon_opts, &time_out);
    status =
        LrtVerisiliconOptionsGetProfileLevel(verisilicon_opts, &profile_level);
    status = LrtVerisiliconOptionsGetDumpNBG(verisilicon_opts, &dump_nbg);

    LrtDestroyVerisiliconOptions(verisilicon_opts);
  }
  viplite_adapter_api_.api().init();
  viplite_adapter_api_.api().query_hardware(VIP_QUERY_HW_PROP_DEVICE_COUNT,
                                            sizeof(uint32_t), &deviceCount);
  if (device_index_ >= deviceCount) {
    LITERT_LOG(LITERT_WARNING,
               "Total deviceCount is %d, but the specified device index is %d. "
               "Fix it to 0.",
               deviceCount, device_index_);
    device_index_ = 0;
  }
  core_count.resize(deviceCount);
  viplite_adapter_api_.api().query_hardware(
      VIP_QUERY_HW_PROP_DEVICE_COUNT, sizeof(uint32_t) * core_count.size(),
      core_count.data());
  if (core_index_ >= core_count[device_index_]) {
    LITERT_LOG(LITERT_WARNING,
               "Total core count is %d, but the specified core index is %d. "
               "Fix it to 0.",
               core_count[device_index_], core_index_);
    core_index_ = 0;
  }
  memset(&vpm_param_, 0, sizeof(vpm_param_));
  vpm_param_.time_out = time_out;
  vpm_param_.device_index = device_index_;
  vpm_param_.core_index = core_index_;
  vpm_param_.cnnprofile_level = profile_level;
  vpm_param_.dump_nbg = dump_nbg;

  return {};
}

litert::Expected<LiteRtTensorBufferHandle>
LiteRtDispatchDeviceContextT::RegisterTensorBuffer(
    LiteRtTensorBuffer tensor_buffer) {
  LiteRtTensorBufferType tensor_buffer_type;
  LITERT_RETURN_IF_ERROR(
      LiteRtGetTensorBufferType(tensor_buffer, &tensor_buffer_type));

  if (tensor_buffer_type != kLiteRtTensorBufferTypeHostMemory &&
      tensor_buffer_type != kLiteRtTensorBufferTypeDmaBuf) {
    return Error(
        kLiteRtStatusErrorUnsupported,
        absl::StrFormat("Unsupported buffer type %d", tensor_buffer_type));
  }

  size_t tensor_buffer_size;
  LITERT_RETURN_IF_ERROR(
      LiteRtGetTensorBufferSize(tensor_buffer, &tensor_buffer_size));

  size_t tensor_buffer_offset;
  if (auto status =
          LiteRtGetTensorBufferOffset(tensor_buffer, &tensor_buffer_offset);
      status != kLiteRtStatusOk) {
    if (status == kLiteRtStatusErrorNotFound) {
      tensor_buffer_offset = 0;
    } else {
      return Error(kLiteRtStatusErrorRuntimeFailure,
                   "Failed to get buffer offset");
    }
  }

  LiteRtRankedTensorType tensor_type;
  LITERT_RETURN_IF_ERROR(
      LiteRtGetTensorBufferTensorType(tensor_buffer, &tensor_type));

  if (tensor_type.layout.has_strides) {
    return Error(kLiteRtStatusErrorRuntimeFailure,
                 "Tensor strides are not supported");
  }
  void* host_addr = NULL;
  vip_buffer_create_params_t tensor_param = {};
  vip_buffer viplite_buffer = NULL;
  int fd = -1;
  memset(&tensor_param, 0, sizeof(tensor_param));

  switch (tensor_buffer_type) {
    case kLiteRtTensorBufferTypeHostMemory: {
      if (auto status =
              LiteRtGetTensorBufferHostMemory(tensor_buffer, &host_addr);
          status != kLiteRtStatusOk) {
        return Error(status, "Failed to get host memory");
      }
      // Use the Allwinner VIPLite API: create a HOST memory buffer
      tensor_param.memory_type = VIP_BUFFER_MEMORY_TYPE_HOST;
      PopulateBufferParams(tensor_param, tensor_type);

      if (viplite_adapter_api_.api().create_buffer(
              &tensor_param, sizeof(tensor_param), &viplite_buffer) !=
          VIP_SUCCESS) {
        return litert::Unexpected(
            kLiteRtStatusErrorRuntimeFailure,
            "Failed to create Viplite buffer");
      }
      // Copy host data into the NPU buffer
      auto handle = viplite_adapter_api_.api().map_buffer(viplite_buffer);
      if (handle && host_addr) {
        memcpy(handle, host_addr, tensor_buffer_size);
      }
      viplite_adapter_api_.api().unmap_buffer(viplite_buffer);

      if (viplite_adapter_api_.api().flush_buffer(
              viplite_buffer, VIP_BUFFER_OPER_TYPE_FLUSH) != VIP_SUCCESS) {
        return litert::Unexpected(kLiteRtStatusErrorRuntimeFailure,
                                  "Failed to flush Viplite buffer");
      }
      return viplite_memory_registry_.Register(
          viplite_buffer, tensor_buffer_size, tensor_buffer_offset, host_addr,
          VIP_BUFFER_MEMORY_TYPE_HOST);
    } break;

    case kLiteRtTensorBufferTypeDmaBuf:
#if LITERT_HAS_DMABUF_SUPPORT
      if (auto status =
              LiteRtGetTensorBufferDmaBufBuffer(tensor_buffer, &host_addr, &fd);
          status != kLiteRtStatusOk) {
        return Error(status, "Failed to get DMA-BUF");
      }
#else
      return Error(kLiteRtStatusErrorRuntimeFailure,
                   "DMA-BUF is not supported on this platform");
#endif  // LITERT_HAS_DMABUF_SUPPORT
      tensor_param.memory_type = VIP_BUFFER_MEMORY_TYPE_DMA_BUF;
      PopulateBufferParams(tensor_param, tensor_type);

      if (viplite_adapter_api_.api().create_buffer(
              &tensor_param, sizeof(tensor_param), &viplite_buffer) !=
          VIP_SUCCESS) {
        return litert::Unexpected(
            kLiteRtStatusErrorRuntimeFailure,
            "Failed to create Viplite buffer from DMA-BUF");
      }
      return viplite_memory_registry_.Register(
          viplite_buffer, tensor_buffer_size, tensor_buffer_offset, host_addr,
          VIP_BUFFER_MEMORY_TYPE_DMA_BUF);
      break;

    default:
      LITERT_LOG(LITERT_ERROR, "Unsupported buffer type: %d",
                 tensor_buffer_type);
      return litert::Unexpected(kLiteRtStatusErrorUnsupported);
  }
}

LiteRtDispatchDeviceContextT::VipliteMemoryRegistry::~VipliteMemoryRegistry() {
  for (auto i = 0; i < records_.size(); ++i) {
    auto& record = records_[i];
    if (record.buffer != nullptr) {
      viplite_adapter_api_.api().destroy_buffer(record.buffer);
    }
  }
}

LiteRtTensorBufferHandle
LiteRtDispatchDeviceContextT::VipliteMemoryRegistry::Register(
    vip_buffer buffer, size_t size, size_t offset, void* host_addr,
    vip_buffer_memory_type_e type) {
  int dest_index = -1;
  for (auto i = 0; i < records_.size(); ++i) {
    if (!records_[i].buffer) {
      dest_index = i;
      break;
    }
  }
  if (dest_index < 0) {
    dest_index = records_.size();
    records_.push_back({});
  }
  auto& dest = records_[dest_index];
  dest = {buffer, size, offset, host_addr, type};
  return dest_index;
}

litert::Expected<void>
LiteRtDispatchDeviceContextT::VipliteMemoryRegistry::Unregister(
    LiteRtTensorBufferHandle tensor_buffer_handle) {
  auto record = Find(tensor_buffer_handle);
  if (!record) {
    return record.Error();
  } else {
    auto& mem = (*record)->buffer;
    viplite_adapter_api_.api().destroy_buffer(mem);
    mem = nullptr;
    return {};
  }
}

litert::Expected<LiteRtDispatchDeviceContextT::VipliteMemoryInfo*>
LiteRtDispatchDeviceContextT::VipliteMemoryRegistry::Find(
    LiteRtTensorBufferHandle tensor_buffer_handle) {
  if (tensor_buffer_handle < 0 || tensor_buffer_handle >= records_.size()) {
    return litert::Unexpected(kLiteRtStatusErrorInvalidArgument,
                              "Invalid tensor buffer handle");
  }
  return &records_[tensor_buffer_handle];
}
