#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "HoloLogo.h"
#include "OutputSpectrum.h"

#include <cmath>
#include <functional>

namespace
{
    juce::String pctText   (float v) { return juce::String (v * 100.0f, 0) + " %"; }
    juce::String msText    (float v) { return juce::String (v, 0) + " ms"; }
    juce::String dbText    (float v) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; }
    juce::String hzText    (float v) { return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String (v, 0) + " Hz"; }
    juce::String xText     (float v) { return juce::String (v, 2) + "x"; }
    juce::String slopeText (float v) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB/oct"; }
    juce::String grText    (float v) { return juce::String (v, 1) + " dB"; }

    const juce::Colour kResCol  (0xffff5a5a);
    const juce::Colour kOttCol  (0xffffb347);
    const juce::Colour kGateCol (0xff4dd0e1);
    const juce::Colour kDetCol  (0xff7ee787);

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

    // ---- a rounded "pill" button: lit or not, with a label that can change ----
    class Pill : public juce::Component
    {
    public:
        Pill (std::function<juce::String()> labelFn, std::function<bool()> litFn, std::function<void()> onClick)
            : label (std::move (labelFn)), lit (std::move (litFn)), click (std::move (onClick)) {}

        void paint (juce::Graphics& g) override
        {
            const bool on = lit && lit();
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
            g.setFont (juce::FontOptions (10.5f, on ? juce::Font::bold : juce::Font::plain));
            g.setColour (on ? TemperLookAndFeel::text : TemperLookAndFeel::muted);
            g.drawText (label ? label() : juce::String(), r, juce::Justification::centred, false);
        }

        void mouseDown (const juce::MouseEvent&) override { if (click) click(); repaint(); }

    private:
        std::function<juce::String()> label;
        std::function<bool()> lit;
        std::function<void()> click;
    };

    // ---- an on/off toggle for a boolean parameter --------------------------
    class ToggleButton : public juce::Component,
                         private juce::AudioProcessorValueTreeState::Listener,
                         private juce::AsyncUpdater
    {
    public:
        ToggleButton (const juce::String& label, const juce::String& paramId,
                      juce::AudioProcessorValueTreeState& st)
            : pill ([this] { return isOn() ? name + " ON" : name + " OFF"; }, [this] { return isOn(); },
                    [this] { if (auto* p = state.getParameter (param)) p->setValueNotifyingHost (isOn() ? 0.0f : 1.0f); }),
              name (label), param (paramId), state (st)
        {
            addAndMakeVisible (pill);
            state.addParameterListener (param, this);
        }
        ~ToggleButton() override { state.removeParameterListener (param, this); }

        void parameterChanged (const juce::String&, float) override { triggerAsyncUpdate(); }
        void handleAsyncUpdate() override { pill.repaint(); }
        void resized() override { pill.setBounds (getLocalBounds()); }
        bool isOn() const { return state.getRawParameterValue (param)->load() > 0.5f; }

    private:
        Pill pill;
        juce::String name;
        juce::String param;
        juce::AudioProcessorValueTreeState& state;
    };

    // ---- a labelled combo box bound to a choice parameter ---------------------
    class ChoiceBox : public juce::Component
    {
    public:
        ChoiceBox (juce::AudioProcessorValueTreeState& st, const juce::String& paramId, const juce::String& title)
        {
            label.setText (title, juce::dontSendNotification);
            label.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            label.setColour (juce::Label::textColourId, TemperLookAndFeel::muted);
            label.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (label);
            if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (st.getParameter (paramId)))
                combo.addItemList (p->choices, 1);
            addAndMakeVisible (combo);
            attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (st, paramId, combo);
        }
        ~ChoiceBox() override { attachment.reset(); }
        void resized() override
        {
            auto b = getLocalBounds();
            label.setBounds (b.removeFromLeft (juce::jmin (juce::GlyphArrangement::getStringWidthInt (label.getFont(), label.getText()) + 10, b.getWidth() / 2)));
            combo.setBounds (b.reduced (0, 2));
        }

    private:
        juce::Label label;
        juce::ComboBox combo;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
    };

    // ---- spectrum, module curves, gain reduction, and the two node editors ----
    // Edits either the output EQ (8 bands) or the resonance detector's pre-filters (6), which
    // shape what the dynamic EQs hear, not the sound.
    class EqCurveEditor : public juce::Component,
                          private juce::Timer
    {
    public:
        enum class Target { Eq, Detector };

        EqCurveEditor (temper::SpectralSuppressor& e, juce::AudioProcessorValueTreeState& st)
            : engine (e), state (st)
        {
            grImage = juce::Image (juce::Image::ARGB, kGrCols, temper::kCurvePoints, true);
            setOpaque (true);
            startTimerHz (15);
        }
        ~EqCurveEditor() override { stopTimer(); }

        void setTarget (Target t) { target = t; dragged = hovered = -1; repaint(); }
        Target getTarget() const  { return target; }
        void setSpectrogram (bool on) { spectrogram = on; repaint(); }
        bool spectrogramOn() const { return spectrogram; }

        void timerCallback() override
        {
            outputSpectrum.update (engine.vizOutputL(), engine.vizOutputR(), engine.vizWritePos(), engine.getSampleRate());
            // Gain reduction spectrogram: scroll one column, newest on the right.
            {
                juce::Image::BitmapData bd (grImage, juce::Image::BitmapData::readWrite);
                for (int y = 0; y < temper::kCurvePoints; ++y)
                    for (int x = 0; x < kGrCols - 1; ++x)
                        bd.setPixelColour (x, y, bd.getPixelColour (x + 1, y));
                const float maxGr = juce::jmax (1.0f, state.getRawParameterValue ("suppMaxGr")->load());
                for (int p = 0; p < temper::kCurvePoints; ++p)
                {
                    const float g = juce::jlimit (0.0f, 1.0f, -engine.dynamics().grAt (p) / maxGr);
                    bd.setPixelColour (kGrCols - 1, temper::kCurvePoints - 1 - p, kResCol.withAlpha (std::sqrt (g) * 0.9f));
                }
            }
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            {
                auto wbg = getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
                g.fillAll (wbg.isTransparent() ? juce::Colour (0xff0a0c10) : wbg);
            }
            auto r = getLocalBounds().toFloat().reduced (10.0f);
            auto plot = r.withLeft (r.getX() + 38.0f);

            g.setColour (TemperLookAndFeel::track);
            g.fillRoundedRectangle (r, 8.0f);
            g.setColour (TemperLookAndFeel::border);
            g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.5f);

            if (spectrogram)
                g.drawImage (grImage, plot, juce::RectanglePlacement::stretchToFit);

            drawOutputAnalyzer (g, plot);

            const float zeroY = gainToY (0.0f, plot);
            g.setColour (TemperLookAndFeel::muted.withAlpha (0.45f));
            g.drawLine (plot.getX(), zeroY, plot.getRight(), zeroY, 1.0f);

            g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
            g.setColour (TemperLookAndFeel::muted);
            g.drawText ("+18", juce::roundToInt (r.getX()), juce::roundToInt (plot.getY() - 5), 32, 12, juce::Justification::right, false);
            g.drawText (" 0",  juce::roundToInt (r.getX()), juce::roundToInt (zeroY - 6), 32, 12, juce::Justification::right, false);
            g.drawText ("-18", juce::roundToInt (r.getX()), juce::roundToInt (plot.getBottom() - 5), 32, 12, juce::Justification::right, false);

            // Each module's live gain over frequency (resonance per bin; OTT and gate per band or bin).
            const std::pair<temper::Module, juce::Colour> mods[] = { { temper::Module::Suppressor, kResCol }, { temper::Module::Ott, kOttCol }, { temper::Module::Gate, kGateCol } };
            for (const auto& md : mods)
            {
                if (! engine.isModuleEnabled (md.first)) continue;
                juce::Path c;
                for (int p = 0; p < temper::kCurvePoints; ++p)
                {
                    const float x = freqToX (temper::curvePointFreq (p), plot), y = gainToY (engine.moduleGainAt (md.first, p), plot);
                    if (p == 0) c.startNewSubPath (x, y); else c.lineTo (x, y);
                }
                juce::Path fill (c);
                fill.lineTo (plot.getRight(), zeroY); fill.lineTo (plot.getX(), zeroY); fill.closeSubPath();
                g.setColour (md.second.withAlpha (0.18f)); g.fillPath (fill);
                g.setColour (md.second.withAlpha (0.95f)); g.strokePath (c, juce::PathStrokeType (1.3f));
            }

            // The detector's weighting (pre-filters + slope) and, in Custom mode, the learned curve.
            const bool det = target == Target::Detector;
            {
                juce::Path c;
                const float slope = engine.dynamics().currentSlope();
                for (int i = 0; i < kNumPts; ++i)
                {
                    const float f = ptFreq (i);
                    float db = slope * std::log2 (f / 1000.0f);
                    for (int n = 0; n < temper::kNumPreFilters; ++n) db += temper::nodeResponseDb (node (Target::Detector, n), sr(), f);
                    const float x = freqToX (f, plot), y = gainToY (db, plot);
                    if (i == 0) c.startNewSubPath (x, y); else c.lineTo (x, y);
                }
                g.setColour (kDetCol.withAlpha (det ? 0.95f : 0.25f));
                g.strokePath (c, juce::PathStrokeType (det ? 1.6f : 1.0f));
                if (det && juce::roundToInt (state.getRawParameterValue ("suppThrMode")->load()) == 2 && engine.dynamics().hasCustomCurve())
                {
                    float cv[temper::kCurvePoints];
                    engine.dynamics().getCustomCurve (cv);
                    juce::Path cc;
                    for (int p = 0; p < temper::kCurvePoints; ++p)
                    {
                        const float x = freqToX (temper::curvePointFreq (p), plot), y = gainToY (cv[p], plot);
                        if (p == 0) cc.startNewSubPath (x, y); else cc.lineTo (x, y);
                    }
                    juce::Path dashed;
                    const float dashes[] = { 4.0f, 3.0f };
                    juce::PathStrokeType (1.0f).createDashedStroke (dashed, cc, dashes, 2);
                    g.setColour (TemperLookAndFeel::text.withAlpha (0.6f));
                    g.fillPath (dashed);
                }
            }
            // The output EQ.
            {
                juce::Path c;
                for (int i = 0; i < kNumPts; ++i)
                {
                    const float f = ptFreq (i);
                    float db = 0.0f;
                    for (int n = 0; n < temper::kNumEqBands; ++n) db += temper::nodeResponseDb (node (Target::Eq, n), sr(), f);
                    const float x = freqToX (f, plot), y = gainToY (db, plot);
                    if (i == 0) c.startNewSubPath (x, y); else c.lineTo (x, y);
                }
                g.setColour (TemperLookAndFeel::accent2.withAlpha (det ? 0.3f : 0.95f));
                g.strokePath (c, juce::PathStrokeType (det ? 1.0f : 1.6f));
            }

            // Nodes of whichever is being edited.
            const juce::Colour nodeCol = det ? kDetCol : TemperLookAndFeel::accent2;
            const int listenBand = juce::roundToInt (state.getRawParameterValue ("eqListen")->load());
            for (int i = 0; i < count(); ++i)
            {
                const auto nd = node (target, i);
                if (! nd.on) continue;
                const auto pos = nodePos (i, plot);
                const float sz = 6.0f;
                g.setColour (nodeCol);
                g.fillEllipse (pos.x - sz, pos.y - sz, sz * 2.0f, sz * 2.0f);
                g.setColour (TemperLookAndFeel::background);
                g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
                g.drawText (juce::String (i + 1), juce::Rectangle<float> (pos.x - sz, pos.y - sz, sz * 2.0f, sz * 2.0f), juce::Justification::centred, false);
                if (i == hovered || i == dragged || (! det && listenBand == i + 1))
                {
                    g.setColour ((! det && listenBand == i + 1) ? juce::Colours::yellow.withAlpha (0.9f) : TemperLookAndFeel::text.withAlpha (0.8f));
                    g.drawEllipse (pos.x - sz - 2.5f, pos.y - sz - 2.5f, (sz + 2.5f) * 2.0f, (sz + 2.5f) * 2.0f, 1.2f);
                }
            }

            // Readout for the hovered / dragged node, or a hint.
            {
                const int i = dragged >= 0 ? dragged : hovered;
                juce::String txt;
                if (i >= 0 && node (target, i).on)
                {
                    const auto nd = node (target, i);
                    txt = (det ? "DETECTOR " : "EQ ") + juce::String (i + 1) + "   " + juce::String (temper::filterTypeName (nd.type)).toUpperCase()
                        + "   " + hzText (nd.freq);
                    if (temper::filterHasGain (nd.type)) txt += "   " + dbText (nd.gainDb);
                    txt += "   Q " + juce::String (nd.q, 2);
                    if (temper::filterHasSlope (nd.type)) txt += "   " + juce::String (12 << nd.slope) + " dB/oct";
                    if (! det) { static const char* ch[] = { "STEREO", "LEFT", "RIGHT", "MID", "SIDE" }; txt += "   " + juce::String (ch[juce::jlimit (0, 4, nd.channel)]); }
                }
                else
                    txt = det ? "DETECTOR FILTERS - shape what the resonance module reacts to (not the sound)"
                              : "click to add a band - drag - wheel: Q - right-click: type / slope / channel / listen - double-click: remove";
                g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
                g.setColour (TemperLookAndFeel::text.withAlpha (0.75f));
                g.drawText (txt, juce::roundToInt (plot.getX() + 6), juce::roundToInt (plot.getY() + 4), juce::roundToInt (plot.getWidth() * 0.75f), 12, juce::Justification::left, true);
            }

            // Legend.
            {
                g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
                const int ly = juce::roundToInt (plot.getY() + 4.0f);
                const int lx = juce::roundToInt (plot.getRight() - 110.0f);
                const std::pair<juce::Colour, const char*> items[] = { { kResCol, "RES (GR)" }, { kOttCol, "OTT" }, { kGateCol, "GATE" },
                    { TemperLookAndFeel::accent2, "EQ" }, { kDetCol, "DETECTOR" }, { TemperLookAndFeel::text.withAlpha (0.6f), "OUTPUT" } };
                int row = 0;
                for (const auto& it : items)
                {
                    const int yy = ly + row++ * 13;
                    g.setColour (it.first);
                    g.drawLine (static_cast<float> (lx), yy + 5.0f, static_cast<float> (lx) + 10.0f, yy + 5.0f, 2.0f);
                    g.drawText (it.second, lx + 14, yy - 2, 80, 12, juce::Justification::left, false);
                }
                const float gr = engine.dynamics().maxGrNow();
                if (engine.isModuleEnabled (temper::Module::Suppressor))
                {
                    g.setColour (kResCol);
                    g.drawText ("MAX GR " + grText (gr), lx, ly + row * 13 + 2, 100, 12, juce::Justification::left, false);
                }
            }
        }

        // ---- interaction ---------------------------------------------------
        void mouseMove (const juce::MouseEvent& e) override
        {
            const int h = nearestNode (e.position, 14.0f);
            if (h != hovered) { hovered = h; repaint(); }
        }
        void mouseExit (const juce::MouseEvent&) override { hovered = -1; repaint(); }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const int n = nearestNode (e.position, 16.0f);
            if (e.mods.isPopupMenu())
            {
                if (n >= 0) showMenu (n);
                return;
            }
            dragged = n;
            if (dragged < 0)
            {
                dragged = firstFreeNode();
                if (dragged >= 0)
                {
                    setParam (id (dragged, "on"), 1.0f);
                    moveNode (dragged, e.position);
                }
            }
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragged >= 0 && ! e.mods.isPopupMenu()) moveNode (dragged, e.position);
        }

        void mouseUp (const juce::MouseEvent&) override { dragged = -1; }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            const int n = nearestNode (e.position, 14.0f);
            if (n >= 0) removeNode (n);
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            const int n = nearestNode (e.position, 20.0f);
            if (n < 0) return;
            const float q = node (target, n).q * std::pow (1.15f, w.deltaY > 0 ? 1.0f : -1.0f);
            setParam (id (n, "q"), juce::jlimit (0.1f, 18.0f, q));
        }

    private:
        static constexpr int kNumPts = 220;
        static constexpr int kGrCols = 240;

        double sr() const { return engine.getSampleRate(); }
        int count() const { return target == Target::Eq ? temper::kNumEqBands : temper::kNumPreFilters; }
        juce::String id (int i, const char* field) const
        {
            return juce::String (target == Target::Eq ? "eq" : "pf") + juce::String (i + 1) + field;
        }
        float raw (const juce::String& pid) const { return state.getRawParameterValue (pid)->load(); }

        temper::FilterNode node (Target t, int i) const
        {
            const juce::String p = juce::String (t == Target::Eq ? "eq" : "pf") + juce::String (i + 1);
            temper::FilterNode nd;
            nd.on = raw (p + "on") > 0.5f; nd.type = juce::roundToInt (raw (p + "type")); nd.freq = raw (p + "freq");
            nd.gainDb = raw (p + "gain"); nd.q = raw (p + "q"); nd.slope = juce::roundToInt (raw (p + "slope"));
            nd.channel = t == Target::Eq ? juce::roundToInt (raw (p + "ch")) : 0;
            return nd;
        }

        juce::Point<float> nodePos (int i, juce::Rectangle<float> plot) const
        {
            const auto nd = node (target, i);
            const float g = temper::filterHasGain (nd.type) ? nd.gainDb : 0.0f;
            return { freqToX (nd.freq, plot), gainToY (g, plot) };
        }

        void moveNode (int i, juce::Point<float> p)
        {
            auto plot = plotArea();
            setParam (id (i, "freq"), xToFreq (p.x, plot));
            if (temper::filterHasGain (node (target, i).type))
                setParam (id (i, "gain"), yToGain (p.y, plot));
        }

        void removeNode (int i)
        {
            setParam (id (i, "on"), 0.0f);
            if (target == Target::Eq && juce::roundToInt (raw ("eqListen")) == i + 1) setParam ("eqListen", 0.0f);
        }

        void showMenu (int i)
        {
            const auto nd = node (target, i);
            juce::PopupMenu types, slopes, chans, m;
            for (int t = 0; t < static_cast<int> (temper::FilterType::Count); ++t)
                types.addItem (100 + t, temper::filterTypeName (t), true, nd.type == t);
            for (int s = 0; s < 3; ++s)
                slopes.addItem (200 + s, juce::String (12 << s) + " dB/oct", true, nd.slope == s);
            static const char* ch[] = { "Stereo", "Left", "Right", "Mid", "Side" };
            for (int c = 0; c < 5; ++c)
                chans.addItem (300 + c, ch[c], true, nd.channel == c);
            m.addSectionHeader ((target == Target::Eq ? "EQ band " : "Detector filter ") + juce::String (i + 1));
            m.addSubMenu ("Type", types);
            m.addSubMenu ("Slope", slopes, temper::filterHasSlope (nd.type));
            if (target == Target::Eq)
            {
                m.addSubMenu ("Channel", chans);
                m.addItem (400, "Listen to this band", true, juce::roundToInt (raw ("eqListen")) == i + 1);
            }
            m.addSeparator();
            m.addItem (500, "Remove");
            juce::Component::SafePointer<EqCurveEditor> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options(), [safe, i] (int r)
            {
                if (safe == nullptr || r == 0) return;
                auto& self = *safe;
                if (r >= 100 && r < 200)      self.setParam (self.id (i, "type"), static_cast<float> (r - 100));
                else if (r >= 200 && r < 300) self.setParam (self.id (i, "slope"), static_cast<float> (r - 200));
                else if (r >= 300 && r < 400) self.setParam (self.id (i, "ch"), static_cast<float> (r - 300));
                else if (r == 400)            self.setParam ("eqListen", juce::roundToInt (self.raw ("eqListen")) == i + 1 ? 0.0f : static_cast<float> (i + 1));
                else if (r == 500)            self.removeNode (i);
                self.repaint();
            });
        }

        juce::Rectangle<float> plotArea() const
        {
            auto r = getLocalBounds().toFloat().reduced (10.0f);
            return r.withLeft (r.getX() + 38.0f);
        }
        static float ptFreq (int i) { return 20.0f * std::pow (1000.0f, static_cast<float> (i) / static_cast<float> (kNumPts - 1)); }

        float freqToX (float freq, juce::Rectangle<float> plot) const
        {
            const float lo = std::log (20.0f), hi = std::log (20000.0f);
            const float t = (std::log (juce::jlimit (20.0f, 20000.0f, freq)) - lo) / (hi - lo);
            return plot.getX() + t * plot.getWidth();
        }
        float xToFreq (float x, juce::Rectangle<float> plot) const
        {
            const float lo = std::log (20.0f), hi = std::log (20000.0f);
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

        void drawOutputAnalyzer (juce::Graphics& g, juce::Rectangle<float> plot)
        {
            const auto dbToY = [&plot] (float db)
            {
                return plot.getBottom() - (db - OutputSpectrum::kFloorDb) / -OutputSpectrum::kFloorDb * plot.getHeight();
            };
            g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
            for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
            {
                const float x = freqToX (f, plot);
                g.setColour (TemperLookAndFeel::muted.withAlpha (0.14f));
                g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
                g.setColour (TemperLookAndFeel::muted.withAlpha (0.7f));
                g.drawText (f >= 1000.0f ? juce::String (f / 1000.0f, 0) + "k" : juce::String (f, 0),
                            juce::roundToInt (x) - 16, juce::roundToInt (plot.getBottom()) - 13, 32, 12, juce::Justification::centred, false);
            }
            for (float db : { 0.0f, -30.0f, -60.0f })
            {
                const float y = dbToY (db);
                g.setColour (TemperLookAndFeel::muted.withAlpha (0.14f));
                g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
                g.setColour (TemperLookAndFeel::muted.withAlpha (0.7f));
                g.drawText (juce::String (db, 0) + " dB", juce::roundToInt (plot.getRight()) - 44, juce::roundToInt (y) + 1, 42, 12, juce::Justification::right, false);
            }
            juce::Path line;
            for (int i = 0; i < OutputSpectrum::kPoints; ++i)
            {
                const float x = freqToX (OutputSpectrum::freqAt (i), plot);
                const float y = dbToY (outputSpectrum.dbAt (i));
                if (i == 0) line.startNewSubPath (x, y); else line.lineTo (x, y);
            }
            juce::Path fill (line);
            fill.lineTo (plot.getRight(), plot.getBottom());
            fill.lineTo (plot.getX(), plot.getBottom());
            fill.closeSubPath();
            g.setColour (TemperLookAndFeel::text.withAlpha (0.10f));
            g.fillPath (fill);
            g.setColour (TemperLookAndFeel::text.withAlpha (0.38f));
            g.strokePath (line, juce::PathStrokeType (1.0f));
        }

        void setParam (const juce::String& pid, float rawValue)
        {
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (state.getParameter (pid)))
                p->setValueNotifyingHost (p->getNormalisableRange().convertTo0to1 (rawValue));
        }

        int nearestNode (juce::Point<float> pt, float thresh) const
        {
            int best = -1;
            float bestD = thresh;
            auto plot = plotArea();
            for (int i = 0; i < count(); ++i)
            {
                if (! node (target, i).on) continue;
                const float d = nodePos (i, plot).getDistanceFrom (pt);
                if (d < bestD) { bestD = d; best = i; }
            }
            return best;
        }

        int firstFreeNode() const
        {
            for (int i = 0; i < count(); ++i)
                if (! node (target, i).on) return i;
            return -1;
        }

        temper::SpectralSuppressor& engine;
        juce::AudioProcessorValueTreeState& state;
        OutputSpectrum outputSpectrum;
        juce::Image grImage;
        Target target = Target::Eq;
        bool spectrogram = false;
        int dragged = -1, hovered = -1;
    };
}

