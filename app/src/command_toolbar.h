#pragma once

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlctrls.h>
#include <atlgdi.h>

namespace turbodb::app {

// The native toolbar borrows images owned by this wrapper until window teardown.
class CommandToolbar final : public CToolBarCtrl {
 public:
  static constexpr UINT kControlId = 4401;

  bool Create(HWND parent);
  bool RefreshColors();
  LRESULT CustomDraw(NMTBCUSTOMDRAW& draw);
  static const wchar_t* CommandTip(UINT command) noexcept;

 private:
  CImageListManaged images_;
  CFont font_;
};

}  // namespace turbodb::app
