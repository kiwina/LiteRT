# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0

load("//litert/build_common:litert_build_defs.bzl", "litert_dynamic_lib")

package(
    default_visibility = ["//litert:litert_public"],
)

# The compiler plugin .so that apply_plugin loads.
# Compiles supported ops to NBG bytecode via the ACUITY pegasus toolchain.
litert_dynamic_lib(
    name = "compiler_plugin_so",
    srcs = ["compiler_plugin.cc"],
    hdrs = [
        "nbg_compiler.h",
        "supported_ops.h",
        "//litert/vendors/c:litert_compiler_plugin.h",
    ],
    export_litert_only = True,
    linkopts = select({
        "//litert:windows": [],
        "//conditions:default": ["-Wl,-rpath,$$ORIGIN"],
    }),
    linkstatic = 1,
    shared_lib_name = "compiler_plugin_verisilicon",
    so_name = "libLiteRtCompilerPluginVerisilicon.so",
    deps = [
        "//litert/c:litert_common",
        "//litert/c:litert_model",
        "//litert/c:litert_op_code",
        "//litert/c/internal:litert_compiler_context_header",
        "//litert/c/internal:litert_logging",
        "//litert/vendors/c:litert_compiler_plugin",
    ],
)
