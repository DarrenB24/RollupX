#include "../raft_store_fixture.hpp"
#include <csignal>
#include <iostream>
#include <unistd.h>

int main(int argc, char* argv[]) {
  using namespace rollupx::raft;
  try {
    if (argc != 4) return 80;
    const std::string_view boundary(argv[2]), operation(argv[3]);
    if (boundary != "before" && boundary != "after") return 81;
    Store store(argv[1],rollupx::test::raft_manifest(),[&](StoreCrashPoint point) {
      if ((point == StoreCrashPoint::before_write && boundary == "before") ||
          (point == StoreCrashPoint::after_write && boundary == "after")) ::kill(::getpid(),SIGKILL);
    },StoreOpenMode::existing_only);
    auto next = store.load();
    if (operation == "term") { ++next.term; next.voted_for.reset(); }
    else if (operation == "vote") next.voted_for = 2;
    else if (operation == "append") next.log.push_back({1,"append"});
    else if (operation == "commit") next.commit = 1;
    else if (operation == "replace") { next.term = 2; next.log[0] = {2,"replacement"}; }
    else return 82;
    store.persist(next); return 83; // A missed kill hook must never look like a passing crash.
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 84; }
}
