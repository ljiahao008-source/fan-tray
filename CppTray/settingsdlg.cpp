#include "settingsdlg.h"

#include <cstdlib>
#include <string>

namespace {

constexpr int kIdIntervalEdit = 101;
constexpr int kIdAutoStartCheck = 102;
constexpr int kIdOk = 103;
constexpr int kIdCancel = 104;
constexpr int kIdShowBase = 110;   // 110..115 = 功耗/风扇/占用/温度/内存/网速 勾选框

constexpr wchar_t kDlgClass[] = L"MechrevoSettingsDlgClass";

struct DlgCtx {
    AppConfig* cfg;
    HWND edit;
    HWND check;
    HWND show[6] = {};
    bool ok = false;
    bool closed = false;
};

bool IsChecked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

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
                ctx->cfg->AutoStart = IsChecked(ctx->check);
                ctx->cfg->ShowPower = IsChecked(ctx->show[0]);
                ctx->cfg->ShowFan = IsChecked(ctx->show[1]);
                ctx->cfg->ShowCpuUsage = IsChecked(ctx->show[2]);
                ctx->cfg->ShowCpuTemp = IsChecked(ctx->show[3]);
                ctx->cfg->ShowMem = IsChecked(ctx->show[4]);
                ctx->cfg->ShowNet = IsChecked(ctx->show[5]);
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
                               WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 348, 306,
                               parent, nullptr, hInst, &ctx);
    if (!dlg)
        return false;
    // 置前：从托盘菜单弹出时确保可见可交互
    SetForegroundWindow(dlg);

    // —— 刷新间隔 ——
    CreateWindowExW(0, L"STATIC", L"刷新间隔（毫秒，250~60000）：",
                    WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 18, 220, 18, dlg, nullptr, hInst, nullptr);
    wchar_t msBuf[16] = {};
    swprintf_s(msBuf, L"%d", cfg.RefreshIntervalMs);
    ctx.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", msBuf,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                               244, 16, 80, 22, dlg, (HMENU)(INT_PTR)kIdIntervalEdit, hInst, nullptr);

    // —— 开机自启 ——
    ctx.check = CreateWindowExW(0, L"BUTTON", L"开机自启（计划任务，登录即提权）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                20, 50, 300, 22, dlg, (HMENU)(INT_PTR)kIdAutoStartCheck, hInst, nullptr);
    SendMessageW(ctx.check, BM_SETCHECK, cfg.AutoStart ? BST_CHECKED : BST_UNCHECKED, 0);

    // —— 任务栏显示项（两列三行）——
    CreateWindowExW(0, L"STATIC", L"任务栏显示项（数值在上、标签在下，从左到右排列）：",
                    WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 82, 300, 18, dlg, nullptr, hInst, nullptr);

    struct { const wchar_t* text; bool on; int x, y; } items[6] = {
        { L"功耗", cfg.ShowPower, 20, 106 },
        { L"风扇转速", cfg.ShowFan, 180, 106 },
        { L"CPU 占用", cfg.ShowCpuUsage, 20, 132 },
        { L"CPU 温度", cfg.ShowCpuTemp, 180, 132 },
        { L"内存占用", cfg.ShowMem, 20, 158 },
        { L"网速上下行", cfg.ShowNet, 180, 158 },
    };
    for (int i = 0; i < 6; i++) {
        ctx.show[i] = CreateWindowExW(0, L"BUTTON", items[i].text,
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                      items[i].x, items[i].y, 150, 22, dlg,
                                      (HMENU)(INT_PTR)(kIdShowBase + i), hInst, nullptr);
        SendMessageW(ctx.show[i], BM_SETCHECK, items[i].on ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    // —— 按钮 ——
    CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    132, 196, 82, 28, dlg, (HMENU)(INT_PTR)kIdOk, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    226, 196, 82, 28, dlg, (HMENU)(INT_PTR)kIdCancel, hInst, nullptr);

    SetFocus(ctx.edit);

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
