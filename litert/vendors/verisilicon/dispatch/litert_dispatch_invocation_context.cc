// LiteRT VeriSilicon Dispatch Plug-in — fixed for Allwinner A733 VIPLite API
//
// The upstream PR #6700 uses the struct-based vip_create_network API:
//   vip_create_network(&param_struct, sizeof(param_struct), &network)
//
// Allwinner's libNBGlinker.so (v2.0.3.2-AW) uses a different signature:
//   vip_create_network(data, size, VIP_CREATE_NETWORK_FROM_MEMORY, &network)
//
// This file patches the dispatch to use the correct Allwinner API.

#include "litert/vendors/verisilicon/dispatch/litert_dispatch_invocation_context.h"

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"
#include "litert/c/internal/litert_logging.h"
#include "litert/c/litert_common.h"
#include "litert/c/litert_model.h"
#include "litert/c/litert_tensor_buffer.h"
#include "litert/cc/litert_expected.h"
#include "litert/core/util/tensor_type_util.h"
#include "litert/vendors/c/litert_dispatch.h"
#include "litert/vendors/verisilicon/dispatch/viplite_adapter_api.h"

using litert::Error;
using litert::Expected;
using litert::Unexpected;

namespace {

template <class X, class Align>
inline constexpr auto Pad(X x, Align align) {
  return ((x + align - 1) / align) * align;
}

}  // namespace

