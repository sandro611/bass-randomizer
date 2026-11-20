#pragma once
#include <array>
#include <atomic>
#include <map>
#include <vector>
#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
// ClipCapture: records the MIDI that flows through a plugin's MIDI buffer into
// a pattern, shows it in a piano roll and lets the user drag it into the DAW
// as a .mid file (or save it).
//
//  * One-shot mode (default): press Record, press play. Recording starts at the
//    next bar line, captures "Capture Bars" bars, then stops by itself.
//  * Loop Overdub mode: keeps recording around a loop of "Capture Bars" bars.
//==============================================================================

struct RecNote
{
    double start;     // position inside the clip, in quarter notes
    double length;    // in quarter notes
    int note;
    int velocity;
    int channel;
};

class ClipCapture : private juce::Timer
{
public:
    explicit ClipCapture (juce::AudioProcessorValueTreeState& s) : apvts (s)
    {
        pRecord = apvts.getRawParameterValue ("capRecord");
        pBars   = apvts.getRawParameterValue ("capBars");
        pLoop   = apvts.getRawParameterValue ("capLoop");
        pQuant  = apvts.getRawParameterValue ("capQuant");
        recordParam = apvts.getParameter ("capRecord");
        startTimerHz (30);
    }

    ~ClipCapture() override { stopTimer(); }

