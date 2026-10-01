// Channel Mixer - tray tool: per-channel volume sliders + LED meters
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#undef WINVER
#define WINVER 0x0601
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#endif

// Tiny COM smart pointer (works on MSVC and MinGW)
template <class T> struct ComPtr {
    T* p = nullptr;
    ComPtr() {}
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    void Reset() { if (p) { p->Release(); p = nullptr; } }
    T** GetAddressOf() { Reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// MinGW headers don't define the meter interface, so declare it ourselves (same vtable layout as Windows')
struct MeterInfo : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 count, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) = 0;
};

// GUIDs written out so no extra libraries are needed
static const GUID kClsidEnum   = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIidEnum     = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID kIidVol      = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};
static const GUID kIidMeter    = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};
static const GUID kIidClient   = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const PROPERTYKEY kKeyFriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

#define WM_TRAY (WM_APP + 1)
#define ID_TIMER 1
#define ID_EXIT 100
#define ID_SLIDER_BASE 1000

static const int SEGS = 24;
static const float MIN_DB = -60.f;
static const int METER_W = 14, METER_H = 200;

struct Channel {
    HWND slider = nullptr, pct = nullptr;
    RECT meter{};
    float level = 0, hold = 0, holdTimer = 0;
    int lit = -1, holdSeg = -1;
};

static HINSTANCE g_inst;
static HWND g_wnd, g_status;
static NOTIFYICONDATAW g_nid{};
static UINT g_taskbarMsg;
static std::vector<Channel> g_ch;
static ComPtr<IMMDevice> g_dev;
static ComPtr<IAudioEndpointVolume> g_vol;
static ComPtr<MeterInfo> g_meter;
static std::wstring g_devId, g_devName;
static int g_n = 0;
static DWORD g_mask = 0;
static DWORD g_last = 0;
static float g_chk = 0;
static const DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
static const DWORD kExStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;

static const wchar_t* kSpk[] = {
    L"Front L", L"Front R", L"Center", L"LFE", L"Back L", L"Back R", L"FL Ctr", L"FR Ctr", L"Back Ctr",
    L"Side L", L"Side R", L"Top Ctr", L"Top FL", L"Top FC", L"Top FR", L"Top BL", L"Top BC", L"Top BR"};

// ---------- audio ----------
static bool Acquire()
{
    g_vol.Reset(); g_meter.Reset(); g_dev.Reset();
    g_n = 0; g_mask = 0; g_devName.clear();

    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(kClsidEnum, nullptr, CLSCTX_ALL, kIidEnum, (void**)en.GetAddressOf()))) return false;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, g_dev.GetAddressOf()))) return false;

    LPWSTR id = nullptr;
    if (SUCCEEDED(g_dev->GetId(&id))) { g_devId = id; CoTaskMemFree(id); }

    ComPtr<IPropertyStore> ps;
    if (SUCCEEDED(g_dev->OpenPropertyStore(STGM_READ, ps.GetAddressOf()))) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(kKeyFriendlyName, &pv)) && pv.vt == VT_LPWSTR) g_devName = pv.pwszVal;
        PropVariantClear(&pv);
    }

    g_dev->Activate(kIidVol, CLSCTX_ALL, nullptr, (void**)g_vol.GetAddressOf());
    g_dev->Activate(kIidMeter, CLSCTX_ALL, nullptr, (void**)g_meter.GetAddressOf());

    ComPtr<IAudioClient> ac;
    if (SUCCEEDED(g_dev->Activate(kIidClient, CLSCTX_ALL, nullptr, (void**)ac.GetAddressOf()))) {
        WAVEFORMATEX* wf = nullptr;
        if (SUCCEEDED(ac->GetMixFormat(&wf)) && wf) {
            g_n = wf->nChannels;
            if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) g_mask = ((WAVEFORMATEXTENSIBLE*)wf)->dwChannelMask;
            CoTaskMemFree(wf);
        }
    }
    if (g_n <= 0 && g_meter) { UINT c = 0; g_meter->GetMeteringChannelCount(&c); g_n = (int)c; }
    return g_vol && g_meter && g_n > 0;
}

static std::wstring ChannelName(int idx)
{
    if (g_mask) {   // channels are in ascending bit order of the speaker mask
        int seen = 0;
        for (int bit = 0; bit < 18; bit++)
            if (g_mask & (1u << bit)) { if (seen == idx) return kSpk[bit]; seen++; }
    }
    wchar_t b[24]; wsprintfW(b, L"Ch %d", idx + 1); return b;
}