namespace litert {
namespace verisilicon {

litert::Expected<VipliteNetworkT::ModelPtr> VipliteNetworkT::CreateFromByteCode(
    litert::verisilicon::VipliteAdapterApi& viplite_adapter_api,
    const void* exec_bytecode_ptr, size_t exec_bytecode_size, int num_inputs,
    int num_outputs, LiteRtDispatchDeviceContextT::VpmNetworkParam* vpm_param) {
  auto model_ptr = std::make_unique<VipliteNetworkT>(
      viplite_adapter_api, exec_bytecode_ptr, exec_bytecode_size);
  LITERT_RETURN_IF_ERROR(model_ptr->Setup(vpm_param));
  // Identify inputs and outputs
  if (model_ptr->InputCount() != num_inputs ||
      model_ptr->OutputCount() != num_outputs) {
    LITERT_LOG(LITERT_WARNING,
               "Input/output count mismatch: got %d/%d, expected %d/%d",
               model_ptr->InputCount(), model_ptr->OutputCount(),
               num_inputs, num_outputs);
    // Don't fail — just warn. The NBG knows its own I/O counts.
  }

  return model_ptr;
}

VipliteNetworkT::~VipliteNetworkT() {
  if (network_) {
    viplite_adapter_api_.api().finish_network(network_);
    viplite_adapter_api_.api().destroy_network(network_);
  }
  network_ = NULL;
}

litert::Expected<void> VipliteNetworkT::Setup(
    LiteRtDispatchDeviceContextT::VpmNetworkParam* vpm_param) {
  uint32_t input_count = 0;
  uint32_t output_count = 0;

  // Use the Allwinner API: vip_create_network(data, size, type, &network)
  // The NBG bytecode is already in memory (from the flatbuffer or malloc'd buffer)
  if (auto result = viplite_adapter_api_.api().create_network(
          (void*)exec_bytecode_ptr_, exec_bytecode_size_,
          VIP_CREATE_NETWORK_FROM_MEMORY, &network_);
      result != VIP_SUCCESS) {
    return Error(kLiteRtStatusErrorRuntimeFailure,
                 "Failed to create viplite network from NBG bytecode");
  }

  // Query network properties
  LITERT_RETURN_IF_ERROR(Query(VIP_NETWORK_PROP_CORE_COUNT, &core_count_));
  if (vpm_param && vpm_param->core_index >= core_count_) {
    LITERT_LOG(LITERT_WARNING,
               "The core index is large than core count, using default 0");
    core_index_ = 0;
  } else if (vpm_param) {
    core_index_ = vpm_param->core_index;
  }

  LITERT_RETURN_IF_ERROR(Query(VIP_NETWORK_PROP_INPUT_COUNT, &input_count));
  LITERT_RETURN_IF_ERROR(Query(VIP_NETWORK_PROP_OUTPUT_COUNT, &output_count));

  // Note: Allwinner VIPLite does not have VIP_NETWORK_PROP_SET_CORE_INDEX.
  // Core selection is handled via device_id. Single-core devices (A733) don't need it.

  // Set timeout if specified
  if (vpm_param && vpm_param->time_out > 0) {
    LITERT_RETURN_IF_ERROR(Set(VIP_NETWORK_PROP_SET_TIME_OUT, &vpm_param->time_out));
  }

  LITERT_RETURN_IF_ERROR(Prepare());
  input_buffers_.resize(input_count, NULL);
  output_buffers_.resize(output_count, NULL);

  LITERT_LOG(LITERT_INFO, "Network created: %d inputs, %d outputs, %d cores",
             input_count, output_count, core_count_);

  return {};
}

litert::Expected<void> VipliteNetworkT::Query(VipEnum property, void* value) {
  if (auto result =
          viplite_adapter_api_.api().query_network(network_, property, value);
      result != VIP_SUCCESS) {
    return Error(kLiteRtStatusErrorRuntimeFailure,
                 "Failed to query viplite network");
  }
  return {};
}

litert::Expected<void> VipliteNetworkT::Set(VipEnum property, void* value) {
  if (auto result =
          viplite_adapter_api_.api().set_network(network_, property, value);
      result != VIP_SUCCESS) {
    return Error(kLiteRtStatusErrorRuntimeFailure,
                 "Failed to set viplite network");
  }
  return {};
}

litert::Expected<void> VipliteNetworkT::Prepare() {
  if (auto result = viplite_adapter_api_.api().prepare_network(network_);
      result != VIP_SUCCESS) {
    return Error(kLiteRtStatusErrorRuntimeFailure,
                 "Failed to prepare viplite network");
  }
  return {};
}

litert::Expected<void> VipliteNetworkT::Run() {
  if (auto result = viplite_adapter_api_.api().run_network(network_);
      result != VIP_SUCCESS) {
    return Error(kLiteRtStatusErrorRuntimeFailure,
                 absl::StrFormat("Failed to run viplite network (status %d)", result));
  }
  return {};
}

litert::Expected<void> VipliteNetworkT::Trigger() {
  if (auto result = viplite_adapter_api_.api().trigger_network(network_);
      result != VIP_SUCCESS) {
    return Error(kLiteRtStatusErrorRuntimeFailure,
                 "Failed to trigger viplite network");
  }
  return {};
}

litert::Expected<void> VipliteNetworkT::Wait() {
  if (auto result = viplite_adapter_api_.api().wait_network(network_);
      result != VIP_SUCCESS) {
    return Error(kLiteRtStatusErrorRuntimeFailure,
                 "Failed to wait viplite network");
  }
  return {};
}

litert::Expected<void> VipliteNetworkT::Cancel() {
  if (auto result = viplite_adapter_api_.api().cancel_network(network_);
      result != VIP_SUCCESS) {
    return Error(kLiteRtStatusErrorRuntimeFailure,
                 "Failed to cancel viplite network");
  }
  return {};
}

litert::Expected<void> VipliteNetworkT::QueryInput(uint32_t index,
                                                   VipEnum property,
                                                   void* value) {
  if (auto result = viplite_adapter_api_.api().query_input(network_, index,
                                                           property, value);
      result != VIP_SUCCESS) {
    return Error(kLiteRtStatusErrorRuntimeFailure,
                 absl::StrFormat("Failed to query input %d", index));
  }
  return {};
}

litert::Expected<void> VipliteNetworkT::QueryOutput(uint32_t index,
                                                    VipEnum property,
                                                    void* value) {
  if (auto result = viplite_adapter_api_.api().query_output(network_, index,
                                                            property, value);
      result != VIP_SUCCESS) {
    return Error(kLiteRtStatusErrorRuntimeFailure,
                 absl::StrFormat("Failed to query output %d", index));
  }
  return {};
}

litert::Expected<void> VipliteNetworkT::SetInput(uint32_t index,
                                                 VipBuffer buffer) {
  if (auto result =
          viplite_adapter_api_.api().set_input(network_, index, buffer);
      result != VIP_SUCCESS) {
    return Error(kLiteRtStatusErrorRuntimeFailure,
                 absl::StrFormat("Failed to set input %d", index));
  }
  input_buffers_.at(index) = buffer;
  return {};
}

litert::Expected<void> VipliteNetworkT::SetOutput(uint32_t index,
                                                  VipBuffer buffer) {
  if (index >= output_buffers_.size()) {
    LITERT_LOG(LITERT_WARNING,
               "Skipping output %d: NBG only has %d outputs",
               index, output_buffers_.size());
    return {};
  }
  if (auto result =
          viplite_adapter_api_.api().set_output(network_, index, buffer);
      result != VIP_SUCCESS) {
    return Error(kLiteRtStatusErrorRuntimeFailure,
                 absl::StrFormat("Failed to set output %d (status %d)",
                                 index, result));
  }
  output_buffers_.at(index) = buffer;
  return {};
}

litert::Expected<VipliteNetworkT::VipBuffer> VipliteNetworkT::GetInput(
    uint32_t index) {
  return input_buffers_.at(index);
}

litert::Expected<VipliteNetworkT::VipBuffer> VipliteNetworkT::GetOutput(
    uint32_t index) {
  return output_buffers_.at(index);
}

}  // namespace verisilicon
}  // namespace litert

