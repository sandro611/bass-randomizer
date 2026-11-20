#include "PluginProcessor.h"

namespace
{
    struct Scale { const char* name; std::vector<int> notes; };

    const std::vector<Scale>& getScales()
    {
        static const std::vector<Scale> scales = {
            { "Minor",            { 0, 2, 3, 5, 7, 8, 10 } },
            { "Major",            { 0, 2, 4, 5, 7, 9, 11 } },
            { "Dorian",           { 0, 2, 3, 5, 7, 9, 10 } },
            { "Phrygian",         { 0, 1, 3, 5, 7, 8, 10 } },
            { "Harmonic Minor",   { 0, 2, 3, 5, 7, 8, 11 } },
            { "Minor Pentatonic", { 0, 3, 5, 7, 10 } },
            { "Major Pentatonic", { 0, 2, 4, 7, 9 } },
            { "Blues",            { 0, 3, 5, 6, 7, 10 } },
            { "Chromatic",        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } }
        };
        return scales;
    }

    // Stateless hash so the same seed always yields the same pattern.
    uint64_t mix (uint64_t x)
    {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

    int64_t floorDiv (int64_t a, int64_t b)
    {
        int64_t q = a / b;
        if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
        return q;
    }
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout BassRandomizerProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    StringArray scaleNames;
    for (auto& s : getScales()) scaleNames.add (s.name);

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "root", 1 }, "Root Note",
        StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "octave", 1 }, "Base Octave", 0, 4, 1));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "scale", 1 }, "Scale", scaleNames, 0));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "steps", 1 }, "Steps", 1, 32, 16));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "rate", 1 }, "Rate",
        StringArray { "1/4", "1/8", "1/16", "1/32" }, 2));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "density", 1 }, "Density",
        NormalisableRange<float> (0.f, 1.f), 0.75f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "rootBias", 1 }, "Root Bias",
        NormalisableRange<float> (0.f, 1.f), 0.4f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "octJump", 1 }, "Octave Jump",
        NormalisableRange<float> (0.f, 1.f), 0.15f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "gate", 1 }, "Gate",
        NormalisableRange<float> (0.05f, 0.95f), 0.6f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "swing", 1 }, "Swing",
        NormalisableRange<float> (0.f, 0.5f), 0.f));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "velMin", 1 }, "Velocity Min", 1, 127, 70));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "velMax", 1 }, "Velocity Max", 1, 127, 115));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "seed", 1 }, "Seed", 0, 9999, 1));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "evolve", 1 }, "New Pattern Each Loop", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "anchor", 1 }, "Anchor First Step On Root", true));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "channel", 1 }, "MIDI Channel", 1, 16, 1));

    ClipCapture::addParameters (p);

    return { std::make_move_iterator (p.begin()), std::make_move_iterator (p.end()) };
}

//==============================================================================
BassRandomizerProcessor::BassRandomizerProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    pRoot     = apvts.getRawParameterValue ("root");
    pOctave   = apvts.getRawParameterValue ("octave");
    pScale    = apvts.getRawParameterValue ("scale");
    pSteps    = apvts.getRawParameterValue ("steps");
    pRate     = apvts.getRawParameterValue ("rate");
    pDensity  = apvts.getRawParameterValue ("density");
    pRootBias = apvts.getRawParameterValue ("rootBias");
    pOctJump  = apvts.getRawParameterValue ("octJump");
    pGate     = apvts.getRawParameterValue ("gate");
    pSwing    = apvts.getRawParameterValue ("swing");
    pVelMin   = apvts.getRawParameterValue ("velMin");
    pVelMax   = apvts.getRawParameterValue ("velMax");
    pSeed     = apvts.getRawParameterValue ("seed");
    pEvolve   = apvts.getRawParameterValue ("evolve");
    pAnchor   = apvts.getRawParameterValue ("anchor");
    pChannel  = apvts.getRawParameterValue ("channel");
}

void BassRandomizerProcessor::prepareToPlay (double, int)
{
    currentNote = -1;
    lastEndPpq = -1.0;
}

bool BassRandomizerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

//==============================================================================
void BassRandomizerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    processGenerated (buffer, midi);
    capture.processBlock (getPlayHead(), midi, buffer.getNumSamples(), getSampleRate());
}

