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

#include <JuceHeader.h>
#include "SharedMemoryBlock.h"   // portable IPC (windows.h / POSIX shm live here)

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
    bool                        hasEditor()    const override { return true; }
    juce::AudioProcessorEditor* createEditor()       override;

    // Returns true if PlugNspectrPost has set a heartbeat within the last second.
    bool isPostConnected() const { return m_ipc.isPostAlive (1000u); }

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

    // Test seam — feed a command directly so the stimulus generators can be
    // driven without any shared memory (used by the unit tests).
    void setTestCommand (const pns::PNS_CmdBlock& c) { m_cmd = c; m_testCmd = true; }

private:
    //==========================================================================
    pns::Transport     m_ipc;            // single cross-platform IPC segment
    pns::PNS_CmdBlock  m_cmd;            // latest command snapshot (read each block)
    bool               m_testCmd = false; // true -> use injected m_cmd, skip IPC read

    // Sine test-tone generator state
    double m_tonePhase = 0.0;

    // White-noise measurement stimulus generator
    juce::Random m_noiseRng;

    // Level-ramp (transfer-curve) / level-step (envelope) stimulus position
    double   m_dynRampPhase     = 0.0;
    uint32_t m_dynEnvBlockStart = 0;   // envelope cycle position at block start

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlugNspectrPreProcessor)
};
