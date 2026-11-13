#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    juce::StringArray filterTypeNames()
    {
        juce::StringArray a;
        for (int t = 0; t < static_cast<int> (temper::FilterType::Count); ++t) a.add (temper::filterTypeName (t));
        return a;
    }

    juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
    {
        using APF = juce::AudioParameterFloat;
        using APC = juce::AudioParameterChoice;
        using APB = juce::AudioParameterBool;
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        layout.add (std::make_unique<APC> ("order", "Module Order",
            juce::StringArray { "Resonance > OTT > Gate", "Resonance > Gate > OTT", "OTT > Resonance > Gate",
                                "OTT > Gate > Resonance", "Gate > Resonance > OTT", "Gate > OTT > Resonance" }, 0));
        layout.add (std::make_unique<APF> ("mix", "Mix", 0.0f, 1.0f, 1.0f));
        layout.add (std::make_unique<APF> ("output", "Output", 0.0f, 2.0f, 1.0f));
        layout.add (std::make_unique<APB> ("autoGain", "Auto Gain", false));
        layout.add (std::make_unique<APC> ("listen", "Listen", juce::StringArray { "Off", "Delta", "Detector" }, 0));
        layout.add (std::make_unique<juce::AudioParameterInt> ("eqListen", "EQ Band Listen", 0, temper::kNumEqBands, 0));

        // --- Resonance: a dynamic spectral EQ with one set of global controls (DSEQ-style) ---
        {
            const juce::String k = "supp";
            layout.add (std::make_unique<APB> (k + "On", "Resonance On", true));
            layout.add (std::make_unique<APC> (k + "Quality", "Resonance Quality", juce::StringArray { "Normal", "High", "Ultra" }, 1));
            layout.add (std::make_unique<APC> (k + "Phase", "Resonance Phase", juce::StringArray { "Linear", "Natural" }, 0));
            layout.add (std::make_unique<APC> (k + "Stereo", "Resonance Stereo", juce::StringArray { "Left / Right", "Mid / Side" }, 0));
            layout.add (std::make_unique<APC> (k + "ThrMode", "Resonance Threshold Mode", juce::StringArray { "Fixed", "Adaptive", "Custom" }, 1));
            layout.add (std::make_unique<APF> (k + "Thresh", "Resonance Threshold", juce::NormalisableRange<float> (-60.0f, 30.0f, 0.1f), 6.0f));
            layout.add (std::make_unique<APF> (k + "Slope", "Resonance Slope", juce::NormalisableRange<float> (-6.0f, 6.0f, 0.1f), 0.0f));
            layout.add (std::make_unique<APB> (k + "AdaptSlope", "Resonance Adaptive Slope", false));
            layout.add (std::make_unique<APF> (k + "Select", "Resonance Selectivity", 0.0f, 1.0f, 0.5f));
            layout.add (std::make_unique<APF> (k + "Att", "Resonance Attack", juce::NormalisableRange<float> (1.0f, 200.0f, 0.0f, 0.4f), 10.0f));
            layout.add (std::make_unique<APF> (k + "Rel", "Resonance Release", juce::NormalisableRange<float> (10.0f, 1000.0f, 0.0f, 0.4f), 150.0f));
            layout.add (std::make_unique<APF> (k + "Strength", "Resonance Strength", juce::NormalisableRange<float> (0.1f, 4.0f, 0.0f, 0.5f), 1.0f));
            layout.add (std::make_unique<APF> (k + "MaxGr", "Resonance Max GR", juce::NormalisableRange<float> (0.0f, 36.0f, 0.1f), 12.0f));
            layout.add (std::make_unique<APF> (k + "Link", "Resonance Channel Link", 0.0f, 1.0f, 0.75f));
            layout.add (std::make_unique<APB> (k + "Sc", "Resonance Sidechain", false));
            layout.add (std::make_unique<APF> (k + "ScGain", "Resonance Sidechain Gain", juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f));

            // Detector pre-filters: shape what the dynamic EQs hear, not the audio.
            const float pfFreq[temper::kNumPreFilters] = { 100.0f, 300.0f, 1000.0f, 3000.0f, 8000.0f, 15000.0f };
            for (int i = 1; i <= temper::kNumPreFilters; ++i)
            {
                const juce::String s = "pf" + juce::String (i), n = "Detector Filter " + juce::String (i);
                layout.add (std::make_unique<APB> (s + "on", n + " On", false));
                layout.add (std::make_unique<APC> (s + "type", n + " Type", filterTypeNames(), 0));
                layout.add (std::make_unique<APF> (s + "freq", n + " Freq", juce::NormalisableRange<float> (20.0f, 20000.0f, 0.0f, 0.30f), pfFreq[i - 1]));
                layout.add (std::make_unique<APF> (s + "gain", n + " Sensitivity", juce::NormalisableRange<float> (-48.0f, 30.0f, 0.1f), 0.0f));
                layout.add (std::make_unique<APF> (s + "q", n + " Q", juce::NormalisableRange<float> (0.1f, 18.0f, 0.0f, 0.4f), 1.0f));
                layout.add (std::make_unique<APC> (s + "slope", n + " Slope", juce::StringArray { "12 dB/oct", "24 dB/oct", "48 dB/oct" }, 0));
            }
        }

        // --- OTT: MO-TT-style controls, multiband or spectral engine (off by default) ---------
        // Band defaults approximate Ableton's OTT preset (88.3 Hz / 2.5 kHz, +5.2 dB in).
        {
            const juce::String k = "ott";
            layout.add (std::make_unique<APB> (k + "On", "OTT On", false));
            layout.add (std::make_unique<APC> (k + "Engine", "OTT Engine", juce::StringArray { "Multiband", "Spectral" }, 0));
            layout.add (std::make_unique<APC> (k + "Style", "OTT Timing Style", juce::StringArray { "Classic", "Smooth", "Smack" }, 0));
            layout.add (std::make_unique<APC> (k + "Knee", "OTT Knee", juce::StringArray { "Soft", "Hard" }, 0));
            layout.add (std::make_unique<APC> (k + "Detect", "OTT Detection", juce::StringArray { "RMS", "Peak" }, 0));
            layout.add (std::make_unique<APF> (k + "Amount", "OTT Amount", 0.0f, 1.0f, 1.0f));
            layout.add (std::make_unique<APF> (k + "Time", "OTT Time", juce::NormalisableRange<float> (0.1f, 10.0f, 0.0f, 0.3f), 1.0f));
            layout.add (std::make_unique<APF> (k + "In", "OTT Input", juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f));
            layout.add (std::make_unique<APF> (k + "Out", "OTT Output", juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f));
            layout.add (std::make_unique<APF> (k + "LowTone", "OTT Low Tone", juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f));
            layout.add (std::make_unique<APF> (k + "HighTone", "OTT High Tone", juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f));
            layout.add (std::make_unique<APF> (k + "Link", "OTT Stereo Link", 0.0f, 2.0f, 1.0f));
            layout.add (std::make_unique<APF> (k + "Res", "OTT Spectral Resolution", 0.0f, 1.0f, 0.5f));
            layout.add (std::make_unique<APF> (k + "Look", "OTT Lookahead", juce::NormalisableRange<float> (0.0f, 10.0f, 0.01f), 0.0f));
            layout.add (std::make_unique<APB> (k + "Clip", "OTT Clip", false));
            layout.add (std::make_unique<APF> (k + "ClipLvl", "OTT Clip Level", juce::NormalisableRange<float> (-24.0f, 0.0f, 0.1f), 0.0f));
            layout.add (std::make_unique<APB> (k + "Sc", "OTT Sidechain", false));
            layout.add (std::make_unique<APC> (k + "ScMode", "OTT Sidechain Filtering", juce::StringArray { "Band", "Wide" }, 0));
            layout.add (std::make_unique<APF> (k + "X1", "OTT Low / Mid", juce::NormalisableRange<float> (20.0f, 1000.0f, 0.0f, 0.4f), 88.3f));
            layout.add (std::make_unique<APF> (k + "X2", "OTT Mid / High", juce::NormalisableRange<float> (300.0f, 16000.0f, 0.0f, 0.4f), 2500.0f));
            const char* bn[3] = { "Low", "Mid", "High" };
            const float above[3] = { -33.8f, -30.2f, -35.5f }, below[3] = { -40.8f, -41.8f, -40.8f };
            const float att[3] = { 47.8f, 22.4f, 13.5f };
            for (int b = 1; b <= 3; ++b)
            {
                const juce::String s2 = juce::String (b), n = juce::String ("OTT ") + bn[b - 1] + " ";
                layout.add (std::make_unique<APF> (k + "Above" + s2, n + "Above Threshold", juce::NormalisableRange<float> (-80.0f, 0.0f, 0.1f), above[b - 1]));
                layout.add (std::make_unique<APF> (k + "Down"  + s2, n + "Downward", juce::NormalisableRange<float> (0.0f, 1.5f, 0.001f), 1.0f));
                layout.add (std::make_unique<APF> (k + "Below" + s2, n + "Below Threshold", juce::NormalisableRange<float> (-90.0f, 0.0f, 0.1f), below[b - 1]));
                layout.add (std::make_unique<APF> (k + "Up"    + s2, n + "Upward", juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.76f));
                layout.add (std::make_unique<APF> (k + "BIn"   + s2, n + "Input", juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 5.2f));
                layout.add (std::make_unique<APF> (k + "BOut"  + s2, n + "Output", juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f));
                layout.add (std::make_unique<APF> (k + "BAmt"  + s2, n + "Amount", 0.0f, 1.0f, 1.0f));
                layout.add (std::make_unique<APF> (k + "Att"   + s2, n + "Attack", juce::NormalisableRange<float> (0.1f, 500.0f, 0.0f, 0.35f), att[b - 1]));
                layout.add (std::make_unique<APF> (k + "Rel"   + s2, n + "Release", juce::NormalisableRange<float> (1.0f, 3000.0f, 0.0f, 0.35f), 282.0f));
                layout.add (std::make_unique<APB> (k + "Solo"  + s2, n + "Solo", false));
                layout.add (std::make_unique<APB> (k + "Byp"   + s2, n + "Bypass", false));
            }
        }

        // --- Gate: band gate / expander or per-bin spectral gate (off by default) ---------------
        {
            const juce::String k = "gate";
            layout.add (std::make_unique<APB> (k + "On", "Gate On", false));
            layout.add (std::make_unique<APC> (k + "Engine", "Gate Engine", juce::StringArray { "Band", "Spectral" }, 0));
            layout.add (std::make_unique<APC> (k + "Bands", "Gate Bands", juce::StringArray { "1", "2", "3" }, 0));
            layout.add (std::make_unique<APF> (k + "X1", "Gate Low / Mid", juce::NormalisableRange<float> (20.0f, 2000.0f, 0.0f, 0.4f), 200.0f));
            layout.add (std::make_unique<APF> (k + "X2", "Gate Mid / High", juce::NormalisableRange<float> (300.0f, 16000.0f, 0.0f, 0.4f), 4000.0f));
            layout.add (std::make_unique<APF> (k + "Att", "Gate Attack", juce::NormalisableRange<float> (0.01f, 100.0f, 0.0f, 0.35f), 1.0f));
            layout.add (std::make_unique<APF> (k + "Hold", "Gate Hold", juce::NormalisableRange<float> (0.0f, 500.0f, 0.0f, 0.5f), 20.0f));
            layout.add (std::make_unique<APF> (k + "Rel", "Gate Release", juce::NormalisableRange<float> (1.0f, 2000.0f, 0.0f, 0.35f), 100.0f));
            layout.add (std::make_unique<APF> (k + "Hyst", "Gate Hysteresis", juce::NormalisableRange<float> (0.0f, 20.0f, 0.1f), 4.0f));
            layout.add (std::make_unique<APF> (k + "Look", "Gate Lookahead", juce::NormalisableRange<float> (0.0f, 10.0f, 0.01f), 0.0f));
            layout.add (std::make_unique<APB> (k + "Flip", "Gate Flip (Duck)", false));
            layout.add (std::make_unique<APB> (k + "Sc", "Gate Sidechain", false));
            layout.add (std::make_unique<APF> (k + "ScHpf", "Gate Sidechain High-Pass", juce::NormalisableRange<float> (20.0f, 2000.0f, 0.0f, 0.3f), 20.0f));
            layout.add (std::make_unique<APF> (k + "ScLpf", "Gate Sidechain Low-Pass", juce::NormalisableRange<float> (500.0f, 20000.0f, 0.0f, 0.3f), 20000.0f));
            layout.add (std::make_unique<APF> (k + "Smooth", "Gate Spectral Smoothing", 0.0f, 1.0f, 0.5f));
            layout.add (std::make_unique<APC> (k + "NoiseRef", "Gate Noise Reference", juce::StringArray { "Off", "Learned", "Auto" }, 0));
            for (int b = 1; b <= 3; ++b)
            {
                const juce::String s2 = juce::String (b), n = "Gate Band " + s2 + " ";
                layout.add (std::make_unique<APF> (k + "Thr"   + s2, n + "Threshold", juce::NormalisableRange<float> (-90.0f, 30.0f, 0.1f), -40.0f));
                layout.add (std::make_unique<APF> (k + "Range" + s2, n + "Range", juce::NormalisableRange<float> (-90.0f, 0.0f, 0.1f), -60.0f));
                layout.add (std::make_unique<APF> (k + "Ratio" + s2, n + "Ratio", juce::NormalisableRange<float> (1.0f, 50.0f, 0.0f, 0.4f), 50.0f));
            }
        }

        // --- Manual EQ: 8 graphically editable bands ---------------------------------------
        {
            const float defFreq[8] = { 120.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 12000.0f };
            for (int i = 1; i <= temper::kNumEqBands; ++i)
            {
                const juce::String s = "eq" + juce::String (i), n = "EQ " + juce::String (i);
                layout.add (std::make_unique<APB> (s + "on", n + " On", false));
                layout.add (std::make_unique<APC> (s + "type", n + " Type", filterTypeNames(), 0));
                layout.add (std::make_unique<APF> (s + "freq", n + " Freq", juce::NormalisableRange<float> (20.0f, 20000.0f, 0.0f, 0.30f), defFreq[i - 1]));
                layout.add (std::make_unique<APF> (s + "gain", n + " Gain", -18.0f, 18.0f, 0.0f));
                layout.add (std::make_unique<APF> (s + "q", n + " Q", juce::NormalisableRange<float> (0.1f, 18.0f, 0.0f, 0.4f), 1.0f));
                layout.add (std::make_unique<APC> (s + "slope", n + " Slope", juce::StringArray { "12 dB/oct", "24 dB/oct", "48 dB/oct" }, 0));
                layout.add (std::make_unique<APC> (s + "ch", n + " Channel", juce::StringArray { "Stereo", "Left", "Right", "Mid", "Side" }, 0));
            }
        }

        return layout;
    }
}

