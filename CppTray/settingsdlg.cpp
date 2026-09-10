#include "settingsdlg.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr int kIdIntervalEdit = 101;
constexpr int kIdAutoStartCheck = 102;
constexpr int kIdOk = 103;
constexpr int kIdCancel = 104;
constexpr int kIdShowBase = 110;   // 110..115 = 功耗/风扇/占用/温度/内存/网速 勾选框
constexpr int kIdBrightCheck = 105;
constexpr int kIdCtEnable = 106;
constexpr int kIdCtDay = 107;
constexpr int kIdCtNight = 108;
constexpr int kIdCtSunrise = 109;
constexpr int kIdCtSunset = 116;

constexpr wchar_t kDlgClass[] = L"MechrevoSettingsDlgClass";

struct DlgCtx {
    AppConfig* cfg;
    HWND edit;
    HWND check;
    HWND bright;
    HWND ctEnable;
    HWND ctDay, ctNight, ctSunrise, ctSunset;
    HWND show[6] = {};
    bool ok = false;
    bool closed = false;
};

bool IsChecked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

int ClampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// "HH:MM" → 分钟；失败返回 false
bool ParseHHMM(const wchar_t* s, int* minutes) {
    int h = 0, m = 0;
    if (swscanf_s(s, L"%d:%d", &h, &m) != 2)
        return false;
    if (h < 0 || h > 23 || m < 0 || m > 59)
        return false;
    *minutes = h * 60 + m;
    return true;
}

