// 锁屏设置窗口实现（Win32 原生控件）
// 设计要点：三页 Tab（电源/屏保/动态锁）+ 底部状态栏；所有系统操作失败都在状态栏给中文提示，
// 不弹崩溃；耗时操作（powercfg）期间显示等待光标并禁用按钮，保证操作反馈明确。

#include "lockscreendlg.h"
#include "lockscreen.h"

#include <commctrl.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace lockscreen {

namespace {

// ---------------- 控件 ID ----------------
constexpr int kIdTab = 1000;

constexpr int kIdPowerLabelBase = 1100;   // +i*3
constexpr int kIdPowerAcBase = 1101;
constexpr int kIdPowerDcBase = 1102;

constexpr int kIdSaverInfo = 1200;
constexpr int kIdSaverOn = 1201;
constexpr int kIdSaverEdit = 1202;
constexpr int kIdSaverApply = 1203;
constexpr int kIdSaverPresetBase = 1210;   // 1210..1215

constexpr int kIdDynOn = 1300;
constexpr int kIdDynDevice = 1301;
constexpr int kIdDynKeys = 1302;
constexpr int kIdDynPaired = 1303;
constexpr int kIdDynOpenBt = 1304;
constexpr int kIdDynOpenDl = 1305;
constexpr int kIdDynWarn = 1306;

constexpr int kIdExport = 1400;
constexpr int kIdImport = 1401;
constexpr int kIdRefresh = 1402;
constexpr int kIdNeverAll = 1403;
constexpr int kIdRestore = 1404;

constexpr int kIdStatus = 1500;
constexpr int kIdInfo = 1501;

// 时间预设（秒）
const int kPresets[] = { 0, 60, 120, 300, 600, 900, 1200, 1800, 2700, 3600, 7200, 10800, 18000, 36000 };
const int kPresetCount = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

// 屏保预设（分钟）
const int kSaverPresets[] = { 1, 5, 10, 15, 30 };
const int kSaverPresetCount = (int)(sizeof(kSaverPresets) / sizeof(kSaverPresets[0]));

constexpr wchar_t kDlgClass[] = L"MechrevoLockScreenSettingsClass";

struct DlgState {
    HWND hwnd = nullptr;
    HWND tab = nullptr;
    HFONT hFont = nullptr;        // 正文
    HFONT hFontTitle = nullptr;   // 标题
    HFONT hFontSmall = nullptr;   // 小字说明
    int page = 0;                 // 0=电源 1=屏保 2=动态锁
    bool loading = false;         // 回填期间抑制变更事件
    bool closed = false;
    int dpi = 96;

    // 页面容器（用于整页显隐）
    std::vector<HWND> controls[3];

    // 电源页
    HWND powerLabel[(int)PowerKey::Count] = {};
    HWND powerAc[(int)PowerKey::Count] = {};
    HWND powerDc[(int)PowerKey::Count] = {};
    PowerState powerState[(int)PowerKey::Count] = {};
    bool powerAvail[(int)PowerKey::Count] = {};
    bool hasBattery = false;
    bool hibernateOk = false;

    // 屏保页
    HWND saverOn = nullptr;
    HWND saverEdit = nullptr;

    // 动态锁页
    HWND dynOn = nullptr;
    HWND dynDevice = nullptr;
    HWND dynKeys = nullptr;
    HWND dynPaired = nullptr;
    HWND dynWarn = nullptr;

