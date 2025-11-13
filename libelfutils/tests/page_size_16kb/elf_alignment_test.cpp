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

#include <elf.h>
#include <gtest/gtest.h>
#include <mntent.h>

#include <iomanip>
#include <regex>
#include <set>

#include <elfutils/iter.h>
#include <libdm/dm.h>

#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android/api-level.h>

using ::android::elfutils::Elf64_File;

constexpr char kLowRamProp[] = "ro.config.low_ram";
constexpr char kVendorApiLevelProp[] = "ro.vendor.api_level";
// 16KB by default (unsupported devices must explicitly opt-out)
constexpr uint64_t kRequiredMaxSupportedPageSize = 0x4000;

static inline std::string escapeForRegex(const std::string& str) {
    // Regex metacharacters to be escaped
    static const std::regex specialChars(R"([.^$|(){}\[\]+*?\\])");

    // Replace each special character with its escaped version
    return std::regex_replace(str, specialChars, R"(\$&)");
}

static inline bool startsWithPattern(const std::string& str, const std::string& pattern) {
    std::regex _pattern("^" + pattern + ".*");
    return std::regex_match(str, _pattern);
}

static std::set<std::string> getMounts() {
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> fp(setmntent("/proc/mounts", "re"), endmntent);
    std::set<std::string> exclude({"/", "/config", "/data", "/data_mirror", "/dev", "/linkerconfig",
                                   "/mnt", "/proc", "/storage", "/sys"});
    std::set<std::string> mounts;

    if (fp == nullptr) {
        return mounts;
    }

    mntent* mentry;
    while ((mentry = getmntent(fp.get())) != nullptr) {
        std::string mountDir(mentry->mnt_dir);

        std::string dir = "/" + android::base::Split(mountDir, "/")[1];

        if (exclude.find(dir) != exclude.end()) {
            continue;
        }

        mounts.insert(dir);
    }

    return mounts;
}

using ::android::elfutils::ElfFile;

class ElfAlignmentTest : public ::testing::TestWithParam<std::string> {
  protected:
    static void validateRelroSegment(ElfFile& elfFile) {
        // We can safely use static_cast because the caller (loadAlignmentCb)
        // has already verified that this is a 64-bit ELF file.
        const auto* elf64File = static_cast<const Elf64_File*>(&elfFile);

        const auto& phdrs = elf64File->getPhdrs();
        const auto& shdrs = elf64File->getShdrs();
        const std::string& path = elfFile.getPath();

        const Elf64_Phdr* relroPhdr = nullptr;
        for (const auto& phdr : phdrs) {
            if (phdr.p_type == PT_GNU_RELRO) {
                relroPhdr = &phdr;
                break;
            }
        }

        // No RELRO segment, nothing to validate.
        if (!relroPhdr) {
            return;
        }

        ASSERT_GT(relroPhdr->p_memsz, 0) << "RELRO segment's mem size cannot be zero in " << path;

        const Elf64_Phdr* loadPhdr = nullptr;
        for (const auto& phdr : phdrs) {
            if (phdr.p_type == PT_LOAD && phdr.p_vaddr == relroPhdr->p_vaddr) {
                loadPhdr = &phdr;
                break;
            }
        }

        ASSERT_TRUE(loadPhdr) << "RELRO segment does not have a corresponding LOAD segment in "
                              << path;
        ASSERT_GT(loadPhdr->p_memsz, 0)
                << "RELRO segment's corresponding LOAD segment mem size cannot be zero in " << path;

        auto getContainedSections = [&](const Elf64_Phdr* segment) {
            std::vector<Elf64_Shdr> contained;
            for (const auto& shdr : shdrs) {
                if (shdr.sh_addr >= segment->p_vaddr &&
                    (shdr.sh_addr + shdr.sh_size) <= (segment->p_vaddr + segment->p_memsz)) {
                    contained.push_back(shdr);
                }
            }
            return contained;
        };

        auto relroSections = getContainedSections(relroPhdr);
        auto loadSections = getContainedSections(loadPhdr);

        auto shdrSorter = [](const Elf64_Shdr& a, const Elf64_Shdr& b) {
            return a.sh_addr < b.sh_addr;
        };
        std::sort(relroSections.begin(), relroSections.end(), shdrSorter);
        std::sort(loadSections.begin(), loadSections.end(), shdrSorter);

        bool sameSections = relroSections.size() == loadSections.size();
        if (sameSections) {
            for (size_t i = 0; i < relroSections.size(); ++i) {
                const auto& relroShdr = relroSections[i];
                const auto& loadShdr = loadSections[i];
                if (memcmp(&relroShdr, &loadShdr, sizeof(Elf64_Shdr)) != 0) {
                    sameSections = false;
                    break;
                }
            }
        }

        // If sections are the same, RELRO is the entire LOAD segment and alignment is guaranteed.
        if (sameSections) {
            return;
        }

        auto relroStart = relroPhdr->p_vaddr;
        auto loadStart = loadPhdr->p_vaddr;
        if (relroStart % kRequiredMaxSupportedPageSize) {
            EXPECT_EQ(relroStart, loadStart)
                    << "Unaligned RELRO start 0x" << std::hex << relroStart
                    << " must match LOAD start 0x" << loadStart << " in " << path;
        }

        auto relroEnd = relroStart + relroPhdr->p_memsz;
        auto loadEnd = loadStart + loadPhdr->p_memsz;
        if (relroEnd % kRequiredMaxSupportedPageSize) {
            EXPECT_EQ(relroEnd, loadEnd) << "Unaligned RELRO end 0x" << std::hex << relroEnd
                                         << " must match LOAD end 0x" << loadEnd << " in " << path;
        }
    }

