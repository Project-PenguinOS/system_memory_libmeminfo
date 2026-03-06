#include <android-base/file.h>
#include <elfutils/elf-file.h>
#include <elfutils/iter.h>
#include <gtest/gtest.h>
#include <string>

using namespace android::elfutils;

TEST(ElfIteratorTest, EmptyDir) {
    TemporaryDir temp_dir;
    int count = 0;
    int parsed = ElfIterator::forEachElfFromDir(temp_dir.path, [&](const ElfFile&) { count++; });
    EXPECT_EQ(parsed, 0);
    EXPECT_EQ(count, 0);
}

TEST(ElfIteratorTest, RealFiles) {
    std::string exe_dir = android::base::GetExecutableDirectory();
    std::string testdata_dir = exe_dir + "/testdata";
    int count = 0;
    int parsed = ElfIterator::forEachElfFromDir(testdata_dir, [&](const ElfFile&) { count++; });
    EXPECT_EQ(parsed, 1);
    EXPECT_EQ(count, 1);
}