// ===========================================================================
class TemperAudioProcessorEditor::Impl : private juce::Timer
{
public:
    explicit Impl (TemperAudioProcessor& p) : processor (p), state (p.getState())
    {
        build();
        rebuildModuleControls();
        startTimerHz (6);
    }
    ~Impl() override { stopTimer(); }

    void attachTo (juce::Component& root)
    {
        root.addAndMakeVisible (*title);
        root.addAndMakeVisible (subtitle);
        root.addAndMakeVisible (brand);
        root.addAndMakeVisible (footer);
        root.addAndMakeVisible (*orderBox);
        root.addAndMakeVisible (*listenBox);
        root.addAndMakeVisible (*autoGainToggle);
        root.addAndMakeVisible (*eqPanel);
        eqPanel->addAndMakeVisible (*curve);
        eqPanel->addAndMakeVisible (*editEqPill);
        eqPanel->addAndMakeVisible (*editDetPill);
        eqPanel->addAndMakeVisible (*specPill);
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

        auto orderRow = b.removeFromTop (30);
        orderBox->setBounds (orderRow.removeFromLeft (static_cast<int> (orderRow.getWidth() * 0.42f)).reduced (2, 0));
        listenBox->setBounds (orderRow.removeFromLeft (static_cast<int> (orderRow.getWidth() * 0.5f)).reduced (8, 0));
        autoGainToggle->setBounds (orderRow.reduced (2, 2));

        b.removeFromTop (6);
        eqPanel->setBounds (b.removeFromTop (290));
        {
            auto top = eqPanel->getLocalBounds().reduced (12, 6).removeFromTop (24);
            specPill->setBounds (top.removeFromRight (130));
            top.removeFromRight (6);
            editDetPill->setBounds (top.removeFromRight (110));
            editEqPill->setBounds (top.removeFromRight (70));
        }
        auto eqb = eqPanel->getLocalBounds().reduced (10);
        eqb.removeFromTop (26);
        curve->setBounds (eqb);

        b.removeFromTop (8);
        modulePanel->setBounds (b);

        auto mb = modulePanel->getLocalBounds().reduced (12);
        mb.removeFromTop (26);
        auto tabRow = mb.removeFromTop (30);
        const int nw = tabRow.getWidth() / static_cast<int> (moduleTabs.size());
        for (auto& t : moduleTabs)
            t->setBounds (tabRow.removeFromLeft (nw).reduced (2, 2));
        mb.removeFromTop (6);

        // Top row: enable, the module's menus and switches; then (resonance) a second row of
        // switches; then knobs. OTT / gate: global knobs, band selector, band knobs.
        auto row1 = mb.removeFromTop (28);
        enableToggle->setBounds (row1.removeFromLeft (130).reduced (2, 1));
        {
            const int n = static_cast<int> (combos.size() + toggles.size());
            const int w = n > 0 ? row1.getWidth() / n : 0;
            for (auto& c : combos)  c->setBounds (row1.removeFromLeft (w).reduced (5, 0));
            for (auto& t : toggles) t->setBounds (row1.removeFromLeft (w).reduced (3, 1));
        }
        mb.removeFromTop (4);
        if (! actionPills.empty())
        {
            auto row2 = mb.removeFromTop (26);
            for (auto& a2 : actionPills) a2->setBounds (row2.removeFromLeft (150).reduced (2, 1));
            statusLabel.setBounds (row2.reduced (8, 0));
            mb.removeFromTop (4);
        }
        if (bandPills.empty())
        {
            const int kw = globalKnobs.empty() ? 0 : mb.getWidth() / static_cast<int> (globalKnobs.size());
            for (auto& k : globalKnobs) k->setBounds (mb.removeFromLeft (kw).reduced (3, 0));
            return;
        }
        const int half = (mb.getHeight() - 30) / 2;
        auto grow = mb.removeFromTop (half);
        const int kw = globalKnobs.empty() ? 0 : grow.getWidth() / static_cast<int> (globalKnobs.size());
        for (auto& k : globalKnobs) k->setBounds (grow.removeFromLeft (kw).reduced (2, 0));
        auto prow = mb.removeFromTop (30).reduced (0, 3);
        for (auto& bp : bandPills) bp->setBounds (prow.removeFromLeft (110).reduced (2, 0));
        if (soloToggle)   soloToggle->setBounds (prow.removeFromLeft (120).reduced (6, 0));
        if (bypassToggle) bypassToggle->setBounds (prow.removeFromLeft (130).reduced (2, 0));
        const int bw = bandKnobs.empty() ? 0 : mb.getWidth() / static_cast<int> (bandKnobs.size());
        for (auto& k : bandKnobs) k->setBounds (mb.removeFromLeft (bw).reduced (2, 0));
    }

private:
    TemperAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& state;