    static void loadAlignmentCb(ElfFile& elfFile) {
        using namespace android::elfutils;

        static std::array ignored_directories{
                // Ignore VNDK APEXes. They are prebuilts from old branches, and would
                // only be used on devices with old vendor images.
                escapeForRegex("/apex/com.android.vndk.v"),
                // Ignore Trusty VM images under */etc/vm/trusty_vm as they don't run
                // in userspace, so 16K is not required. See b/365240530 and b/406626518
                // for more context.
                ".*" + escapeForRegex("/etc/vm/trusty_vm"),
                // Ignore non-Android firmware images.
                escapeForRegex("/odm/firmware/"), escapeForRegex("/vendor/firmware/"),
                escapeForRegex("/vendor/firmware_mnt/image"),
                // Ignore TEE binaries ("glob: /apex/com.*.android.authfw.ta*")
                escapeForRegex("/apex/com.") + ".*" + escapeForRegex(".android.authfw.ta"),
                // Ignore wlan debug vendor prebuilts
                escapeForRegex("/vendor/bin/dhd"),
                escapeForRegex("/vendor/bin/wl")};

        // Don't check 32-bit ELFs
        if (elfFile.is32Bit()) return;

        std::string path = elfFile.getPath();
        for (const auto& pattern : ignored_directories) {
            if (startsWithPattern(path, pattern)) {
                return;
            }
        }

        // Ignore ART Odex files for now. They are not 16K aligned.
        // b/376814207
        if (path.ends_with(".odex")) {
            return;
        }

        if (auto minAlign = elfFile.getMinLoadSegmentAlignment()) {
            EXPECT_GE(*minAlign, kRequiredMaxSupportedPageSize)
                    << " " << path << " has alignment 0x" << std::hex << *minAlign
                    << " which is not at least 0x" << kRequiredMaxSupportedPageSize;
        }

        validateRelroSegment(elfFile);
    };

    static bool isLowRamDevice() { return android::base::GetBoolProperty(kLowRamProp, false); }

    static int vendorApiLevel() {
        // "ro.vendor.api_level" is added in Android T. Undefined indicates S or below
        return android::base::GetIntProperty(kVendorApiLevelProp, __ANDROID_API_S__);
    }

    void SetUp() override {
        if (vendorApiLevel() < 202404) {
            GTEST_SKIP() << "16kB support is only required on V and later releases.";
        } else if (isLowRamDevice()) {
            GTEST_SKIP() << "Low Ram devices only support 4kB page size";
        }
    }
};

using ::android::elfutils::ElfIterator;

// @VsrTest = 3.14.1
TEST_P(ElfAlignmentTest, VerifyLoadSegmentAlignment) {
    ElfIterator::forEachElfFromDir(GetParam(), &loadAlignmentCb);
}

INSTANTIATE_TEST_SUITE_P(ElfTestPartitionsAligned, ElfAlignmentTest,
                         ::testing::ValuesIn(getMounts()));

int main(int argc, char* argv[]) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
