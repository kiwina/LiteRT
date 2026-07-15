# Copyright 2025 Google LLC.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# VeriSilicon NPU Build Configuration
# Targets the Allwinner A733 / Vivante VIP9000NANO-DI NPU
# (Orange Pi Zero 3W)
cmake_minimum_required(VERSION 3.20)

message(STATUS "Configuring VeriSilicon NPU support")

option(LITERT_ENABLE_VERISILICON "Enable VeriSilicon NPU build" ON)
if(NOT LITERT_ENABLE_VERISILICON)
  message(STATUS "Skipping VeriSilicon: LITERT_ENABLE_VERISILICON=OFF")
  return()
endif()

# Add subdirectories for modular build (matches Samsung pattern)
add_subdirectory(dispatch)
add_subdirectory(compiler)
