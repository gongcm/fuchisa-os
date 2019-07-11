// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include <filesystem>
#include <fstream>
#include <iostream>

#include "src/developer/bugreport/client/bug_report_handler.h"
#include "src/lib/fxl/command_line.h"

namespace {

std::ifstream* OpenFile(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    FXL_LOG(ERROR) << "Could not open " << path;
    return nullptr;
  }

  static std::ifstream iss;
  iss.open(path, std::ifstream::in);
  if (!iss.good()) {
    FXL_LOG(ERROR) << "Could not read " << path;
    return nullptr;
  }

  return &iss;
}

constexpr char kUsage[] = R"(bugreport

    Process an incoming bugreport generated by a target device.
    Will output several report files in a given output directory.

Usage:

  bugreport [--input-file=<FILEPATH>] <OUTPUT_DIR>

Arguments:

    --input_file    Optional input file to be read. If not specified, will read
                    from stdin.
    OUTPUT_DIR      Where bugreport will store the files. Required.
)";

}  // namespace

int main(int argc, char* argv[]) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);

  // Check if an input file was given, use stdin otherwise.
  std::istream* input;
  if (command_line.HasOption("input-file")) {
    std::string input_value;
    command_line.GetOptionValue("input-file", &input_value);

    input = OpenFile(input_value);
    if (!input)
      exit(1);
  } else {
    input = &std::cin;
  }
  FXL_DCHECK(input);

  // Bugreport requires the caller to provide a valid directory to where output the report.
  auto& pos_args = command_line.positional_args();
  if (pos_args.size() != 1u) {
    FXL_LOG(ERROR) << "A single output path should be given.";
    std::cerr << kUsage;
    exit(1);
  }

  // Output path should be valid.
  std::filesystem::path output_path(pos_args[0]);
  std::error_code ec;
  if (!std::filesystem::is_directory(output_path, ec)) {
    FXL_LOG(ERROR) << "Invalid output directory given: " << output_path;
    exit(1);
  }

  auto targets = bugreport::HandleBugReport(output_path, input);
  if (!targets) {
    std::cerr << "Error processing input bug report. Exiting." << std::endl;
    exit(1);
  }

  // Report the success.
  std::cout << "Bug report processing successful." << std::endl;
  for (auto& target : *targets) {
    auto path = output_path / target.name;
    std::cout << "Exported " << path << std::endl;
  }
}
