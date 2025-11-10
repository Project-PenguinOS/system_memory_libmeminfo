/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <android-base/logging.h>
#include <elfutils/elf-file.h>

#include <fstream>

namespace android {
namespace elfutils {

/*
 * Class to parse ELF binaries.
 *
 * The class will parse the 4 parts if present
 *
 * - Executable header (Elf64_Ehdr or Elf32_Ehdr).
 * - Program headers (Elf64_Phdr or Elf32_Phdr - present in executables or shared libraries).
 * - Section headers (Elf64_Shdr or Elf32_Shdr)
 * - Sections (.interp, .init, .plt, .text, .rodata, .data, .bss, .shstrtab, etc).
 *
 * The parser is used internally by ElfFile::create() to populate its fields.
 */
template <typename ElfFile_t>
class ElfParser {
  public:
    explicit ElfParser(ElfFile_t& elfFile) : mElfFile(elfFile), mElfStream(mElfFile.getPath()) {}

    ~ElfParser() = default;

    [[nodiscard]] bool parse() {
        return parseExecutableHeader() && parseProgramHeaders() && parseSectionHeaders() &&
               parseSections();
    }

  private:
    ElfFile_t& mElfFile;
    std::ifstream mElfStream;

    using Elf_Ehdr = typename ElfFile_t::Elf_Ehdr;
    using Elf_Phdr = typename ElfFile_t::Elf_Phdr;
    using Elf_Shdr = typename ElfFile_t::Elf_Shdr;
    using Elf_Dyn = typename ElfFile_t::Elf_Dyn;

    bool parseExecutableHeader() {
        if (!mElfStream) {
            return false;
        }

        mElfStream.seekg(0);
        mElfStream.read((char*)&mElfFile.mEhdr, sizeof(mElfFile.mEhdr));

        return !!mElfStream;
    }

    bool parseProgramHeaders() {
        uint64_t phOffset = mElfFile.mEhdr.e_phoff;
        uint16_t phNum = mElfFile.mEhdr.e_phnum;

        if (!mElfStream) {
            return false;
        }

        mElfStream.seekg(phOffset);
        for (int i = 0; i < phNum; i++) {
            Elf_Phdr phdr;

            mElfStream.read((char*)&phdr, sizeof(phdr));
            if (!mElfStream) {
                return false;
            }

            mElfFile.mPhdrs.push_back(phdr);
        }

        return !!mElfStream;
    }

    bool parseSectionHeaders() {
        uint64_t shOffset = mElfFile.mEhdr.e_shoff;
        uint16_t shNum = mElfFile.mEhdr.e_shnum;

        if (!mElfStream) {
            return false;
        }

        mElfStream.seekg(shOffset);
        for (int i = 0; i < shNum; i++) {
            Elf_Shdr shdr;

            mElfStream.read((char*)&shdr, sizeof(shdr));
            if (!mElfStream) {
                return false;
            }

            mElfFile.mShdrs.push_back(shdr);
        }

        return !!mElfStream;
    }

    bool parseSections() {
        Elf_Sc strTblPtr;

        if (!mElfStream) {
            return false;
        }

        for (size_t i = 0; i < mElfFile.mShdrs.size(); i++) {
            uint64_t sOffset = mElfFile.mShdrs[i].sh_offset;
            uint64_t sSize = mElfFile.mShdrs[i].sh_size;

            Elf_Sc section;
            if (mElfFile.mShdrs[i].sh_type != SHT_NOBITS) {
                section.data.resize(sSize);
                mElfStream.seekg(sOffset);

                mElfStream.read(section.data.data(), sSize);
                if (!mElfStream) {
                    return false;
                }
            }

            section.size = sSize;
            section.index = i;

            if (mElfFile.mEhdr.e_shstrndx == i) {
                strTblPtr = section;
            }

            mElfFile.mSections.push_back(section);
        }

        // Set the data section names.
        // This has to be done after reading the data section with index e_shstrndx.
        for (size_t i = 0; i < mElfFile.mSections.size(); i++) {
            uint32_t nameIdx = mElfFile.mShdrs[i].sh_name;
            char* st = strTblPtr.data.data();

            if (nameIdx < strTblPtr.size) {
                CHECK_NE(memchr(&st[nameIdx], 0, strTblPtr.size - nameIdx), nullptr);
                mElfFile.mSections[i].name = &st[nameIdx];
            }
        }

        return !!mElfStream;
    }
};

}  // namespace elfutils
}  // namespace android
