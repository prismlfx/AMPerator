#include "PluginWindow.h"

namespace amp
{
PluginWindow::PluginWindow(juce::AudioPluginInstance& instance, juce::String id,
                           std::function<void(PluginWindow&)> onClose)
    : DocumentWindow(instance.getName(), juce::Colour(0xff1b1d22), DocumentWindow::closeButton | DocumentWindow::minimiseButton),
      nodeId(std::move(id)),
      onCloseRequested(std::move(onClose))
{
    setUsingNativeTitleBar(true);
    juce::AudioProcessorEditor* editor = instance.hasEditor() ? instance.createEditorIfNeeded() : nullptr;
    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor(instance);
    setContentOwned(editor, true);
    setResizable(editor->isResizable(), false);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

PluginWindow::~PluginWindow()
{
    clearContentComponent(); // deletes the editor (AudioProcessor::editorBeingDeleted) while the instance lives
}

void PluginWindow::closeButtonPressed()
{
    onCloseRequested(*this);
}

//==============================================================================
PluginWindows::PluginWindows(std::function<void(const juce::String&)> r) : reopen(std::move(r)) {}

PluginWindows::~PluginWindows()
{
    *alive = false;
    windows.clear();
}

void PluginWindows::open(juce::AudioPluginInstance& instance, const juce::String& nodeId)
{
    if (auto it = windows.find(nodeId); it != windows.end() && ! it->second->isVisible())
        close(nodeId); // closing (hidden, deletion pending): open a fresh one instead
    if (auto it = windows.find(nodeId); it != windows.end())
    {
        it->second->setMinimised(false);
        it->second->toFront(true);
        return;
    }
    auto window = std::make_unique<PluginWindow>(instance, nodeId, [this](PluginWindow& w) { requestClose(w); });
    if (auto b = lastBounds.find(nodeId); b != lastBounds.end())
        window->setTopLeftPosition(b->second.getPosition());
    window->toFront(true);
    windows[nodeId] = std::move(window);
}

void PluginWindows::close(const juce::String& nodeId)
{
    if (auto it = windows.find(nodeId); it != windows.end())
    {
        lastBounds[nodeId] = it->second->getBounds();
        windows.erase(it);
    }
}

void PluginWindows::requestClose(PluginWindow& window)
{
    window.setVisible(false); // feels immediate; the deletion follows on the next message loop turn
    juce::MessageManager::callAsync([this, id = window.nodeId, target = &window, guard = alive]
                                    {
                                        if (! *guard)
                                            return;
                                        if (auto it = windows.find(id); it != windows.end() && it->second.get() == target)
                                            close(id);
                                    });
}

void PluginWindows::closeAll(bool keepForReopen)
{
    juce::StringArray ids;
    for (auto& [id, w] : windows)
    {
        ids.add(id);
        lastBounds[id] = w->getBounds();
    }
    windows.clear();
    if (! keepForReopen)
    {
        ++generation; // another rig: drop any pending reopen
        lastBounds.clear();
        return;
    }
    if (ids.isEmpty())
        return;
    // After the rebuild that is about to happen: the engine has new instances for the nodes that still exist.
    juce::MessageManager::callAsync([this, ids, gen = generation, guard = alive]
                                    {
                                        if (! *guard || gen != generation)
                                            return;
                                        for (auto& id : ids)
                                            reopen(id);
                                    });
}
} // namespace amp
