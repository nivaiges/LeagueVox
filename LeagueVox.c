/*
 * LeagueVox - voice-activated hotkey that wakes up only while League runs.
 *
 * Idle:   one process-list check every poll_ms (default 2 s). No audio open.
 * Active: WASAPI shared-mode capture, event driven (~10 ms packets),
 *         RMS energy gate with attack / hangover. Key is sent via SendInput.
 * Exit of League is detected by waiting on the process handle (zero cost).
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -O2 -s -municode -mwindows -o LeagueVox.exe LeagueVox.c \
 *       -lole32 -lshell32 -luser32 -lgdi32 -ladvapi32 -lm
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <math.h>
#include <wchar.h>
#include <stdio.h>

#define APP_NAME   L"LeagueVox"
#define WM_TRAY    (WM_APP + 1)
#define WM_STATE   (WM_APP + 2)
#define WM_LEVEL   (WM_APP + 3)
#define WM_REKEY   (WM_APP + 4)                  /* config reloaded: re-register mute hotkey */
#define HK_MUTE    1

enum { ST_OFF, ST_WAIT, ST_LISTEN, ST_TALK, ST_ERR };
enum { ID_STATUS = 100, ID_MUTE, ID_TEST, ID_EDIT, ID_RELOAD, ID_EXIT };

/* GUIDs defined locally so we don't depend on libuuid having them. */
static const GUID xCLSID_MMDeviceEnumerator = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID xIID_IMMDeviceEnumerator  = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID xIID_IAudioClient         = {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const GUID xIID_IAudioCaptureClient  = {0xC8ADBD64,0xE71E,0x48A0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};
static const GUID xSUBTYPE_FLOAT            = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const PROPERTYKEY xPKEY_FriendlyName = {{0xA45C254E,0xDF1C,0x4EFD,{0x80,0x20,0x67,0xD1,0x46,0xA8,0x50,0xE0}}, 14};

typedef struct {
    wchar_t procs[512];
    wchar_t device[256];
    WORD    vk, scan;
    BOOL    ext;
    int     mouse;          /* 0 = keyboard, 3 = middle, 4 = XBUTTON1, 5 = XBUTTON2 */
    BOOL    tap;
    float   openDb, closeDb;
    int     attackMs, releaseMs, pollMs;
    WORD    muteVk;         /* 0 = no mute hotkey */
    UINT    muteMods;       /* MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN */
    BOOL    muteBeep;
    wchar_t muteName[64];
} Config;

static HWND          g_hwnd;
static HANDLE        g_wake;                 /* auto-reset: state changed, re-evaluate */
static HANDLE        g_thread;
static volatile LONG g_quit, g_enabled = 1, g_test, g_reload = 1, g_muted;
static int           g_state = -1;
static int           g_levelDb10 = -1200;
static BOOL          g_hotkeyOk;
static HICON         g_icons[5], g_iconMuted;
static UINT          g_taskbarCreated;
static wchar_t       g_ini[MAX_PATH], g_exe[MAX_PATH];
static Config        g_cfg;

/* ------------------------------------------------------------------ config */

static const char *DEFAULT_INI =
"[LeagueVox]\r\n"
"processes=League of Legends.exe\r\n"
"talk_key=\r\n"
"mode=hold\r\n"
"mute_key=\r\n"
"mute_beep=1\r\n"
"device=\r\n"
"open_db=-42\r\n"
"close_db=-48\r\n"
"attack_ms=30\r\n"
"release_ms=400\r\n"
"poll_ms=2000\r\n";

static void ini_str(const wchar_t *k, const wchar_t *def, wchar_t *out, DWORD n) {
    GetPrivateProfileStringW(APP_NAME, k, def, out, n, g_ini);
}
static float ini_f(const wchar_t *k, float def) {
    wchar_t b[64], d[64];
    swprintf(d, 64, L"%g", def);
    ini_str(k, d, b, 64);
    return (float)_wtof(b);
}
static int ini_i(const wchar_t *k, int def) { return GetPrivateProfileIntW(APP_NAME, k, def, g_ini); }

static void trim(wchar_t *s) {
    wchar_t *p = s; size_t n;
    while (*p == L' ' || *p == L'\t' || *p == L'"') p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t));
    n = wcslen(s);
    while (n && (s[n-1] == L' ' || s[n-1] == L'\t' || s[n-1] == L'"')) s[--n] = 0;
}

