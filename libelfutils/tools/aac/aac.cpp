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

#include "aac.h"

#include <iostream>
#include <system_error>

#include <ZipAlign.h>
#include <android-base/file.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <elfpolicy/elf_policy.h>
#include <elfutils/parse.h>
#include <fcntl.h>
#include <unistd.h>
#include <ziparchive/zip_archive.h>

#include <fstream>

using ::android::elfpolicy::kMaxSupportedPageSize;
using ::android::elfutils::Elf32_File;
using ::android::elfutils::Elf_Sc;
using ::android::elfutils::ElfFile;
using ::android::elfutils::ElfParser;

AppAlignmentChecker::AppAlignmentChecker(const std::vector<PathPair>& initialPaths)
    : mScanQueue(initialPaths) {}

AppAlignmentChecker::~AppAlignmentChecker() {
    for (const auto& tempDir : mTempDirs) {
        std::filesystem::remove_all(tempDir);
    }
}

bool AppAlignmentChecker::run() {
    discoverFiles();
    runZipalignChecks();
    runElfChecks();

    return mAllPassed;
}

void AppAlignmentChecker::discoverFiles() {
    std::cout << "--- Discovering and extracting files ---" << std::endl;
    while (!mScanQueue.empty()) {
        PathPair currentItem = mScanQueue.back();
        mScanQueue.pop_back();
        const auto& currentPath = currentItem.first;
        const auto& displayBase = currentItem.second;

        std::error_code ec;
        if (std::filesystem::is_directory(currentPath, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(currentPath, ec)) {
                std::filesystem::path newDisplayPath =
                        std::filesystem::path(displayBase) / entry.path().filename();
                mScanQueue.emplace_back(entry.path(), newDisplayPath.string());
            }
        } else if (std::filesystem::is_regular_file(currentPath, ec)) {
            if (auto elfFile = ElfFile::create(currentPath.string()); elfFile) {
                mElfFiles.push_back(currentItem);
            } else {
                std::string extension = currentPath.extension().string();
                if (extension == ".apk") {
                    mZipFiles.push_back(currentItem);
                    extractAndQueue(currentItem);
                }
            }
        }
    }
}

void AppAlignmentChecker::runZipalignChecks() {
    std::cout << "\n--- Running Zipalign Checks ---" << std::endl;
    for (const auto& pair : mZipFiles) {
        const int alignment = 4;
        const bool pageAlignSharedLibs = true;
        const int targetPageSize = kMaxSupportedPageSize;
        const bool verbose = false;

        if (android::verify(pair.first.string().c_str(), alignment, verbose, pageAlignSharedLibs,
                            targetPageSize) != 0) {
            std::cout << "[ FAIL ] " << pair.second << ": Zip alignment verification failed."
                      << std::endl;
            mAllPassed = false;
        } else {
            std::cout << "[ PASS ] " << pair.second << std::endl;
        }
    }
}