    // 状态栏
    HWND status = nullptr;
    HWND info = nullptr;
    bool statusError = false;
    HBRUSH brGreen = nullptr;
    HBRUSH brRed = nullptr;
} g;

int S(int px) { return (px * g.dpi + 48) / 96; }

void SetStatus(const std::wstring& text, bool error) {
    g.statusError = error;
    if (g.status) {
        SetWindowTextW(g.status, text.c_str());
        InvalidateRect(g.status, nullptr, TRUE);
    }
}

void SetInfo(const std::wstring& text) {
    if (g.info)
        SetWindowTextW(g.info, text.c_str());
}

// 等待光标 + 禁用底部按钮（避免重入）
struct BusyGuard {
    BusyGuard() {
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        if (g.hwnd) {
            EnableWindow(GetDlgItem(g.hwnd, kIdExport), FALSE);
            EnableWindow(GetDlgItem(g.hwnd, kIdImport), FALSE);
            EnableWindow(GetDlgItem(g.hwnd, kIdRefresh), FALSE);
            EnableWindow(GetDlgItem(g.hwnd, kIdNeverAll), FALSE);
            EnableWindow(GetDlgItem(g.hwnd, kIdRestore), FALSE);
        }
    }
    ~BusyGuard() {
        if (g.hwnd) {
            EnableWindow(GetDlgItem(g.hwnd, kIdExport), TRUE);
            EnableWindow(GetDlgItem(g.hwnd, kIdImport), TRUE);
            EnableWindow(GetDlgItem(g.hwnd, kIdRefresh), TRUE);
            EnableWindow(GetDlgItem(g.hwnd, kIdNeverAll), TRUE);
            EnableWindow(GetDlgItem(g.hwnd, kIdRestore), TRUE);
        }
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
};

HWND MakeText(HWND parent, const wchar_t* text, int x, int y, int w, int h, HFONT font,
              int page, DWORD extraStyle = SS_LEFT) {
    HWND c = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | extraStyle, S(x), S(y),
                             S(w), S(h), parent, nullptr, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    if (page >= 0)
        g.controls[page].push_back(c);
    return c;
}

HWND MakeButton(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, int page,
                DWORD extraStyle = 0) {
    HWND c = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | extraStyle,
                             S(x), S(y), S(w), S(h), parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.hFont, TRUE);
    if (page >= 0)
        g.controls[page].push_back(c);
    return c;
}

HWND MakeCombo(HWND parent, int id, int x, int y, int w, int h, int page) {
    HWND c = CreateWindowExW(0, L"COMBOBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                             S(x), S(y), S(w), S(h), parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.hFont, TRUE);
    if (page >= 0)
        g.controls[page].push_back(c);
    return c;
}

HWND MakeCheck(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, int page) {
    HWND c = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                             S(x), S(y), S(w), S(h), parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.hFont, TRUE);
    if (page >= 0)
        g.controls[page].push_back(c);
    return c;
}

// ---------------- 下拉框填充与取值 ----------------

void FillComboWithSeconds(HWND combo, std::optional<int> current) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    bool found = false;
    int inserted = 0;
    for (int i = 0; i < kPresetCount; i++) {
        std::wstring text = FormatSeconds(kPresets[i]);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text.c_str());
        SendMessageW(combo, CB_SETITEMDATA, (WPARAM)inserted, (LPARAM)kPresets[i]);
        if (current && *current == kPresets[i])
            found = true;
        inserted++;
    }
    if (current && !found) {
        // 当前值不在预设中：追加一项并选中（显示"当前设置"）
        std::wstring text = FormatSeconds(*current);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text.c_str());
        SendMessageW(combo, CB_SETITEMDATA, (WPARAM)inserted, (LPARAM)*current);
        SendMessageW(combo, CB_SETCURSEL, (WPARAM)inserted, 0);
        return;
    }
    if (!current) {
        SendMessageW(combo, CB_SETCURSEL, (WPARAM)-1, 0);
        return;
    }
    for (int i = 0; i < SendMessageW(combo, CB_GETCOUNT, 0, 0); i++) {
        if (SendMessageW(combo, CB_GETITEMDATA, (WPARAM)i, 0) == (LPARAM)*current) {
            SendMessageW(combo, CB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    }
}

int SelectedSeconds(HWND combo) {
    int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel < 0)
        return -1;
    return (int)SendMessageW(combo, CB_GETITEMDATA, (WPARAM)sel, 0);
}

// ---------------- 建界面 ----------------

