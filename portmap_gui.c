#include "portmap.h"
#include "resource.h"
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>

#define VERSION "v1.0.0 2026-06-08"
#define CONTACT "sulfurandcu@gmail.com"

#if defined(_MSC_VER)
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "/subsystem:windows")
#endif

extern volatile int running;
extern HANDLE log_mutex;

#define ID_LOCAL_PORT 1001
#define ID_REMOTE_HOST 1002
#define ID_REMOTE_PORT 1003
#define ID_BIND_CLIENT 1004
#define ID_MAPPING_NAME 1005
#define ID_LOG_LIST 1010
#define ID_CLEAR_LOG 1011
#define ID_MAPPING_LIST 1012
#define ID_ADD_BUTTON 1013
#define ID_REMOVE_BUTTON 1014
#define ID_SAVE_BUTTON 1015
#define ID_LOAD_BUTTON 1016
#define ID_CONTEXT_START 1017
#define ID_CONTEXT_STOP 1019
#define ID_CONTEXT_REMOVE 1020
#define ID_EDIT_BUTTON 1021
#define ID_TIMER_MAPPING_CONN 2025

#define WM_SOCKET_NOTIFY (WM_USER + 1)
/* Async log line: lParam = heap-allocated string (freed in window_proc). Avoids SendMessage
 * from worker threads while UI is blocked in stop_port_mapping -> deadlock. */
#define WM_APP_LOG_LINE (WM_APP + 10)

typedef struct {
    HWND hwnd;
    HWND mapping_name_edit;
    HWND local_port_edit;
    HWND remote_host_edit;
    HWND remote_port_edit;
    HWND bind_client_edit;
    HWND log_list;
    HWND clear_log_button;
    HWND mapping_list;
    HWND add_button;
    HWND remove_button;
    HWND edit_button;

    // Static text controls
    HWND title_static;
    HWND mapping_name_label;
    HWND bind_client_label;
    HWND local_port_label;
    HWND remote_host_label;
    HWND remote_port_label;
    HWND mappings_label;
    HWND log_label;

    PortMapping mappings[MAX_MAPPINGS];
    int mapping_count;
    int selected_mapping;
} GUIContext;

GUIContext gui_context;
HWND g_log_list = NULL;  // Global log list handle

/* 刷新活动映射表格时抑制 LVN_ITEMCHANGED，避免误写回 */
static int s_mapping_list_updating;

/* 浅色底；全部控件统一为系统消息字体（SPI_NONCLIENTMETRICS），不嵌入字库 */
static HBRUSH g_brush_client = NULL;
static HBRUSH g_brush_list = NULL;
static HFONT g_font_ui = NULL;
static HFONT g_font_head = NULL;

