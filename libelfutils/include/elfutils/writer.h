/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include <elfutils/elf-file.h>

#include <stdint.h>
#include <fstream>
#include <string>
#include <vector>

#include <elf.h>

namespace android {
namespace elfutils {

template <typename ElfFile_t>
class ElfWriter {
  public:
    ElfWriter(const ElfFile_t& elfFile, const std::string& outPath)
        : mElfFile(elfFile), mOutStream(outPath) {}

    ~ElfWriter() = default;

    using Elf_Ehdr = typename ElfFile_t::Elf_Ehdr;
    using Elf_Phdr = typename ElfFile_t::Elf_Phdr;
    using Elf_Shdr = typename ElfFile_t::Elf_Shdr;

    bool writeElfHeader() {
        if (!mOutStream) {
            return false;
        }

        mOutStream.seekp(0);
        if (!mOutStream) {
            return false;
        }

        mOutStream.write(reinterpret_cast<const char*>(&mElfFile.getEhdr()), sizeof(Elf_Ehdr));

        return !!mOutStream;
    }

    bool writeProgramHeaders() {
        if (!mOutStream) {
            return false;
        }

        mOutStream.seekp(mElfFile.getEhdr().e_phoff);
        if (!mOutStream) {
            return false;
        }

        for (const auto& phdr : mElfFile.getPhdrs()) {
            mOutStream.write(reinterpret_cast<const char*>(&phdr), sizeof(Elf_Phdr));
        }

        return !!mOutStream;
    }

    bool writeSectionHeaders() {
        if (!mOutStream) {
            return false;
        }

        mOutStream.seekp(mElfFile.getEhdr().e_shoff);
        if (!mOutStream) {
            return false;
        }

        for (const auto& phdr : mElfFile.getShdrs()) {
            mOutStream.write(reinterpret_cast<const char*>(&phdr), sizeof(Elf_Shdr));
        }

        return !!mOutStream;
    }

    bool writeSections() {
        if (!mOutStream) {
            return false;
        }

        const auto& shdrs = mElfFile.getShdrs();
        const auto& sections = mElfFile.getSections();

        for (const auto& section : sections) {
            if (section.data.empty()) {
                continue;
            }

            mOutStream.seekp(shdrs[section.index].sh_offset);
            mOutStream.write(section.data.data(), section.size);
        }

        return !!mOutStream;
    }

    bool writeElf() {
        return writeElfHeader() && writeProgramHeaders() && writeSectionHeaders() &&
               writeSections();
    }

  private:
    const ElfFile_t& mElfFile;
    std::ofstream mOutStream;
};

}  // namespace elfutils
}  // namespace android
