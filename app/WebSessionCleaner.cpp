#include "WebSessionCleaner.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace amp
{
void clearWebSessionFor(const juce::String&, std::function<void()> done)
{
    juce::WebBrowserComponent::clearCookies();
    if (done)
        juce::MessageManager::callAsync(std::move(done));
}
} // namespace amp