void BuildControls(HWND hwnd) {
    // 字体：Segoe UI + 中文字体回退
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = S(-15);
        g.hFont = CreateFontIndirectW(&lf);
        lf.lfHeight = S(-17);
        lf.lfWeight = FW_SEMIBOLD;
        g.hFontTitle = CreateFontIndirectW(&lf);
        lf.lfHeight = S(-13);
        lf.lfWeight = FW_NORMAL;
        g.hFontSmall = CreateFontIndirectW(&lf);
    } else {
        g.hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        g.hFontTitle = g.hFont;
        g.hFontSmall = g.hFont;
    }

    // Tab 控件
    g.tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
                            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS,
                            S(10), S(10), S(580), S(360), hwnd, (HMENU)(INT_PTR)kIdTab, nullptr, nullptr);
    SendMessageW(g.tab, WM_SETFONT, (WPARAM)g.hFont, TRUE);
    {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        const wchar_t* titles[3] = { L"电源设置", L"屏幕保护", L"动态锁" };
        for (int i = 0; i < 3; i++) {
            item.pszText = (LPWSTR)titles[i];
            SendMessageW(g.tab, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&item);
        }
    }

    // ===== 页面 0：电源设置 =====
    {
        const int page = 0;
        MakeText(hwnd,
                 L"锁屏后、闲置后多久关闭屏幕或进入睡眠。修改立即生效，无需重启。",
                 24, 46, 552, 20, g.hFontSmall, page);

        MakeText(hwnd, L"项目", 28, 76, 100, 18, g.hFontSmall, page);
        MakeText(hwnd, L"接通电源", 150, 76, 140, 18, g.hFontSmall, page, SS_CENTER);
        MakeText(hwnd, L"使用电池", 300, 76, 140, 18, g.hFontSmall, page, SS_CENTER);

        for (int i = 0; i < (int)PowerKey::Count; i++) {
            int y = 96 + i * 34;
            g.powerLabel[i] = MakeText(hwnd, PowerKeyName((PowerKey)i), 28, y + 4, 118, 20, g.hFont, page);
            g.powerAc[i] = MakeCombo(hwnd, kIdPowerAcBase + i * 3, 150, y, 140, 200, page);
            g.powerDc[i] = MakeCombo(hwnd, kIdPowerDcBase + i * 3, 300, y, 140, 200, page);
        }

        MakeText(hwnd, L"（「永不」= 不执行该动作）", 28, 236, 400, 18, g.hFontSmall, page);

        MakeButton(hwnd, L"全部设为永不", kIdNeverAll, 28, 260, 110, 28, page);
        MakeButton(hwnd, L"恢复电源计划默认", kIdRestore, 146, 260, 140, 28, page);
        MakeButton(hwnd, L"刷新", kIdRefresh, 294, 260, 80, 28, page);

        MakeText(hwnd,
                 L"说明：「恢复电源计划默认」会把当前电源方案的所有项目恢复为系统默认值，"
                 L"影响范围大于本页设置，请谨慎使用。",
                 28, 298, 540, 36, g.hFontSmall, page);
    }

    // ===== 页面 1：屏幕保护 =====
    {
        const int page = 1;
        MakeText(hwnd,
                 L"屏保启动后若勾选「在恢复时显示登录屏幕」，同样会进入锁屏界面。",
                 24, 46, 552, 20, g.hFontSmall, page);

        g.saverOn = MakeCheck(hwnd, L"启用屏幕保护", kIdSaverOn, 28, 78, 200, 24, page);

        MakeText(hwnd, L"等待时间（分钟）：", 28, 116, 130, 20, g.hFont, page);
        g.saverEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"15",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                      S(160), S(112), S(70), S(24), hwnd,
                                      (HMENU)(INT_PTR)kIdSaverEdit, nullptr, nullptr);
        SendMessageW(g.saverEdit, WM_SETFONT, (WPARAM)g.hFont, TRUE);
        g.controls[page].push_back(g.saverEdit);
        MakeButton(hwnd, L"应用", kIdSaverApply, 240, 112, 70, 26, page);

        MakeText(hwnd, L"快速预设：", 28, 152, 80, 20, g.hFont, page);
        for (int i = 0; i < kSaverPresetCount; i++) {
            wchar_t text[32] = {};
            swprintf_s(text, L"%d 分钟", kSaverPresets[i]);
            MakeButton(hwnd, text, kIdSaverPresetBase + i, 108 + i * 84, 148, 78, 26, page);
        }

        MakeText(hwnd,
                 L"屏保开关写入当前用户注册表并即时通知系统刷新；"
                 L"个别系统策略下需注销后完全生效。",
                 28, 190, 540, 36, g.hFontSmall, page);
    }

    // ===== 页面 2：动态锁 =====
    {
        const int page = 2;
        MakeText(hwnd,
                 L"利用已配对的手机蓝牙信号：带着手机离开电脑后自动锁屏。",
                 24, 46, 552, 20, g.hFontSmall, page);

        g.dynOn = MakeCheck(hwnd, L"启用动态锁", kIdDynOn, 28, 78, 200, 24, page);

        g.dynDevice = MakeText(hwnd, L"信任设备：读取中…", 28, 112, 540, 20, g.hFont, page);
        g.dynKeys = MakeText(hwnd, L"蓝牙密钥权限：读取中…", 28, 136, 540, 20, g.hFont, page);
        g.dynPaired = MakeText(hwnd, L"已配对设备：读取中…", 28, 160, 540, 20, g.hFont, page);

        MakeButton(hwnd, L"打开蓝牙设置", kIdDynOpenBt, 28, 190, 120, 28, page);
        MakeButton(hwnd, L"打开动态锁设置", kIdDynOpenDl, 156, 190, 130, 28, page);

        g.dynWarn = MakeText(hwnd, L"", 28, 226, 540, 76, g.hFontSmall, page);
    }

    // ===== 底部：按钮 + 状态栏 =====
    MakeButton(hwnd, L"导出配置", kIdExport, 320, 380, 84, 26, -1);
    MakeButton(hwnd, L"导入配置", kIdImport, 410, 380, 84, 26, -1);
    MakeButton(hwnd, L"关闭", IDCANCEL, 500, 380, 84, 26, -1);

    g.status = MakeText(hwnd, L"就绪", 14, 414, 570, 20, g.hFont, -1);
    g.info = MakeText(hwnd, L"", 14, 436, 570, 20, g.hFontSmall, -1);

    g.brGreen = CreateSolidBrush(RGB(0x16, 0x76, 0x2E));
    g.brRed = CreateSolidBrush(RGB(0xC0, 0x2B, 0x2B));

    // 初始只显示第 0 页
    for (int p = 1; p < 3; p++)
        for (HWND c : g.controls[p])
            ShowWindow(c, SW_HIDE);
}

