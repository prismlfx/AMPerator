#pragma once

#include <memory>

#include "dsp/Node.h"

namespace amp
{
std::unique_ptr<ProcessorNode> makeGate();        // params: threshold dB (-55), release ms (100)
std::unique_ptr<ProcessorNode> makeEQ();          // params: low, mid, high dB (0), midFreq Hz (800)
std::unique_ptr<ProcessorNode> makeOutputGain();  // param: level dB (0)
std::unique_ptr<ProcessorNode> makePlaceholder(); // passthrough
} // namespace amp