static std::wstring LayoutName()
{
    switch (g_n) {
    case 1: return L"Mono";
    case 2: return L"Stereo";
    case 4: return L"Quad";
    case 6: return L"5.1";
    case 8: return L"7.1";
    }
    wchar_t b[24]; wsprintfW(b, L"%d channels", g_n); return b;
}

// ---------- UI ----------
static HWND Label(const wchar_t* t, int x, int y, int w, int h, DWORD style)
{
    HWND l = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, nullptr, g_inst, nullptr);
    SendMessageW(l, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return l;
}

static void DestroyChildren()
{
    while (HWND c = GetWindow(g_wnd, GW_CHILD)) DestroyWindow(c);
    g_ch.clear();
    g_status = nullptr;
}

static void SetClientSize(int cw, int chh)
{
    RECT r = {0, 0, cw, chh};
    AdjustWindowRectEx(&r, kStyle, FALSE, kExStyle);
    SetWindowPos(g_wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void BuildUI()
{
    DestroyChildren();
    if (!Acquire()) {
        Label(L"No audio output device found.", 12, 14, 300, 20, SS_LEFT);
        SetClientSize(330, 50);
        return;
    }

    const int colW = 84;
    int cw = std::max(g_n * colW + 24, 360);
    std::wstring head = g_devName + L"  -  " + LayoutName();
    Label(head.c_str(), 12, 10, cw - 24, 20, SS_LEFT | SS_ENDELLIPSIS);

    int totalW = g_n * colW;
    int startX = (cw - totalW) / 2;

    for (int i = 0; i < g_n; i++) {
        Channel c;
        int x0 = startX + i * colW;
        std::wstring name = ChannelName(i);
        Label(name.c_str(), x0, 40, colW, 18, SS_CENTER);

        c.slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_VERT | TBS_NOTICKS,
                                   x0 + 14, 62, 30, METER_H, g_wnd, (HMENU)(INT_PTR)(ID_SLIDER_BASE + i), g_inst, nullptr);
        SendMessageW(c.slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        float v = 1.f;
        if (g_vol) g_vol->GetChannelVolumeLevelScalar((UINT)i, &v);
        int pct = (int)(v * 100.f + 0.5f);
        SendMessageW(c.slider, TBM_SETPOS, TRUE, 100 - pct);   // vertical trackbar: top = min, so invert

        c.meter = {x0 + 52, 62, x0 + 52 + METER_W, 62 + METER_H};

        wchar_t buf[16]; wsprintfW(buf, L"%d%%", pct);
        c.pct = Label(buf, x0, 268, colW, 18, SS_CENTER);
        g_ch.push_back(c);
    }
    g_status = Label(L"", 12, 294, cw - 24, 34, SS_LEFT);
    SetClientSize(cw, 336);
    InvalidateRect(g_wnd, nullptr, TRUE);
}

static COLORREF Dim(COLORREF c)
{
    auto d = [](int v) { return (v * 20 + 20 * 80) / 100; };
    return RGB(d(GetRValue(c)), d(GetGValue(c)), d(GetBValue(c)));
}

static void DrawMeter(HDC hdc, const Channel& c)
{
    HDC m = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, METER_W, METER_H);
    HGDIOBJ old = SelectObject(m, bmp);
    SelectObject(m, GetStockObject(DC_BRUSH));

    SetDCBrushColor(m, RGB(20, 20, 20));
    RECT all = {0, 0, METER_W, METER_H};
    FillRect(m, &all, (HBRUSH)GetStockObject(DC_BRUSH));

    float segH = (float)METER_H / SEGS;
    for (int i = 0; i < SEGS; i++) {
        float db = MIN_DB + (i + 1) / (float)SEGS * -MIN_DB;
        COLORREF col = db > -3 ? RGB(255, 40, 40) : db > -12 ? RGB(255, 200, 0) : RGB(40, 220, 60);
        bool on = i < c.lit || i == c.holdSeg;
        SetDCBrushColor(m, on ? col : Dim(col));
        RECT r = {1, (LONG)(METER_H - (i + 1) * segH + 1), METER_W - 1, (LONG)(METER_H - i * segH - 1)};
        FillRect(m, &r, (HBRUSH)GetStockObject(DC_BRUSH));
    }
    BitBlt(hdc, c.meter.left, c.meter.top, METER_W, METER_H, m, 0, 0, SRCCOPY);
    SelectObject(m, old);
    DeleteObject(bmp);
    DeleteDC(m);
}

static void OnTick()
{
    DWORD now = GetTickCount();
    float dt = (now - g_last) / 1000.f;
    g_last = now;

    // once a second: did the default device or speaker layout change?
    if ((g_chk += dt) > 1.f) {
        g_chk = 0;
        std::wstring oldId = g_devId; int oldN = g_n;
        if (!Acquire() || g_devId != oldId || g_n != oldN) { BuildUI(); return; }
    }
    if (!g_meter) return;

    float peaks[32] = {};
    UINT cnt = 0;
    g_meter->GetMeteringChannelCount(&cnt);
    if (cnt > 32) cnt = 32;
    if (cnt) g_meter->GetChannelsPeakValues(cnt, peaks);

    for (size_t i = 0; i < g_ch.size(); i++) {
        Channel& c = g_ch[i];
        float p = i < cnt ? peaks[i] : 0.f;
        float db = p > 0 ? 20.f * std::log10(p) : MIN_DB;
        float target = std::min(1.f, std::max(0.f, (db - MIN_DB) / -MIN_DB));

        c.level = target > c.level ? target : std::max(target, c.level - dt * 0.8f);
        if (c.level >= c.hold) { c.hold = c.level; c.holdTimer = 0.8f; }
        else if ((c.holdTimer -= dt) < 0) c.hold = std::max(c.level, c.hold - dt * 0.5f);

        int lit = (int)(c.level * SEGS + 0.5f), hs = (int)(c.hold * SEGS + 0.5f) - 1;
        if (lit != c.lit || hs != c.holdSeg) {   // redraw only when something visible changed
            c.lit = lit; c.holdSeg = hs;
            InvalidateRect(g_wnd, &c.meter, FALSE);
        }
    }
}

// ---------- show / hide ----------
static void ShowPanel()
{
    BuildUI();
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    RECT wr; GetWindowRect(g_wnd, &wr);
    int x = wa.right - (wr.right - wr.left) - 12, y = wa.bottom - (wr.bottom - wr.top) - 12;
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_wnd);
    g_last = GetTickCount(); g_chk = 0;
    SetTimer(g_wnd, ID_TIMER, 33, nullptr);
}

