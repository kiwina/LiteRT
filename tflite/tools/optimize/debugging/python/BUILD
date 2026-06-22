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

# QuantizationDebugger for TFLite accuracy tooling.
load("@org_tensorflow//tensorflow:pytype.default.bzl", "pytype_strict_library")
load("@org_tensorflow//tensorflow:tensorflow.bzl", "py_test")

package(
    # copybara:uncomment default_applicable_licenses = ["@org_tensorflow//tensorflow:license"],
    licenses = ["notice"],
)

pytype_strict_library(
    name = "debugger",
    srcs = ["debugger.py"],
    visibility = ["//visibility:public"],
    deps = [
        "//tflite/python:convert",
        "//tflite/python:interpreter",
        "//tflite/python/metrics",
        "@org_tensorflow//tensorflow/python/util:tf_export",
        "@org_tensorflow//third_party/py/numpy",
    ],
)

py_test(
    name = "debugger_test",
    srcs = [
        "debugger_test.py",
    ],
    strict_deps = True,
    deps = [
        ":debugger",
        "@absl_py//absl/testing:parameterized",
        #internal proto upb dep
        "@org_tensorflow//third_party/py/numpy",
        "@org_tensorflow//tensorflow:tensorflow_py",
        "//tflite/python:convert",
        "//tflite/python:lite",
        "//tflite/python/metrics",
        "@org_tensorflow//tensorflow/python/framework:test_lib",
        "@org_tensorflow//tensorflow/python/platform:client_testlib",
        "@org_tensorflow//tensorflow/python/trackable:autotrackable",
    ],
)
