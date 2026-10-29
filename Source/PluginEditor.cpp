#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "HoloLogo.h"

#include <cmath>
#include <functional>

namespace
{
    juce::String pctText  (float v) { return juce::String (v * 100.0f, 0) + " %"; }
    juce::String gainText (float v) { return juce::String (v, 2) + "x"; }
    juce::String msText   (float v) { return juce::String (v, 0) + " ms"; }
    juce::String dbText   (float v) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; }
    juce::String hzText   (float v) { return juce::String (v, 0) + " Hz"; }

    // Magnitude response (dB) of an RBJ peaking filter — same biquad as the DSP.
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
        Knob (juce::AudioProcessorValueTreeState& st, const juce::String& paramId,
              const juce::String& name, std::function<juce::String (float)> textFnIn)
            : stateRef (st), param (paramId), textFn (textFnIn)
        {
            slider.setSliderStyle (juce::Slider::Rotary);
            slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            slider.setRotaryParameters ({ juce::MathConstants<float>::pi * 1.25f,
                                          juce::MathConstants<float>::pi * 2.75f, true });
            addAndMakeVisible (slider);

            nameLabel.setText (name, juce::dontSendNotification);
            nameLabel.setFont (juce::FontOptions (11.0f, juce::Font::plain));
            nameLabel.setColour (juce::Label::textColourId, TemperLookAndFeel::muted);
            nameLabel.setJustificationType (juce::Justification::centred);
            addAndMakeVisible (nameLabel);

            valueLabel.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            valueLabel.setColour (juce::Label::textColourId, TemperLookAndFeel::text);
            valueLabel.setJustificationType (juce::Justification::centred);
            addAndMakeVisible (valueLabel);

            attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (st, paramId, slider);
            stateRef.addParameterListener (param, this);
            refreshLabel();
        }

        ~Knob() override { stateRef.removeParameterListener (param, this); }

        void parameterChanged (const juce::String&, float) override { triggerAsyncUpdate(); }
        void handleAsyncUpdate() override { refreshLabel(); }

        void resized() override
        {
            auto b = getLocalBounds();
            nameLabel.setBounds (b.removeFromTop (18));
            valueLabel.setBounds (b.removeFromBottom (18));
            slider.setBounds (b);
        }

    private:
        void refreshLabel()
        {
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (stateRef.getParameter (param)))
                if (textFn)
                    valueLabel.setText (textFn (p->getNormalisableRange().convertFrom0to1 (p->getValue())),
                                        juce::dontSendNotification);
        }

        juce::AudioProcessorValueTreeState& stateRef;
        juce::String param;
        std::function<juce::String (float)> textFn;
        juce::Slider slider;
        juce::Label nameLabel, valueLabel;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    // ---- a titled panel ----------------------------------------------------
    class Panel : public juce::Component
    {
    public:
        explicit Panel (const juce::String& title)
        {
            titleLabel.setText (title, juce::dontSendNotification);
            titleLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            titleLabel.setColour (juce::Label::textColourId, TemperLookAndFeel::accent);
            titleLabel.setJustificationType (juce::Justification::centred);
            addAndMakeVisible (titleLabel);
        }

        void paint (juce::Graphics& g) override { TemperLookAndFeel::drawPanelBackground (g, getLocalBounds()); }
        void resized() override { titleLabel.setBounds (0, 8, getWidth(), 18); }

        juce::Label titleLabel;
    };

    // ---- an on/off toggle for a boolean parameter --------------------------
    class ToggleButton : public juce::Component,
                         private juce::AudioProcessorValueTreeState::Listener,
                         private juce::AsyncUpdater
    {
    public:
        ToggleButton (const juce::String& label, const juce::String& paramId,
                      juce::AudioProcessorValueTreeState& st)
            : name (label), param (paramId), state (st)
        {
            state.addParameterListener (param, this);
        }

        ~ToggleButton() override { state.removeParameterListener (param, this); }

        void parameterChanged (const juce::String&, float) override { triggerAsyncUpdate(); }
        void handleAsyncUpdate() override { repaint(); }

        void paint (juce::Graphics& g) override
        {
            const bool on = isOn();
            auto r = getLocalBounds().toFloat().reduced (1.5f);
            const juce::Colour col = on ? TemperLookAndFeel::accent : TemperLookAndFeel::muted;

            if (on)
            {
                g.setColour (col.withAlpha (0.15f));
                g.fillRoundedRectangle (r, 7.0f);
                g.setColour (col.withAlpha (0.7f));
                g.drawRoundedRectangle (r.reduced (0.5f), 7.0f, 1.5f);
            }
            else
            {
                g.setColour (TemperLookAndFeel::panel);
                g.fillRoundedRectangle (r, 7.0f);
                g.setColour (TemperLookAndFeel::border);
                g.drawRoundedRectangle (r.reduced (0.5f), 7.0f, 1.0f);
            }

            g.setFont (juce::FontOptions (10.0f, on ? juce::Font::bold : juce::Font::plain));
            g.setColour (on ? TemperLookAndFeel::text : TemperLookAndFeel::muted);
            g.drawText (on ? name + " ON" : name + " OFF", r, juce::Justification::centred, false);
        }

        void mouseDown (const juce::MouseEvent&) override
        {
            if (auto* p = state.getParameter (param))
                p->setValueNotifyingHost (isOn() ? 0.0f : 1.0f);
        }

        bool isOn() const { return state.getRawParameterValue (param)->load() > 0.5f; }

    private:
        juce::String name;
        juce::String param;
        juce::AudioProcessorValueTreeState& state;
    };

    // ---- a module selector tab (radio-style) --------------------------------
    class ModuleTab : public juce::Component
    {
    public:
        ModuleTab (const juce::String& label, std::function<bool()> isSel, std::function<void()> onClick)
            : name (label), selected (std::move (isSel)), click (std::move (onClick)) {}

        void paint (juce::Graphics& g) override
        {
            const bool sel = selected();
            auto r = getLocalBounds().toFloat().reduced (2.0f);
            const juce::Colour col = sel ? TemperLookAndFeel::accent : TemperLookAndFeel::muted;

            if (sel)
            {
                g.setColour (col.withAlpha (0.16f));
                g.fillRoundedRectangle (r, 7.0f);
                g.setColour (col.withAlpha (0.7f));
                g.drawRoundedRectangle (r.reduced (0.5f), 7.0f, 1.5f);
            }
            else
            {
                g.setColour (TemperLookAndFeel::panel);
                g.fillRoundedRectangle (r, 7.0f);
                g.setColour (TemperLookAndFeel::border);
                g.drawRoundedRectangle (r.reduced (0.5f), 7.0f, 1.0f);
            }

            g.setFont (juce::FontOptions (11.0f, sel ? juce::Font::bold : juce::Font::plain));
            g.setColour (sel ? TemperLookAndFeel::text : TemperLookAndFeel::muted);
            g.drawText (name, r, juce::Justification::centred, false);
        }

        void mouseDown (const juce::MouseEvent&) override { if (click) click(); }

    private:
        juce::String name;
        std::function<bool()> selected;
        std::function<void()> click;
    };

    // ---- spectrum + per-module response curves + draggable EQ --------------
    class EqCurveEditor : public juce::Component,
                          private juce::Timer
    {
    public:
        EqCurveEditor (temper::SpectralSuppressor& e, juce::AudioProcessorValueTreeState& st)
            : engine (e), state (st)
        {
            fft = std::make_unique<juce::dsp::FFT> (kOrder);
            window.assign (kFftSize, 0.0f);
            for (int i = 0; i < kFftSize; ++i)
                window[static_cast<size_t> (i)] = 0.5f * (1.0f - std::cos (2.0f * 3.14159265358979323846f * static_cast<float> (i) / static_cast<float> (kFftSize - 1)));
            fftBuf.assign (kFftSize * 2, 0.0f);
            spectrum.assign (kNumBins, 0.0f);
            startTimerHz (30);
        }

        ~EqCurveEditor() override { stopTimer(); }

        void timerCallback() override { updateSpectrum(); repaint(); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (10.0f);
            auto plot = r.withLeft (r.getX() + 38.0f);

            g.setColour (TemperLookAndFeel::track);
            g.fillRoundedRectangle (r, 8.0f);
            g.setColour (TemperLookAndFeel::border);
            g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.5f);

            const float zeroY = gainToY (0.0f, plot);
            g.setColour (TemperLookAndFeel::muted.withAlpha (0.45f));
            g.drawLine (plot.getX(), zeroY, plot.getRight(), zeroY, 1.0f);

            g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
            g.setColour (TemperLookAndFeel::muted);
            g.drawText ("+18", juce::roundToInt (r.getX()), juce::roundToInt (plot.getY() - 5), 32, 12, juce::Justification::right, false);
            g.drawText (" 0",  juce::roundToInt (r.getX()), juce::roundToInt (zeroY - 6), 32, 12, juce::Justification::right, false);
            g.drawText ("-18", juce::roundToInt (r.getX()), juce::roundToInt (plot.getBottom() - 5), 32, 12, juce::Justification::right, false);

            // Spectrum (input).
            juce::Path spec;
            for (int i = 0; i < kNumBins; ++i)
            {
                const float freq = 40.0f * std::pow (500.0f, static_cast<float> (i) / (kNumBins - 1));
                const float x = freqToX (freq, plot);
                const float y = plot.getBottom() - spectrum[static_cast<size_t> (i)] * plot.getHeight();
                if (i == 0) spec.startNewSubPath (x, y); else spec.lineTo (x, y);
            }
            spec.lineTo (plot.getRight(), plot.getBottom());
            spec.lineTo (plot.getX(), plot.getBottom());
            spec.closeSubPath();
            g.setColour (TemperLookAndFeel::accent.withAlpha (0.14f));
            g.fillPath (spec);
            g.setColour (TemperLookAndFeel::accent.withAlpha (0.55f));
            g.strokePath (spec, juce::PathStrokeType (1.1f));

            // Per-module response curves (enabled modules only).
            const temper::Module mods[3] = { temper::Module::Suppressor, temper::Module::Ott, temper::Module::Gate };
            const juce::Colour modCol[3] = { juce::Colour (0xffff5a5a), juce::Colour (0xffffb347), juce::Colour (0xff4dd0e1) };
            const char* modName[3] = { "RES", "OTT", "GATE" };
            for (int m = 0; m < 3; ++m)
            {
                if (! engine.isModuleEnabled (mods[m]))
                    continue;
                drawModuleCurve (g, plot, mods[m], modCol[m], modName[m]);
            }

            // Manual EQ response curve.
            {
                juce::Path eqCurve;
                bool anyActive = false;
                for (int i = 0; i < temper::kNumEqBands; ++i)
                    if (std::fabs (getEqGain (i)) > 0.01f) { anyActive = true; break; }

                if (anyActive)
                {
                    for (int i = 0; i < kNumBins; ++i)
                    {
                        const float freq = 40.0f * std::pow (500.0f, static_cast<float> (i) / (kNumBins - 1));
                        const float x = freqToX (freq, plot);
                        const float y = gainToY (eqResponseDb (freq), plot);
                        if (i == 0) eqCurve.startNewSubPath (x, y); else eqCurve.lineTo (x, y);
                    }
                    g.setColour (TemperLookAndFeel::accent2.withAlpha (0.95f));
                    g.strokePath (eqCurve, juce::PathStrokeType (1.6f));
                }
            }

            // EQ nodes.
            for (int i = 0; i < temper::kNumEqBands; ++i)
            {
                const float freq = getEqFreq (i);
                const float gain = getEqGain (i);
                const bool active = std::fabs (gain) > 0.01f;
                const float x = freqToX (freq, plot);
                const float y = gainToY (gain, plot);
                const float sz = active ? 6.0f : 3.5f;
                g.setColour (active ? TemperLookAndFeel::accent2 : TemperLookAndFeel::muted.withAlpha (0.5f));
                g.fillEllipse (x - sz, y - sz, sz * 2.0f, sz * 2.0f);
                if (active)
                {
                    g.setColour (TemperLookAndFeel::text.withAlpha (0.8f));
                    g.drawEllipse (x - sz - 2.0f, y - sz - 2.0f, (sz + 2.0f) * 2.0f, (sz + 2.0f) * 2.0f, 1.0f);
                }
            }

            // Legend.
            {
                g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
                const int ly = juce::roundToInt (plot.getY() + 4.0f);
                const int lx = juce::roundToInt (plot.getRight() - 120.0f);
                for (int m = 0; m < 3; ++m)
                {
                    const int yy = ly + m * 13;
                    g.setColour (modCol[m]);
                    g.drawLine (static_cast<float> (lx), yy + 5.0f, static_cast<float> (lx) + 10.0f, yy + 5.0f, 2.0f);
                    g.drawText (modName[m], lx + 14, yy - 2, 46, 12, juce::Justification::left, false);
                }
                g.setColour (TemperLookAndFeel::accent2);
                g.drawLine (static_cast<float> (lx), ly + 3 * 13 + 5.0f, static_cast<float> (lx) + 10.0f, ly + 3 * 13 + 5.0f, 2.0f);
                g.drawText ("EQ", lx + 14, ly + 3 * 13 - 2, 46, 12, juce::Justification::left, false);
            }
        }

        // ---- interaction ---------------------------------------------------
        void mouseDown (const juce::MouseEvent& e) override
        {
            auto plot = plotArea();
            dragged = nearestNode (e.position.x, e.position.y, 16.0f);
            if (dragged < 0)
            {
                dragged = nearestInactiveNode();
                if (dragged >= 0)
                    setNode (dragged, xToFreq (e.position.x, plot), yToGain (e.position.y, plot));
            }
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragged < 0) return;
            auto plot = plotArea();
            setNode (dragged, xToFreq (e.position.x, plot), yToGain (e.position.y, plot));
        }

        void mouseUp (const juce::MouseEvent&) override { dragged = -1; }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            const int n = nearestNode (e.position.x, e.position.y, 14.0f);
            if (n >= 0)
                setParam (nodeGainId (n), 0.0f);
        }

    private:
        juce::Rectangle<float> plotArea() const
        {
            auto r = getLocalBounds().toFloat().reduced (10.0f);
            return r.withLeft (r.getX() + 38.0f);
        }

        float freqToX (float freq, juce::Rectangle<float> plot) const
        {
            const float lo = std::log (40.0f), hi = std::log (20000.0f);
            const float t = (std::log (juce::jlimit (40.0f, 20000.0f, freq)) - lo) / (hi - lo);
            return plot.getX() + t * plot.getWidth();
        }

        float xToFreq (float x, juce::Rectangle<float> plot) const
        {
            const float lo = std::log (40.0f), hi = std::log (20000.0f);
            const float t = juce::jlimit (0.0f, 1.0f, (x - plot.getX()) / plot.getWidth());
            return std::exp (lo + t * (hi - lo));
        }

        float gainToY (float gain, juce::Rectangle<float> plot) const
        {
            const float t = (juce::jlimit (-18.0f, 18.0f, gain) + 18.0f) / 36.0f;
            return plot.getBottom() - t * plot.getHeight();
        }

        float yToGain (float y, juce::Rectangle<float> plot) const
        {
            const float t = juce::jlimit (0.0f, 1.0f, (plot.getBottom() - y) / plot.getHeight());
            return t * 36.0f - 18.0f;
        }

        // Per-band gain curve for one module, connected across band centres.
        void drawModuleCurve (juce::Graphics& g, juce::Rectangle<float> plot,
                              temper::Module m, juce::Colour col, const char* /*name*/)
        {
            const int nb = engine.getModuleBands (m);
            if (nb <= 0) return;

            juce::Path curve;
            for (int b = 0; b < nb; ++b)
            {
                const float freq = engine.getModuleBandFreq (m, b);
                const float gain = juce::jlimit (-18.0f, 18.0f, engine.getModuleBandGainDb (m, b));
                const float x = freqToX (freq, plot);
                const float y = gainToY (gain, plot);
                if (b == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
            }

            // Fill the region between the 0 dB line and the curve.
            juce::Path fill = curve;
            const float xFirst = freqToX (engine.getModuleBandFreq (m, 0), plot);
            const float xLast  = freqToX (engine.getModuleBandFreq (m, nb - 1), plot);
            fill.lineTo (xLast,  gainToY (0.0f, plot));
            fill.lineTo (xFirst, gainToY (0.0f, plot));
            fill.closeSubPath();

            g.setColour (col.withAlpha (0.22f));
            g.fillPath (fill);
            g.setColour (col.withAlpha (0.95f));
            g.strokePath (curve, juce::PathStrokeType (1.4f));
        }

        float eqResponseDb (float freq) const
        {
            const float fs = static_cast<float> (engine.getSampleRate());
            float db = 0.0f;
            for (int i = 0; i < temper::kNumEqBands; ++i)
            {
                const float gain = getEqGain (i);
                if (std::fabs (gain) < 0.01f)
                    continue;
                db += peakingDb (freq, getEqFreq (i), 1.0f, gain, fs);
            }
            return db;
        }

        juce::String nodeFreqId (int i) const { return "eq" + juce::String (i + 1) + "freq"; }
        juce::String nodeGainId (int i) const { return "eq" + juce::String (i + 1) + "gain"; }

        float getEqFreq (int i) const { return state.getRawParameterValue (nodeFreqId (i))->load(); }
        float getEqGain (int i) const { return state.getRawParameterValue (nodeGainId (i))->load(); }

        void setNode (int i, float freq, float gain)
        {
            setParam (nodeFreqId (i), freq);
            setParam (nodeGainId (i), gain);
        }

        void setParam (const juce::String& id, float raw)
        {
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (state.getParameter (id)))
                p->setValueNotifyingHost (p->getNormalisableRange().convertTo0to1 (raw));
        }

        int nearestNode (float x, float y, float thresh) const
        {
            int best = -1;
            float bestD = thresh;
            auto plot = plotArea();
            for (int i = 0; i < temper::kNumEqBands; ++i)
            {
                if (std::fabs (getEqGain (i)) < 0.01f) continue;
                const float nx = freqToX (getEqFreq (i), plot);
                const float ny = gainToY (getEqGain (i), plot);
                const float d = std::hypot (nx - x, ny - y);
                if (d < bestD) { bestD = d; best = i; }
            }
            return best;
        }

        int nearestInactiveNode() const
        {
            for (int i = 0; i < temper::kNumEqBands; ++i)
                if (std::fabs (getEqGain (i)) < 0.01f)
                    return i;
            return -1;
        }

        void updateSpectrum()
        {
            const auto& audio = engine.vizInput();
            const int n = static_cast<int> (audio.size());
            const int wp = engine.vizWritePos();
            std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
            for (int i = 0; i < kFftSize && i < n; ++i)
            {
                int idx = wp - kFftSize + i;
                idx = ((idx % n) + n) % n;
                fftBuf[static_cast<size_t> (i)] = audio[static_cast<size_t> (idx)] * window[static_cast<size_t> (i)];
            }
            fft->performRealOnlyForwardTransform (fftBuf.data(), true);

            const float norm = 1.0f / (kFftSize * 0.25f);
            for (int b = 0; b < kNumBins; ++b)
            {
                const float freq = 40.0f * std::pow (500.0f, static_cast<float> (b) / (kNumBins - 1));
                const int bin = static_cast<int> (std::round (freq * kFftSize / 48000.0f));
                if (bin < 1 || bin * 2 + 1 >= kFftSize) { spectrum[static_cast<size_t> (b)] = 0.0f; continue; }
                const float re = fftBuf[static_cast<size_t> (bin * 2)];
                const float im = fftBuf[static_cast<size_t> (bin * 2 + 1)];
                spectrum[static_cast<size_t> (b)] = juce::jlimit (0.0f, 1.0f, std::sqrt (re * re + im * im) * norm);
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
    explicit Impl (TemperAudioProcessor& p) : processor (p), state (p.getState())
    {
        build();
        rebuildModuleControls();
    }

    void attachTo (juce::Component& root)
    {
        root.addAndMakeVisible (*title);
        root.addAndMakeVisible (subtitle);
        root.addAndMakeVisible (brand);
        root.addAndMakeVisible (footer);
        root.addAndMakeVisible (orderLabel);
        root.addAndMakeVisible (orderCombo);
        root.addAndMakeVisible (*autoGainToggle);
        root.addAndMakeVisible (*eqPanel);
        root.addAndMakeVisible (*curve);
        root.addAndMakeVisible (*modulePanel);
        for (auto& t : moduleTabs) modulePanel->addAndMakeVisible (*t);
        attachModuleControls();
    }

    void resized (juce::Rectangle<int> bounds)
    {
        lastBounds = bounds;
        auto b = bounds.reduced (16);

        footer.setBounds (b.removeFromBottom (18));

        auto header = b.removeFromTop (96);
        brand.setBounds (header.removeFromTop (18));
        title->setBounds (header.removeFromTop (44));
        subtitle.setBounds (header.removeFromTop (22));

        // Module order row.
        auto orderRow = b.removeFromTop (30);
        orderLabel.setBounds (orderRow.removeFromLeft (90));
        orderCombo.setBounds (orderRow.removeFromLeft (static_cast<int> (orderRow.getWidth() * 0.68f)));
        autoGainToggle->setBounds (orderRow);

        b.removeFromTop (6);

        // EQ panel (spectrum + curves) then the module panel.
        eqPanel->setBounds (b.removeFromTop (270));
        auto eqb = eqPanel->getLocalBounds().reduced (10);
        eqb.removeFromTop (26);
        curve->setBounds (eqb);

        b.removeFromTop (8);
        modulePanel->setBounds (b);

        auto mb = modulePanel->getLocalBounds().reduced (12);
        mb.removeFromTop (26);

        // Module tabs.
        auto tabRow = mb.removeFromTop (30);
        const int nw = tabRow.getWidth() / static_cast<int> (moduleTabs.size());
        for (auto& t : moduleTabs)
            t->setBounds (tabRow.removeFromLeft (nw).reduced (2, 2));

        mb.removeFromTop (6);

        // Enable toggle + band count + band selector.
        auto row2 = mb.removeFromTop (30);
        enableToggle->setBounds (row2.removeFromLeft (150).reduced (2, 2));
        bandsLabel.setBounds (row2.removeFromLeft (60));
        bandsCombo->setBounds (row2.removeFromLeft (70).reduced (2, 2));
        bandLabel.setBounds (row2.removeFromLeft (60));
        bandCombo.setBounds (row2);

        mb.removeFromTop (6);

        // Crossovers row (up to 4).
        auto xrow = mb.removeFromTop (86);
        const int nx = static_cast<int> (xoverKnobs.size());
        const int xw = nx > 0 ? xrow.getWidth() / nx : 0;
        for (auto& k : xoverKnobs)
            k->setBounds (xrow.removeFromLeft (xw).reduced (4, 2));

        mb.removeFromTop (6);

        // Per-band + global knobs.
        const int total = static_cast<int> (bandKnobs.size() + globalKnobs.size());
        const int kw = total > 0 ? mb.getWidth() / total : 0;
        for (auto& k : bandKnobs)   k->setBounds (mb.removeFromLeft (kw).reduced (4, 2));
        for (auto& k : globalKnobs) k->setBounds (mb.removeFromLeft (kw).reduced (4, 2));
    }

private:
    TemperAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& state;

    std::unique_ptr<HoloTitle> title;
    juce::Label subtitle, brand, footer;
    juce::Label orderLabel;
    juce::ComboBox orderCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> orderAttachment;
    std::unique_ptr<ToggleButton> autoGainToggle;

    std::unique_ptr<Panel> eqPanel;
    std::unique_ptr<EqCurveEditor> curve;

    std::unique_ptr<Panel> modulePanel;
    std::vector<std::unique_ptr<ModuleTab>> moduleTabs;

    // Rebuilt on module/band change:
    std::unique_ptr<ToggleButton> enableToggle;
    juce::Label bandsLabel, bandLabel;
    std::unique_ptr<juce::ComboBox> bandsCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> bandsAttachment;
    juce::ComboBox bandCombo;
    std::vector<std::unique_ptr<Knob>> xoverKnobs;
    std::vector<std::unique_ptr<Knob>> bandKnobs;
    std::vector<std::unique_ptr<Knob>> globalKnobs;

    int selectedModule = 0;   // 0=resonance, 1=OTT, 2=gate
    int selectedBand   = 1;
    juce::Rectangle<int> lastBounds;

    juce::String key (int m) const { return juce::String (kModKey[m]); }

    static const char* kModKey[3];

    void build()
    {
        title = std::make_unique<HoloTitle> ("TEMPER", 40.0f, true);

        subtitle.setText ("SPECTRAL PROCESSOR", juce::dontSendNotification);
        subtitle.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        subtitle.setColour (juce::Label::textColourId, TemperLookAndFeel::muted);
        subtitle.setJustificationType (juce::Justification::centred);

        brand.setText ("DEWPOINT AV", juce::dontSendNotification);
        brand.setFont (monumentFont (13.0f, false));
        brand.setColour (juce::Label::textColourId, TemperLookAndFeel::accent);
        brand.setJustificationType (juce::Justification::centred);

        footer.setText (juce::String ("DEWPOINT AV  -  v") + juce::String (JucePlugin_VersionString), juce::dontSendNotification);
        footer.setFont (monumentFont (11.0f, false));
        footer.setColour (juce::Label::textColourId, TemperLookAndFeel::muted);
        footer.setJustificationType (juce::Justification::centred);

        // Module order.
        orderLabel.setText ("ORDER", juce::dontSendNotification);
        orderLabel.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        orderLabel.setColour (juce::Label::textColourId, TemperLookAndFeel::muted);
        orderLabel.setJustificationType (juce::Justification::centredLeft);

        for (int i = 0; i < 6; ++i)
            orderCombo.addItem (juce::StringArray {
                "Resonance > OTT > Gate", "Resonance > Gate > OTT", "OTT > Resonance > Gate",
                "OTT > Gate > Resonance", "Gate > Resonance > OTT", "Gate > OTT > Resonance" }[i], i + 1);
        orderCombo.setSelectedId (1);
        orderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, "order", orderCombo);

        autoGainToggle = std::make_unique<ToggleButton> ("AUTO GAIN", "autoGain", state);

        // EQ panel.
        eqPanel = std::make_unique<Panel> ("SPECTRUM  -  click to add, drag to move, double-click to remove EQ nodes");
        curve = std::make_unique<EqCurveEditor> (processor.getEngine(), state);

        // Module panel + tabs.
        modulePanel = std::make_unique<Panel> ("MODULE");
        const char* tabNames[3] = { "RESONANCE", "SPECTRAL OTT", "SPECTRAL GATE" };
        for (int m = 0; m < 3; ++m)
        {
            moduleTabs.push_back (std::make_unique<ModuleTab> (tabNames[m],
                [this, m] { return selectedModule == m; },
                [this, m] { selectedModule = m; rebuildModuleControls(); relayout(); }));
        }

        // Band selector (local UI state).
        bandCombo.onChange = [this] { selectedBand = bandCombo.getSelectedId(); rebuildModuleControls(); relayout(); };
    }

    void attachModuleControls()
    {
        modulePanel->addAndMakeVisible (*enableToggle);
        modulePanel->addAndMakeVisible (bandsLabel);
        modulePanel->addAndMakeVisible (*bandsCombo);
        modulePanel->addAndMakeVisible (bandLabel);
        modulePanel->addAndMakeVisible (bandCombo);
        for (auto& k : xoverKnobs)  modulePanel->addAndMakeVisible (*k);
        for (auto& k : bandKnobs)   modulePanel->addAndMakeVisible (*k);
        for (auto& k : globalKnobs) modulePanel->addAndMakeVisible (*k);
    }

    void rebuildModuleControls()
    {
        const int m = selectedModule;
        const juce::String k = key (m);
        const juce::String s = juce::String (selectedBand);

        // Rebuild everything bound to the module/band.
        enableToggle  = std::make_unique<ToggleButton> ("ENABLE", k + "On", state);

        bandsLabel.setText ("BANDS", juce::dontSendNotification);
        bandsLabel.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        bandsLabel.setColour (juce::Label::textColourId, TemperLookAndFeel::muted);
        bandsLabel.setJustificationType (juce::Justification::centred);

        bandsCombo = std::make_unique<juce::ComboBox> ("Bands");
        for (int i = 1; i <= temper::kMaxBands; ++i)
            bandsCombo->addItem (juce::String (i), i);
        bandsAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, k + "Bands", *bandsCombo);

        bandLabel.setText ("BAND", juce::dontSendNotification);
        bandLabel.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        bandLabel.setColour (juce::Label::textColourId, TemperLookAndFeel::muted);
        bandLabel.setJustificationType (juce::Justification::centred);

        bandCombo.clear();
        for (int i = 1; i <= temper::kMaxBands; ++i)
            bandCombo.addItem (juce::String (i), i);
        bandCombo.setSelectedId (juce::jlimit (1, temper::kMaxBands, selectedBand), juce::dontSendNotification);

        // Crossovers (shared for the module).
        xoverKnobs.clear();
        for (int i = 1; i <= temper::kMaxCrossovers; ++i)
            xoverKnobs.push_back (std::make_unique<Knob> (state, k + "X" + juce::String (i), "X-OVER " + juce::String (i), hzText));

        bandKnobs.clear();
        globalKnobs.clear();

        if (m == 0)
        {
            bandKnobs.push_back (std::make_unique<Knob> (state, k + "Amt"  + s, "AMOUNT",      pctText));
            bandKnobs.push_back (std::make_unique<Knob> (state, k + "Sens" + s, "SENSITIVITY", pctText));
            bandKnobs.push_back (std::make_unique<Knob> (state, k + "Q"    + s, "BANDWIDTH",   pctText));
            globalKnobs.push_back (std::make_unique<Knob> (state, k + "Att",   "ATTACK",  msText));
            globalKnobs.push_back (std::make_unique<Knob> (state, k + "Rel",   "RELEASE", msText));
            globalKnobs.push_back (std::make_unique<Knob> (state, k + "Slope", "SLOPE",   pctText));
        }
        else if (m == 1)
        {
            bandKnobs.push_back (std::make_unique<Knob> (state, k + "Thr" + s, "THRESHOLD", dbText));
            bandKnobs.push_back (std::make_unique<Knob> (state, k + "Up"  + s, "UPWARD",    pctText));
            bandKnobs.push_back (std::make_unique<Knob> (state, k + "Dn"  + s, "DOWNWARD",  pctText));
            globalKnobs.push_back (std::make_unique<Knob> (state, k + "Att", "ATTACK",  msText));
            globalKnobs.push_back (std::make_unique<Knob> (state, k + "Rel", "RELEASE", msText));
        }
        else
        {
            bandKnobs.push_back (std::make_unique<Knob> (state, k + "Thr"   + s, "THRESHOLD", dbText));
            bandKnobs.push_back (std::make_unique<Knob> (state, k + "Range" + s, "RANGE",     dbText));
            globalKnobs.push_back (std::make_unique<Knob> (state, k + "Att", "ATTACK",  msText));
            globalKnobs.push_back (std::make_unique<Knob> (state, k + "Rel", "RELEASE", msText));
        }

        // (Re)attach after clearing the old ones (which removes them from the panel).
        attachModuleControls();
    }

    void relayout()
    {
        if (lastBounds.getWidth() > 0)
            resized (lastBounds);
    }
};

const char* TemperAudioProcessorEditor::Impl::kModKey[3] = { "supp", "ott", "gate" };

// ===========================================================================
TemperAudioProcessorEditor::TemperAudioProcessorEditor (TemperAudioProcessor& p)
    : AudioProcessorEditor (&p)
{
    setLookAndFeel (&lookAndFeel);

    impl = std::make_unique<Impl> (p);
    impl->attachTo (*this);

    setResizable (true, true);
    setResizeLimits (900, 640, 1500, 1050);
    setSize (980, 760);
}

TemperAudioProcessorEditor::~TemperAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void TemperAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (TemperLookAndFeel::background);
}

void TemperAudioProcessorEditor::resized()
{
    impl->resized (getLocalBounds());
}
