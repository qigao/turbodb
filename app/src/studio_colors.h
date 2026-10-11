#pragma once

#include <windows.h>

namespace turbodb::app {

// UI-only palette. Native controls remain the input/state owners. High Contrast
// uses the user's system colors, not semantic accents or custom button painting.
struct StudioColors {
  bool system;
  COLORREF paper, ink, surface, muted, current_line, selection;
  COLORREF blue, green, amber, purple, teal, red;
  COLORREF chrome, chrome_text, hover, pressed, success, warning, error;
  // Bright accents belong on the dark command bar, not on white content.
  COLORREF chrome_hover, chrome_pressed, chrome_muted, chrome_focus;
  COLORREF chrome_green, chrome_blue, chrome_amber, chrome_purple, chrome_teal;
};

inline StudioColors CurrentStudioColors() noexcept {
  HIGHCONTRASTW contrast{sizeof(contrast)};
  const bool system =
      !::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) ||
      (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
  if (system) {
    const auto ink = ::GetSysColor(COLOR_WINDOWTEXT);
    const auto paper = ::GetSysColor(COLOR_WINDOW);
    return {true, paper, ink, paper, ink, paper,
            ::GetSysColor(COLOR_HIGHLIGHT), ink, ink, ink, ink, ink, ink,
            ::GetSysColor(COLOR_BTNFACE), ::GetSysColor(COLOR_BTNTEXT),
            ::GetSysColor(COLOR_HIGHLIGHT), ::GetSysColor(COLOR_HIGHLIGHT),
            paper, paper, paper,
            ::GetSysColor(COLOR_HIGHLIGHT), ::GetSysColor(COLOR_HIGHLIGHT),
            ::GetSysColor(COLOR_GRAYTEXT), ::GetSysColor(COLOR_HIGHLIGHTTEXT),
            ink, ink, ink, ink, ink};
  }
  return {false, RGB(255, 255, 255), RGB(35, 45, 61), RGB(241, 241, 241),
          RGB(92, 106, 124), RGB(246, 246, 255), RGB(207, 227, 255),
          RGB(34, 92, 174), RGB(32, 117, 66), RGB(158, 89, 16),
          RGB(120, 66, 158), RGB(16, 112, 122), RGB(174, 49, 59),
          RGB(76, 76, 76), RGB(250, 250, 250), RGB(221, 234, 251),
          RGB(192, 215, 246), RGB(228, 244, 233), RGB(255, 244, 222),
          RGB(255, 234, 236),
          RGB(92, 92, 92), RGB(56, 56, 56), RGB(190, 190, 190), RGB(153, 218, 250),
          RGB(155, 220, 151), RGB(153, 218, 250), RGB(255, 211, 139),
          RGB(213, 184, 249), RGB(142, 228, 216)};
}

// The stock DC brush is borrowed; its color is local to this device context.
inline HBRUSH StudioControlColors(HDC dc, COLORREF foreground,
                                  COLORREF background) noexcept {
  ::SetTextColor(dc, foreground);
  ::SetBkColor(dc, background);
  ::SetDCBrushColor(dc, background);
  return static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
}

}  // namespace turbodb::app
