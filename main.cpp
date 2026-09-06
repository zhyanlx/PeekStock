// ============================================================================
// PeekStock (原生 Win32 版)
// 与 PyQt5 版 (E:\dev\deskopStockView\deskopStockView.py) 功能对等：
//  - 无边框/置顶/半透明圆角悬浮窗，可拖动，鼠标移出收缩成文字便签(精简模式)
//  - 自选股行情每 5 秒批量拉取(单请求)，上证指数常驻最底部，红涨绿跌
//  - 成本价设置/清除，显示持仓盈亏；按涨跌幅排序；固定；删除
//  - 输入框支持 6 位代码或中文名(新浪suggest查询，后台线程不卡界面)
//  - Ctrl+Q 全局热键显隐；隐藏期间不请求，显示瞬间若距上次>=5秒立即拉取
//  - 配置 %APPDATA%\PeekStock\conf.json，先写临时文件再原子替换
// 构建 (w64devkit/MinGW):
//   g++ -O2 -std=c++17 -municode -mwindows main.cpp -lgdiplus -lwinhttp
//       -limm32 -lshell32 -lgdi32 -o PeekStock.exe
// 自测: g++ -O2 -std=c++17 -DSELFTEST -municode -mconsole main.cpp -lwinhttp -o selftest.exe
// ============================================================================
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <imm.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <process.h>

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// 常量 (逻辑像素，96DPI 基准，运行时按实际 DPI 缩放)
// ---------------------------------------------------------------------------
static const wchar_t* APP_CLASS  = L"PeekStockWnd";
static const wchar_t* APP_DLGCLS = L"PeekStockDlg";
static const wchar_t* kPlaceholder = L"股票代码/名称";

static const int kWinW = 450;      // 展开模式窗口宽
static const int kFullH = 86;      // 展开模式高度基数(无股票时)
static const int kRowStep = 33;    // 每只股票占用高度
static const int kMinH = 100, kMaxH = 800;
static const int kToolbarY = 6, kCtrlH = 26;
static const int kRowY0 = 39, kRowH = 30;
static const int kBtnAddW = 55, kBtnSortW = 60, kBtnFixW = 55, kBtnCloseW = 26;
static const int kRowBtnW = 45, kRowBtnH = 24;
static const ULONGLONG kFetchIntervalMs = 5000;

// 颜色与 Qt 版一致 (green=#00FF00, red=#FF0000)
static Color ColPanel()    { return Color(230, 30, 30, 30); }
static Color ColToolbar()  { return Color(255, 51, 51, 51); }
static Color ColToolbarH() { return Color(255, 68, 68, 68); }
static Color ColRowBtn()   { return Color(255, 68, 68, 68); }
static Color ColRowBtnH()  { return Color(255, 85, 85, 85); }
static Color ColInput()    { return Color(255, 68, 68, 68); }
static Color ColWhite()    { return Color(255, 255, 255, 255); }
static Color ColRed()      { return Color(255, 235, 105, 105); }   // 柔和红(原纯红太醒目)
static Color ColGreen()    { return Color(255, 105, 195, 105); }   // 柔和绿(原纯绿)
static Color ColGray()     { return Color(255, 150, 150, 150); }

// ---------------------------------------------------------------------------
// 全局
// ---------------------------------------------------------------------------
static HINSTANCE   g_hInst = NULL;
static ULONG_PTR   g_gdipToken = 0;
static HINTERNET   g_hSession = NULL;
static float       g_scale = 1.0f;          // dpi/96
static FontFamily* g_famUI = NULL;          // Microsoft YaHei
static FontFamily* g_famMono = NULL;        // Consolas
static Font*       g_fontUI = NULL;         // 10pt
static Font*       g_fontBtn = NULL;        // 9pt
static Font*       g_fontMono = NULL;       // Consolas 10pt bold (已弃用, 保留)
static Font*       g_fontCompact = NULL;    // 雅黑 9pt 常规 (精简模式低调字体)

static inline int S(int v) { return (int)(v * g_scale + 0.5f); }

// 调试日志: 仅当环境变量 DSCV_DEBUG=1 时启用 (生产默认关闭)
static void LogA(const char* fmt, ...) {
    static int enabled = -1;
    if (enabled == -1) { wchar_t ebuf[8] = {0}; enabled = (GetEnvironmentVariableW(L"DSCV_DEBUG", ebuf, 8) > 0) ? 1 : 0; }
    if (!enabled) return;
    FILE* f = _wfopen(L"dscv_log.txt", L"a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// 前置声明
// ---------------------------------------------------------------------------
struct LineInput;
static void FillRoundRect(Graphics& g, const Color& c, const Rect& rc, int radius);
static void DrawLabel(Graphics& g, const std::wstring& s, const Rect& rc,
                      const Font* font, const Color& c,
                      StringAlignment hAlign = StringAlignmentNear,
                      StringAlignment vAlign = StringAlignmentCenter);
static void DrawButton2(Graphics& g, const Rect& rc, const std::wstring& text, bool hover);
static void TrackMouseEventLeave(HWND hwnd);
static void InputCaretChanged(HWND hwnd, LineInput& in);
static void Render();
static void AddByCode(const std::wstring& code, double cost, bool silent);
static void SaveConfig();

// ---------------------------------------------------------------------------
// 字符串/编码工具
// ---------------------------------------------------------------------------
static std::wstring GbkToW(const char* s, int len) {
    std::wstring out;
    if (!s || len <= 0) return out;
    int n = MultiByteToWideChar(936, 0, s, len, NULL, 0);
    out.resize(n);
    MultiByteToWideChar(936, 0, s, len, &out[0], n);
    return out;
}
static std::wstring Utf8ToW(const std::string& s) {
    std::wstring out;
    if (s.empty()) return out;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    out.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}
static std::string WToUtf8(const std::wstring& w) {
    std::string out;
    if (w.empty()) return out;
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    out.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &out[0], n, NULL, NULL);
    return out;
}
static std::string UrlEncodeUtf8(const std::wstring& w) {
    std::string u8 = WToUtf8(w);
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : u8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
static std::wstring Fmt2(double v) {
    wchar_t buf[64] = {0};
    _snwprintf(buf, 63, L"%.2f", v);
    return buf;
}
static std::wstring Fmt3(double v) {
    wchar_t buf[64] = {0};
    _snwprintf(buf, 63, L"%.3f", v);
    std::wstring s = buf;
    size_t p = s.find(L'.');
    if (p != std::wstring::npos) {
        size_t last = s.size() - 1;
        while (last > p && s[last] == L'0') last--;
        if (s[last] == L'.') last--;
        s.resize(last + 1);
    }
    return s;
}
// "(+1.23%)" — 正数带 +，负数自带 -，0 无符号 (与 Qt 版一致)
static std::wstring FmtRate(double rate) {
    wchar_t buf[64] = {0};
    _snwprintf(buf, 63, L"(%s%.2f%%)", rate > 0 ? L"+" : L"", rate);
    return buf;
}
static Color RateColor(double rate) {
    if (rate > 0) return ColRed();
    if (rate < 0) return ColGreen();
    return ColWhite();
}
static bool HasCJK(const std::wstring& w) {
    for (wchar_t c : w)
        if (c >= 0x4E00 && c <= 0x9FFF) return true;
    return false;
}

// ---------------------------------------------------------------------------
// 行情数据结构
// ---------------------------------------------------------------------------
struct Quote { std::wstring name; double curr = 0; double prev = 0; };

struct StockItem {
    std::wstring code;      // sh600111
    std::wstring display;   // 600111
    std::wstring name;
    double curr = 0, prev = 0;
    double cost = 0;        // 0 = 未设置
    bool hasData = false;   // 已收到过行情
};

struct IndexData {
    std::wstring name = L"上证指数";
    std::wstring value = L"加载中...";
    double rate = 0;
};

struct FetchResult {
    bool ok = false;
    std::map<std::wstring, Quote> quotes;
    IndexData idx;
};

// 解析 qt.gtimg.cn 返回文本(已解码为宽字符)
// 行格式: v_sh600111="1~北方稀土~600111~40.62~40.91~..."; 以 ';' 分隔
static void ParseQuotes(const std::wstring& body, FetchResult& out) {
    out.ok = true;
    out.idx.value = L"--";   // 接口成功但无指数行时，与 Qt 默认一致
    size_t pos = 0;
    while (pos < body.size()) {
        size_t semi = body.find(L';', pos);
        if (semi == std::wstring::npos) semi = body.size();
        std::wstring line = body.substr(pos, semi - pos);
        pos = semi + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t v = line.find(L"v_");
        if (v == std::wstring::npos) continue;
        size_t eq = line.find(L'=', v);
        if (eq == std::wstring::npos) continue;
        std::wstring code = line.substr(v + 2, eq - v - 2);
        size_t q1 = line.find(L'"', eq);
        size_t q2 = line.find(L'"', q1 + 1);
        if (q1 == std::wstring::npos || q2 == std::wstring::npos || q2 <= q1 + 1) continue;
        std::wstring payload = line.substr(q1 + 1, q2 - q1 - 1);

        std::vector<std::wstring> f;
        size_t p = 0;
        while (true) {
            size_t t = payload.find(L'~', p);
            if (t == std::wstring::npos) { f.push_back(payload.substr(p)); break; }
            f.push_back(payload.substr(p, t - p));
            p = t + 1;
        }
        if (f.size() < 5) continue;
        Quote q;
        q.name = f[1];
        q.curr = wcstod(f[3].c_str(), NULL);
        q.prev = wcstod(f[4].c_str(), NULL);
        if (code == L"sh000001") {
            double rate = q.prev > 0 ? (q.curr - q.prev) / q.prev * 100.0 : 0.0;
            out.idx.name = q.name.empty() ? L"上证指数" : q.name;
            out.idx.value = Fmt2(q.curr) + L" " + FmtRate(rate);
            out.idx.rate = rate;
        } else if (!code.empty()) {
            out.quotes[code] = q;
        }
    }
}

// 新浪 suggest 返回文本中提取 [s][hz]xxxxxx 代码
static std::wstring ExtractSuggestCode(const std::wstring& body) {
    auto isDigit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
    auto isHz = [](wchar_t c) { return c == L'h' || c == L'z'; };
    if (body.size() < 8) return L"";
    for (size_t i = 0; i + 7 < body.size(); i++) {
        if (body[i] == L's' && isHz(body[i + 1]) &&
            isDigit(body[i + 2]) && isDigit(body[i + 3]) && isDigit(body[i + 4]) &&
            isDigit(body[i + 5]) && isDigit(body[i + 6]) && isDigit(body[i + 7])) {
            return body.substr(i, 8);
        }
    }
    return L"";
}

// ---------------------------------------------------------------------------
// 配置 (与 Qt 版格式一致: [{"code": "600111", "cost": null|12.5}])
// ---------------------------------------------------------------------------
struct ConfigEntry { std::wstring code6; bool hasCost = false; double cost = 0; };

static std::wstring GetConfigPath() {
    PWSTR appdata = NULL;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &appdata))) {
        dir = std::wstring(appdata) + L"\\PeekStock";
        CoTaskMemFree(appdata);
    } else {
        wchar_t tmp[MAX_PATH] = {0};
        GetTempPathW(MAX_PATH, tmp);
        dir = std::wstring(tmp) + L"PeekStock";
    }
    CreateDirectoryW(dir.c_str(), NULL);
    return dir + L"\\conf.json";
}