void SwitchPage(int page) {
    if (page == g.page)
        return;
    for (HWND c : g.controls[g.page])
        ShowWindow(c, SW_HIDE);
    g.page = page;
    for (HWND c : g.controls[page])
        ShowWindow(c, SW_SHOW);
}

// ---------------- 业务动作 ----------------

void RefreshPowerPage() {
    std::vector<std::pair<PowerKey, PowerState>> items;
    std::wstring err;
    bool ok = PowerManager::QueryAll(items, err);

    // 不可用的下拉框填占位文本（避免空白框让人以为坏了）
    auto fillPlaceholder = [](HWND combo, const wchar_t* text) {
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text);
        SendMessageW(combo, CB_SETITEMDATA, 0, (LPARAM)-1);
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    };

    g.loading = true;
    for (int i = 0; i < (int)PowerKey::Count; i++) {
        PowerKey k = (PowerKey)i;
        PowerState ps{};
        for (const auto& p : items)
            if (p.first == k)
                ps = p.second;
        g.powerState[i] = ps;
        g.powerAvail[i] = ok;

        // 休眠不可用（hiberfil.sys 不存在）→ 该行禁用并标注
        bool avail = ok;
        std::wstring label = PowerKeyName(k);
        if (k == PowerKey::Hibernate && !g.hibernateOk) {
            avail = false;
            label += L"（未开启）";
        }
        SetWindowTextW(g.powerLabel[i], label.c_str());
        EnableWindow(g.powerAc[i], avail);
        EnableWindow(g.powerDc[i], avail && g.hasBattery);

        if (!avail)
            fillPlaceholder(g.powerAc[i], L"未启用");
        else
            FillComboWithSeconds(g.powerAc[i], ps.ac);

        if (!g.hasBattery)
            fillPlaceholder(g.powerDc[i], L"无电池");
        else if (!avail)
            fillPlaceholder(g.powerDc[i], L"未启用");
        else
            FillComboWithSeconds(g.powerDc[i], ps.dc);
    }
    g.loading = false;
}

void RefreshSaverPage() {
    SaverState sv = SaverManager::Get();
    g.loading = true;
    SendMessageW(g.saverOn, BM_SETCHECK, sv.active ? BST_CHECKED : BST_UNCHECKED, 0);
    wchar_t buf[32] = {};
    swprintf_s(buf, L"%d", sv.timeout / 60);
    SetWindowTextW(g.saverEdit, buf);
    g.loading = false;
}