Expected<LiteRtDispatchInvocationContextT::Ptr>
LiteRtDispatchInvocationContextT::Create(
    litert::verisilicon::VipliteAdapterApi& viplite_adapter_api,
    LiteRtDispatchDeviceContext device_context,
    LiteRtDispatchExecutableType exec_type,
    const LiteRtMemBuffer* exec_bytecode_buffer, const char* function_name,
    int num_inputs, int num_outputs) {
  (void)function_name;
  const char* exec_bytecode_ptr =
      static_cast<const char*>(exec_bytecode_buffer->base_addr) +
      exec_bytecode_buffer->offset;
  auto exec_bytecode_size = exec_bytecode_buffer->size;
  LiteRtDispatchDeviceContextT::VpmNetworkParam vpm_param = {0};
  device_context->GetVpmNetworkParam(&vpm_param);

  LITERT_ASSIGN_OR_RETURN(
      litert::verisilicon::VipliteNetworkT::ModelPtr model,
      litert::verisilicon::VipliteNetworkT::CreateFromByteCode(
          viplite_adapter_api, exec_bytecode_ptr, exec_bytecode_size,
          num_inputs, num_outputs, &vpm_param));

  return Ptr(new LiteRtDispatchInvocationContextT(
      viplite_adapter_api, device_context, std::move(model), num_inputs,
      num_outputs));
}

namespace {

Expected<LiteRtTensorBufferRequirements> GetTensorBufferRequirements(
    const LiteRtRankedTensorType& tensor_type) {
  if (tensor_type.layout.has_strides) {
    return Unexpected(kLiteRtStatusErrorRuntimeFailure,
                      "Tensor strides are not supported on Verisilicon NPU");
  }

  int num_supported_tensor_buffer_types =
      sizeof(LiteRtDispatchDeviceContextT::kSupportedTensorBufferTypes) /
      sizeof(LiteRtDispatchDeviceContextT::kSupportedTensorBufferTypes[0]);

  auto buffer_size = litert::internal::GetNumPackedBytes(tensor_type);
  if (!buffer_size) {
    return Unexpected(buffer_size.Error());
  }
  size_t padded_buffer_size =
      Pad(*buffer_size, litert::verisilicon::kVipliteCacheLineAlignment);
  LiteRtTensorBufferRequirements requirements;
  if (auto status = LiteRtCreateTensorBufferRequirementsWithAlignment(
          num_supported_tensor_buffer_types,
          LiteRtDispatchDeviceContextT::kSupportedTensorBufferTypes,
          padded_buffer_size, /*num_strides=*/0, /*strides=*/nullptr,
          litert::verisilicon::kVipliteAddressAlignment, &requirements);
      status != kLiteRtStatusOk) {
    return Unexpected(kLiteRtStatusErrorRuntimeFailure,
                      "Failed to create tensor buffer requirements");
  }

  return requirements;
}
}  // namespace

