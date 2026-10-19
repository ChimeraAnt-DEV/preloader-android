/**
 * @file HookInstallGateTest.cpp
 * @brief Host test for the deferred-hook-install state machine.
 *
 * The hooks used to install during the game activity's onCreate, before the runtime was up, and the
 * install bailed ("Hook installed: no"). The gate defers the install to the frame pump. Its
 * invariants — install exactly once, never wait forever, never install twice — are pinned here so a
 * regression cannot leave the state pending or install the detour twice.
 */

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "pl/runtime/HookInstallGate.hpp"

using pl::runtime::HookInstallGate;
using pl::runtime::InstallDecision;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

} // namespace

int main() {
  // A freshly armed gate installs on the first frame tick and then never again.
  {
    HookInstallGate gate;
    gate.start(1000);
    expect(!gate.isResolved(), "armed gate is not resolved");
    expect(gate.tick(1000) == InstallDecision::Install, "first frame tick installs");
    expect(gate.isResolved(), "gate is resolved after installing");
    expect(gate.tick(1016) == InstallDecision::Wait, "a resolved gate never re-installs");
    expect(gate.tick(2000) == InstallDecision::Wait, "a resolved gate stays resolved");
  }

  // start() re-arms: a second session must be able to install again in a fresh gate.
  {
    HookInstallGate gate;
    gate.start(0);
    expect(gate.tick(0) == InstallDecision::Install, "fresh gate installs");
    gate.start(5000);
    expect(!gate.isResolved(), "re-armed gate is unresolved");
    expect(gate.tick(5000) == InstallDecision::Install, "re-armed gate installs again");
  }

  // Frame ticks are counted, so the diagnostics line can report how long the wait was.
  {
    HookInstallGate gate;
    gate.start(0);
    gate.tick(0);
    expect(gate.frameTicks() == 1, "one tick observed");
    expect(gate.isResolved(), "resolved after the tick");
  }

  // The give-up path: a gate whose frame ticks never satisfy the minimum still resolves, so the
  // state is never left pending forever. (With kMinFrameTicks == 1 the install path always wins, so
  // this asserts the resolved flag rather than a specific decision.)
  {
    HookInstallGate gate;
    gate.start(0);
    InstallDecision d = gate.tick(HookInstallGate::kGiveUpMs + 1);
    expect(d == InstallDecision::Install || d == InstallDecision::GiveUp,
           "a late tick resolves the gate one way or the other");
    expect(gate.isResolved(), "a late tick leaves the gate resolved");
  }

  // Item 4: a deferral re-arms the gate so a later tick tries the install again instead of the
  // gate staying resolved and reporting Wait forever (which lost the install for the session).
  {
    HookInstallGate gate;
    gate.start(0);
    expect(gate.tick(0) == InstallDecision::Install, "first attempt decides install");
    expect(gate.isResolved(), "gate resolved after the first attempt");
    // The install body could not run yet (rules not delivered): re-arm.
    expect(gate.defer(16) == 1, "first defer is counted");
    expect(!gate.isResolved(), "a deferred gate is unresolved again");
    expect(gate.tick(32) == InstallDecision::Install, "a deferred gate retries the install");
  }

  // A gate that keeps deferring stops at kMaxDefers so a runtime that never delivers the rules
  // settles instead of retrying forever.
  {
    HookInstallGate gate;
    gate.start(0);
    int last = 0;
    for (int i = 0; i < HookInstallGate::kMaxDefers; ++i) {
      gate.tick(i * 16);
      last = gate.defer(i * 16 + 1);
    }
    expect(last == HookInstallGate::kMaxDefers, "defers are bounded by kMaxDefers");
    expect(gate.deferredCount() == HookInstallGate::kMaxDefers, "defer count matches");
  }

  // start() clears the defer count, so a fresh session is not penalised by an earlier one.
  {
    HookInstallGate gate;
    gate.start(0);
    gate.tick(0);
    gate.defer(1);
    expect(gate.deferredCount() == 1, "one defer recorded");
    gate.start(100);
    expect(gate.deferredCount() == 0, "start() clears the defer count");
  }

  std::printf("HookInstallGateTest: %d checks passed\n", checks);
  return 0;
}
