#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "HoloLogo.h"

#include <cmath>
#include <functional>

namespace
{
    juce::String pctText  (float v) { return juce::String(v * 100.0f, 0) + " %"; }
    juce::String gainText (float v) { return juce::String(v, 2) + "x"; }
    juce::String msText   (float v) { return juce::String(v, 0) + " ms"; }

    // Magnitude response (dB) of an RBJ peaking filter at `freq` Hz — the same
    // biquad the DSP uses for its manual EQ nodes (Q = 1.0), evaluated directly
    // so the displayed curve matches the audio exactly.
    float peakingDb (float freq, float center, float q, float gainDb, float fs)
    {
        const float A     = std::pow (10.0f, gainDb / 40.0f);
        const float w0    = 2.0f * 3.14159265358979323846f * center / fs;
        const float alpha = std::sin (w0) / (2.0f * q);
        const float cw    = std::cos (w0);
        const float a0    = 1.0f + alpha / A;
        const float b0    = (1.0f + alpha * A) / a0;
        const float b1    = -2.0f * cw / a0;
        const float b2    = (1.0f - alpha * A) / a0;
        const float a1    = -2.0f * cw / a0;
        const float a2    = (1.0f - alpha / A) / a0;

        const float w  = 2.0f * 3.14159265358979323846f * freq / fs;
        const float c1 = std::cos (w),        s1 = std::sin (w);
        const float c2 = std::cos (2.0f * w), s2 = std::sin (2.0f * w);

        const float numRe = b0 + b1 * c1 + b2 * c2;
        const float numIm = -(b1 * s1 + b2 * s2);
        const float denRe = 1.0f + a1 * c1 + a2 * c2;
        const float denIm = -(a1 * s1 + a2 * s2);

        const float mag = std::sqrt (numRe * numRe + numIm * numIm)
                        / std::sqrt (denRe * denRe + denIm * denIm);
        return 20.0f * std::log10 (juce::jmax (mag, 1.0e-6f));
    }

    // ---- a labelled rotary knob bound to a parameter ------------------------
    class Knob : public juce::Component,
                 private juce::AudioProcessorValueTreeState::Listener,
                 private juce::AsyncUpdater
    {
    public:
        Knob(juce::AudioProcessorValueTreeState& st, const juce::String& paramId,
             const juce::String& name, std::function<juce::String(float)> textFnIn)
            : stateRef(st), param(paramId), textFn(textFnIn)
        {
            slider.setSliderStyle(juce::Slider::Rotary);
            slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            slider.setRotaryParameters({ juce::MathConstants<float>::pi * 1.25f,
                                         juce::MathConstants<float>::pi * 2.75f, true });
            addAndMakeVisible(slider);

            nameLabel.setText(name, juce::dontSendNotification);
            nameLabel.setFont(juce::FontOptions(11.0f, juce::Font::plain));
            nameLabel.setColour(juce::Label::textColourId, TemperLookAndFeel::muted);
            nameLabel.setJustificationType(juce::Justification::centred);
            addAndMakeVisible(nameLabel);

            valueLabel.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            valueLabel.setColour(juce::Label::textColourId, TemperLookAndFeel::text);
            valueLabel.setJustificationType(juce::Justification::centred);
            addAndMakeVisible(valueLabel);

            attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(st, paramId, slider);
            stateRef.addParameterListener(param, this);
            refreshLabel();
        }

        ~Knob() override { stateRef.removeParameterListener(param, this); }

        void parameterChanged(const juce::String&, float) override { triggerAsyncUpdate(); }
        void handleAsyncUpdate() override { refreshLabel(); }

        void resized() override
        {
            auto b = getLocalBounds();
            nameLabel.setBounds(b.removeFromTop(18));
            valueLabel.setBounds(b.removeFromBottom(18));
            slider.setBounds(b);
        }

    private:
        void refreshLabel()
        {
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(stateRef.getParameter(param)))
                if (textFn)
                    valueLabel.setText(textFn(p->getNormalisableRange().convertFrom0to1(p->getValue())),
                                       juce::dontSendNotification);
        }

        juce::AudioProcessorValueTreeState& stateRef;
        juce::String param;
        std::function<juce::String(float)> textFn;
        juce::Slider slider;
        juce::Label nameLabel, valueLabel;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    // ---- a titled panel that draws its own background ----------------------
    class Panel : public juce::Component
    {
    public:
        explicit Panel(const juce::String& title)
        {
            titleLabel.setText(title, juce::dontSendNotification);
            titleLabel.setFont(juce::FontOptions(13.0f, juce::Font::bold));
            titleLabel.setColour(juce::Label::textColourId, TemperLookAndFeel::accent);
            titleLabel.setJustificationType(juce::Justification::centred);
            addAndMakeVisible(titleLabel);
        }