Expected<LiteRtTensorBufferRequirements>
LiteRtDispatchInvocationContextT::GetInputRequirements(
    int input_index, const LiteRtRankedTensorType& tensor_type) {
  return GetTensorBufferRequirements(tensor_type);
}

Expected<LiteRtTensorBufferRequirements>
LiteRtDispatchInvocationContextT::GetOutputRequirements(
    int output_index, const LiteRtRankedTensorType& tensor_type) {
  return GetTensorBufferRequirements(tensor_type);
}

Expected<void> LiteRtDispatchInvocationContextT::AttachInput(
    int graph_input_index, LiteRtTensorBufferHandle tensor_buffer_handle) {
  // Pegasus may optimize away inputs during NBG compilation.
  if (graph_input_index >= model_->InputCount()) {
    LITERT_LOG(LITERT_WARNING,
               "Skipping input %d: NBG only has %d inputs",
               graph_input_index, model_->InputCount());
    return {};
  }
  auto viplite_memory_info =
      device_context_->GetVipliteMemoryInfo(tensor_buffer_handle);
  if (!viplite_memory_info) {
    return litert::Error(viplite_memory_info.Error());
  }

  // Try setting the input with the existing buffer first.
  if (model_->SetInput(graph_input_index, viplite_memory_info->buffer)) {
    input_buffers_handles_.at(graph_input_index) = tensor_buffer_handle;
    return {};
  }

  // Buffer format mismatch. Query NBG's expected format and recreate.
  LITERT_LOG(LITERT_WARNING,
             "Input %d buffer mismatch, querying NBG format and recreating",
             graph_input_index);

  vip_buffer_format_e nbg_format = VIP_BUFFER_FORMAT_FP32;
  uint32_t nbg_num_dims = 0;
  uint32_t nbg_sizes[8] = {};

  model_->QueryInput(graph_input_index, VIP_BUFFER_PROP_DATA_FORMAT, &nbg_format);
  model_->QueryInput(graph_input_index, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &nbg_num_dims);
  model_->QueryInput(graph_input_index, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, nbg_sizes);

  vip_buffer_create_params_t nbg_params = {};
  nbg_params.memory_type = viplite_memory_info->create_type;
  nbg_params.data_format = nbg_format;
  nbg_params.num_of_dims = nbg_num_dims;
  for (uint32_t i = 0; i < nbg_num_dims && i < 6; i++) {
    nbg_params.sizes[i] = nbg_sizes[i];
  }

  auto nbg_buffer = device_context_->CreateNbgBuffer(
      nbg_params, viplite_memory_info->size,
      viplite_memory_info->create_type == VIP_BUFFER_MEMORY_TYPE_HOST
          ? viplite_memory_info->host_addr
          : nullptr);
  if (!nbg_buffer) {
    return litert::Error(nbg_buffer.Error());
  }

  LITERT_RETURN_IF_ERROR(
      model_->SetInput(graph_input_index, *nbg_buffer));
  nbg_input_buffers_.at(graph_input_index) = *nbg_buffer;
  input_buffers_handles_.at(graph_input_index) = tensor_buffer_handle;
  return {};
}

