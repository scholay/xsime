#include "buffer_auto_send.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace rimes::buffer;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    AutoSendClock clock;
    std::vector<Block> blocks{{"a", "hello", Origin::Rime, 0}};
    clock.Synchronize("default", blocks);
    Check(!clock.Tick(100, true, 1) && clock.Age("a") == 0, "first tick inherited empty time");
    Check(!clock.Tick(100.5, true, 1), "premature delivery");
    clock.Pause();
    Check(!clock.Tick(500, true, 1) && clock.Age("a") == .5, "hidden time aged source");
    blocks.push_back({"b", "world", Origin::Rime, 0});
    clock.Synchronize("default", blocks);
    Check(clock.Tick(500.5, true, 1) == "a" && clock.Age("b") == 0, "new block inherited previous age");
    // An aged head stays pending until native delivery actually removes it.
    Check(clock.Tick(500.6, true, 1) == "a", "tick consumed source");
    blocks.erase(blocks.begin()); clock.Synchronize("default", blocks);
    Check(!clock.Tick(500.7, true, 1), "new head skipped its own clock");
    blocks[0].text = "edited"; clock.Synchronize("default", blocks);
    Check(!clock.Tick(900, true, 1) && clock.Age("b") == 0, "same ID text replacement retained age");
    Check(!clock.Tick(899, true, 1) && clock.Age("b") == 0, "backward time accrued age");
    clock.Tick(899.5, true, 1);
    clock.Tick(900, false, 1);
    Check(!clock.Tick(1000, true, 1) && clock.Age("b") == .5, "disabled interval accrued age");
    clock.Synchronize("plugin", blocks);
    Check(!clock.Tick(1100, true, 1) && clock.Age("b") == 0, "workspace switch retained age");
    Check(!clock.Tick(std::numeric_limits<double>::quiet_NaN(), true, 1), "nonfinite tick accepted");
    clock.Reset(); clock.Synchronize("default", blocks);
    Check(!clock.Tick(2000, true, 1) && clock.Age("b") == 0, "privacy reset retained age");
    std::cout << "Auto-send monotonic time, source identity, pause and delivery retirement passed\n";
}
