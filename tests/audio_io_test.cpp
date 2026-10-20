#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "audio/AudioIO.h"
#include "support/FakeAudioDevice.h"

using namespace amp;
using Catch::Matchers::WithinAbs;

namespace
{
struct Fixture
{
    const juce::ScopedJuceInitialiser_GUI juce; // this thread becomes the message thread
    juce::AudioDeviceManager manager;
    amp_test::FakeType* type = nullptr;
    RealtimeEngine engine;
    std::unique_ptr<AudioIO> io;

    Fixture()
    {
        auto fake = std::make_unique<amp_test::FakeType>();
        type = fake.get();
        manager.addAudioDeviceType(std::move(fake)); // the only type: no real CoreAudio is ever touched
        io = std::make_unique<AudioIO>(manager, engine);
    }

    static Settings solo()
    {
        Settings s;
        s.audioDeviceType = "Fake";
        s.audioDeviceName = "Fake Solo";
        s.sampleRate = 44100;
        s.bufferSize = 256;
        return s;
    }

    juce::String currentName() const
    {
        auto* d = manager.getCurrentAudioDevice();
        return d != nullptr ? d->getName() : juce::String();
    }

    // What the OS does on hot-plug: the type announces the change, the manager broadcasts it.
    void hotPlug()
    {
        type->notifyDeviceListChanged();
        manager.sendSynchronousChangeMessage();
    }
};
} // namespace

TEST_CASE("audio_io: applies settings to named device")
{
    Fixture f;
    REQUIRE(f.io->apply(Fixture::solo()).wasOk());

    CHECK(f.io->status() == DeviceStatus::Running);
    CHECK(f.currentName() == "Fake Solo");
    auto* dev = f.manager.getCurrentAudioDevice();
    REQUIRE(dev != nullptr);
    CHECK(dev->getCurrentSampleRate() == 44100.0);
    CHECK(dev->getCurrentBufferSizeSamples() == 256);
    CHECK(dev->getActiveInputChannels().getHighestBit() == 0);
    CHECK(dev->getActiveOutputChannels().getHighestBit() == 1);
    CHECK(f.io->availableDevices().contains("Fake Solo"));
    CHECK(f.io->availableDevices().contains("Fake Other"));
    CHECK(f.io->sampleRates().contains(48000.0));
    CHECK(f.io->bufferSizes().contains(64));
}

TEST_CASE("audio_io: unknown device falls back to default")
{
    Fixture f;
    auto s = Fixture::solo();
    s.audioDeviceName = "No Such Interface";
    REQUIRE(f.io->apply(s).wasOk());

    CHECK(f.io->status() == DeviceStatus::Running);
    CHECK(f.currentName() == "Fake Other"); // the type's default device, not merely the first in the list
    Settings out = s;
    f.io->writeBack(out);
    CHECK(out.audioDeviceName == "Fake Other");
    CHECK(out.inputChannel == 0);
    CHECK(out.outputFirstChannel == 0);
}

TEST_CASE("audio_io: out-of-range channels fall back to input 0 and outputs 0/1")
{
    Fixture f;
    auto s = Fixture::solo();
    s.inputChannel = 5;
    s.outputFirstChannel = 7;
    REQUIRE(f.io->apply(s).wasOk());

    Settings out;
    f.io->writeBack(out);
    CHECK(out.inputChannel == 0);
    CHECK(out.outputFirstChannel == 0);
}