void AppAlignmentChecker::runElfChecks() {
    std::cout << "\n--- Running ELF Compatibility Checks ---" << std::endl;
    for (const auto& pair : mElfFiles) {
        const auto& path = pair.first;
        const auto& displayPath = pair.second;
        std::string errorMsg;

        auto elfFile = ElfFile::createFromIdent(path.string());
        if (!elfFile) {
            std::cout << "[ FAIL ] " << displayPath << ": Not a valid ELF file." << std::endl;
            mAllPassed = false;
            continue;
        }

        // The 16KB and RELRO checks are only applicable to 64-bit ELF files.
        // Silently ignore 32-bit files as they are not subject to these requirements.
        if (elfFile->is32Bit()) {
            continue;
        }

        auto* elf64File = static_cast<Elf64_File*>(elfFile.get());
        bool parsedOk = parse64BitElf(*elf64File, path);
        if (!parsedOk) {
            std::cout << "[ FAIL ] " << displayPath << ": Could not parse ELF file." << std::endl;
            mAllPassed = false;
            continue;
        }
        bool alignmentOk = android::elfpolicy::VerifyLoadSegmentsAlignment(
                *elfFile, kMaxSupportedPageSize, errorMsg);
        if (!alignmentOk) {
            std::cout << "[ FAIL ] " << displayPath << " (LOAD Segments)" << std::endl;
            std::cout << "         " << errorMsg << std::endl;
            mAllPassed = false;
        } else {
            std::cout << "[ PASS ] " << displayPath << " (LOAD Segments)" << std::endl;
        }

        errorMsg.clear();

        bool relroOk =
                android::elfpolicy::VerifyRelroSegments(*elfFile, kMaxSupportedPageSize, errorMsg);
        if (!relroOk) {
            std::cout << "[ FAIL ] " << displayPath << " (RELRO Segment)" << std::endl;
            std::cout << "         " << errorMsg << std::endl;

            if (auto ndkVersion = getNdkVersion(*elf64File); ndkVersion) {
                std::cout << "           NDK Version: " << *ndkVersion << std::endl;
            }
            auto toolchains = getToolchainStrings(*elf64File);
            if (!toolchains.empty()) {
                std::cout << "           Toolchain: " << toolchains[0] << std::endl;
                for (size_t i = 1; i < toolchains.size(); ++i) {
                    std::cout << "           | " << toolchains[i] << std::endl;
                }
            }
            mAllPassed = false;
        } else {
            if (!errorMsg.empty()) {
                std::cout << "[ WARN ] " << displayPath << " (RELRO Segment)" << std::endl;
                std::cout << "         " << errorMsg << std::endl;
            } else {
                std::cout << "[ PASS ] " << displayPath << " (RELRO Segment)" << std::endl;
            }
        }
    }
}

#ifdef _WIN32
static char* mkdtemp(char* tmpl) {
    if (mktemp(tmpl) == NULL) {
        return NULL;
    }
    if (mkdir(tmpl) == -1) {
        return NULL;
    }
    return tmpl;
}
#endif

void AppAlignmentChecker::extractAndQueue(const PathPair& archivePathPair) {
    const auto& realPath = archivePathPair.first;
    const auto& displayPath = archivePathPair.second;

    std::string tempDirTemplateStr =
            (std::filesystem::temp_directory_path() / "compat_check_XXXXXX").string();
    std::vector<char> tempDirTemplate(tempDirTemplateStr.begin(), tempDirTemplateStr.end());
    tempDirTemplate.push_back('\0');

    char* tempDirCstr = mkdtemp(tempDirTemplate.data());
    if (tempDirCstr == nullptr) {
        std::cout << "Failed to create temporary directory for " << displayPath << std::endl;
        mAllPassed = false;
        return;
    }
    std::filesystem::path tempPath(tempDirCstr);
    mTempDirs.push_back(tempPath);

    ZipArchiveHandle handle;
    int32_t openResult = OpenArchive(realPath.string().c_str(), &handle);
    if (openResult != 0) {
        std::cout << "Failed to open archive " << displayPath << ": " << ErrorCodeString(openResult)
                  << std::endl;
        mAllPassed = false;
        return;
    }

    void* cookie;
    if (StartIteration(handle, &cookie) != 0) {
        std::cout << "Failed to start iteration on archive " << displayPath << std::endl;
        mAllPassed = false;
        CloseArchive(handle);
        return;
    }

    ZipEntry64 entry;
    std::string name;
    while (Next(cookie, &entry, &name) == 0) {
        std::filesystem::path outPath = tempPath / name;
        if (name.back() == '/') {
            std::filesystem::create_directories(outPath);
            continue;
        }

        std::filesystem::create_directories(outPath.parent_path());
        android::base::unique_fd fd(
                open(outPath.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, entry.unix_mode));

        if (fd == -1) {
            std::cout << "Failed to create file: " << outPath << std::endl;
            mAllPassed = false;
            continue;
        }

        ExtractEntryToFile(handle, &entry, fd);
    }

    EndIteration(cookie);
    CloseArchive(handle);

    mScanQueue.emplace_back(tempPath, displayPath);
}

