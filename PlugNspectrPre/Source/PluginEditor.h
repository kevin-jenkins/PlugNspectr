/*
  ==============================================================================
    PlugNspectrPre  –  PluginEditor.h

    Simple informational editor — shows signal-chain setup instructions
    and a live connection indicator for PlugNspectrPost.
  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class PlugNspectrPreEditor : public juce::AudioProcessorEditor,
                             private juce::Timer
{
public:
    explicit PlugNspectrPreEditor (PlugNspectrPreProcessor& p);
    ~PlugNspectrPreEditor() override;

    void paint   (juce::Graphics& g) override;
    void resized ()                   override {}

private:
    void timerCallback() override;

    PlugNspectrPreProcessor& m_proc;
    bool m_postConnected = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlugNspectrPreEditor)
};
