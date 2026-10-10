#include "../raft_store_fixture.hpp"
#include <charconv>
#include <csignal>
#include <iostream>
#include <unistd.h>

int main(int argc, char* argv[]) {
  using namespace rollupx::raft;
  using namespace rollupx::test;
  try {
    if (argc != 4) return 80;
    const std::string_view name(argv[2]), count(argv[3]); unsigned occurrence = 0;
    const auto [end,error] = std::from_chars(count.data(),count.data()+count.size(),occurrence);
    if (error != std::errc{} || end != count.data()+count.size() || occurrence == 0) return 81;
    const auto point = name == "after_commit" ? StoreCrashPoint::after_write :
      name == "before_apply" ? StoreCrashPoint::before_apply : StoreCrashPoint::after_apply;
    if (name != "after_commit" && name != "before_apply" && name != "after_apply") return 82;
    unsigned seen = 0;
    Store store(argv[1],raft_ledger_manifest(),ledger_genesis(),[&](StoreCrashPoint current) {
      if (current == point && ++seen == occurrence) ::kill(::getpid(),SIGKILL);
    },StoreOpenMode::existing_only);
    store.persist(raft_ledger_history()); (void)store.apply_committed(); return 83;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 84; }
}
