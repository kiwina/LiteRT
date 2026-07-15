#ifndef LITERT_VENDORS_VERISILICON_COMPILER_SUPPORTED_OPS_H_
#define LITERT_VENDORS_VERISILICON_COMPILER_SUPPORTED_OPS_H_

#include "litert/c/litert_op_code.h"

namespace litert {
namespace verisilicon {

// Ops that the Vivante VIP9000 NPU can accelerate via NBG.
// Conservative set proven via MobileNet/ShuffleNet + LLM-relevant ops.
constexpr LiteRtOpCode kSupportedOps[] = {
    // Basic ops (proven with MobileNet)
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
    kLiteRtOpCodeTflMean,
    kLiteRtOpCodeTflFullyConnected,
    kLiteRtOpCodeTflSoftmax,
    kLiteRtOpCodeTflTranspose,
    kLiteRtOpCodeTflPad,
    // LLM-relevant ops (pegasus schema supports these)
    kLiteRtOpCodeTflDequantize,
    kLiteRtOpCodeTflMinimum,
    kLiteRtOpCodeTflMaximum,
    kLiteRtOpCodeTflRsqrt,
    kLiteRtOpCodeTflLog,
    kLiteRtOpCodeTflTanh,
    kLiteRtOpCodeTflSplit,
    kLiteRtOpCodeTflGather,
    kLiteRtOpCodeTflResizeNearestNeighbor,
    kLiteRtOpCodeTflStridedSlice,
    kLiteRtOpCodeTflMirrorPad,
    kLiteRtOpCodeTflLess,
    kLiteRtOpCodeTflReduceMin,
    kLiteRtOpCodeTflReduceAny,
    kLiteRtOpCodeTflSum,
    kLiteRtOpCodeTflFill,
    kLiteRtOpCodeTflConv3dTranspose,
    kLiteRtOpCodeTflSpaceToBatchNd,
    kLiteRtOpCodeTflMatrixSetDiag,
    kLiteRtOpCodeTflLogicalNot,
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
