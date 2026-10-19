#pragma once

#include "dsp/Node.h"

// Runs `in` through `node` in blocks of `block` samples and returns the output (same size as `in`).
// Calls node.prepare(); channel count of the result matches `in` (nodes see >= 2 channels internally).
juce::AudioBuffer<float> renderOffline(amp::ProcessorNode& node, const juce::AudioBuffer<float>& in,
                                       double sr, int block = 128);