        void paint(juce::Graphics& g) override { TemperLookAndFeel::drawPanelBackground(g, getLocalBounds()); }

        void resized() override { titleLabel.setBounds(0, 8, getWidth(), 18); }

        juce::Label titleLabel;
    };

    // ---- the DSEQ3-style spectrum + draggable EQ curve ---------------------
    class EqCurveEditor : public juce::Component,
                          private juce::Timer
    {
    public:
        EqCurveEditor(temper::SpectralSuppressor& e, juce::AudioProcessorValueTreeState& st)
            : engine(e), state(st)
        {
            fft = std::make_unique<juce::dsp::FFT>(kOrder);
            window.assign(kFftSize, 0.0f);
            for (int i = 0; i < kFftSize; ++i)
                window[static_cast<size_t>(i)] = 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979323846f * static_cast<float>(i) / static_cast<float>(kFftSize - 1)));
            fftBuf.assign(kFftSize * 2, 0.0f);
            spectrum.assign(kNumBins, 0.0f);
            startTimerHz(30);
        }

        ~EqCurveEditor() override { stopTimer(); }

        void timerCallback() override { updateSpectrum(); repaint(); }

        void paint(juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced(10.0f);
            auto plot = r.withLeft(r.getX() + 34.0f);

            g.setColour(TemperLookAndFeel::track);
            g.fillRoundedRectangle(r, 8.0f);
            g.setColour(TemperLookAndFeel::border);
            g.drawRoundedRectangle(r.reduced(0.5f), 8.0f, 1.5f);

            // 0 dB line + gain scale labels.
            const float zeroY = gainToY(0.0f, plot);
            g.setColour(TemperLookAndFeel::muted.withAlpha(0.45f));
            g.drawLine(plot.getX(), zeroY, plot.getRight(), zeroY, 1.0f);
            g.setFont(juce::FontOptions(9.0f, juce::Font::plain));
            g.setColour(TemperLookAndFeel::muted);
            g.drawText("+18", juce::roundToInt(r.getX()), juce::roundToInt(plot.getY() - 5), 30, 12, juce::Justification::right, false);
            g.drawText(" 0",  juce::roundToInt(r.getX()), juce::roundToInt(zeroY - 6), 30, 12, juce::Justification::right, false);
            g.drawText("-18", juce::roundToInt(r.getX()), juce::roundToInt(plot.getBottom() - 5), 30, 12, juce::Justification::right, false);

            // Legend (top-right of the plot).
            {
                g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
                const int ly = juce::roundToInt(plot.getY() + 4.0f);
                const int lw = 110;
                const int lx = juce::roundToInt(plot.getRight() - static_cast<float>(lw) - 4.0f);

                g.setColour(juce::Colour(0xffff5a5a));
                g.drawLine(static_cast<float>(lx), ly + 5.0f, static_cast<float>(lx) + 10.0f, ly + 5.0f, 2.0f);
                g.drawText("SUPPRESS", lx + 14, ly - 2, lw - 14, 12, juce::Justification::left, false);

                g.setColour(TemperLookAndFeel::accent2);
                g.drawLine(static_cast<float>(lx), ly + 17.0f, static_cast<float>(lx) + 10.0f, ly + 17.0f, 2.0f);
                g.drawText("EQ", lx + 14, ly + 10, lw - 14, 12, juce::Justification::left, false);
            }

            // Spectrum.
            juce::Path spec;
            for (int i = 0; i < kNumBins; ++i)
            {
                const float freq = 40.0f * std::pow(500.0f, static_cast<float>(i) / (kNumBins - 1));
                const float x = freqToX(freq, plot);
                const float y = plot.getBottom() - spectrum[static_cast<size_t>(i)] * plot.getHeight();
                if (i == 0) spec.startNewSubPath(x, y); else spec.lineTo(x, y);
            }
            spec.lineTo(plot.getRight(), plot.getBottom());
            spec.lineTo(plot.getX(), plot.getBottom());
            spec.closeSubPath();
            g.setColour(TemperLookAndFeel::accent.withAlpha(0.16f));
            g.fillPath(spec);
            g.setColour(TemperLookAndFeel::accent.withAlpha(0.65f));
            g.strokePath(spec, juce::PathStrokeType(1.2f));

            // --- dynamic suppression overlay (what's being cut, and by how much)
            {
                juce::Path suppress;
                bool started = false;
                for (int i = 0; i < temper::kNumBands; ++i)
                {
                    const float freq = engine.getBandFrequency(i);
                    const float red  = engine.getReductionDb(i);   // negative = cutting
                    const float x = freqToX(freq, plot);
                    const float y = gainToY(red, plot);
                    if (!started) { suppress.startNewSubPath(x, y); started = true; }
                    else          suppress.lineTo(x, y);
                }

                // Close the region down to the 0 dB line so the fill reads as
                // "energy being removed below zero".
                juce::Path fill = suppress;
                const float xFirst = freqToX(engine.getBandFrequency(0), plot);
                const float xLast  = freqToX(engine.getBandFrequency(temper::kNumBands - 1), plot);
                fill.lineTo(xLast,  zeroY);
                fill.lineTo(xFirst, zeroY);
                fill.closeSubPath();

                g.setColour(juce::Colour(0xffff5a5a).withAlpha(0.28f));
                g.fillPath(fill);
                g.setColour(juce::Colour(0xffff5a5a).withAlpha(0.95f));
                g.strokePath(suppress, juce::PathStrokeType(1.4f));
            }

            // --- combined manual-EQ response curve -------------------------
            {
                juce::Path eqCurve;
                for (int i = 0; i < kNumBins; ++i)
                {
                    const float freq = 40.0f * std::pow(500.0f, static_cast<float>(i) / (kNumBins - 1));
                    const float x = freqToX(freq, plot);
                    const float y = gainToY(eqResponseDb(freq), plot);
                    if (i == 0) eqCurve.startNewSubPath(x, y); else eqCurve.lineTo(x, y);
                }
                g.setColour(TemperLookAndFeel::accent2.withAlpha(0.95f));
                g.strokePath(eqCurve, juce::PathStrokeType(1.6f));
            }

            // EQ nodes.
            for (int i = 0; i < temper::kNumEqBands; ++i)
            {
                const float freq = getEqFreq(i);
                const float gain = getEqGain(i);
                const bool active = std::fabs(gain) > 0.01f;
                const float x = freqToX(freq, plot);
                const float y = gainToY(gain, plot);
                const float sz = active ? 6.0f : 3.5f;
                g.setColour(active ? TemperLookAndFeel::accent : TemperLookAndFeel::muted.withAlpha(0.5f));
                g.fillEllipse(x - sz, y - sz, sz * 2.0f, sz * 2.0f);
                if (active)
                {
                    g.setColour(TemperLookAndFeel::text.withAlpha(0.8f));
                    g.drawEllipse(x - sz - 2.0f, y - sz - 2.0f, (sz + 2.0f) * 2.0f, (sz + 2.0f) * 2.0f, 1.0f);
                }
            }
        }

        // ---- interaction ---------------------------------------------------
        void mouseDown(const juce::MouseEvent& e) override
        {
            auto plot = plotArea();
            dragged = nearestNode(e.position.x, e.position.y, 16.0f);

            if (dragged < 0)
            {
                dragged = nearestInactiveNode();
                if (dragged >= 0)
                    setNode(dragged, xToFreq(e.position.x, plot), yToGain(e.position.y, plot));
            }
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (dragged < 0) return;
            auto plot = plotArea();
            setNode(dragged, xToFreq(e.position.x, plot), yToGain(e.position.y, plot));
        }

        void mouseUp(const juce::MouseEvent&) override { dragged = -1; }

        void mouseDoubleClick(const juce::MouseEvent& e) override
        {
            const int n = nearestNode(e.position.x, e.position.y, 14.0f);
            if (n >= 0)
                setParam(nodeGainId(n), 0.0f);   // deactivate
        }

    private:
        juce::Rectangle<float> plotArea() const
        {
            auto r = getLocalBounds().toFloat().reduced(10.0f);
            return r.withLeft(r.getX() + 34.0f);
        }

        float freqToX(float freq, juce::Rectangle<float> plot) const
        {
            const float lo = std::log(40.0f), hi = std::log(20000.0f);
            const float t = (std::log(juce::jlimit(40.0f, 20000.0f, freq)) - lo) / (hi - lo);
            return plot.getX() + t * plot.getWidth();
        }

        float xToFreq(float x, juce::Rectangle<float> plot) const
        {
            const float lo = std::log(40.0f), hi = std::log(20000.0f);
            const float t = juce::jlimit(0.0f, 1.0f, (x - plot.getX()) / plot.getWidth());
            return std::exp(lo + t * (hi - lo));
        }

        float gainToY(float gain, juce::Rectangle<float> plot) const
        {
            const float t = (juce::jlimit(-18.0f, 18.0f, gain) + 18.0f) / 36.0f;
            return plot.getBottom() - t * plot.getHeight();
        }

        float yToGain(float y, juce::Rectangle<float> plot) const
        {
            const float t = juce::jlimit(0.0f, 1.0f, (plot.getBottom() - y) / plot.getHeight());
            return t * 36.0f - 18.0f;
        }

        // Combined magnitude response (dB) of all active manual EQ nodes.
        float eqResponseDb(float freq) const
        {
            const float fs = static_cast<float>(engine.getSampleRate());
            float db = 0.0f;
            for (int i = 0; i < temper::kNumEqBands; ++i)
            {
                const float gain = getEqGain(i);
                if (std::fabs(gain) < 0.01f)
                    continue;
                db += peakingDb(freq, getEqFreq(i), 1.0f, gain, fs);
            }
            return db;
        }

        juce::String nodeFreqId(int i) const { return "eq" + juce::String(i + 1) + "freq"; }
        juce::String nodeGainId(int i) const { return "eq" + juce::String(i + 1) + "gain"; }

        float getEqFreq(int i) const { return state.getRawParameterValue(nodeFreqId(i))->load(); }
        float getEqGain(int i) const { return state.getRawParameterValue(nodeGainId(i))->load(); }

        void setNode(int i, float freq, float gain)
        {
            setParam(nodeFreqId(i), freq);
            setParam(nodeGainId(i), gain);
        }

        void setParam(const juce::String& id, float raw)
        {
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(state.getParameter(id)))
                p->setValueNotifyingHost(p->getNormalisableRange().convertTo0to1(raw));
        }

        int nearestNode(float x, float y, float thresh) const
        {
            int best = -1;
            float bestD = thresh;
            auto plot = plotArea();
            for (int i = 0; i < temper::kNumEqBands; ++i)
            {
                if (std::fabs(getEqGain(i)) < 0.01f) continue;
                const float nx = freqToX(getEqFreq(i), plot);
                const float ny = gainToY(getEqGain(i), plot);
                const float d = std::hypot(nx - x, ny - y);
                if (d < bestD) { bestD = d; best = i; }
            }
            return best;
        }

        int nearestInactiveNode() const
        {
            for (int i = 0; i < temper::kNumEqBands; ++i)
                if (std::fabs(getEqGain(i)) < 0.01f)
                    return i;
            return -1;
        }

        void updateSpectrum()
        {
            const auto& audio = engine.vizInput();
            const int n = static_cast<int>(audio.size());
            const int wp = engine.vizWritePos();
            std::fill(fftBuf.begin(), fftBuf.end(), 0.0f);
            for (int i = 0; i < kFftSize && i < n; ++i)
            {
                int idx = wp - kFftSize + i;
                idx = ((idx % n) + n) % n;
                fftBuf[static_cast<size_t>(i)] = audio[static_cast<size_t>(idx)] * window[static_cast<size_t>(i)];
            }
            fft->performRealOnlyForwardTransform(fftBuf.data(), true);

            const float norm = 1.0f / (kFftSize * 0.25f);
            for (int b = 0; b < kNumBins; ++b)
            {
                const float freq = 40.0f * std::pow(500.0f, static_cast<float>(b) / (kNumBins - 1));
                const int bin = static_cast<int>(std::round(freq * kFftSize / 48000.0f));
                if (bin < 1 || bin * 2 + 1 >= kFftSize) { spectrum[static_cast<size_t>(b)] = 0.0f; continue; }
                const float re = fftBuf[static_cast<size_t>(bin * 2)];
                const float im = fftBuf[static_cast<size_t>(bin * 2 + 1)];
                spectrum[static_cast<size_t>(b)] = juce::jlimit(0.0f, 1.0f, std::sqrt(re * re + im * im) * norm);
            }
        }

        temper::SpectralSuppressor& engine;
        juce::AudioProcessorValueTreeState& state;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, fftBuf, spectrum;
        int dragged = -1;
        static constexpr int kOrder   = 10;
        static constexpr int kFftSize = 1 << kOrder;   // 1024
        static constexpr int kNumBins = 96;
    };
}

