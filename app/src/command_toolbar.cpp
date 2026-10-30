#include "command_toolbar.h"

#include "command_ids.h"
#include "studio_colors.h"

namespace turbodb::app {

namespace {

constexpr int kIconSize = 20;
constexpr int kButtonHeight = 36;
constexpr COLORREF kTransparent = RGB(255, 0, 255);

struct ToolbarItem {
  UINT command;
  const wchar_t* label;
};

constexpr ToolbarItem kItems[] = {
    {kQueryExecute, L"Execute"},
    {kQueryPlan, L"Plan"},
    {kQueryAnalyze, L"Analyze..."},
    {0, nullptr},
    {kEditUndo, L"Undo"},
    {kEditRedo, L"Redo"},
    {0, nullptr},
    {kViewExplorer, L"Schemas"},
    {kViewEditor, L"Editor"},
    {kViewResults, L"Results"},
};

bool Line(CDC& dc, int x1, int y1, int x2, int y2) {
  const POINT points[] = {{x1, y1}, {x2, y2}};
  return dc.Polyline(points, _countof(points)) != FALSE;
}

bool DrawGlyph(CDC& dc, UINT command) {
  switch (command) {
    case kQueryExecute: {
      const POINT play[] = {{5, 3}, {16, 10}, {5, 17}};
      return dc.Polygon(play, _countof(play)) != FALSE;
    }
    case kQueryPlan:
      return Line(dc, 10, 6, 10, 10) && Line(dc, 5, 10, 15, 10) &&
             Line(dc, 5, 10, 5, 13) && Line(dc, 15, 10, 15, 13) &&
             dc.Rectangle(7, 2, 14, 7) && dc.Rectangle(2, 13, 8, 18) &&
             dc.Rectangle(12, 13, 18, 18);
    case kQueryAnalyze:
      return Line(dc, 3, 17, 18, 17) && dc.Rectangle(4, 11, 7, 17) &&
             dc.Rectangle(9, 7, 12, 17) && dc.Rectangle(14, 2, 17, 17);
    case kEditUndo:
    case kEditRedo: {
      const bool undo = command == kEditUndo;
      const int head = undo ? 3 : 17;
      const int tail = undo ? 8 : 12;
      return Line(dc, head, 6, tail, 2) && Line(dc, head, 6, tail, 10) &&
             Line(dc, head, 6, undo ? 14 : 6, 6) &&
             Line(dc, undo ? 14 : 6, 6, undo ? 17 : 3, 10) &&
             Line(dc, undo ? 17 : 3, 10, undo ? 17 : 3, 15) &&
             Line(dc, undo ? 17 : 3, 15, undo ? 11 : 9, 15);
    }
    case kViewExplorer:
      return Line(dc, 3, 5, 3, 15) && Line(dc, 17, 5, 17, 15) &&
             dc.Ellipse(3, 2, 18, 8) && dc.Arc(3, 7, 18, 13, 3, 10, 17, 10) &&
             dc.Arc(3, 12, 18, 18, 3, 15, 17, 15);
    case kViewEditor:
      return dc.Rectangle(2, 3, 18, 18) && Line(dc, 3, 7, 17, 7) &&
             Line(dc, 7, 10, 4, 12) && Line(dc, 4, 12, 7, 15) &&
             Line(dc, 13, 10, 16, 12) && Line(dc, 16, 12, 13, 15);
    case kViewResults:
      return dc.Rectangle(2, 3, 18, 18) && Line(dc, 3, 8, 17, 8) &&
             Line(dc, 3, 13, 17, 13) && Line(dc, 8, 4, 8, 17) &&
             Line(dc, 13, 4, 13, 17);
    default:
      return false;
  }
}

COLORREF GlyphColor(UINT command, const StudioColors& colors) {
  if (colors.system) return ::GetSysColor(COLOR_BTNTEXT);
  switch (command) {
    case kQueryExecute: return colors.chrome_green;
    case kQueryPlan: return colors.chrome_blue;
    case kQueryAnalyze: return colors.chrome_amber;
    case kEditUndo:
    case kEditRedo: return colors.chrome_blue;
    case kViewExplorer: return colors.chrome_purple;
    case kViewEditor: return colors.chrome_teal;
    case kViewResults: return colors.chrome_blue;
    default: return colors.chrome_text;
  }
}

int AddGlyph(CImageListManaged& images, HDC reference, UINT command,
             COLORREF color) {
  CDC dc;
  CBitmap bitmap;
  CBrush background;
  CPen pen;
  CBrush foreground;
  if (dc.CreateCompatibleDC(reference) == nullptr ||
      bitmap.CreateCompatibleBitmap(reference, kIconSize, kIconSize) == nullptr ||
      background.CreateSolidBrush(kTransparent) == nullptr ||
      pen.CreatePen(PS_SOLID, 2, color) == nullptr ||
      foreground.CreateSolidBrush(color) == nullptr)
    return -1;

  const HBITMAP old_bitmap = dc.SelectBitmap(bitmap);
  const HPEN old_pen = dc.SelectPen(pen);
  const HBRUSH old_brush = dc.SelectBrush(
      command == kQueryExecute ? foreground.m_hBrush
                              : static_cast<HBRUSH>(::GetStockObject(NULL_BRUSH)));
  RECT bounds{0, 0, kIconSize, kIconSize};
  const bool drawn = dc.FillRect(&bounds, background) && DrawGlyph(dc, command);
  dc.SelectBrush(old_brush);
  dc.SelectPen(old_pen);
  dc.SelectBitmap(old_bitmap);
  return drawn ? images.Add(bitmap, kTransparent) : -1;
}

}  // namespace

const wchar_t* CommandToolbar::CommandTip(UINT command) noexcept {
  switch (command) {
    case kQueryExecute: return L"Execute SQL (F5 / Ctrl+Enter)\nRun the selected SQL, or the editor contents.";
    case kQueryPlan: return L"Execution Plan (F6)\nShow the database execution plan.";
    case kQueryAnalyze: return L"Analyze SQL (Shift+F6)\nMay execute the statement; confirmation is required.";
    case kEditUndo: return L"Undo (Ctrl+Z)\nUndo the last SQL edit.";
    case kEditRedo: return L"Redo (Ctrl+Y)\nRedo the last undone SQL edit.";
    case kViewExplorer: return L"Schemas\nShow or hide the database tree. A checked button means visible.";
    case kViewEditor: return L"SQL Editor\nShow or hide this query's editor. SQL and undo history are retained.";
    case kViewResults: return L"Results\nShow or hide this query's results, messages, and execution plan.";
    default: return L"";
  }
}

bool CommandToolbar::Create(HWND parent) {
  const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBSTYLE_FLAT |
                      TBSTYLE_LIST | TBSTYLE_TOOLTIPS | TBSTYLE_WRAPABLE |
                      CCS_TOP | CCS_NODIVIDER;
  if (CToolBarCtrl::Create(parent, nullptr, nullptr, style, 0, kControlId) == nullptr)
    return false;
  SetButtonStructSize();
  SetExtendedStyle(TBSTYLE_EX_MIXEDBUTTONS);
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof(metrics);
  if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) ||
      font_.CreateFontIndirect(&metrics.lfMessageFont) == nullptr)
    return false;
  SetFont(font_);
  if (!SetBitmapSize(kIconSize, kIconSize) || !SetButtonSize(24, kButtonHeight) ||
      !RefreshColors())
    return false;

  TBBUTTON buttons[_countof(kItems)]{};
  int image_index = 0;
  for (std::size_t index = 0; index < _countof(kItems); ++index) {
    const auto& item = kItems[index];
    auto& button = buttons[index];
    if (item.command == 0) {
      button.fsStyle = BTNS_SEP;
      button.iBitmap = 12;
      continue;
    }
    button.iBitmap = image_index++;
    // TB_ADDBUTTONS accepts a label pointer and stores the button text.
    button.iString = reinterpret_cast<INT_PTR>(item.label);
    button.idCommand = item.command;
    button.fsState = TBSTATE_ENABLED;
    button.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT;
    if (item.command >= kViewExplorer && item.command <= kViewResults)
      button.fsStyle |= BTNS_CHECK;
  }
  if (!AddButtons(_countof(buttons), buttons)) return false;
  CToolTipCtrl tips(GetToolTips());
  if (tips.IsWindow()) tips.SetMaxTipWidth(360);
  AutoSize();
  return true;
}

