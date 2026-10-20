#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

namespace amp
{
class Engine;

/** The app window: a WebBrowserComponent hosting the web UI (ui/), connected to the engine's JSON bridge.

    Native functions registered with the page (called from ui/src/bridge.ts):
      amp(json)       one bridge command envelope (a JSON string, or an already-decoded object) -> Bridge::handle.
                      Completes immediately with undefined; the bridge's reply arrives as an "amp" event, like
                      every other engine event (Bridge::send -> emitEventIfBrowserIsVisible("amp", message)).
      chooseFolder()  opens a native folder chooser (the web view has none); resolves with the chosen absolute
                      path, or "" when cancelled.

    The page is served from the embedded ui/dist (binary data) through the resource provider; with the environment
    variable AMP_UI_DEV=1 it loads the Vite dev server at http://localhost:5173 instead (cd ui && npm run dev). */
class MainWindow final : public juce::DocumentWindow
{
public:
    explicit MainWindow(Engine&);
    ~MainWindow() override;

    void closeButtonPressed() override;

private:
    class Content;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};
} // namespace amp
