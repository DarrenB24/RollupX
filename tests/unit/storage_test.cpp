#include "rollupx/rocksdb_open.hpp"
#include "../test_support.hpp"
#include "rollupx/identity_store.hpp"
#include <gtest/gtest.h>

TEST(Storage, IdentitySurvivesReopen) {
  rollupx::test::Directory directory;
  auto c = rollupx::test::config(directory.path);
  { rollupx::IdentityStore first(c); }
  EXPECT_NO_THROW({ rollupx::IdentityStore reopened(c); });
  std::swap(c.peers.front(), c.peers.back());
  EXPECT_NO_THROW({ rollupx::IdentityStore reordered(c); });
}
TEST(Storage, RefusesChangedClusterMemberAndTopology) {
  rollupx::test::Directory directory;
  auto original = rollupx::test::config(directory.path);
  { rollupx::IdentityStore first(original); }
  auto changed = original; changed.cluster_id = "another";
  EXPECT_THROW({ rollupx::IdentityStore store(changed); }, std::runtime_error);
  changed = original; changed.id = "n2";
  EXPECT_THROW({ rollupx::IdentityStore store(changed); }, std::runtime_error);
  changed = original; changed.peers[1].endpoint = "localhost:12345";
  EXPECT_THROW({ rollupx::IdentityStore store(changed); }, std::runtime_error);
}
TEST(Storage, ExclusiveOwnership) {
  rollupx::test::Directory directory;
  const auto c = rollupx::test::config(directory.path);
  rollupx::IdentityStore first(c);
  EXPECT_THROW({ rollupx::IdentityStore second(c); }, std::runtime_error);
}
TEST(Storage, NonemptyDatabaseWithoutManifestIsNotReinitialized) {
  rollupx::test::Directory directory;
  rocksdb::Options options; options.create_if_missing = true;
  std::unique_ptr<rocksdb::DB> db;
  ASSERT_TRUE(rollupx::open_rocksdb(options, directory.path, db).ok());
  ASSERT_TRUE(db->Put(rocksdb::WriteOptions{}, "unexpected", "data").ok()); db.reset();
  EXPECT_THROW({ rollupx::IdentityStore store(rollupx::test::config(directory.path)); }, std::runtime_error);
}
