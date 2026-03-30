/*
  ==============================================================================
    PlugNspectrPre  –  PluginEditor.cpp
  ==============================================================================
*/

#include "PluginEditor.h"
// Share the Deep Space theme from the Post plugin (same repository)
#include "../../PlugNspectrPost/Source/PlugNspectrTheme.h"

#include <cmath>

//==============================================================================
PlugNspectrPreEditor::PlugNspectrPreEditor (PlugNspectrPreProcessor& p)
    : AudioProcessorEditor (&p), m_proc (p)
{
    setSize (400, 200);
    startTimer (500);   // check Post connection every 500ms
}

PlugNspectrPreEditor::~PlugNspectrPreEditor()
{
    stopTimer();
}

//==============================================================================
void PlugNspectrPreEditor::timerCallback()
{
    const bool connected = m_proc.isPostConnected();
    if (connected != m_postConnected)
    {
        m_postConnected = connected;
        repaint();
    }
}

//==============================================================================
void PlugNspectrPreEditor::paint (juce::Graphics& g)
{
    constexpr int hH = PnsTheme::kHeaderHeight;  // 36px
    const int W = getWidth(), H = getHeight();

    // ── Background ────────────────────────────────────────────────────────
    g.fillAll (PnsTheme::kBgDark);

    // ── Header bar ────────────────────────────────────────────────────────
    g.setColour (PnsTheme::kBgPanel);
    g.fillRect (0, 0, W, hH);
    g.setColour (PnsTheme::kBorderSubtle);
    g.drawHorizontalLine (hH - 1, 0.0f, (float) W);

    // Header title — same kerned treatment as Post
    {
        const juce::Font titleFont = juce::Font (juce::FontOptions()
            .withName (juce::Font::getDefaultSansSerifFontName())
            .withHeight (16.0f));
        g.setFont (titleFont);
        g.setColour (PnsTheme::kTextPrimary);
        const juce::String titleStr = "PlugNspectr Pre";
        constexpr float kExtraKern = 2.5f;
        const float baselineY = (float) hH * 0.5f + titleFont.getAscent() * 0.5f - 1.0f;
        float tx = (float) PnsTheme::kPaddingMid;
        for (int i = 0; i < titleStr.length(); ++i)
        {
            const juce::String ch = titleStr.substring (i, i + 1);
            g.drawSingleLineText (ch, juce::roundToInt (tx), juce::roundToInt (baselineY));
            juce::GlyphArrangement ga;
            ga.addLineOfText (titleFont, ch, 0.0f, 0.0f);
            tx += (ga.getNumGlyphs() > 0
                    ? ga.getBoundingBox (0, 1, true).getWidth()
                    : 7.0f) + kExtraKern;
        }
    }

    // Version label
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("v1.0", W - PnsTheme::kPaddingMid - 36, (hH - 11) / 2, 36, 11,
                juce::Justification::centredRight);

    // ── Content area ──────────────────────────────────────────────────────
    const int cY = hH;
    const int cH = H - hH;

    // Frosted instruction panel — centred in content area
    constexpr float panelW = 376.0f;
    constexpr float panelH = 130.0f;
    const float panelX = (W - panelW) * 0.5f;
    const float panelY = cY + (cH - panelH) * 0.5f;

    PnsTheme::drawFrostedPanel (g, { panelX, panelY, panelW, panelH });

    constexpr float pad = 12.0f;
    float cy = panelY + pad;

    // Primary message
    g.setFont (PnsTheme::fontTitle());
    g.setColour (PnsTheme::kTextPrimary);
    g.drawText ("PlugNspectr Pre is active",
                (int) panelX, (int) cy, (int) panelW, 13,
                juce::Justification::centred);
    cy += 17.0f;

    // Secondary message
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("Insert PlugNspectr Post after the plugin you want to analyze",
                (int) panelX, (int) cy, (int) panelW, 11,
                juce::Justification::centred);
    cy += 13.0f;
    g.drawText ("to begin inspection.",
                (int) panelX, (int) cy, (int) panelW, 11,
                juce::Justification::centred);
    cy += 18.0f;

    // ── Signal chain diagram ──────────────────────────────────────────────
    constexpr float bH  = 20.0f;
    constexpr float bW1 = 42.0f;   // Pre block
    constexpr float bW2 = 86.0f;   // Your Plugin block
    constexpr float bW3 = 48.0f;   // Post block
    constexpr float arrW = 16.0f;
    const float chainTotalW = bW1 + arrW + bW2 + arrW + bW3;
    const float chainX = panelX + (panelW - chainTotalW) * 0.5f;

    auto drawBlock = [&] (float bx, float bw, juce::Colour border, const char* lbl)
    {
        g.setColour (PnsTheme::kBgWidget);
        g.fillRoundedRectangle (bx, cy, bw, bH, 3.0f);
        g.setColour (border);
        g.drawRoundedRectangle (bx + 0.5f, cy + 0.5f, bw - 1.0f, bH - 1.0f, 3.0f, 1.0f);
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText (lbl, (int) bx, (int) cy, (int) bw, (int) bH, juce::Justification::centred);
    };

    auto drawArrow = [&] (float ax)
    {
        const float ay = cy + bH * 0.5f;
        g.setColour (PnsTheme::kTextSecondary);
        g.drawLine (ax + 1.0f, ay, ax + arrW - 5.0f, ay, 1.0f);
        juce::Path head;
        const float hx = ax + arrW - 5.0f;
        head.addTriangle (hx, ay - 3.5f, hx + 5.0f, ay, hx, ay + 3.5f);
        g.fillPath (head);
    };

    drawBlock (chainX,                         bW1, PnsTheme::kColorPost,    "Pre");
    drawArrow (chainX + bW1);
    drawBlock (chainX + bW1 + arrW,            bW2, PnsTheme::kBorderActive, "Your Plugin");
    drawArrow (chainX + bW1 + arrW + bW2);
    drawBlock (chainX + bW1 + arrW + bW2 + arrW, bW3, PnsTheme::kColorPostAvg, "Post");
    cy += bH + 10.0f;

    // ── Connection status indicator ───────────────────────────────────────
    const juce::Colour dotCol = m_postConnected
        ? juce::Colour (0, 200, 100)          // green
        : PnsTheme::kColorPostAvg;             // amber

    constexpr float dotR = 4.0f;
    const float statusW = 140.0f;
    const float statusX = panelX + (panelW - statusW) * 0.5f;
    const float dotCY   = cy + dotR;

    g.setColour (dotCol);
    g.fillEllipse (statusX, dotCY - dotR, dotR * 2.0f, dotR * 2.0f);

    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    const char* statusStr = m_postConnected
        ? "Connected to Post"
        : "Waiting for Post plugin...";
    g.drawText (statusStr,
                (int) (statusX + dotR * 2.0f + 5.0f), (int) (dotCY - 5.5f),
                (int) (statusW - dotR * 2.0f - 5.0f), 11,
                juce::Justification::centredLeft);
}