TemperAudioProcessor::TemperAudioProcessor()
    : AudioProcessor (BusesProperties()
        .withInput ("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
        .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false))
    , parameters (*this, nullptr, "Parameters", makeLayout())
{
}

TemperAudioProcessor::~TemperAudioProcessor()
{
    cancelPendingUpdate();
}

temper::Params TemperAudioProcessor::readParams() const
{
    const auto param = [this] (const juce::String& id) { return parameters.getRawParameterValue (id)->load(); };
    const auto idx = [&] (const juce::String& id) { return juce::roundToInt (param (id)); };
    const auto on = [&] (const juce::String& id) { return param (id) > 0.5f; };

    temper::Params p;
    p.order    = idx ("order");
    p.mix      = param ("mix");
    p.output   = param ("output");
    p.autoGain = on ("autoGain");
    p.listen   = idx ("listen");
    p.eqListen = idx ("eqListen");

    auto& d = p.dyn;
    d.enabled       = on ("suppOn");
    d.quality       = idx ("suppQuality");
    d.naturalPhase  = idx ("suppPhase") == 1;
    d.midSide       = idx ("suppStereo") == 1;
    d.thrMode       = idx ("suppThrMode");
    d.thresholdDb   = param ("suppThresh");
    d.slopeDbOct    = param ("suppSlope");
    d.adaptiveSlope = on ("suppAdaptSlope");
    d.selectivity   = param ("suppSelect");
    d.attackMs      = param ("suppAtt");
    d.releaseMs     = param ("suppRel");
    d.strength      = param ("suppStrength");
    d.maxGrDb       = param ("suppMaxGr");
    d.link          = param ("suppLink");
    d.sidechain     = on ("suppSc") && hasSidechain();
    d.scGainDb      = param ("suppScGain");
    for (int i = 0; i < temper::kNumPreFilters; ++i)
    {
        const juce::String s = "pf" + juce::String (i + 1);
        auto& f = d.pre[static_cast<size_t> (i)];
        f.on = on (s + "on"); f.type = idx (s + "type"); f.freq = param (s + "freq");
        f.gainDb = param (s + "gain"); f.q = param (s + "q"); f.slope = idx (s + "slope");
    }

    {
        auto& o = p.ott;
        o.enabled = on ("ottOn"); o.engine = idx ("ottEngine"); o.style = idx ("ottStyle");
        o.softKnee = idx ("ottKnee") == 0; o.rms = idx ("ottDetect") == 0;
        o.amount = param ("ottAmount"); o.time = param ("ottTime"); o.inDb = param ("ottIn"); o.outDb = param ("ottOut");
        o.lowTone = param ("ottLowTone"); o.highTone = param ("ottHighTone"); o.link = param ("ottLink");
        o.resolution = param ("ottRes"); o.lookaheadMs = param ("ottLook"); o.clip = on ("ottClip"); o.clipDb = param ("ottClipLvl");
        o.sidechain = on ("ottSc") && hasSidechain(); o.scWide = idx ("ottScMode") == 1;
        o.x1 = param ("ottX1"); o.x2 = param ("ottX2");
        for (int b = 0; b < temper::kDynBands; ++b)
        {
            const juce::String s = juce::String (b + 1);
            auto& bb = o.band[static_cast<size_t> (b)];
            bb.aboveDb = param ("ottAbove" + s); bb.down = param ("ottDown" + s);
            bb.belowDb = param ("ottBelow" + s); bb.up = param ("ottUp" + s);
            bb.inDb = param ("ottBIn" + s); bb.outDb = param ("ottBOut" + s); bb.amount = param ("ottBAmt" + s);
            bb.attMs = param ("ottAtt" + s); bb.relMs = param ("ottRel" + s);
            bb.solo = on ("ottSolo" + s); bb.bypass = on ("ottByp" + s);
        }
    }
    {
        auto& g = p.gate;
        g.enabled = on ("gateOn"); g.engine = idx ("gateEngine"); g.bands = idx ("gateBands") + 1;
        g.x1 = param ("gateX1"); g.x2 = param ("gateX2");
        g.attMs = param ("gateAtt"); g.holdMs = param ("gateHold"); g.relMs = param ("gateRel");
        g.hystDb = param ("gateHyst"); g.lookMs = param ("gateLook"); g.flip = on ("gateFlip");
        g.sidechain = on ("gateSc") && hasSidechain(); g.scHpf = param ("gateScHpf"); g.scLpf = param ("gateScLpf");
        g.smoothing = param ("gateSmooth"); g.profileMode = idx ("gateNoiseRef");
        for (int b = 0; b < temper::kDynBands; ++b)
        {
            const juce::String s = juce::String (b + 1);
            auto& bb = g.band[static_cast<size_t> (b)];
            bb.thrDb = param ("gateThr" + s); bb.rangeDb = param ("gateRange" + s); bb.ratio = param ("gateRatio" + s);
        }
    }

    for (int i = 0; i < temper::kNumEqBands; ++i)
    {
        const juce::String s = "eq" + juce::String (i + 1);
        auto& f = p.eq[static_cast<size_t> (i)];
        f.on = on (s + "on"); f.type = idx (s + "type"); f.freq = param (s + "freq"); f.gainDb = param (s + "gain");
        f.q = param (s + "q"); f.slope = idx (s + "slope"); f.channel = idx (s + "ch");
    }
    return p;
}

