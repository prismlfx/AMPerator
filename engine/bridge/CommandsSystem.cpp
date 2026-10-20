// Engine command handlers: asset and plugin browsing, plugin editors, settings and the tuner.

#include "Engine.h"

#include "bridge/Bridge.h"

#include <type_traits>

namespace amp
{
namespace
{
template <typename T>
bool same(const T& a, const T& b)
{
    if constexpr (std::is_floating_point_v<T>)
        return juce::exactlyEqual(a, b);
    else
        return a == b;
}
} // namespace

CommandResult Engine::listAssets(const juce::String& kind)
{
    if (kind != "nam" && kind != "ir")
        return CommandResult::failure("unknown asset kind: " + kind + " (expected \"nam\" or \"ir\")");
    juce::Array<juce::var> out;
    for (auto& e : assets.list(kind))
        out.add(json::asset(e));
    return CommandResult::success(json::object({ { "assets", out } }));
}

CommandResult Engine::listPlugins()
{
    juce::Array<juce::var> out;
    for (auto& d : plugins->available())
        out.add(json::pluginRef(PluginHost::refFor(d)));
    return CommandResult::success(json::object({ { "plugins", out } }));
}

CommandResult Engine::rescanPlugins()
{
    plugins->scan([](float) {}); // blocks the message thread for the whole scan (in-process)
    return listPlugins();
}

CommandResult Engine::openPluginEditor(const juce::String& id)
{
    auto* pn = live ? live->plan->find(id) : nullptr;
    auto* node = pn != nullptr ? dynamic_cast<PluginNode*>(pn->node.get()) : nullptr;
    if (! onOpenPluginEditor || node == nullptr || node->instance() == nullptr)
        return CommandResult::failure("not available");
    onOpenPluginEditor(node->instance(), id);
    return CommandResult::success();
}

CommandResult Engine::getSettings()
{
    auto v = json::settings(settings);
    juce::Array<juce::var> rates, buffers;
    juce::StringArray devices, inputs, outputs;
    if (audio)
    {
        devices = audio->availableDevices();
        inputs = audio->inputChannelNames();
        outputs = audio->outputChannelNames();
        for (auto r : audio->sampleRates())
            rates.add(r);
        for (auto b : audio->bufferSizes())
            buffers.add(b);
    }
    auto* o = v.getDynamicObject();
    o->setProperty("availableDevices", json::strings(devices));
    o->setProperty("availableSampleRates", rates);
    o->setProperty("availableBufferSizes", buffers);
    o->setProperty("availableInputChannels", json::strings(inputs));
    o->setProperty("availableOutputChannels", json::strings(outputs));
    return CommandResult::success(v);
}

CommandResult Engine::setSettings(const SettingsPatch& p)
{
    bool deviceChanged = false; // only an actual change reopens the device
    auto set = [&deviceChanged](auto& field, const auto& value)
    {
        if (value && ! same(field, *value))
        {
            field = *value;
            deviceChanged = true;
        }
    };
    set(settings.audioDeviceType, p.audioDeviceType);
    set(settings.audioDeviceName, p.audioDeviceName);
    set(settings.inputChannel, p.inputChannel);
    set(settings.outputFirstChannel, p.outputFirstChannel);
    set(settings.bufferSize, p.bufferSize);
    set(settings.sampleRate, p.sampleRate);

    juce::Result deviceResult = juce::Result::ok();
    if (deviceChanged && audio)
    {
        deviceResult = audio->apply(settings); // a successful (re)open rebuilds the plan via onDeviceRestarted
        audio->writeBack(settings);
    }
    if (p.watchedFolders)
    {
        settings.watchedFolders = *p.watchedFolders;
        applyAssetRoots();
    }
    if (p.globalMappings)
    {
        settings.globalMappings = *p.globalMappings;
        refreshMappings();
    }
    if (p.footswitchKeys)
    {
        settings.footswitchKeys = *p.footswitchKeys;
        hid.setKeyAssignments(settings.footswitchKeys);
    }
    saveSettings();
    pollStatus();

    if (deviceResult.failed())
        return CommandResult::failure(deviceResult.getErrorMessage());
    return getSettings();
}

void Engine::injectRawKey(int code) { handleRawKey(code); }

void Engine::handleRawKey(int code)
{
    lastRawKey = code;
    if (! pendingAssign.isEmpty())
    {
        for (auto it = settings.footswitchKeys.begin(); it != settings.footswitchKeys.end();)
            it = it->second == pendingAssign ? settings.footswitchKeys.erase(it) : std::next(it);
        settings.footswitchKeys[code] = pendingAssign; // also takes the key over from any other control
        pendingAssign = {};
        hid.setCapturing(false);
        hid.setKeyAssignments(settings.footswitchKeys);
        saveSettings();
    }
    pollStatus();
}

CommandResult Engine::assignFootswitchKey(const juce::String& control)
{
    if (control != "footswitch:L" && control != "footswitch:M" && control != "footswitch:R")
        return CommandResult::failure("not a footswitch control: " + control);
    pendingAssign = control;
    hid.setCapturing(true);
    pollStatus();
    return CommandResult::success();
}

CommandResult Engine::cancelAssignFootswitchKey()
{
    pendingAssign = {};
    hid.setCapturing(false);
    pollStatus();
    return CommandResult::success();
}

CommandResult Engine::setTuner(bool on)
{
    if (on && ! tunerOn)
    {
        tuner.reset();
        tunerTimer.startTimerHz(20);
    }
    else if (! on)
        tunerTimer.stopTimer();
    tunerOn = on;
    return CommandResult::success(json::object({ { "on", tunerOn } }));
}
} // namespace amp
