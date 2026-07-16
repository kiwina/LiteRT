#ifndef LITERT_VENDORS_VERISILICON_COMPILER_NBG_COMPILER_H_
#define LITERT_VENDORS_VERISILICON_COMPILER_NBG_COMPILER_H_

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Use fprintf instead of LITERT_LOG to avoid versioned symbol dependencies
#define VS_LOG(fmt, ...) fprintf(stderr, "[VeriSilicon] " fmt "\n", ##__VA_ARGS__)
#define VS_LOG_ERR(fmt, ...) fprintf(stderr, "[VeriSilicon ERROR] " fmt "\n", ##__VA_ARGS__)

namespace litert {
namespace verisilicon {

// Compiles a tflite flatbuffer partition to VeriSilicon NBG bytecode
// by shelling out to the ACUITY pegasus toolchain.
//
// The pegasus tool:
//   1. Imports the tflite → acuity JSON+data
//   2. Exports via ovxlib → NBG (network_binary.nb)
//
// Environment variables (set by the user or apply_plugin):
//   LITERT_VERISILICON_PEGASUS    - path to pegasus.py (required)
//   LITERT_VERISILICON_VIV_SDK    - path to Vivante IDE (required)
//   LITERT_VERISILICON_ACUITY_PATH - alternative: path to acuity bin dir
//   LITERT_VERISILICON_OPTIMIZE   - target config (default: VIP9000NANODI_PLUS_PID0X1000003B)
//   LITERT_VERISILICON_DTYPE      - data type (default: float)
//   LITERT_VERISILICON_KEEP_TEMP  - if set, don't delete temp files
class NbgCompiler {
 public:
  NbgCompiler() {
    // Read config from environment
    if (const char* p = getenv("LITERT_VERISILICON_PEGASUS")) {
      pegasus_path_ = p;
    } else if (const char* p = getenv("LITERT_VERISILICON_ACUITY_PATH")) {
      pegasus_path_ = std::string(p) + "/pegasus.py";
    }
    if (const char* p = getenv("LITERT_VERISILICON_VIV_SDK")) {
      viv_sdk_ = p;
    }
    if (const char* p = getenv("LITERT_VERISILICON_OPTIMIZE")) {
      optimize_ = p;
    } else {
      optimize_ = "VIP9000NANODI_PLUS_PID0X1000003B";
    }
    if (const char* p = getenv("LITERT_VERISILICON_DTYPE")) {
      dtype_ = p;
    } else {
      dtype_ = "float32";  // default: FP32 I/O (pegasus "float" = FP16!)
    }
    keep_temp_ = getenv("LITERT_VERISILICON_KEEP_TEMP") != nullptr;
  }

