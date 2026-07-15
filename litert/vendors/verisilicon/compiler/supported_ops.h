#ifndef LITERT_VENDORS_VERISILICON_COMPILER_SUPPORTED_OPS_H_
#define LITERT_VENDORS_VERISILICON_COMPILER_SUPPORTED_OPS_H_

#include "litert/c/litert_op_code.h"

namespace litert {
namespace verisilicon {

// Ops that the Vivante VIP9000 NPU can accelerate via NBG.
// Start conservative — only ops we KNOW compile successfully through ACUITY
// (proven via ShuffleNet/MobileNet in the model zoo).
// Expand as we verify more ops against the NBG compiler.
constexpr LiteRtOpCode kSupportedOps[] = {
    kLiteRtOpCodeTflConv2d,
    kLiteRtOpCodeTflDepthwiseConv2d,
    kLiteRtOpCodeTflAdd,
    kLiteRtOpCodeTflMul,
    kLiteRtOpCodeTflSub,
    kLiteRtOpCodeTflRelu,
    kLiteRtOpCodeTflRelu6,
    kLiteRtOpCodeTflConcatenation,
    kLiteRtOpCodeTflReshape,
    kLiteRtOpCodeTflAveragePool2d,
    kLiteRtOpCodeTflMaxPool2d,
    kLiteRtOpCodeTflMean,  // global avg pooling is often implemented as Mean
    kLiteRtOpCodeTflFullyConnected,
    kLiteRtOpCodeTflSoftmax,
    kLiteRtOpCodeTflTranspose,
    kLiteRtOpCodeTflPad,
};

constexpr size_t kNumSupportedOps = sizeof(kSupportedOps) / sizeof(kSupportedOps[0]);

inline bool IsOpSupported(LiteRtOpCode code) {
  for (size_t i = 0; i < kNumSupportedOps; ++i) {
    if (code == kSupportedOps[i]) {
      return true;
    }
  }
  return false;
}

}  // namespace verisilicon
}  // namespace litert

#endif  // LITERT_VENDORS_VERISILICON_COMPILER_SUPPORTED_OPS_H_
