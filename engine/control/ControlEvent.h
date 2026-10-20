#pragma once

#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>

namespace amp
{
/** One edge from a physical control. `control` is a stable ID: "footswitch:L", "midi:<device>:cc<n>", ... */
struct ControlEvent
{
    juce::String control;
    bool pressed; // true on the down edge, false on the up edge
};

/** Something that produces ControlEvents. `onEvent` is only ever called on the message thread. */
class ControlSource
{
public:
    virtual ~ControlSource() { alive->store(false); } // derived classes stop their producers first

    std::function<void(const ControlEvent&)> onEvent;

protected:
    /** Callable from any producer thread: hops to the message thread and calls onEvent there,
        unless cancelPending() ran in between. */
    void post(const ControlEvent& e)
    {
        juce::MessageManager::callAsync([this, token = alive, e] {
            if (token->load() && onEvent)
                onEvent(e);
        });
    }

    /** Message thread only, with every producer thread already quiesced (joined / closed):
        drops events posted so far so that none is delivered after stop() or destruction. */
    void cancelPending()
    {
        alive->store(false);
        alive = std::make_shared<std::atomic<bool>>(true);
    }

private:
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};
} // namespace amp