static std::vector<ConfigEntry> ParseConfig(const std::wstring& text) {
    std::vector<ConfigEntry> out;
    size_t p = 0;
    while (true) {
        size_t cb = text.find(L"\"code\"", p);
        if (cb == std::wstring::npos) break;
        size_t colon = text.find(L':', cb);
        if (colon == std::wstring::npos) break;
        size_t q1 = text.find(L'"', colon + 1);
        if (q1 == std::wstring::npos) break;
        size_t q2 = text.find(L'"', q1 + 1);
        if (q2 == std::wstring::npos) break;
        ConfigEntry e;
        e.code6 = text.substr(q1 + 1, q2 - q1 - 1);
        size_t cost = text.find(L"\"cost\"", q2);
        size_t end = text.find(L'}', q2);
        if (cost != std::wstring::npos && end != std::wstring::npos && cost < end) {
            size_t c2 = text.find(L':', cost);
            size_t num = c2 + 1;
            while (num < end && text[num] == L' ') num++;
            if (num < end && text[num] != L'n') {   // 不是 null
                wchar_t* endp = NULL;
                e.cost = wcstod(text.c_str() + num, &endp);
                e.hasCost = (endp && endp > text.c_str() + num);
            }
        }
        if (!e.code6.empty()) out.push_back(e);
        p = q2 + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// WinHTTP
// ---------------------------------------------------------------------------
static bool HttpGet(const wchar_t* host, const std::wstring& path, std::string& bytesOut) {
    bytesOut.clear();
    if (!g_hSession) return false;
    HINTERNET hConnect = WinHttpConnect(g_hSession, host, INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect) return false;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                            NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    bool ok = false;
    do {
        if (!hRequest) break;
        if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE) break;
        if (WinHttpReceiveResponse(hRequest, NULL) == FALSE) break;
        char buf[8192];
        DWORD rd = 0;
        while (WinHttpReadData(hRequest, buf, sizeof(buf), &rd) && rd > 0)
            bytesOut.append(buf, rd);
        ok = true;
    } while (false);
    if (hRequest) WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    return ok;
}

struct FetchThreadCtx {
    HWND hwnd;
    std::wstring codes;   // 逗号分隔
};
static unsigned __stdcall FetchThreadProc(void* param) {
    FetchThreadCtx* ctx = (FetchThreadCtx*)param;
    FetchResult* res = new FetchResult();
    std::string bytes;
    if (HttpGet(L"qt.gtimg.cn", L"/q=" + ctx->codes, bytes)) {
        ParseQuotes(GbkToW(bytes.data(), (int)bytes.size()), *res);
    }
    PostMessage(ctx->hwnd, WM_APP + 1, 0, (LPARAM)res);
    delete ctx;
    _endthreadex(0);
    return 0;
}

struct NameThreadCtx {
    HWND hwnd;
    std::wstring name;
};
static unsigned __stdcall NameThreadProc(void* param) {
    NameThreadCtx* ctx = (NameThreadCtx*)param;
    std::string bytes;
    std::wstring* code = new std::wstring();
    bool httpOk = HttpGet(L"suggest3.sinajs.cn",
                          L"/suggest/type=11&key=" + Utf8ToW(UrlEncodeUtf8(ctx->name)), bytes);
    if (httpOk) *code = ExtractSuggestCode(GbkToW(bytes.data(), (int)bytes.size()));
    LogA("namequery ok=%d bytes=%d code=%ls", (int)httpOk, (int)bytes.size(),
         code->empty() ? L"(none)" : code->c_str());
    PostMessage(ctx->hwnd, WM_APP + 2, 0, (LPARAM)code);
    delete ctx;
    _endthreadex(0);
    return 0;
}

// ---------------------------------------------------------------------------
// 单行输入框 (自绘，支持 IME/粘贴)
// ---------------------------------------------------------------------------
struct LineInput {
    Rect rc;                 // 设备像素
    std::wstring text;
    int caret = 0;
    bool focused = false;
    bool enabled = true;
    bool caretOn = true;
};

static void InputDraw(Graphics& g, LineInput& in, const Font* font) {
    FillRoundRect(g, ColInput(), in.rc, S(2));
    g.SetClip(RectF((REAL)in.rc.X, (REAL)in.rc.Y, (REAL)in.rc.Width, (REAL)in.rc.Height));
    int pad = S(5);
    if (in.text.empty() && !in.focused) {
        DrawLabel(g, kPlaceholder, Rect(in.rc.X + pad, in.rc.Y, in.rc.Width - pad * 2, in.rc.Height),
                  font, ColGray());
    } else if (!in.text.empty()) {
        DrawLabel(g, in.text, Rect(in.rc.X + pad, in.rc.Y, in.rc.Width - pad * 2, in.rc.Height),
                  font, ColWhite());
    }
    if (in.focused && in.caretOn) {
        REAL w = 0;
        if (in.caret > 0) {
            RectF m;
            g.MeasureString(in.text.c_str(), in.caret, font, PointF(0, 0), &m);
            w = m.Width;
        }
        Pen pen(Color(255, 220, 220, 220), 1.0f);
        int cx = in.rc.X + pad + (int)w;
        g.DrawLine(&pen, cx, in.rc.Y + S(6), cx, in.rc.Y + in.rc.Height - S(6));
    }
    g.ResetClip();
}

static void InputCaretChanged(HWND hwnd, LineInput& in) {
    in.caretOn = true;
    // 让系统输入法组合窗口跟随光标
    HIMC himc = ImmGetContext(hwnd);
    if (himc) {
        COMPOSITIONFORM cf;
        memset(&cf, 0, sizeof(cf));
        cf.dwStyle = CFS_POINT;
        int pad = S(5);
        cf.ptCurrentPos.x = in.rc.X + pad;
        cf.ptCurrentPos.y = in.rc.Y + (in.rc.Height - S(16)) / 2;
        if (in.caret > 0) {
            HDC hdc = GetDC(hwnd);
            Graphics g(hdc);
            RectF m;
            g.MeasureString(in.text.c_str(), in.caret, g_fontUI, PointF(0, 0), &m);
            cf.ptCurrentPos.x += (int)m.Width;
            ReleaseDC(hwnd, hdc);
        }
        ImmSetCompositionWindow(himc, &cf);
        ImmReleaseContext(hwnd, himc);
    }
}

static void InputInsert(HWND hwnd, LineInput& in, const std::wstring& s) {
    in.text.insert(in.caret, s);
    in.caret += (int)s.size();
    InputCaretChanged(hwnd, in);
}

// 返回 true 表示按键已消费
static bool InputKeyDown(HWND hwnd, LineInput& in, WPARAM vk) {
    switch (vk) {
    case VK_BACK:
        if (in.caret > 0) { in.text.erase(in.caret - 1, 1); in.caret--; InputCaretChanged(hwnd, in); }
        return true;
    case VK_DELETE:
        if (in.caret < (int)in.text.size()) { in.text.erase(in.caret, 1); InputCaretChanged(hwnd, in); }
        return true;
    case VK_LEFT:
        if (in.caret > 0) { in.caret--; InputCaretChanged(hwnd, in); }
        return true;
    case VK_RIGHT:
        if (in.caret < (int)in.text.size()) { in.caret++; InputCaretChanged(hwnd, in); }
        return true;
    case VK_HOME: in.caret = 0; InputCaretChanged(hwnd, in); return true;
    case VK_END:  in.caret = (int)in.text.size(); InputCaretChanged(hwnd, in); return true;
    case 'V': {   // Ctrl+V 粘贴
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            if (OpenClipboard(hwnd)) {
                HANDLE h = GetClipboardData(CF_UNICODETEXT);
                if (h) {
                    wchar_t* p = (wchar_t*)GlobalLock(h);
                    if (p) {
                        std::wstring t = p;
                        GlobalUnlock(h);
                        while (!t.empty() && (t.back() == L'\r' || t.back() == L'\n' ||
                                              t.back() == L' ' || t.back() == L'\t')) t.pop_back();
                        InputInsert(hwnd, in, t);
                    }
                }
                CloseClipboard();
            }
            return true;
        }
        return false;
    }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 应用状态
// ---------------------------------------------------------------------------
enum HoverTarget {
    H_NONE = 0, H_INPUT, H_ADD, H_SORT, H_FIX, H_CLOSE, H_COST, H_DEL
};

struct CompactLine {
    std::wstring main;      // 代码 名称 现价 (灰色)
    std::wstring rate;      // (+x.xx%) (低饱和色)
    double rateVal = 0;
    bool hasRate = false;
};

struct AppState {
    HWND hwnd = NULL;
    std::vector<StockItem> items;
    IndexData idx;
    bool compact = false;
    bool fixedWin = false;
    bool sortAsc = false;
    LineInput input;
    HoverTarget hover = H_NONE;
    int hoverRow = -1;
    bool dragging = false;
    POINT dragOff = {0, 0};
    bool fetchRunning = false;
    bool nameRunning = false;
    ULONGLONG lastFetchTick = 0;
    HANDLE fetchThread = NULL;
    HANDLE nameThread = NULL;
    // 精简模式内容缓存
    std::vector<CompactLine> compactLines;

    // 内存渲染缓冲
    HDC memDC = NULL;
    HBITMAP dib = NULL;
    int dibW = 0, dibH = 0;
};
static AppState g_st;

// ---------------------------------------------------------------------------
// 配置读写
// ---------------------------------------------------------------------------
static std::wstring ConfigSerialize(const std::vector<StockItem>& items) {
    std::wstring s = L"[";
    for (size_t i = 0; i < items.size(); i++) {
        if (i) s += L", ";
        s += L"{\"code\": \"" + items[i].code.substr(2) + L"\", \"cost\": ";
        if (items[i].cost > 0) s += Fmt3(items[i].cost);
        else s += L"null";
        s += L"}";
    }
    s += L"]";
    return s;
}

static void SaveConfig() {
    std::wstring path = GetConfigPath();
    std::wstring tmp = path + L".tmp";
    std::string u8 = WToUtf8(ConfigSerialize(g_st.items));
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(f, u8.data(), (DWORD)u8.size(), &wr, NULL);
    CloseHandle(f);
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

static void LoadConfig() {
    std::wstring path = GetConfigPath();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string bytes;
    char buf[4096]; DWORD rd = 0;
    while (ReadFile(f, buf, sizeof(buf), &rd, NULL) && rd > 0) bytes.append(buf, rd);
    CloseHandle(f);
    std::vector<ConfigEntry> list = ParseConfig(Utf8ToW(bytes));
    for (const ConfigEntry& e : list) {
        const std::wstring& full = e.code6;
        if (full.size() != 6) continue;
        bool allDigit = true;
        for (wchar_t c : full) if (c < L'0' || c > L'9') allDigit = false;
        if (!allDigit) continue;
        std::wstring code = (full[0] == L'6' ? L"sh" : L"sz") + full;
        AddByCode(code, e.hasCost ? e.cost : 0.0, true);
    }
    // 与 Qt 版一致: 批量加载后统一保存并刷新一次
    SaveConfig();
}

// ---------------------------------------------------------------------------
// GDI+ 绘制辅助
// ---------------------------------------------------------------------------
static void FillRoundRect(Graphics& g, const Color& c, const Rect& rc, int radius) {
    GraphicsPath p;
    REAL r = (REAL)radius;
    REAL x = (REAL)rc.X, y = (REAL)rc.Y, w = (REAL)rc.Width, h = (REAL)rc.Height;
    p.AddArc(x, y, r, r, 180, 90);
    p.AddArc(x + w - r, y, r, r, 270, 90);
    p.AddArc(x + w - r, y + h - r, r, r, 0, 90);
    p.AddArc(x, y + h - r, r, r, 90, 90);
    p.CloseFigure();
    SolidBrush br(c);
    g.FillPath(&br, &p);
}

static void DrawLabel(Graphics& g, const std::wstring& s, const Rect& rc,
                      const Font* font, const Color& c,
                      StringAlignment hAlign, StringAlignment vAlign) {
    if (s.empty()) return;
    SolidBrush br(c);
    StringFormat sf;
    sf.SetAlignment(hAlign);
    sf.SetLineAlignment(vAlign);
    sf.SetTrimming(StringTrimmingNone);
    sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces |
                      StringFormatFlagsNoFitBlackBox);
    g.DrawString(s.c_str(), (INT)s.size(), font,
                 RectF((REAL)rc.X, (REAL)rc.Y, (REAL)rc.Width, (REAL)rc.Height), &sf, &br);
}

static REAL TextWidth(Graphics& g, const std::wstring& s, const Font* font) {
    RectF m;
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoFitBlackBox);
    g.MeasureString(s.c_str(), (INT)s.size(), font, PointF(0, 0), &sf, &m);
    return m.Width;
}

// 行内小按钮 (成本/删除)
static void DrawButton2(Graphics& g, const Rect& rc, const std::wstring& text, bool hover) {
    FillRoundRect(g, hover ? ColRowBtnH() : ColRowBtn(), rc, S(2));
    DrawLabel(g, text, rc, g_fontBtn, ColWhite(), StringAlignmentCenter);
}

// ---------------------------------------------------------------------------
// 布局 (展开模式)
// ---------------------------------------------------------------------------
struct RowLayout { Rect cost, del; };
struct Layout {
    Rect input, btnAdd, btnSort, btnFix, btnClose;
    std::vector<RowLayout> rows;
    int idxNameX = 0, idxValueX = 0;
    int idxY = 0;
};

static Layout GetLayout(int W) {
    Layout L;
    int x = S(8);
    L.input = Rect(x, S(kToolbarY),
                   W - x - S(8) - S(kBtnAddW) - S(kBtnSortW) - S(kBtnFixW) - S(kBtnCloseW) - S(8) * 4,
                   S(kCtrlH));
    x += L.input.Width + S(8);
    L.btnAdd = Rect(x, S(kToolbarY), S(kBtnAddW), S(kCtrlH));   x += S(kBtnAddW) + S(8);
    L.btnSort = Rect(x, S(kToolbarY), S(kBtnSortW), S(kCtrlH)); x += S(kBtnSortW) + S(8);
    L.btnFix = Rect(x, S(kToolbarY), S(kBtnFixW), S(kCtrlH));   x += S(kBtnFixW) + S(8);
    L.btnClose = Rect(x, S(kToolbarY), S(kBtnCloseW), S(kCtrlH));

    int n = (int)g_st.items.size();
    L.rows.resize(n);
    for (int i = 0; i < n; i++) {
        int rowY = S(kRowY0) + i * S(kRowStep);
        L.rows[i].del = Rect(W - S(12) - S(kRowBtnW), rowY + S(3), S(kRowBtnW), S(kRowBtnH));
        L.rows[i].cost = Rect(L.rows[i].del.X - S(5) - S(kRowBtnW), rowY + S(3), S(kRowBtnW), S(kRowBtnH));
    }
    L.idxY = S(kRowY0) + n * S(kRowStep) - S(3) + S(5);
    // GDI+ 渲染下数字与汉字墨迹边距相同, 指数列与个股列同起点即可精确对齐
    L.idxNameX = S(12);
    L.idxValueX = S(76);
    return L;
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
static void DrawButton(Graphics& g, const Rect& rc, const std::wstring& text,
                       bool hover, bool enabled) {
    Color bg = hover ? ColToolbarH() : ColToolbar();
    if (!enabled) bg = Color(255, 40, 40, 40);
    FillRoundRect(g, bg, rc, S(2));
    Color fg = enabled ? ColWhite() : Color(255, 120, 120, 120);
    DrawLabel(g, text, rc, g_fontBtn, fg, StringAlignmentCenter);
}

// 展开模式整幅绘制
static void PaintFull(Graphics& g, int W, int H) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    FillRoundRect(g, ColPanel(), Rect(0, 0, W, H), S(6));
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);

    Layout L = GetLayout(W);
    g_st.input.rc = L.input;   // 供命中测试使用

    // 顶栏
    InputDraw(g, g_st.input, g_fontUI);
    DrawButton(g, L.btnAdd, g_st.nameRunning ? L"查询中" : L"添加",
               g_st.hover == H_ADD, !g_st.nameRunning);
    DrawButton(g, L.btnSort, g_st.sortAsc ? L"排序↑" : L"排序↓", g_st.hover == H_SORT, true);
    DrawButton(g, L.btnFix, g_st.fixedWin ? L"固定✓" : L"固定", g_st.hover == H_FIX, true);
    DrawButton(g, L.btnClose, L"×", g_st.hover == H_CLOSE, true);

    // 个股行
    for (size_t i = 0; i < g_st.items.size(); i++) {
        const StockItem& it = g_st.items[i];
        int rowY = S(kRowY0) + (int)i * S(kRowStep);

        DrawLabel(g, it.display, Rect(S(12), rowY, S(58), S(kRowH)), g_fontUI, ColWhite());

        // 价格区: [名称 现价 (涨跌)] 同色 + [ (持x%) ] 独立颜色
        int priceX = S(76);
        int clipW = L.rows[i].cost.X - S(6) - priceX;   // 到成本按钮左侧为止, 避免持仓文字被裁
        g.SetClip(RectF((REAL)priceX, (REAL)rowY, REAL(clipW), REAL(S(kRowH))));
        double rate = it.prev > 0 ? (it.curr - it.prev) / it.prev * 100.0 : 0.0;
        Color mainCol = it.hasData ? RateColor(rate) : ColWhite();
        std::wstring main = it.hasData ? (it.name + L" " + Fmt2(it.curr) + L" " + FmtRate(rate))
                                       : L"加载中...";
        {
            SolidBrush br(mainCol);
            StringFormat sf;
            sf.SetLineAlignment(StringAlignmentCenter);
            sf.SetTrimming(StringTrimmingNone);
            sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
            g.DrawString(main.c_str(), (INT)main.size(), g_fontUI,
                         RectF((REAL)priceX, (REAL)rowY, REAL(S(240)), REAL(S(kRowH))), &sf, &br);
        }
        if (it.cost > 0 && it.curr > 0) {
            double cr = (it.curr - it.cost) / it.cost * 100.0;
            std::wstring cs = L" (持" + std::wstring(cr > 0 ? L"+" : L"") + Fmt2(cr) + L"%)";
            SolidBrush br(RateColor(cr));
            StringFormat sf;
            sf.SetLineAlignment(StringAlignmentCenter);
            sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
            RectF m;
            g.MeasureString(main.c_str(), (INT)main.size(), g_fontUI, PointF(0, 0), &sf, &m);
            g.DrawString(cs.c_str(), (INT)cs.size(), g_fontUI,
                         RectF((REAL)priceX + m.Width, (REAL)rowY,
                               REAL(clipW - (int)m.Width), REAL(S(kRowH))), &sf, &br);
        }
        g.ResetClip();

        DrawButton2(g, L.rows[i].cost, L"成本", g_st.hover == H_COST && g_st.hoverRow == (int)i);
        DrawButton2(g, L.rows[i].del, L"删除", g_st.hover == H_DEL && g_st.hoverRow == (int)i);
    }

    // 上证指数 (最底部)
    DrawLabel(g, g_st.idx.name, Rect(L.idxNameX, L.idxY, S(70), S(kRowH)), g_fontUI, ColWhite());
    DrawLabel(g, g_st.idx.value, Rect(L.idxValueX, L.idxY, W - L.idxValueX - S(12), S(kRowH)),
              g_fontUI, RateColor(g_st.idx.rate));
}