  // Compile a tflite flatbuffer to NBG bytes.
  // Returns empty vector on failure, NBG bytes on success.
  std::vector<uint8_t> Compile(const uint8_t* tflite_data, size_t tflite_size,
                                const std::string& partition_name) {
    if (pegasus_path_.empty()) {
      VS_LOG_ERR(
                 "LITERT_VERISILICON_PEGASUS or LITERT_VERISILICON_ACUITY_PATH "
                 "not set. Cannot compile NBG.");
      return {};
    }
    if (viv_sdk_.empty()) {
      VS_LOG_ERR(
                 "LITERT_VERISILICON_VIV_SDK not set. Cannot compile NBG.");
      return {};
    }

    // Create temp working directory
    auto tmp_dir = std::filesystem::temp_directory_path() /
                   ("litert_nbg_" + partition_name);
    std::filesystem::create_directories(tmp_dir);

    // Write tflite to temp file
    auto tflite_path = tmp_dir / "partition.tflite";
    {
      std::ofstream f(tflite_path, std::ios::binary);
      f.write(reinterpret_cast<const char*>(tflite_data),
              static_cast<std::streamsize>(tflite_size));
    }

    // Fix the partition tflite's operator code table.
    // LiteRT's LiteRtSerializeModel has a bug: it writes operator code indices
    // from the parent model but only copies 1 entry (DISPATCH_OP) to the
    // partition's operator code table. This causes pegasus import to crash.
    // Fix: use fix_opcodes.py which copies operator codes from the original model.
    {
      // Copy the fixer script to the temp dir
      auto fixer_src = std::filesystem::path(__FILE__).parent_path() / "fix_opcodes.py";
      auto fixer_dst = tmp_dir / "fix_opcodes.py";
      if (std::filesystem::exists(fixer_src)) {
        std::filesystem::copy_file(fixer_src, fixer_dst,
                                    std::filesystem::copy_options::overwrite_existing);
      }
      std::ostringstream cmd;
      cmd << "cd " << tmp_dir.string() << " && python3 fix_opcodes.py partition.tflite 2>&1";
      std::string output;
      RunCommand(cmd.str(), output);
      if (output.find("FIXED") != std::string::npos) {
        VS_LOG("Fixed partition tflite: %s", output.c_str());
      } else if (output.find("OPCODE_TABLE_OK") == std::string::npos) {
        VS_LOG_ERR("Opcode table fix failed: %s", output.c_str());
      }
    }

    // Step 1: pegasus import tflite → JSON + data
    // All pegasus commands run with CWD = tmp_dir.
    auto json_path = tmp_dir / "partition.json";
    auto data_path = tmp_dir / "partition.data";
    {
      std::ostringstream cmd;
      cmd << "cd " << tmp_dir.string() << " && python3 " << pegasus_path_
          << " import tflite"
          << " --model " << tflite_path.string()
          << " --output-model " << json_path.string()
          << " --output-data " << data_path.string()
          << " 2>&1";
      std::string output;
      int rc = RunCommand(cmd.str(), output);
      if (rc != 0) {
        VS_LOG_ERR( "pegasus import failed (rc=%d): %s", rc,
                   output.c_str());
        if (!keep_temp_) std::filesystem::remove_all(tmp_dir);
        return {};
      }
      VS_LOG( "pegasus import succeeded for %s",
                 partition_name.c_str());
    }

    // Step 1b: pegasus generate inputmeta
    auto inputmeta_path = tmp_dir / "partition_inputmeta.yml";
    {
      std::ostringstream cmd;
      cmd << "cd " << tmp_dir.string() << " && python3 " << pegasus_path_
          << " generate inputmeta"
          << " --model " << json_path.string()
          << " --separated-database"
          << " --input-meta-output " << inputmeta_path.string()
          << " 2>&1";
      std::string output;
      int rc = RunCommand(cmd.str(), output);
      if (rc != 0) {
        VS_LOG_ERR( "pegasus generate inputmeta failed (rc=%d): %s", rc,
                   output.c_str());
        if (!keep_temp_) std::filesystem::remove_all(tmp_dir);
        return {};
      }
    }

    // Step 1c: pegasus generate postprocess-file
    // The postprocess file adds a dequantize node that converts the NPU's
    // internal quantized output to FP32 at the NBG boundary. Without this,
    // quantized models output FP16 which doesn't match LiteRT's FP32 buffers.
    // We enable add_postproc_node + force_float32 (matching the official
    // Allwinner model zoo config_yml.py pipeline).
    auto postprocess_path = tmp_dir / "partition_postprocess.yml";
    {
      std::ostringstream cmd;
      cmd << "cd " << tmp_dir.string() << " && python3 " << pegasus_path_
          << " generate postprocess-file"
          << " --model " << json_path.string()
          << " --postprocess-file-output " << postprocess_path.string()
          << " 2>&1";
      std::string output;
      int rc = RunCommand(cmd.str(), output);
      if (rc != 0) {
        VS_LOG_ERR( "pegasus generate postprocess-file failed (rc=%d): %s", rc,
                   output.c_str());
        if (!keep_temp_) std::filesystem::remove_all(tmp_dir);
        return {};
      }

      // Enable the postproc node: add_postproc_node: false → true
      // This activates force_float32: true on output tensors.
      std::ifstream infile(postprocess_path);
      std::string content((std::istreambuf_iterator<char>(infile)),
                          std::istreambuf_iterator<char>());
      infile.close();
      // Replace all occurrences
      size_t pos = 0;
      while ((pos = content.find("add_postproc_node: false", pos)) != std::string::npos) {
        content.replace(pos, 24, "add_postproc_node: true");
        pos += 23;
      }
      std::ofstream outfile(postprocess_path);
      outfile << content;
      outfile.close();
      VS_LOG( "Enabled postproc node (force_float32) in %s",
                 postprocess_path.string().c_str());
    }

    // Step 2: pegasus export ovxlib → NBG
    // The model is taken AS-IS. If the user wants int16 quantization,
    // they quantize the model BEFORE feeding it to LiteRT. The plugin
    // passes --model-quantize if the user provides a .quantize file via
    // LITERT_VERISILICON_QUANTIZE env var, otherwise compiles as-is.
    {
      auto output_path = tmp_dir / "partition_nbg";
      std::ostringstream cmd;
      cmd << "cd " << tmp_dir.string() << " && python3 " << pegasus_path_
          << " export ovxlib"
          << " --pack-nbg-unify"
          << " --optimize " << optimize_
          << " --viv-sdk " << viv_sdk_
          << " --model " << json_path.string()
          << " --model-data " << data_path.string()
          << " --postprocess-file " << postprocess_path.string()
          << " --target-ide-project linux64";

      // If user pre-quantized the model, pass the quantize file + dtype
      const char* quantize_file = getenv("LITERT_VERISILICON_QUANTIZE");
      if (quantize_file && std::filesystem::exists(quantize_file)) {
        cmd << " --model-quantize " << quantize_file
            << " --dtype quantized";
        VS_LOG( "Using pre-quantized model: %s", quantize_file);
      } else {
        cmd << " --dtype float32";
      }

      cmd << " --with-input-meta " << inputmeta_path.string()
          << " --output-path " << output_path.string()
          << " 2>&1";
      std::string output;
      int rc = RunCommand(cmd.str(), output);
      if (rc != 0) {
        VS_LOG_ERR( "pegasus export ovxlib failed (rc=%d): %s",
                   rc, output.c_str());
        if (!keep_temp_) std::filesystem::remove_all(tmp_dir);
        return {};
      }
      VS_LOG( "pegasus NBG export succeeded for %s",
                 partition_name.c_str());
    }

    // Step 3: Find and read the NBG file
    // pegasus creates <basename(output_path)>_nbg_unify/network_binary.nb
    // The exact location depends on pegasus version — search broadly.
    auto nbg_file = findNBGFile(tmp_dir);


    std::vector<uint8_t> nbg_bytes;
    if (std::filesystem::exists(nbg_file)) {
      std::ifstream f(nbg_file, std::ios::binary | std::ios::ate);
      auto size = f.tellg();
      f.seekg(0);
      nbg_bytes.resize(size);
      f.read(reinterpret_cast<char*>(nbg_bytes.data()), size);
      VS_LOG( "Read NBG: %zu bytes from %s",
                 nbg_bytes.size(), nbg_file.string().c_str());
    } else {
      VS_LOG_ERR( "NBG file not found at %s",
                 nbg_file.string().c_str());
    }

    if (!keep_temp_) {
      std::filesystem::remove_all(tmp_dir);
    }

    return nbg_bytes;
  }

