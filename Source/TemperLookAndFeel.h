#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// A holographic dark theme: near-black background, iridescent accent, soft
// glowing knob arcs and bordered panels — the shared Dewpoint AV look.
class TemperLookAndFeel : public juce::LookAndFeel_V4
{
public:
    TemperLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, background);
        setColour (juce::Slider::rotarySliderFillColourId,   accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, track);
        setColour (juce::Slider::thumbColourId,              accent);
        setColour (juce::Slider::textBoxTextColourId,        text);
        setColour (juce::Slider::textBoxOutlineColourId,     juce::Colours::transparentBlack);

        setColour (juce::ComboBox::backgroundColourId, panel);
        setColour (juce::ComboBox::outlineColourId,     border);
        setColour (juce::ComboBox::textColourId,        text);
        setColour (juce::ComboBox::arrowColourId,       accent);
        setColour (juce::ComboBox::buttonColourId,      panel);
        setColour (juce::ComboBox::focusedOutlineColourId, accent);

        setColour (juce::Label::textColourId, text);
        setColour (juce::Label::textWhenEditingColourId, text);

        setColour (juce::PopupMenu::backgroundColourId,        panel);
        setColour (juce::PopupMenu::textColourId,              text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.35f));
        setColour (juce::PopupMenu::highlightedTextColourId,   juce::Colours::white);
    }

    // --- palette -----------------------------------------------------------
    static inline const juce::Colour background { 0xff07090d };
    static inline const juce::Colour panel      { 0xff10141c };
    static inline const juce::Colour border     { 0xff1c2331 };
    static inline const juce::Colour track      { 0xff232b3a };
    static inline const juce::Colour accent     { 0xff6ea8fe };   // holographic blue
    static inline const juce::Colour accent2    { 0xff7a5cff };   // iridescent violet
    static inline const juce::Colour text       { 0xffe6eaf2 };
    static inline const juce::Colour muted      { 0xff7d8496 };

    // --- rotary knobs (holographic iridescent dial) -------------------------
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& slider) override
    {
        const float cx = static_cast<float> (x) + width  * 0.5f;
        const float cy = static_cast<float> (y) + height * 0.5f;
        const float radius = juce::jmin (static_cast<float> (width), static_cast<float> (height)) * 0.5f - 4.0f;
        if (radius <= 1.0f)
            return;
        const float valueAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        const bool  hover = slider.isMouseOverOrDragging();

        const juce::Colour blue   (0xff6ea8fe);
        const juce::Colour purple (0xff8b5cf6);
        const juce::Colour red    (0xffff2a2a);

        // Soft outer glow ring.
        g.setColour (accent.withAlpha (hover ? 0.24f : 0.12f));
        g.drawEllipse (cx - radius - 3.0f, cy - radius - 3.0f, (radius + 3.0f) * 2.0f, (radius + 3.0f) * 2.0f, 3.5f);

        // Track ring.
        g.setColour (track);
        g.drawEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 2.0f);

        // Tick marks around the dial.
        const int numTicks = 12;
        for (int i = 0; i <= numTicks; ++i)
        {
            const float ang = rotaryStartAngle + (static_cast<float> (i) / numTicks) * (rotaryEndAngle - rotaryStartAngle);
            g.setColour (muted.withAlpha (0.5f));
            g.drawLine (cx + (radius + 3.5f) * std::sin (ang), cy - (radius + 3.5f) * std::cos (ang),
                        cx + (radius + 6.5f) * std::sin (ang), cy - (radius + 6.5f) * std::cos (ang), 1.0f);
        }

        // Draw the value arc as short coloured segments — avoids stroked-path
        // construction and gradient fills, which are fragile on Windows.
        const float sweep = valueAngle - rotaryStartAngle;
        const int segs = juce::jmax (1, juce::roundToInt (std::fabs (sweep) / 0.45f));
        for (int s = 0; s < segs; ++s)
        {
            const float t0 = static_cast<float> (s) / segs;
            const float t1 = static_cast<float> (s + 1) / segs;
            const float mid = (t0 + t1) * 0.5f;
            const juce::Colour c = mid < 0.5f ? blue.interpolatedWith  (purple, mid * 2.0f)
                                              : purple.interpolatedWith (red,    (mid - 0.5f) * 2.0f);

            juce::Path seg;
            seg.addCentredArc (cx, cy, radius, radius, 0.0f,
                               rotaryStartAngle + sweep * t0, rotaryStartAngle + sweep * t1, true);
            g.setColour (c.withAlpha (0.28f));
            g.strokePath (seg, juce::PathStrokeType (7.0f, juce::PathStrokeType::beveled, juce::PathStrokeType::butt));
            g.setColour (c);
            g.strokePath (seg, juce::PathStrokeType (3.2f, juce::PathStrokeType::beveled, juce::PathStrokeType::butt));
        }

        // Glowing needle.
        const float nx = std::sin (valueAngle);
        const float ny = -std::cos (valueAngle);
        g.setColour (accent.withAlpha (0.45f));
        g.drawLine (cx, cy, cx + radius * nx, cy + radius * ny, 5.0f);
        g.setColour (juce::Colours::white.withAlpha (0.95f));
        g.drawLine (cx, cy, cx + radius * nx, cy + radius * ny, 1.8f);

        // Center "lens" (covers the needle's origin).
        g.setColour (background.brighter (0.22f));
        g.fillEllipse (cx - radius * 0.40f, cy - radius * 0.40f, radius * 0.80f, radius * 0.80f);

        // Bright cap at the needle tip + centre dot.
        g.setColour (accent.withAlpha (0.35f));
        g.fillEllipse (cx + radius * nx - 5.0f, cy + radius * ny - 5.0f, 10.0f, 10.0f);
        g.setColour (juce::Colours::white);
        g.fillEllipse (cx + radius * nx - 3.0f, cy + radius * ny - 3.0f, 6.0f, 6.0f);

        g.setColour (accent);
        g.fillEllipse (cx - 2.0f, cy - 2.0f, 4.0f, 4.0f);
    }

    // --- combo box ---------------------------------------------------------
    void drawComboBox (juce::Graphics& g, int width, int height, bool /*isButtonDown*/,
                       int /*buttonX*/, int /*buttonY*/, int /*buttonW*/, int /*buttonH*/,
                       juce::ComboBox& box) override
    {
        const juce::Rectangle<int> r (0, 0, width, height);
        g.setColour (panel);
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
        g.setColour (box.hasKeyboardFocus (true) ? accent : border);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 6.0f, 1.5f);

        const float cx = static_cast<float> (width)  - 16.0f;
        const float cy = static_cast<float> (height) * 0.5f;
        juce::Path arrow;
        arrow.addTriangle (cx - 4.0f, cy - 3.0f, cx + 4.0f, cy - 3.0f, cx, cy + 3.0f);
        g.setColour (accent);
        g.fillPath (arrow);
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (6, 1, box.getWidth() - 26, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override
    {
        return juce::Font (14.0f, juce::Font::bold);
    }

    juce::Font getLabelFont (juce::Label&) override
    {
        return juce::Font (11.0f, juce::Font::plain);
    }

    static void drawPanelBackground (juce::Graphics& g, juce::Rectangle<int> bounds)
    {
        g.setColour (panel);
        g.fillRoundedRectangle (bounds.toFloat(), 10.0f);
        g.setColour (border);
        g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), 10.0f, 1.0f);
    }
};