// 精简模式: 低调水印风 — 雅黑9pt 灰调正文 + 低饱和涨跌色 + 柔和阴影
static void PaintCompact(Graphics& g, int W, int H) {
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentNear);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);

    REAL lineH = g_fontCompact->GetHeight(&g);
    int x = S(12), y0 = S(8);

    // shadowPass=true 时全部画黑(作阴影源); false 时画灰调正文+低饱和涨跌色
    auto drawSegments = [&](Graphics& gg, bool shadowPass) {
        SolidBrush blk(Color(190, 0, 0, 0));
        SolidBrush grayBr(Color(215, 184, 184, 184));
        for (size_t i = 0; i < g_st.compactLines.size(); i++) {
            const CompactLine& cl = g_st.compactLines[i];
            if (cl.main.empty() && cl.rate.empty()) continue;
            REAL ry = (REAL)(y0 + (int)(i * lineH));
            REAL cx = (REAL)x;
            SolidBrush* mb = shadowPass ? (SolidBrush*)&blk : &grayBr;
            SolidBrush* rb = shadowPass ? (SolidBrush*)&blk : &grayBr;   // 极简模式无颜色, 仅用±号区分
            if (!cl.main.empty()) {
                gg.DrawString(cl.main.c_str(), (INT)cl.main.size(), g_fontCompact,
                              RectF(cx, ry, REAL(W), lineH), &sf, mb);
                cx += TextWidth(gg, cl.main, g_fontCompact);
            }
            if (!cl.rate.empty()) {
                gg.DrawString(cl.rate.c_str(), (INT)cl.rate.size(), g_fontCompact,
                              RectF(cx, ry, REAL(W), lineH), &sf, rb);
            }
        }
    };

    // 1) 阴影源
    Bitmap srcBmp(W, H, PixelFormat32bppARGB);
    {
        Graphics tg(&srcBmp);
        tg.SetTextRenderingHint(TextRenderingHintAntiAlias);
        drawSegments(tg, true);
    }
    // 2) 两趟盒模糊近似高斯
    const int R = 6;
    {
        BitmapData bd;
        Rect r(0, 0, W, H);
        srcBmp.LockBits(&r, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &bd);
        BYTE* base = (BYTE*)bd.Scan0;
        int stride = bd.Stride;
        std::vector<BYTE> tmpA(std::max(W, H));
        for (int yy = 0; yy < H; yy++) {
            BYTE* row = base + (size_t)yy * stride + 3;
            for (int xx = 0; xx < W; xx++) {
                int sum = 0, cnt = 0;
                for (int d = -R; d <= R; d++) {
                    int x2 = xx + d;
                    if (x2 >= 0 && x2 < W) { sum += row[x2 * 4]; cnt++; }
                }
                tmpA[xx] = (BYTE)(sum / cnt);
            }
            for (int xx = 0; xx < W; xx++) row[xx * 4] = tmpA[xx];
        }
        for (int xx = 0; xx < W; xx++) {
            for (int yy = 0; yy < H; yy++) {
                int sum = 0, cnt = 0;
                for (int d = -R; d <= R; d++) {
                    int y2 = yy + d;
                    if (y2 >= 0 && y2 < H) { sum += base[(size_t)y2 * stride + xx * 4 + 3]; cnt++; }
                }
                tmpA[yy] = (BYTE)(sum / cnt);
            }
            for (int yy = 0; yy < H; yy++) base[(size_t)yy * stride + xx * 4 + 3] = tmpA[yy];
        }
        srcBmp.UnlockBits(&bd);
    }
    // 3) 阴影(1,1) + 灰调正文
    g.DrawImage(&srcBmp, S(1), S(1));
    drawSegments(g, false);
}

