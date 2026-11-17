#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>

namespace amp
{
/** The TONE3000 sign-in / picker window: a web view at `url`, in its own window. The web content is third-party:
    the browser has no native integration and no native functions, only https pages load, and no new windows open.

    The first request starting with `redirectPrefix` (TONE3000 redirects there with the result in the query) is not
    loaded: done(<text after '?'>) is called and the window closes itself, both asynchronously. Closing the window
    by hand calls done(""). done is called at most once; destroying the window (app quit) does not call it.
    The owner deletes the window when onClosed fires (never inside its own callback chain: it is called
    asynchronously) or whenever it likes. */
class Tone3000AuthWindow final : public juce::DocumentWindow
{
public:
    /** `centreAround`: the window is centred over this component (the main window) when given. */
    Tone3000AuthWindow(const juce::String& url, const juce::String& redirectPrefix,
                       std::function<void(const juce::String& query)> done, juce::Component* centreAround,
                       std::function<void(Tone3000AuthWindow&)> onClosed);
    ~Tone3000AuthWindow() override;

    void closeButtonPressed() override;

    /** Reports done("") (when nothing was reported yet); the owner then deletes the window. */
    void cancel();

private:
    class Browser;

    void finish(const juce::String& query);

    std::function<void(const juce::String&)> done;
    std::function<void(Tone3000AuthWindow&)> onClosed;
    juce::Component::SafePointer<Tone3000AuthWindow> self { this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Tone3000AuthWindow)
};
} // namespace amp