static void gui_apply_font(HWND hwnd, HFONT font) {
    if (hwnd && font) {
        SendMessage(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
    }
}

static int gui_init_theme_gdi(void) {
    g_brush_client = CreateSolidBrush(RGB(245, 245, 247));
    g_brush_list = CreateSolidBrush(RGB(252, 252, 254));
    if (!g_brush_list && g_brush_client) {
        g_brush_list = g_brush_client;
    }
    if (!g_brush_client && g_brush_list) {
        g_brush_client = g_brush_list;
    }

    HDC hdcDpi = GetDC(NULL);
    int dpi = GetDeviceCaps(hdcDpi, LOGPIXELSY);
    ReleaseDC(NULL, hdcDpi);
    int h_px = -MulDiv(9, dpi, 72);

    NONCLIENTMETRICS ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(NONCLIENTMETRICS);
    if (SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICS), &ncm, 0)) {
        LOGFONT lf = ncm.lfMessageFont;
        lf.lfHeight = h_px;
        lf.lfWidth = 0;
        g_font_ui = CreateFontIndirect(&lf);
        lf.lfWeight = FW_SEMIBOLD;
        g_font_head = CreateFontIndirect(&lf);
    }
    if (!g_font_ui) {
        g_font_ui = CreateFont(h_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    }
    if (!g_font_head) {
        g_font_head = CreateFont(h_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    }

    return (g_font_ui != NULL);
}

static void gui_cleanup_theme_gdi(void) {
    if (g_font_ui) {
        DeleteObject(g_font_ui);
        g_font_ui = NULL;
    }
    if (g_font_head) {
        DeleteObject(g_font_head);
        g_font_head = NULL;
    }

    if (g_brush_list && g_brush_list != g_brush_client) {
        DeleteObject(g_brush_list);
    }
    g_brush_list = NULL;
    if (g_brush_client) {
        DeleteObject(g_brush_client);
        g_brush_client = NULL;
    }
}

// Tray icon variables
NOTIFYICONDATA g_nid;
HWND g_hwnd = NULL;
BOOL g_tray_active = FALSE;

void add_log_entry(HWND hwnd_list, const char* message) {
    if (hwnd_list && message) {
        int index = SendMessage(hwnd_list, LB_ADDSTRING, 0, (LPARAM)message);
        SendMessage(hwnd_list, LB_SETTOPINDEX, index, 0);

        int count = SendMessage(hwnd_list, LB_GETCOUNT, 0, 0);
        if (count > 1000) {
            SendMessage(hwnd_list, LB_DELETESTRING, 0, 0);
        }
    }
}

// Function to be called from portmap_core.c (any thread; must not block on UI)
void gui_log_message(const char* message) {
    if (!message || !g_hwnd) {
        return;
    }
    size_t len = strlen(message) + 1;
    char* copy = (char*)malloc(len);
    if (!copy) {
        return;
    }
    memcpy(copy, message, len);
    if (!PostMessage(g_hwnd, WM_APP_LOG_LINE, 0, (LPARAM)copy)) {
        free(copy);
    }
}

/* LVCOLUMN 的 LVCFMT_CENTER 在无主题表头上有时不生效，直接改 Header 的 HDF_CENTER */
static void mapping_listview_apply_header_center(HWND lv) {
    HWND hdr = ListView_GetHeader(lv);
    if (!hdr) {
        return;
    }
    int n = (int)SendMessage(hdr, HDM_GETITEMCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        HDITEM hdi;
        ZeroMemory(&hdi, sizeof(hdi));
        hdi.mask = HDI_FORMAT;
        if (!Header_GetItem(hdr, i, &hdi)) {
            continue;
        }
        hdi.fmt = (hdi.fmt & ~HDF_JUSTIFYMASK) | HDF_CENTER;
        hdi.mask = HDI_FORMAT;
        Header_SetItem(hdr, i, &hdi);
    }
}

static void layout_mapping_listview_columns(HWND lv, int total_w) {
    if (total_w < 280) {
        total_w = 280;
    }
    /* 状态、名称、指定主机、本地端口、远程主机、远程端口、连接数量 */
    int w0 = total_w * 9 / 100;
    int w1 = total_w * 14 / 100;
    int w2 = total_w * 14 / 100;
    int w3 = total_w * 10 / 100;
    int w4 = total_w * 18 / 100;
    int w5 = total_w * 10 / 100;
    int w6 = total_w - w0 - w1 - w2 - w3 - w4 - w5;
    if (w6 < 72) {
        w6 = 72;
    }

    HWND hdr = ListView_GetHeader(lv);
    int ncol = hdr ? (int)SendMessage(hdr, HDM_GETITEMCOUNT, 0, 0) : 0;
    if (ncol != 7) {
        while (ListView_DeleteColumn(lv, 0)) {
        }
        LVCOLUMN col;
        ZeroMemory(&col, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        col.fmt = LVCFMT_CENTER;

        col.iSubItem = 0;
        col.pszText = "状态";
        col.cx = w0;
        ListView_InsertColumn(lv, 0, &col);

        col.iSubItem = 1;
        col.pszText = "名称";
        col.cx = w1;
        ListView_InsertColumn(lv, 1, &col);

        col.iSubItem = 2;
        col.pszText = "指定主机";
        col.cx = w2;
        ListView_InsertColumn(lv, 2, &col);

        col.iSubItem = 3;
        col.pszText = "本地端口";
        col.cx = w3;
        ListView_InsertColumn(lv, 3, &col);

        col.iSubItem = 4;
        col.pszText = "远程主机";
        col.cx = w4;
        ListView_InsertColumn(lv, 4, &col);

        col.iSubItem = 5;
        col.pszText = "远程端口";
        col.cx = w5;
        ListView_InsertColumn(lv, 5, &col);

        col.iSubItem = 6;
        col.pszText = "连接数量";
        col.cx = w6;
        ListView_InsertColumn(lv, 6, &col);
    }

    ListView_SetColumnWidth(lv, 0, w0);
    ListView_SetColumnWidth(lv, 1, w1);
    ListView_SetColumnWidth(lv, 2, w2);
    ListView_SetColumnWidth(lv, 3, w3);
    ListView_SetColumnWidth(lv, 4, w4);
    ListView_SetColumnWidth(lv, 5, w5);
    ListView_SetColumnWidth(lv, 6, w6);

    mapping_listview_apply_header_center(lv);
}

static void mapping_listview_set_selection(HWND lv, int index, int nitems) {
    if (nitems <= 0 || index < 0 || index >= nitems) {
        ListView_SetItemState(lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        return;
    }
    ListView_SetItemState(lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(lv, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(lv, index, FALSE);
}

static int mapping_listview_get_selection(HWND lv) {
    return ListView_GetNextItem(lv, -1, LVNI_SELECTED);
}

/* 活动映射表格：整行自绘（CDRF_SKIPDEFAULT），避免主题强制蓝底白字；浅蓝底 + 黑/绿字 */
#define MAPPING_LIST_COLS 7

/* 报表模式下第 0 列用 GetSubItemRect 常失败，需由行矩形截出首列 */
static BOOL mapping_listview_get_cell_rect(HWND lv, int item, int col, RECT* prc) {
    if (col == 0) {
        RECT rc_row;
        if (!ListView_GetItemRect(lv, item, &rc_row, LVIR_BOUNDS)) {
            return FALSE;
        }
        *prc = rc_row;
        prc->right = prc->left + ListView_GetColumnWidth(lv, 0);
        return TRUE;
    }
    return ListView_GetSubItemRect(lv, item, col, LVIR_BOUNDS, prc);
}

static LRESULT mapping_listview_nm_customdraw(GUIContext* ctx, NMLVCUSTOMDRAW* cd) {
    HWND lv = ctx->mapping_list;
    const COLORREF bg_sel_focus = RGB(200, 222, 252);
    const COLORREF bg_sel_blur = RGB(228, 236, 252);
    const COLORREF bg_norm = RGB(252, 252, 254);
    const COLORREF fg_run = RGB(0, 176, 80);
    const COLORREF fg_black = RGB(0, 0, 0);
    const COLORREF fg_idle = RGB(25, 25, 25);

    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        int i = (int)cd->nmcd.dwItemSpec;
        if (i < 0 || i >= ctx->mapping_count) {
            return CDRF_DODEFAULT;
        }
        BOOL sel = (ListView_GetItemState(lv, i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        BOOL list_has_focus = (GetFocus() == lv);
        BOOL active = ctx->mappings[i].active != 0;
        COLORREF bg = sel ? (list_has_focus ? bg_sel_focus : bg_sel_blur) : bg_norm;
        COLORREF fg = active ? fg_run : (sel ? fg_black : fg_idle);

        HDC hdc = cd->nmcd.hdc;
        HFONT hf = (HFONT)SendMessage(lv, WM_GETFONT, 0, 0);
        HFONT oldf = hf ? (HFONT)SelectObject(hdc, hf) : NULL;
        SetBkMode(hdc, TRANSPARENT);

        for (int col = 0; col < MAPPING_LIST_COLS; col++) {
            RECT rc;
            if (!mapping_listview_get_cell_rect(lv, i, col, &rc)) {
                continue;
            }
            HBRUSH br = CreateSolidBrush(bg);
            FillRect(hdc, &rc, br);
            DeleteObject(br);

            char buf[512];
            buf[0] = '\0';
            ListView_GetItemText(lv, i, col, buf, (int)sizeof(buf));

            SetTextColor(hdc, fg);
            RECT trc = rc;
            trc.left += 6;
            trc.right -= 4;
            DrawText(hdc, buf, -1, &trc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | DT_CENTER);
        }

        if (oldf) {
            SelectObject(hdc, oldf);
        }
        return CDRF_SKIPDEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

static LRESULT CALLBACK mapping_list_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    (void)dwRefData;
    switch (msg) {
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, mapping_list_subclass_proc, uIdSubclass);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void refresh_mapping_list_connection_column(GUIContext* ctx) {
    HWND lv = ctx->mapping_list;
    if (!lv || ctx->mapping_count <= 0) {
        return;
    }
    if (s_mapping_list_updating) {
        return;
    }
    int n = (int)SendMessage(lv, LVM_GETITEMCOUNT, 0, 0);
    if (n > ctx->mapping_count) {
        n = ctx->mapping_count;
    }
    /* 仅更新变化的单元格，并批量抑制重绘；避免每秒整表 Invalidate 导致整行自绘闪烁 */
    SendMessage(lv, WM_SETREDRAW, FALSE, 0);
    int lo = n;
    int hi = -1;
    for (int i = 0; i < n; i++) {
        char newb[32];
        snprintf(newb, sizeof(newb), "%d", count_active_connections_for_mapping(&ctx->mappings[i]));
        char oldb[32];
        oldb[0] = '\0';
        ListView_GetItemText(lv, i, 6, oldb, (int)sizeof(oldb));
        if (strcmp(oldb, newb) != 0) {
            ListView_SetItemText(lv, i, 6, newb);
            if (i < lo) {
                lo = i;
            }
            if (i > hi) {
                hi = i;
            }
        }
    }
    SendMessage(lv, WM_SETREDRAW, TRUE, 0);
    if (hi >= lo) {
        ListView_RedrawItems(lv, lo, hi);
    }
}

void update_mapping_list(GUIContext* ctx) {
    HWND lv = ctx->mapping_list;
    s_mapping_list_updating++;
    ListView_DeleteAllItems(lv);

    for (int i = 0; i < ctx->mapping_count; i++) {
        PortMapping* m = &ctx->mappings[i];
        char local_str[16];
        char remote_port_str[16];
        char conn_str[32];
        snprintf(local_str, sizeof(local_str), "%d", m->local_port);
        snprintf(remote_port_str, sizeof(remote_port_str), "%d", m->remote_port);
        snprintf(conn_str, sizeof(conn_str), "%d", count_active_connections_for_mapping(m));

        LVITEM lvi;
        ZeroMemory(&lvi, sizeof(lvi));
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.iSubItem = 0;
        lvi.pszText = m->active ? "运行中" : "已停止";
        ListView_InsertItem(lv, &lvi);

        {
            char name_empty[1] = {0};
            ListView_SetItemText(lv, i, 1, m->name[0] ? m->name : name_empty);
        }
        {
            char bind_empty[1] = {0};
            ListView_SetItemText(lv, i, 2, m->bind_client[0] ? m->bind_client : bind_empty);
        }
        ListView_SetItemText(lv, i, 3, local_str);
        ListView_SetItemText(lv, i, 4, m->remote_host);
        ListView_SetItemText(lv, i, 5, remote_port_str);
        ListView_SetItemText(lv, i, 6, conn_str);
    }

    if (ctx->selected_mapping >= 0 && ctx->selected_mapping < ctx->mapping_count) {
        mapping_listview_set_selection(lv, ctx->selected_mapping, ctx->mapping_count);
    }
    s_mapping_list_updating--;
    InvalidateRect(lv, NULL, FALSE);
}

void load_selected_mapping(GUIContext* ctx) {
    if (ctx->selected_mapping >= 0 && ctx->selected_mapping < ctx->mapping_count) {
        PortMapping* mapping = &ctx->mappings[ctx->selected_mapping];

        char local_port_str[16], remote_port_str[16];
        snprintf(local_port_str, sizeof(local_port_str), "%d", mapping->local_port);
        snprintf(remote_port_str, sizeof(remote_port_str), "%d", mapping->remote_port);

        SetWindowText(ctx->mapping_name_edit, mapping->name);
        SetWindowText(ctx->local_port_edit, local_port_str);
        SetWindowText(ctx->remote_host_edit, mapping->remote_host);
        SetWindowText(ctx->remote_port_edit, remote_port_str);
        SetWindowText(ctx->bind_client_edit, mapping->bind_client);

    }
}

void save_current_mapping(GUIContext* ctx) {
    if (ctx->selected_mapping >= 0 && ctx->selected_mapping < ctx->mapping_count) {
        PortMapping* mapping = &ctx->mappings[ctx->selected_mapping];

        char local_port_str[16], remote_port_str[16];
        GetWindowText(ctx->mapping_name_edit, mapping->name, sizeof(mapping->name));
        mapping->name[sizeof(mapping->name) - 1] = '\0';
        GetWindowText(ctx->local_port_edit, local_port_str, sizeof(local_port_str));
        GetWindowText(ctx->remote_host_edit, mapping->remote_host, sizeof(mapping->remote_host));
        GetWindowText(ctx->remote_port_edit, remote_port_str, sizeof(remote_port_str));
        GetWindowText(ctx->bind_client_edit, mapping->bind_client, sizeof(mapping->bind_client));

        mapping->local_port = atoi(local_port_str);
        mapping->remote_port = atoi(remote_port_str);
        mapping->address_family = AF_IPV4;
        /* Do not touch socket / thread_handle here: they are runtime state.
           Clearing them while a mapping is active breaks stop_port_mapping and
           leaves the port bound (next start fails with bind error). */
    }
}

void enable_controls(GUIContext* ctx, BOOL enable) {
    EnableWindow(ctx->mapping_name_edit, enable);
    EnableWindow(ctx->local_port_edit, enable);
    EnableWindow(ctx->remote_host_edit, enable);
    EnableWindow(ctx->remote_port_edit, enable);
    EnableWindow(ctx->bind_client_edit, enable);
}

/* 左侧编辑框：仅当选中项为「运行中」时锁定；选中已停止项或无有效选中时允许修改 */
static void update_controls_for_current_selection(GUIContext* ctx) {
    if (ctx->mapping_count <= 0 || ctx->selected_mapping < 0 || ctx->selected_mapping >= ctx->mapping_count) {
        EnableWindow(ctx->mapping_name_edit, TRUE);
        EnableWindow(ctx->local_port_edit, TRUE);
        EnableWindow(ctx->remote_host_edit, TRUE);
        EnableWindow(ctx->remote_port_edit, TRUE);
        EnableWindow(ctx->bind_client_edit, TRUE);
        EnableWindow(ctx->edit_button, FALSE);
        return;
    }
    if (ctx->mappings[ctx->selected_mapping].active) {
        enable_controls(ctx, FALSE);
        EnableWindow(ctx->edit_button, FALSE);
    } else {
        enable_controls(ctx, TRUE);
        EnableWindow(ctx->edit_button, TRUE);
    }
}

void resize_controls(GUIContext* ctx, int window_width, int window_height) {
    /* 边距与行高略增，接近当前系统设置类工具的疏朗布局 */
    const int MARGIN = 18;

    // Use a more stable left panel width (minimum 350, or 40% of window)
    int left_panel_width = (window_width * 4) / 10;
    if (left_panel_width < 350) left_panel_width = 350;
    if (left_panel_width > 550) left_panel_width = 550;

    int right_panel_start = left_panel_width + 20;

    const int ROW_HEIGHT = 26;
    const int ROW_SPACING = 36;
    /* 与右侧活动映射列表上缘对齐（非与「活动映射」标题对齐） */
    const int MAPPING_LIST_TOP = 45;
    const int START_Y = MAPPING_LIST_TOP;

    // Labels and edits positioning（标签区收窄 + 右对齐，避免与编辑框之间留空过大）
    const int LABEL_EDIT_GAP = 8;
    int label_width = 68;
    int edit_x = MARGIN + label_width + LABEL_EDIT_GAP;
    int edit_width = left_panel_width - edit_x;

    // Update left panel controls
    SetWindowPos(ctx->title_static, NULL, MARGIN, 10, left_panel_width - MARGIN, 25, SWP_NOZORDER);

    SetWindowPos(ctx->mapping_name_label, NULL, MARGIN, START_Y, label_width, ROW_HEIGHT, SWP_NOZORDER);
    SetWindowPos(ctx->mapping_name_edit, NULL, edit_x, START_Y, edit_width, ROW_HEIGHT, SWP_NOZORDER);

    SetWindowPos(ctx->bind_client_label, NULL, MARGIN, START_Y + ROW_SPACING, label_width, ROW_HEIGHT, SWP_NOZORDER);
    SetWindowPos(ctx->bind_client_edit, NULL, edit_x, START_Y + ROW_SPACING, edit_width, ROW_HEIGHT, SWP_NOZORDER);

    SetWindowPos(ctx->local_port_label, NULL, MARGIN, START_Y + 2 * ROW_SPACING, label_width, ROW_HEIGHT, SWP_NOZORDER);
    SetWindowPos(ctx->local_port_edit, NULL, edit_x, START_Y + 2 * ROW_SPACING, edit_width, ROW_HEIGHT, SWP_NOZORDER);

    SetWindowPos(ctx->remote_host_label, NULL, MARGIN, START_Y + 3 * ROW_SPACING, label_width, ROW_HEIGHT, SWP_NOZORDER);
    SetWindowPos(ctx->remote_host_edit, NULL, edit_x, START_Y + 3 * ROW_SPACING, edit_width, ROW_HEIGHT, SWP_NOZORDER);

    SetWindowPos(ctx->remote_port_label, NULL, MARGIN, START_Y + 4 * ROW_SPACING, label_width, ROW_HEIGHT, SWP_NOZORDER);
    SetWindowPos(ctx->remote_port_edit, NULL, edit_x, START_Y + 4 * ROW_SPACING, edit_width, ROW_HEIGHT, SWP_NOZORDER);

    // Buttons area (aligned in one row across both panels)
    int btn_y = START_Y + 5 * ROW_SPACING + 10;
    int spacing = 15;
    int btn_w = 100;
    int btn_h = 35;  // Match start/stop button height

    SetWindowPos(ctx->add_button, NULL, MARGIN, btn_y, btn_w, btn_h, SWP_NOZORDER);
    SetWindowPos(ctx->remove_button, NULL, MARGIN + btn_w + spacing, btn_y, btn_w, btn_h, SWP_NOZORDER);
    SetWindowPos(ctx->edit_button, NULL, MARGIN + 2 * (btn_w + spacing), btn_y, btn_w, btn_h, SWP_NOZORDER);

    // Update right panel (Activity mappings)
    int right_panel_width = window_width - right_panel_start - MARGIN;
    SetWindowPos(ctx->mappings_label, NULL, right_panel_start, 10, right_panel_width, 25, SWP_NOZORDER);
    {
        int list_h = btn_y + btn_h - MAPPING_LIST_TOP - 10;
        if (list_h < 80) list_h = 80;
        SetWindowPos(ctx->mapping_list, NULL, right_panel_start, MAPPING_LIST_TOP, right_panel_width, list_h, SWP_NOZORDER);
        layout_mapping_listview_columns(ctx->mapping_list, right_panel_width);
    }

    // Update status and log area
    int log_top = 310; // Moved up since status area is removed
    int log_height = window_height - log_top - MARGIN;

    SetWindowPos(ctx->log_label, NULL, MARGIN, log_top - 30, 80, 25, SWP_NOZORDER);
    SetWindowPos(ctx->clear_log_button, NULL, window_width - MARGIN - 90, log_top - 30, 90, 25, SWP_NOZORDER);

    SetWindowPos(ctx->log_list, NULL, MARGIN, log_top, window_width - 2 * MARGIN, log_height, SWP_NOZORDER);
}

// Tray icon functions
void add_tray_icon(HWND hwnd) {
    g_hwnd = hwnd;

    ZeroMemory(&g_nid, sizeof(NOTIFYICONDATA));
    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_USER + 2;  // Tray message
    g_nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(ID_TRAY_ICON));
    strcpy(g_nid.szTip, "Port Forwarding Tool");

    if (Shell_NotifyIcon(NIM_ADD, &g_nid)) {
        g_tray_active = TRUE;
    }
}

void remove_tray_icon() {
    if (g_tray_active) {
        Shell_NotifyIcon(NIM_DELETE, &g_nid);
        g_tray_active = FALSE;
    }
}

void show_tray_menu(HWND hwnd, POINT pt) {
    HMENU hMenu = CreatePopupMenu();
    AppendMenu(hMenu, MF_STRING, ID_TRAY_SHOW, "窗口");
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, "退出");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_LEFTBUTTON | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

DWORD WINAPI gui_forward_thread(LPVOID param) {
    GUIContext* ctx = (GUIContext*)param;

    if (ctx->selected_mapping >= 0 && ctx->selected_mapping < ctx->mapping_count) {
        PortMapping* mapping = &ctx->mappings[ctx->selected_mapping];

        if (start_port_mapping(mapping)) {
            PostMessage(ctx->hwnd, WM_SOCKET_NOTIFY, 1, 0);

            while (mapping->active && running) {
                Sleep(100);
            }

            stop_port_mapping(mapping);
            PostMessage(ctx->hwnd, WM_SOCKET_NOTIFY, 0, 0);
        } else {
            PostMessage(ctx->hwnd, WM_SOCKET_NOTIFY, 2, 0);
        }
    }

    return 0;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    GUIContext* ctx = &gui_context;

    switch (msg) {
        case WM_ERASEBKGND: {
            if (g_brush_client) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                FillRect((HDC)wParam, &rc, g_brush_client);
                return 1;
            }
            break;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, RGB(245, 245, 247));
            SetTextColor(hdc, RGB(48, 48, 50));
            if (g_brush_client) {
                return (LRESULT)g_brush_client;
            }
            break;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, RGB(255, 255, 255));
            SetTextColor(hdc, RGB(32, 32, 34));
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }

        case WM_CTLCOLORLISTBOX: {
            HWND ctl = (HWND)lParam;
            if (ctl == ctx->log_list) {
                HDC hdc = (HDC)wParam;
                SetBkColor(hdc, RGB(252, 252, 254));
                SetTextColor(hdc, RGB(40, 40, 43));
                if (g_brush_list) {
                    return (LRESULT)g_brush_list;
                }
            }
            break;
        }

        case WM_CREATE: {
            ctx->hwnd = hwnd;
            g_hwnd = hwnd;
            ctx->mapping_count = 0;
            ctx->selected_mapping = -1;

            // Left panel - Mapping configuration (45% of width)
            ctx->title_static = CreateWindow("STATIC", "端口映射配置", WS_VISIBLE | WS_CHILD | SS_LEFT | SS_CENTERIMAGE, 10, 10, 400, 25, hwnd, NULL, NULL, NULL);

            ctx->mapping_name_label = CreateWindow("STATIC", "映射名称", WS_VISIBLE | WS_CHILD | SS_RIGHT | SS_CENTERIMAGE, 10, 45, 68, 25, hwnd, NULL, NULL, NULL);
            ctx->mapping_name_edit = CreateWindow("EDIT", "", WS_VISIBLE | WS_CHILD | WS_BORDER, 86, 45, 200, 25, hwnd, (HMENU)ID_MAPPING_NAME, NULL, NULL);

            ctx->bind_client_label = CreateWindow("STATIC", "指定主机", WS_VISIBLE | WS_CHILD | SS_RIGHT | SS_CENTERIMAGE, 10, 81, 68, 25, hwnd, NULL, NULL, NULL);
            ctx->bind_client_edit = CreateWindow("EDIT", "", WS_VISIBLE | WS_CHILD | WS_BORDER, 86, 81, 200, 25, hwnd, (HMENU)ID_BIND_CLIENT, NULL, NULL);

            ctx->local_port_label = CreateWindow("STATIC", "本地端口", WS_VISIBLE | WS_CHILD | SS_RIGHT | SS_CENTERIMAGE, 10, 117, 68, 25, hwnd, NULL, NULL, NULL);
            ctx->local_port_edit = CreateWindow("EDIT", "8888", WS_VISIBLE | WS_CHILD | WS_BORDER, 86, 117, 200, 25, hwnd, (HMENU)ID_LOCAL_PORT, NULL, NULL);

            ctx->remote_host_label = CreateWindow("STATIC", "远程主机", WS_VISIBLE | WS_CHILD | SS_RIGHT | SS_CENTERIMAGE, 10, 153, 68, 25, hwnd, NULL, NULL, NULL);
            ctx->remote_host_edit = CreateWindow("EDIT", "192.168.1.100", WS_VISIBLE | WS_CHILD | WS_BORDER, 86, 153, 200, 25, hwnd, (HMENU)ID_REMOTE_HOST, NULL, NULL);

            ctx->remote_port_label = CreateWindow("STATIC", "远程端口", WS_VISIBLE | WS_CHILD | SS_RIGHT | SS_CENTERIMAGE, 10, 189, 68, 25, hwnd, NULL, NULL, NULL);
            ctx->remote_port_edit = CreateWindow("EDIT", "8888", WS_VISIBLE | WS_CHILD | WS_BORDER, 86, 189, 200, 25, hwnd, (HMENU)ID_REMOTE_PORT, NULL, NULL);

            ctx->add_button = CreateWindow("BUTTON", "添加", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 10, 235, 80, 30, hwnd, (HMENU)ID_ADD_BUTTON, NULL, NULL);
            ctx->remove_button = CreateWindow("BUTTON", "删除", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_DISABLED, 100, 235, 80, 30, hwnd, (HMENU)ID_REMOVE_BUTTON, NULL, NULL);
            ctx->edit_button = CreateWindow("BUTTON", "修改", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_DISABLED, 190, 235, 80, 30, hwnd, (HMENU)ID_EDIT_BUTTON, NULL, NULL);

            // Right panel - Mapping list (starts at 586, 55% of width)
            ctx->mappings_label = CreateWindow("STATIC", "活动映射（双击停止/运行）", WS_VISIBLE | WS_CHILD | SS_LEFT | SS_CENTERIMAGE, 586, 10, 679, 25, hwnd, NULL, NULL, NULL);
            ctx->mapping_list = CreateWindow(WC_LISTVIEW, "", WS_VISIBLE | WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_VSCROLL,
                                             586, 45, 679, 205, hwnd, (HMENU)ID_MAPPING_LIST, GetModuleHandle(NULL), NULL);
            ListView_SetExtendedListViewStyle(ctx->mapping_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            /* 去掉 Explorer 主题，否则选中行仍被画成蓝底白字，NM_CUSTOMDRAW 的 clrText 不生效 */
            SetWindowTheme(ctx->mapping_list, L"", L"");
            {
                HWND hdr = ListView_GetHeader(ctx->mapping_list);
                if (hdr) {
                    SetWindowTheme(hdr, L"", L"");
                }
            }
            layout_mapping_listview_columns(ctx->mapping_list, 679);
            SetWindowSubclass(ctx->mapping_list, mapping_list_subclass_proc, 1, 0);

            ctx->clear_log_button = CreateWindow("BUTTON", "清空日志", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 1185, 305, 80, 25, hwnd, (HMENU)ID_CLEAR_LOG, NULL, NULL);

            ctx->log_label = CreateWindow("STATIC", "日志", WS_VISIBLE | WS_CHILD | SS_CENTERIMAGE, 10, 305, 80, 25, hwnd, NULL, NULL, NULL);
            ctx->log_list = CreateWindow("LISTBOX", "", WS_VISIBLE | WS_CHILD | WS_BORDER | LBS_NOINTEGRALHEIGHT | WS_VSCROLL, 15, 330, 1250, 375, hwnd, (HMENU)ID_LOG_LIST, NULL, NULL);

            gui_apply_font(ctx->mapping_name_edit, g_font_ui);
            gui_apply_font(ctx->local_port_edit, g_font_ui);
            gui_apply_font(ctx->remote_host_edit, g_font_ui);
            gui_apply_font(ctx->remote_port_edit, g_font_ui);
            gui_apply_font(ctx->bind_client_edit, g_font_ui);
            gui_apply_font(ctx->add_button, g_font_ui);
            gui_apply_font(ctx->remove_button, g_font_ui);
            gui_apply_font(ctx->edit_button, g_font_ui);
            gui_apply_font(ctx->clear_log_button, g_font_ui);
            gui_apply_font(ctx->log_list, g_font_ui);
            gui_apply_font(ctx->mapping_list, g_font_ui);

            /* Set global log list handle for core module */
            g_log_list = ctx->log_list;

            gui_apply_font(ctx->title_static, g_font_head);
            gui_apply_font(ctx->mappings_label, g_font_head);
            gui_apply_font(ctx->log_label, g_font_head);
            gui_apply_font(ctx->mapping_name_label, g_font_ui);
            gui_apply_font(ctx->bind_client_label, g_font_ui);
            gui_apply_font(ctx->local_port_label, g_font_ui);
            gui_apply_font(ctx->remote_host_label, g_font_ui);
            gui_apply_font(ctx->remote_port_label, g_font_ui);

            // Try to load existing config
            if (load_config(CONFIG_FILE, ctx->mappings, &ctx->mapping_count)) {
                if (ctx->mapping_count > 0) {
                    ctx->selected_mapping = 0;
                }
                update_mapping_list(ctx);

                if (ctx->mapping_count > 0) {
                    load_selected_mapping(ctx);

                    EnableWindow(ctx->remove_button, TRUE);
                    EnableWindow(ctx->edit_button, TRUE);
                    update_controls_for_current_selection(ctx);
                }
            }

            // Add tray icon
            add_tray_icon(hwnd);

            SetTimer(hwnd, ID_TIMER_MAPPING_CONN, 1000, NULL);

            break;
        }

        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case ID_ADD_BUTTON: {
                    if (ctx->mapping_count >= MAX_MAPPINGS) {
                        MessageBox(hwnd, "Maximum mappings reached", "Error", MB_OK | MB_ICONERROR);
                        break;
                    }

                    char local_port_str[16], remote_host[256], remote_port_str[16], bind_client[256];

                    GetWindowText(ctx->local_port_edit, local_port_str, sizeof(local_port_str));
                    GetWindowText(ctx->remote_host_edit, remote_host, sizeof(remote_host));
                    GetWindowText(ctx->remote_port_edit, remote_port_str, sizeof(remote_port_str));
                    GetWindowText(ctx->bind_client_edit, bind_client, sizeof(bind_client));

                    int local_port = atoi(local_port_str);
                    int remote_port = atoi(remote_port_str);

                    if (local_port <= 0 || local_port > 65535) {
                        MessageBox(hwnd, "Invalid local port", "Error", MB_OK | MB_ICONERROR);
                        break;
                    }

                    if (remote_port <= 0 || remote_port > 65535) {
                        MessageBox(hwnd, "Invalid remote port", "Error", MB_OK | MB_ICONERROR);
                        break;
                    }

                    if (strlen(remote_host) == 0) {
                        MessageBox(hwnd, "Remote host is required", "Error", MB_OK | MB_ICONERROR);
                        break;
                    }

                    PortMapping* mapping = &ctx->mappings[ctx->mapping_count];
                    memset(mapping, 0, sizeof(PortMapping));
                    GetWindowText(ctx->mapping_name_edit, mapping->name, sizeof(mapping->name));
                    mapping->name[sizeof(mapping->name) - 1] = '\0';
                    mapping->local_port = local_port;
                    strcpy(mapping->remote_host, remote_host);
                    mapping->remote_port = remote_port;
                    strcpy(mapping->bind_client, bind_client);
                    mapping->address_family = AF_IPV4;
                    mapping->socket = INVALID_SOCKET;
                    mapping->udp_socket = INVALID_SOCKET;
                    mapping->thread_handle = NULL;
                    mapping->udp_thread_handle = NULL;
                    mapping->active = 0;

                    ctx->mapping_count++;
                    ctx->selected_mapping = ctx->mapping_count - 1;
                    update_mapping_list(ctx);
                    load_selected_mapping(ctx);

                    EnableWindow(ctx->remove_button, TRUE);
                    EnableWindow(ctx->edit_button, TRUE);
                    update_controls_for_current_selection(ctx);

                    save_config(CONFIG_FILE, ctx->mappings, ctx->mapping_count);

                    if (mapping->name[0]) {
                        log_message("添加映射 \"%s\": %d -> %s:%d%s%s",
                                    mapping->name,
                                    mapping->local_port,
                                    mapping->remote_host,
                                    mapping->remote_port,
                                    strlen(mapping->bind_client) > 0 ? " 指定主机: " : "",
                                    strlen(mapping->bind_client) > 0 ? mapping->bind_client : "");
                    } else {
                        log_message("添加映射: %d -> %s:%d%s%s",
                                    mapping->local_port,
                                    mapping->remote_host,
                                    mapping->remote_port,
                                    strlen(mapping->bind_client) > 0 ? " 指定主机: " : "",
                                    strlen(mapping->bind_client) > 0 ? mapping->bind_client : "");
                    }
                    break;
                }

                case ID_REMOVE_BUTTON: {
                    if (ctx->selected_mapping >= 0 && ctx->selected_mapping < ctx->mapping_count) {
                        PortMapping to_remove = ctx->mappings[ctx->selected_mapping];
                        if (ctx->mappings[ctx->selected_mapping].active) {
                            stop_port_mapping(&ctx->mappings[ctx->selected_mapping]);
                        }

                        for (int i = ctx->selected_mapping; i < ctx->mapping_count - 1; i++) {
                            ctx->mappings[i] = ctx->mappings[i + 1];
                        }

                        ctx->mapping_count--;

                        // Select the previous item if possible
                        if (ctx->mapping_count > 0) {
                            if (ctx->selected_mapping > 0) {
                                ctx->selected_mapping--;
                            } else {
                                ctx->selected_mapping = 0;
                            }
                            update_mapping_list(ctx);
                            load_selected_mapping(ctx);

                            EnableWindow(ctx->remove_button, TRUE);
                            EnableWindow(ctx->edit_button, TRUE);
                            update_controls_for_current_selection(ctx);
                        } else {
                            ctx->selected_mapping = -1;
                            update_mapping_list(ctx);
                            EnableWindow(ctx->remove_button, FALSE);
                            EnableWindow(ctx->edit_button, FALSE);
                            update_controls_for_current_selection(ctx);
                        }

                        save_config(CONFIG_FILE, ctx->mappings, ctx->mapping_count);

                        log_message("删除映射: %d -> %s:%d%s%s",
                                    to_remove.local_port,
                                    to_remove.remote_host,
                                    to_remove.remote_port,
                                    strlen(to_remove.bind_client) > 0 ? " 绑定客户端: " : "",
                                    strlen(to_remove.bind_client) > 0 ? to_remove.bind_client : "");
                    }
                    break;
                }

                case ID_EDIT_BUTTON: {
                    if (ctx->selected_mapping < 0 || ctx->selected_mapping >= ctx->mapping_count) {
                        break;
                    }
                    if (ctx->mappings[ctx->selected_mapping].active) {
                        MessageBox(hwnd, "请先停止该映射后再修改配置。", "提示", MB_OK | MB_ICONINFORMATION);
                        break;
                    }

                    char local_port_str[16], remote_host[256], remote_port_str[16];

                    GetWindowText(ctx->local_port_edit, local_port_str, sizeof(local_port_str));
                    GetWindowText(ctx->remote_host_edit, remote_host, sizeof(remote_host));
                    GetWindowText(ctx->remote_port_edit, remote_port_str, sizeof(remote_port_str));

                    int local_port = atoi(local_port_str);
                    int remote_port = atoi(remote_port_str);

                    if (local_port <= 0 || local_port > 65535) {
                        MessageBox(hwnd, "Invalid local port", "Error", MB_OK | MB_ICONERROR);
                        break;
                    }

                    if (remote_port <= 0 || remote_port > 65535) {
                        MessageBox(hwnd, "Invalid remote port", "Error", MB_OK | MB_ICONERROR);
                        break;
                    }

                    if (strlen(remote_host) == 0) {
                        MessageBox(hwnd, "Remote host is required", "Error", MB_OK | MB_ICONERROR);
                        break;
                    }

                    save_current_mapping(ctx);
                    PortMapping* mapping = &ctx->mappings[ctx->selected_mapping];
                    update_mapping_list(ctx);
                    save_config(CONFIG_FILE, ctx->mappings, ctx->mapping_count);

                    if (mapping->name[0]) {
                        log_message("修改映射 \"%s\": %d -> %s:%d%s%s",
                                    mapping->name,
                                    mapping->local_port,
                                    mapping->remote_host,
                                    mapping->remote_port,
                                    strlen(mapping->bind_client) > 0 ? " 指定主机: " : "",
                                    strlen(mapping->bind_client) > 0 ? mapping->bind_client : "");
                    } else {
                        log_message("修改映射: %d -> %s:%d%s%s",
                                    mapping->local_port,
                                    mapping->remote_host,
                                    mapping->remote_port,
                                    strlen(mapping->bind_client) > 0 ? " 指定主机: " : "",
                                    strlen(mapping->bind_client) > 0 ? mapping->bind_client : "");
                    }
                    break;
                }

                case ID_CLEAR_LOG: {
                    SendMessage(ctx->log_list, LB_RESETCONTENT, 0, 0);
                    break;
                }

                case ID_CONTEXT_START:
                case ID_CONTEXT_STOP:
                case ID_CONTEXT_REMOVE: {
                    int selected = mapping_listview_get_selection(ctx->mapping_list);
                    if (selected >= 0 && selected < ctx->mapping_count) {
                        save_current_mapping(ctx);
                        ctx->selected_mapping = selected;
                        load_selected_mapping(ctx);

                        if (LOWORD(wParam) == ID_CONTEXT_START) {
                            // Start the mapping
                            if (ctx->mappings[selected].active) {
                                // Already active, don't start again
                                MessageBox(hwnd, "Mapping already running", "Info", MB_OK | MB_ICONINFORMATION);
                                break;
                            }
                            enable_controls(ctx, FALSE);
                            CreateThread(NULL, 0, gui_forward_thread, ctx, 0, NULL);
                        } else if (LOWORD(wParam) == ID_CONTEXT_STOP) {
                            // Stop the mapping
                            stop_port_mapping(&ctx->mappings[selected]);
                            update_mapping_list(ctx);
                            update_controls_for_current_selection(ctx);
                        } else if (LOWORD(wParam) == ID_CONTEXT_REMOVE) {
                            // Remove the mapping
                            if (ctx->mappings[selected].active) {
                                stop_port_mapping(&ctx->mappings[selected]);
                            }

                            for (int i = selected; i < ctx->mapping_count - 1; i++) {
                                ctx->mappings[i] = ctx->mappings[i + 1];
                            }

                            ctx->mapping_count--;

                            // Select the previous item if possible
                            if (ctx->mapping_count > 0) {
                                if (selected > 0) {
                                    ctx->selected_mapping = selected - 1;
                                } else {
                                    ctx->selected_mapping = 0;
                                }
                                update_mapping_list(ctx);
                                load_selected_mapping(ctx);

                                EnableWindow(ctx->remove_button, TRUE);
                                EnableWindow(ctx->edit_button, TRUE);
                                update_controls_for_current_selection(ctx);
                            } else {
                                ctx->selected_mapping = -1;
                                update_mapping_list(ctx);
                                EnableWindow(ctx->remove_button, FALSE);
                                EnableWindow(ctx->edit_button, FALSE);
                                update_controls_for_current_selection(ctx);
                            }

                            save_config(CONFIG_FILE, ctx->mappings, ctx->mapping_count);


                        }
                    }
                    break;
                }

                case ID_TRAY_SHOW: {
                    ShowWindow(hwnd, SW_RESTORE);
                    SetForegroundWindow(hwnd);
                    break;
                }

                case ID_TRAY_EXIT: {
                    remove_tray_icon();
                    DestroyWindow(hwnd);
                    break;
                }
            }
            break;
        }

        case WM_NOTIFY: {
            LPNMHDR nh = (LPNMHDR)lParam;
            if (!nh || nh->hwndFrom != ctx->mapping_list) {
                break;
            }
            if (nh->code == NM_CUSTOMDRAW) {
                return mapping_listview_nm_customdraw(ctx, (NMLVCUSTOMDRAW*)lParam);
            }
            if (nh->code == LVN_ITEMCHANGED) {
                NMLISTVIEW* nmlv = (NMLISTVIEW*)lParam;
                if (s_mapping_list_updating) {
                    return 0;
                }
                if (!(nmlv->uChanged & LVIF_STATE)) {
                    break;
                }
                if (!(nmlv->uNewState & LVIS_SELECTED)) {
                    break;
                }
                int selected = nmlv->iItem;
                if (selected < 0 || selected >= ctx->mapping_count) {
                    break;
                }
                if (ctx->selected_mapping >= 0 && ctx->selected_mapping < ctx->mapping_count) {
                    save_current_mapping(ctx);
                }
                ctx->selected_mapping = selected;
                load_selected_mapping(ctx);
                EnableWindow(ctx->remove_button, TRUE);
                EnableWindow(ctx->edit_button, TRUE);
                update_controls_for_current_selection(ctx);
                return 0;
            }
            if (nh->code == NM_DBLCLK) {
                if (s_mapping_list_updating) {
                    return 0;
                }
                int selected = mapping_listview_get_selection(ctx->mapping_list);
                if (selected < 0 || selected >= ctx->mapping_count) {
                    return 0;
                }
                save_current_mapping(ctx);
                ctx->selected_mapping = selected;
                load_selected_mapping(ctx);
                if (ctx->mappings[selected].active) {
                    stop_port_mapping(&ctx->mappings[selected]);
                    update_controls_for_current_selection(ctx);
                } else {
                    enable_controls(ctx, FALSE);
                    CreateThread(NULL, 0, gui_forward_thread, ctx, 0, NULL);
                }
                update_mapping_list(ctx);
                return 0;
            }
            break;
        }

        case WM_APP_LOG_LINE: {
            char* s = (char*)lParam;
            if (g_log_list && s) {
                add_log_entry(g_log_list, s);
            }
            if (s) {
                free(s);
            }
            return 0;
        }

        case WM_SOCKET_NOTIFY: {
            switch (wParam) {
                case 0:
                    if (ctx->selected_mapping >= 0 && ctx->selected_mapping < ctx->mapping_count) {
                        ctx->mappings[ctx->selected_mapping].active = 0;
                    }
                    update_mapping_list(ctx);
                    update_controls_for_current_selection(ctx);
                    break;
                case 1:
                    if (ctx->selected_mapping >= 0 && ctx->selected_mapping < ctx->mapping_count) {
                        ctx->mappings[ctx->selected_mapping].active = 1;
                    }
                    update_mapping_list(ctx);
                    update_controls_for_current_selection(ctx);

                    break;
                case 2:
                    if (ctx->selected_mapping >= 0 && ctx->selected_mapping < ctx->mapping_count) {
                        ctx->mappings[ctx->selected_mapping].active = 0;
                    }
                    update_mapping_list(ctx);
                    update_controls_for_current_selection(ctx);

                    MessageBox(hwnd, "端口转发启动失败", "错误", MB_OK | MB_ICONERROR);
                    break;
            }
            break;
        }

        case WM_USER + 2: {
            // Tray icon message
            if (lParam == WM_LBUTTONDBLCLK) {
                // Double click - restore window
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
            } else if (lParam == WM_RBUTTONDOWN) {
                // Right click - show context menu
                POINT pt;
                GetCursorPos(&pt);
                show_tray_menu(hwnd, pt);
            }
            return 0;
        }

        case WM_CLOSE: {
            // Minimize to tray instead of closing
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }

        case WM_TIMER: {
            if (wParam == ID_TIMER_MAPPING_CONN) {
                refresh_mapping_list_connection_column(ctx);
                return 0;
            }
            break;
        }

        case WM_DESTROY: {
            KillTimer(hwnd, ID_TIMER_MAPPING_CONN);
            // Stop all active mappings
            for (int i = 0; i < ctx->mapping_count; i++) {
                if (ctx->mappings[i].active) {
                    stop_port_mapping(&ctx->mappings[i]);
                }
            }

            // Save configuration before exit
            save_config(CONFIG_FILE, ctx->mappings, ctx->mapping_count);

            // Remove tray icon
            remove_tray_icon();

            gui_cleanup_theme_gdi();

            PostQuitMessage(0);
            break;
        }


        case WM_SIZE: {
            int window_width = LOWORD(lParam);
            int window_height = HIWORD(lParam);

            // Use centralized resize logic to ensure consistency
            resize_controls(ctx, window_width, window_height);

            // Force a full redraw of the window to prevent ghosting
            InvalidateRect(hwnd, NULL, TRUE);
            UpdateWindow(hwnd);

            return 0;
        }

        case WM_CONTEXTMENU: {
            HWND hwnd_list = (HWND)wParam;
            if (hwnd_list == ctx->mapping_list) {
                POINT pt = { LOWORD(lParam), HIWORD(lParam) };

                // Get selected item
                int selected = mapping_listview_get_selection(ctx->mapping_list);
                if (selected >= 0 && selected < ctx->mapping_count) {
                    HMENU hMenu = CreatePopupMenu();
                    AppendMenu(hMenu, MF_STRING, ID_CONTEXT_START, ctx->mappings[selected].active ? "Restart" : "Start");
                    AppendMenu(hMenu, MF_STRING | (ctx->mappings[selected].active ? 0 : MF_GRAYED), ID_CONTEXT_STOP, "Stop");
                    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
                    AppendMenu(hMenu, MF_STRING, ID_CONTEXT_REMOVE, "Remove");

                    // Show context menu
                    TrackPopupMenu(hMenu, TPM_LEFTBUTTON | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                    DestroyMenu(hMenu);
                }
            }
            return 0;
        }

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    init_paths();
    // Hide console window more aggressively
    HWND hConsole = GetConsoleWindow();
    if (hConsole != NULL) {
        ShowWindow(hConsole, SW_HIDE);
        // Also try to free the console
        FreeConsole();
    }

    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        MessageBox(NULL, "WSAStartup失败", "错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    log_mutex = CreateMutex(NULL, FALSE, NULL);
    if (!log_mutex) {
        MessageBox(NULL, "创建日志互斥锁失败", "错误", MB_OK | MB_ICONERROR);
        WSACleanup();
        return 1;
    }

    if (!gui_init_theme_gdi()) {
        MessageBox(NULL, "界面字体初始化失败，将使用系统默认字体。", "提示", MB_OK | MB_ICONWARNING);
    }

    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES;
        InitCommonControlsEx(&icc);
    }

    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = hInstance;
    wc.hIcon = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_brush_client ? g_brush_client : (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "PortMapGUI";
    wc.hIconSm = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_SHARED);

    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, "窗口注册失败", "错误", MB_OK | MB_ICONERROR);
        WSACleanup();
        CloseHandle(log_mutex);
        return 1;
    }

    // Calculate window position for centering
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);
    int window_width = 1280;
    int window_height = 720;
    int pos_x = (screen_width - window_width) / 2;
    int pos_y = (screen_height - window_height) / 2;

    HWND hwnd = CreateWindowEx(
        0,
        "PortMapGUI",
        "端口映射工具 " VERSION " " CONTACT,
        WS_OVERLAPPEDWINDOW,  // Includes maximize box and resizable border
        pos_x, pos_y, window_width, window_height,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) {
        MessageBox(NULL, "窗口创建失败", "错误", MB_OK | MB_ICONERROR);
        WSACleanup();
        CloseHandle(log_mutex);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    WSACleanup();
    CloseHandle(log_mutex);

    return (int)msg.wParam;
}