// ---------------------------------------------------------------------------
// 业务操作
// ---------------------------------------------------------------------------
// 展开模式窗口高度
static int FullHeight() {
    int h = S(kFullH) + (int)g_st.items.size() * S(kRowStep);
    return std::max(S(kMinH), std::min(h, S(kMaxH)));
}

// 发起一次行情刷新(含全部限频/防重入/隐藏跳过逻辑)
static void TryFetch() {
    if (!IsWindowVisible(g_st.hwnd)) return;          // 隐藏期间不发请求
    if (g_st.fetchRunning) return;                     // 上一轮未结束
    ULONGLONG now = GetTickCount64();
    if (g_st.lastFetchTick != 0 && now - g_st.lastFetchTick < kFetchIntervalMs)
        return;                                        // 限频: 最少 5 秒一次
    g_st.lastFetchTick = now;
    g_st.fetchRunning = true;

    std::wstring codes;
    for (size_t i = 0; i < g_st.items.size(); i++) {
        if (i) codes += L",";
        codes += g_st.items[i].code;
    }
    if (!codes.empty()) codes += L",";
    codes += L"sh000001";

    FetchThreadCtx* ctx = new FetchThreadCtx();
    ctx->hwnd = g_st.hwnd;
    ctx->codes = codes;
    unsigned tid = 0;
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, FetchThreadProc, ctx, 0, &tid);
    if (g_st.fetchThread) CloseHandle(g_st.fetchThread);   // 已结束的旧线程句柄
    g_st.fetchThread = h;
}

