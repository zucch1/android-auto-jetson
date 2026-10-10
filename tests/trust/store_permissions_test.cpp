// SPDX-License-Identifier: GPL-3.0-or-later
// Task 27 store hardening: the store file is created and re-enforced at 0600,
// written via atomic replace, stored under the XDG state home with the
// documented fallback, and a group/world-accessible store is rejected.
#include <aa/trust/Store.hpp>

#include "fakes.hpp"

#include "../../src/trust/StoreFile.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace trust = aa::trust;
namespace test = aa::trust::test;
using aa::Error;
using aa::ErrorCode;

[[nodiscard]] mode_t mode_of(const std::filesystem::path& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return static_cast<mode_t>(~0);
    }
    return info.st_mode & 07777;
}

TEST(StorePermissions, StoreFileIs0600AfterWrite) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWired}).has_value());

    EXPECT_EQ(mode_of(dir.file()), 0600);
}

TEST(StorePermissions, StoreFileStays0600AcrossRewrites) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWired}).has_value());
    ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWireless}).has_value());

    EXPECT_EQ(mode_of(dir.file()), 0600);
    const auto temp_leftover = std::filesystem::path{dir.file().string() + ".tmp"};
    EXPECT_FALSE(std::filesystem::exists(temp_leftover));
}

TEST(StorePermissions, PermissiveUmaskStillYields0600) {
    const mode_t previous = ::umask(0);
    {
        test::TempStoreDir dir;
        trust::ApprovedPhoneStore store{dir.file()};
        ASSERT_TRUE(store.load().has_value());
        ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWired}).has_value());
        EXPECT_EQ(mode_of(dir.file()), 0600);
    }
    (void)::umask(previous);
}

TEST(StorePermissions, WorldAccessibleStoreIsRejected) {
    test::TempStoreDir dir;
    std::filesystem::create_directories(dir.file().parent_path());
    {
        std::ofstream out{dir.file()};
        out << "{\"schema_version\":2,\"next_phone_id\":1,\"phones\":[]}";
    }
    ASSERT_EQ(::chmod(dir.file().c_str(), 0644), 0);

    trust::ApprovedPhoneStore store{dir.file()};
    const auto loaded = store.load();
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code(), ErrorCode::trust_store_io);
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::unknown);
}

TEST(StorePermissions, DefaultPathUsesXdgStateHome) {
    const char* previous = std::getenv("XDG_STATE_HOME");
    const std::string saved = previous == nullptr ? std::string{} : std::string{previous};
    ::setenv("XDG_STATE_HOME", "/var/state-example", 1);

    const auto resolved = trust::ApprovedPhoneStore::default_path();
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved.value(),
              std::filesystem::path{"/var/state-example/android-auto-receiver/approved-phones.json"});

    if (previous == nullptr) {
        ::unsetenv("XDG_STATE_HOME");
    } else {
        ::setenv("XDG_STATE_HOME", saved.c_str(), 1);
    }
}

TEST(StorePermissions, DefaultPathFallsBackToLocalState) {
    const char* previous = std::getenv("XDG_STATE_HOME");
    const std::string saved = previous == nullptr ? std::string{} : std::string{previous};
    ::unsetenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);

    const auto resolved = trust::ApprovedPhoneStore::default_path();
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved.value(),
              std::filesystem::path{home} / ".local" / "state" / "android-auto-receiver" /
                  "approved-phones.json");

    if (previous != nullptr) {
        ::setenv("XDG_STATE_HOME", saved.c_str(), 1);
    }
}

TEST(StorePermissions, DefaultPathIgnoresRelativeXdgValue) {
    const char* previous = std::getenv("XDG_STATE_HOME");
    const std::string saved = previous == nullptr ? std::string{} : std::string{previous};
    ::setenv("XDG_STATE_HOME", "relative/state", 1);
    const char* home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);

    const auto resolved = trust::ApprovedPhoneStore::default_path();
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved.value(),
              std::filesystem::path{home} / ".local" / "state" / "android-auto-receiver" /
                  "approved-phones.json");

    if (previous == nullptr) {
        ::unsetenv("XDG_STATE_HOME");
    } else {
        ::setenv("XDG_STATE_HOME", saved.c_str(), 1);
    }
}

