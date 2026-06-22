# Copyright 2026 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

# copybara:uncomment package(default_applicable_licenses = ["@org_tensorflow//tensorflow:license"])

cc_library(
    name = "util",
    srcs = [
    ],
    hdrs = [
        "util.h",
    ],
    tags = ["no_oss"],
    deps = [
        "//tflite/experimental/shlo/legacy:float",
    ],
)

cc_binary(
    name = "shlo_benchmark",
    srcs = [
        "shlo_benchmark.cc",
    ],
    tags = ["no_oss"],
    deps = [
        ":util",
        "//tflite/experimental/shlo/legacy:shlo",
        "//tflite/experimental/shlo/legacy/test:util",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_benchmark//:benchmark",
    ],
)

cc_binary(
    name = "xnn_benchmark",
    srcs = [
        "xnn_benchmark.cc",
    ],
    data = [
    ],
    tags = ["no_oss"],
    deps = [
        ":util",
        "//tflite/experimental/shlo/legacy:float",
        "@XNNPACK",
        "@com_google_absl//absl/log",
        "@com_google_benchmark//:benchmark",
    ],
)