/* Key name (already trimmed + uppercased) -> VK. Returns 0 if unknown. Mouse names set *mouse. */
static WORD key_vk(const wchar_t *k, BOOL *ext, int *mouse) {
    static const struct { const wchar_t *n; WORD vk; BOOL ext; } T[] = {
        {L"SPACE",VK_SPACE,0},{L"CAPSLOCK",VK_CAPITAL,0},{L"TAB",VK_TAB,0},
        {L"LSHIFT",VK_LSHIFT,0},{L"RSHIFT",VK_RSHIFT,0},{L"LCTRL",VK_LCONTROL,0},
        {L"RCTRL",VK_RCONTROL,1},{L"LALT",VK_LMENU,0},{L"RALT",VK_RMENU,1},
        {L"INSERT",VK_INSERT,1},{L"DELETE",VK_DELETE,1},{L"HOME",VK_HOME,1},
        {L"END",VK_END,1},{L"PAGEUP",VK_PRIOR,1},{L"PAGEDOWN",VK_NEXT,1},
        {L"SCROLLLOCK",VK_SCROLL,0},{L"PAUSE",VK_PAUSE,0},{L"GRAVE",VK_OEM_3,0},
        {L"UP",VK_UP,1},{L"DOWN",VK_DOWN,1},{L"LEFT",VK_LEFT,1},{L"RIGHT",VK_RIGHT,1},
    };
    size_t i;
    *ext = FALSE; *mouse = 0;
    if (!wcscmp(k, L"MOUSE3")) { *mouse = 3; return 0; }
    if (!wcscmp(k, L"MOUSE4")) { *mouse = 4; return 0; }
    if (!wcscmp(k, L"MOUSE5")) { *mouse = 5; return 0; }
    if (k[0] == L'0' && k[1] == L'X') return (WORD)wcstoul(k, NULL, 16);
    if (k[0] == L'F' && k[1] >= L'1' && k[1] <= L'9') {
        int n = _wtoi(k + 1); return (n >= 1 && n <= 24) ? (WORD)(VK_F1 + n - 1) : 0;
    }
    if (!wcsncmp(k, L"NUMPAD", 6) && k[6] >= L'0' && k[6] <= L'9' && !k[7])
        return (WORD)(VK_NUMPAD0 + (k[6] - L'0'));
    if (k[0] && !k[1] && ((k[0] >= L'A' && k[0] <= L'Z') || (k[0] >= L'0' && k[0] <= L'9')))
        return (WORD)k[0];
    for (i = 0; i < sizeof T / sizeof *T; i++)
        if (!wcscmp(k, T[i].n)) { *ext = T[i].ext; return T[i].vk; }
    return 0;
}

static void parse_key(const wchar_t *in, Config *c) {
    wchar_t k[64];
    lstrcpynW(k, in, 64); trim(k); CharUpperW(k);
    c->scan = 0;
    c->vk = key_vk(k, &c->ext, &c->mouse);
    if (c->mouse || !c->vk) return;         /* !vk: empty or unknown = unbound */

    c->scan = (WORD)MapVirtualKeyW(c->vk, MAPVK_VK_TO_VSC);
    if (!c->scan) {                     /* F13-F24 aren't on most layouts' tables */
        if (c->vk >= VK_F13 && c->vk <= VK_F23) c->scan = (WORD)(0x64 + (c->vk - VK_F13));
        else if (c->vk == VK_F24)               c->scan = 0x76;
    }
}

/* "CTRL+SHIFT+F14" -> modifiers + VK. Leaves muteVk = 0 if empty / invalid / mouse. */
static void parse_mute(const wchar_t *in, Config *c) {
    wchar_t buf[64], *tok, *plus; BOOL ext; int mouse;
    c->muteVk = 0; c->muteMods = 0;
    lstrcpynW(buf, in, 64); trim(buf); CharUpperW(buf);
    lstrcpynW(c->muteName, buf, 64);
    for (tok = buf; ; tok = plus + 1) {
        plus = wcschr(tok, L'+');
        if (plus) *plus = 0;
        trim(tok);
        if (!plus) break;
        if      (!wcscmp(tok, L"CTRL") || !wcscmp(tok, L"CONTROL")) c->muteMods |= MOD_CONTROL;
        else if (!wcscmp(tok, L"ALT"))                              c->muteMods |= MOD_ALT;
        else if (!wcscmp(tok, L"SHIFT"))                            c->muteMods |= MOD_SHIFT;
        else if (!wcscmp(tok, L"WIN"))                              c->muteMods |= MOD_WIN;
        else return;                                                /* unknown modifier */
    }
    if (*tok) c->muteVk = key_vk(tok, &ext, &mouse);
    if (mouse) c->muteVk = 0;
}

