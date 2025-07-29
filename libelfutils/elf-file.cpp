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

#include <elfutils/elf-file.h>
#include <elfutils/parse.h>

#include <elf.h>

#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace android {
namespace elfutils {

std::unique_ptr<ElfFile> ElfFile::create(const std::string& path) {
    std::ifstream elfStream(path);
    if (!elfStream) {
        return {};
    }

    // Read the ELF identity.
    // See: https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/elf.h#L334
    char elfIdent[EI_NIDENT];
    elfStream.read(elfIdent, EI_NIDENT);
    if (elfStream.gcount() != EI_NIDENT) {
        return {};
    }

    // Check the magic number.
    if (elfIdent[EI_MAG0] != ELFMAG0 || elfIdent[EI_MAG1] != ELFMAG1 ||
        elfIdent[EI_MAG2] != ELFMAG2 || elfIdent[EI_MAG3] != ELFMAG3) {
        return {};
    }

    // Check for 32/64 bit.
    char classNum = elfIdent[EI_CLASS];
    if (classNum == ELFCLASS32) {
        auto elfFile = std::make_unique<Elf32_File>(path);

        ElfParser<Elf32_File> parser(*elfFile);
        if (!parser.parse()) {
            return {};
        }

        return elfFile;
    } else if (classNum == ELFCLASS64) {
        auto elfFile = std::make_unique<Elf64_File>(path);

        ElfParser<Elf64_File> parser(*elfFile);
        if (!parser.parse()) {
            return {};
        }

        return elfFile;
    }

    return {};
}

// Explicitly instantiate the templates for 32-bit and 64-bit ELF files.
template class ElfFileImpl<Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr, Elf32_Dyn>;
template class ElfFileImpl<Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr, Elf64_Dyn>;

}  // namespace elfutils
}  // namespace android