bool TemperAudioProcessor::hasSidechain() const
{
    if (auto* bus = getBus (true, 1))
        return bus->isEnabled() && bus->getNumberOfChannels() > 0;
    return false;
}

void TemperAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.setParams (readParams());              // Quality decides the FFT size and latency
    engine.prepare (sampleRate, samplesPerBlock);
    engine.setParams (readParams());
    setLatencySamples (engine.getLatency());
}

void TemperAudioProcessor::releaseResources()
{
    engine.reset();
}

bool TemperAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo())
        return false;
    if (in != out)
        return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void TemperAudioProcessor::handleAsyncUpdate()
{
    const int l = pendingLatency.exchange (-1);
    if (l >= 0 && l != getLatencySamples())
        setLatencySamples (l);
}

void TemperAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    auto main = getBusBuffer (buffer, true, 0);
    const int numSamples = buffer.getNumSamples();
    const int mainCh = main.getNumChannels();
    if (mainCh == 0)
        return;

    engine.setParams (readParams());
    if (engine.getLatency() != getLatencySamples())
    {
        pendingLatency.store (engine.getLatency());
        triggerAsyncUpdate();
    }

    const float* scL = nullptr;
    const float* scR = nullptr;
    if (hasSidechain())
    {
        auto sc = getBusBuffer (buffer, true, 1);
        if (sc.getNumChannels() > 0)
        {
            scL = sc.getReadPointer (0);
            scR = sc.getNumChannels() > 1 ? sc.getReadPointer (1) : scL;
        }
    }

    float* outL = main.getWritePointer (0);
    float* outR = mainCh > 1 ? main.getWritePointer (1) : outL;
    // In place is fine: each sample is read before it is written.
    engine.process (outL, outR, scL, scR, outL, outR, numSamples);
}