TEST(StorePermissions, MissingStoreFileLoadsAsEmptyStore) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    const auto loaded = store.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(store.usable());
    EXPECT_TRUE(store.phones().empty());
}

TEST(StorePermissions, PrePlacedTmpSymlinkCannotRedirectWrites) {
    test::TempStoreDir dir;
    std::filesystem::create_directories(dir.file().parent_path());
    const auto sentinel = dir.dir() / "sentinel";
    {
        std::ofstream out{sentinel};
        out << "SENTINEL-CONTENT";
    }
    const auto decoy = std::filesystem::path{dir.file().string() + ".tmp"};
    ASSERT_EQ(::symlink(sentinel.c_str(), decoy.c_str()), 0);

    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWired}).has_value());

    std::ifstream in{sentinel};
    std::string content;
    std::getline(in, content);
    EXPECT_EQ(content, "SENTINEL-CONTENT");

    struct stat info {};
    ASSERT_EQ(::lstat(dir.file().c_str(), &info), 0);
    EXPECT_TRUE(S_ISREG(info.st_mode));

    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_EQ(fresh.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::approved);
}

TEST(StorePermissions, SymlinkedStoreFileIsRejectedOnLoad) {
    test::TempStoreDir dir;
    std::filesystem::create_directories(dir.file().parent_path());
    const auto target = dir.dir() / "target";
    {
        std::ofstream out{target};
        out << "{\"schema_version\":2,\"next_phone_id\":1,\"phones\":[]}";
    }
    ASSERT_EQ(::chmod(target.c_str(), 0600), 0);
    ASSERT_EQ(::symlink(target.c_str(), dir.file().c_str()), 0);

    trust::ApprovedPhoneStore store{dir.file()};
    const auto loaded = store.load();
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code(), ErrorCode::trust_store_io);
    EXPECT_FALSE(store.usable());
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::unknown);
}

TEST(StorePermissions, PermissiveUmaskStillYields0700Ancestors) {
    const mode_t previous = ::umask(0);
    {
        test::TempStoreDir dir;
        const auto deep = dir.dir() / "a" / "b" / "approved-phones.json";
        trust::ApprovedPhoneStore store{deep};
        ASSERT_TRUE(store.load().has_value());
        ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWired}).has_value());

        EXPECT_EQ(mode_of(deep), 0600);
        EXPECT_EQ(mode_of(dir.dir() / "a"), 0700);
        EXPECT_EQ(mode_of(dir.dir() / "a" / "b"), 0700);
    }
    (void)::umask(previous);
}

TEST(StorePermissions, UnTightenableParentDirectoryFailsClosed) {
    test::TempStoreDir dir;
    const auto dangling = dir.dir() / "dangling";
    ASSERT_EQ(::symlink((dir.dir() / "missing-target").c_str(), dangling.c_str()), 0);

    trust::ApprovedPhoneStore store{dangling / "approved-phones.json"};
    ASSERT_TRUE(store.load().has_value());
    const auto approved = store.approve(trust::TransportIdentity{test::kWired});
    ASSERT_FALSE(approved.has_value());
    EXPECT_EQ(approved.error().code(), ErrorCode::trust_store_io);
    EXPECT_TRUE(store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(dangling / "approved-phones.json"));
}

