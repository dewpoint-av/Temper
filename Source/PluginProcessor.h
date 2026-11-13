#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/SpectralSuppressor.h"

class TemperAudioProcessor : public juce::AudioProcessor,
                             private juce::AsyncUpdater
{
public:
    TemperAudioProcessor();
    ~TemperAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override               { return true; }

    const juce::String getName() const override   { return JucePlugin_Name; }
    bool acceptsMidi() const override             { return false; }
    bool producesMidi() const override            { return false; }
    bool isMidiEffect() const override            { return false; }
    double getTailLengthSeconds() const override  { return 0.0; }

    int getNumPrograms() override                 { return 1; }
    int getCurrentProgram() override              { return 0; }
    void setCurrentProgram(int) override          {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState parameters;
    juce::AudioProcessorValueTreeState& getState() { return parameters; }

    temper::SpectralSuppressor& getEngine() { return engine; }

    // Custom threshold: learn from what's playing.
    void startLearn() { engine.dynamics().startLearn(); }
    void stopLearn()  { engine.dynamics().stopLearn(); }
    bool isLearning() const { return engine.dynamics().isLearning(); }
    void clearCustomCurve() { engine.dynamics().clearCustomCurve(); }
    // Gate noise profile (spectral gate).
    void startNoiseLearn() { engine.gateEngine().startLearn(); }
    void stopNoiseLearn()  { engine.gateEngine().stopLearn(); }
    bool isNoiseLearning() const { return engine.gateEngine().isLearning(); }
    void clearNoiseProfile() { engine.gateEngine().clearProfile(); }
    bool hasSidechain() const;

private:
    void handleAsyncUpdate() override;            // reports a changed latency (Quality)
    temper::Params readParams() const;

    temper::SpectralSuppressor engine;
    std::atomic<int> pendingLatency { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TemperAudioProcessor)
};
