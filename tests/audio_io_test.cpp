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
    CHECK(f.io->inputChannelNames() == juce::StringArray { "In 1" });
    CHECK(f.io->outputChannelNames() == juce::StringArray { "Out 1", "Out 2" });
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
    std::vector<std::pair<double, int>> restarts;
    f.io->onDeviceRestarted = [&](double sr, int bs) { restarts.emplace_back(sr, bs); };
    REQUIRE(f.io->apply(Fixture::solo()).wasOk());
    CHECK(restarts.size() == 1);
    seen.clear();

    f.type->removeDevice("Fake Solo");
    f.hotPlug();
    CHECK(f.io->status() == DeviceStatus::Disconnected);
    CHECK(f.manager.getCurrentAudioDevice() == nullptr); // no silent fallback to "Fake Other": output is silent
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == DeviceStatus::Disconnected);
    CHECK(restarts.size() == 1); // JUCE's fallback open of "Fake Other" is not reported

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
    REQUIRE(restarts.size() == 2); // exactly one more, for the reconnected device
    CHECK(restarts.back() == std::make_pair(44100.0, 256));
}

TEST_CASE("audio_io: a device switched outside apply is adopted, not closed")
{
    Fixture f;
    REQUIRE(f.io->apply(Fixture::solo()).wasOk());
    auto other = Fixture::solo();
    other.audioDeviceName = "Fake Other";
    REQUIRE(f.io->apply(other).wasOk());
    f.hotPlug();
    CHECK(f.currentName() == "Fake Other");
    CHECK(f.io->status() == DeviceStatus::Running);

    // Now switch behind AudioIO's back, straight on the manager.
    auto setup = f.manager.getAudioDeviceSetup();
    setup.inputDeviceName = setup.outputDeviceName = "Fake Solo";
    REQUIRE(f.manager.setAudioDeviceSetup(setup, true).isEmpty());
    f.manager.sendSynchronousChangeMessage();
    CHECK(f.currentName() == "Fake Solo");
    CHECK(f.io->status() == DeviceStatus::Running);
    Settings out;
    f.io->writeBack(out);
    CHECK(out.audioDeviceName == "Fake Solo");
}

TEST_CASE("audio_io: a failed reconnect stays Disconnected")
{
    Fixture f;
    std::vector<DeviceStatus> seen;
    REQUIRE(f.io->apply(Fixture::solo()).wasOk());
    f.io->onStatus = [&](DeviceStatus s) { seen.push_back(s); };
    f.type->removeDevice("Fake Solo");
    f.hotPlug();
    f.type->addDevice("Fake Solo");
    f.type->failOpen = true;
    f.hotPlug();
    CHECK(f.io->status() == DeviceStatus::Disconnected);
    CHECK(seen == std::vector<DeviceStatus> { DeviceStatus::Disconnected }); // no NoDevice flicker
    f.type->failOpen = false;
    f.hotPlug();
    CHECK(f.io->status() == DeviceStatus::Running);
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
    REQUIRE(calls.size() == 1);
    CHECK(calls.back().first == 44100.0);
    CHECK(calls.back().second == 256);

    auto s = Fixture::solo();
    s.sampleRate = 48000;
    s.bufferSize = 64;
    REQUIRE(f.io->apply(s).wasOk());
    REQUIRE(calls.size() == 2);
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

TEST_CASE("audio_io: after a fallback the preferred device is watched and reopened with its own channels")
{
    Fixture f;
    f.type->removeDevice("Fake Solo");
    std::vector<std::pair<double, int>> restarts;
    f.io->onDeviceRestarted = [&](double sr, int bs) { restarts.emplace_back(sr, bs); };
    auto s = Fixture::solo();
    s.outputFirstChannel = 1;
    REQUIRE(f.io->apply(s).wasOk());
    CHECK(f.currentName() == "Fake Other");
    CHECK_FALSE(f.io->usingPreferred());
    CHECK(f.io->status() == DeviceStatus::Running);

    f.hotPlug(); // unrelated list change: stays on the fallback
    CHECK(f.currentName() == "Fake Other");

    f.type->addDevice("Fake Solo");
    f.hotPlug();
    CHECK(f.currentName() == "Fake Solo");
    CHECK(f.io->usingPreferred());
    CHECK(f.io->status() == DeviceStatus::Running);
    CHECK(f.manager.getCurrentAudioDevice()->getActiveOutputChannels() == juce::BigInteger(2));
    REQUIRE(restarts.size() == 2);
    CHECK(restarts.back() == std::make_pair(44100.0, 256));
}

TEST_CASE("audio_io: usingPreferred is true when the named device opened")
{
    Fixture f;
    REQUIRE(f.io->apply(Fixture::solo()).wasOk());
    CHECK(f.io->usingPreferred());
}
