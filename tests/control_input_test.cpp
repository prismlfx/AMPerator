#include <catch2/catch_test_macros.hpp>

#include "control/HidFootswitch.h"
#include "control/MidiControl.h"
#include "storage/Settings.h"

using namespace amp;

namespace
{
KeyEdgeDetector factoryPedals()
{
    KeyEdgeDetector d;
    d.assign(0x04, "footswitch:L");
    d.assign(0x05, "footswitch:M");
    d.assign(0x06, "footswitch:R");
    return d;
}

struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("amp_control_" + juce::Uuid().toString());
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

std::vector<uint8_t> bootReport(std::initializer_list<uint8_t> keys, uint8_t modifiers = 0)
{
    std::vector<uint8_t> r(8, 0);
    r[0] = modifiers;
    size_t i = 2;
    for (auto k : keys)
        r[i++] = k;
    return r;
}
} // namespace

// --- KeyEdgeDetector ---------------------------------------------------------------------------

TEST_CASE("control_input: down edge emits pressed")
{
    auto d = factoryPedals();
    auto ev = d.feed({ 0x05 });
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].control == "footswitch:M");
    CHECK(ev[0].pressed);
}

TEST_CASE("control_input: up edge emits released")
{
    auto d = factoryPedals();
    d.feed({ 0x04 });
    auto ev = d.feed({});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].control == "footswitch:L");
    CHECK_FALSE(ev[0].pressed);
}

TEST_CASE("control_input: auto-repeat collapses to one press")
{
    auto d = factoryPedals();
    std::vector<ControlEvent> all;
    for (int i = 0; i < 20; ++i)
        for (auto& e : d.feed({ 0x06 }))
            all.push_back(e);
    REQUIRE(all.size() == 1);
    CHECK(all[0].control == "footswitch:R");
    CHECK(all[0].pressed);
}

TEST_CASE("control_input: two pedals at once emit two presses")
{
    auto d = factoryPedals();
    auto ev = d.feed({ 0x04, 0x06 });
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].control == "footswitch:L");
    CHECK(ev[1].control == "footswitch:R");
    CHECK(ev[0].pressed);
    CHECK(ev[1].pressed);

    auto up = d.feed({ 0x06 }); // L lifted, R still held
    REQUIRE(up.size() == 1);
    CHECK(up[0].control == "footswitch:L");
    CHECK_FALSE(up[0].pressed);
}

TEST_CASE("control_input: unassigned key is remembered for learning and emits nothing")
{
    auto d = factoryPedals();
    CHECK_FALSE(d.lastUnassignedKey().has_value());
    CHECK(d.feed({ 0x1e }).empty()); // '1'
    CHECK(d.feed({}).empty());
    REQUIRE(d.lastUnassignedKey().has_value());
    CHECK(*d.lastUnassignedKey() == 0x1e);

    d.assign(0x1e, "footswitch:L"); // learned
    auto ev = d.feed({ 0x1e });
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].control == "footswitch:L");
}

TEST_CASE("control_input: clearing assignments silences previously assigned keys")
{
    auto d = factoryPedals();
    CHECK(d.clearAssignments().empty()); // nothing held
    CHECK(d.feed({ 0x04 }).empty());
    CHECK(d.lastUnassignedKey() == std::optional<uint8_t>(0x04));
}

TEST_CASE("control_input: reassigning while a pedal is held releases it under the old map")
{
    auto d = factoryPedals();
    REQUIRE(d.feed({ 0x04 }).size() == 1);
    auto released = d.clearAssignments();
    REQUIRE(released.size() == 1);
    CHECK(released[0].control == "footswitch:L");
    CHECK_FALSE(released[0].pressed);

    d.assign(0x04, "footswitch:R");
    CHECK(d.feed({ 0x04 }).empty()); // still the same physical hold: no phantom press
    CHECK(d.feed({}).empty());       // its press was never reported, so neither is its release
    auto ev = d.feed({ 0x04 });
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].control == "footswitch:R");
    CHECK(ev[0].pressed);
}