static void load_config(Config *c) {
    wchar_t b[64];
    if (GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(g_ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, DEFAULT_INI, (DWORD)strlen(DEFAULT_INI), &w, NULL); CloseHandle(f); }
    }
    ini_str(L"processes", L"League of Legends.exe", c->procs, 512);
    ini_str(L"device", L"", c->device, 256); trim(c->device);
    { wchar_t old[64];                      /* "key" was the old name; talk_key wins if both exist */
      ini_str(L"key", L"", old, 64); ini_str(L"talk_key", old, b, 64); parse_key(b, c); }
    ini_str(L"mode", L"hold", b, 64); trim(b); c->tap = !lstrcmpiW(b, L"tap");
    c->openDb    = ini_f(L"open_db", -42.f);
    c->closeDb   = ini_f(L"close_db", c->openDb - 6.f);
    if (c->closeDb > c->openDb) c->closeDb = c->openDb;
    c->attackMs  = max(0,   ini_i(L"attack_ms", 30));
    c->releaseMs = max(50,  ini_i(L"release_ms", 400));
    c->pollMs    = max(250, ini_i(L"poll_ms", 2000));
    ini_str(L"mute_key", L"", b, 64); parse_mute(b, c);
    c->muteBeep  = ini_i(L"mute_beep", 1) != 0;
}

/* ------------------------------------------------------------------- input */

static void send_one(const Config *c, BOOL down) {
    INPUT in;
    if (!c->mouse && !c->vk) return;        /* no talk key bound (test mode still shows levels) */
    ZeroMemory(&in, sizeof in);
    if (c->mouse) {
        in.type = INPUT_MOUSE;
        if (c->mouse == 3) in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        else { in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
               in.mi.mouseData = c->mouse == 4 ? XBUTTON1 : XBUTTON2; }
    } else {
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = c->vk;           /* VK for hooks/messages (Discord), scan for raw input/games */
        in.ki.wScan = c->scan;
        in.ki.dwFlags = (c->ext ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    }
    SendInput(1, &in, sizeof in);
}

static void voice_on(const Config *c)  { if (c->tap) { send_one(c, 1); send_one(c, 0); } else send_one(c, 1); }
static void voice_off(const Config *c) { if (c->tap) { send_one(c, 1); send_one(c, 0); } else send_one(c, 0); }

/* --------------------------------------------------------------- processes */

static BOOL name_in_list(const wchar_t *exe, const wchar_t *list) {
    wchar_t buf[512], *tok = buf, *comma;
    lstrcpynW(buf, list, 512);
    for (;;) {
        comma = wcschr(tok, L',');
        if (comma) *comma = 0;
        trim(tok);
        if (*tok && !lstrcmpiW(exe, tok)) return TRUE;
        if (!comma) return FALSE;
        tok = comma + 1;
    }
}

static DWORD find_pid(const wchar_t *list) {
    PROCESSENTRY32W pe; DWORD pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof pe;
    if (Process32FirstW(s, &pe))
        do { if (name_in_list(pe.szExeFile, list)) { pid = pe.th32ProcessID; break; } }
        while (Process32NextW(s, &pe));
    CloseHandle(s);
    return pid;
}

static BOOL pid_alive(DWORD pid, const wchar_t *list) {
    PROCESSENTRY32W pe; BOOL ok = FALSE;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return TRUE;
    pe.dwSize = sizeof pe;
    if (Process32FirstW(s, &pe))
        do { if (pe.th32ProcessID == pid && name_in_list(pe.szExeFile, list)) { ok = TRUE; break; } }
        while (Process32NextW(s, &pe));
    CloseHandle(s);
    return ok;
}

/* ------------------------------------------------------------------- audio */

static void set_state(int st) { PostMessageW(g_hwnd, WM_STATE, (WPARAM)st, 0); }

static IMMDevice *pick_device(IMMDeviceEnumerator *en, const wchar_t *want) {
    IMMDevice *dev = NULL;
    if (want[0]) {
        IMMDeviceCollection *col; UINT n = 0, i; wchar_t w[256];
        lstrcpynW(w, want, 256); CharLowerW(w);
        if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eCapture, DEVICE_STATE_ACTIVE, &col))) {
            IMMDeviceCollection_GetCount(col, &n);
            for (i = 0; i < n && !dev; i++) {
                IMMDevice *d; IPropertyStore *ps; PROPVARIANT v;
                if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
                if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps))) {
                    PropVariantInit(&v);
                    if (SUCCEEDED(IPropertyStore_GetValue(ps, &xPKEY_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
                        wchar_t nm[256]; lstrcpynW(nm, v.pwszVal, 256); CharLowerW(nm);
                        if (wcsstr(nm, w)) { dev = d; IMMDevice_AddRef(dev); }
                    }
                    PropVariantClear(&v);
                    IPropertyStore_Release(ps);
                }
                IMMDevice_Release(d);
            }
            IMMDeviceCollection_Release(col);
        }
    }
    if (!dev) IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eCapture, eConsole, &dev);
    return dev;
}