void BassRandomizerProcessor::processGenerated (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    midi.clear();

    const int numSamples = buffer.getNumSamples();

    auto killNote = [&] (int offset)
    {
        if (currentNote >= 0)
        {
            midi.addEvent (juce::MidiMessage::noteOff (currentChannel, currentNote), offset);
            currentNote = -1;
        }
    };

    // --- transport -----------------------------------------------------------
    bool playing = false;
    double startPpq = 0.0, bpm = 120.0;

    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (auto pp = pos->getPpqPosition()) startPpq = *pp; else playing = false;
            bpm = pos->getBpm().orFallback (120.0);
        }

    if (! playing)
    {
        killNote (0);
        lastEndPpq = -1.0;
        return;
    }

    if (bpm <= 0.0) bpm = 120.0;

    const double samplesPerPpq = getSampleRate() * 60.0 / bpm;
    const double endPpq = startPpq + numSamples / samplesPerPpq;

    if (lastEndPpq >= 0.0 && startPpq < lastEndPpq - 1e-3)   // loop or jump back
        killNote (0);
    lastEndPpq = endPpq;

    auto flushOff = [&]
    {
        if (currentNote >= 0 && noteOffPpq < endPpq)
            killNote (juce::jlimit (0, numSamples - 1, (int) ((noteOffPpq - startPpq) * samplesPerPpq)));
    };
    flushOff();

    // --- parameters ----------------------------------------------------------
    static const double rateLengths[] = { 1.0, 0.5, 0.25, 0.125 };
    const double L        = rateLengths[juce::jlimit (0, 3, (int) pRate->load())];
    const int    steps    = juce::jlimit (1, 32, (int) pSteps->load());
    const double swing    = pSwing->load();
    const float  density  = pDensity->load();
    const float  rootBias = pRootBias->load();
    const float  octJump  = pOctJump->load();
    const double gate     = pGate->load();
    const int    velMin   = (int) pVelMin->load();
    const int    velMax   = juce::jmax (velMin, (int) pVelMax->load());
    const uint64_t seed   = (uint64_t) pSeed->load();
    const bool   evolve   = pEvolve->load() > 0.5f;
    const bool   anchor   = pAnchor->load() > 0.5f;
    const int    channel  = juce::jlimit (1, 16, (int) pChannel->load());
    const int    baseNote = 12 * ((int) pOctave->load() + 1) + (int) pRoot->load();

    const auto& scale = getScales()[(size_t) juce::jlimit (0, (int) getScales().size() - 1, (int) pScale->load())].notes;
    const int scaleLen = (int) scale.size();

    auto stepPos = [&] (int64_t k)
    {
        return (double) k * L + ((k & 1) ? swing * L * 0.5 : 0.0);
    };

    // --- step events in this block --------------------------------------------
    for (int64_t k = (int64_t) std::floor (startPpq / L) - 1; ; ++k)
    {
        const double pos = stepPos (k);
        if (pos >= endPpq) break;
        if (pos < startPpq) continue;

        const int idx = (int) (((k % steps) + steps) % steps);
        const int64_t cycle = evolve ? floorDiv (k, steps) : 0;

        juce::Random r ((juce::int64) mix (mix (mix (seed) + (uint64_t) cycle) + (uint64_t) idx));

        const bool first = (idx == 0 && anchor);
        const bool active = first || r.nextFloat() < density;
        const float rootRoll = r.nextFloat();
        const int degRoll = r.nextInt (scaleLen);
        const bool up = r.nextFloat() < octJump;
        const int vel = velMin + r.nextInt (velMax - velMin + 1);

        if (! active) continue;

        const int degree = (first || rootRoll < rootBias) ? 0 : degRoll;
        const int note = juce::jlimit (0, 127, baseNote + scale[(size_t) degree] + ((up && ! first) ? 12 : 0));
        const int offset = juce::jlimit (0, numSamples - 1, (int) ((pos - startPpq) * samplesPerPpq));

        killNote (offset);
        midi.addEvent (juce::MidiMessage::noteOn (channel, note, (juce::uint8) vel), offset);
        currentNote = note;
        currentChannel = channel;
        noteOffPpq = juce::jmin (pos + gate * L, stepPos (k + 1) - 1e-4);
        flushOff();
    }
}

//==============================================================================
class BassRandomizerEditor : public juce::AudioProcessorEditor
{
public:
    explicit BassRandomizerEditor (BassRandomizerProcessor& p)
        : AudioProcessorEditor (&p), proc (p), generic (p), clipUI (p.capture)
    {
        addAndMakeVisible (randomButton);
        randomButton.setButtonText ("Randomize pattern");
        randomButton.onClick = [this]
        {
            if (auto* seed = proc.apvts.getParameter ("seed"))
            {
                seed->beginChangeGesture();
                seed->setValueNotifyingHost (juce::Random::getSystemRandom().nextFloat());
                seed->endChangeGesture();
            }
        };
        addAndMakeVisible (clipUI);
        addAndMakeVisible (generic);
        setSize (460, 860);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        randomButton.setBounds (b.removeFromTop (44).reduced (6));
        clipUI.setBounds (b.removeFromTop (250));
        generic.setBounds (b);
    }

private:
    BassRandomizerProcessor& proc;
    juce::GenericAudioProcessorEditor generic;
    ClipCaptureUI clipUI;
    juce::TextButton randomButton;
};

juce::AudioProcessorEditor* BassRandomizerProcessor::createEditor()
{
    return new BassRandomizerEditor (*this);
}

//==============================================================================
void BassRandomizerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    capture.saveToTree (state);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void BassRandomizerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            apvts.replaceState (tree);
            capture.loadFromTree (tree);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BassRandomizerProcessor();
}