static void ApplyFetchResult(FetchResult* res) {
    g_st.fetchRunning = false;
    if (g_st.fetchThread) { CloseHandle(g_st.fetchThread); g_st.fetchThread = NULL; }
    if (res->ok) {
        for (StockItem& it : g_st.items) {
            auto itr = res->quotes.find(it.code);
            if (itr != res->quotes.end()) {
                it.name = itr->second.name;
                it.curr = itr->second.curr;
                it.prev = itr->second.prev;
                it.hasData = true;
            }
        }
        g_st.idx = res->idx;
    } else {
        // 请求失败: 指数显示 --，个股保持旧值 (与 Qt 版一致)
        g_st.idx.name = L"上证指数";
        g_st.idx.value = L"--";
        g_st.idx.rate = 0;
    }
    delete res;
    Render();
}

static void ApplyNameResult(std::wstring* code) {
    g_st.nameRunning = false;
    if (g_st.nameThread) { CloseHandle(g_st.nameThread); g_st.nameThread = NULL; }
    g_st.input.enabled = true;
    if (code && !code->empty()) {
        AddByCode(*code, 0.0, false);
    } else {
        MessageBoxW(g_st.hwnd, L"未找到该股票，请检查名称或代码！", L"提示", MB_OK | MB_ICONWARNING);
    }
    delete code;
    Render();
}

static void AddByCode(const std::wstring& code, double cost, bool silent) {
    for (const StockItem& it : g_st.items)
        if (it.code == code) return;   // 重复添加静默忽略
    StockItem it;
    it.code = code;
    it.display = code.substr(2);
    it.cost = cost > 0 ? cost : 0;
    g_st.items.push_back(it);
    if (!silent) {
        SaveConfig();
        TryFetch();
    }
    Render();
}

static void RemoveItem(int idx) {
    if (idx < 0 || idx >= (int)g_st.items.size()) return;
    g_st.items.erase(g_st.items.begin() + idx);
    SaveConfig();
    Render();
}

static void ToggleSort() {
    g_st.sortAsc = !g_st.sortAsc;
    std::vector<StockItem> valid, invalid;
    for (const StockItem& it : g_st.items) {
        if (it.prev > 0 && it.curr > 0) valid.push_back(it);
        else invalid.push_back(it);
    }
    std::stable_sort(valid.begin(), valid.end(), [asc = g_st.sortAsc](const StockItem& a, const StockItem& b) {
        double ra = (a.curr - a.prev) / a.prev;
        double rb = (b.curr - b.prev) / b.prev;
        return asc ? (ra < rb) : (ra > rb);
    });
    g_st.items = valid;
    for (const StockItem& it : invalid) g_st.items.push_back(it);
    Render();
}

// ---------------------------------------------------------------------------
// 精简模式内容
// ---------------------------------------------------------------------------
static void RebuildCompact() {
    g_st.compactLines.clear();
    for (const StockItem& it : g_st.items) {
        CompactLine cl;
        if (it.hasData) {
            cl.main = it.name.empty() ? L"" : it.name + L" ";
            cl.main += Fmt2(it.curr) + L" ";
            double rate = it.prev > 0 ? (it.curr - it.prev) / it.prev * 100.0 : 0.0;
            cl.rate = FmtRate(rate);
            cl.rateVal = rate;
            cl.hasRate = true;
            if (it.cost > 0 && it.curr > 0) {
                double cr = (it.curr - it.cost) / it.cost * 100.0;
                cl.rate += L" (持" + std::wstring(cr > 0 ? L"+" : L"") + Fmt2(cr) + L"%)";
            }
        } else {
            cl.main = L"加载中...";
        }
        g_st.compactLines.push_back(cl);
    }
    if (!g_st.compactLines.empty())
        g_st.compactLines.push_back(CompactLine());
    CompactLine idx;
    idx.main = g_st.idx.name + L" ";
    size_t sp = g_st.idx.value.find(L' ');
    if (sp != std::wstring::npos && g_st.idx.value != L"--" &&
        g_st.idx.value.find(L"加载中") == std::wstring::npos) {
        idx.main += g_st.idx.value.substr(0, sp) + L" ";
        idx.rate = g_st.idx.value.substr(sp + 1);
        idx.rateVal = g_st.idx.rate;
        idx.hasRate = true;
    } else {
        idx.main += g_st.idx.value;
    }
    g_st.compactLines.push_back(idx);
}

// ---------------------------------------------------------------------------
// 分层窗口渲染
// ---------------------------------------------------------------------------
static void EnsureDib(int w, int h) {
    if (g_st.memDC && g_st.dibW == w && g_st.dibH == h) return;
    if (g_st.dib) { DeleteObject(g_st.dib); g_st.dib = NULL; }
    if (g_st.memDC) { DeleteDC(g_st.memDC); g_st.memDC = NULL; }
    HDC screen = GetDC(NULL);
    g_st.memDC = CreateCompatibleDC(screen);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    g_st.dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (g_st.dib) SelectObject(g_st.memDC, g_st.dib);
    g_st.dibW = w; g_st.dibH = h;
}