bool CommandToolbar::RefreshColors() {
  CImageListManaged replacement;
  if (!replacement.Create(kIconSize, kIconSize, ILC_COLOR32 | ILC_MASK, 8, 0))
    return false;
  CClientDC reference(m_hWnd);
  if (reference.IsNull()) return false;
  const auto colors = CurrentStudioColors();
  for (const auto& item : kItems) {
    if (item.command != 0 &&
        AddGlyph(replacement, reference, item.command,
                 GlyphColor(item.command, colors)) < 0)
      return false;
  }
  // Publish only a complete list; detach the control before releasing its old
  // borrowed images. Button indices and command availability remain unchanged.
  SetImageList(replacement);
  images_.Attach(replacement.Detach());
  Invalidate();
  return true;
}

LRESULT CommandToolbar::CustomDraw(NMTBCUSTOMDRAW& draw) {
  const auto colors = CurrentStudioColors();
  if (colors.system) return CDRF_DODEFAULT;
  auto& native = draw.nmcd;
  if (native.dwDrawStage == CDDS_PREPAINT) {
    RECT client{};
    GetClientRect(&client);
    ::FillRect(native.hdc, &client,
                StudioControlColors(native.hdc, colors.chrome_text, colors.chrome));
    return CDRF_NOTIFYITEMDRAW;
  }
  const UINT command = static_cast<UINT>(native.dwItemSpec);
  const bool enabled = IsButtonEnabled(command) != FALSE;
  const bool hot = enabled && (native.uItemState & CDIS_HOT) != 0;
  const bool pressed = enabled && IsButtonPressed(command) != FALSE;
  const bool checked = IsButtonChecked(command) != FALSE;
  if (native.dwDrawStage == CDDS_ITEMPREPAINT) {
    RECT bounds = native.rc;
    ::InflateRect(&bounds, -2, -2);
    const COLORREF background = pressed || checked ? colors.chrome_pressed :
        hot ? colors.chrome_hover : colors.chrome;
    ::FillRect(native.hdc, &bounds,
                StudioControlColors(native.hdc, colors.ink, background));
    draw.clrText = enabled ? colors.chrome_text : colors.chrome_muted;
    draw.clrTextHighlight = draw.clrText;
    draw.nStringBkMode = TRANSPARENT;
    draw.nHLStringBkMode = TRANSPARENT;
    // Only paint the surface. Windows still draws the label and image and owns
    // hit-testing, tooltips, press/capture, disabled behavior and accessibility.
    return TBCDRF_NOBACKGROUND | TBCDRF_NOEDGES | TBCDRF_NOETCHEDEFFECT |
           TBCDRF_USECDCOLORS | CDRF_NOTIFYPOSTPAINT;
  }
  if (native.dwDrawStage == CDDS_ITEMPOSTPAINT && checked) {
    // The themed Win32 toolbar may overpaint ITEMPREPAINT's checked fill
    // after TBSTYLE_WRAPABLE moves buttons onto a second row. Give checked
    // buttons a persistent two-pixel inset outline AFTER native text/icon
    // painting, preserving the native control and its input/accessibility.
    RECT outline = native.rc;
    ::InflateRect(&outline, -1, -1);
    HBRUSH outline_brush = StudioControlColors(
        native.hdc, colors.chrome_text, colors.chrome_pressed);
    ::FrameRect(native.hdc, &outline, outline_brush);
    ::InflateRect(&outline, -1, -1);
    ::FrameRect(native.hdc, &outline, outline_brush);

    RECT marker = native.rc;
    ::InflateRect(&marker, -4, -2);
    marker.top = marker.bottom - 2;
    ::FillRect(native.hdc, &marker, StudioControlColors(native.hdc, colors.chrome_text,
        enabled ? GlyphColor(command, colors) : colors.chrome_muted));
  }
  if (native.dwDrawStage == CDDS_ITEMPOSTPAINT && hot && ::GetFocus() == m_hWnd) {
    RECT focus = native.rc;
    ::InflateRect(&focus, -2, -2);
    const auto brush = StudioControlColors(native.hdc, colors.chrome_text, colors.chrome_focus);
    ::FrameRect(native.hdc, &focus, brush);
    ::InflateRect(&focus, -1, -1);
    ::FrameRect(native.hdc, &focus, brush);
  }
  return CDRF_DODEFAULT;
}

}  // namespace turbodb::app
