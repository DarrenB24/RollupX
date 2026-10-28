#include "../snapshot_fixture.hpp"
#include <csignal>
#include <iostream>
#include <fstream>
#include <iterator>
#include <unistd.h>

int main(int argc, char* argv[]) {
  using namespace rollupx::raft;
  using namespace rollupx::test;
  try {
    if (argc != 4) return 80;
    const std::string_view boundary(argv[2]), operation(argv[3]);
    if (boundary != "before" && boundary != "after") return 81;
    const bool before = boundary == "before";
    StoreCrashPoint point;
    if (operation == "checkpoint") point = before ? StoreCrashPoint::before_checkpoint : StoreCrashPoint::after_checkpoint;
    else if (operation == "stage-first" || operation == "stage-last") point = before ? StoreCrashPoint::before_stage : StoreCrashPoint::after_stage;
    else if (operation == "publish") point = before ? StoreCrashPoint::before_publish : StoreCrashPoint::after_publish;
    else return 82;
    Store store(argv[1],raft_ledger_manifest(),ledger_genesis(),[&](StoreCrashPoint current) {
      if (current == point) ::kill(::getpid(),SIGKILL);
    },StoreOpenMode::existing_only);
    std::ifstream input(std::string(argv[1])+"/snapshot-input.bin",std::ios::binary);
    if (!input) return 85;
    const std::string image{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    const auto half = image.size()/2;
    if (operation == "checkpoint") (void)store.create_snapshot();
    else if (operation == "stage-first") (void)store.stage_snapshot(snapshot_chunk(image,0,half));
    else if (operation == "stage-last") (void)store.stage_snapshot(snapshot_chunk(image,half,image.size()-half));
    else (void)store.publish_staged_snapshot();
    return 83; // A requested hook must actually kill the process.
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 84; }
}
