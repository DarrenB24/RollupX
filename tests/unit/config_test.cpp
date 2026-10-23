#include "../test_support.hpp"
#include <gtest/gtest.h>

TEST(Config, IdentityIndependentOfMemberOrder) {
  auto a = rollupx::test::config("unused"); auto b = a;
  std::swap(b.peers.front(), b.peers.back());
  EXPECT_EQ(a.identity(), b.identity());
  b.cluster_id = "other"; EXPECT_NE(a.identity(), b.identity());
}
TEST(Config, RejectsInvalidMembershipAndUnsafeLabels) {
  auto c = rollupx::test::config("unused");
  c.id = "n1\"\n"; EXPECT_THROW(c.validate(), std::invalid_argument);
  c.id = "n1"; c.peers[1].id = "n1"; EXPECT_THROW(c.validate(), std::invalid_argument);
  c = rollupx::test::config("unused"); c.peers.pop_back(); EXPECT_THROW(c.validate(), std::invalid_argument);
  c = rollupx::test::config("unused"); c.id = "n4"; EXPECT_THROW(c.validate(), std::invalid_argument);
}
TEST(Config, RejectsMissingUnknownAndDuplicateOptions) {
  char name[] = "node", key[] = "--unknown", value[] = "x";
  char* args[] = {name, key, value, key, value};
  EXPECT_THROW(rollupx::parse_config(2, args), std::invalid_argument);
  EXPECT_THROW(rollupx::parse_config(3, args), std::invalid_argument);
  EXPECT_THROW(rollupx::parse_config(5, args), std::invalid_argument);
}
TEST(Config, RejectsInvalidEndpointAndTiming) {
  for (const auto* endpoint : {"host:0", "host:65536", "host:90x", "host:-1", "[::1]:90"})
    EXPECT_THROW(rollupx::parse_endpoint(endpoint), std::invalid_argument);
  auto c = rollupx::test::config("unused"); c.rpc_timeout = std::chrono::milliseconds(0);
  EXPECT_THROW(c.validate(), std::invalid_argument);
}
TEST(Config, LedgerRequiresCompleteDistinctPublicRouting) {
  auto c = rollupx::test::config("unused"); c.genesis_file = "genesis.txt";
  EXPECT_THROW(c.validate(),std::invalid_argument);
  c.public_peers = {{"n3","127.0.0.1:50071"},{"n1","127.0.0.1:50051"},{"n2","127.0.0.1:50061"}};
  EXPECT_NO_THROW(c.validate());
  auto changed = c; changed.public_peers[0].id = "unknown";
  EXPECT_THROW(changed.validate(),std::invalid_argument);
  changed = c; changed.public_peers[0].endpoint = c.peers[0].endpoint;
  EXPECT_THROW(changed.validate(),std::invalid_argument);
  changed = c; changed.public_peers[0].id = "n1";
  EXPECT_THROW(changed.validate(),std::invalid_argument);
  changed = c; changed.genesis_file.clear();
  EXPECT_THROW(changed.validate(),std::invalid_argument);
}
