#include "BackgroundProcess.h"

#import <Cocoa/Cocoa.h>

namespace amp
{
void runAsBackgroundProcess()
{
    [NSApplication sharedApplication]; // the same instance JUCE picks up afterwards
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
}
} // namespace amp
