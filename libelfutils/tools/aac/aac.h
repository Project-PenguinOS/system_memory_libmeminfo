/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <elfutils/elf-file.h>

using PathPair = std::pair<std::filesystem::path, std::string>;
using ::android::elfutils::Elf64_File;

class AppAlignmentChecker {
  public:
    explicit AppAlignmentChecker(const std::vector<PathPair>& initialPaths);
    ~AppAlignmentChecker();

    bool run();
    static void printHelp(const char* arg0);

  private:
    void discoverFiles();
    void runZipalignChecks();
    void runElfChecks();
    void extractAndQueue(const PathPair& archivePathPair);
    bool parse64BitElf(Elf64_File& elf64File, const std::filesystem::path& path);
    std::optional<std::string> getNdkVersion(const Elf64_File& elfFile);
    std::vector<std::string> getToolchainStrings(const Elf64_File& elfFile);

    std::vector<PathPair> mZipFiles;
    std::vector<PathPair> mElfFiles;
    std::vector<std::filesystem::path> mTempDirs;
    std::vector<PathPair> mScanQueue;
    bool mAllPassed = true;
};