void FormatHHMM(int minutes, wchar_t* out, size_t n) {
    swprintf_s(out, n, L"%02d:%02d", minutes / 60, minutes % 60);
}

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
                ctx->cfg->BrightnessKeysEnabled = IsChecked(ctx->bright);

                // 色温设置
                ctx->cfg->ColorTempEnabled = IsChecked(ctx->ctEnable);
                wchar_t tmp[32] = {};
                GetWindowTextW(ctx->ctDay, tmp, 32);
                ctx->cfg->ColorTempDayK = ClampInt(_wtoi(tmp), 1000, 10000);
                GetWindowTextW(ctx->ctNight, tmp, 32);
                ctx->cfg->ColorTempNightK = ClampInt(_wtoi(tmp), 1000, 10000);
                int mins = 0;
                GetWindowTextW(ctx->ctSunrise, tmp, 32);
                if (ParseHHMM(tmp, &mins))
                    ctx->cfg->ColorTempSunriseMinutes = mins;
                GetWindowTextW(ctx->ctSunset, tmp, 32);
                if (ParseHHMM(tmp, &mins))
                    ctx->cfg->ColorTempSunsetMinutes = mins;

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
                               CW_USEDEFAULT, CW_USEDEFAULT, 362, 558,
                               parent, nullptr, hInst, &ctx);
    if (!dlg)
        return false;
    // 置前：从托盘菜单弹出时确保可见可交互
    SetForegroundWindow(dlg);

    // —— 统一 UI 字体（Segoe UI / 系统消息字体）——
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    HFONT uiFont = nullptr;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
    auto addCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w,
                       int h, int id) -> HWND {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, dlg,
                                 (HMENU)(INT_PTR)id, hInst, nullptr);
        if (uiFont)
            SendMessageW(c, WM_SETFONT, (WPARAM)uiFont, TRUE);
        return c;
    };
    auto addGroup = [&](const wchar_t* text, int x, int y, int w, int h) {
        HWND c = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_GROUPBOX, x, y, w, h,
                                 dlg, nullptr, hInst, nullptr);
        if (uiFont)
            SendMessageW(c, WM_SETFONT, (WPARAM)uiFont, TRUE);
        return c;
    };

    // —— 分组一：采样 ——
    addGroup(L"采样", 14, 8, 320, 62);
    addCtrl(L"STATIC", L"刷新间隔（毫秒，250~60000）：", SS_LEFT, 26, 32, 210, 18, 0);
    wchar_t msBuf[16] = {};
    swprintf_s(msBuf, L"%d", cfg.RefreshIntervalMs);
    ctx.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", msBuf,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                               252, 30, 70, 22, dlg, (HMENU)(INT_PTR)kIdIntervalEdit, hInst, nullptr);
    if (uiFont)
        SendMessageW(ctx.edit, WM_SETFONT, (WPARAM)uiFont, TRUE);

    // —— 分组二：任务栏显示项 ——
    addGroup(L"任务栏显示项（数值在上、标签在下，从左到右排列）", 14, 76, 320, 150);
    struct ShowItem {
        const wchar_t* text;
        bool on;
        int x, y;
        HWND* slot;
    };
    ShowItem shows[6] = {
        { L"功耗", cfg.ShowPower, 28, 100, &ctx.show[0] },
        { L"风扇转速", cfg.ShowFan, 172, 100, &ctx.show[1] },
        { L"CPU 占用", cfg.ShowCpuUsage, 28, 126, &ctx.show[2] },
        { L"CPU 温度", cfg.ShowCpuTemp, 172, 126, &ctx.show[3] },
        { L"内存占用", cfg.ShowMem, 28, 152, &ctx.show[4] },
        { L"网速上下行", cfg.ShowNet, 172, 152, &ctx.show[5] },
    };
    for (int i = 0; i < 6; i++) {
        *shows[i].slot = addCtrl(L"BUTTON", shows[i].text,
                                 WS_TABSTOP | BS_AUTOCHECKBOX, shows[i].x, shows[i].y, 142, 22,
                                 kIdShowBase + i);
        SendMessageW(*shows[i].slot, BM_SETCHECK, shows[i].on ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    addCtrl(L"STATIC", L"取消勾选的项目不会显示在任务栏上。", SS_LEFT, 28, 178, 280, 16, 0);

    // —— 分组三：启动 ——
    addGroup(L"启动", 14, 232, 320, 56);
    ctx.check = addCtrl(L"BUTTON", L"开机自启（计划任务，登录即提权）",
                        WS_TABSTOP | BS_AUTOCHECKBOX, 26, 252, 290, 22, kIdAutoStartCheck);
    SendMessageW(ctx.check, BM_SETCHECK, cfg.AutoStart ? BST_CHECKED : BST_UNCHECKED, 0);

    // —— 分组四：屏幕亮度快捷键（DDC/CI，仅外接显示器）——
    addGroup(L"屏幕亮度（DDC/CI，仅外接显示器）", 14, 294, 320, 56);
    ctx.bright = addCtrl(L"BUTTON", L"启用快捷键调节亮度（Ctrl+Alt+↑ / ↓，每次 ±10%）",
                         WS_TABSTOP | BS_AUTOCHECKBOX, 26, 316, 292, 22, kIdBrightCheck);
    SendMessageW(ctx.bright, BM_SETCHECK, cfg.BrightnessKeysEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

    // —— 分组五：色温护眼（LightBulb 引擎）——
    addGroup(L"色温护眼（LightBulb 引擎，日落变暖）", 14, 356, 320, 142);
    wchar_t ctBuf[16] = {};
    ctx.ctEnable = addCtrl(L"BUTTON", L"启用自动色温（Ctrl+Alt+PgUp/PgDn 手动微调）",
                           WS_TABSTOP | BS_AUTOCHECKBOX, 26, 378, 292, 22, kIdCtEnable);
    SendMessageW(ctx.ctEnable, BM_SETCHECK, cfg.ColorTempEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

    addCtrl(L"STATIC", L"白天色温 (K)：", SS_LEFT, 26, 404, 110, 18, 0);
    swprintf_s(ctBuf, L"%d", cfg.ColorTempDayK);
    ctx.ctDay = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ctBuf,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                150, 402, 72, 22, dlg, (HMENU)(INT_PTR)kIdCtDay, hInst, nullptr);
    if (uiFont) SendMessageW(ctx.ctDay, WM_SETFONT, (WPARAM)uiFont, TRUE);

    addCtrl(L"STATIC", L"夜间色温 (K)：", SS_LEFT, 26, 430, 110, 18, 0);
    swprintf_s(ctBuf, L"%d", cfg.ColorTempNightK);
    ctx.ctNight = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ctBuf,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                  150, 428, 72, 22, dlg, (HMENU)(INT_PTR)kIdCtNight, hInst, nullptr);
    if (uiFont) SendMessageW(ctx.ctNight, WM_SETFONT, (WPARAM)uiFont, TRUE);

    addCtrl(L"STATIC", L"日出时间 (HH:MM)：", SS_LEFT, 26, 456, 120, 18, 0);
    FormatHHMM(cfg.ColorTempSunriseMinutes, ctBuf, 16);
    ctx.ctSunrise = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ctBuf,
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                    150, 454, 72, 22, dlg, (HMENU)(INT_PTR)kIdCtSunrise, hInst, nullptr);
    if (uiFont) SendMessageW(ctx.ctSunrise, WM_SETFONT, (WPARAM)uiFont, TRUE);

    addCtrl(L"STATIC", L"日落时间 (HH:MM)：", SS_LEFT, 26, 482, 120, 18, 0);
    FormatHHMM(cfg.ColorTempSunsetMinutes, ctBuf, 16);
    ctx.ctSunset = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ctBuf,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                   150, 480, 72, 22, dlg, (HMENU)(INT_PTR)kIdCtSunset, hInst, nullptr);
    if (uiFont) SendMessageW(ctx.ctSunset, WM_SETFONT, (WPARAM)uiFont, TRUE);

    // —— 按钮 ——
    addCtrl(L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, 130, 510, 82, 28, kIdOk);
    addCtrl(L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON, 226, 510, 82, 28, kIdCancel);

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
    if (uiFont)
        DeleteObject(uiFont);
    return ctx.ok;
}
