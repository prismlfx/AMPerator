// Catch2 entry point. The test executable doubles as the plugin-scan worker for the out-of-process scanner tests:
// launched with the scanner's command line it serves the Fake format (with the misbehaving extra ids) over the
// pipe and exits when the coordinator lets go, never running any tests.
#include <catch2/catch_session.hpp>

#include <juce_events/juce_events.h>

#include "plugins/PluginScanner.h"

#include "support/FakePlugins.h"

int main(int argc, char* argv[])
{
    juce::String commandLine;
    for (int i = 1; i < argc; ++i)
        commandLine << (i > 1 ? " " : "") << argv[i];

    if (amp::PluginScannerWorker::isWorkerCommandLine(commandLine))
    {
        // AMP_SCAN_WORKER_LOG=<file>: append a line per step, for debugging the launch itself.
        const juce::File log(juce::SystemStats::getEnvironmentVariable("AMP_SCAN_WORKER_LOG", {}));
        const auto note = [&](const juce::String& line) { if (log != juce::File()) log.appendText(line + "\n"); };
        note("worker started: " + commandLine);
        const juce::ScopedJuceInitialiser_GUI juce;
        note("juce initialised");
        auto worker = amp::PluginScannerWorker::startIfRequested(commandLine, [](juce::AudioPluginFormatManager& fm)
        {
            fm.addFormat(new amp_test::FakeFormat({ "fake://crash", "fake://crash-once", "fake://hang" }));
        });
        note(worker != nullptr ? "connected" : "connect failed");
        if (worker == nullptr)
            return 1;
        // No NSApplication in this process (runDispatchLoop would return at once): pump the loop until the
        // coordinator lets go.
        while (worker->isConnected())
            juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
        note("loop ended");
        return 0;
    }
    return Catch::Session().run(argc, argv);
}
