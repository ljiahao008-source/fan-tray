#include "settingsdlg.h"

#include <cstdlib>
#include <string>

namespace {

constexpr int kIdIntervalEdit = 101;
constexpr int kIdAutoStartCheck = 102;
constexpr int kIdOk = 103;
constexpr int kIdCancel = 104;

constexpr wchar_t kDlgClass[] = L"MechrevoSettingsDlgClass";

struct DlgCtx {
    AppConfig* cfg;
    HWND edit;
    HWND check;
    bool ok = false;
    bool closed = false;
};

LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DlgCtx* ctx = (DlgCtx*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        ctx = (DlgCtx*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);
    }
    if (!ctx)
        return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == kIdOk) {
                wchar_t buf[32] = {};
                GetWindowTextW(ctx->edit, buf, 32);
                int ms = _wtoi(buf);
                if (ms >= 250 && ms <= 60000)
                    ctx->cfg->RefreshIntervalMs = ms;
                ctx->cfg->AutoStart = (SendMessageW(ctx->check, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->ok = true;
                ctx->closed = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == kIdCancel) {
                ctx->closed = true;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE: {
            ctx->closed = true;
            DestroyWindow(hwnd);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool ShowSettingsDialog(HWND parent, AppConfig& cfg) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = DlgProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kDlgClass;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    DlgCtx ctx;
    ctx.cfg = &cfg;

    HWND dlg = CreateWindowExW(0, kDlgClass, L"机械革命监控 - 设置",
                               WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED,
                               CW_USEDEFAULT, CW_USEDEFAULT, 340, 190,
                               parent, nullptr, hInst, &ctx);
    if (!dlg)
        return false;

    // 刷新间隔
    CreateWindowExW(0, L"STATIC", L"刷新间隔（毫秒，250~60000）：",
                    WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 20, 220, 18, dlg, nullptr, hInst, nullptr);
    wchar_t msBuf[16] = {};
    swprintf_s(msBuf, L"%d", cfg.RefreshIntervalMs);
    ctx.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", msBuf,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                               240, 18, 80, 22, dlg, (HMENU)(INT_PTR)kIdIntervalEdit, hInst, nullptr);

    // 开机自启
    ctx.check = CreateWindowExW(0, L"BUTTON", L"开机自启（计划任务，登录即提权）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                20, 56, 280, 22, dlg, (HMENU)(INT_PTR)kIdAutoStartCheck, hInst, nullptr);
    SendMessageW(ctx.check, BM_SETCHECK, cfg.AutoStart ? BST_CHECKED : BST_UNCHECKED, 0);

    // 按钮
    CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    120, 100, 80, 28, dlg, (HMENU)(INT_PTR)kIdOk, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    210, 100, 80, 28, dlg, (HMENU)(INT_PTR)kIdCancel, hInst, nullptr);

    if (parent)
        EnableWindow(parent, FALSE);

    MSG msg;
    while (!ctx.closed && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (parent)
        EnableWindow(parent, TRUE);

    DestroyWindow(dlg);
    return ctx.ok;
}