    static void addParameters (std::vector<std::unique_ptr<juce::RangedAudioParameter>>& p, int defaultQuant = 0)
    {
        using namespace juce;
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "capRecord", 1 }, "Capture Record", false));
        p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "capBars", 1 }, "Capture Bars", 1, 16, 4));
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "capLoop", 1 }, "Capture Loop Overdub", false));
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "capQuant", 1 }, "Capture Quantize On Export",
            StringArray { "Off", "1/4", "1/8", "1/16", "1/32" }, defaultQuant));
    }

    //==========================================================================
    // AUDIO THREAD. Call once per block AFTER the plugin has filled `midi`.
    // channelFilter: 0 = all channels. allowFreeRun: keep a clock running even
    // when the host transport is stopped (for plugins that record live input).
    void processBlock (juce::AudioPlayHead* ph, const juce::MidiBuffer& midi, int numSamples,
                       double sampleRate, int channelFilter = 0, bool allowFreeRun = false)
    {
        bool playing = false;
        double ppqStart = freeRunPpq, bpm = 120.0;

        if (ph != nullptr)
            if (auto pos = ph->getPosition())
            {
                bpm = pos->getBpm().orFallback (120.0);
                if (pos->getIsPlaying())
                    if (auto pp = pos->getPpqPosition())
                    {
                        ppqStart = *pp;
                        playing = true;
                    }
            }

        if (bpm <= 0.0) bpm = 120.0;

        const bool rec = pRecord->load() > 0.5f;
        const bool oneShot = pLoop->load() < 0.5f;
        const bool active = playing || allowFreeRun;

        // free-run clock starts from zero when recording is armed with transport stopped
        if (allowFreeRun && ! playing && rec && ! wasRecording)
        {
            freeRunPpq = 0.0;
            ppqStart = 0.0;
        }

        const double samplesPerPpq = sampleRate * 60.0 / bpm;
        const double ppqEnd = ppqStart + numSamples / samplesPerPpq;
        const double loopLen = juce::jlimit (1, 16, (int) pBars->load()) * 4.0;

        struct Finisher { double& target; double value; ~Finisher() { target = value; } } finisher { freeRunPpq, ppqEnd };

        auto wrap = [loopLen] (double ppq)
        {
            double w = std::fmod (ppq, loopLen);
            if (w < 0.0) w += loopLen;
            return w;
        };
        auto toPos = [&] (double absPpq) { return oneShot ? absPpq - recStart : wrap (absPpq); };

        // ---- record switched off -------------------------------------------------
        if (! rec)
        {
            if (wasRecording) closeHeld (lastPos);
            wasRecording = false;
            state = Idle;
            stateAtomic = Idle;
            return;
        }

        // ---- record just switched on ------------------------------------------------
        if (! wasRecording)
        {
            wasRecording = true;
            if (oneShot)
            {
                push ({ 0.0, 0, 0, 0, Clear });
                state = Armed;
            }
            else
                state = Recording;
        }

        // ---- transport not running --------------------------------------------------
        if (! active)
        {
            closeHeld (lastPos);
            if (oneShot && state != Idle)
            {
                state = Idle;
                done = true;
            }
            stateAtomic = state;
            return;
        }

        // ---- armed: wait for the next bar line --------------------------------------
        if (oneShot && state == Armed)
        {
            const double bar = std::ceil (ppqStart / 4.0 - 1e-9) * 4.0;
            if (bar < ppqEnd - 1e-9)
            {
                recStart = bar;
                state = Recording;
            }
        }

        // ---- recording ---------------------------------------------------------------
        if (state == Recording)
        {
            for (const auto metadata : midi)
            {
                const auto msg = metadata.getMessage();
                if (! (msg.isNoteOn() || msg.isNoteOff())) continue;
                if (channelFilter != 0 && msg.getChannel() != channelFilter) continue;

                const double absPpq = ppqStart + metadata.samplePosition / samplesPerPpq;
                if (oneShot && (absPpq < recStart || absPpq >= recStart + loopLen)) continue;

                const double p = toPos (absPpq);
                const int ch = msg.getChannel();
                const int note = msg.getNoteNumber();
                const size_t idx = (size_t) ((ch - 1) * 128 + note);

                if (msg.isNoteOn())
                {
                    push ({ p, note, (int) msg.getVelocity(), ch, On });
                    held[idx] = true;
                }
                else
                {
                    push ({ p, note, 0, ch, Off });
                    held[idx] = false;
                }
            }

            lastPos = toPos (ppqEnd);

            if (oneShot && ppqEnd >= recStart + loopLen - 1e-9)
            {
                closeHeld (loopLen);
                state = Idle;
                done = true;
            }
        }

        stateAtomic = state;
    }

    //==========================================================================
    // MESSAGE THREAD
    const std::vector<RecNote>& getNotes() const { return notes; }
    double getLoopLengthPpq() const { return juce::jlimit (1, 16, (int) pBars->load()) * 4.0; }
    int getState() const { return stateAtomic.load(); }   // 0 idle, 1 armed, 2 recording
    juce::AudioProcessorValueTreeState& getApvts() { return apvts; }

    void clearPattern()
    {
        drainFifo();
        notes.clear();
        openNotes.clear();
    }

    juce::File getTempExportFile() const
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("MIDI Clip Capture")
                   .getChildFile ("Captured Pattern.mid");
    }

    bool exportMidi (const juce::File& target) const
    {
        const double loopLen = getLoopLengthPpq();
        const int tpq = 960;

        static const double grids[] = { 0.0, 1.0, 0.5, 0.25, 0.125 };
        const double grid = grids[juce::jlimit (0, 4, (int) pQuant->load())];

        juce::MidiMessageSequence seq;
        seq.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4));

        for (const auto& n : notes)
        {
            if (n.start >= loopLen) continue;

            double s = n.start;
            double len = n.length;

            if (grid > 0.0)
            {
                s = std::round (s / grid) * grid;
                len = std::max (grid, std::round (len / grid) * grid);
            }

            if (s >= loopLen) s -= loopLen;
            len = std::min (len, loopLen - s);
            if (len <= 0.01) continue;

            auto on = juce::MidiMessage::noteOn (n.channel, n.note, (juce::uint8) n.velocity);
            on.setTimeStamp (s * tpq);
            auto off = juce::MidiMessage::noteOff (n.channel, n.note);
            off.setTimeStamp ((s + len) * tpq);

            seq.addEvent (on);
            seq.addEvent (off);
        }

        auto endMarker = juce::MidiMessage::endOfTrack();
        endMarker.setTimeStamp (loopLen * tpq);
        seq.addEvent (endMarker);
        seq.updateMatchedPairs();

        juce::MidiFile file;
        file.setTicksPerQuarterNote (tpq);
        file.addTrack (seq);

        target.getParentDirectory().createDirectory();
        target.deleteFile();

        juce::FileOutputStream out (target);
        if (! out.openedOk()) return false;
        return file.writeTo (out);
    }

    void saveToTree (juce::ValueTree& state) const
    {
        auto old = state.getChildWithName ("NOTES");
        if (old.isValid()) state.removeChild (old, nullptr);

        juce::ValueTree noteTree ("NOTES");
        for (const auto& n : notes)
        {
            juce::ValueTree v ("N");
            v.setProperty ("s", n.start, nullptr);
            v.setProperty ("l", n.length, nullptr);
            v.setProperty ("n", n.note, nullptr);
            v.setProperty ("v", n.velocity, nullptr);
            v.setProperty ("c", n.channel, nullptr);
            noteTree.appendChild (v, nullptr);
        }
        state.appendChild (noteTree, nullptr);
    }

    void loadFromTree (const juce::ValueTree& state)
    {
        notes.clear();
        openNotes.clear();

        auto noteTree = state.getChildWithName ("NOTES");
        for (int i = 0; i < noteTree.getNumChildren(); ++i)
        {
            auto v = noteTree.getChild (i);
            notes.push_back ({ (double) v["s"], (double) v["l"], (int) v["n"], (int) v["v"], (int) v["c"] });
        }
    }