    std::unique_ptr<HoloTitle> title;
    juce::Label subtitle, brand, footer;
    std::unique_ptr<ChoiceBox> orderBox, listenBox;
    std::unique_ptr<ToggleButton> autoGainToggle;

    std::unique_ptr<Panel> eqPanel;
    std::unique_ptr<EqCurveEditor> curve;
    std::unique_ptr<Pill> editEqPill, editDetPill, specPill;

    std::unique_ptr<Panel> modulePanel;
    std::vector<std::unique_ptr<Pill>> moduleTabs;

    // Rebuilt on module/band change:
    std::unique_ptr<ToggleButton> enableToggle;
    std::vector<std::unique_ptr<ChoiceBox>> combos;        // the module's top row
    std::vector<std::unique_ptr<ToggleButton>> toggles;
    std::vector<std::unique_ptr<Pill>> actionPills;         // learn / clear
    juce::Label statusLabel;
    std::vector<std::unique_ptr<Pill>> bandPills;           // OTT / gate: low / mid / high
    std::unique_ptr<ToggleButton> soloToggle, bypassToggle;
    std::vector<std::unique_ptr<Knob>> globalKnobs, bandKnobs;

    int selectedModule = 0;   // 0 = resonance, 1 = OTT, 2 = gate
    int selectedBand   = 1;   // 1..3 (OTT / gate)
    juce::Rectangle<int> lastBounds;

