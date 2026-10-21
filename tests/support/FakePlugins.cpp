#include "FakePlugins.h"

namespace amp_test
{
std::unique_ptr<amp::PluginHost> makeFakePluginHost(const juce::File& dataDir, juce::StringArray extraIds)
{
    std::vector<std::unique_ptr<juce::AudioPluginFormat>> formats;
    formats.push_back(std::make_unique<FakeFormat>(std::move(extraIds)));
    return std::make_unique<amp::PluginHost>(dataDir, std::move(formats), false);
}
} // namespace amp_test