private:
    enum Kind { On = 0, Off = 1, Clear = 2 };
    enum State { Idle = 0, Armed = 1, Recording = 2 };

    struct RawEvent { double ppq; int note; int velocity; int channel; int kind; };
    struct OpenNote { double start; int velocity; };

    void push (const RawEvent& e)
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 > 0)
        {
            fifoData[(size_t) s1] = e;
            fifo.finishedWrite (1);
        }
    }

    void closeHeld (double position)
    {
        for (int i = 0; i < (int) held.size(); ++i)
            if (held[(size_t) i])
            {
                push ({ position, i % 128, 0, i / 128 + 1, Off });
                held[(size_t) i] = false;
            }
    }

    void timerCallback() override
    {
        drainFifo();

        if (done.exchange (false) && recordParam != nullptr)
        {
            recordParam->beginChangeGesture();
            recordParam->setValueNotifyingHost (0.0f);
            recordParam->endChangeGesture();
        }
    }

    void addNote (const RecNote& n)
    {
        for (auto& e : notes)
            if (e.note == n.note && e.channel == n.channel && std::abs (e.start - n.start) < 0.02)
            {
                e = n;   // same note at the same place: replace instead of stacking duplicates
                return;
            }

        if (notes.size() < 20000)
            notes.push_back (n);
    }

    void drainFifo()
    {
        const double loopLen = getLoopLengthPpq();

        auto closeNote = [this, loopLen] (int key, double endPos, int note, int ch)
        {
            auto it = openNotes.find (key);
            if (it == openNotes.end()) return;

            const double start = it->second.start;
            double len = endPos >= start ? endPos - start : endPos + loopLen - start;
            if (len < 0.02) len = 0.02;

            addNote ({ start, len, note, it->second.velocity, ch });
            openNotes.erase (it);
        };

        while (fifo.getNumReady() > 0)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            const RawEvent e = fifoData[(size_t) (n1 > 0 ? s1 : s2)];
            fifo.finishedRead (n1 + n2);

            if (e.kind == Clear)
            {
                notes.clear();
                openNotes.clear();
                continue;
            }

            const int key = e.channel * 128 + e.note;

            if (e.kind == On)
            {
                closeNote (key, e.ppq, e.note, e.channel);   // retrigger of a held note
                openNotes[key] = { e.ppq, e.velocity };
            }
            else
                closeNote (key, e.ppq, e.note, e.channel);
        }
    }

    juce::AudioProcessorValueTreeState& apvts;
    std::atomic<float> *pRecord = nullptr, *pBars = nullptr, *pLoop = nullptr, *pQuant = nullptr;
    juce::RangedAudioParameter* recordParam = nullptr;

    // audio thread -> message thread hand-off
    static constexpr int fifoSize = 8192;
    juce::AbstractFifo fifo { fifoSize };
    std::array<RawEvent, (size_t) fifoSize> fifoData {};

    // audio-thread state
    bool wasRecording = false;
    int state = Idle;
    double recStart = 0.0;
    double lastPos = 0.0;
    double freeRunPpq = 0.0;
    std::array<bool, 2048> held {};
    std::atomic<bool> done { false };
    std::atomic<int> stateAtomic { Idle };

    // message-thread state
    std::vector<RecNote> notes;
    std::map<int, OpenNote> openNotes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipCapture)
};

//==============================================================================
// UI
//==============================================================================
class ClipPianoRoll : public juce::Component
{
public:
    explicit ClipPianoRoll (ClipCapture& c) : cc (c) {}

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.fillAll (juce::Colour (0xff181818));

        const double loopLen = cc.getLoopLengthPpq();
        const auto& notes = cc.getNotes();

        for (int i = 0; i <= (int) loopLen; ++i)
        {
            const float x = b.getX() + b.getWidth() * (float) (i / loopLen);
            g.setColour (i % 4 == 0 ? juce::Colour (0xff555555) : juce::Colour (0xff2a2a2a));
            g.drawVerticalLine ((int) x, b.getY(), b.getBottom());
        }

