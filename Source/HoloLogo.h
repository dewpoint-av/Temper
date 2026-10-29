#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

#include "BinaryData.h"

// Monument Extended typefaces, embedded via juce_add_binary_data.
inline juce::Typeface::Ptr monumentTypeface (bool bold)
{
    static juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (
        BinaryData::MonumentExtendedRegular_otf, BinaryData::MonumentExtendedRegular_otfSize);
    static juce::Typeface::Ptr ultrabold = juce::Typeface::createSystemTypefaceFor (
        BinaryData::MonumentExtendedUltrabold_otf, BinaryData::MonumentExtendedUltrabold_otfSize);
    return bold ? ultrabold : regular;
}

inline juce::Font monumentFont (float height, bool bold = true)
{
    return juce::FontOptions (monumentTypeface (bold)).withHeight (height);
}

// An animated, holographic title rendered in Monument Extended. The glyph
// outline is laid out once and cached; only the gradient colours animate.
// This keeps the per-frame Direct2D work minimal (important on Windows, where
// JUCE 8 renders through Direct2D).
class HoloTitle : public juce::Component,
                  private juce::Timer
{
public:
    HoloTitle (const juce::String& titleText, float height, bool useBold = true)
        : text (titleText), fontHeight (height), bold (useBold)
    {
        startTimerHz (15);
    }

    ~HoloTitle() override { stopTimer(); }

    void timerCallback() override
    {
        phase += 0.012f;
        if (phase > 1.0f)
            phase -= 1.0f;
        repaint();
    }

    void resized() override { rebuildPath(); }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        if (glyphPath.isEmpty())
            rebuildPath();
        if (glyphPath.isEmpty())
            return;

        // Website palette: blue -> purple -> red (no green / cyan).
        const auto palette = [] (float p)
        {
            const juce::Colour blue   (0xff6ea8fe);
            const juce::Colour purple (0xff8b5cf6);
            const juce::Colour red    (0xffff2a2a);
            p = std::fmod (p, 1.0f);
            return (p < 0.5f) ? blue.interpolatedWith  (purple, p * 2.0f)
                              : purple.interpolatedWith (red, (p - 0.5f) * 2.0f);
        };
        const juce::Colour a = palette (phase);
        const juce::Colour c = palette (phase + 0.33f);

        // Soft pulsing glow.
        const float glowAlpha = 0.22f + 0.14f * (0.5f + 0.5f * std::sin (phase * 6.2831853f));
        g.setColour (a.withAlpha (glowAlpha));
        g.strokePath (glyphPath, juce::PathStrokeType (7.0f, juce::PathStrokeType::beveled, juce::PathStrokeType::butt));

        // Iridescent gradient fill.
        g.setGradientFill (juce::ColourGradient (a, b.getX(), b.getY(), c, b.getRight(), b.getBottom(), false));
        g.fillPath (glyphPath);
    }

private:
    void rebuildPath()
    {
        const auto b = getLocalBounds().toFloat();
        if (b.getWidth() <= 0.0f || b.getHeight() <= 0.0f)
            return;

        juce::GlyphArrangement glyphs;
        glyphs.addFittedText (monumentFont (fontHeight, bold), text,
                              b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                              juce::Justification::centred, 1, 0.9f);
        glyphPath.clear();   // createPath() appends, so clear first to avoid stacking glyphs
        glyphs.createPath (glyphPath);
    }

    juce::String text;
    float fontHeight;
    bool bold;
    float phase = 0.0f;
    juce::Path glyphPath;
};
