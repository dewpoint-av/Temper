#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // Module key strings used as parameter-id prefixes.
    const char* kModKey[3] = { "supp", "ott", "gate" };

    juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        layout.add (std::make_unique<juce::AudioParameterChoice> ("order", "Module Order",
            juce::StringArray {
                "Resonance > OTT > Gate",
                "Resonance > Gate > OTT",
                "OTT > Resonance > Gate",
                "OTT > Gate > Resonance",
                "Gate > Resonance > OTT",
                "Gate > OTT > Resonance"
            }, 0));

        layout.add (std::make_unique<juce::AudioParameterFloat> ("mix", "Mix", 0.0f, 1.0f, 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> ("output", "Output", 0.0f, 2.0f, 1.0f));
        layout.add (std::make_unique<juce::AudioParameterBool> ("autoGain", "Auto Gain", false));

        // --- Resonance suppressor module -------------------------------------
        {
            const juce::String k = "supp";
            layout.add (std::make_unique<juce::AudioParameterBool> (k + "On", "Resonance On", true));
            layout.add (std::make_unique<juce::AudioParameterInt>  (k + "Bands", "Resonance Bands", 1, 5, 3));
            for (int i = 1; i <= 4; ++i)
                layout.add (std::make_unique<juce::AudioParameterFloat> (k + "X" + juce::String (i), "Resonance X-Over " + juce::String (i),
                    juce::NormalisableRange<float> (20.0f, 20000.0f, 0.0f, 0.30f), 200.0f * i * i));
            layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Att", "Resonance Attack",
                juce::NormalisableRange<float> (1.0f, 200.0f, 0.0f, 0.35f), 10.0f));
            layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Rel", "Resonance Release",
                juce::NormalisableRange<float> (10.0f, 1000.0f, 0.0f, 0.35f), 120.0f));
            layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Slope", "Resonance Slope", 0.0f, 1.0f, 0.5f));
            for (int b = 1; b <= temper::kMaxBands; ++b)
            {
                const juce::String s = juce::String (b);
                layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Amt"  + s, "Resonance Amount "      + s, 0.0f, 1.0f, 0.5f));
                layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Sens" + s, "Resonance Sensitivity " + s, 0.0f, 1.0f, 0.5f));
                layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Q"    + s, "Resonance Bandwidth "   + s, 0.0f, 1.0f, 0.5f));
            }
        }

        // --- Spectral OTT module (off by default) ---------------------------
        {
            const juce::String k = "ott";
            layout.add (std::make_unique<juce::AudioParameterBool> (k + "On", "OTT On", false));
            layout.add (std::make_unique<juce::AudioParameterInt>  (k + "Bands", "OTT Bands", 1, 5, 3));
            for (int i = 1; i <= 4; ++i)
                layout.add (std::make_unique<juce::AudioParameterFloat> (k + "X" + juce::String (i), "OTT X-Over " + juce::String (i),
                    juce::NormalisableRange<float> (20.0f, 20000.0f, 0.0f, 0.30f), 200.0f * i * i));
            layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Att", "OTT Attack",
                juce::NormalisableRange<float> (0.1f, 200.0f, 0.0f, 0.35f), 5.0f));
            layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Rel", "OTT Release",
                juce::NormalisableRange<float> (10.0f, 1000.0f, 0.0f, 0.35f), 80.0f));
            for (int b = 1; b <= temper::kMaxBands; ++b)
            {
                const juce::String s = juce::String (b);
                layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Thr" + s, "OTT Threshold " + s, -60.0f, 0.0f, -30.0f));
                layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Up"  + s, "OTT Upward "   + s, 0.0f, 1.0f, 0.6f));
                layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Dn"  + s, "OTT Downward " + s, 0.0f, 1.0f, 0.6f));
            }
        }

        // --- Spectral gate module (off by default) --------------------------
        {
            const juce::String k = "gate";
            layout.add (std::make_unique<juce::AudioParameterBool> (k + "On", "Gate On", false));
            layout.add (std::make_unique<juce::AudioParameterInt>  (k + "Bands", "Gate Bands", 1, 5, 3));
            for (int i = 1; i <= 4; ++i)
                layout.add (std::make_unique<juce::AudioParameterFloat> (k + "X" + juce::String (i), "Gate X-Over " + juce::String (i),
                    juce::NormalisableRange<float> (20.0f, 20000.0f, 0.0f, 0.30f), 200.0f * i * i));
            layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Att", "Gate Attack",
                juce::NormalisableRange<float> (0.1f, 200.0f, 0.0f, 0.35f), 2.0f));
            layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Rel", "Gate Release",
                juce::NormalisableRange<float> (10.0f, 1000.0f, 0.0f, 0.35f), 60.0f));
            for (int b = 1; b <= temper::kMaxBands; ++b)
            {
                const juce::String s = juce::String (b);
                layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Thr"  + s, "Gate Threshold " + s, -80.0f, 0.0f, -40.0f));
                layout.add (std::make_unique<juce::AudioParameterFloat> (k + "Range" + s, "Gate Range "    + s, -80.0f, 0.0f, -60.0f));
            }
        }

        // --- Manual EQ nodes (graphically editable) --------------------------
        {
            const float defFreq[8] = { 120.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 12000.0f };
            for (int i = 1; i <= temper::kNumEqBands; ++i)
            {
                const juce::String s = juce::String (i);
                layout.add (std::make_unique<juce::AudioParameterFloat> ("eq" + s + "freq", "EQ " + s + " Freq",
                    juce::NormalisableRange<float> (40.0f, 20000.0f, 0.0f, 0.30f), defFreq[i - 1]));
                layout.add (std::make_unique<juce::AudioParameterFloat> ("eq" + s + "gain", "EQ " + s + " Gain", -18.0f, 18.0f, 0.0f));
            }
        }

        return layout;
    }
}