void RefreshDynlockPage() {
    DynlockState dl = DynlockManager::Get();
    g.loading = true;
    SendMessageW(g.dynOn, BM_SETCHECK, dl.enabled ? BST_CHECKED : BST_UNCHECKED, 0);

    std::wstring dev = L"信任设备：";
    if (dl.selectedMac.empty())
        dev += L"未选择（开启动态锁时将自动选择）";
    else if (dl.deviceSelected)
        dev += dl.selectedName + L"  ✔ 有效";
    else
        dev += dl.selectedName + L"  ⚠ 该设备已不在蓝牙设备列表中";
    SetWindowTextW(g.dynDevice, dev.c_str());

    std::wstring keys = L"蓝牙密钥权限：";
    if (dl.keysHealth == L"healthy")
        keys += L"正常（" + std::to_wstring(dl.keysAce) + L" 项权限）";
    else if (dl.keysHealth == L"broken")
        keys += L"异常（仅 " + std::to_wstring(dl.keysAce) + L" 项权限，可能导致动态锁失效；"
                L"可尝试删除并重新配对手机）";
    else
        keys += L"无法检测（需要管理员权限）";
    SetWindowTextW(g.dynKeys, keys.c_str());

    std::wstring paired = L"已配对设备：" + std::to_wstring(dl.pairedCount) + L" 台";
    if (dl.pairedCount > 0)
        paired += L"（其中手机 " + std::to_wstring(dl.phoneCount) + L" 台）";
    if (dl.lockdownOnLeave)
        paired += L"　·　Win11「离开时锁定」：已开启";
    SetWindowTextW(g.dynPaired, paired.c_str());

    g.loading = false;
}

void RefreshInfoLine() {
    std::wstring name, guid;
    PowerManager::ActiveScheme(name, guid);
    std::wstring info = L"电源计划：" + name;
    info += L"　|　休眠：" + std::wstring(g.hibernateOk ? L"可用" : L"未开启");
    info += L"　|　电池：" + std::wstring(g.hasBattery ? L"检测到" : L"无");
    SetInfo(info);
}

void RefreshAll() {
    BusyGuard busy;
    g.hasBattery = PowerManager::HasBattery();
    g.hibernateOk = PowerManager::HibernateAvailable();
    RefreshPowerPage();
    RefreshSaverPage();
    RefreshDynlockPage();
    RefreshInfoLine();
}

void ApplyPowerItem(PowerKey k) {
    int i = (int)k;
    int acSec = SelectedSeconds(g.powerAc[i]);
    int dcSec = SelectedSeconds(g.powerDc[i]);
    if (acSec < 0 || dcSec < 0)
        return;

    BusyGuard busy;
    std::vector<std::wstring> errors;
    // 分别写 AC / DC，最后统一激活一次
    PowerManager::SetItem(k, acSec, "ac", false, errors);
    PowerManager::SetItem(k, dcSec, "dc", false, errors);
    if (errors.empty()) {
        std::wstring err;
        if (!PowerManager::Activate(err))
            errors.push_back(L"使设置生效: " + err);
    }

    if (errors.empty()) {
        SetStatus(std::wstring(PowerKeyName(k)) + L" 已设为：接通电源 " + FormatSeconds(acSec) +
                      L"、使用电池 " + FormatSeconds(dcSec),
                  false);
    } else {
        std::wstring msg = L"设置失败：";
        for (size_t n = 0; n < errors.size(); n++) {
            if (n)
                msg += L"；";
            msg += errors[n];
        }
        SetStatus(msg, true);
    }
}

void ApplySaver() {
    bool on = SendMessageW(g.saverOn, BM_GETCHECK, 0, 0) == BST_CHECKED;
    wchar_t buf[32] = {};
    GetWindowTextW(g.saverEdit, buf, 32);
    int minutes = _wtoi(buf);
    if (minutes < 1)
        minutes = 1;
    if (minutes > 9999)
        minutes = 9999;
    {
        wchar_t norm[32] = {};
        swprintf_s(norm, L"%d", minutes);
        SetWindowTextW(g.saverEdit, norm);
    }

    BusyGuard busy;
    std::vector<std::wstring> errors;
    SaverManager::Set(on, minutes * 60, std::nullopt, errors);
    if (errors.empty()) {
        SetStatus(on ? (L"屏幕保护已启用，等待 " + std::to_wstring(minutes) + L" 分钟")
                     : std::wstring(L"屏幕保护已关闭"),
                  false);
    } else {
        std::wstring msg = L"屏幕保护设置失败：";
        for (size_t n = 0; n < errors.size(); n++) {
            if (n)
                msg += L"；";
            msg += errors[n];
        }
        SetStatus(msg, true);
    }
}

