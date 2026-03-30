/*
  ==============================================================================
    PlugNspectrPre  –  PluginProcessor.h

    Pass-through VST3 plugin inserted *before* the plugin under analysis.
    On every processBlock it copies the audio into the Windows named shared
    memory "BiltroyPlugNspectrShared" so PlugNspectrPost can compare the
    pre-signal against the post-signal.
  ==============================================================================
*/

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <JuceHeader.h>
#include "SharedMemoryBlock.h"

class PlugNspectrPreProcessor  : public juce::AudioProcessor
{
public:
    //==========================================================================
    PlugNspectrPreProcessor();
    ~PlugNspectrPreProcessor() override;

    //==========================================================================
    void prepareToPlay   (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==========================================================================
    // No editor — Pre is a silent capture plugin
    bool                        hasEditor()    const override { return false; }
    juce::AudioProcessorEditor* createEditor()       override { return nullptr; }

    //==========================================================================
    const juce::String getName() const override { return JucePlugin_Name; }

    bool   acceptsMidi()           const override { return false; }
    bool   producesMidi()          const override { return false; }
    bool   isMidiEffect()          const override { return false; }
    double getTailLengthSeconds()  const override { return 0.0;   }

    int                getNumPrograms()                              override { return 1;  }
    int                getCurrentProgram()                           override { return 0;  }
    void               setCurrentProgram (int)                       override {}
    const juce::String getProgramName    (int)                       override { return {}; }
    void               changeProgramName (int, const juce::String&)  override {}

    void getStateInformation (juce::MemoryBlock&)    override {}
    void setStateInformation (const void*, int)      override {}

private:
    //==========================================================================
    HANDLE m_hMapFile = nullptr;
    PNS_SharedBlock* m_pShared = nullptr;

    void openSharedMemory();
    void closeSharedMemory();

    // Command block — written by Post (test-tone controls), read here
    HANDLE        m_hCmdFile = nullptr;
    PNS_CmdBlock* m_pCmd     = nullptr;

    void openCmdMemory();
    void closeCmdMemory();

    // Sine test-tone generator state
    double m_tonePhase = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlugNspectrPreProcessor)
};