TEST_CASE("audio_io: disconnect sets status and reconnect restores same settings")
{
    Fixture f;
    std::vector<DeviceStatus> seen;
    f.io->onStatus = [&](DeviceStatus s) { seen.push_back(s); };
    REQUIRE(f.io->apply(Fixture::solo()).wasOk());
    seen.clear();

    f.type->removeDevice("Fake Solo");
    f.hotPlug();
    CHECK(f.io->status() == DeviceStatus::Disconnected);
    CHECK(f.manager.getCurrentAudioDevice() == nullptr); // no silent fallback to "Fake Other": output is silent
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == DeviceStatus::Disconnected);

    f.type->addDevice("Fake Solo");
    f.hotPlug();
    CHECK(f.io->status() == DeviceStatus::Running);
    CHECK(f.currentName() == "Fake Solo");
    auto* dev = f.manager.getCurrentAudioDevice();
    REQUIRE(dev != nullptr);
    CHECK(dev->getCurrentSampleRate() == 44100.0);
    CHECK(dev->getCurrentBufferSizeSamples() == 256);
    REQUIRE(seen.size() == 2);
    CHECK(seen[1] == DeviceStatus::Running);
}

TEST_CASE("audio_io: a stopped device is reopened once the change notification arrives")
{
    Fixture f;
    REQUIRE(f.io->apply(Fixture::solo()).wasOk());
    f.manager.getCurrentAudioDevice()->stop(); // driver stopped it; the device is still listed
    f.manager.sendSynchronousChangeMessage();
    CHECK(f.io->status() == DeviceStatus::Running);
    CHECK(f.manager.getCurrentAudioDevice()->isPlaying());
}

TEST_CASE("audio_io: latency formula")
{
    Fixture f;
    f.type->inputLatency = 32;
    f.type->outputLatency = 32;
    auto s = Fixture::solo();
    s.sampleRate = 48000;
    s.bufferSize = 64;
    REQUIRE(f.io->apply(s).wasOk());
    CHECK_THAT(f.io->roundTripLatencyMs(), WithinAbs(4.0, 0.01)); // (32 + 32 + 2*64) / 48000 * 1000
}

TEST_CASE("audio_io: writeBack captures current device")
{
    Fixture f;
    REQUIRE(f.io->apply(Fixture::solo()).wasOk());

    Settings out;
    out.lastRigId = "keep-me";
    f.io->writeBack(out);
    CHECK(out.audioDeviceType == "Fake");
    CHECK(out.audioDeviceName == "Fake Solo");
    CHECK(out.sampleRate == 44100.0);
    CHECK(out.bufferSize == 256);
    CHECK(out.inputChannel == 0);
    CHECK(out.outputFirstChannel == 0);
    CHECK(out.lastRigId == "keep-me");
}

TEST_CASE("audio_io: open failure reports the error and NoDevice")
{
    Fixture f;
    f.type->failOpen = true;
    const auto r = f.io->apply(Fixture::solo());
    CHECK(r.failed());
    CHECK(r.getErrorMessage().contains("refused to open"));
    CHECK(f.io->status() == DeviceStatus::NoDevice);
    CHECK(f.io->xrunCount() == -1);
    CHECK(f.io->roundTripLatencyMs() == 0.0);
}

TEST_CASE("audio_io: onDeviceRestarted reports the rate and buffer the device opened with")
{
    Fixture f;
    std::vector<std::pair<double, int>> calls;
    f.io->onDeviceRestarted = [&](double sr, int bs) { calls.emplace_back(sr, bs); };
    REQUIRE(f.io->apply(Fixture::solo()).wasOk());
    REQUIRE(! calls.empty());
    CHECK(calls.back().first == 44100.0);
    CHECK(calls.back().second == 256);

    auto s = Fixture::solo();
    s.sampleRate = 48000;
    s.bufferSize = 64;
    REQUIRE(f.io->apply(s).wasOk());
    CHECK(calls.back().first == 48000.0);
    CHECK(calls.back().second == 64);
}

TEST_CASE("audio_io: xrunCount reads the device counter, -1 without a device")
{
    Fixture f;
    CHECK(f.io->xrunCount() == -1);
    REQUIRE(f.io->apply(Fixture::solo()).wasOk());
    CHECK(f.io->xrunCount() == 0);
    static_cast<amp_test::FakeDevice*>(f.manager.getCurrentAudioDevice())->xruns = 3;
    CHECK(f.io->xrunCount() == 3);
}
