#include "PluginProcessor.h"
#include "PluginEditor.h"

TemperAudioProcessor::TemperAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , parameters(*this, nullptr, "Parameters",
        {
            std::make_unique<juce::AudioParameterFloat>("amount", "Amount", 0.0f, 1.0f, 0.5f),
            std::make_unique<juce::AudioParameterFloat>("sensitivity", "Sensitivity", 0.0f, 1.0f, 0.5f),
            std::make_unique<juce::AudioParameterFloat>("slope", "Slope", 0.0f, 1.0f, 0.5f),
            std::make_unique<juce::AudioParameterFloat>("q", "Bandwidth", 0.0f, 1.0f, 0.5f),
            std::make_unique<juce::AudioParameterFloat>("attack", "Attack",
                juce::NormalisableRange<float>(1.0f, 200.0f, 0.0f, 0.35f), 10.0f),
            std::make_unique<juce::AudioParameterFloat>("release", "Release",
                juce::NormalisableRange<float>(10.0f, 1000.0f, 0.0f, 0.35f), 120.0f),
            std::make_unique<juce::AudioParameterFloat>("mix", "Mix", 0.0f, 1.0f, 1.0f),
            std::make_unique<juce::AudioParameterFloat>("output", "Output", 0.0f, 2.0f, 1.0f),

            // Manual EQ nodes (graphically editable).
            std::make_unique<juce::AudioParameterFloat>("eq1freq", "EQ 1 Freq", juce::NormalisableRange<float>(40.0f, 20000.0f, 0.0f, 0.30f), 120.0f),
            std::make_unique<juce::AudioParameterFloat>("eq1gain", "EQ 1 Gain", -18.0f, 18.0f, 0.0f),
            std::make_unique<juce::AudioParameterFloat>("eq2freq", "EQ 2 Freq", juce::NormalisableRange<float>(40.0f, 20000.0f, 0.0f, 0.30f), 250.0f),
            std::make_unique<juce::AudioParameterFloat>("eq2gain", "EQ 2 Gain", -18.0f, 18.0f, 0.0f),
            std::make_unique<juce::AudioParameterFloat>("eq3freq", "EQ 3 Freq", juce::NormalisableRange<float>(40.0f, 20000.0f, 0.0f, 0.30f), 500.0f),
            std::make_unique<juce::AudioParameterFloat>("eq3gain", "EQ 3 Gain", -18.0f, 18.0f, 0.0f),
            std::make_unique<juce::AudioParameterFloat>("eq4freq", "EQ 4 Freq", juce::NormalisableRange<float>(40.0f, 20000.0f, 0.0f, 0.30f), 1000.0f),
            std::make_unique<juce::AudioParameterFloat>("eq4gain", "EQ 4 Gain", -18.0f, 18.0f, 0.0f),
            std::make_unique<juce::AudioParameterFloat>("eq5freq", "EQ 5 Freq", juce::NormalisableRange<float>(40.0f, 20000.0f, 0.0f, 0.30f), 2000.0f),
            std::make_unique<juce::AudioParameterFloat>("eq5gain", "EQ 5 Gain", -18.0f, 18.0f, 0.0f),
            std::make_unique<juce::AudioParameterFloat>("eq6freq", "EQ 6 Freq", juce::NormalisableRange<float>(40.0f, 20000.0f, 0.0f, 0.30f), 4000.0f),
            std::make_unique<juce::AudioParameterFloat>("eq6gain", "EQ 6 Gain", -18.0f, 18.0f, 0.0f),
            std::make_unique<juce::AudioParameterFloat>("eq7freq", "EQ 7 Freq", juce::NormalisableRange<float>(40.0f, 20000.0f, 0.0f, 0.30f), 8000.0f),
            std::make_unique<juce::AudioParameterFloat>("eq7gain", "EQ 7 Gain", -18.0f, 18.0f, 0.0f),
            std::make_unique<juce::AudioParameterFloat>("eq8freq", "EQ 8 Freq", juce::NormalisableRange<float>(40.0f, 20000.0f, 0.0f, 0.30f), 12000.0f),
            std::make_unique<juce::AudioParameterFloat>("eq8gain", "EQ 8 Gain", -18.0f, 18.0f, 0.0f)
        })
{
}

TemperAudioProcessor::~TemperAudioProcessor() = default;

void TemperAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    engine.prepare(sampleRate, samplesPerBlock);
}

void TemperAudioProcessor::releaseResources()
{
    engine.reset();
}

bool TemperAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo())
        return false;

    return in == out;
}

void TemperAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();

    for (int i = totalIn; i < totalOut; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    if (totalIn == 0 || totalOut == 0)
        return;

    const auto param = [&](const char* id) { return parameters.getRawParameterValue(id)->load(); };
    temper::Params p;
    p.amount      = param("amount");
    p.sensitivity = param("sensitivity");
    p.slope       = param("slope");
    p.q           = param("q");
    p.attackMs    = param("attack");
    p.releaseMs   = param("release");
    p.mix         = param("mix");
    p.output      = param("output");

    for (int i = 0; i < temper::kNumEqBands; ++i)
    {
        const juce::String f = "eq" + juce::String(i + 1) + "freq";
        const juce::String g = "eq" + juce::String(i + 1) + "gain";
        p.eqFreq[static_cast<size_t>(i)] = param(f.toRawUTF8());
        p.eqGain[static_cast<size_t>(i)] = param(g.toRawUTF8());
    }

    engine.setParams(p);

    const int numSamples = buffer.getNumSamples();
    const float* inL = buffer.getReadPointer(0);
    const float* inR = (totalIn > 1) ? buffer.getReadPointer(1) : inL;
    float* outL = buffer.getWritePointer(0);
    float* outR = (totalOut > 1) ? buffer.getWritePointer(1) : outL;

    engine.process(inL, inR, outL, outR, numSamples);
}

juce::AudioProcessorEditor* TemperAudioProcessor::createEditor()
{
    return new TemperAudioProcessorEditor(*this);
}

void TemperAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void TemperAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr)
        if (xml->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TemperAudioProcessor();
}
