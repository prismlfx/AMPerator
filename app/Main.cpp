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
        engine->onQuitConfirmed = [this]
        {
            // After the bridge has sent its reply to confirmQuit.
            juce::MessageManager::callAsync([] { juce::JUCEApplication::quit(); });
        };
        window = std::make_unique<amp::MainWindow>(*engine);

        // Ask for microphone access first, without blocking: opening the input device while the OS prompt is
        // pending would block the message thread inside CoreAudio until the user answers.
        amp::requestMicrophoneAccess([this](bool granted)
                                     {
                                         if (engine != nullptr)
                                             engine->startAudio(granted);
                                     });
    }

    /** Cmd-Q and the window's close button. With unsaved changes the engine asks the web UI (quitRequested ->
        Save / Discard / Cancel -> confirmQuit); if the page does not answer within 5 s (a broken page), a native
        dialog asks instead. */
    void systemRequestedQuit() override
    {
        if (engine == nullptr || engine->requestQuit())
        {
            quit();
            return;
        }
        quitFallback.startTimer(5000);
    }

    void shutdown() override
    {
        quitFallback.stopTimer();
        window.reset(); // detaches Bridge::send before the engine goes away
        engine.reset();
    }

private:
    void askNatively()
    {
        quitFallback.stopTimer();
        if (engine == nullptr || ! engine->quitPending() || nativeAskOpen)
            return;
        nativeAskOpen = true;
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                         .withIconType(juce::MessageBoxIconType::WarningIcon)
                                         .withTitle("Quit AMPerator")
                                         .withMessage("The current rig has unsaved changes. Save them before quitting?")
                                         .withButton("Save")
                                         .withButton("Discard")
                                         .withButton("Cancel"),
                                     [this](int result) // three buttons: 1, 2, 0
                                     {
                                         nativeAskOpen = false;
                                         if (engine == nullptr)
                                             return;
                                         const auto action = result == 1 ? "save" : result == 2 ? "discard" : "cancel";
                                         const auto r = engine->confirmQuit(action);
                                         if (! r.ok)
                                             juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                                                              .withIconType(juce::MessageBoxIconType::WarningIcon)
                                                                              .withTitle("Couldn't save")
                                                                              .withMessage(r.error)
                                                                              .withButton("OK"),
                                                                          nullptr);
                                     });
    }

    std::unique_ptr<amp::Engine> engine;
    std::unique_ptr<amp::MainWindow> window;
    juce::TimedCallback quitFallback { [this] { askNatively(); } };
    bool nativeAskOpen = false;
};
} // namespace

START_JUCE_APPLICATION(AmperatorApp)