/* Returns FALSE on audio error (caller backs off and retries). */
static BOOL run_session(const Config *c, HANDLE proc, DWORD pid) {
    IMMDeviceEnumerator *en = NULL; IMMDevice *dev = NULL; IAudioClient *ac = NULL;
    IAudioCaptureClient *cc = NULL; WAVEFORMATEX *wf = NULL; HANDLE ev = NULL;
    BOOL ok = FALSE, isFloat, talking = FALSE;
    UINT32 rate, ch, bits, attackF, releaseF, above = 0, quiet = 0;
    ULONGLONG lastPoll, lastLevel;
    float peak = -120.f;

    if (FAILED(CoCreateInstance(&xCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &xIID_IMMDeviceEnumerator, (void **)&en))) goto done;
    if (!(dev = pick_device(en, c->device))) goto done;
    if (FAILED(IMMDevice_Activate(dev, &xIID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac))) goto done;
    if (FAILED(IAudioClient_GetMixFormat(ac, &wf))) goto done;

    rate = wf->nSamplesPerSec; ch = wf->nChannels; bits = wf->wBitsPerSample;
    isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
              (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               IsEqualGUID(&((WAVEFORMATEXTENSIBLE *)wf)->SubFormat, &xSUBTYPE_FLOAT));
    if (!(isFloat && bits == 32) && bits != 16 && bits != 32) goto done;

    if (FAILED(IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, wf, NULL))) goto done;
    if (!(ev = CreateEventW(NULL, FALSE, FALSE, NULL))) goto done;
    if (FAILED(IAudioClient_SetEventHandle(ac, ev))) goto done;
    if (FAILED(IAudioClient_GetService(ac, &xIID_IAudioCaptureClient, (void **)&cc))) goto done;
    if (FAILED(IAudioClient_Start(ac))) goto done;

    attackF  = (UINT32)((ULONGLONG)rate * c->attackMs / 1000);
    releaseF = (UINT32)((ULONGLONG)rate * c->releaseMs / 1000);
    lastPoll = lastLevel = GetTickCount64();
    set_state(ST_LISTEN);
    ok = TRUE;

    for (;;) {
        HANDLE hs[3] = { g_wake, ev, proc };
        DWORD r = WaitForMultipleObjects(proc ? 3 : 2, hs, FALSE, 2000);
        UINT32 pk = 0;
        HRESULT hr;
        ULONGLONG now;
        if (r == WAIT_OBJECT_0 || r == WAIT_OBJECT_0 + 2 || r == WAIT_FAILED) break;

        while ((hr = IAudioCaptureClient_GetNextPacketSize(cc, &pk)) == S_OK && pk) {
            BYTE *data; UINT32 frames, n, i; DWORD flags; double sum = 0; float db;
            if (FAILED(hr = IAudioCaptureClient_GetBuffer(cc, &data, &frames, &flags, NULL, NULL))) break;
            n = frames * ch;
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && n) {
                if (isFloat)        { const float *s = (const float *)data; for (i = 0; i < n; i++) sum += (double)s[i] * s[i]; }
                else if (bits == 16){ const short *s = (const short *)data; for (i = 0; i < n; i++) sum += (double)s[i] * s[i]; sum /= 32768.0 * 32768.0; }
                else                { const int   *s = (const int   *)data; for (i = 0; i < n; i++) { double x = s[i] / 2147483648.0; sum += x * x; } }
            }
            IAudioCaptureClient_ReleaseBuffer(cc, frames);
            db = (n && sum > 1e-12 * n) ? (float)(10.0 * log10(sum / n)) : -120.f;
            if (db > peak) peak = db;

            if (g_muted) {                           /* muted: never press, release at once */
                if (talking) { talking = FALSE; voice_off(c); set_state(ST_LISTEN); }
                above = 0;
            } else if (!talking) {
                if (db >= c->openDb) {
                    above += frames;
                    if (above >= attackF) { talking = TRUE; quiet = 0; voice_on(c); set_state(ST_TALK); }
                } else above = 0;
            } else if (db >= c->closeDb) quiet = 0;
            else if ((quiet += frames) >= releaseF) { talking = FALSE; above = 0; voice_off(c); set_state(ST_LISTEN); }
        }
        if (FAILED(hr)) { ok = FALSE; break; }       /* device unplugged / changed */

        now = GetTickCount64();
        if (now - lastLevel >= 1000) {
            PostMessageW(g_hwnd, WM_LEVEL, (WPARAM)(int)(peak * 10), 0);
            peak = -120.f; lastLevel = now;
        }
        if (!proc && pid && now - lastPoll >= (ULONGLONG)c->pollMs) {   /* no handle: poll instead */
            lastPoll = now;
            if (!pid_alive(pid, c->procs)) break;
        }
    }
    if (talking) voice_off(c);
    IAudioClient_Stop(ac);

