/*
  ==============================================================================
    PlugNspectrTheme.h  —  design tokens and custom LookAndFeel
    All colours, fonts, and spacing constants live here.
  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

namespace PnsTheme
{
    //──────────────────────────────────────────────────────────────────────────
    // Backgrounds
    static const juce::Colour kBgDark   { 22,  20,  18  };   // main background
    static const juce::Colour kBgPanel  { 30,  28,  25  };   // header / panels
    static const juce::Colour kBgWidget { 38,  35,  31  };   // buttons, controls
    static const juce::Colour kBgPlot   { 26,  24,  22  };   // analysis area (same as Dark here)

    //──────────────────────────────────────────────────────────────────────────
    // Borders
    static const juce::Colour kBorderSubtle { 55,  50,  44 };
    static const juce::Colour kBorderActive { 80,  75,  68 };

    //──────────────────────────────────────────────────────────────────────────
    // Text
    static const juce::Colour kTextPrimary   { 220, 215, 205 };
    static const juce::Colour kTextSecondary { 140, 135, 125 };
    static const juce::Colour kTextAccent    {  90, 210, 190 };

    //──────────────────────────────────────────────────────────────────────────
    // Accents
    static const juce::Colour kAccentPrimary   {  90, 210, 190 };
    static const juce::Colour kAccentSecondary {  60, 180, 200 };
    static const juce::Colour kAccentDim       {  50, 120, 110 };
    static const juce::Colour kBtnActiveBg     {  35,  50,  48 };   // active button fill

    //──────────────────────────────────────────────────────────────────────────
    // Signal colours
    static const juce::Colour kColorPre     { 140, 120, 220 };   // lavender  — PRE curve
    static const juce::Colour kColorPost    {  90, 210, 190 };   // teal      — POST curve
    static const juce::Colour kColorPreAvg  { 180, 160, 255 };   // light lavender — PRE avg
    static const juce::Colour kColorPostAvg { 255, 200,  80 };   // amber     — POST avg
    static const juce::Colour kColorGainRed { 255, 160,  60 };   // warm orange — GR

    //──────────────────────────────────────────────────────────────────────────
    // Grid & reference lines
    static const juce::Colour kGridLine  {  45,  42,  38 };
    static const juce::Colour kGridLabel { 100,  96,  88 };
    static const juce::Colour kZeroLine  {  80,  76,  70 };
    static const juce::Colour kClipLine  { 200,  80,  80 };

    //──────────────────────────────────────────────────────────────────────────
    // Spacing  (px)
    static constexpr int kHeaderHeight = 36;
    static constexpr int kTabBarHeight = 32;
    static constexpr int kPaddingSmall = 6;
    static constexpr int kPaddingMid   = 12;
    static constexpr int kPaddingLarge = 20;
    static constexpr int kCornerRadius = 4;
    static constexpr int kButtonHeight = 22;

    //──────────────────────────────────────────────────────────────────────────
    // Fonts — Helvetica Neue preferred; system sans-serif as fallback
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
    // Reusable frosted panel — semi-transparent dark background + subtle border.
    // Draw before any overlaid text/legend so content remains legible.
    static inline void drawFrostedPanel (juce::Graphics& g, juce::Rectangle<float> bounds)
    {
        g.setColour (juce::Colour (22, 20, 18).withAlpha (0.82f));
        g.fillRoundedRectangle (bounds, (float) kCornerRadius);
        g.setColour (juce::Colour (55, 50, 44).withAlpha (0.9f));
        g.drawRoundedRectangle (bounds, (float) kCornerRadius, 1.0f);
    }
}  // namespace PnsTheme

//==============================================================================
// PnsLookAndFeel — custom styling for all plugin controls
//==============================================================================
class PnsLookAndFeel : public juce::LookAndFeel_V4
{
public:
    PnsLookAndFeel()
    {
        // Global colour overrides used by standard JUCE components
        setColour (juce::ComboBox::textColourId,               PnsTheme::kTextPrimary);
        setColour (juce::ComboBox::backgroundColourId,         PnsTheme::kBgWidget);
        setColour (juce::ComboBox::arrowColourId,              PnsTheme::kTextSecondary);
        setColour (juce::ComboBox::outlineColourId,            PnsTheme::kBorderSubtle);
        setColour (juce::PopupMenu::backgroundColourId,        PnsTheme::kBgWidget);
        setColour (juce::PopupMenu::textColourId,              PnsTheme::kTextPrimary);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, PnsTheme::kBtnActiveBg);
        setColour (juce::PopupMenu::highlightedTextColourId,   PnsTheme::kAccentPrimary);
    }

    //──────────────────────────────────────────────────────────────────────────
    void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                               const juce::Colour& /*bg*/,
                               bool highlighted, bool down) override
    {
        // Tab buttons: background handled by the editor; draw only the underline
        if (button.getProperties().contains ("tabButton"))
        {
            if ((bool) button.getProperties().getWithDefault ("tabActive", false))
            {
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
                         bool /*highlighted*/, bool down) override
    {
        // Tab buttons: text colour set externally via setColour in switchTab()
        if (button.getProperties().contains ("tabButton"))
        {
            g.setFont (PnsTheme::fontPrimary());
            g.setColour (button.findColour (juce::TextButton::textColourOffId));
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

        // Minimal chevron (∨) drawn with two lines
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
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                           float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override
    {
        const float cx    = x + w * 0.5f;
        const float cy    = y + h * 0.5f;
        const float r     = juce::jmin (w, h) * 0.5f - 3.0f;
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        // Background disc
        g.setColour (PnsTheme::kBgWidget);
        g.fillEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);

        // Border
        g.setColour (PnsTheme::kBorderSubtle);
        g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, 1.0f);

        // Arc from start to current position
        juce::Path arc;
        arc.addArc (cx - r + 4, cy - r + 4, (r - 4) * 2.0f, (r - 4) * 2.0f,
                    rotaryStartAngle, angle, true);
        g.setColour (PnsTheme::kAccentPrimary);
        g.strokePath (arc, juce::PathStrokeType (2.0f));

        // Indicator line
        const float ix = cx + (r - 6.0f) * std::sin (angle);
        const float iy = cy - (r - 6.0f) * std::cos (angle);
        g.setColour (PnsTheme::kAccentPrimary);
        g.drawLine (cx, cy, ix, iy, 2.0f);

        // Centre dot
        g.setColour (PnsTheme::kBorderActive);
        g.fillEllipse (cx - 2.5f, cy - 2.5f, 5.0f, 5.0f);
    }

    //──────────────────────────────────────────────────────────────────────────
    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.fillAll (PnsTheme::kBgWidget);
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
            g.setColour (PnsTheme::kBtnActiveBg);
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
