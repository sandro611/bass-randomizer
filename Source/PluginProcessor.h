#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "ClipCapture.h"

class BassRandomizerProcessor : public juce::AudioProcessor
{
public:
    BassRandomizerProcessor();
    ~BassRandomizerProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Bass Randomizer"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    ClipCapture capture { apvts };   // records our MIDI output into a draggable clip

private:
    void processGenerated (juce::AudioBuffer<float>&, juce::MidiBuffer&);
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    std::atomic<float> *pRoot, *pOctave, *pScale, *pSteps, *pRate, *pDensity,
        *pRootBias, *pOctJump, *pGate, *pSwing, *pVelMin, *pVelMax, *pSeed,
        *pEvolve, *pAnchor, *pChannel;

    int currentNote = -1;
    int currentChannel = 1;
    double noteOffPpq = 0.0;
    double lastEndPpq = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassRandomizerProcessor)
};