juce::AudioProcessorEditor* TemperAudioProcessor::createEditor()
{
    return new TemperAudioProcessorEditor (*this);
}

void TemperAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    // The learned custom threshold curve isn't a parameter: kept as text with the state.
    if (engine.dynamics().hasCustomCurve())
    {
        float c[temper::kCurvePoints];
        engine.dynamics().getCustomCurve (c);
        juce::StringArray v;
        for (float x : c) v.add (juce::String (x, 2));
        state.setProperty ("customCurve", v.joinIntoString (","), nullptr);
    }
    else
        state.removeProperty ("customCurve", nullptr);
    if (engine.gateEngine().hasProfile())
    {
        float c[temper::kCurvePoints];
        engine.gateEngine().getProfile (c);
        juce::StringArray v;
        for (float x : c) v.add (juce::String (x, 2));
        state.setProperty ("gateProfile", v.joinIntoString (","), nullptr);
    }
    else
        state.removeProperty ("gateProfile", nullptr);
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void TemperAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;

    // Sessions from before 0.4 had no EQ on/off: a band with gain was on.
    juce::StringArray savedIds;
    for (auto* c : xml->getChildIterator())
        savedIds.add (c->getStringAttribute ("id"));

    parameters.replaceState (juce::ValueTree::fromXml (*xml));

    for (int i = 1; i <= temper::kNumEqBands; ++i)
    {
        const juce::String s = "eq" + juce::String (i);
        if (! savedIds.contains (s + "on") && savedIds.contains (s + "gain"))
            if (std::fabs (parameters.getRawParameterValue (s + "gain")->load()) > 0.01f)
                if (auto* p = parameters.getParameter (s + "on"))
                    p->setValueNotifyingHost (1.0f);
    }

    const juce::String curve = parameters.state.getProperty ("customCurve").toString();
    if (curve.isNotEmpty())
    {
        juce::StringArray v;
        v.addTokens (curve, ",", {});
        if (v.size() == temper::kCurvePoints)
        {
            float c[temper::kCurvePoints];
            for (int i = 0; i < temper::kCurvePoints; ++i) c[i] = v[i].getFloatValue();
            engine.dynamics().setCustomCurve (c);
        }
    }
    const juce::String prof = parameters.state.getProperty ("gateProfile").toString();
    if (prof.isNotEmpty())
    {
        juce::StringArray v;
        v.addTokens (prof, ",", {});
        if (v.size() == temper::kCurvePoints)
        {
            float c[temper::kCurvePoints];
            for (int i = 0; i < temper::kCurvePoints; ++i) c[i] = v[i].getFloatValue();
            engine.gateEngine().setProfile (c);
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TemperAudioProcessor();
}
