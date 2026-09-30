#include "results/result_workspace_view.h"

#include <climits>
#include <iomanip>
#include <sstream>

namespace turbodb::app {

namespace {

constexpr UINT kGridId = 4201u;
constexpr UINT kMessageId = 4202u;
constexpr UINT kStatusId = 4203u;
constexpr UINT kPlanTreeId = 4204u;
constexpr UINT kPlanRawId = 4205u;
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

  const DWORD tree_style = WS_CHILD | WS_TABSTOP | TVS_HASBUTTONS |
                           TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS;
  if (plan_tree_.Create(m_hWnd, client, nullptr, tree_style,
                        WS_EX_CLIENTEDGE, kPlanTreeId) == nullptr) {
    return -1;
  }
  const DWORD raw_style = WS_CHILD | ES_MULTILINE | ES_READONLY |
                          ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL;
  if (plan_raw_.Create(m_hWnd, client, nullptr, raw_style,
                       WS_EX_CLIENTEDGE, kPlanRawId) == nullptr) {
    return -1;
  }

  if (status_.Create(m_hWnd, client, L"Ready",
                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0,
                     kStatusId) == nullptr) {
    return -1;
  }

  ShowGrid(false);
  ShowPlan(false);
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
  const int tree_height = body_height * 3 / 5;
  if (plan_tree_.IsWindow())
    plan_tree_.SetWindowPos(nullptr, 0, 0, width, tree_height,
                            SWP_NOACTIVATE | SWP_NOZORDER);
  if (plan_raw_.IsWindow())
    plan_raw_.SetWindowPos(nullptr, 0, tree_height, width,
                           body_height - tree_height,
                           SWP_NOACTIVATE | SWP_NOZORDER);
  if (status_.IsWindow())
    status_.SetWindowPos(nullptr, 0, body_height, width, kStatusHeight,
                         SWP_NOACTIVATE | SWP_NOZORDER);
}

void ResultWorkspaceView::ShowGrid(bool visible) {
  if (grid_.IsWindow()) grid_.ShowWindow(visible ? SW_SHOW : SW_HIDE);
  if (message_.IsWindow()) message_.ShowWindow(visible ? SW_HIDE : SW_SHOW);
  if (visible) ShowPlan(false);
}

void ResultWorkspaceView::ShowPlan(bool visible) {
  if (plan_tree_.IsWindow())
    plan_tree_.ShowWindow(visible ? SW_SHOW : SW_HIDE);
  if (plan_raw_.IsWindow())
    plan_raw_.ShowWindow(visible ? SW_SHOW : SW_HIDE);
  if (visible) {
    if (grid_.IsWindow()) grid_.ShowWindow(SW_HIDE);
    if (message_.IsWindow()) message_.ShowWindow(SW_HIDE);
  }
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
  ShowPlan(false);
  ShowGrid(false);
  message_.SetWindowText(
      L"Executing SQL on a background worker.\r\n"
      L"Materialized execution does not expose active cancellation; "
      L"TurboDB will not imply rollback or retry.");
  const std::wstring text = L"Running request " + std::to_wstring(request_id);
  status_.SetWindowText(text.c_str());
}

void ResultWorkspaceView::SetExplainRunning(std::uint64_t request_id,
                                            orm_explain_mode_t mode) {
  ShowGrid(false);
  ShowPlan(false);
  const wchar_t* action =
      mode == ORM_EXPLAIN_ANALYZE ? L"ANALYZE" : L"PLAN";
  std::wstring body =
      mode == ORM_EXPLAIN_ANALYZE
          ? L"Executing ANALYZE on a background worker.\r\n"
            L"ANALYZE may execute the SQL statement."
          : L"Acquiring execution PLAN on a background worker.";
  message_.SetWindowText(body.c_str());
  const std::wstring text =
      std::wstring(action) + L" request " + std::to_wstring(request_id);
  status_.SetWindowText(text.c_str());
}

void ResultWorkspaceView::ShowError(std::string message) {
  ShowPlan(false);
  ShowGrid(false);
  const std::wstring wide = Utf8(message);
  message_.SetWindowText(wide.c_str());
  status_.SetWindowText(L"Not executed");
}

std::wstring ResultWorkspaceView::PlanNodeText(
    const ExecutionPlanNodeSnapshot& node) {
  std::wostringstream text;
  text << Utf8(node.node_type.empty() ? std::string_view{"Plan Node"}
                                      : std::string_view{node.node_type});
  if (!node.relation.empty()) text << L" | " << Utf8(node.relation);
  if (!node.index_name.empty()) text << L" | index=" << Utf8(node.index_name);
  if ((node.flags & ORM_PLAN_NODE_HAS_ESTIMATED_ROWS) != 0u)
    text << L" | est rows=" << node.estimated_rows;
  if ((node.flags & ORM_PLAN_NODE_HAS_ACTUAL_ROWS) != 0u)
    text << L" | actual rows=" << node.actual_rows;
  if ((node.flags & ORM_PLAN_NODE_HAS_TOTAL_COST) != 0u)
    text << L" | cost=" << node.total_cost;
  if ((node.flags & ORM_PLAN_NODE_HAS_ACTUAL_TOTAL_MS) != 0u)
    text << L" | " << node.actual_total_ms << L" ms";
  return text.str();
}

void ResultWorkspaceView::RenderPlan(const ExecutionPlanSnapshot& plan) {
  if (plan.status != ORM_STATUS_OK) {
    ShowPlan(false);
    ShowGrid(false);
    std::wstring body =
        (plan.mode == ORM_EXPLAIN_ANALYZE ? L"ANALYZE failed" : L"PLAN failed");
    body += L" (TurboDB status " + std::to_wstring(plan.status) + L")";
    if (!plan.message.empty()) body += L": " + Utf8(plan.message);
    message_.SetWindowText(body.c_str());
  } else {
    ShowPlan(true);
    plan_tree_.DeleteAllItems();
    plan_items_.assign(plan.nodes.size(), nullptr);
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
      const auto& node = plan.nodes[i];
      HTREEITEM parent = TVI_ROOT;
      if (node.parent_index != ORM_EXECUTION_PLAN_ROOT_INDEX &&
          node.parent_index < plan_items_.size())
        parent = plan_items_[static_cast<std::size_t>(node.parent_index)];
      const std::wstring label = PlanNodeText(node);
      TVINSERTSTRUCTW insert{};
      insert.hParent = parent;
      insert.hInsertAfter = TVI_LAST;
      insert.item.mask = TVIF_TEXT;
      insert.item.pszText = const_cast<wchar_t*>(label.c_str());
      plan_items_[i] = TreeView_InsertItem(plan_tree_.m_hWnd, &insert);
    }
    for (HTREEITEM item : plan_items_) {
      if (item != nullptr) plan_tree_.Expand(item);
    }
    const std::wstring raw = Utf8(plan.raw_detail);
    plan_raw_.SetWindowText(raw.c_str());
  }

  const double elapsed_ms =
      static_cast<double>(plan.elapsed_microseconds) / 1000.0;
  std::wostringstream status_text;
  status_text << (plan.mode == ORM_EXPLAIN_ANALYZE ? L"ANALYZE" : L"PLAN")
              << L" | provider=" << Utf8(plan.provider)
              << L" | nodes=" << plan.nodes.size()
              << L" | " << std::fixed << std::setprecision(2)
              << elapsed_ms << L" ms";
  const std::wstring status = status_text.str();
  status_.SetWindowText(status.c_str());
}

void ResultWorkspaceView::Render(const QueryResultSnapshot& result) {
  ShowPlan(false);
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
    message_.SetWindowText(body.c_str());
    status_text << L"Command complete | " << elapsed_ms << L" ms";
  } else {
    ShowGrid(false);
    std::wstring body = L"TurboDB status " + std::to_wstring(result.status);
    if (!result.message.empty()) body += L": " + Utf8(result.message);
    message_.SetWindowText(body.c_str());
    status_text << L"Failed | " << elapsed_ms << L" ms";
  }

  const std::wstring status = status_text.str();
  status_.SetWindowText(status.c_str());
}

}  // namespace turbodb::app
