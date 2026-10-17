#include "derived_workspace.hpp"
#include <iostream>
#include <stdexcept>
using namespace rimes::buffer;
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  rimes::plugins::Registry registry;
  registry.Register({"builtin.fixture",
                     "Fixture",
                     "1",
                     rimes::plugins::Kind::Buffer,
                     {"ai.generate"}});
  registry.ApplyState("builtin.fixture", true, true);
  Check(registry.SelectBuffer("builtin.fixture"), "fixture selection");
  const auto lease = registry.Authorize("builtin.fixture", "ai.generate");
  Check(lease.has_value(), "fixture grant");
  const std::vector<Block> source{{"first", "你好。", Origin::Rime, 0},
                                  {"second", "世界。", Origin::Rime, 0}};
  DerivedWorkspace workspace;
  auto sync = [&](std::vector<Block> blocks = {},
                  FocusIdentity focus = {"field", 1}, bool visible = true,
                  bool protect = false, const char *config = "revision-1") {
    workspace.Synchronize(blocks.empty() ? source : blocks, "builtin.fixture",
                          config, focus, visible, protect);
  };
  sync();
  const auto job = *workspace.Begin(*lease);
  Check(job.text == "你好。世界。", "frozen source changed");
  Check(workspace.Update(job, registry.Accepts(job.lease), 0, "Hello"),
        "stream update");
  Check(workspace.CopyText().empty(), "partial output became ready");
  const auto first_id = workspace.Results().front().id;
  Check(workspace.Update(job, true, 0, "Hello world."), "replacement update");
  Check(workspace.Results().front().id == first_id, "stream lost identity");
  Check(workspace.Update(job, true, 1, "Hi world."), "alternative update");
  Check(workspace.Finish(job, true, true), "completion");
  Check(workspace.CopyText() == "Hello world.", "ready output missing");
  workspace.MoveResult(1);
  Check(workspace.CopyText() == "Hi world.", "alternative selection");
  const auto selected_id = workspace.SelectedResult()->id;
  Check(!workspace.ConsumePrefix(job, true, first_id, "Hi "), "unselected alternative delivered");
  Check(!workspace.ConsumePrefix(job, false, selected_id, "Hi "), "revoked delivery accepted");
  Check(!workspace.ConsumePrefix(job, true, selected_id, "Hi" "!"), "mismatched output consumed");
  Check(workspace.ConsumePrefix(job, true, selected_id, "Hi "), "ready first child not consumed");
  Check(workspace.CopyText() == "world." && workspace.Results().size() == 1 &&
        workspace.SelectedResult()->id != selected_id, "partial delivery retained alternatives or stale identity");
  Check(!workspace.ConsumePrefix(job, true, selected_id, "world."), "stale result duplicated delivery");
  Check(workspace.ConsumePrefix(job, true, workspace.SelectedResult()->id, "world.") &&
        workspace.CopyText().empty(), "final child not consumed");
  workspace.ReturnToEdit();
  Check(workspace.Results().empty() && workspace.CanGenerate(),
        "return to edit lost source");
  for (int scenario = 0; scenario < 7; ++scenario) {
    sync();
    const auto old = *workspace.Begin(*lease);
    if (scenario == 0)
      workspace.Cancel();
    if (scenario == 1)
      sync({}, {"field", 1}, false);
    if (scenario == 2)
      sync({}, {"field", 2});
    if (scenario == 3)
      sync({}, {"field", 1}, true, true);
    if (scenario == 4)
      sync({}, {"field", 1}, true, false, "revision-2");
    if (scenario == 5) {
      auto edited = source;
      edited[0].text = "修改。";
      sync(edited);
    }
    if (scenario == 6) {
      auto replaced = source;
      replaced[0].id = "replacement";
      sync(replaced);
    }
    sync();
    Check(!workspace.Update(old, true, 0, "late"),
          "late update restored authority");
    Check(!workspace.Finish(old, true, true), "late finish restored authority");
    Check(job.text == "你好。世界。", "mutable source snapshot");
  }
  const auto active = *workspace.Begin(*lease);
  Check(!workspace.Update(active, true, 5, "sixth"), "too many alternatives");
  Check(!workspace.Update(active, true, 0, "\xED\xA0\x80"),
        "UTF-8 surrogate accepted");
  Check(!workspace.Update(active, true, 0,
                          std::string(DerivedWorkspace::kResultLimit + 1, 'x')),
        "oversized result");
  registry.ApplyState("builtin.fixture", true, false);
  Check(!workspace.Update(active, registry.Accepts(active.lease), 0, "revoked"),
        "revoked result accepted");
  DerivedWorkspace restarted;
  restarted.Synchronize(source, "builtin.fixture", "revision-1", {"field", 1},
                        true, false);
  Check(!restarted.Finish(active, true, true),
        "cross-lifetime request accepted");
  std::cout << "Derived workspace source/focus/configuration/grant ownership, "
               "stream and alternatives passed\n";
}