// --- HID report parsing ------------------------------------------------------------------------

TEST_CASE("control_input: boot report keycodes come from bytes 2..7")
{
    auto r = bootReport({ 0x04, 0x05 });
    auto keys = parseKeyboardReport(r.data(), r.size());
    REQUIRE(keys.has_value());
    CHECK(*keys == std::set<uint8_t> { 0x04, 0x05 });

    auto empty = bootReport({});
    auto none = parseKeyboardReport(empty.data(), empty.size());
    REQUIRE(none.has_value());
    CHECK(none->empty());
}

TEST_CASE("control_input: report-id-prefixed 9-byte report is understood")
{
    std::vector<uint8_t> r { 0x01, 0x00, 0x00, 0x06, 0, 0, 0, 0, 0 };
    auto keys = parseKeyboardReport(r.data(), r.size());
    REQUIRE(keys.has_value());
    CHECK(*keys == std::set<uint8_t> { 0x06 });
}

TEST_CASE("control_input: modifier-only change produces no edges")
{
    auto d = factoryPedals();
    auto a = bootReport({ 0x04 });
    auto b = bootReport({ 0x04 }, 0x02); // shift added
    auto c = bootReport({}, 0x02);       // key lifted, shift still down
    CHECK(d.feed(*parseKeyboardReport(a.data(), a.size())).size() == 1);
    CHECK(d.feed(*parseKeyboardReport(b.data(), b.size())).empty());
    auto ev = d.feed(*parseKeyboardReport(c.data(), c.size()));
    REQUIRE(ev.size() == 1);
    CHECK_FALSE(ev[0].pressed);
}

TEST_CASE("control_input: 9-byte reports need report id 1 and other lengths are ignored")
{
    std::vector<uint8_t> otherId { 0x02, 0x00, 0x00, 0x06, 0, 0, 0, 0, 0 };
    CHECK_FALSE(parseKeyboardReport(otherId.data(), otherId.size()).has_value());
    std::vector<uint8_t> ten(10, 0), sixteen(16, 0);
    CHECK_FALSE(parseKeyboardReport(ten.data(), ten.size()).has_value());
    CHECK_FALSE(parseKeyboardReport(sixteen.data(), sixteen.size()).has_value());
}

TEST_CASE("control_input: short and rollover reports are ignored")
{
    std::vector<uint8_t> shortReport { 0, 0, 4 };
    CHECK_FALSE(parseKeyboardReport(shortReport.data(), shortReport.size()).has_value());
    auto rollover = bootReport({ 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 });
    CHECK_FALSE(parseKeyboardReport(rollover.data(), rollover.size()).has_value());
}

TEST_CASE("control_input: only footswitch-like keyboards are eligible")
{
    // Known VID/PID on the keyboard collection.
    CHECK(isEligibleFootswitch(0x0c45, 0x7403, 0x01, 0x06, ""));
    // Unknown IDs but a footswitch product string, any case.
    CHECK(isEligibleFootswitch(0x1234, 0x5678, 0x01, 0x06, "USB FootSwitch"));
    CHECK(isEligibleFootswitch(0x1234, 0x5678, 0x01, 0x06, "foot switch"));
    CHECK(isEligibleFootswitch(0x1234, 0x5678, 0x01, 0x06, "PCsensor HID"));
    // The user's real typing keyboard is never grabbed.
    CHECK_FALSE(isEligibleFootswitch(0x05ac, 0x0342, 0x01, 0x06, "Apple Internal Keyboard / Trackpad"));
    // Known device, but its mouse collection: not the keyboard interface.
    CHECK_FALSE(isEligibleFootswitch(0x0c45, 0x7403, 0x01, 0x02, "FootSwitch"));
}

// --- MIDI translation --------------------------------------------------------------------------

