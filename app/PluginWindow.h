#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>

namespace amp
{
/** A hosted plugin's native editor in its own window: the plugin's editor (createEditorIfNeeded), or JUCE's generic
    parameter editor when the plugin has none. The window owns the editor, so deleting the window deletes the
    editor -- which must happen before the plugin instance is destroyed. */
class PluginWindow final : public juce::DocumentWindow
{
public:
    PluginWindow(juce::AudioPluginInstance&, juce::String nodeId, std::function<void(PluginWindow&)> onCloseRequested);
    ~PluginWindow() override;

    void closeButtonPressed() override;

    const juce::String nodeId;

private:
    std::function<void(PluginWindow&)> onCloseRequested;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginWindow)
};

/** The app's plugin editor windows: at most one per node id (opening it again brings it to the front).

    Lifetime: Engine::beforeNodesReplaced (rig load, structural edit, device rebuild, ~Engine) calls closeAll()
    before any plugin instance is destroyed. For a rebuild of the same rig the editors that were open are reopened
    (at the same position) once the rebuild has finished, through `reopen` -- removed nodes simply fail to reopen.
    The owner must destroy this (closing every window) before the Engine. */
class PluginWindows
{
public:
    /** reopen(nodeId) asks the engine to open that node's editor again (Engine::openPluginEditor). */
    explicit PluginWindows(std::function<void(const juce::String&)> reopen);
    ~PluginWindows();

    void open(juce::AudioPluginInstance&, const juce::String& nodeId);
    void close(const juce::String& nodeId);
    /** keepForReopen: remember the open node ids and reopen them asynchronously (same rig, rebuilt). A later
        closeAll(false) (another rig) cancels a pending reopen. */
    void closeAll(bool keepForReopen);

private:
    /** From a window's close button: the window is deleted asynchronously (never inside its own callback), and
        only if it is still the window shown for that node. */
    void requestClose(PluginWindow&);

    std::function<void(const juce::String&)> reopen;
    std::map<juce::String, std::unique_ptr<PluginWindow>> windows;
    std::map<juce::String, juce::Rectangle<int>> lastBounds; // where each node's window was, for reopening
    std::shared_ptr<bool> alive = std::make_shared<bool>(true); // guards the async callbacks
    int generation = 0; // bumped by closeAll(false): a pending reopen of the previous rig's editors is dropped
};
} // namespace amp
