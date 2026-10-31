#include <cstdlib>
#include <iostream>

#include "model.hpp"
#include "semantic_blocks.hpp"
#include "buffer_metrics.hpp"
#include "buffer_auto_send.hpp"
#include "translation_languages.hpp"
using namespace rimes::windows::workbench;
void Check(bool ok, const char* message) {
  if (!ok) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
int main() {
  Target a{10, 1, 1}, b{10, 2, 2};
  Model editing;
  editing.Focus(a); editing.Open(); editing.Append("A"); editing.Append("C");
  Check(editing.MoveInsertion(-1) && editing.Append("B") && editing.SourceText() == "ABC",
        "insertion preserves hard blocks on both sides");
  Check(editing.DeleteForward() && editing.SourceText() == "AB", "delete removes following block");
  Check(editing.SelectAll() && editing.Append("替换") && editing.SourceText() == "替换",
        "select-all replacement edits the Buffer only");
  Model reordered; reordered.Focus(a); reordered.Open(); reordered.Append("A"); reordered.Append("C");
  reordered.SetInsertion(1); reordered.Append("B");
  auto ordered_send = reordered.Send(true);
  Check(ordered_send && !reordered.SetInsertion(0), "issued source head cannot be moved or edited");
  for (const auto* expected : {"A", "B", "C"}) {
    Check(ordered_send && ordered_send->block.text == expected, "send-all follows visual order after insertion");
    ordered_send = reordered.Acknowledge(ordered_send->request, a, true);
  }
  Check(!ordered_send && reordered.source.empty(), "inserted later ids do not truncate send-all");
  Model split;
  split.Focus(a); split.Open(); split.translate = true; split.Append("原文。后文。");
  auto split_job = split.Generate(1, true);
  split.Stream(split_job, 1, "First. Second."); split.Finish(split_job, 1, true);
  Check(!split.CompleteResult(), "copy waits for untranslated tail");
  auto split_delivery = split.Send(false);
  Check(split_delivery.has_value(), "first translated child can send");
  split.Acknowledge(split_delivery->request, a, true);
  Check(split.SourceText() == "后文。" && split.ResultText() == "Second.",
        "first acknowledged child retires source exactly once");
  split.Append("x", InputKind::kDirect); split.Backspace();
  Check(split.ResultText() == "Second.", "tail editing preserves unsent children of retired source");
  auto suffix_job = split.Generate(1, true);
  Check(suffix_job.source == "后文。", "next job never replays retired original");
  split.Stream(suffix_job, 1, "Tail."); split.Finish(suffix_job, 1, true);
  Check(split.CompleteResult(), "complete copy contains remaining children and translated tail");
  split.SetInsertion(0); split.Append("new");
  Check(split.ResultText() == "Second.", "editing untranslated prefix only invalidates affected units");
  Model active;
  active.translate = true; active.Append("Old."); auto active_job = active.Generate(1, true);
  active.SetInsertion(0); active.Append("New");
  Check(!active.busy && !active.Stream(active_job, 1, "stale"), "prefix edit cancels stale local or remote work");
  Model blocks;
  blocks.Focus(a); blocks.Open();
  blocks.Append("你好"); blocks.Append("世界");
  Check(blocks.source.size() == 2 && blocks.source[0].text == "你好" &&
        blocks.source[1].text == "世界", "each Rime commit is a hard block boundary");
  Check(blocks.Backspace() && blocks.SourceText() == "你好",
        "Backspace removes a finalized commit block, not one character");
  blocks.Append("h", InputKind::kDirect);
  const auto tail_id = blocks.source.back().id;
  blocks.Append("i", InputKind::kDirect);
  Check(blocks.source.size() == 2 && blocks.source.back().text == "hi" &&
        blocks.source.back().id == tail_id,
        "direct keystrokes share a stable tail without merging into a Rime block");
  Check(blocks.Backspace() && blocks.source.back().text == "h",
        "only the live direct-input tail is character editable");
  blocks.Append("。新的提交");
  Check(blocks.source.size() == 3 && blocks.source.back().text == "。新的提交",
        "even punctuation inside a Rime commit does not split its hard boundary");
  blocks.Backspace(); blocks.Backspace();
  Check(blocks.SourceText() == "你好", "new commit terminates direct editing");
  auto frozen = blocks.Send(false);
  blocks.Append("新", InputKind::kDirect);
  Check(frozen && blocks.source.front().text == "你好",
        "typing never changes an in-flight block");
  blocks.Acknowledge(frozen->request, a, true);
  Check(blocks.SourceText() == "新", "ack consumes only the frozen head");
  blocks.Focus(b); blocks.Open(); blocks.Append("目标", InputKind::kDirect);
  Check(blocks.source.size() == 2, "direct-input runs cannot cross focus targets");
  Model m;
  m.Focus(a);
  m.Open();
  m.Append("你好。");
  m.Append("第二句。");
  auto d = m.Send(true);
  Check(d.has_value(), "send starts");
  Check(m.source.size() == 2, "enqueue does not consume");
  Check(!m.Acknowledge(d->request, b, true), "foreign ack rejected");
  Check(m.Pending(), "foreign ack retains pending");
  auto next = m.Acknowledge(d->request, a, true);
  Check(next.has_value() && m.source.size() == 1, "confirmed prefix consumed");
  m.Acknowledge(next->request, a, false);
  Check(m.source.size() == 1, "failed suffix retained");
  m.Focus(b);
  Check(!m.Send(false), "cannot redirect old target");
  m.Open();
  auto job = m.Generate(2, false);
  Check(m.Stream(job, 2, "result"), "stream accepted");
  m.Append("new");
  Check(!m.Stream(job, 2, "stale"), "source edit retires generation");
  m.result.clear();
  job = m.Generate(3, false);
  Check(!m.Stream(job, 4, "wrong route"), "route revision frozen");
  m.Cancel();
  Check(!m.Stream(job, 3, "late"), "cancel retires generation");
  Model t;
  t.translate = true;
  t.Focus(a);
  t.Open();
  t.Append("第一句。");
  job = t.Generate(1, true);
  t.Append("第二句。");
  Check(t.Stream(job, 1, "First sentence."), "stable prefix survives append");
  Check(t.Finish(job, 1, true), "translation completes");
  d = t.Send(false);
  Check(d.has_value(), "translation deliverable");
  t.Acknowledge(d->request, a, true);
  Check(t.SourceText() == "第二句。", "translation retires exact prefix once");
  t.Append("💡");
  Check(t.Backspace() && t.SourceText() == "第二句。",
        "backspace preserves UTF-8");
  d = t.Send(false);
  Check(!d && !t.uncertain, "translation never falls back to untranslated source");
  job = t.Generate(1, true);
  t.Stream(job, 1, "Second sentence."); t.Finish(job, 1, true);
  d = t.Send(false);
  t.LostAcknowledgement();
  Check(t.uncertain && !t.Send(false), "lost ack never retries");
  t.Open();
  auto resend = t.Send(false);
  Check(resend && resend->request != d->request && resend->block.id == d->block.id &&
            resend->block.text == "Second sentence." && !t.uncertain,
        "explicit Send after a lost ack resends the retained block");
  Check(!t.OwnsDelivery(d->request, a), "resend retires the unconfirmed request's guard");
  t.Acknowledge(d->request, a, true);
  Check(t.Pending() && t.result.size() == 1, "late ack for the retired request consumes nothing");
  t.Acknowledge(resend->request, a, true);
  Check(t.result.empty() && t.SourceText().empty(), "resent block consumes its source once");
  Model closed;
  closed.translate = true; closed.Focus(a); closed.Open(); closed.Append("第一句。");
  job = closed.Generate(1, true);
  closed.Stream(job, 1, "First sentence."); closed.Finish(job, 1, true);
  d = closed.Send(false);
  closed.AbandonDelivery(b.session);
  Check(closed.Pending(), "another session's close keeps the delivery");
  closed.AbandonDelivery(a.session);
  Check(!closed.Pending() && closed.uncertain && closed.result.size() == 1,
        "closed session releases the lock and retains the block");
  Check(closed.Append("第二句。"), "typing resumes after a closed-session release");
  resend = closed.Send(false);
  Check(resend && resend->block.text == "First sentence.", "explicit Send resends after a closed session");
  t.Protect();
  Check(!t.visible && !t.capture && !t.live, "protection revokes authority");
  Check(Sentences("a.\nb。c").size() == 3,
        "sentence boundaries retain newlines");
  Model incremental;
  incremental.translate = true;
  incremental.Focus(a);
  incremental.Open();
  incremental.Append("One. Two.");
  auto first = incremental.Generate(1, true);
  incremental.Stream(first, 1, "一。");
  Check(incremental.Finish(first, 1, true), "first incremental result");
  auto second = incremental.Generate(1, true);
  Check(second.source == "Two.",
        "translation only processes unprocessed suffix");
  incremental.Stream(second, 1, "二。");
  Check(incremental.Finish(second, 1, true),
        "append translation without requiring a send");
  Check(incremental.result.size() == 2, "two translated blocks coexist");
  d = incremental.Send(true);
  next = incremental.Acknowledge(d->request, a, true);
  Check(incremental.SourceText() == "Two." && next.has_value(),
        "first result consumes only its original sentence");
  incremental.Acknowledge(next->request, a, true);
  Check(incremental.source.empty() && incremental.result.empty(),
        "translated output drains exactly once");
  Model stable;
  stable.translate = true;
  stable.Append("半句");
  auto waiting = stable.Generate(1, true, true);
  Check(!stable.busy && waiting.source.empty(),
        "automatic translation waits for a sentence boundary");
  stable.Append("结束。  ");
  auto complete = stable.Generate(1, true, true);
  Check(stable.busy && complete.source == "半句结束。  ",
        "automatic translation freezes a complete sentence including spaces");
  stable.Append("下一句尚未完成");
  Check(stable.Stream(complete, 1, "Completed.") &&
            stable.Finish(complete, 1, true),
        "appending a new sentence preserves the completed prefix");
  Check(stable.Generate(1, true, true).source.empty() && !stable.busy,
        "unfinished suffix is not automatically sent");
  Check(!stable.Generate(1, true).source.empty() && stable.busy,
        "explicit translation can send an unfinished suffix");
  Model disposable;
  Check(NormalizeTranslation("  First sentence.  ", "第一句。", "English") == "First sentence. " &&
        NormalizeTranslation("你好", "  Hello.\n", "Chinese") == "  你好\n",
        "translated units retain source whitespace and separate Latin sentences");
  Model chips;
  chips.Focus(a); chips.Open(); chips.BeginTranslation(); chips.Append("一句较长的原文。");
  auto chips_job = chips.Generate(1, true);
  const std::string long_translation = "This translated sentence contains several words and preserves every original byte.";
  chips.Stream(chips_job, 1, long_translation); chips.Finish(chips_job, 1, true);
  Check(chips.result.size() > 1 && chips.ResultText() == long_translation,
        "long translations form small chips without losing spaces or source ownership");
  const auto chip_delivery = chips.Send(false); chips.Acknowledge(chip_delivery->request, a, true);
  Check(chips.source.empty() && !chips.result.empty(), "first small chip retires the entire source unit exactly once");
  Model tail;
  tail.Focus(a); tail.Open(); tail.BeginTranslation(); tail.Append("我想", InputKind::kDirect);
  auto partial = tail.Generate(1, true);
  tail.Stream(partial, 1, "I want"); tail.Finish(partial, 1, true);
  const auto original_tail = tail.source.front().id;
  tail.Append("回家", InputKind::kDirect);
  Check(tail.result.empty() && tail.source.size() == 1 && tail.source.front().id == original_tail,
        "continuing a translated unfinished sentence preserves editability and retires stale output");
  partial = tail.Generate(1, true);
  Check(partial.source == "我想回家", "retranslation includes the entire changed sentence");
  tail.Append("。下一句");
  Check(!tail.busy && !tail.Stream(partial, 1, "stale"), "late partial translation cannot survive a tail edit");
  auto prefix_job = tail.Generate(1, true);
  tail.Append("x", InputKind::kDirect); tail.Backspace();
  Check(tail.Stream(prefix_job, 1, "First. Remaining.") && tail.Finish(prefix_job, 1, true),
        "editing a later sentence does not cancel the stable active unit");
  auto sent = tail.Send(false); Check(sent.has_value(), "stable prefix is deliverable");
  tail.Acknowledge(sent->request, a, true);
  tail.ResetTranslation();
  Check(tail.ResultText() == "Remaining." && tail.SourceText() == "下一句",
        "language change keeps immutable pending children and never restores their retired source");
  tail.translate = false; tail.Append("补充");
  Check(tail.ResultText() == "Remaining.", "editing in another mode retains already-started translation");
  tail.ReturnToEdit();
  Check(tail.ResultText() == "Remaining.", "another plugin's edit action cannot erase retired translation children");
  tail.BeginTranslation();
  Check(tail.Generate(2, true).source == "下一句补充", "resuming never retranslates retired source");
  tail.Cancel();
  auto parked = tail.ParkResults();
  Check(tail.result.empty(), "parked plugin output is removed from the active rail");
  tail.translate = false; tail.Append("继续");
  tail.RestoreResults(std::move(parked)); tail.BeginTranslation();
  Check(tail.ResultText() == "Remaining." && tail.Generate(3, true).source == "下一句补充继续",
        "restoring a workspace preserves retired children without reviving consumed source");
  tail.Discard(); Check(tail.result.empty(), "privacy discard erases retained translation");
  disposable.Focus(a); disposable.Open(); disposable.Append("Temporary.");
  auto obsolete = disposable.Generate(1, false);
  disposable.Stream(obsolete, 1, "Temporary result.");
  disposable.Finish(obsolete, 1, true);
  auto old_delivery = disposable.Send(true);
  disposable.Discard();
  Check(disposable.source.empty() && disposable.result.empty() &&
            !disposable.Pending() && !disposable.capture && !disposable.visible,
        "maintenance discards pending Buffer content without a confirmation");
  Check(!disposable.Stream(obsolete, 1, "Late text"),
        "a pre-reset generation cannot repopulate the Buffer");
  disposable.Focus(a); disposable.Open(); disposable.Append("New.");
  auto new_delivery = disposable.Send(false);
  Check(new_delivery && old_delivery && new_delivery->request > old_delivery->request,
        "reset preserves monotonic delivery identities");
  disposable.Acknowledge(old_delivery->request, a, true);
  Check(disposable.SourceText() == "New." && disposable.Pending(),
        "late reset acknowledgement never consumes new content");
  Model streamed; streamed.Focus(a); streamed.Open(); streamed.Append("Request"); streamed.generated_mode = true;
  const auto live_job = streamed.Generate(4, false);
  Check(streamed.Stream(live_job, 4, "First. ") && streamed.streaming.size() == 1, "first delta stages a live chip");
  const auto stable_id = streamed.streaming.front().id;
  streamed.Focus({});
  Check(!streamed.capture && streamed.busy && streamed.streaming.front().id == stable_id,
        "blur revokes input but retains live stream and block identities");
  Check(streamed.Stream(live_job, 4, "Second 💡.") && streamed.streaming.size() == 2 &&
      streamed.streaming.front().id == stable_id && streamed.result.empty() && !streamed.Send(false), "incomplete chips cannot be delivered");
  streamed.Focus(b); streamed.Open();
  Check(streamed.Accepts(live_job, 4) && streamed.SourceText() == "Request",
        "fresh focus resumes the existing workspace without restarting generation");
  Check(streamed.Finish(live_job, 4, true) && streamed.result.front().id == stable_id && streamed.streaming.empty(), "finalization preserves streamed identities");
  streamed.ReturnToEdit();
  const auto failed = streamed.Generate(4, false); streamed.Stream(failed, 4, "Partial.");
  Check(!streamed.Finish(failed, 4, false) && streamed.streaming.empty() && streamed.result.empty() && streamed.SourceText() == "Request", "failed stream retires every partial chip");
  for (const std::string exact : {"Dr. Smith uses https://example.com/a.b?q=1,2 and version 1.2.3. Next.",
      "\"Keep, this quote.\" Then split.", "`x = 1.2; y = 3;` done.", "组合 e\xcc\x81 👩‍💻。 下一句，继续。"}) {
    const auto fragments = SemanticBlocks(exact); std::string joined;
    for (const auto& fragment : fragments) joined += fragment.text;
    Check(joined == exact, "semantic blocks preserve Unicode and every original byte");
    if (exact.starts_with("Dr.")) Check(fragments.front().text.find("https://example.com/a.b?q=1,2") != std::string::npos, "URL stays in one semantic block");
  }
  std::string many; for (int i=0; i<550; ++i) many += "句。";
  Check(SemanticBlocks(many).size() == 200, "oversized semantic output uses bounded balanced groups");
  BufferLiveMetrics metrics; metrics.Key(0,false,false); metrics.Key(200,true,false); metrics.Commit(1000,2);
  Check(metrics.keys == 1 && metrics.Line().find("120 字/分") != std::string::npos, "typing burst rate ignores repeat");
  metrics.Key(2000,false,true); Check(metrics.backspaces == 1, "typing burst counts backspace");
  metrics.Key(9001,false,false); Check(metrics.keys == 1 && metrics.characters == 0, "six-second idle starts a fresh burst");
  BufferAutoSendClock clock; std::deque<Block> timed{{1,"first"},{2,"second"}};
  Check(!clock.Tick(timed,0,true,1000) && clock.Tick(timed,1000,true,1000) && clock.Age(2) == 1000, "all blocks age concurrently");
  clock.Pause(); timed.pop_front(); Check(clock.Tick(timed,9000,true,1000), "next already-mature block does not restart its wait");
  timed.front().text = "edited"; Check(!clock.Tick(timed,10000,true,1000) && clock.Age(2) == 0, "text replacement resets only its clock");
  clock.Tick(timed,10500,true,1000); clock.Tick(timed,10600,false,1000); clock.Tick(timed,20000,true,1000);
  Check(clock.Age(2) == 500, "paused time is excluded from countdown");
  std::cout << "Workbench model tests passed\n";
}