done:
    if (cc) IAudioCaptureClient_Release(cc);
    if (ev) CloseHandle(ev);
    if (wf) CoTaskMemFree(wf);
    if (ac) IAudioClient_Release(ac);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    return ok;
}

static DWORD WINAPI worker(LPVOID unused) {
    Config c;
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    while (!g_quit) {
        DWORD pid = 0; HANDLE proc = NULL;
        if (InterlockedExchange(&g_reload, 0)) { load_config(&c); g_cfg = c; PostMessageW(g_hwnd, WM_REKEY, 0, 0); }
        /* Off until a talk key is set (config reload wakes us). Test mode still runs, to tune levels. */
        if (!g_enabled || (!c.vk && !c.mouse && !g_test)) { set_state(ST_OFF); WaitForSingleObject(g_wake, INFINITE); continue; }
        if (!g_test) {
            if (!(pid = find_pid(c.procs))) { set_state(ST_WAIT); WaitForSingleObject(g_wake, (DWORD)c.pollMs); continue; }
            proc = OpenProcess(SYNCHRONIZE, FALSE, pid);   /* may be NULL if protected; we poll then */
        }
        if (!run_session(&c, proc, pid)) { set_state(ST_ERR); WaitForSingleObject(g_wake, 1500); }
        if (proc) CloseHandle(proc);
    }
    CoUninitialize();
    return 0;
}

/* ---------------------------------------------------------- config watch */

/* Sleeps in the kernel until something in the exe folder is written,
   then reloads only if LeagueVox.ini's timestamp actually changed. */