        int lo = 127, hi = 0;
        for (const auto& n : notes)
        {
            lo = std::min (lo, n.note);
            hi = std::max (hi, n.note);
        }
        if (notes.empty()) { lo = 48; hi = 72; }
        if (hi - lo < 11)  hi = lo + 11;

        const float rowH = b.getHeight() / (float) (hi - lo + 1);

        g.setColour (juce::Colour (0xffff9b3d));
        for (const auto& n : notes)
        {
            if (n.start >= loopLen) continue;
            const float x = b.getX() + b.getWidth() * (float) (n.start / loopLen);
            const float w = std::max (2.0f, b.getWidth() * (float) (n.length / loopLen));
            const float y = b.getBottom() - (float) (n.note - lo + 1) * rowH;
            g.fillRect (x, y, std::min (w, b.getRight() - x), std::max (2.0f, rowH - 1.0f));
        }
    }

private:
    ClipCapture& cc;
};

class ClipDragPad : public juce::Component
{
public:
    explicit ClipDragPad (ClipCapture& c) : cc (c) {}

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        const bool empty = cc.getNotes().empty();
        g.setColour (empty ? juce::Colour (0xff2a2a2a) : juce::Colour (0xff2e6b3a));
        g.fillRoundedRectangle (b, 8.0f);
        g.setColour (juce::Colours::white.withAlpha (empty ? 0.4f : 0.95f));
        g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        g.drawText (empty ? "Nothing captured yet" : "Drag this MIDI clip into your DAW",
                    getLocalBounds(), juce::Justification::centred);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging || e.getDistanceFromDragStart() < 8) return;
        if (cc.getNotes().empty()) return;

        const auto file = cc.getTempExportFile();
        if (! cc.exportMidi (file)) return;

        dragging = true;
        juce::DragAndDropContainer::performExternalDragDropOfFiles (
            juce::StringArray (file.getFullPathName()), false, this,
            [safe = juce::Component::SafePointer<ClipDragPad> (this)] { if (safe != nullptr) safe->dragging = false; });
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

private:
    ClipCapture& cc;
    bool dragging = false;
};

class ClipCaptureUI : public juce::Component,
                      private juce::Timer
{
public:
    explicit ClipCaptureUI (ClipCapture& c) : cc (c), roll (c), pad (c)
    {
        recButton.setButtonText ("Record");
        clearButton.setButtonText ("Clear");
        saveButton.setButtonText ("Save .mid...");
        status.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

        recAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (cc.getApvts(), "capRecord", recButton);

        clearButton.onClick = [this] { cc.clearPattern(); repaint(); };
        saveButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Save MIDI clip",
                juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Captured Pattern.mid"),
                "*.mid");
            chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                      | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [c = &cc] (const juce::FileChooser& fc)
                                  {
                                      const auto f = fc.getResult();
                                      if (f != juce::File())
                                          c->exportMidi (f.withFileExtension ("mid"));
                                  });
        };

        for (juce::Component* comp : std::initializer_list<juce::Component*> {
                 &recButton, &clearButton, &saveButton, &status, &roll, &pad })
            addAndMakeVisible (*comp);

        startTimerHz (15);
    }

    ~ClipCaptureUI() override { stopTimer(); }

    void resized() override
    {
        auto b = getLocalBounds().reduced (6);
        auto row = b.removeFromTop (30);
        recButton.setBounds (row.removeFromLeft (100));
        row.removeFromLeft (8);
        clearButton.setBounds (row.removeFromLeft (80));
        row.removeFromLeft (8);
        saveButton.setBounds (row.removeFromLeft (120));

        status.setBounds (b.removeFromTop (24));
        pad.setBounds (b.removeFromBottom (52));
        b.removeFromBottom (6);
        roll.setBounds (b);
    }

private:
    void timerCallback() override
    {
        const int st = cc.getState();
        juce::String text;
        if (st == 1)      text = "Armed - press play, recording starts at the next bar";
        else if (st == 2) text = "Recording...";
        else if (cc.getNotes().empty()) text = "Press Record, then play";
        else              text = "Clip ready (" + juce::String ((int) cc.getNotes().size()) + " notes) - drag it into your DAW";

        if (status.getText() != text)
            status.setText (text, juce::dontSendNotification);

        roll.repaint();
        pad.repaint();
    }

    ClipCapture& cc;
    juce::ToggleButton recButton;
    juce::TextButton clearButton, saveButton;
    juce::Label status;
    ClipPianoRoll roll;
    ClipDragPad pad;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> recAtt;
};