TEST_CASE("control_input: midi CC 127 then 0 -> pressed then released")
{
    MidiControl m;
    auto down = m.translate(juce::MidiMessage::controllerEvent(1, 64, 127), "Dev");
    REQUIRE(down.has_value());
    CHECK(down->control == "midi:Dev:cc64");
    CHECK(down->pressed);

    CHECK_FALSE(m.translate(juce::MidiMessage::controllerEvent(1, 64, 127), "Dev").has_value()); // no re-press

    auto up = m.translate(juce::MidiMessage::controllerEvent(1, 64, 0), "Dev");
    REQUIRE(up.has_value());
    CHECK(up->control == "midi:Dev:cc64");
    CHECK_FALSE(up->pressed);

    CHECK_FALSE(m.translate(juce::MidiMessage::controllerEvent(1, 64, 10), "Dev").has_value()); // still released
}

TEST_CASE("control_input: midi CC edges are tracked per device and controller")
{
    MidiControl m;
    REQUIRE(m.translate(juce::MidiMessage::controllerEvent(1, 64, 127), "A").has_value());
    REQUIRE(m.translate(juce::MidiMessage::controllerEvent(1, 65, 127), "A").has_value());
    REQUIRE(m.translate(juce::MidiMessage::controllerEvent(1, 64, 127), "B").has_value());
}

TEST_CASE("control_input: midi note on -> pressed, note off -> released")
{
    MidiControl m;
    auto on = m.translate(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), "Dev");
    REQUIRE(on.has_value());
    CHECK(on->control == "midi:Dev:note60");
    CHECK(on->pressed);

    auto off = m.translate(juce::MidiMessage::noteOff(1, 60), "Dev");
    REQUIRE(off.has_value());
    CHECK(off->control == "midi:Dev:note60");
    CHECK_FALSE(off->pressed);

    auto zeroVel = m.translate(juce::MidiMessage::noteOn(1, 61, (juce::uint8) 0), "My Pedal");
    REQUIRE(zeroVel.has_value());
    CHECK(zeroVel->control == "midi:My Pedal:note61");
    CHECK_FALSE(zeroVel->pressed);

    CHECK_FALSE(m.translate(juce::MidiMessage::pitchWheel(1, 9000), "Dev").has_value());
}

// --- Settings ----------------------------------------------------------------------------------

TEST_CASE("control_input: footswitch keys default to PCsensor a/b/c and F13-F15")
{
    Settings s;
    CHECK(s.footswitchKeys == std::map<int, juce::String> {
        { 0x04, "footswitch:L" }, { 0x05, "footswitch:M" }, { 0x06, "footswitch:R" },
        { 0x68, "footswitch:L" }, { 0x69, "footswitch:M" }, { 0x6A, "footswitch:R" } });
}

TEST_CASE("control_input: footswitch keys round trip through settings")
{
    TempDir t;
    auto f = t.dir.getChildFile("settings.json");
    Settings s;
    s.footswitchKeys = { { 0x1e, "footswitch:L" }, { 0x28, "footswitch:R" } };
    REQUIRE(s.save(f));
    auto l = Settings::load(f);
    CHECK(l.footswitchKeys == s.footswitchKeys);
}

TEST_CASE("control_input: footswitch keys parse hex and decimal and fall back defensively")
{
    TempDir t;
    auto f = t.dir.getChildFile("settings.json");

    f.replaceWithText(R"({"footswitchKeys":{"0x07":"footswitch:L","8":"footswitch:M","zz":"x","9":5,"0x1000":"y","0x0a":""}})");
    CHECK(Settings::load(f).footswitchKeys == std::map<int, juce::String> { { 0x07, "footswitch:L" }, { 8, "footswitch:M" } });

    f.replaceWithText(R"({"footswitchKeys":[1,2,3]})");
    CHECK(Settings::load(f).footswitchKeys == Settings().footswitchKeys);

    f.replaceWithText(R"({"footswitchKeys":{}})"); // user cleared every pedal
    CHECK(Settings::load(f).footswitchKeys.empty());
}
