#include <android-base/file.h>
#include <elfutils/elf-file.h>
#include <elfutils/iter.h>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

using namespace android::elfutils;

TEST(ElfIteratorTest, EmptyDir) {
    TemporaryDir temp_dir;
    int count = 0;
    int parsed = ElfIterator::forEachElfFromDir(temp_dir.path, [&](const ElfFile&) { count++; });
    EXPECT_EQ(parsed, 0);
    EXPECT_EQ(count, 0);
}

TEST(ElfIteratorTest, NonElfFile) {
    TemporaryDir temp_dir;
    std::string file_path = std::string(temp_dir.path) + "/test.txt";
    std::ofstream ofs(file_path);
    ofs << "This is not an ELF file.";
    ofs.close();
    int count = 0;
    int parsed = ElfIterator::forEachElfFromDir(temp_dir.path, [&](const ElfFile&) { count++; });
    EXPECT_EQ(parsed, 0);
    EXPECT_EQ(count, 0);
}

TEST(ElfIteratorTest, RealElfFile) {
    std::string exe_dir = android::base::GetExecutableDirectory();
    int count = 0;
    int parsed = ElfIterator::forEachElfFromDir(exe_dir, [&](const ElfFile&) { count++; });
    EXPECT_GT(parsed, 0);
    EXPECT_GT(count, 0);
    EXPECT_EQ(parsed, count);
}