TEST(StorePermissions, UnownedParentDirectoryFailsClosed) {
    // A writable foreign-owned parent (world-writable /tmp) cannot be
    // tightened to 0700: the chmod fails and the write must fail closed
    // instead of trusting an unowned directory to hold the store.
    std::filesystem::path foreign;
    if (::geteuid() == 0) {
        test::TempStoreDir dir;
        foreign = dir.dir() / "foreign";
        std::filesystem::create_directories(foreign);
        ASSERT_EQ(::chown(foreign.c_str(), static_cast<uid_t>(65534), static_cast<gid_t>(65534)),
                  0);
    } else {
        foreign = std::filesystem::temp_directory_path();
        struct stat info {};
        ASSERT_EQ(::stat(foreign.c_str(), &info), 0);
        if (info.st_uid == ::geteuid()) {
            GTEST_SKIP() << "no foreign-owned writable parent available in this environment";
        }
    }

    const auto path = foreign / ("aa-unowned-parent-" + std::to_string(::getpid()) + ".json");
    trust::ApprovedPhoneStore store{path};
    ASSERT_TRUE(store.load().has_value());
    const auto approved = store.approve(trust::TransportIdentity{test::kWired});
    ASSERT_FALSE(approved.has_value());
    EXPECT_EQ(approved.error().code(), ErrorCode::trust_store_io);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(StorePermissions, NonRegularStoreFileIsRejected) {
    test::TempStoreDir dir;
    std::filesystem::create_directories(dir.file().parent_path());
    ASSERT_EQ(::mkfifo(dir.file().c_str(), 0600), 0);

    trust::ApprovedPhoneStore store{dir.file()};
    const auto loaded = store.load();
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code(), ErrorCode::trust_store_io);
    EXPECT_FALSE(store.usable());
}

TEST(StorePermissions, SymlinkedParentWriteFailsClosedWithoutMutatingTarget) {
    test::TempStoreDir dir;
    const auto target = dir.dir() / "target";
    std::filesystem::create_directories(target);
    ASSERT_EQ(::chmod(target.c_str(), 0755), 0);
    const auto link = dir.dir() / "link";
    ASSERT_EQ(::symlink(target.c_str(), link.c_str()), 0);

    trust::ApprovedPhoneStore store{link / "approved-phones.json"};
    ASSERT_TRUE(store.load().has_value());
    const auto approved = store.approve(trust::TransportIdentity{test::kWired});
    ASSERT_FALSE(approved.has_value());
    EXPECT_EQ(approved.error().code(), ErrorCode::trust_store_io);

    EXPECT_EQ(mode_of(target), 0755);  // rejection must NOT mutate the target
    EXPECT_FALSE(std::filesystem::exists(target / "approved-phones.json"));
    EXPECT_FALSE(std::filesystem::exists(link / "approved-phones.json"));
}

TEST(StorePermissions, SymlinkedParentCannotLoadAnExistingStore) {
    test::TempStoreDir dir;
    const auto target = dir.dir() / "target";
    std::filesystem::create_directories(target);
    ASSERT_EQ(::chmod(target.c_str(), 0755), 0);
    const auto link = dir.dir() / "link";
    ASSERT_EQ(::symlink(target.c_str(), link.c_str()), 0);

    {
        std::ofstream out{target / "approved-phones.json"};
        out << "{\"schema_version\":2,\"next_phone_id\":2,\"phones\":["
               "{\"phone_id\":1,\"identity\":{\"kind\":\"opaque\",\"key\":\"approved-key\"}}]}";
    }
    ASSERT_EQ(::chmod((target / "approved-phones.json").c_str(), 0600), 0);

    trust::ApprovedPhoneStore direct{target / "approved-phones.json"};
    ASSERT_TRUE(direct.load().has_value());  // control: the store itself is valid
    EXPECT_EQ(direct.lookup(trust::PhoneIdentity{"approved-key"}), trust::Decision::approved);

    trust::ApprovedPhoneStore through_link{link / "approved-phones.json"};
    const auto loaded = through_link.load();
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code(), ErrorCode::trust_store_io);
    EXPECT_FALSE(through_link.usable());
    EXPECT_EQ(through_link.lookup(trust::PhoneIdentity{"approved-key"}),
              trust::Decision::unknown);

    EXPECT_EQ(mode_of(target), 0755);  // the failed load must NOT mutate the target
}

} // namespace
