#include "Engine.h"
#include "BackgroundProcess.h"
#include "MainWindow.h"
#include "bridge/Bridge.h"
#include "MicrophonePermission.h"
#include "plugins/PluginScanner.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>

namespace
{
class AmperatorApp final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "AMPerator"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override
    {
        // One owner of the audio device and footswitch -- except the plugin-scan workers the app launches itself,
        // and the headless scan (which opens neither).
        const auto args = getCommandLineParameters();
        return amp::PluginScannerWorker::isWorkerCommandLine(args) || isHeadlessScan(args);
    }
    static bool isHeadlessScan(const juce::String& args) { return args.contains("--scan-plugins"); }

    void initialise(const juce::String& commandLine) override
    {
        if (amp::PluginScannerWorker::isWorkerCommandLine(commandLine))
        {
            // Relaunched by the running app to load plugins for a scan: no window, no audio, no Dock icon (main()
            // below arranged that). The process ends when the app disconnects (or when the plugin it is loading
            // crashes it -- the point).
            scanWorker = amp::PluginScannerWorker::startIfRequested(commandLine);
            if (scanWorker == nullptr)
                quit(); // the app that launched us is already gone
            return;
        }

        if (isHeadlessScan(commandLine))
        {
            runHeadlessScan(); // `AMPerator --scan-plugins`: scan from a terminal, progress on stdout
            return;
        }

        amp::Engine::Options options; // the Engine owns the AudioDeviceManager
        options.deferAudioStart = true;
        options.pluginScanWorker = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
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
        scanWorker.reset();
        window.reset(); // detaches Bridge::send before the engine goes away
        engine.reset();
    }

private:
    /** Diagnostic mode for plugin trouble: the same scan the Rescan button runs (worker process, same library
        files), printed to stdout, no window or audio. Run it as Contents/MacOS/AMPerator --scan-plugins. */
    void runHeadlessScan()
    {
        amp::Engine::Options options;
        options.startAudio = false;
        options.startControls = false;
        options.pluginScanWorker = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        engine = std::make_unique<amp::Engine>(std::move(options));
        engine->bridge().send = [](const juce::var& m)
        {
            const auto event = m["event"].toString();
            const auto& data = m["data"];
            if (event == "pluginScanProgress")
            {
                if (data["plugin"].toString().isNotEmpty())
                    std::cout << juce::roundToInt(100.0f * (float) data["progress"]) << "%  " << data["plugin"].toString()
                              << std::endl;
            }
            else if (event == "error")
                std::cout << "error: " << data["message"].toString() << std::endl;
            else if (event == "pluginsChanged")
            {
                const auto n = data["plugins"].isArray() ? data["plugins"].getArray()->size() : 0;
                if (data.hasProperty("error"))
                    std::cout << "scan failed: " << data["error"].toString() << std::endl;
                std::cout << "done: " << n << " plugins available" << std::endl;
                juce::MessageManager::callAsync([] { juce::JUCEApplication::quit(); });
            }
        };
        std::cout << "scanning..." << std::endl;
        engine->rescanPlugins();
    }

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
    std::unique_ptr<amp::PluginScannerWorker> scanWorker; // worker mode only
    juce::TimedCallback quitFallback { [this] { askNatively(); } };
    bool nativeAskOpen = false;
};
} // namespace

// START_JUCE_APPLICATION, unrolled: the scan worker and the headless scan must stay out of the Dock and Cmd-Tab,
// which has to be decided before NSApplication is created (JUCE creates it inside JUCEApplicationBase::main).
JUCE_CREATE_APPLICATION_DEFINE(AmperatorApp)

#if JUCE_MAC
int main(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg(argv[i]);
        if (amp::PluginScannerWorker::isWorkerCommandLine(arg) || AmperatorApp::isHeadlessScan(arg))
        {
            amp::runAsBackgroundProcess();
            break;
        }
    }
    juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;
    return juce::JUCEApplicationBase::main(argc, (const char**) argv);
}
#else
JUCE_MAIN_FUNCTION_DEFINITION
#endif