// ===========================================================================
class TemperAudioProcessorEditor::Impl
{
public:
    explicit Impl(TemperAudioProcessor& p) : processor(p), state(p.getState())
    {
        build();
    }

    void attachTo(juce::Component& root)
    {
        root.addAndMakeVisible(*title);
        root.addAndMakeVisible(subtitle);
        root.addAndMakeVisible(brand);
        root.addAndMakeVisible(footer);
        root.addAndMakeVisible(*eqPanel);
        root.addAndMakeVisible(*curve);
        root.addAndMakeVisible(*controlsPanel);
        for (auto& k : knobs)
            controlsPanel->addAndMakeVisible(*k);
    }

    void resized(juce::Rectangle<int> bounds)
    {
        auto b = bounds.reduced(16);

        footer.setBounds(b.removeFromBottom(18));

        auto header = b.removeFromTop(96);
        brand.setBounds(header.removeFromTop(18));
        title->setBounds(header.removeFromTop(44));
        subtitle.setBounds(header.removeFromTop(22));

        // EQ panel (spectrum + suppression + EQ curve).
        eqPanel->setBounds(b.removeFromTop(330));
        auto eqb = eqPanel->getLocalBounds().reduced(10);
        eqb.removeFromTop(26);
        curve->setBounds(eqb);

        // Controls panel with 8 knobs.
        controlsPanel->setBounds(b);

        const int cols = 4, rows = 2, gap = 12;
        auto cb = controlsPanel->getLocalBounds().reduced(14);
        cb.removeFromTop(26);
        const int cw = (cb.getWidth()  - gap * (cols - 1)) / cols;
        const int ch = (cb.getHeight() - gap * (rows - 1)) / rows;
        int idx = 0;
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols && idx < static_cast<int>(knobs.size()); ++c, ++idx)
                knobs[static_cast<size_t>(idx)]->setBounds(cb.getX() + c * (cw + gap),
                                                           cb.getY() + r * (ch + gap),
                                                           cw, ch);
    }

