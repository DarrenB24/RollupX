#include "../ledger_fixture.hpp"
#include <csignal>
#include <string_view>
#include <unistd.h>

int main(int argc, char* argv[]) {
  if (argc != 3) return 80;
  using namespace rollupx;
  const std::string_view name(argv[2]);
  LedgerCrashPoint wanted;
  if (name == "before_commit") wanted = LedgerCrashPoint::before_commit;
  else if (name == "after_commit") wanted = LedgerCrashPoint::after_commit;
  else if (name == "before_apply") wanted = LedgerCrashPoint::before_apply;
  else if (name == "after_apply") wanted = LedgerCrashPoint::after_apply;
  else return 81;
  try {
    DurableLedger ledger(argv[1], test::ledger_genesis(), [wanted](LedgerCrashPoint actual) {
      if (actual == wanted) { ::kill(::getpid(), SIGKILL); ::_exit(82); }
    });
    (void)ledger.submit(test::ledger_transaction());
    return 83; // Reaching here means the requested crash hook was not reached.
  } catch (...) { return 84; }
}