bool AppAlignmentChecker::parse64BitElf(Elf64_File& elf64File, const std::filesystem::path& path) {
    ElfParser<Elf64_File> parser64(elf64File);

    if (!parser64.parseExecutableHeader() || !parser64.parseProgramHeaders()) return false;

    std::error_code ec;
    const auto fileSize = std::filesystem::file_size(path, ec);
    if (ec) {
        std::cout << "Error getting file size for " << path << ": " << ec.message() << std::endl;
        return false;
    }
    auto& ehdr = elf64File.getEhdr();

    if (ehdr.e_shoff == 0) return true;

    uint64_t sh_end = (uint64_t)ehdr.e_shoff + (uint64_t)ehdr.e_shnum * ehdr.e_shentsize;
    if (sh_end > fileSize) {
        LOG(INFO) << "Invalid section header offset in " << elf64File.getPath()
                  << ". Skipping section parsing.";
        return true;
    }

    return parser64.parseSectionHeaders() && parser64.parseSections();
}

std::optional<std::string> AppAlignmentChecker::getNdkVersion(const Elf64_File& elfFile) {
    const auto& sections = elfFile.getSections();
    const Elf_Sc* noteSection = nullptr;
    for (const auto& section : sections) {
        if (section.name != ".note.android.ident") continue;
        noteSection = &section;
        break;
    }

    if (!noteSection) return std::nullopt;

    const char* p = noteSection->data.data();
    const char* end = p + noteSection->size;

    while (p < end) {
        // Not enough space for a header?
        if (p + sizeof(Elf64_Nhdr) > end) break;

        const Elf64_Nhdr* nhdr = reinterpret_cast<const Elf64_Nhdr*>(p);
        p += sizeof(Elf64_Nhdr);

        const char* name = p;
        p += (nhdr->n_namesz + 3) & ~3;  // 4-byte alignment

        const char* desc = p;
        p += (nhdr->n_descsz + 3) & ~3;  // 4-byte alignment

        // Note entry goes past the end of the section ?
        if (p > end) break;

        // NT_VERSION?
        if (nhdr->n_type != 1) continue;

        // Name is Android?
        if (nhdr->n_namesz != sizeof("Android")) continue;
        if (strcmp(name, "Android") != 0) continue;

        // Large enough?
        // The format is: API level (4 bytes), NDK version string (64 bytes), build ID (64 bytes)
        if (nhdr->n_descsz < 132) continue;

        // Parse the NDK version
        std::string ndkVersion(desc + 4, 64);

        // Trim null characters
        size_t firstNull = ndkVersion.find('\0');

        if (firstNull != std::string::npos) ndkVersion.resize(firstNull);

        return ndkVersion;
    }

    return std::nullopt;
}

// Toolchain strings are usually contained in the .comment section.
std::vector<std::string> AppAlignmentChecker::getToolchainStrings(const Elf64_File& elfFile) {
    std::vector<std::string> toolchains;
    const Elf_Sc* commentSection = elfFile.findSectionByName(".comment");

    if (!commentSection || commentSection->data.empty()) return toolchains;

    const char* start = commentSection->data.data();
    const char* end = start + commentSection->size;
    const char* current = start;

    while (current < end) {
        const char* strEnd = (const char*)memchr(current, '\0', end - current);
        if (strEnd == nullptr) break;

        bool isPrintable = true;
        for (const char* c = current; c < strEnd; ++c) {
            if (!isprint(*c)) {
                isPrintable = false;
                break;
            }
        }

        if (isPrintable) {
            std::string toolchainStr(current, strEnd - current);
            if (!toolchainStr.empty()) toolchains.push_back(toolchainStr);
        }

        current = strEnd + 1;
    }

    return toolchains;
}

void AppAlignmentChecker::printHelp(const char* arg0) {
    std::cout
            << "aac (App Alignment Checker)\n\n"
            << "Usage: " << arg0 << " <file_or_directory>...\n\n"
            << "A host tool to verify Android application packages (.apk) and ELF binaries (.so)\n"
            << "for modern compatibility requirements.\n\n"
            << "The tool performs the following checks:\n"
            << "  - Zipalign verification for APKs.\n"
            << "  - 16KB page alignment for ELF LOAD segments.\n"
            << "  - RELRO segment validity for ELF files.\n\n"
            << "It recursively scans directories and extracts APKs for deep analysis.\n\n"
            << "Options:\n"
            << "  -h, --help    Display this help message and exit.\n"
            << std::endl;
}