static void Render() {
    if (!g_st.hwnd || !g_fontUI) return;
    RECT wr;
    GetWindowRect(g_st.hwnd, &wr);
    int curX = wr.left, curY = wr.top;
    int w, h;
    if (g_st.compact) {
        RebuildCompact();
        HDC hdc = GetDC(NULL);
        {
            Graphics mg(hdc);
            REAL lineH = g_fontCompact->GetHeight(&mg);
            REAL maxW = (REAL)S(140);
            for (const CompactLine& cl : g_st.compactLines) {
                if (cl.main.empty() && cl.rate.empty()) continue;
                REAL tw = TextWidth(mg, cl.main, g_fontCompact) + TextWidth(mg, cl.rate, g_fontCompact);
                if (tw > maxW) maxW = tw;
            }
            w = (int)maxW + S(24);
            h = (int)(lineH * g_st.compactLines.size()) + S(16);
        }
        ReleaseDC(NULL, hdc);
    } else {
        w = S(kWinW);
        h = FullHeight();
    }
    EnsureDib(w, h);
    if (!g_st.memDC) return;

    {
        Graphics g(g_st.memDC);
        g.Clear(Color(1, 0, 0, 0));
        if (g_st.compact) PaintCompact(g, w, h);
        else PaintFull(g, w, h);
    }

    POINT src = {0, 0};
    POINT dst = {curX, curY};
    SIZE sz = {w, h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC screen = GetDC(NULL);
    BOOL ulwOk = UpdateLayeredWindow(g_st.hwnd, screen, &dst, &sz, g_st.memDC, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
    LogA("render compact=%d w=%d h=%d ulw=%d", (int)g_st.compact, w, h, (int)ulwOk);
}

// ---------------------------------------------------------------------------
// 命中测试
// ---------------------------------------------------------------------------
static HoverTarget HitTest(int x, int y, int* rowOut) {
    *rowOut = -1;
    if (g_st.compact) return H_NONE;
    Layout L = GetLayout(g_st.dibW);
    auto in = [x, y](const Rect& r) {
        return x >= r.X && x < r.X + r.Width && y >= r.Y && y < r.Y + r.Height;
    };
    if (g_st.input.enabled && in(g_st.input.rc)) return H_INPUT;
    if (in(L.btnAdd)) return H_ADD;
    if (in(L.btnSort)) return H_SORT;
    if (in(L.btnFix)) return H_FIX;
    if (in(L.btnClose)) return H_CLOSE;
    for (size_t i = 0; i < L.rows.size(); i++) {
        if (in(L.rows[i].cost)) { *rowOut = (int)i; return H_COST; }
        if (in(L.rows[i].del)) { *rowOut = (int)i; return H_DEL; }
    }
    return H_NONE;
}

// ---------------------------------------------------------------------------
// 成本弹框 (自绘模态)
// ---------------------------------------------------------------------------
struct CostDlgState {
    HWND dlg = NULL;
    HWND owner = NULL;
    LineInput input;
    int itemIdx = -1;
    bool ok = false;
    bool hoverOk = false, hoverCancel = false;
    HDC mdc = NULL;
    HBITMAP mdb = NULL;
    int mw = 0, mh = 0;
};
static CostDlgState g_dlg;

static void DlgRender() {
    if (!g_dlg.dlg) return;
    RECT wr;
    GetWindowRect(g_dlg.dlg, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    HDC screen = GetDC(NULL);
    if (!g_dlg.mdc || g_dlg.mw != w || g_dlg.mh != h) {
        if (g_dlg.mdb) DeleteObject(g_dlg.mdb);
        if (g_dlg.mdc) DeleteDC(g_dlg.mdc);
        g_dlg.mdc = CreateCompatibleDC(screen);
        BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        g_dlg.mdb = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, NULL, NULL, 0);
        SelectObject(g_dlg.mdc, g_dlg.mdb);
        g_dlg.mw = w; g_dlg.mh = h;
    }
    {
        Graphics g(g_dlg.mdc);
        g.Clear(Color(1, 0, 0, 0));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        FillRoundRect(g, Color(255, 30, 30, 30), Rect(0, 0, w, h), S(6));
        DrawLabel(g, L"设置成本价", Rect(S(14), S(8), w - S(28), S(22)), g_fontUI,
                  ColWhite(), StringAlignmentNear, StringAlignmentNear);
        std::wstring prompt = g_st.items[g_dlg.itemIdx].display +
                              L" 成本价（留空确定则清除）：";
        DrawLabel(g, prompt, Rect(S(14), S(34), w - S(28), S(18)), g_fontBtn,
                  Color(255, 200, 200, 200), StringAlignmentNear, StringAlignmentCenter);
        InputDraw(g, g_dlg.input, g_fontUI);
        int by = g_dlg.input.rc.Y + g_dlg.input.rc.Height + S(8);
        Rect okRect(S(14), by, S(60), S(24));
        Rect cancelRect(S(80), by, S(60), S(24));
        FillRoundRect(g, g_dlg.hoverOk ? ColRowBtnH() : ColRowBtn(), okRect, S(2));
        DrawLabel(g, L"确定", okRect, g_fontBtn, ColWhite(), StringAlignmentCenter);
        FillRoundRect(g, g_dlg.hoverCancel ? ColRowBtnH() : ColRowBtn(), cancelRect, S(2));
        DrawLabel(g, L"取消", cancelRect, g_fontBtn, ColWhite(), StringAlignmentCenter);
    }
    POINT src = {0, 0};
    SIZE sz = {w, h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_dlg.dlg, screen, NULL, &sz, g_dlg.mdc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

static void CostApply() {
    if (g_dlg.itemIdx < 0 || g_dlg.itemIdx >= (int)g_st.items.size()) return;
    StockItem& it = g_st.items[g_dlg.itemIdx];
    std::wstring text = g_dlg.input.text;
    size_t b = text.find_first_not_of(L" \t");
    size_t e = text.find_last_not_of(L" \t");
    if (b == std::wstring::npos) text.clear();
    else text = text.substr(b, e - b + 1);

    if (text.empty()) {
        // 留空确定 → 清除成本
        if (it.cost > 0) { it.cost = 0; SaveConfig(); }
    } else {
        std::wstring t = text;
        std::replace(t.begin(), t.end(), L',', L'.');
        wchar_t* endp = NULL;
        double v = wcstod(t.c_str(), &endp);
        if (endp && *endp == 0 && v > 0) {
            it.cost = v;
            SaveConfig();
        }
        // 无效输入: 静默忽略(与 Qt 版一致)
    }
}

static void DlgClose() {
    if (g_dlg.dlg) DestroyWindow(g_dlg.dlg);
}

static LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SHOWWINDOW:
        if (wp) { SetFocus(hwnd); DlgRender(); }
        return 0;
    case WM_SETFOCUS:
        g_dlg.input.focused = true;
        SetTimer(hwnd, 2, 500, NULL);
        DlgRender();
        return 0;
    case WM_KILLFOCUS:
        g_dlg.input.focused = false;
        KillTimer(hwnd, 2);
        DlgRender();
        return 0;
    case WM_TIMER:
        if (wp == 2) { g_dlg.input.caretOn = !g_dlg.input.caretOn; DlgRender(); }
        return 0;
    case WM_IME_STARTCOMPOSITION:
        InputCaretChanged(hwnd, g_dlg.input);
        break;   // 交给 DefWindowProc 继续 IME 流程
    case WM_IME_COMPOSITION:
        InputCaretChanged(hwnd, g_dlg.input);
        break;   // 必须走 DefWindowProc, 结果字符才会以 WM_CHAR 到达
    case WM_CHAR:
        if (wp >= 0x20 && wp != 0x7F) {
            InputInsert(hwnd, g_dlg.input, std::wstring(1, (wchar_t)wp));
            DlgRender();
        }
        return 0;
    case WM_KEYDOWN: {
        if (wp == VK_RETURN) {
            g_dlg.ok = true;
            CostApply();
            DlgClose();
            return 0;
        }
        if (wp == VK_ESCAPE) {
            DlgClose();
            return 0;
        }
        if (InputKeyDown(hwnd, g_dlg.input, wp)) { DlgRender(); return 0; }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int by = g_dlg.input.rc.Y + g_dlg.input.rc.Height + S(8);
        bool inBtnY = pt.y >= by && pt.y < by + S(24);
        bool hOk = inBtnY && pt.x >= S(14) && pt.x < S(14) + S(60);
        bool hC = inBtnY && pt.x >= S(80) && pt.x < S(80) + S(60);
        if (hOk != g_dlg.hoverOk || hC != g_dlg.hoverCancel) {
            g_dlg.hoverOk = hOk; g_dlg.hoverCancel = hC; DlgRender();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int by = g_dlg.input.rc.Y + g_dlg.input.rc.Height + S(8);
        bool inBtnY = pt.y >= by && pt.y < by + S(24);
        if (pt.x >= g_dlg.input.rc.X && pt.x < g_dlg.input.rc.X + g_dlg.input.rc.Width &&
            pt.y >= g_dlg.input.rc.Y && pt.y < g_dlg.input.rc.Y + g_dlg.input.rc.Height) {
            g_dlg.input.focused = true;
            g_dlg.input.caret = (int)g_dlg.input.text.size();
            SetTimer(hwnd, 2, 500, NULL);
            InputCaretChanged(hwnd, g_dlg.input);
        } else if (inBtnY && pt.x >= S(14) && pt.x < S(14) + S(60)) {
            g_dlg.ok = true;
            CostApply();
            DlgClose();
            return 0;
        } else if (inBtnY && pt.x >= S(80) && pt.x < S(80) + S(60)) {
            DlgClose();
            return 0;
        } else {
            g_dlg.input.focused = false;
            KillTimer(hwnd, 2);
        }
        DlgRender();
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 2);
        if (g_dlg.mdb) { DeleteObject(g_dlg.mdb); g_dlg.mdb = NULL; }
        if (g_dlg.mdc) { DeleteDC(g_dlg.mdc); g_dlg.mdc = NULL; }
        g_dlg.dlg = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowCostDialog(int idx) {
    if (idx < 0 || idx >= (int)g_st.items.size()) return;
    g_dlg = CostDlgState();
    g_dlg.owner = g_st.hwnd;
    g_dlg.itemIdx = idx;
    const StockItem& it = g_st.items[idx];
    if (it.cost > 0) g_dlg.input.text = Fmt3(it.cost);
    g_dlg.input.caret = (int)g_dlg.input.text.size();
    g_dlg.input.focused = true;

    int w = S(320), h = S(124);
    RECT orr;
    GetWindowRect(g_st.hwnd, &orr);
    int x = orr.left + ((orr.right - orr.left) - w) / 2;
    int y = orr.top + ((orr.bottom - orr.top) - h) / 2;

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DlgProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = APP_DLGCLS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    g_dlg.input.rc = Rect(S(14), S(56), w - S(28), S(24));

    g_dlg.dlg = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, APP_DLGCLS, L"",
                                WS_POPUP, x, y, w, h, g_st.hwnd, NULL, g_hInst, NULL);
    if (!g_dlg.dlg) return;

    EnableWindow(g_st.hwnd, FALSE);
    ShowWindow(g_dlg.dlg, SW_SHOW);
    DlgRender();

    // 模态消息循环
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
        if (!IsWindow(g_dlg.dlg)) break;
    }
    EnableWindow(g_st.hwnd, TRUE);
    Render();   // 成本可能变化, 刷新主窗
}

// ---------------------------------------------------------------------------
// 主窗口消息处理
// ---------------------------------------------------------------------------
static void ExpandIfCompact() {
    if (g_st.compact) {
        g_st.compact = false;
        LogA("expand");
        Render();
    }
}

static void CompactIfAllowed() {
    if (!g_st.fixedWin && !g_st.compact) {
        g_st.compact = true;
        LogA("compact (mouseleave)");
        Render();
    }
}

static void OnAddClicked() {
    std::wstring text = g_st.input.text;
    size_t b = text.find_first_not_of(L" \t");
    size_t e = text.find_last_not_of(L" \t");
    if (b == std::wstring::npos) return;
    text = text.substr(b, e - b + 1);

    if (HasCJK(text)) {
        // 中文名查询走后台线程 (与 Qt 版一致), 期间按钮显示"查询中"
        LogA("onadd name path: %ls", text.c_str());
        g_st.nameRunning = true;
        g_st.input.enabled = false;
        g_st.input.text.clear();
        g_st.input.caret = 0;
        Render();
        NameThreadCtx* ctx = new NameThreadCtx();
        ctx->hwnd = g_st.hwnd;
        ctx->name = text;
        unsigned tid = 0;
        HANDLE h = (HANDLE)_beginthreadex(NULL, 0, NameThreadProc, ctx, 0, &tid);
        if (g_st.nameThread) CloseHandle(g_st.nameThread);
        g_st.nameThread = h;
        return;
    }

    std::wstring digits;
    for (wchar_t c : text) if (c >= L'0' && c <= L'9') digits += c;
    if (digits.size() == 6) {
        std::wstring code = (digits[0] == L'6' ? L"sh" : L"sz") + digits;
        AddByCode(code, 0.0, false);
    } else {
        MessageBoxW(g_st.hwnd, L"未找到该股票，请检查名称或代码！", L"提示",
                    MB_OK | MB_ICONWARNING);
    }
    g_st.input.text.clear();
    g_st.input.caret = 0;
    Render();
}

static void TrackMouseEventLeave(HWND hwnd) {
    TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
    TrackMouseEvent(&tme);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            TryFetch();
        } else if (wp == 2) {
            g_st.input.caretOn = !g_st.input.caretOn;
            Render();
        }
        return 0;
    case WM_SHOWWINDOW:
        if (wp) TryFetch();   // 显示瞬间立即拉一次 (限频内则跳过)
        return 0;
    case WM_HOTKEY:
        if (wp == 1) {
            if (IsWindowVisible(hwnd)) {
                ShowWindow(hwnd, SW_HIDE);
            } else {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            }
        }
        return 0;
    case WM_APP + 1:   // 行情结果
        ApplyFetchResult((FetchResult*)lp);
        return 0;
    case WM_APP + 2:   // 名称查询结果
        ApplyNameResult((std::wstring*)lp);
        return 0;
    case WM_IME_STARTCOMPOSITION:
        InputCaretChanged(hwnd, g_st.input);
        break;   // 交给 DefWindowProc 继续 IME 流程
    case WM_IME_COMPOSITION:
        InputCaretChanged(hwnd, g_st.input);
        break;   // 必须走 DefWindowProc, 结果字符才会以 WM_CHAR 到达
    case WM_CHAR:
        if (g_st.input.focused && g_st.input.enabled && wp >= 0x20 && wp != 0x7F) {
            InputInsert(hwnd, g_st.input, std::wstring(1, (wchar_t)wp));
            Render();
        }
        return 0;
    case WM_KEYDOWN:
        if (g_st.input.focused && g_st.input.enabled) {
            if (wp == VK_RETURN) { OnAddClicked(); return 0; }
            if (InputKeyDown(hwnd, g_st.input, wp)) { Render(); return 0; }
        }
        return 0;
    case WM_MOUSEMOVE: {
        LogA("mousemove x=%d y=%d compact=%d", GET_X_LPARAM(lp), GET_Y_LPARAM(lp), (int)g_st.compact);
        TrackMouseEventLeave(hwnd);
        if (g_st.compact) ExpandIfCompact();   // 鼠标进入 → 展开(与 Qt enterEvent 一致)
        int row = -1;
        HoverTarget h = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &row);
        if (h != g_st.hover || row != g_st.hoverRow) {
            g_st.hover = h; g_st.hoverRow = row;
            if (!g_st.compact) Render();
        }
        if (g_st.dragging) {
            POINT spt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &spt);
            SetWindowPos(hwnd, NULL,
                         spt.x - g_st.dragOff.x, spt.y - g_st.dragOff.y,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        int row = -1;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        HoverTarget h = HitTest(pt.x, pt.y, &row);
        switch (h) {
        case H_INPUT:
            if (g_st.input.enabled) {
                g_st.input.focused = true;
                g_st.input.caret = (int)g_st.input.text.size();
                SetTimer(hwnd, 2, 500, NULL);
                InputCaretChanged(hwnd, g_st.input);
            }
            break;
        case H_ADD:  if (!g_st.nameRunning) OnAddClicked(); break;
        case H_SORT: ToggleSort(); break;
        case H_FIX:  g_st.fixedWin = !g_st.fixedWin; Render(); break;
        case H_CLOSE: DestroyWindow(hwnd); return 0;
        case H_COST: ShowCostDialog(row); break;
        case H_DEL:  RemoveItem(row); break;
        default:
            ExpandIfCompact();
            g_st.dragging = true;
            g_st.dragOff.x = pt.x; g_st.dragOff.y = pt.y;
            SetCapture(hwnd);
            break;
        }
        Render();
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_st.dragging) {
            g_st.dragging = false;
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        g_st.dragging = false;
        return 0;
    case WM_MOUSELEAVE:
        LogA("mouseleave msg");
        g_st.hover = H_NONE; g_st.hoverRow = -1;
        CompactIfAllowed();   // 鼠标离开 → 收缩(未固定时)
        return 0;
    case WM_KILLFOCUS:
        g_st.input.focused = false;
        KillTimer(hwnd, 2);
        Render();
        return 0;
    case WM_DESTROY: {
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        UnregisterHotKey(hwnd, 1);
        if (g_st.fetchThread) {
            WaitForSingleObject(g_st.fetchThread, 1000);
            CloseHandle(g_st.fetchThread);
        }
        if (g_st.nameThread) {
            WaitForSingleObject(g_st.nameThread, 1000);
            CloseHandle(g_st.nameThread);
        }
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// 初始化 & 入口
// ---------------------------------------------------------------------------
static void InitGraphics() {
    GdiplusStartupInput si;
    GdiplusStartup(&g_gdipToken, &si, NULL);

    UINT dpi = 96;
    HDC dc = GetDC(NULL);
    dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);

    g_scale = dpi / 96.0f;
    g_famUI = new FontFamily(L"Microsoft YaHei");
    g_famMono = new FontFamily(L"Consolas");
    REAL emUI = dpi * 10.0f / 72.0f;    // 10pt
    REAL emBtn = dpi * 9.0f / 72.0f;    // 9pt
    g_fontUI = new Font(g_famUI, emUI, FontStyleRegular, UnitPixel);
    g_fontBtn = new Font(g_famUI, emBtn, FontStyleRegular, UnitPixel);
    g_fontMono = new Font(g_famMono, emUI, FontStyleBold, UnitPixel);
    g_fontCompact = new Font(g_famUI, emBtn, FontStyleRegular, UnitPixel);   // 9pt
}

#ifndef SELFTEST
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInst = hInst;

    // Per-monitor DPI 感知
    HMODULE u32 = GetModuleHandleW(L"user32");
    typedef BOOL(WINAPI * SetDpiCtxFn)(HANDLE);
    SetDpiCtxFn setCtx = u32 ? (SetDpiCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
    if (setCtx) setCtx((HANDLE)-4);   // PER_MONITOR_AWARE_V2
    else SetProcessDPIAware();

    InitGraphics();
    g_hSession = WinHttpOpen(L"PeekStock/1.0",
                             WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (g_hSession) WinHttpSetTimeouts(g_hSession, 3000, 3000, 3000, 3000);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = APP_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);   // 不设置的话会一直显示启动转圈光标
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                APP_CLASS, L"", WS_POPUP,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(kWinW), S(100),
                                NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    g_st.hwnd = hwnd;

    LoadConfig();
    Render();
    RegisterHotKey(hwnd, 1, MOD_CONTROL, 'Q');
    SetTimer(hwnd, 1, kFetchIntervalMs, NULL);
    ShowWindow(hwnd, SW_SHOW);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_hSession) WinHttpCloseHandle(g_hSession);
    GdiplusShutdown(g_gdipToken);
    return 0;
}

#else   // SELFTEST: 离线/在线功能自测
#include <stdio.h>
static int g_fail = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("[PASS] %s\n", WToUtf8(name).c_str()); \
    else { printf("[FAIL] %s\n", WToUtf8(name).c_str()); g_fail++; } \
} while (0)

int wmain(int argc, wchar_t** argv) {
    g_hSession = WinHttpOpen(L"selftest", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    WinHttpSetTimeouts(g_hSession, 3000, 3000, 3000, 3000);

    // 1) GBK 解码 + 行情解析 (用真实抓包样例)
    FILE* f = _wfopen(L"test_data/quote_sample.txt", L"rb");
    CHECK(f != NULL, L"打开 quote_sample.txt");
    if (f) {
        std::string bytes;
        char buf[4096]; size_t rd;
        while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) bytes.append(buf, rd);
        fclose(f);
        std::wstring body = GbkToW(bytes.data(), (int)bytes.size());
        CHECK(body.find(L"北方稀土") != std::wstring::npos, L"GBK 解码(中文股票名)");

        FetchResult res;
        ParseQuotes(body, res);
        CHECK(res.ok, L"行情解析 ok 标志");
        auto it = res.quotes.find(L"sh600111");
        CHECK(it != res.quotes.end(), L"解析出 sh600111");
        if (it != res.quotes.end()) {
            CHECK(it->second.name == L"北方稀土", L"股票名称正确");
            CHECK(it->second.curr == 40.62, L"现价字段=40.62");
            CHECK(it->second.prev == 40.91, L"昨收字段=40.91");
        }
        CHECK(res.idx.value.find(L".") != std::wstring::npos &&
              res.idx.value != L"--", L"上证指数已解析");
    }

    // 2) 名称→代码
    f = _wfopen(L"test_data/suggest_sample.txt", L"rb");
    CHECK(f != NULL, L"打开 suggest_sample.txt");
    if (f) {
        std::string bytes;
        char buf[1024]; size_t rd;
        while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) bytes.append(buf, rd);
        fclose(f);
        CHECK(ExtractSuggestCode(GbkToW(bytes.data(), (int)bytes.size())) == L"sh600111",
              L"suggest 提取代码=sh600111");
    }

    // 3) 涨跌格式化 (测试值避开二进制舍入边界)
    CHECK(FmtRate(1.25) == L"(+1.25%)", L"涨幅格式 (+1.25%)");
    CHECK(FmtRate(0.0) == L"(0.00%)", L"平盘格式 (0.00%)");
    CHECK(FmtRate(-1.25) == L"(-1.25%)", L"跌幅格式 (-1.25%)");
    CHECK(Fmt3(12.5) == L"12.5", L"成本格式 12.5");
    CHECK(Fmt3(12.0) == L"12", L"成本格式 12");

    // 4) 配置序列化/解析往返
    {
        std::wstring json = L"[{\"code\": \"600111\", \"cost\": null}]";
        auto list = ParseConfig(json);
        CHECK(list.size() == 1 && list[0].code6 == L"600111" && !list[0].hasCost,
              L"配置解析 cost=null");
        std::wstring json2 = L"[{\"code\": \"000001\", \"cost\": 12.5}]";
        auto list2 = ParseConfig(json2);
        CHECK(list2.size() == 1 && list2[0].hasCost && list2[0].cost == 12.5,
              L"配置解析 cost=12.5");
    }

    // 5) 在线: 真实接口拉一次
    {
        std::string bytes;
        bool ok = HttpGet(L"qt.gtimg.cn", L"/q=sh600111,sh000001", bytes);
        CHECK(ok && !bytes.empty(), L"在线拉取 qt.gtimg.cn");
        if (ok && !bytes.empty()) {
            FetchResult res;
            ParseQuotes(GbkToW(bytes.data(), (int)bytes.size()), res);
            auto it = res.quotes.find(L"sh600111");
            CHECK(it != res.quotes.end() && it->second.curr > 0, L"在线解析现价>0");
            printf("       live: curr=%.2f prev=%.2f idx=%s\n",
                   it != res.quotes.end() ? it->second.curr : 0.0,
                   it != res.quotes.end() ? it->second.prev : 0.0,
                   WToUtf8(res.idx.value).c_str());
        }
        std::string sbytes;
        ok = HttpGet(L"suggest3.sinajs.cn", L"/suggest/type=11&key=%E5%8C%97%E6%96%B9%E7%A8%80%E5%9C%9F", sbytes);
        CHECK(ok && ExtractSuggestCode(GbkToW(sbytes.data(), (int)sbytes.size())) == L"sh600111",
              L"在线名称查询→sh600111");
    }

    printf("\nresult: %s (failed %d)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    if (g_hSession) WinHttpCloseHandle(g_hSession);
    return g_fail ? 1 : 0;
}
#endif