Expected<void> LiteRtDispatchInvocationContextT::AttachOutput(
    int graph_output_index, LiteRtTensorBufferHandle tensor_buffer_handle) {
  // Pegasus may optimize away outputs during NBG compilation.
  if (graph_output_index >= model_->OutputCount()) {
    LITERT_LOG(LITERT_WARNING,
               "Skipping output %d: NBG only has %d outputs",
               graph_output_index, model_->OutputCount());
    return {};
  }
  auto viplite_memory_info =
      device_context_->GetVipliteMemoryInfo(tensor_buffer_handle);
  if (!viplite_memory_info) {
    return litert::Error(viplite_memory_info.Error());
  }

  // Try setting the output with the existing buffer first.
  if (model_->SetOutput(graph_output_index, viplite_memory_info->buffer)) {
    // Success — buffer format matches NBG expectation.
    output_buffers_handles_.at(graph_output_index) = tensor_buffer_handle;
    return {};
  }

  // Buffer format mismatch (status -3). Query the NBG's actual expected
  // format and create a matching buffer.
  LITERT_LOG(LITERT_WARNING,
             "Output %d buffer mismatch, querying NBG format and recreating",
             graph_output_index);

  vip_buffer_format_e nbg_format = VIP_BUFFER_FORMAT_FP32;
  uint32_t nbg_num_dims = 0;
  uint32_t nbg_sizes[8] = {};

  auto q_fmt = model_->QueryOutput(graph_output_index,
                                    VIP_BUFFER_PROP_DATA_FORMAT, &nbg_format);
  auto q_nd = model_->QueryOutput(graph_output_index,
                                   VIP_BUFFER_PROP_NUM_OF_DIMENSION,
                                   &nbg_num_dims);
  auto q_sz = model_->QueryOutput(graph_output_index,
                                   VIP_BUFFER_PROP_SIZES_OF_DIMENSION,
                                   nbg_sizes);

  if (!q_fmt || !q_nd || !q_sz) {
    return litert::Error(
        kLiteRtStatusErrorRuntimeFailure,
        absl::StrFormat("Failed to query NBG output %d format", graph_output_index));
  }

  LITERT_LOG(LITERT_INFO,
             "NBG output %d: format=%d, dims=%d, sizes=[%u,%u,%u,%u,%u,%u]",
             graph_output_index, nbg_format, nbg_num_dims,
             nbg_sizes[0], nbg_sizes[1], nbg_sizes[2],
             nbg_sizes[3], nbg_sizes[4], nbg_sizes[5]);

  // Create a new buffer matching the NBG's expected format.
  vip_buffer_create_params_t nbg_params = {};
  nbg_params.memory_type = viplite_memory_info->create_type;
  nbg_params.data_format = nbg_format;
  nbg_params.num_of_dims = nbg_num_dims;
  for (uint32_t i = 0; i < nbg_num_dims && i < 6; i++) {
    nbg_params.sizes[i] = nbg_sizes[i];
  }

  auto nbg_buffer = device_context_->CreateNbgBuffer(
      nbg_params, viplite_memory_info->size,
      viplite_memory_info->create_type == VIP_BUFFER_MEMORY_TYPE_HOST
          ? viplite_memory_info->host_addr
          : nullptr);
  if (!nbg_buffer) {
    return litert::Error(nbg_buffer.Error());
  }

  LITERT_RETURN_IF_ERROR(
      model_->SetOutput(graph_output_index, *nbg_buffer));
  nbg_output_buffers_.at(graph_output_index) = *nbg_buffer;
  output_buffers_handles_.at(graph_output_index) = tensor_buffer_handle;
  return {};
}

Expected<void> LiteRtDispatchInvocationContextT::DetachInput(
    int graph_input_index, LiteRtTensorBufferHandle tensor_buffer_handle) {
  return {};
}

Expected<void> LiteRtDispatchInvocationContextT::DetachOutput(
    int graph_output_index, LiteRtTensorBufferHandle tensor_buffer_handle) {
  return {};
}