static DWORD WINAPI watch_config(LPVOID unused) {
    wchar_t dir[MAX_PATH], *slash;
    WIN32_FILE_ATTRIBUTE_DATA a; FILETIME last = {0, 0};
    HANDLE ch;
    (void)unused;
    lstrcpynW(dir, g_ini, MAX_PATH);
    if ((slash = wcsrchr(dir, L'\\'))) *slash = 0;
    if (GetFileAttributesExW(g_ini, GetFileExInfoStandard, &a)) last = a.ftLastWriteTime;
    ch = FindFirstChangeNotificationW(dir, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
    if (ch == INVALID_HANDLE_VALUE) return 0;
    while (!g_quit && WaitForSingleObject(ch, INFINITE) == WAIT_OBJECT_0) {
        Sleep(150);                                  /* let the editor finish writing */
        if (GetFileAttributesExW(g_ini, GetFileExInfoStandard, &a) &&
            CompareFileTime(&a.ftLastWriteTime, &last)) {
            last = a.ftLastWriteTime;
            InterlockedExchange(&g_reload, 1);
            SetEvent(g_wake);
        }
        if (!FindNextChangeNotification(ch)) break;
    }
    FindCloseChangeNotification(ch);
    return 0;
}

/* --------------------------------------------------------------------- UI */

static HICON make_icon(DWORD rgb) {
    int s = GetSystemMetrics(SM_CXSMICON), x, y;
    BITMAPV5HEADER bi; void *bits; HDC dc; HBITMAP col, mask; ICONINFO ii; HICON h;
    float r = s / 2.0f, rad = r - 1.0f;
    BYTE R = (BYTE)(rgb >> 16), G = (BYTE)(rgb >> 8), B = (BYTE)rgb;
    ZeroMemory(&bi, sizeof bi);
    bi.bV5Size = sizeof bi; bi.bV5Width = s; bi.bV5Height = -s; bi.bV5Planes = 1;
    bi.bV5BitCount = 32; bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0xFF0000; bi.bV5GreenMask = 0xFF00; bi.bV5BlueMask = 0xFF; bi.bV5AlphaMask = 0xFF000000;
    dc = GetDC(NULL);
    col = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    for (y = 0; y < s; y++) for (x = 0; x < s; x++) {
        float dx = x + 0.5f - r, dy = y + 0.5f - r, cov = rad - sqrtf(dx * dx + dy * dy) + 0.5f;
        BYTE a = (BYTE)(255 * (cov < 0 ? 0 : cov > 1 ? 1 : cov));
        ((DWORD *)bits)[y * s + x] = ((DWORD)a << 24) | ((DWORD)(R * a / 255) << 16) | ((DWORD)(G * a / 255) << 8) | (DWORD)(B * a / 255);
    }
    mask = CreateBitmap(s, s, 1, 1, NULL);
    ii.fIcon = TRUE; ii.xHotspot = ii.yHotspot = 0; ii.hbmMask = mask; ii.hbmColor = col;
    h = CreateIconIndirect(&ii);
    DeleteObject(mask); DeleteObject(col);
    return h;
}

static const wchar_t *state_text(int st) {
    switch (st) {
    case ST_OFF:    return L"No talk key - set talk_key in Edit config";
    case ST_WAIT:   return L"Waiting for League";
    case ST_LISTEN: return g_test ? L"Listening (test mode)" : L"Listening";
    case ST_TALK:   return L"Talking - key down";
    default:        return L"Mic error - retrying";
    }
}

static void tray(DWORD msg) {
    NOTIFYICONDATAW nid; int st = g_state < 0 ? ST_WAIT : g_state;
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid; nid.hWnd = g_hwnd; nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE; nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_muted && st != ST_OFF && st != ST_ERR ? g_iconMuted : g_icons[st];
    if (g_muted)
        swprintf(nid.szTip, 128, L"LeagueVox - MUTED (%ls to unmute)\n%ls", g_hotkeyOk ? g_cfg.muteName : L"tray menu", state_text(st));
    else if (st == ST_LISTEN || st == ST_TALK)
        swprintf(nid.szTip, 128, L"LeagueVox - %ls\npeak %d dB (opens at %d)", state_text(st), g_levelDb10 / 10, (int)g_cfg.openDb);
    else
        swprintf(nid.szTip, 128, L"LeagueVox - %ls", state_text(st));
    Shell_NotifyIconW(msg, &nid);
}

static void startup_set(BOOL on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k)) return;
    if (on) {
        wchar_t v[MAX_PATH + 3];
        swprintf(v, MAX_PATH + 3, L"\"%ls\"", g_exe);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, APP_NAME);
    RegCloseKey(k);
}

static DWORD WINAPI beep_thread(LPVOID freq) { Beep((DWORD)(ULONG_PTR)freq, 70); return 0; }

static void toggle_mute(void) {
    LONG now = !g_muted;
    InterlockedExchange(&g_muted, now);       /* worker sees it on the next ~10 ms audio packet */
    if (g_cfg.muteBeep) {
        HANDLE t = CreateThread(NULL, 0, beep_thread, (LPVOID)(ULONG_PTR)(now ? 440 : 880), 0, NULL);
        if (t) CloseHandle(t);
    }
    tray(NIM_MODIFY);
}

