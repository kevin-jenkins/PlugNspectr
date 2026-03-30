/*
  ==============================================================================
    PlugNspectrTheme.h  —  "Deep Space" design tokens + custom LookAndFeel
  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

namespace PnsTheme
{
    //──────────────────────────────────────────────────────────────────────────
    // Backgrounds
    static const juce::Colour kBgDark   { 15,  15,  15  };   // #0F0F0F
    static const juce::Colour kBgPanel  { 26,  26,  26  };   // #1A1A1A
    static const juce::Colour kBgWidget { 36,  36,  36  };   // #242424

    //──────────────────────────────────────────────────────────────────────────
    // Borders
    static const juce::Colour kBorderSubtle { 42, 42, 42 };  // #2A2A2A
    static const juce::Colour kBorderActive { 61, 61, 61 };  // #3D3D3D

    //──────────────────────────────────────────────────────────────────────────
    // Text
    static const juce::Colour kTextPrimary   { 224, 219, 208 };  // warm off-white
    static const juce::Colour kTextSecondary { 107, 101,  96 };  // muted warm grey
    static const juce::Colour kTextAccent    {   0, 229, 204 };  // bright teal

    //──────────────────────────────────────────────────────────────────────────
    // Signal colours
    static const juce::Colour kColorPre      { 123, 111, 204 };  // lavender
    static const juce::Colour kColorPost     {   0, 229, 204 };  // bright teal
    static const juce::Colour kColorPreAvg   { 155, 143, 232 };  // lighter lavender
    static const juce::Colour kColorPostAvg  { 255, 179,   0 };  // vibrant amber
    static const juce::Colour kColorGainRed  { 255, 107,   0 };  // bright orange

    // Per-signal opacity constants
    static constexpr float kColorPreAlpha  = 0.45f;
    static constexpr float kColorPostAlpha = 0.95f;

    //──────────────────────────────────────────────────────────────────────────
    // Harmonics gradient (H2=cyan → H8=pink)
    static const juce::Colour kColorHarmLow  {   0, 255, 255 };  // electric cyan
    static const juce::Colour kColorHarmHigh { 255,   0, 255 };  // electric pink

    //──────────────────────────────────────────────────────────────────────────
    // Accents (derived from post teal for LookAndFeel)
    static const juce::Colour kAccentPrimary   {   0, 229, 204 };  // bright teal
    static const juce::Colour kAccentSecondary {   0, 180, 160 };  // mid teal
    static const juce::Colour kAccentDim       {   0,  70,  60 };  // dark teal
    static const juce::Colour kBtnActiveBg     {   0,  40,  36 };  // very dark teal

    //──────────────────────────────────────────────────────────────────────────
    // Grid & reference lines
    static const juce::Colour kGridLine  {  30,  30,  30 };   // #1E1E1E very subtle
    static const juce::Colour kGridLabel {  74,  69,  64 };   // #4A4540 muted warm
    static const juce::Colour kZeroLine  {  46,  46,  46 };   // #2E2E2E
    static const juce::Colour kClipLine  { 204,  51,  51 };   // #CC3333 soft red

    //──────────────────────────────────────────────────────────────────────────
    // Spacing  (px)
    static constexpr int kHeaderHeight    = 36;
    static constexpr int kTabBarHeight    = 32;
    static constexpr int kPaddingSmall    = 6;
    static constexpr int kPaddingMid      = 12;
    static constexpr int kPaddingLarge    = 20;
    static constexpr int kCornerRadius    = 4;
    static constexpr int kButtonHeight    = 22;

    //──────────────────────────────────────────────────────────────────────────
    // Fonts
    static inline juce::Font fontPrimary()
    {
        return juce::Font (juce::FontOptions().withName (juce::Font::getDefaultSansSerifFontName()).withHeight (12.0f));
    }
    static inline juce::Font fontLabel()
    {
        return juce::Font (juce::FontOptions().withName (juce::Font::getDefaultSansSerifFontName()).withHeight (10.0f));
    }
    static inline juce::Font fontTitle()
    {
        return juce::Font (juce::FontOptions().withName (juce::Font::getDefaultSansSerifFontName()).withHeight (11.0f).withStyle ("Bold"));
    }
    static inline juce::Font fontReadout()
    {
        return juce::Font (juce::FontOptions().withName (juce::Font::getDefaultSansSerifFontName()).withHeight (13.0f));
    }

    //──────────────────────────────────────────────────────────────────────────
    // Frosted panel
    static inline void drawFrostedPanel (juce::Graphics& g, juce::Rectangle<float> bounds)
    {
        g.setColour (juce::Colour (15, 15, 15).withAlpha (0.82f));
        g.fillRoundedRectangle (bounds, (float) kCornerRadius);
        g.setColour (juce::Colour (42, 42, 42).withAlpha (0.9f));
        g.drawRoundedRectangle (bounds, (float) kCornerRadius, 1.0f);
    }

    //──────────────────────────────────────────────────────────────────────────
    // Dotted grid lines
    static inline void drawDottedHLine (juce::Graphics& g, float y, float x1, float x2)
    {
        const float dashes[] = { 3.0f, 6.0f };
        juce::Path src;
        src.startNewSubPath (x1, y);
        src.lineTo (x2, y);
        juce::Path dashed;
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, src, dashes, 2);
        g.fillPath (dashed);
    }

    static inline void drawDottedVLine (juce::Graphics& g, float x, float y1, float y2)
    {
        const float dashes[] = { 3.0f, 6.0f };
        juce::Path src;
        src.startNewSubPath (x, y1);
        src.lineTo (x, y2);
        juce::Path dashed;
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, src, dashes, 2);
        g.fillPath (dashed);
    }

    //──────────────────────────────────────────────────────────────────────────
    // Glow fill — draws a filled area under a spectrum curve at given opacity.
    // curvePath: open path from left to right; rightX/bottomY close the shape.
    static inline void drawGlowFill (juce::Graphics& g, const juce::Path& curvePath,
                                     juce::Colour colour, float rightX, float bottomY,
                                     float alpha = 0.15f)
    {
        const float leftX = curvePath.getBounds().getX();
        juce::Path fillPath = curvePath;
        fillPath.lineTo (rightX, bottomY);
        fillPath.lineTo (leftX,  bottomY);
        fillPath.closeSubPath();
        g.setColour (colour.withAlpha (alpha));
        g.fillPath (fillPath);
    }

    //──────────────────────────────────────────────────────────────────────────
    // Glow line — three-pass stroke (wide dim outer → narrow bright core).
    static inline void drawGlowLine (juce::Graphics& g, const juce::Path& path,
                                     juce::Colour colour, float lineThickness = 1.5f)
    {
        g.setColour (colour.withAlpha (0.12f));
        g.strokePath (path, juce::PathStrokeType (lineThickness + 5.0f));
        g.setColour (colour.withAlpha (0.28f));
        g.strokePath (path, juce::PathStrokeType (lineThickness + 2.5f));
        g.setColour (colour.withAlpha (0.95f));
        g.strokePath (path, juce::PathStrokeType (lineThickness));
    }

}  // namespace PnsTheme

//==============================================================================
// PnsLookAndFeel
//==============================================================================
class PnsLookAndFeel : public juce::LookAndFeel_V4
{
public:
    PnsLookAndFeel()
    {
        setColour (juce::ComboBox::textColourId,               PnsTheme::kTextPrimary);
        setColour (juce::ComboBox::backgroundColourId,         PnsTheme::kBgWidget);
        setColour (juce::ComboBox::arrowColourId,              PnsTheme::kTextSecondary);
        setColour (juce::ComboBox::outlineColourId,            PnsTheme::kBorderSubtle);
        setColour (juce::PopupMenu::backgroundColourId,        PnsTheme::kBgPanel);
        setColour (juce::PopupMenu::textColourId,              PnsTheme::kTextPrimary);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, PnsTheme::kAccentDim);
        setColour (juce::PopupMenu::highlightedTextColourId,   PnsTheme::kAccentPrimary);
        setColour (juce::Slider::trackColourId,                PnsTheme::kBorderSubtle);
        setColour (juce::Slider::thumbColourId,                PnsTheme::kAccentPrimary);
    }

    //──────────────────────────────────────────────────────────────────────────
    void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                               const juce::Colour& /*bg*/,
                               bool highlighted, bool down) override
    {
        if (button.getProperties().contains ("tabButton"))
        {
            if ((bool) button.getProperties().getWithDefault ("tabActive", false))
            {
                // Active tab: bright teal underline + dim glow beneath
                g.setColour (PnsTheme::kAccentPrimary.withAlpha (0.30f));
                g.fillRect (0, button.getHeight() - 4, button.getWidth(), 4);
                g.setColour (PnsTheme::kAccentPrimary);
                g.fillRect (0, button.getHeight() - 2, button.getWidth(), 2);
            }
            return;
        }

        const bool active = button.getToggleState() || down;
        const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        const float r     = (float) PnsTheme::kCornerRadius;

        g.setColour (active ? PnsTheme::kBtnActiveBg : PnsTheme::kBgWidget);
        g.fillRoundedRectangle (bounds, r);

        const auto borderCol = active      ? PnsTheme::kAccentPrimary
                             : highlighted ? PnsTheme::kBorderActive
                                           : PnsTheme::kBorderSubtle;
        g.setColour (borderCol);
        g.drawRoundedRectangle (bounds, r, 1.0f);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& button,
                         bool highlighted, bool down) override
    {
        if (button.getProperties().contains ("tabButton"))
        {
            g.setFont (PnsTheme::fontPrimary());
            const bool tabActive = (bool) button.getProperties().getWithDefault ("tabActive", false);
            g.setColour ((tabActive || highlighted) ? PnsTheme::kTextPrimary
                                                    : PnsTheme::kTextSecondary);
            g.drawText (button.getButtonText(), button.getLocalBounds(),
                        juce::Justification::centred);
            return;
        }

        const bool active = button.getToggleState() || down;
        g.setFont (PnsTheme::fontLabel());
        g.setColour (active ? PnsTheme::kAccentPrimary : PnsTheme::kTextSecondary);
        g.drawText (button.getButtonText(),
                    button.getLocalBounds().reduced (PnsTheme::kPaddingSmall, 0),
                    juce::Justification::centred);
    }

    //──────────────────────────────────────────────────────────────────────────
    void drawComboBox (juce::Graphics& g, int w, int h, bool,
                       int arrowX, int arrowY, int arrowW, int arrowH,
                       juce::ComboBox&) override
    {
        const auto b = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
        g.setColour (PnsTheme::kBgWidget);
        g.fillRoundedRectangle (b, (float) PnsTheme::kCornerRadius);
        g.setColour (PnsTheme::kBorderSubtle);
        g.drawRoundedRectangle (b, (float) PnsTheme::kCornerRadius, 1.0f);

        const float cx = (float) arrowX + (float) arrowW * 0.5f;
        const float cy = (float) arrowY + (float) arrowH * 0.5f - 1.0f;
        const float cs = 4.0f;
        g.setColour (PnsTheme::kTextSecondary);
        g.drawLine (cx - cs, cy - cs * 0.45f, cx, cy + cs * 0.45f, 1.0f);
        g.drawLine (cx, cy + cs * 0.45f, cx + cs, cy - cs * 0.45f, 1.0f);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override
    {
        return PnsTheme::fontLabel();
    }

    //──────────────────────────────────────────────────────────────────────────
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           const juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style == juce::Slider::LinearHorizontal)
        {
            const float trackY = (float) y + (float) height * 0.5f;

            // Track
            g.setColour (PnsTheme::kBorderSubtle);
            g.fillRect (juce::Rectangle<float> ((float) x, trackY - 1.0f, (float) width, 2.0f));

            // Thumb
            constexpr float tw = 8.0f, th = 16.0f;
            const float tx = sliderPos - tw * 0.5f;
            const float ty = trackY - th * 0.5f;
            g.setColour (PnsTheme::kBgWidget);
            g.fillRoundedRectangle (tx, ty, tw, th, 2.0f);
            g.setColour (PnsTheme::kAccentPrimary);
            g.drawRoundedRectangle (tx + 0.5f, ty + 0.5f, tw - 1.0f, th - 1.0f, 2.0f, 1.0f);
        }
        else
        {
            // Fallback for any other style — delegate to JUCE default with correct args
            juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height,
                                                    sliderPos, minSliderPos, maxSliderPos,
                                                    style, slider);
        }
    }

    //──────────────────────────────────────────────────────────────────────────
    // FabFilter-inspired flat ring knob: dark disc + full-range track arc +
    // filled value arc in teal.
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                           float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override
    {
        const float cx    = (float) x + (float) w * 0.5f;
        const float cy    = (float) y + (float) h * 0.5f;
        const float r     = juce::jmin (w, h) * 0.5f - 2.0f;
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        constexpr float arcInset   = 4.0f;
        const float arcR = r - arcInset;

        // Dark disc background
        g.setColour (PnsTheme::kBgWidget);
        g.fillEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);

        // Full-range track arc (BORDER_ACTIVE, thin)
        {
            juce::Path track;
            track.addArc (cx - arcR, cy - arcR, arcR * 2.0f, arcR * 2.0f,
                          rotaryStartAngle, rotaryEndAngle, true);
            g.setColour (PnsTheme::kBorderActive);
            g.strokePath (track, juce::PathStrokeType (2.0f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Value arc (COLOR_POST teal, slightly thicker)
        if (sliderPos > 0.0f)
        {
            juce::Path fill;
            fill.addArc (cx - arcR, cy - arcR, arcR * 2.0f, arcR * 2.0f,
                         rotaryStartAngle, angle, true);
            // Glow outer
            g.setColour (PnsTheme::kColorPost.withAlpha (0.20f));
            g.strokePath (fill, juce::PathStrokeType (5.0f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            // Core bright
            g.setColour (PnsTheme::kColorPost);
            g.strokePath (fill, juce::PathStrokeType (2.5f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    //──────────────────────────────────────────────────────────────────────────
    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.fillAll (PnsTheme::kBgPanel);
        g.setColour (PnsTheme::kBorderSubtle);
        g.drawRect (0, 0, w, h, 1);
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted,
                            bool /*isTicked*/, bool /*hasSubMenu*/,
                            const juce::String& text, const juce::String&,
                            const juce::Drawable*, const juce::Colour*) override
    {
        if (isSeparator)
        {
            g.setColour (PnsTheme::kBorderSubtle);
            g.fillRect (area.getX() + 6, area.getCentreY(), area.getWidth() - 12, 1);
            return;
        }

        if (isHighlighted)
        {
            g.setColour (PnsTheme::kAccentDim);
            g.fillRect (area);
        }

        g.setFont (PnsTheme::fontLabel());
        g.setColour (isHighlighted ? PnsTheme::kAccentPrimary
                     : isActive    ? PnsTheme::kTextPrimary
                                   : PnsTheme::kTextSecondary);
        g.drawText (text, area.reduced (8, 0), juce::Justification::centredLeft);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PnsLookAndFeel)
};