static void HidePanel()
{
    KillTimer(g_wnd, ID_TIMER);
    ShowWindow(g_wnd, SW_HIDE);
    DestroyChildren();
    g_vol.Reset(); g_meter.Reset(); g_dev.Reset();
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);   // give memory back to Windows
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbarMsg) { Shell_NotifyIconW(NIM_ADD, &g_nid); return 0; }   // explorer restarted

    switch (msg) {
    case WM_TRAY:
        if (lp == WM_LBUTTONUP) {
            if (IsWindowVisible(h)) HidePanel(); else ShowPanel();
        } else if (lp == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(h);
            TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, h, nullptr);
            PostMessageW(h, WM_NULL, 0, 0);
            DestroyMenu(menu);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_EXIT) DestroyWindow(h);
        return 0;

    case WM_TIMER:
        if (wp == ID_TIMER) OnTick();
        return 0;

    case WM_VSCROLL: {
        HWND tb = (HWND)lp;
        for (size_t i = 0; i < g_ch.size(); i++) {
            if (g_ch[i].slider != tb) continue;
            int pos = (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
            int pct = 100 - pos;
            float v = pct / 100.f;
            wchar_t buf[16]; wsprintfW(buf, L"%d%%", pct);
            SetWindowTextW(g_ch[i].pct, buf);
            if (g_vol) {
                HRESULT hr = g_vol->SetChannelVolumeLevelScalar((UINT)i, v, nullptr);
                float rb = v; g_vol->GetChannelVolumeLevelScalar((UINT)i, &rb);
                if (g_status)
                    SetWindowTextW(g_status, (FAILED(hr) || std::fabs(rb - v) > 0.05f)
                        ? L"Warning: the audio driver ignored the per-channel volume change." : L"");
            }
            break;
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
        for (auto& c : g_ch) { RECT t; if (IntersectRect(&t, &ps.rcPaint, &c.meter)) DrawMeter(hdc, c); }
        EndPaint(h, &ps);
        return 0;
    }

    case WM_CLOSE:       // X button: back to the tray instead of quitting
        HidePanel();
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"ChannelMixerTrayMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;   // already running

    g_inst = hInst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX ic = {sizeof(ic), ICC_BAR_CLASSES};
    InitCommonControlsEx(&ic);
    g_taskbarMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"ChannelMixerWnd";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    g_wnd = CreateWindowExW(kExStyle, wc.lpszClassName, L"Channel Mixer", kStyle,
                            CW_USEDEFAULT, CW_USEDEFAULT, 400, 380, nullptr, nullptr, hInst, nullptr);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);   // fallback if built without the icon
    lstrcpynW(g_nid.szTip, L"Channel Mixer", 128);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_vol.Reset(); g_meter.Reset(); g_dev.Reset();
    CoUninitialize();
    if (mtx) CloseHandle(mtx);
    return 0;
}