    static const char* kModKey[3];
    juce::String key (int m) const { return juce::String (kModKey[m]); }

    void timerCallback() override
    {
        for (auto& a2 : actionPills) a2->repaint();
        if (selectedModule == 2)
        {
            const int bands = juce::roundToInt (state.getRawParameterValue ("gateBands")->load()) + 1;
            if (bands != static_cast<int> (bandPills.size())) { rebuildModuleControls(); relayout(); }
            juce::String s2;
            const bool spectral = state.getRawParameterValue ("gateEngine")->load() > 0.5f;
            if (processor.isNoiseLearning())                          s2 = "Learning noise... play only the noise, then press STOP";
            else if (! spectral)                                      s2 = "Band gate: per band threshold / range / ratio";
            else if (! processor.getEngine().gateEngine().hasProfile()) s2 = "Spectral: no noise profile yet - LEARN NOISE on a noise-only part";
            else if (state.getRawParameterValue ("gateProfile")->load() > 0.5f) s2 = "Thresholds = dB above the learned noise, bin by bin";
            else                                                      s2 = "Profile learned: turn NOISE PROFILE on to use it";
            if (s2 != statusLabel.getText()) statusLabel.setText (s2, juce::dontSendNotification);
            return;
        }
        if (selectedModule != 0) return;
        juce::String s;
        const int mode = juce::roundToInt (state.getRawParameterValue ("suppThrMode")->load());
        if (processor.isLearning())                         s = "Learning... play a representative part, then press STOP";
        else if (mode == 2 && ! processor.getEngine().dynamics().hasCustomCurve()) s = "Custom: no curve yet - press LEARN";
        else if (mode == 2)                                 s = "Custom curve loaded (dashed, in DETECTOR view)";
        else if (mode == 1)                                 s = "Threshold = dB above the music's own spectrum";
        else                                                s = "Threshold = level per frequency (0 dB = full-scale sine)";
        if (state.getRawParameterValue ("suppSc")->load() > 0.5f && ! processor.hasSidechain())
            s = "Sidechain is on but the host sends none: using the main input";
        if (s != statusLabel.getText()) statusLabel.setText (s, juce::dontSendNotification);
    }

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

