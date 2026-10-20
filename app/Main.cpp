#include "Engine.h"
#include "MainWindow.h"
#include "MicrophonePermission.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace
{
class AmperatorApp final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "AMPerator"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; } // one owner of the audio device and footswitch

    void initialise(const juce::String&) override
    {
        amp::Engine::Options options; // the Engine owns the AudioDeviceManager
        options.deferAudioStart = true;
        engine = std::make_unique<amp::Engine>(std::move(options));
        window = std::make_unique<amp::MainWindow>(*engine);

        // Ask for microphone access first, without blocking: opening the input device while the OS prompt is
        // pending would block the message thread inside CoreAudio until the user answers.
        amp::requestMicrophoneAccess([this](bool granted)
                                     {
                                         if (engine != nullptr)
                                             engine->startAudio(granted);
                                     });
    }

    void shutdown() override
    {
        window.reset(); // detaches Bridge::send before the engine goes away
        engine.reset();
    }

private:
    std::unique_ptr<amp::Engine> engine;
    std::unique_ptr<amp::MainWindow> window;
};
} // namespace

START_JUCE_APPLICATION(AmperatorApp)