TemperAudioProcessor::TemperAudioProcessor()
    : AudioProcessor (BusesProperties()
        .withInput ("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    , parameters (*this, nullptr, "Parameters", makeLayout())
{
}

TemperAudioProcessor::~TemperAudioProcessor() = default;

void TemperAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
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
    return in == out;
}

void TemperAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();

    for (int i = totalIn; i < totalOut; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    if (totalIn == 0 || totalOut == 0)
        return;

    const auto param = [&] (const juce::String& id)
    {
        return parameters.getRawParameterValue (id)->load();
    };

    temper::Params p;
    p.order  = juce::roundToInt (param ("order"));
    p.mix    = param ("mix");
    p.output = param ("output");
    p.autoGain = param ("autoGain") > 0.5f;

    for (int m = 0; m < temper::kNumModules; ++m)
    {
        const juce::String k = kModKey[m];
        auto& mp = p.mod[static_cast<size_t> (m)];
        mp.enabled = param (k + "On") > 0.5f;
        mp.bands   = juce::roundToInt (param (k + "Bands"));
        for (int i = 0; i < temper::kMaxCrossovers; ++i)
            mp.xover[static_cast<size_t> (i)] = param (k + "X" + juce::String (i + 1));

        if (m == 0)
        {
            mp.suppAttackMs  = param (k + "Att");
            mp.suppReleaseMs = param (k + "Rel");
            mp.suppSlope     = param (k + "Slope");
            for (int b = 0; b < temper::kMaxBands; ++b)
            {
                const juce::String s = juce::String (b + 1);
                mp.suppAmount[static_cast<size_t> (b)]      = param (k + "Amt"  + s);
                mp.suppSensitivity[static_cast<size_t> (b)] = param (k + "Sens" + s);
                mp.suppQ[static_cast<size_t> (b)]           = param (k + "Q"    + s);
            }
        }
        else if (m == 1)
        {
            mp.ottAttackMs  = param (k + "Att");
            mp.ottReleaseMs = param (k + "Rel");
            for (int b = 0; b < temper::kMaxBands; ++b)
            {
                const juce::String s = juce::String (b + 1);
                mp.ottThresholdDb[static_cast<size_t> (b)] = param (k + "Thr" + s);
                mp.ottUp[static_cast<size_t> (b)]          = param (k + "Up"  + s);
                mp.ottDown[static_cast<size_t> (b)]        = param (k + "Dn"  + s);
            }
        }
        else
        {
            mp.gateAttackMs  = param (k + "Att");
            mp.gateReleaseMs = param (k + "Rel");
            for (int b = 0; b < temper::kMaxBands; ++b)
            {
                const juce::String s = juce::String (b + 1);
                mp.gateThresholdDb[static_cast<size_t> (b)] = param (k + "Thr"   + s);
                mp.gateRangeDb[static_cast<size_t> (b)]     = param (k + "Range" + s);
            }
        }
    }

    for (int i = 0; i < temper::kNumEqBands; ++i)
    {
        const juce::String s = juce::String (i + 1);
        p.eqFreq[static_cast<size_t> (i)] = param ("eq" + s + "freq");
        p.eqGain[static_cast<size_t> (i)] = param ("eq" + s + "gain");
    }

    engine.setParams (p);

    const int numSamples = buffer.getNumSamples();
    const float* inL = buffer.getReadPointer (0);
    const float* inR = (totalIn > 1) ? buffer.getReadPointer (1) : inL;
    float* outL = buffer.getWritePointer (0);
    float* outR = (totalOut > 1) ? buffer.getWritePointer (1) : outL;

    engine.process (inL, inR, outL, outR, numSamples);
}

juce::AudioProcessorEditor* TemperAudioProcessor::createEditor()
{
    return new TemperAudioProcessorEditor (*this);
}

void TemperAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void TemperAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr)
        if (xml->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TemperAudioProcessor();
}
