#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "engine/RtSafety.h"
#include "plugins/PluginHost.h"

#include "support/FakePlugins.h"

using namespace amp_test;

namespace
{
struct Fixture
{
    const juce::ScopedJuceInitialiser_GUI juce; // this thread becomes the message thread
    juce::TemporaryFile tmp;
    juce::File dir = tmp.getFile();

    Fixture() { dir.createDirectory(); }
    ~Fixture() { dir.deleteRecursively(); }

    std::unique_ptr<amp::PluginHost> makeHost() const { return makeFakePluginHost(dir); }
};

std::optional<juce::PluginDescription> findByName(const juce::Array<juce::PluginDescription>& list,
                                                  const juce::String& name)
{
    for (const auto& d : list)
        if (d.name == name)
            return d;
    return std::nullopt;
}

const amp::ProcessSpec spec { 48000.0, 256 };
} // namespace

TEST_CASE("plugin_host: create known plugin returns node")
{
    Fixture f;
    auto host = f.makeHost();
    host->scan([](float) {});
    const auto desc = findByName(host->available(), "FakeGain");
    REQUIRE(desc.has_value());

    const auto ref = amp::PluginHost::refFor(*desc);
    REQUIRE(ref.format == "Fake");
    REQUIRE(ref.uid == desc->createIdentifierString());
    REQUIRE(ref.name == "FakeGain");
    REQUIRE(ref.vendor == "Test Vendor");

    juce::String error;
    auto node = host->create(ref, spec, error);
    REQUIRE(node != nullptr);
    REQUIRE(error.isEmpty());
    REQUIRE(node->isStereo());
    REQUIRE_FALSE(f.dir.getChildFile("crash-marker").exists()); // cleared after creation

    auto* pluginNode = dynamic_cast<amp::PluginNode*>(node.get());
    REQUIRE(pluginNode != nullptr);
    REQUIRE(pluginNode->instance() != nullptr);
    pluginNode->instance()->getHostedParameter(0)->setValue(0.25f); // linear gain 0.5

    node->prepare(spec);
    REQUIRE_FALSE(f.dir.getChildFile("crash-marker").exists()); // cleared after first prepare
    juce::AudioBuffer<float> io(2, spec.maxBlock);
    for (int ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::fill(io.getWritePointer(ch), 0.8f, spec.maxBlock);
    amp::RtSafety::reset();
    {
        const amp::RtSafety::ScopedNoAlloc noAlloc;
        node->process(io, 128);
    }
    REQUIRE(amp::RtSafety::violations() == 0);
    REQUIRE(std::abs(io.getSample(0, 0) - 0.4f) < 1e-6f);
    REQUIRE(std::abs(io.getSample(1, 127) - 0.4f) < 1e-6f);
    REQUIRE(std::abs(io.getSample(0, 128) - 0.8f) < 1e-6f); // beyond numSamples: untouched
}

TEST_CASE("plugin_host: format and name fall back when uid is unknown")
{
    Fixture f;
    auto host = f.makeHost();
    host->scan([](float) {});
    juce::String error;
    auto node = host->create({ "Fake", "Fake-FakeGain-stale-uid", "FakeGain", "" }, spec, error);
    REQUIRE(node != nullptr);
}

TEST_CASE("plugin_host: unknown uid returns null with error")
{
    Fixture f;
    auto host = f.makeHost();
    host->scan([](float) {});
    juce::String error;
    auto node = host->create({ "Fake", "Fake-Nope-123", "Nope", "Nobody" }, spec, error);
    REQUIRE(node == nullptr);
    REQUIRE(error.isNotEmpty());
}

TEST_CASE("plugin_host: state round trips")
{
    Fixture f;
    auto host = f.makeHost();
    host->scan([](float) {});
    const auto ref = amp::PluginHost::refFor(*findByName(host->available(), "FakeGain"));
    juce::String error;
    auto a = host->create(ref, spec, error);
    auto b = host->create(ref, spec, error);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    dynamic_cast<amp::PluginNode&>(*a).instance()->getHostedParameter(0)->setValue(0.125f);
    const auto state = a->getState();
    REQUIRE(state.getSize() > 0);
    b->setState(state);
    REQUIRE(dynamic_cast<amp::PluginNode&>(*b).instance()->getHostedParameter(0)->getValue() == 0.125f);
}

TEST_CASE("plugin_host: crash marker on startup blacklists that plugin")
{
    Fixture f;
    juce::String uid;
    {
        auto host = f.makeHost();
        host->scan([](float) {});
        uid = amp::PluginHost::refFor(*findByName(host->available(), "FakeGain")).uid;
    }
    // Simulate a crash during creation: the marker survives because the process died.
    REQUIRE(f.dir.getChildFile("crash-marker").replaceWithText(uid));

    auto host = f.makeHost(); // reloads known-plugins.xml, no rescan
    host->checkCrashMarkerOnStartup();
    REQUIRE(host->blacklisted().contains(uid));
    REQUIRE_FALSE(f.dir.getChildFile("crash-marker").exists());
    REQUIRE(f.dir.getChildFile("blacklist.txt").loadFileAsString().contains(uid));

    host->checkCrashMarkerOnStartup(); // no marker: nothing changes
    REQUIRE(host->blacklisted().size() == 1);

    // The same plugin crashing again must not produce a duplicate entry.
    REQUIRE(f.dir.getChildFile("crash-marker").replaceWithText(uid));
    host->checkCrashMarkerOnStartup();
    REQUIRE(host->blacklisted().size() == 1);
    juce::StringArray lines;
    f.dir.getChildFile("blacklist.txt").readLines(lines);
    lines.removeEmptyStrings();
    REQUIRE(lines.size() == 1);
    REQUIRE(f.makeHost()->blacklisted().size() == 1);
}

TEST_CASE("plugin_host: blacklisted plugin cannot be created and is hidden from available()")
{
    Fixture f;
    auto host = f.makeHost();
    host->scan([](float) {});
    const auto ref = amp::PluginHost::refFor(*findByName(host->available(), "FakeGain"));
    REQUIRE(f.dir.getChildFile("crash-marker").replaceWithText(ref.uid));
    host->checkCrashMarkerOnStartup();

    REQUIRE_FALSE(findByName(host->available(), "FakeGain").has_value());
    REQUIRE(findByName(host->available(), "FakeBoost").has_value());
    juce::String error;
    REQUIRE(host->create(ref, spec, error) == nullptr);
    REQUIRE(error.isNotEmpty());
}

TEST_CASE("plugin_host: unblacklist restores it")
{
    Fixture f;
    auto host = f.makeHost();
    host->scan([](float) {});
    const auto ref = amp::PluginHost::refFor(*findByName(host->available(), "FakeGain"));
    REQUIRE(f.dir.getChildFile("crash-marker").replaceWithText(ref.uid));
    host->checkCrashMarkerOnStartup();

    host->unblacklist(ref.uid);
    REQUIRE(host->blacklisted().isEmpty());
    REQUIRE(findByName(host->available(), "FakeGain").has_value());
    juce::String error;
    REQUIRE(host->create(ref, spec, error) != nullptr);
    REQUIRE_FALSE(f.dir.getChildFile("blacklist.txt").loadFileAsString().contains(ref.uid));

    auto reloaded = f.makeHost(); // blacklist.txt persists the removal
    REQUIRE(reloaded->blacklisted().isEmpty());
}

TEST_CASE("plugin_host: a plugin file that crashed the scan is skipped on the next scan")
{
    Fixture f;
    SECTION("our crash marker") { REQUIRE(f.dir.getChildFile("crash-marker").replaceWithText("fake://boost")); }
    SECTION("JUCE dead-man's pedal") { REQUIRE(f.dir.getChildFile("scan-in-progress").replaceWithText("fake://boost")); }

    auto host = f.makeHost();
    host->checkCrashMarkerOnStartup();
    REQUIRE(host->blacklisted().contains("fake://boost"));

    float lastProgress = 0.0f;
    host->scan([&](float p) { lastProgress = p; });
    REQUIRE(lastProgress == 1.0f);
    REQUIRE(findByName(host->available(), "FakeGain").has_value());
    REQUIRE_FALSE(findByName(host->available(), "FakeBoost").has_value());
    REQUIRE_FALSE(f.dir.getChildFile("crash-marker").exists());
    REQUIRE_FALSE(f.dir.getChildFile("scan-in-progress").loadFileAsString().contains("fake://"));
}

namespace
{
// Creates the named plugin, prepares it, fills io with distinct per-channel levels (L 0.8, R 0.6) and runs
// two blocks under the allocation guard. Returns the node; io holds the output of the second block.
std::unique_ptr<amp::ProcessorNode> createAndRun(Fixture& f, const juce::String& name, juce::AudioBuffer<float>& io)
{
    auto host = f.makeHost();
    host->scan([](float) {});
    const auto desc = findByName(host->available(), name);
    REQUIRE(desc.has_value());
    juce::String error;
    auto node = host->create(amp::PluginHost::refFor(*desc), spec, error);
    REQUIRE(node != nullptr);
    node->prepare(spec);

    io.setSize(2, spec.maxBlock);
    amp::RtSafety::reset();
    for (int block = 0; block < 2; ++block)
    {
        juce::FloatVectorOperations::fill(io.getWritePointer(0), 0.8f, spec.maxBlock);
        juce::FloatVectorOperations::fill(io.getWritePointer(1), 0.6f, spec.maxBlock);
        const amp::RtSafety::ScopedNoAlloc noAlloc;
        node->process(io, 128);
    }
    REQUIRE(amp::RtSafety::violations() == 0);
    return node;
}

bool near(float a, float b) { return std::abs(a - b) < 1e-6f; }
} // namespace

TEST_CASE("plugin_host: mono-only plugin processes channel 0 and leaves channel 1 alone")
{
    Fixture f;
    juce::AudioBuffer<float> io;
    auto node = createAndRun(f, "FakeMono", io);
    REQUIRE_FALSE(node->isStereo());
    auto& plugin = dynamic_cast<FakeMono&>(*dynamic_cast<amp::PluginNode&>(*node).instance());
    REQUIRE(plugin.channelsSeen == 1);
    REQUIRE(near(io.getSample(0, 0), 0.4f));
    REQUIRE(near(io.getSample(0, 127), 0.4f));
    REQUIRE(near(io.getSample(1, 0), 0.6f));
}

TEST_CASE("plugin_host: mono-in stereo-out layout is negotiated and fed from channel 0")
{
    Fixture f;
    juce::AudioBuffer<float> io;
    auto node = createAndRun(f, "FakeMonoToStereo", io);
    REQUIRE(node->isStereo());
    auto& plugin = dynamic_cast<FakeMonoToStereo&>(*dynamic_cast<amp::PluginNode&>(*node).instance());
    REQUIRE(plugin.channelsSeen == 2);
    REQUIRE(near(io.getSample(0, 0), 0.4f)); // 0.5 * ch0
    REQUIRE(near(io.getSample(1, 0), 0.2f)); // 0.25 * ch0 (ch1's 0.6 is not an input)
}

TEST_CASE("plugin_host: sidechain channels use cleared scratch, main bus maps to io")
{
    Fixture f;
    juce::AudioBuffer<float> io;
    auto node = createAndRun(f, "FakeSidechain", io);
    REQUIRE(node->isStereo());
    auto& plugin = dynamic_cast<FakeSidechain&>(*dynamic_cast<amp::PluginNode&>(*node).instance());
    REQUIRE(plugin.channelsSeen == 4);
    REQUIRE(plugin.sidechainPeak == 0.0f); // second block: the first block's scribbles were cleared
    REQUIRE(near(io.getSample(0, 0), 0.4f));
    REQUIRE(near(io.getSample(1, 127), 0.3f));
}

TEST_CASE("plugin_host: plugin teardown runs under the crash marker")
{
    Fixture f;
    gCrashMarker = f.dir.getChildFile("crash-marker");
    gMarkerDuringRelease = gMarkerDuringDestruction = false;
    {
        juce::AudioBuffer<float> io;
        auto node = createAndRun(f, "FakeGain", io);
        REQUIRE_FALSE(gCrashMarker.exists());
    }
    REQUIRE(gMarkerDuringRelease);
    REQUIRE(gMarkerDuringDestruction);
    REQUIRE_FALSE(gCrashMarker.exists());
    gCrashMarker = juce::File();
}

namespace
{
struct SnapshotFixture : Fixture
{
    std::unique_ptr<amp::PluginHost> host = makeHost();
    juce::File cache = dir.getChildFile("Art").getChildFile("cache");
    amp::PluginRef ref;
    std::unique_ptr<amp::ProcessorNode> node;

    SnapshotFixture()
    {
        host->scan([](float) {});
        ref = amp::PluginHost::refFor(*findByName(host->available(), "FakeGain"));
        juce::String error;
        node = host->create(ref, spec, error);
        REQUIRE(node != nullptr);
    }
    juce::AudioPluginInstance& instance() const { return *dynamic_cast<amp::PluginNode&>(*node).instance(); }
};
} // namespace

TEST_CASE("plugin_host: snapshotEditor caches a PNG of a painted editor")
{
    SnapshotFixture f;
    const ScopedFakeEditor editor(FakeEditorKind::Painted);

    const auto png = f.host->snapshotEditor(f.instance(), f.ref.uid, f.cache);
    REQUIRE(png.existsAsFile());
    CHECK(png == amp::PluginHost::snapshotFile(f.cache, f.ref.uid));
    CHECK(png.getParentDirectory() == f.cache);
    CHECK(png.hasFileExtension("png"));
    const auto image = juce::ImageFileFormat::loadFrom(png);
    REQUIRE(image.isValid());
    CHECK(image.getWidth() == 200);
    CHECK(image.getPixelAt(5, 5).getARGB() == 0xff2060c0);
    CHECK(f.instance().getActiveEditor() == nullptr);              // the offscreen editor is gone again
    CHECK_FALSE(f.dir.getChildFile("crash-marker").exists());       // guarded like every plugin call

    // Cached: a later call returns the file without creating another editor.
    const ScopedFakeEditor none(FakeEditorKind::None);
    CHECK(f.host->snapshotEditor(f.instance(), f.ref.uid, f.cache) == png);
}

TEST_CASE("plugin_host: snapshotEditor caches nothing for a blank editor or none")
{
    SnapshotFixture f;
    {
        const ScopedFakeEditor none(FakeEditorKind::None);
        CHECK(f.host->snapshotEditor(f.instance(), f.ref.uid, f.cache) == juce::File());
    }
    {
        const ScopedFakeEditor blank(FakeEditorKind::Blank);
        CHECK(f.host->snapshotEditor(f.instance(), f.ref.uid, f.cache) == juce::File());
        CHECK_FALSE(amp::PluginHost::snapshotFile(f.cache, f.ref.uid).exists());
    }
    // A blank result is remembered, so the editor is not created offscreen again.
    const ScopedFakeEditor painted(FakeEditorKind::Painted);
    CHECK(f.host->snapshotEditor(f.instance(), f.ref.uid, f.cache) == juce::File());
}

TEST_CASE("plugin_host: snapshot runs under its own marker, so a crash there never blacklists the plugin")
{
    SnapshotFixture f;
    const ScopedFakeEditor editor(FakeEditorKind::Painted);
    const auto blank = amp::PluginHost::snapshotFile(f.cache, f.ref.uid).withFileExtension("blank");
    gEditorProbeFile = blank;
    gCrashMarker = f.dir.getChildFile("crash-marker");

    REQUIRE(f.host->snapshotEditor(f.instance(), f.ref.uid, f.cache).existsAsFile());
    CHECK(gProbeExistedDuringEditor);       // the snapshot marker was in place while the editor was created...
    CHECK_FALSE(gCrashMarkerDuringEditor);  // ...the plugin crash marker was not
    CHECK_FALSE(blank.exists());            // removed after the PNG was written
    gEditorProbeFile = juce::File();
    gCrashMarker = juce::File();
}

TEST_CASE("plugin_host: a snapshot that crashed is not retried and the plugin stays usable")
{
    SnapshotFixture f;
    const ScopedFakeEditor editor(FakeEditorKind::Painted);
    const auto blank = amp::PluginHost::snapshotFile(f.cache, f.ref.uid).withFileExtension("blank");
    REQUIRE(f.cache.createDirectory());
    REQUIRE(blank.replaceWithText(f.ref.uid)); // what a crash inside snapshotEditor leaves behind

    auto host = f.makeHost(); // next launch
    host->checkCrashMarkerOnStartup();
    CHECK(host->blacklisted().isEmpty());
    CHECK(host->snapshotEditor(f.instance(), f.ref.uid, f.cache) == juce::File()); // never tried again
    CHECK_FALSE(amp::PluginHost::snapshotFile(f.cache, f.ref.uid).exists());
    juce::String error;
    CHECK(host->create(f.ref, spec, error) != nullptr);
}

TEST_CASE("plugin_host: snapshot file names are safe for any plugin id")
{
    const juce::File cache("/tmp/cache");
    const auto f = amp::PluginHost::snapshotFile(cache, "VST3-Some Plug/in:x-1a2b3c4d-../../etc");
    CHECK(f.getParentDirectory() == cache);
    CHECK(f.hasFileExtension("png"));
    CHECK_FALSE(f.getFileName().containsAnyOf("/\\:"));
}
