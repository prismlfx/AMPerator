#pragma once

#include <memory>

#include "dsp/Node.h"

namespace amp
{
// Ranges: dsp/ParamRanges.h (out-of-range values are clamped, non-finite ones ignored).
std::unique_ptr<ProcessorNode> makeGate();        // params: threshold dB (-55), release ms (100)
std::unique_ptr<ProcessorNode> makeEQ();          // params: low, mid, high dB (0), midFreq Hz (800)
std::unique_ptr<ProcessorNode> makeOutputGain();  // param: level dB (0, [-60, 12])
std::unique_ptr<ProcessorNode> makePlaceholder(); // passthrough
} // namespace amp