void ApplyDynlock(bool on) {
    BusyGuard busy;
    std::wstring warning, error;
    if (!DynlockManager::Set(on, warning, error)) {
        SetStatus(L"动态锁设置失败：" + (error.empty() ? L"未知错误" : error), true);
        g.loading = true;
        SendMessageW(g.dynOn, BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
        g.loading = false;
        return;
    }

    std::wstring msg = on ? L"动态锁已开启" : L"动态锁已关闭（含 Win11「离开时锁定」独立开关）";
    if (!warning.empty())
        msg += L"　｜ " + warning;
    SetStatus(msg, false);
    RefreshDynlockPage();
}

void DoExport() {
    wchar_t file[MAX_PATH] = L"锁屏设置.json";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"JSON 配置 (*.json)\0*.json\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn))
        return;

    BusyGuard busy;
    std::wstring outPath, err;
    if (ConfigIO::Export(file, outPath, err)) {
        SetStatus(L"已导出配置：" + outPath, false);
    } else {
        SetStatus(L"导出失败：" + err, true);
    }
}

void DoImport() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"JSON 配置 (*.json)\0*.json\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn))
        return;

    std::wstring confirm = L"将把配置文件中的设置应用到本机：\n\n" + std::wstring(file) +
                          L"\n\n电源计划 / 屏幕保护 / 动态锁都会被覆盖，是否继续？";
    if (MessageBoxW(g.hwnd, confirm.c_str(), L"导入配置", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    BusyGuard busy;
    std::vector<std::wstring> results, errors;
    bool ok = ConfigIO::Import(file, results, errors);

    RefreshAll();

    std::wstring msg;
    for (const auto& r : results)
        msg += r + L"\r\n";
    for (const auto& e : errors)
        msg += L"✗ " + e + L"\r\n";
    if (msg.empty())
        msg = L"配置文件中没有可应用的设置项。";

    MessageBoxW(g.hwnd, msg.c_str(), ok ? L"导入完成" : L"导入完成（有失败项）",
                MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));

    if (ok)
        SetStatus(L"配置导入完成，共应用 " + std::to_wstring(results.size()) + L" 组设置", false);
    else
        SetStatus(L"配置导入完成，但有 " + std::to_wstring(errors.size()) + L" 项失败", true);
}

void DoNeverAll() {
    if (MessageBoxW(g.hwnd,
                    L"将把「锁屏后黑屏 / 关闭显示器 / 睡眠 / 休眠」全部设为「永不」。\n\n"
                    L"这会让电脑在闲置时不再自动熄屏或睡眠（适合长时间演示），是否继续？",
                    L"全部设为永不", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    BusyGuard busy;
    std::vector<std::wstring> errors;
    for (int i = 0; i < (int)PowerKey::Count; i++) {
        PowerKey k = (PowerKey)i;
        if (k == PowerKey::Hibernate && !g.hibernateOk)
            continue;   // 休眠未开启时跳过，避免报错
        PowerManager::SetItem(k, 0, "both", false, errors);
    }
    std::wstring err;
    if (!PowerManager::Activate(err))
        errors.push_back(L"使设置生效: " + err);

    RefreshPowerPage();
    if (errors.empty()) {
        SetStatus(L"已将全部电源项设为「永不」", false);
    } else {
        std::wstring msg = L"部分设置失败：";
        for (size_t n = 0; n < errors.size(); n++) {
            if (n)
                msg += L"；";
            msg += errors[n];
        }
        SetStatus(msg, true);
    }
}

void DoRestoreDefaults() {
    if (MessageBoxW(g.hwnd,
                    L"将把当前电源计划恢复为系统默认值。\n\n"
                    L"注意：这会重置该电源计划下的所有设置项（不止本页显示的四项），是否继续？",
                    L"恢复电源计划默认", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    BusyGuard busy;
    std::wstring err;
    if (PowerManager::RestoreDefaults(err)) {
        RefreshAll();
        SetStatus(L"电源计划已恢复为系统默认值", false);
    } else {
        SetStatus(L"恢复默认失败：" + err, true);
    }
}

// ---------------- 窗口过程 ----------------

LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g.hwnd = hwnd;
            g.dpi = (int)GetDpiForWindow(hwnd);
            if (g.dpi <= 0)
                g.dpi = 96;
            BuildControls(hwnd);
            RefreshAll();
            SetStatus(L"就绪。修改任意下拉框会立即生效。", false);
            return 0;

        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);

            // 电源页下拉变更 → 立即应用
            for (int i = 0; i < (int)PowerKey::Count; i++) {
                if (id == kIdPowerAcBase + i * 3 || id == kIdPowerDcBase + i * 3) {
                    if (code == CBN_SELCHANGE && !g.loading)
                        ApplyPowerItem((PowerKey)i);
                    return 0;
                }
            }

            // 屏保预设
            for (int i = 0; i < kSaverPresetCount; i++) {
                if (id == kIdSaverPresetBase + i && code == BN_CLICKED) {
                    wchar_t buf[16] = {};
                    swprintf_s(buf, L"%d", kSaverPresets[i]);
                    SetWindowTextW(g.saverEdit, buf);
                    SendMessageW(g.saverOn, BM_SETCHECK, BST_CHECKED, 0);
                    ApplySaver();
                    return 0;
                }
            }

            switch (id) {
                case kIdSaverOn:
                    if (code == BN_CLICKED && !g.loading)
                        ApplySaver();
                    return 0;

                case kIdSaverApply:
                    if (code == BN_CLICKED)
                        ApplySaver();
                    return 0;

                case kIdDynOn:
                    if (code == BN_CLICKED && !g.loading)
                        ApplyDynlock(SendMessageW(g.dynOn, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    return 0;

                case kIdDynOpenBt:
                    if (!DynlockManager::OpenSettings(L"bluetooth"))
                        SetStatus(L"无法打开蓝牙设置页", true);
                    return 0;

                case kIdDynOpenDl:
                    if (!DynlockManager::OpenSettings(L"dynamiclock"))
                        SetStatus(L"无法打开动态锁设置页", true);
                    return 0;

                case kIdExport:
                    DoExport();
                    return 0;

                case kIdImport:
                    DoImport();
                    return 0;

                case kIdRefresh:
                    RefreshAll();
                    SetStatus(L"已刷新当前设置", false);
                    return 0;

                case kIdNeverAll:
                    DoNeverAll();
                    return 0;

                case kIdRestore:
                    DoRestoreDefaults();
                    return 0;

                case IDCANCEL:
                    DestroyWindow(hwnd);
                    return 0;

                default:
                    break;
            }
            return 0;
        }

        case WM_NOTIFY: {
            LPNMHDR hdr = (LPNMHDR)lp;
            if (hdr->idFrom == kIdTab && hdr->code == TCN_SELCHANGE) {
                int sel = (int)SendMessageW(g.tab, TCM_GETCURSEL, 0, 0);
                SwitchPage(sel);
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            HWND c = (HWND)lp;
            if (c == g.status && g.statusError) {
                SetTextColor(dc, RGB(0xC0, 0x2B, 0x2B));
                SetBkMode(dc, TRANSPARENT);
                return (LRESULT)g.brRed;
            }
            if (c == g.status) {
                SetTextColor(dc, RGB(0x16, 0x76, 0x2E));
                SetBkMode(dc, TRANSPARENT);
                return (LRESULT)g.brGreen;
            }
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g.closed = true;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void ShowLockScreenWindow(HWND parent) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = DlgProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kDlgClass;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    RECT rc{ 0, 0, 600, 466 };
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED | WS_VISIBLE;
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    g.closed = false;
    g.page = 0;

    HWND dlg = CreateWindowExW(0, kDlgClass, L"锁屏设置",
                               style, x, y, w, h, parent, nullptr, hInst, nullptr);
    if (!dlg) {
        MessageBoxW(parent, L"锁屏设置窗口创建失败。", L"锁屏设置", MB_OK | MB_ICONERROR);
        return;
    }

    SetForegroundWindow(dlg);
    if (parent)
        EnableWindow(parent, FALSE);

    MSG msg;
    while (!g.closed && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (parent)
        EnableWindow(parent, TRUE);

    // 释放资源
    if (g.hFont && g.hFont != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(g.hFont);
        DeleteObject(g.hFontTitle);
        DeleteObject(g.hFontSmall);
    }
    if (g.brGreen)
        DeleteObject(g.brGreen);
    if (g.brRed)
        DeleteObject(g.brRed);
    g.hFont = g.hFontTitle = g.hFontSmall = nullptr;
    g.brGreen = g.brRed = nullptr;
    for (int p = 0; p < 3; p++)
        g.controls[p].clear();
    g.hwnd = nullptr;
    g.tab = nullptr;
}

}  // namespace lockscreen