 private:
  static int RunCommand(const std::string& cmd, std::string& output) {
    VS_LOG( "Running: %s", cmd.c_str());
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"),
                                                   pclose);
    if (!pipe) return -1;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get())) {
      output += buffer;
    }
    return pclose(pipe.release());
  }

  // Find the NBG file produced by pegasus.
  // Pegasus creates <something>_nbg_unify/network_binary.nb but the exact
  // location varies by version. Search the temp dir and its parent.
  static std::filesystem::path findNBGFile(
      const std::filesystem::path& tmp_dir) {
    const char* nb_name = "network_binary.nb";

    // Search pattern 1: tmp_dir/*_nbg_unify/network_binary.nb
    if (std::filesystem::exists(tmp_dir)) {
      for (const auto& entry : std::filesystem::directory_iterator(tmp_dir)) {
        if (entry.path().string().find("_nbg_unify") != std::string::npos) {
          auto candidate = entry.path() / nb_name;
          if (std::filesystem::exists(candidate)) return candidate;
        }
      }
    }

    // Search pattern 2: parent/tmp_dir_basename_nbg_unify/network_binary.nb
    // (pegasus sometimes creates it as a sibling of tmp_dir)
    auto parent = tmp_dir.parent_path();
    auto stem = tmp_dir.filename().string() + "_nbg_unify";
    auto sibling = parent / stem / nb_name;
    if (std::filesystem::exists(sibling)) return sibling;

    // Search pattern 3: parent/*_nbg_unify/network_binary.nb
    if (std::filesystem::exists(parent)) {
      for (const auto& entry : std::filesystem::directory_iterator(parent)) {
        if (entry.path().string().find("_nbg_unify") != std::string::npos) {
          auto candidate = entry.path() / nb_name;
          if (std::filesystem::exists(candidate)) return candidate;
        }
      }
    }

    return {};  // not found
  }


  std::string pegasus_path_;
  std::string viv_sdk_;
  std::string optimize_;
  std::string dtype_;
  bool keep_temp_ = false;
};

}  // namespace verisilicon
}  // namespace litert

#endif  // LITERT_VENDORS_VERISILICON_COMPILER_NBG_COMPILER_H_