private:
    TemperAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& state;

    std::unique_ptr<HoloTitle> title;
    juce::Label subtitle, brand, footer;

    std::unique_ptr<Panel> eqPanel;
    std::unique_ptr<EqCurveEditor> curve;
    std::unique_ptr<Panel> controlsPanel;
    std::vector<std::unique_ptr<Knob>> knobs;

    void build()
    {
        title = std::make_unique<HoloTitle>("TEMPER", 40.0f, true);

        subtitle.setText("RESONANCE SUPPRESSOR", juce::dontSendNotification);
        subtitle.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        subtitle.setColour(juce::Label::textColourId, TemperLookAndFeel::muted);
        subtitle.setJustificationType(juce::Justification::centred);

        brand.setText("DEWPOINT AV", juce::dontSendNotification);
        brand.setFont(monumentFont(13.0f, false));
        brand.setColour(juce::Label::textColourId, TemperLookAndFeel::accent);
        brand.setJustificationType(juce::Justification::centred);

        footer.setText(juce::String("DEWPOINT AV  -  v") + juce::String(JucePlugin_VersionString), juce::dontSendNotification);
        footer.setFont(monumentFont(11.0f, false));
        footer.setColour(juce::Label::textColourId, TemperLookAndFeel::muted);
        footer.setJustificationType(juce::Justification::centred);

        eqPanel = std::make_unique<Panel>("SPECTRUM / EQ  -  click to add, drag to move, double-click to remove");
        curve = std::make_unique<EqCurveEditor>(processor.getEngine(), state);
        controlsPanel = std::make_unique<Panel>("DYNAMICS");

        knobs.push_back(std::make_unique<Knob>(state, "amount",      "AMOUNT",      pctText));
        knobs.push_back(std::make_unique<Knob>(state, "sensitivity", "SENSITIVITY", pctText));
        knobs.push_back(std::make_unique<Knob>(state, "slope",       "SLOPE",       pctText));
        knobs.push_back(std::make_unique<Knob>(state, "q",           "BANDWIDTH",   pctText));
        knobs.push_back(std::make_unique<Knob>(state, "attack",      "ATTACK",      msText));
        knobs.push_back(std::make_unique<Knob>(state, "release",     "RELEASE",     msText));
        knobs.push_back(std::make_unique<Knob>(state, "mix",         "MIX",         pctText));
        knobs.push_back(std::make_unique<Knob>(state, "output",      "OUTPUT",      gainText));
    }
};

// ===========================================================================
TemperAudioProcessorEditor::TemperAudioProcessorEditor(TemperAudioProcessor& p)
    : AudioProcessorEditor(&p)
{
    setLookAndFeel(&lookAndFeel);

    impl = std::make_unique<Impl>(p);
    impl->attachTo(*this);

    setResizable(true, true);
    setResizeLimits(860, 600, 1400, 1000);
    setSize(960, 700);
}

TemperAudioProcessorEditor::~TemperAudioProcessorEditor()
{
    setLookAndFeel(nullptr);
}

void TemperAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(TemperLookAndFeel::background);
}

void TemperAudioProcessorEditor::resized()
{
    impl->resized(getLocalBounds());
}