        orderBox  = std::make_unique<ChoiceBox> (state, "order", "ORDER");
        listenBox = std::make_unique<ChoiceBox> (state, "listen", "LISTEN");
        autoGainToggle = std::make_unique<ToggleButton> ("AUTO GAIN", "autoGain", state);

        eqPanel = std::make_unique<Panel> ("SPECTRUM");
        eqPanel->titleLabel.setJustificationType (juce::Justification::centredLeft);
        curve = std::make_unique<EqCurveEditor> (processor.getEngine(), state);
        editEqPill  = std::make_unique<Pill> ([] { return juce::String ("EQ"); },
            [this] { return curve->getTarget() == EqCurveEditor::Target::Eq; },
            [this] { curve->setTarget (EqCurveEditor::Target::Eq); editDetPill->repaint(); });
        editDetPill = std::make_unique<Pill> ([] { return juce::String ("DETECTOR"); },
            [this] { return curve->getTarget() == EqCurveEditor::Target::Detector; },
            [this] { curve->setTarget (EqCurveEditor::Target::Detector); editEqPill->repaint(); });
        specPill = std::make_unique<Pill> ([] { return juce::String ("GR SPECTROGRAM"); },
            [this] { return curve->spectrogramOn(); },
            [this] { curve->setSpectrogram (! curve->spectrogramOn()); });