Expected<void> LiteRtDispatchInvocationContextT::Invoke() {
  // Ensure all NBG outputs have buffers set. Pegasus may produce more outputs
  // than the partition declared (e.g., intermediate values). Create dummy
  // buffers for any unset outputs so vip_run_network doesn't fail with
  // VIP_ERROR_NETWORK_INCOMPATIBLE (-10).
  for (size_t i = 0; i < model_->OutputCount(); i++) {
    if (i >= nbg_output_buffers_.size() || !nbg_output_buffers_[i]) {
      // No buffer set for this output — query NBG's expected format and create one
      vip_buffer_format_e fmt = VIP_BUFFER_FORMAT_FP32;
      uint32_t num_dims = 0;
      uint32_t sizes[8] = {};
      if (model_->QueryOutput(i, VIP_BUFFER_PROP_DATA_FORMAT, &fmt) &&
          model_->QueryOutput(i, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &num_dims) &&
          model_->QueryOutput(i, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, sizes)) {
        vip_buffer_create_params_t params = {};
        params.memory_type = VIP_BUFFER_MEMORY_TYPE_HOST;
        params.data_format = fmt;
        params.num_of_dims = num_dims > 0 ? num_dims : 1;
        for (uint32_t j = 0; j < num_dims && j < 6; j++) {
          params.sizes[j] = sizes[j];
        }
        auto dummy = device_context_->CreateNbgBuffer(params, 0, nullptr);
        if (dummy) {
          model_->SetOutput(i, *dummy);
          if (i >= nbg_output_buffers_.size()) {
            nbg_output_buffers_.resize(i + 1, nullptr);
          }
          nbg_output_buffers_[i] = *dummy;
        }
      }
    }
  }

  // Ensure all NBG inputs have buffers set. Same reasoning.
  for (size_t i = 0; i < model_->InputCount(); i++) {
    if (i >= nbg_input_buffers_.size() || !nbg_input_buffers_[i]) {
      // Check if there's a registered handle we can use
      if (i < input_buffers_handles_.size()) {
        auto info = device_context_->GetVipliteMemoryInfo(input_buffers_handles_[i]);
        if (info && model_->SetInput(i, info->buffer)) {
          if (i >= nbg_input_buffers_.size()) {
            nbg_input_buffers_.resize(i + 1, nullptr);
          }
          nbg_input_buffers_[i] = info->buffer;
        }
      }
    }
  }

  for (size_t i = 0; i < model_->InputCount(); i++) {
    if (i >= input_buffers_handles_.size()) break;
    auto viplite_memory_info =
        device_context_->GetVipliteMemoryInfo(input_buffers_handles_[i]);
    if (!viplite_memory_info) continue;
    // Use NBG-matching buffer if one was created, otherwise use registered buffer
    vip_buffer active_buf = (i < nbg_input_buffers_.size() && nbg_input_buffers_[i])
                                ? nbg_input_buffers_[i]
                                : viplite_memory_info->buffer;
    if (viplite_memory_info->create_type == VIP_BUFFER_MEMORY_TYPE_HOST) {
      // Copy only the minimum of host size and NBG buffer size to avoid overflow
      size_t nbg_size = viplite_adapter_api_.api().get_buffer_size(active_buf);
      size_t copy_size = std::min(nbg_size, viplite_memory_info->size);
      auto handle = viplite_adapter_api_.api().map_buffer(active_buf);
      memcpy(handle, viplite_memory_info->host_addr, copy_size);
      viplite_adapter_api_.api().unmap_buffer(active_buf);
    }
    viplite_adapter_api_.api().flush_buffer(active_buf,
                                            VIP_BUFFER_OPER_TYPE_FLUSH);
  }
  LITERT_RETURN_IF_ERROR(model_->Run());
  for (size_t i = 0; i < model_->OutputCount(); i++) {
    if (i >= output_buffers_handles_.size()) break;
    auto viplite_memory_info =
        device_context_->GetVipliteMemoryInfo(output_buffers_handles_[i]);
    if (!viplite_memory_info) continue;
    vip_buffer active_buf = (i < nbg_output_buffers_.size() && nbg_output_buffers_[i])
                                ? nbg_output_buffers_[i]
                                : viplite_memory_info->buffer;
    viplite_adapter_api_.api().flush_buffer(active_buf,
                                            VIP_BUFFER_OPER_TYPE_INVALIDATE);
    if (viplite_memory_info->create_type == VIP_BUFFER_MEMORY_TYPE_HOST) {
      // Copy only the minimum to avoid overflow when NBG format differs
      size_t nbg_size = viplite_adapter_api_.api().get_buffer_size(active_buf);
      size_t copy_size = std::min(nbg_size, viplite_memory_info->size);
      auto handle = viplite_adapter_api_.api().map_buffer(active_buf);
      memcpy(viplite_memory_info->host_addr, handle, copy_size);
      viplite_adapter_api_.api().unmap_buffer(active_buf);
    }
  }
  return {};
}
