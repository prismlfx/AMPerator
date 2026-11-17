#include "Tone3000AuthWindow.h"

#include "online/ExternalLinks.h"

namespace amp
{
/** Shows third-party pages: https only (plus about:blank), the redirect is intercepted, never loaded. */
class Tone3000AuthWindow::Browser final : public juce::WebBrowserComponent
{
public:
    Browser(juce::String redirect, std::function<void(const juce::String&)> onRedirect)
        : WebBrowserComponent(juce::WebBrowserComponent::Options {}), // no native integration, no native functions
          redirectPrefix(std::move(redirect)),
          redirected(std::move(onRedirect))
    {
    }

    bool pageAboutToLoad(const juce::String& url) override
    {
        if (const auto query = online::callbackQuery(url, redirectPrefix))
        {
            if (! handled)
            {
                handled = true;
                // an empty query ("closed by the user" to the engine) is still a callback: report it as canceled
                redirected(query->isEmpty() ? juce::String("canceled=true") : *query);
            }
            return false;
        }
        return url.startsWith("https://") || url == "about:blank";
    }

    void newWindowAttemptingToLoad(const juce::String& url) override
    {
        if (url.startsWith("https://"))
            goToURL(url);
    }

private:
    juce::String redirectPrefix;
    std::function<void(const juce::String&)> redirected;
    bool handled = false;
};

Tone3000AuthWindow::Tone3000AuthWindow(const juce::String& url, const juce::String& redirectPrefix,
                                       std::function<void(const juce::String&)> doneCallback,
                                       juce::Component* centreAround,
                                       std::function<void(Tone3000AuthWindow&)> closedCallback)
    : DocumentWindow("TONE3000", juce::Colour(0xff121316), DocumentWindow::closeButton),
      done(std::move(doneCallback)),
      onClosed(std::move(closedCallback))
{
    auto* browser = new Browser(redirectPrefix, [this](const juce::String& query) { finish(query); });
    browser->setSize(980, 760);
    setUsingNativeTitleBar(true);
    setContentOwned(browser, true);
    setResizable(true, true);
    if (centreAround != nullptr)
        centreAroundComponent(centreAround, getWidth(), getHeight());
    else
        centreWithSize(getWidth(), getHeight());
    setVisible(true);
    toFront(true);
    browser->goToURL(url);
}

Tone3000AuthWindow::~Tone3000AuthWindow() = default;

void Tone3000AuthWindow::closeButtonPressed()
{
    finish({});
}

void Tone3000AuthWindow::cancel()
{
    if (done)
    {
        auto d = std::move(done);
        done = nullptr;
        d({});
    }
}

/** Reports the result once and asks the owner to delete the window, both from a fresh message-loop turn (the
    redirect arrives inside the web view's navigation callback, the close inside this window's own handler). */
void Tone3000AuthWindow::finish(const juce::String& query)
{
    if (! done)
        return;
    auto d = std::move(done);
    done = nullptr;
    juce::MessageManager::callAsync([safe = self, callback = std::move(d), query]
                                    {
                                        callback(query);
                                        if (safe != nullptr && safe->onClosed)
                                            safe->onClosed(*safe);
                                    });
}
} // namespace amp