        modulePanel = std::make_unique<Panel> ("MODULE");
        const char* tabNames[3] = { "RESONANCE", "OTT", "GATE" };
        for (int m = 0; m < 3; ++m)
            moduleTabs.push_back (std::make_unique<Pill> ([n = juce::String (tabNames[m])] { return n; },
                [this, m] { return selectedModule == m; },
                [this, m]
                {
                    selectedModule = m;
                    rebuildModuleControls();
                    relayout();
                    for (auto& t : moduleTabs) t->repaint();
                }));

        statusLabel.setFont (juce::FontOptions (10.5f, juce::Font::plain));
        statusLabel.setColour (juce::Label::textColourId, TemperLookAndFeel::muted);
        statusLabel.setJustificationType (juce::Justification::centredLeft);
    }

    void attachModuleControls()
    {
        auto& mp = *modulePanel;
        mp.addAndMakeVisible (*enableToggle);
        for (auto& c : combos)  mp.addAndMakeVisible (*c);
        for (auto& t : toggles) mp.addAndMakeVisible (*t);
        for (auto& a2 : actionPills) mp.addAndMakeVisible (*a2);
        if (! actionPills.empty()) mp.addAndMakeVisible (statusLabel);
        for (auto& bp : bandPills) mp.addAndMakeVisible (*bp);
        if (soloToggle)   mp.addAndMakeVisible (*soloToggle);
        if (bypassToggle) mp.addAndMakeVisible (*bypassToggle);
        for (auto& k : globalKnobs) mp.addAndMakeVisible (*k);
        for (auto& k : bandKnobs)   mp.addAndMakeVisible (*k);
    }

    static juce::String downText (float v)   // OTT downward amount as a ratio
    {
        if (v < 0.001f) return "1:1";
        if (v < 0.999f) return juce::String (1.0f / (1.0f - v), 2) + ":1";
        if (v <= 1.001f) return "inf:1";
        return "inf+" + juce::String ((v - 1.0f) * 100.0f, 0) + "%";
    }
    static juce::String upText (float v)
    {
        if (v < 0.001f) return "1:1";
        if (v > 0.999f) return "1:inf";
        return "1:" + juce::String (1.0f / (1.0f - v), 2);
    }
    static juce::String linkText (float v) { return juce::String (v * 100.0f, 0) + " %"; }
    static juce::String ratioText (float v) { return v >= 49.5f ? juce::String ("GATE") : "1:" + juce::String (v, 1); }

    void makeBandPills (int count)
    {
        static const char* names[3] = { "LOW", "MID", "HIGH" };
        for (int b = 1; b <= count; ++b)
            bandPills.push_back (std::make_unique<Pill> ([b, count] { return count == 1 ? juce::String ("BAND") : juce::String (names[b - 1]); },
                [this, b] { return selectedBand == b; },
                [this, b]
                {
                    // The pill being clicked is rebuilt too: do it after the click returns.
                    selectedBand = b;
                    juce::Component::SafePointer<Panel> guard (modulePanel.get());
                    juce::MessageManager::callAsync ([this, guard] { if (guard != nullptr) { rebuildModuleControls(); relayout(); } });
                }));
    }

    void rebuildModuleControls()
    {
        const int m = selectedModule;
        const juce::String s = juce::String (selectedBand);

        // Old controls go first (their attachments and listeners point at the parameters).
        combos.clear(); toggles.clear(); actionPills.clear(); bandPills.clear();
        soloToggle.reset(); bypassToggle.reset();
        globalKnobs.clear(); bandKnobs.clear();
        if (modulePanel != nullptr) modulePanel->removeChildComponent (&statusLabel);

        enableToggle = std::make_unique<ToggleButton> ("ENABLE", key (m) + "On", state);

        if (m == 0)
        {
            combos.push_back (std::make_unique<ChoiceBox> (state, "suppQuality", "QUALITY"));
            combos.push_back (std::make_unique<ChoiceBox> (state, "suppPhase",   "PHASE"));
            combos.push_back (std::make_unique<ChoiceBox> (state, "suppStereo",  "STEREO"));
            combos.push_back (std::make_unique<ChoiceBox> (state, "suppThrMode", "THRESHOLD"));
            toggles.push_back (std::make_unique<ToggleButton> ("ADAPT SLOPE", "suppAdaptSlope", state));
            toggles.push_back (std::make_unique<ToggleButton> ("SIDECHAIN", "suppSc", state));
            actionPills.push_back (std::make_unique<Pill> ([this] { return processor.isLearning() ? juce::String ("STOP LEARNING") : juce::String ("LEARN CURVE"); },
                [this] { return processor.isLearning(); },
                [this] { if (processor.isLearning()) processor.stopLearn(); else processor.startLearn(); }));
            actionPills.push_back (std::make_unique<Pill> ([] { return juce::String ("CLEAR CURVE"); }, [] { return false; },
                [this] { processor.clearCustomCurve(); }));

            globalKnobs.push_back (std::make_unique<Knob> (state, "suppThresh",   "THRESHOLD",   dbText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "suppSlope",    "SLOPE",       slopeText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "suppSelect",   "SELECTIVITY", pctText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "suppAtt",      "ATTACK",      msText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "suppRel",      "RELEASE",     msText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "suppStrength", "STRENGTH",    xText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "suppMaxGr",    "MAX GR",      [] (float v) { return "-" + juce::String (v, 1) + " dB"; }));
            globalKnobs.push_back (std::make_unique<Knob> (state, "suppLink",     "LINK",        pctText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "suppScGain",   "SC GAIN",     dbText));
        }
        else if (m == 1)
        {
            selectedBand = juce::jlimit (1, 3, selectedBand);
            combos.push_back (std::make_unique<ChoiceBox> (state, "ottEngine", "ENGINE"));
            combos.push_back (std::make_unique<ChoiceBox> (state, "ottStyle",  "STYLE"));
            combos.push_back (std::make_unique<ChoiceBox> (state, "ottKnee",   "KNEE"));
            combos.push_back (std::make_unique<ChoiceBox> (state, "ottDetect", "DETECT"));
            combos.push_back (std::make_unique<ChoiceBox> (state, "ottScMode", "SC"));
            toggles.push_back (std::make_unique<ToggleButton> ("CLIP", "ottClip", state));
            toggles.push_back (std::make_unique<ToggleButton> ("SIDECHAIN", "ottSc", state));

            globalKnobs.push_back (std::make_unique<Knob> (state, "ottAmount",   "AMOUNT",     pctText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "ottTime",     "TIME",       pctText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "ottIn",       "IN",         dbText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "ottOut",      "OUT",        dbText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "ottLowTone",  "LOW TONE",   dbText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "ottHighTone", "HIGH TONE",  dbText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "ottLink",     "LINK",       linkText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "ottRes",      "RESOLUTION", pctText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "ottLook",     "LOOKAHEAD",  [] (float v) { return juce::String (v, 2) + " ms"; }));
            globalKnobs.push_back (std::make_unique<Knob> (state, "ottClipLvl",  "CLIP LVL",   dbText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "ottX1",       "LOW/MID",    hzText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "ottX2",       "MID/HIGH",   hzText));

            makeBandPills (3);
            soloToggle   = std::make_unique<ToggleButton> ("SOLO", "ottSolo" + s, state);
            bypassToggle = std::make_unique<ToggleButton> ("BYPASS", "ottByp" + s, state);
            bandKnobs.push_back (std::make_unique<Knob> (state, "ottAbove" + s, "ABOVE",    dbText));
            bandKnobs.push_back (std::make_unique<Knob> (state, "ottDown"  + s, "DOWNWARD", downText));
            bandKnobs.push_back (std::make_unique<Knob> (state, "ottBelow" + s, "BELOW",    dbText));
            bandKnobs.push_back (std::make_unique<Knob> (state, "ottUp"    + s, "UPWARD",   upText));
            bandKnobs.push_back (std::make_unique<Knob> (state, "ottBIn"   + s, "BAND IN",  dbText));
            bandKnobs.push_back (std::make_unique<Knob> (state, "ottBOut"  + s, "BAND OUT", dbText));
            bandKnobs.push_back (std::make_unique<Knob> (state, "ottBAmt"  + s, "AMOUNT",   pctText));
            bandKnobs.push_back (std::make_unique<Knob> (state, "ottAtt"   + s, "ATTACK",   msText));
            bandKnobs.push_back (std::make_unique<Knob> (state, "ottRel"   + s, "RELEASE",  msText));
        }
        else
        {
            const int bands = juce::roundToInt (state.getRawParameterValue ("gateBands")->load()) + 1;
            selectedBand = juce::jlimit (1, bands, selectedBand);
            combos.push_back (std::make_unique<ChoiceBox> (state, "gateEngine", "ENGINE"));
            combos.push_back (std::make_unique<ChoiceBox> (state, "gateBands",  "BANDS"));
            toggles.push_back (std::make_unique<ToggleButton> ("FLIP (DUCK)", "gateFlip", state));
            toggles.push_back (std::make_unique<ToggleButton> ("SIDECHAIN", "gateSc", state));
            toggles.push_back (std::make_unique<ToggleButton> ("NOISE PROFILE", "gateProfile", state));
            actionPills.push_back (std::make_unique<Pill> ([this] { return processor.isNoiseLearning() ? juce::String ("STOP LEARNING") : juce::String ("LEARN NOISE"); },
                [this] { return processor.isNoiseLearning(); },
                [this] { if (processor.isNoiseLearning()) processor.stopNoiseLearn(); else processor.startNoiseLearn(); }));
            actionPills.push_back (std::make_unique<Pill> ([] { return juce::String ("CLEAR NOISE"); }, [] { return false; },
                [this] { processor.clearNoiseProfile(); }));

            globalKnobs.push_back (std::make_unique<Knob> (state, "gateAtt",    "ATTACK",     [] (float v) { return juce::String (v, v < 10.0f ? 2 : 0) + " ms"; }));
            globalKnobs.push_back (std::make_unique<Knob> (state, "gateHold",   "HOLD",       msText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "gateRel",    "RELEASE",    msText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "gateHyst",   "HYSTERESIS", dbText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "gateLook",   "LOOKAHEAD",  [] (float v) { return juce::String (v, 2) + " ms"; }));
            globalKnobs.push_back (std::make_unique<Knob> (state, "gateScHpf",  "SC HPF",     hzText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "gateScLpf",  "SC LPF",     hzText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "gateSmooth", "SMOOTHING",  pctText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "gateX1",     "LOW/MID",    hzText));
            globalKnobs.push_back (std::make_unique<Knob> (state, "gateX2",     "MID/HIGH",   hzText));

            makeBandPills (bands);
            bandKnobs.push_back (std::make_unique<Knob> (state, "gateThr"   + s, "THRESHOLD", dbText));
            bandKnobs.push_back (std::make_unique<Knob> (state, "gateRange" + s, "RANGE",     dbText));
            bandKnobs.push_back (std::make_unique<Knob> (state, "gateRatio" + s, "RATIO",     ratioText));
        }

        if (modulePanel != nullptr) attachModuleControls();
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
    setResizeLimits (1000, 780, 1700, 1200);
    setSize (1100, 880);
}

TemperAudioProcessorEditor::~TemperAudioProcessorEditor()
{
    impl.reset();
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