/* Must run on the window's thread. */
static void register_mute_key(void) {
    UnregisterHotKey(g_hwnd, HK_MUTE);
    g_hotkeyOk = FALSE;
    if (!g_cfg.muteVk) return;
    /* Our own SendInput would trip the hotkey if it's the same bare key as the talk key. */
    if (!g_cfg.mouse && !g_cfg.muteMods && g_cfg.muteVk == g_cfg.vk) return;
    g_hotkeyOk = RegisterHotKey(g_hwnd, HK_MUTE, g_cfg.muteMods | MOD_NOREPEAT, g_cfg.muteVk);
}

static void show_menu(void) {
    POINT pt; HMENU m = CreatePopupMenu();
    wchar_t muteLabel[128];
    if (g_hotkeyOk)          swprintf(muteLabel, 128, L"Mute\t%ls", g_cfg.muteName);
    else if (g_cfg.muteVk)   swprintf(muteLabel, 128, L"Mute\t(%ls unavailable)", g_cfg.muteName);
    else                     lstrcpynW(muteLabel, L"Mute", 128);
    AppendMenuW(m, MF_STRING | MF_GRAYED, ID_STATUS, state_text(g_state < 0 ? ST_WAIT : g_state));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_muted ? MF_CHECKED : 0), ID_MUTE, muteLabel);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_test ? MF_CHECKED : 0), ID_TEST, L"Test mic now (ignore League)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EDIT, L"Edit config");
    AppendMenuW(m, MF_STRING, ID_RELOAD, L"Reload config");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && msg) { tray(NIM_ADD); return 0; }
    switch (msg) {
    case WM_STATE:
        if ((int)wp != g_state) { g_state = (int)wp; tray(NIM_MODIFY); }
        return 0;
    case WM_LEVEL:
        g_levelDb10 = (int)wp; tray(NIM_MODIFY);
        return 0;
    case WM_REKEY:
        register_mute_key(); tray(NIM_MODIFY);
        return 0;
    case WM_HOTKEY:
        if (wp == HK_MUTE) toggle_mute();
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) show_menu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_MUTE:    toggle_mute(); break;
        case ID_TEST:    InterlockedExchange(&g_test, !g_test); if (g_test) InterlockedExchange(&g_enabled, 1); SetEvent(g_wake); break;
        case ID_EDIT:    ShellExecuteW(NULL, L"open", L"notepad.exe", g_ini, NULL, SW_SHOWNORMAL); break;
        case ID_RELOAD:  InterlockedExchange(&g_reload, 1); SetEvent(g_wake); break;
        case ID_EXIT:    DestroyWindow(h); break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, LPWSTR cmd, int show) {
    WNDCLASSW wc; MSG m; wchar_t *slash; HANDLE mtx;
    (void)prev; (void)cmd; (void)show;

    mtx = CreateMutexW(NULL, TRUE, L"Local\\LeagueVox_single_instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    GetModuleFileNameW(NULL, g_exe, MAX_PATH);
    lstrcpynW(g_ini, g_exe, MAX_PATH);
    if ((slash = wcsrchr(g_ini, L'\\'))) slash[1] = 0;
    wcsncat(g_ini, L"LeagueVox.ini", MAX_PATH - wcslen(g_ini) - 1);
    load_config(&g_cfg);                       /* creates default ini on first run */
    startup_set(TRUE);                         /* always start with Windows */

    g_icons[ST_OFF]    = make_icon(0x4A4A4A);
    g_icons[ST_WAIT]   = make_icon(0x8E8E8E);
    g_icons[ST_LISTEN] = make_icon(0x2ECC71);
    g_icons[ST_TALK]   = make_icon(0xE74C3C);
    g_icons[ST_ERR]    = make_icon(0xF39C12);
    g_iconMuted        = make_icon(0x8E44AD);  /* purple */

    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.lpszClassName = APP_NAME;
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, APP_NAME, APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_state = ST_WAIT;
    register_mute_key();
    tray(NIM_ADD);

    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_thread = CreateThread(NULL, 64 * 1024, worker, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    CloseHandle(CreateThread(NULL, 64 * 1024, watch_config, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL));

    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }

    InterlockedExchange(&g_quit, 1);
    SetEvent(g_wake);
    WaitForSingleObject(g_thread, 3000);       /* worker releases the key if held */
    UnregisterHotKey(g_hwnd, HK_MUTE);
    tray(NIM_DELETE);
    if (mtx) CloseHandle(mtx);
    return 0;
}
