// Copyright 2025 Google LLC.
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

#ifndef THIRD_PARTY_ODML_LITERT_ML_DRIFT_DELEGATE_DELEGATE_UTILS_H_
#define THIRD_PARTY_ODML_LITERT_ML_DRIFT_DELEGATE_DELEGATE_UTILS_H_

#include <cstdint>
#include <functional>
#include <memory>

#include "absl/types/span.h"  // from @com_google_absl
#include "litert/c/internal/litert_runtime_context.h"
#include "ml_drift_delegate/delegate/delegate_data.h"
#include "tflite/core/c/common.h"

namespace litert::ml_drift {

bool IsAsyncExecutionMode(TfLiteContext* context,
                          const LiteRtRuntimeContext* runtime_context);

// Returns tensor maps from the delegate data. If the shared tensor maps are
// provided by the client, returns the shared tensor maps from the client.
::ml_drift::ValueIdToSharedTensorMap& GetBufferIdToSpatialTensorMap(
    MlDriftDelegateData& delegate_data);
::ml_drift::ValueIdToSharedTensorMap& GetQuantParamIdToSpatialTensorMap(
    MlDriftDelegateData& delegate_data);

}  // namespace litert::ml_drift

#endif  // THIRD_PARTY_ODML_LITERT_ML_DRIFT_DELEGATE_DELEGATE_UTILS_H_
