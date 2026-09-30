#include "results/result_workspace_view.h"

#include <iomanip>
#include <sstream>

namespace turbodb::app {

namespace {

constexpr UINT kGridId = 4201u;
constexpr UINT kMessageId = 4202u;
constexpr UINT kStatusId = 4203u;
constexpr int kStatusHeight = 24;

}  // namespace

LRESULT ResultWorkspaceView::OnCreate(UINT, WPARAM, LPARAM, BOOL&) {
  RECT client{};
  GetClientRect(&client);

  const DWORD grid_style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                           LVS_SHOWSELALWAYS | LVS_SINGLESEL;
  if (grid_.Create(m_hWnd, client, nullptr, grid_style, WS_EX_CLIENTEDGE,
                   kGridId) == nullptr) {
    return -1;
  }
  grid_.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

  const DWORD message_style = WS_CHILD | ES_MULTILINE | ES_READONLY |
                              ES_AUTOVSCROLL | WS_VSCROLL;
  if (message_.Create(m_hWnd, client, nullptr, message_style,
                      WS_EX_CLIENTEDGE, kMessageId) == nullptr) {
    return -1;
  }

  if (status_.Create(m_hWnd, client, L"Ready",
                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0,
                     kStatusId) == nullptr) {
    return -1;
  }

  ShowGrid(false);
  Layout(client.right - client.left, client.bottom - client.top);
  return 0;
}

LRESULT ResultWorkspaceView::OnSize(UINT, WPARAM, LPARAM lparam, BOOL&) {
  Layout(LOWORD(lparam), HIWORD(lparam));
  return 0;
}

void ResultWorkspaceView::Layout(int width, int height) {
  if (width < 0) width = 0;
  if (height < 0) height = 0;
  const int body_height = height > kStatusHeight ? height - kStatusHeight : 0;
  if (grid_.IsWindow())
    grid_.SetWindowPos(nullptr, 0, 0, width, body_height,
                       SWP_NOACTIVATE | SWP_NOZORDER);
  if (message_.IsWindow())
    message_.SetWindowPos(nullptr, 0, 0, width, body_height,
                          SWP_NOACTIVATE | SWP_NOZORDER);
  if (status_.IsWindow())
    status_.SetWindowPos(nullptr, 0, body_height, width, kStatusHeight,
                         SWP_NOACTIVATE | SWP_NOZORDER);
}

void ResultWorkspaceView::ShowGrid(bool visible) {
  if (grid_.IsWindow()) grid_.ShowWindow(visible ? SW_SHOW : SW_HIDE);
  if (message_.IsWindow()) message_.ShowWindow(visible ? SW_HIDE : SW_SHOW);
}

std::wstring ResultWorkspaceView::Utf8(std::string_view text) {
  if (text.empty()) return {};
  if (text.size() > static_cast<std::size_t>(INT_MAX)) return L"<too large>";
  const int size = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0);
  if (size <= 0) return L"<invalid UTF-8>";
  std::wstring output(static_cast<std::size_t>(size), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), output.data(),
                            size) != size) {
    return L"<invalid UTF-8>";
  }
  return output;
}

std::wstring ResultWorkspaceView::CellText(const QueryCell& cell) {
  switch (cell.kind) {
    case ORM_VALUE_NULL:
      return L"NULL";
    case ORM_VALUE_INT64:
      return std::to_wstring(cell.int64_value);
    case ORM_VALUE_UINT64:
      return std::to_wstring(cell.uint64_value);
    case ORM_VALUE_DOUBLE: {
      std::wostringstream stream;
      stream << std::setprecision(15) << cell.double_value;
      return stream.str();
    }
    case ORM_VALUE_BOOLEAN:
      return cell.boolean_value ? L"true" : L"false";
    case ORM_VALUE_TEXT:
      return Utf8(cell.bytes);
    case ORM_VALUE_BLOB:
      return L"<BLOB " + std::to_wstring(cell.bytes.size()) + L" bytes>";
    default:
      return L"<?>";
  }
}

void ResultWorkspaceView::SetRunning(std::uint64_t request_id) {
  ShowGrid(false);
  message_.SetWindowTextW(
      L"Executing SQL on a background worker.\r\n"
      L"Materialized execution does not expose active cancellation; "
      L"TurboDB will not imply rollback or retry.");
  const std::wstring text = L"Running request " + std::to_wstring(request_id);
  status_.SetWindowTextW(text.c_str());
}

void ResultWorkspaceView::ShowError(std::string message) {
  ShowGrid(false);
  const std::wstring wide = Utf8(message);
  message_.SetWindowTextW(wide.c_str());
  status_.SetWindowTextW(L"Not executed");
}

void ResultWorkspaceView::Render(const QueryResultSnapshot& result) {
  const double elapsed_ms =
      static_cast<double>(result.elapsed_microseconds) / 1000.0;
  std::wostringstream status_text;
  status_text << std::fixed << std::setprecision(2);

  if (result.kind == QueryOutcomeKind::rows) {
    ShowGrid(true);
    grid_.DeleteAllItems();
    while (grid_.DeleteColumn(0)) {}

    for (std::size_t column = 0; column < result.columns.size(); ++column) {
      std::wstring name = Utf8(result.columns[column]);
      grid_.InsertColumn(static_cast<int>(column), name.c_str(), LVCFMT_LEFT,
                         140, static_cast<int>(column));
    }
    for (std::size_t row = 0; row < result.rows.size(); ++row) {
      if (result.rows[row].empty()) continue;
      std::wstring first = CellText(result.rows[row][0]);
      const int item = grid_.InsertItem(static_cast<int>(row), first.c_str());
      for (std::size_t column = 1; column < result.rows[row].size(); ++column) {
        std::wstring value = CellText(result.rows[row][column]);
        grid_.SetItemText(item, static_cast<int>(column), value.c_str());
      }
    }
    status_text << L"Rows: " << result.rows.size() << L" | Columns: "
                << result.columns.size() << L" | " << elapsed_ms << L" ms";
  } else if (result.kind == QueryOutcomeKind::command) {
    ShowGrid(false);
    const std::wstring body =
        L"Command completed.\r\nAffected rows: " +
        std::to_wstring(result.affected_rows);
    message_.SetWindowTextW(body.c_str());
    status_text << L"Command complete | " << elapsed_ms << L" ms";
  } else {
    ShowGrid(false);
    std::wstring body = L"TurboDB status " + std::to_wstring(result.status);
    if (!result.message.empty()) body += L": " + Utf8(result.message);
    message_.SetWindowTextW(body.c_str());
    status_text << L"Failed | " << elapsed_ms << L" ms";
  }

  const std::wstring status = status_text.str();
  status_.SetWindowTextW(status.c_str());
}

}  // namespace turbodb::app
