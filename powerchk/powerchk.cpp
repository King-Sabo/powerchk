// powerchk.cpp — always-on-top LED-style grid power widget for the everHome EcoTracker
//
// Primary source is the EcoTracker's LOCAL REST API. If the local address is
// unreachable, powerchk falls back to the everHome CLOUD API (OAuth2). Cloud
// credentials are read from "powerchk.credentials" next to the EXE.
//
// Consumption (import) is shown in red, feed-in (export) in green, as glowing
// seven-segment digits. A small cyan dot in the top-right corner means the
// current reading came from the cloud fallback rather than the local device.
//
// ---------------------------------------------------------------------------
// One-time cloud enrollment (needed because everHome uses OAuth2 auth-code):
//   1. Create an OAuth2 app at https://everhome.cloud/en/developer/applications
//      and set its redirect URL to:  http://localhost:53127/callback
//   2. Put client_id and client_secret into powerchk.credentials (see the
//      shipped powerchk.credentials.example).
//   3. Run:  powerchk.exe --login
//      A browser opens; approve access. powerchk stores the refresh_token and
//      writes your devices to everhome_devices.json — copy your EcoTracker's
//      id into device_id in powerchk.credentials.
// After that, just run powerchk.exe normally.
// ---------------------------------------------------------------------------
//
// Build (x64 Native Tools Command Prompt), static CRT, no redistributable:
//   cl /nologo /std:c++17 /O2 /MT /EHsc /DUNICODE /D_UNICODE powerchk.cpp ^
//      /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib winhttp.lib ^
//      shell32.lib ws2_32.lib
//
// Run:
//   powerchk.exe                          local http://192.168.1.111/v1/json, 1 s
//   powerchk.exe 192.168.1.111            local host or IP
//   powerchk.exe http://host/v1/json 2000 full local URL + poll interval (ms)
//   powerchk.exe --login [port]           one-time cloud enrollment
//
// Drag with the left mouse button anywhere; right-click for Exit.
//
// The one hardware assumption is the sign of "power": see NEGATIVE_IS_FEEDIN.

#define WIN32_LEAN_AND_MEAN
#define WINVER       0x0A00
#define _WIN32_WINNT 0x0A00
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <objidl.h>   // IStream, PROPID, byte — required by <gdiplus.h> under WIN32_LEAN_AND_MEAN
#include <gdiplus.h>
#include <playsoundapi.h>   // PlaySoundW
#include "resource.h"
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <utility>
#include <thread>
#include <mutex>
#include <atomic>
#include <optional>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <cctype>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")

using namespace Gdiplus;

// ---- configuration ---------------------------------------------------------

// Sign convention of the meter's "power" field. Most grid meters report a
// signed value: positive = drawing from the grid, negative = feeding in. If
// yours is reversed, set this to false.
static constexpr bool NEGATIVE_IS_FEEDIN = true;

static constexpr bool SHOW_COUNTERS = true;   // tiny in/out kWh line at the bottom
static constexpr int  DIGIT_COUNT   = 5;      // fits up to 99999 W

static constexpr const wchar_t* CRED_FILE = L"powerchk.credentials";
static constexpr int      DEFAULT_REDIRECT_PORT = 53127;

// ---------------------------------------------------------------------------

#define WM_APP_DATA  (WM_APP + 1)
#define WM_APP_FLASH (WM_APP + 2)
static constexpr UINT_PTR FLASH_TIMER_ID = 1;

enum { SEG_A = 1, SEG_B = 2, SEG_C = 4, SEG_D = 8, SEG_E = 16, SEG_F = 32, SEG_G = 64 };

static constexpr int kSeg7[10] = {
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F,       // 0
    SEG_B|SEG_C,                               // 1
    SEG_A|SEG_B|SEG_G|SEG_E|SEG_D,             // 2
    SEG_A|SEG_B|SEG_G|SEG_C|SEG_D,             // 3
    SEG_F|SEG_G|SEG_B|SEG_C,                   // 4
    SEG_A|SEG_F|SEG_G|SEG_C|SEG_D,             // 5
    SEG_A|SEG_F|SEG_G|SEG_E|SEG_C|SEG_D,       // 6
    SEG_A|SEG_B|SEG_C,                         // 7
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F|SEG_G, // 8
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_F|SEG_G        // 9
};

static int MaskForChar(wchar_t c) {
    if (c >= L'0' && c <= L'9') return kSeg7[c - L'0'];
    if (c == L'-')              return SEG_G;
    return 0;
}

enum Source { SrcNone, SrcLocal, SrcCloud };

struct Reading {
    double power = 0.0;   // W, signed
    double inKwh = 0.0;
    double outKwh = 0.0;
    bool   valid = false;
    Source source = SrcNone;
};

struct Layout {
    float scale = 1.0f;
    int   w = 0, h = 0;
    int   digits = DIGIT_COUNT;
    bool  showCounters = SHOW_COUNTERS;
    float cellW = 0, cellH = 0, cellGap = 0;
    float arrowX = 0, arrowW = 0;
    float digitsX = 0, digitsY = 0;
    float unitX = 0, unitSize = 0;
    float bottomY = 0, bottomH = 0, bottomFont = 0;
};

// ---- globals ---------------------------------------------------------------

static HWND               g_hwnd = nullptr;
static std::mutex         g_mtx;
static Reading            g_reading;
static std::atomic<bool>  g_running{ true };
static float              g_scale = 1.0f;      // effective = dpi * user
static float              g_dpiScale = 1.0f;
static float              g_userScale = 1.0f;  // persisted zoom (scale= in creds)
static float              g_stashedScale = 0.0f;  // size remembered by the reset toggle (0 = none)
static int                g_posX = 0, g_posY = 0;  // restored window position (screen px)
static bool               g_havePos = false;

static std::wstring       g_localHost = L"192.168.1.111";   // host/IP is overridable
static int                g_localIntervalMs = 1000;

static std::wstring       g_cloudHost = L"everhome.cloud";
static int                g_cloudIntervalMs = 60000;   // conservative; no documented limit
static std::wstring       g_cloudDevicePath;           // e.g. /device/123
static bool               g_cloudEnabled = false;

static std::string        g_clientId, g_clientSecret, g_refresh;  // UTF-8
static std::wstring       g_accessToken;                          // "Bearer" value
static ULONGLONG          g_accessExpiry = 0;                     // GetTickCount64 ms

static std::atomic<bool>  g_soundAlert{ true };   // beep on a green->red (export->import) edge
static std::atomic<bool>  g_flashAlert{ true };   // flash the window on a green->red edge
static std::wstring       g_alertSound;           // optional custom WAV; empty = system sound
static int                g_flashRemaining = 0;   // remaining hide/show toggles (UI thread)

// fixed endpoints (compile-time constants)
static constexpr std::wstring_view g_localPath = L"/v1/json";   // ecotracker local path
static constexpr INTERNET_PORT     g_cloudPort = 443;           // everHome cloud (HTTPS)

// ---- small utilities -------------------------------------------------------

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
static std::string Lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o += c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}
static std::string UrlDecode(const std::string& s) {
    auto hx = [](char h)->int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        return 0;
    };
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.size()) { o += (char)((hx(s[i+1]) << 4) | hx(s[i+2])); i += 2; }
        else if (c == '+') o += ' ';
        else o += c;
    }
    return o;
}
static std::string JsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else o += c; }
    return o;
}

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring(L".") : p.substr(0, s);
}
static bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, nullptr);
    std::string buf; buf.resize(sz);
    DWORD rd = 0;
    BOOL ok = sz == 0 ? TRUE : ReadFile(h, &buf[0], sz, &rd, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    buf.resize(rd);
    out.swap(buf);
    return true;
}
static bool WriteFileBytes(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    return ok && wr == data.size();
}

// ---- JSON: minimal field extractors ---------------------------------------

static std::optional<double> JsonNumber(const std::string& s, const char* key) {
    std::string needle = "\""; needle += key; needle += "\"";
    size_t pos = 0;
    while (true) {
        size_t k = s.find(needle, pos);
        if (k == std::string::npos) return std::nullopt;
        pos = k + needle.size();
        // treat the match as a KEY only if the next non-space char is ':'
        size_t j = pos;
        while (j < s.size() && isspace((unsigned char)s[j])) ++j;
        if (j >= s.size() || s[j] != ':') continue;   // it was a value/substring, not a key
        size_t i = j + 1;
        while (i < s.size() && isspace((unsigned char)s[i])) ++i;
        size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        bool any = false;
        while (i < s.size()) {
            char ch = s[i];
            if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-') { ++i; any = true; }
            else break;
        }
        if (any) {
            try { return std::stod(s.substr(start, i - start)); }
            catch (...) { /* keep searching */ }
        }
        // value wasn't numeric (e.g. the "power" object under statedefinitions) -> keep searching
    }
}
static std::optional<std::string> JsonString(const std::string& s, const char* key) {
    std::string needle = "\""; needle += key; needle += "\"";
    size_t k = s.find(needle);
    if (k == std::string::npos) return std::nullopt;
    size_t c = s.find(':', k + needle.size());
    if (c == std::string::npos) return std::nullopt;
    size_t i = c + 1;
    while (i < s.size() && isspace((unsigned char)s[i])) ++i;
    if (i >= s.size() || s[i] != '"') return std::nullopt;
    ++i;
    std::string out;
    while (i < s.size()) {
        char ch = s[i++];
        if (ch == '\\') {
            if (i < s.size()) {
                char e = s[i++];
                switch (e) {
                    case 'n': out += '\n'; break; case 't': out += '\t'; break;
                    case 'r': out += '\r'; break; case '"': out += '"';  break;
                    case '\\': out += '\\'; break; case '/': out += '/';  break;
                    default: out += e;
                }
            }
        } else if (ch == '"') return out;
        else out += ch;
    }
    return std::nullopt;
}

// ---- generic HTTP via WinHTTP ---------------------------------------------

struct HttpResult { bool ok = false; DWORD status = 0; std::string body; };

static HttpResult HttpRequest(const wchar_t* method, const std::wstring& host,
                              INTERNET_PORT port, bool secure, const std::wstring& path,
                              const std::wstring& extraHeaders, const std::string& reqBody,
                              bool useSystemProxy) {
    HttpResult R;
    DWORD access = useSystemProxy ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY;
    HINTERNET hS = WinHttpOpen(L"powerchk/1.0", access, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return R;
    WinHttpSetTimeouts(hS, 3000, 3000, 4000, 6000);
    HINTERNET hC = WinHttpConnect(hS, host.c_str(), port, 0);
    if (hC) {
        DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hR = WinHttpOpenRequest(hC, method, path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hR) {
            if (!extraHeaders.empty())
                WinHttpAddRequestHeaders(hR, extraHeaders.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
            LPVOID bodyPtr = reqBody.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)reqBody.data();
            DWORD bodyLen = (DWORD)reqBody.size();
            if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, bodyPtr, bodyLen, bodyLen, 0) &&
                WinHttpReceiveResponse(hR, nullptr)) {
                DWORD code = 0, sz = sizeof(code);
                WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
                R.status = code;
                DWORD avail = 0;
                do {
                    avail = 0;
                    if (!WinHttpQueryDataAvailable(hR, &avail)) break;
                    if (avail == 0) break;
                    std::string chunk(avail, '\0');
                    DWORD rd = 0;
                    if (!WinHttpReadData(hR, &chunk[0], avail, &rd)) break;
                    chunk.resize(rd);
                    R.body += chunk;
                } while (avail > 0);
                R.ok = true;
            }
            WinHttpCloseHandle(hR);
        }
        WinHttpCloseHandle(hC);
    }
    WinHttpCloseHandle(hS);
    return R;
}

// ---- credentials -----------------------------------------------------------

struct CredFile {
    std::wstring path;
    std::vector<std::string> raw;               // original lines (UTF-8)
    std::map<std::string, std::string> kv;      // lowercased key -> value
    bool loaded = false;
};
static CredFile g_cred;

static std::string CredGet(const char* key) {
    auto it = g_cred.kv.find(key);
    return it == g_cred.kv.end() ? std::string() : it->second;
}
static void LoadCreds() {
    g_cred.path = ExeDir() + L"\\" + CRED_FILE;
    std::string data;
    if (!ReadFileBytes(g_cred.path, data)) return;
    size_t i = 0;
    while (i <= data.size()) {
        size_t nl = data.find('\n', i);
        std::string line = data.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        g_cred.raw.push_back(line);
        std::string t = Trim(line);
        if (!t.empty() && t[0] != '#') {
            size_t eq = t.find('=');
            if (eq != std::string::npos) {
                std::string key = Lower(Trim(t.substr(0, eq)));
                std::string val = Trim(t.substr(eq + 1));
                // strip a whitespace-preceded inline comment: "value  # note" -> "value"
                // (a '#' not preceded by space is kept, so paths like C:\C#\a.wav survive)
                for (size_t i = 1; i < val.size(); ++i)
                    if (val[i] == '#' && isspace((unsigned char)val[i - 1])) { val = Trim(val.substr(0, i)); break; }
                g_cred.kv[key] = val;
            }
        }
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    g_cred.loaded = true;
}
static void SetCredLine(const std::string& key, const std::string& value) {
    std::string lkey = Lower(key);
    g_cred.kv[lkey] = value;
    for (auto& ln : g_cred.raw) {
        std::string t = Trim(ln);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (Lower(Trim(t.substr(0, eq))) == lkey) {
            ln = key + "=" + value;
            std::string out;
            for (auto& l : g_cred.raw) { out += l; out += "\r\n"; }
            WriteFileBytes(g_cred.path, out);
            return;
        }
    }
    g_cred.raw.push_back(key + "=" + value);
    std::string out;
    for (auto& l : g_cred.raw) { out += l; out += "\r\n"; }
    WriteFileBytes(g_cred.path, out);
}
static void SaveRefresh(const std::string& tok) { SetCredLine("refresh_token", tok); }

// ---- OAuth2 token handling -------------------------------------------------

struct TokenResp { std::string access, refresh; long expires = 0; };

static bool TokenPost(const std::string& contentType, const std::string& body, HttpResult& out) {
    std::wstring hdr = L"Content-Type: " + Widen(contentType);
    out = HttpRequest(L"POST", g_cloudHost, g_cloudPort, true, L"/oauth2/token", hdr, body, true);
    return out.ok;
}
static bool TokenRequest(const std::vector<std::pair<std::string, std::string>>& params, TokenResp& t) {
    // Try RFC-6749 form encoding first, then JSON (the everHome docs configure
    // their client with json:'force', so some deployments expect JSON).
    std::string form;
    for (auto& kv : params) { if (!form.empty()) form += '&'; form += UrlEncode(kv.first) + "=" + UrlEncode(kv.second); }

    HttpResult r;
    bool got = TokenPost("application/x-www-form-urlencoded", form, r) && r.status == 200;
    if (!got) {
        std::string js = "{";
        bool first = true;
        for (auto& kv : params) { if (!first) js += ","; first = false; js += "\"" + kv.first + "\":\"" + JsonEscape(kv.second) + "\""; }
        js += "}";
        got = TokenPost("application/json", js, r) && r.status == 200;
    }
    if (!got) return false;

    auto a = JsonString(r.body, "access_token");
    if (!a) return false;
    t.access = *a;
    if (auto rf = JsonString(r.body, "refresh_token")) t.refresh = *rf;
    if (auto ex = JsonNumber(r.body, "expires_in"))   t.expires = (long)*ex;
    return true;
}
static bool EnsureToken() {
    ULONGLONG now = GetTickCount64();
    if (!g_accessToken.empty() && now + 30000 < g_accessExpiry) return true;
    if (g_refresh.empty() || g_clientId.empty() || g_clientSecret.empty()) return false;

    std::vector<std::pair<std::string, std::string>> p = {
        { "grant_type", "refresh_token" }, { "refresh_token", g_refresh },
        { "client_id", g_clientId },       { "client_secret", g_clientSecret }
    };
    TokenResp t;
    if (!TokenRequest(p, t) || t.access.empty()) return false;

    g_accessToken = Widen(t.access);
    long exp = t.expires > 0 ? t.expires : 3600;
    g_accessExpiry = now + (ULONGLONG)exp * 1000ULL;
    if (!t.refresh.empty() && t.refresh != g_refresh) { g_refresh = t.refresh; SaveRefresh(g_refresh); }
    return true;
}

// ---- cloud device read -----------------------------------------------------

static bool CloudTry(const std::wstring& path, Reading& r) {
    std::wstring auth = L"Authorization: Bearer " + g_accessToken;
    HttpResult res = HttpRequest(L"GET", g_cloudHost, g_cloudPort, true, path, auth, "", true);
    if (res.status == 401) {                 // token rejected: force one refresh and retry
        g_accessToken.clear(); g_accessExpiry = 0;
        if (!EnsureToken()) return false;
        auth = L"Authorization: Bearer " + g_accessToken;
        res = HttpRequest(L"GET", g_cloudHost, g_cloudPort, true, path, auth, "", true);
    }
    if (!res.ok || res.status != 200) return false;

    auto p = JsonNumber(res.body, "power");  // reads the live "states" value; skips metadata
    if (!p) return false;
    r.power = *p;
    r.valid = true;
    r.source = SrcCloud;
    auto ci = JsonNumber(res.body, "energyCounterIn");
    auto co = JsonNumber(res.body, "energyCounterOut");
    r.inKwh  = (ci ? *ci : 0.0) / 1000.0;
    r.outKwh = (co ? *co : 0.0) / 1000.0;
    return true;
}
static bool CloudFetch(Reading& r) {
    if (g_cloudDevicePath.empty()) return false;
    if (!EnsureToken()) return false;
    if (CloudTry(g_cloudDevicePath, r)) return true;
    // Some tenants don't serve a single-device GET; fall back to the full list,
    // from which the meter's live values are picked up by key.
    if (g_cloudDevicePath != L"/device" && CloudTry(L"/device", r)) return true;
    return false;
}

static void InitCloudFromCreds() {
    LoadCreds();
    g_clientId     = CredGet("client_id");
    g_clientSecret = CredGet("client_secret");
    g_refresh      = CredGet("refresh_token");

    std::string dpath = CredGet("cloud_device_path");
    std::string dev   = CredGet("device_id");
    if (!dpath.empty())     g_cloudDevicePath = Widen(dpath);
    else if (!dev.empty())  g_cloudDevicePath = L"/device/" + Widen(dev);

    std::string ci = CredGet("cloud_interval_ms");
    if (!ci.empty()) { int v = atoi(ci.c_str()); if (v >= 5000) g_cloudIntervalMs = v; }
    std::string lh = CredGet("local_host"); if (!lh.empty()) g_localHost = Widen(lh);
    std::string ch = CredGet("cloud_host");  if (!ch.empty()) g_cloudHost = Widen(ch);

    std::string sa = CredGet("sound_alert");
    if (!sa.empty()) { std::string v = Lower(sa); g_soundAlert = (v == "1" || v == "true" || v == "yes" || v == "on"); }
    std::string fa = CredGet("flash_alert");
    if (!fa.empty()) { std::string v = Lower(fa); g_flashAlert = (v == "1" || v == "true" || v == "yes" || v == "on"); }
    std::string as = CredGet("alert_sound"); if (!as.empty()) g_alertSound = Widen(as);
    std::string sc = CredGet("scale");
    if (!sc.empty()) { double v = atof(sc.c_str()); if (v >= 0.6 && v <= 4.0) g_userScale = (float)v; }
    std::string px = CredGet("pos_x"), py = CredGet("pos_y");
    if (!px.empty() && !py.empty()) { g_posX = atoi(px.c_str()); g_posY = atoi(py.c_str()); g_havePos = true; }

    g_cloudEnabled = !g_clientId.empty() && !g_clientSecret.empty() &&
                     !g_refresh.empty()  && !g_cloudDevicePath.empty();
}

// ---- drawing helpers -------------------------------------------------------

static void AddRoundRect(GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}
static void FillGlow(Graphics& g, GraphicsPath& path, Color fill, Color glow, float glowW) {
    Pen pen(glow, glowW);
    pen.SetLineJoin(LineJoinRound);
    g.DrawPath(&pen, &path);
    SolidBrush br(fill);
    g.FillPath(&br, &path);
}
static void DrawDigit(Graphics& g, float x, float y, float w, float h,
                      int mask, Color on, Color off, Color glow) {
    const float t   = w * 0.16f;
    const float p   = t;
    const float gap = t * 0.30f;
    const float left = x + p, right = x + w - p;
    const float top = y + p, mid = y + h / 2, bot = y + h - p;

    auto horiz = [&](float x0, float x1, float cy, bool lit) {
        GraphicsPath path;
        float a0 = x0 + gap, a1 = x1 - gap;
        PointF pts[6] = {
            PointF(a0, cy), PointF(a0 + t / 2, cy - t / 2), PointF(a1 - t / 2, cy - t / 2),
            PointF(a1, cy), PointF(a1 - t / 2, cy + t / 2), PointF(a0 + t / 2, cy + t / 2)
        };
        path.AddPolygon(pts, 6);
        if (lit) FillGlow(g, path, on, glow, t * 1.4f);
        else { SolidBrush b(off); g.FillPath(&b, &path); }
    };
    auto vert = [&](float y0, float y1, float cx, bool lit) {
        GraphicsPath path;
        float b0 = y0 + gap, b1 = y1 - gap;
        PointF pts[6] = {
            PointF(cx, b0), PointF(cx + t / 2, b0 + t / 2), PointF(cx + t / 2, b1 - t / 2),
            PointF(cx, b1), PointF(cx - t / 2, b1 - t / 2), PointF(cx - t / 2, b0 + t / 2)
        };
        path.AddPolygon(pts, 6);
        if (lit) FillGlow(g, path, on, glow, t * 1.4f);
        else { SolidBrush b(off); g.FillPath(&b, &path); }
    };

    horiz(left, right, top, (mask & SEG_A) != 0);
    horiz(left, right, mid, (mask & SEG_G) != 0);
    horiz(left, right, bot, (mask & SEG_D) != 0);
    vert(top, mid, left,  (mask & SEG_F) != 0);
    vert(top, mid, right, (mask & SEG_B) != 0);
    vert(mid, bot, left,  (mask & SEG_E) != 0);
    vert(mid, bot, right, (mask & SEG_C) != 0);
}

static Layout MakeLayout(float s) {
    Layout L;
    L.scale = s;
    L.digits = DIGIT_COUNT;
    L.showCounters = SHOW_COUNTERS;

    const float cellW = 36, cellH = 66, cellGap = 7;
    const float padL = 16, padT = 16, padR = 14, padB = 10;
    const float arrowW = 16, gapAD = 8, gapDU = 8, unitW = 40, unitSize = 24;
    const float gapDB = 8, bottomH = 18, bottomFont = 13;

    const float digitsBlockW = L.digits * cellW + (L.digits - 1) * cellGap;
    const float contentW = arrowW + gapAD + digitsBlockW + gapDU + unitW;
    const float w = padL + contentW + padR;
    const float h = padT + cellH + (L.showCounters ? gapDB + bottomH : 0) + padB;

    L.w = (int)(w * s + 0.5f);
    L.h = (int)(h * s + 0.5f);
    L.cellW = cellW * s; L.cellH = cellH * s; L.cellGap = cellGap * s;
    L.arrowX = padL * s; L.arrowW = arrowW * s;
    L.digitsX = (padL + arrowW + gapAD) * s; L.digitsY = padT * s;
    L.unitX = (padL + arrowW + gapAD + digitsBlockW + gapDU) * s;
    L.unitSize = unitSize * s;
    L.bottomY = (padT + cellH + gapDB) * s;
    L.bottomH = bottomH * s;
    L.bottomFont = bottomFont * s;
    return L;
}

static RECT SpeakerRect() {
    Layout L = MakeLayout(g_scale);
    int grip = (int)(8 * g_dpiScale); if (grip < 6) grip = 6;
    int sz = (int)(20 * g_scale);     if (sz < 14) sz = 14;
    int inset = (int)(9 * g_scale);   if (inset < grip + 2) inset = grip + 2;
    int x0 = L.w - sz - inset;                        // top-right corner (clear of the flow arrow)
    // vertically centered between the top of the window and the top-of-arrow line
    float arrowTop = L.digitsY + L.cellH * 0.26f;
    int y0 = (int)(arrowTop * 0.5f) - sz / 2; if (y0 < 2) y0 = 2;
    return RECT{ x0, y0, x0 + sz, y0 + sz };
}

static void DrawSpeaker(Graphics& g, const RECT& r, bool on) {
    float x = (float)r.left, y = (float)r.top;
    float W = (float)(r.right - r.left), H = (float)(r.bottom - r.top);
    auto P = [&](float nx, float ny){ return PointF(x + nx * W, y + ny * H); };

    Color bodyCol = on ? Color(235, 200, 206, 216) : Color(165, 150, 154, 162);
    GraphicsPath body;
    PointF pts[6] = { P(0.10f,0.40f), P(0.22f,0.40f), P(0.40f,0.22f),
                      P(0.40f,0.78f), P(0.22f,0.60f), P(0.10f,0.60f) };
    body.AddPolygon(pts, 6);
    SolidBrush bb(bodyCol);
    g.FillPath(&bb, &body);

    if (on) {
        float cx = x + 0.42f * W, cy = y + 0.50f * H;
        float wpw = 0.05f * W; if (wpw < 1.0f) wpw = 1.0f;
        Pen wp(Color(210, 190, 196, 206), wpw);
        wp.SetStartCap(LineCapRound); wp.SetEndCap(LineCapRound);
        float r1 = 0.16f * W, r2 = 0.30f * W;
        g.DrawArc(&wp, cx - r1, cy - r1, 2 * r1, 2 * r1, -55.0f, 110.0f);
        g.DrawArc(&wp, cx - r2, cy - r2, 2 * r2, 2 * r2, -55.0f, 110.0f);
    } else {
        float spw = 0.09f * W; if (spw < 1.5f) spw = 1.5f;
        Pen sp(Color(255, 255, 70, 55), spw);
        sp.SetStartCap(LineCapRound); sp.SetEndCap(LineCapRound);
        g.DrawLine(&sp, P(0.48f, 0.24f), P(0.02f, 0.76f));
    }
}

static void DrawWidget(Graphics& g, const Layout& L, const Reading& r) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);

    {
        GraphicsPath panel;
        AddRoundRect(panel, 0.75f, 0.75f, L.w - 1.5f, L.h - 1.5f, 14.0f * L.scale);
        SolidBrush bg(Color(235, 15, 17, 21));
        g.FillPath(&bg, &panel);
        Pen border(Color(150, 70, 76, 90), 1.2f * L.scale);
        g.DrawPath(&border, &panel);
    }

    const double eps = 1.0;
    bool feedin = false, import_ = false;
    if (r.valid) {
        double p = r.power;
        if (NEGATIVE_IS_FEEDIN) { feedin = p < -eps; import_ = p > eps; }
        else                    { feedin = p >  eps; import_ = p < -eps; }
    }

    Color on, off, glow;
    if (!r.valid)      { on = Color(255, 255, 176, 40); off = Color(255, 46, 34, 10); glow = Color(70, 255, 176, 40); }
    else if (feedin)   { on = Color(255, 50, 235, 110); off = Color(255, 12, 42, 22); glow = Color(90, 50, 235, 110); }
    else if (import_)  { on = Color(255, 255, 54, 34);  off = Color(255, 48, 14, 10); glow = Color(90, 255, 54, 34); }
    else               { on = Color(255, 255, 176, 40); off = Color(255, 46, 34, 10); glow = Color(70, 255, 176, 40); }

    if (r.valid && (feedin || import_)) {
        float cx = L.arrowX + L.arrowW / 2;
        float cy = L.digitsY + L.cellH / 2;
        float aw = L.arrowW * 0.55f, ah = L.cellH * 0.24f;
        GraphicsPath tri;
        PointF pts[3];
        if (import_) { pts[0] = PointF(cx - aw, cy - ah); pts[1] = PointF(cx + aw, cy - ah); pts[2] = PointF(cx, cy + ah); }
        else         { pts[0] = PointF(cx - aw, cy + ah); pts[1] = PointF(cx + aw, cy + ah); pts[2] = PointF(cx, cy - ah); }
        tri.AddPolygon(pts, 3);
        FillGlow(g, tri, on, glow, 6.0f * L.scale);
    }

    int masks[8] = { 0 };
    if (!r.valid) {
        for (int i = 0; i < L.digits; ++i) masks[i] = SEG_G;
    } else {
        double mag = std::fabs(r.power);
        if (mag > 99999) mag = 99999;
        wchar_t nb[16];
        swprintf_s(nb, L"%.0f", mag);
        int len = (int)wcslen(nb);
        for (int i = 0; i < len && i < L.digits; ++i)
            masks[L.digits - 1 - i] = MaskForChar(nb[len - 1 - i]);
    }
    for (int i = 0; i < L.digits; ++i) {
        float x = L.digitsX + i * (L.cellW + L.cellGap);
        DrawDigit(g, x, L.digitsY, L.cellW, L.cellH, masks[i], on, off, glow);
    }

    {
        FontFamily ff(L"Consolas");
        Font font(&ff, L.unitSize, FontStyleBold, UnitPixel);
        SolidBrush br(on);
        StringFormat sf; sf.SetAlignment(StringAlignmentNear); sf.SetLineAlignment(StringAlignmentCenter);
        RectF rc(L.unitX, L.digitsY, 44.0f * L.scale, L.cellH);
        g.DrawString(L"W", -1, &font, rc, &sf, &br);
    }

    if (L.showCounters) {
        FontFamily ff(L"Consolas");
        Font font(&ff, L.bottomFont, FontStyleRegular, UnitPixel);
        SolidBrush br(Color(210, 150, 156, 168));
        StringFormat sf; sf.SetAlignment(StringAlignmentNear); sf.SetLineAlignment(StringAlignmentCenter);
        wchar_t cb[96];
        if (r.valid) swprintf_s(cb, L"IN %.1f   OUT %.1f kWh", r.inKwh, r.outKwh);
        else         swprintf_s(cb, L"IN --   OUT --");
        float x = L.digitsX;
        RectF rc(x, L.bottomY, (float)L.w - x - 12.0f * L.scale, L.bottomH);
        g.DrawString(cb, -1, &font, rc, &sf, &br);
    }

    // cloud-fallback indicator
    if (r.source == SrcCloud) {
        float rad = 3.5f * L.scale;
        float cx = 11.0f * L.scale, cy = 11.0f * L.scale;
        GraphicsPath dot;
        dot.AddEllipse(cx - rad, cy - rad, rad * 2, rad * 2);
        FillGlow(g, dot, Color(255, 80, 200, 255), Color(90, 80, 200, 255), 4.0f * L.scale);
    }

    // mute toggle (small speaker, top-left)
    DrawSpeaker(g, SpeakerRect(), g_soundAlert.load());
}

// ---- render into the layered window ---------------------------------------

static void RenderNow() {
    if (!g_hwnd) return;
    Reading r;
    { std::lock_guard<std::mutex> lk(g_mtx); r = g_reading; }

    Layout L = MakeLayout(g_scale);
    if (L.w <= 0 || L.h <= 0) return;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = L.w;
    bmi.bmiHeader.biHeight = -L.h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(mem, dib);

    {
        Bitmap bmp(L.w, L.h, L.w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.Clear(Color(0, 0, 0, 0));
        DrawWidget(g, L, r);
        g.Flush();
    }

    SIZE size{ L.w, L.h };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hwnd, screen, nullptr, &size, mem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mem, oldBmp);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// ---- sound alert -----------------------------------------------------------

enum FlowDir { FLOW_NONE = 0, FLOW_IMPORT = 1, FLOW_FEEDIN = -1, FLOW_NEUTRAL = 2 };

static int DirectionOf(const Reading& r) {
    if (!r.valid) return FLOW_NONE;
    double p = r.power, eps = 1.0;
    bool feedin, import_;
    if (NEGATIVE_IS_FEEDIN) { feedin = p < -eps; import_ = p > eps; }
    else                    { feedin = p >  eps; import_ = p < -eps; }
    if (import_) return FLOW_IMPORT;
    if (feedin)  return FLOW_FEEDIN;
    return FLOW_NEUTRAL;
}
static void PlayAlert() {
    if (!g_soundAlert.load()) return;
    if (!g_alertSound.empty() &&
        PlaySoundW(g_alertSound.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
        return;   // custom WAV played; otherwise fall back so the alert is never silent
    PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
}

// ---- polling thread --------------------------------------------------------

static void WorkerThread() {
    ULONGLONG lastCloud = 0, lastGood = 0;
    const ULONGLONG staleMs = (ULONGLONG)g_cloudIntervalMs * 3 + 10000;
    int lastNonNeutral = FLOW_NONE;

    while (g_running.load()) {
        Reading r;
        HttpResult lr = HttpRequest(L"GET", g_localHost, 80, false, std::wstring(g_localPath), L"", "", false);
        if (lr.ok && lr.status == 200) {
            if (auto p = JsonNumber(lr.body, "power")) {
                r.power = *p; r.valid = true; r.source = SrcLocal;
                auto ci = JsonNumber(lr.body, "energyCounterIn");
                auto co = JsonNumber(lr.body, "energyCounterOut");
                r.inKwh  = (ci ? *ci : 0.0) / 1000.0;
                r.outKwh = (co ? *co : 0.0) / 1000.0;
            }
        }

        ULONGLONG now = GetTickCount64();
        if (r.valid) {                                   // local is up
            lastGood = now;
            { std::lock_guard<std::mutex> lk(g_mtx); g_reading = r; }
        } else if (g_cloudEnabled) {                     // local down -> cloud, throttled
            if (now - lastCloud >= (ULONGLONG)g_cloudIntervalMs) {
                lastCloud = now;
                Reading cr;
                if (CloudFetch(cr)) {
                    lastGood = now;
                    { std::lock_guard<std::mutex> lk(g_mtx); g_reading = cr; }
                } else if (now - lastGood > staleMs) {
                    Reading none;
                    { std::lock_guard<std::mutex> lk(g_mtx); g_reading = none; }
                }
            } else if (now - lastGood > staleMs) {       // keep last cloud value until stale
                Reading none;
                { std::lock_guard<std::mutex> lk(g_mtx); g_reading = none; }
            }
        } else {                                         // no cloud configured
            Reading none;
            { std::lock_guard<std::mutex> lk(g_mtx); g_reading = none; }
        }

        {
            Reading cur; { std::lock_guard<std::mutex> lk(g_mtx); cur = g_reading; }
            int d = DirectionOf(cur);
            if (d == FLOW_IMPORT && lastNonNeutral == FLOW_FEEDIN) {
                PlayAlert();
                if (g_flashAlert.load() && g_hwnd) PostMessageW(g_hwnd, WM_APP_FLASH, 0, 0);
            }
            if (d == FLOW_IMPORT || d == FLOW_FEEDIN) lastNonNeutral = d;
        }

        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DATA, 0, 0);

        int slept = 0;
        while (slept < g_localIntervalMs && g_running.load()) { Sleep(50); slept += 50; }
    }
}

// ---- one-time cloud enrollment (--login) ----------------------------------

static bool RecvRequestLine(SOCKET c, std::string& out) {
    char buf[4096];
    int n = recv(c, buf, (int)sizeof(buf) - 1, 0);
    if (n <= 0) return false;
    out.assign(buf, buf + n);
    return true;
}
static std::string QueryParam(const std::string& req, const std::string& name) {
    size_t sp = req.find(' ');
    if (sp == std::string::npos) return "";
    size_t sp2 = req.find(' ', sp + 1);
    std::string target = req.substr(sp + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp - 1);
    size_t q = target.find('?');
    if (q == std::string::npos) return "";
    std::string qs = target.substr(q + 1);
    std::string key = name + "=";
    size_t p = qs.find(key);
    if (p == std::string::npos) return "";
    p += key.size();
    size_t e = qs.find('&', p);
    return qs.substr(p, e == std::string::npos ? std::string::npos : e - p);
}
static int RunLogin(int port) {
    LoadCreds();
    g_clientId     = CredGet("client_id");
    g_clientSecret = CredGet("client_secret");
    { std::string rp = CredGet("redirect_port");
      if (port == DEFAULT_REDIRECT_PORT && !rp.empty()) { int v = atoi(rp.c_str()); if (v > 0) port = v; } }

    if (g_clientId.empty() || g_clientSecret.empty()) {
        MessageBoxW(nullptr, L"Put client_id and client_secret into powerchk.credentials (next to the EXE) first.",
                    L"powerchk enrollment", MB_ICONWARNING);
        return 1;
    }

    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) {
        MessageBoxW(nullptr, L"WSAStartup failed.", L"powerchk enrollment", MB_ICONERROR);
        return 1;
    }
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    BOOL yes = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ls == INVALID_SOCKET || bind(ls, (sockaddr*)&a, sizeof(a)) != 0 || listen(ls, 1) != 0) {
        MessageBoxW(nullptr, L"Could not bind the loopback port. Change redirect_port and try again.",
                    L"powerchk enrollment", MB_ICONERROR);
        if (ls != INVALID_SOCKET) closesocket(ls);
        WSACleanup();
        return 1;
    }

    std::string redirect = "http://localhost:" + std::to_string(port) + "/callback";
    std::string url = "https://" + Narrow(g_cloudHost) + "/oauth2/authorize?response_type=code"
                      "&client_id=" + UrlEncode(g_clientId) +
                      "&redirect_uri=" + UrlEncode(redirect) +
                      "&state=powerchk";
    ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    fd_set fds; FD_ZERO(&fds); FD_SET(ls, &fds);
    timeval tv{ 180, 0 };
    std::string code;
    if (select(0, &fds, nullptr, nullptr, &tv) > 0) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c != INVALID_SOCKET) {
            std::string req;
            if (RecvRequestLine(c, req)) code = UrlDecode(QueryParam(req, "code"));
            std::string resp =
                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
                "<html><body style='font-family:sans-serif;background:#111;color:#eee'>"
                "<h2>powerchk</h2><p>Enrollment received. You can close this tab.</p></body></html>";
            send(c, resp.c_str(), (int)resp.size(), 0);
            closesocket(c);
        }
    }
    closesocket(ls);

    if (code.empty()) {
        MessageBoxW(nullptr, L"No authorization code received (timed out or access denied).",
                    L"powerchk enrollment", MB_ICONERROR);
        WSACleanup();
        return 1;
    }

    std::vector<std::pair<std::string, std::string>> p = {
        { "grant_type", "authorization_code" }, { "code", code },
        { "redirect_uri", redirect },
        { "client_id", g_clientId }, { "client_secret", g_clientSecret }
    };
    TokenResp t;
    if (!TokenRequest(p, t) || t.refresh.empty()) {
        MessageBoxW(nullptr, L"Token exchange failed. Check the client credentials and the app's redirect URL.",
                    L"powerchk enrollment", MB_ICONERROR);
        WSACleanup();
        return 1;
    }
    SaveRefresh(t.refresh);

    std::wstring msg = L"Enrollment complete. refresh_token saved to powerchk.credentials.";
    if (!t.access.empty()) {
        g_accessToken = Widen(t.access);
        HttpResult dev = HttpRequest(L"GET", g_cloudHost, g_cloudPort, true, L"/device",
                                     L"Authorization: Bearer " + g_accessToken, "", true);
        if (dev.ok && dev.status == 200) {
            WriteFileBytes(ExeDir() + L"\\everhome_devices.json", dev.body);
            msg += L"\n\nYour devices were written to everhome_devices.json — "
                   L"copy your EcoTracker id into device_id in powerchk.credentials.";
        }
    }
    MessageBoxW(nullptr, msg.c_str(), L"powerchk enrollment", MB_ICONINFORMATION);
    WSACleanup();
    return 0;
}

// ---- window ----------------------------------------------------------------

static void ParseLocalEndpoint(const std::wstring& s) {
    if (s.rfind(L"http", 0) == 0) {          // full URL: take the host only (path is fixed)
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t hostBuf[256]{};
        uc.lpszHostName = hostBuf; uc.dwHostNameLength = 256;
        if (WinHttpCrackUrl(s.c_str(), (DWORD)s.size(), 0, &uc))
            g_localHost.assign(uc.lpszHostName, uc.dwHostNameLength);
    } else {
        g_localHost = s;
    }
}

static void ApplyUserScale(HWND hwnd, float userScale) {
    g_userScale = userScale;
    g_scale = g_dpiScale * g_userScale;
    Layout L = MakeLayout(g_scale);
    RECT wr; GetWindowRect(hwnd, &wr);
    SetWindowPos(hwnd, nullptr, wr.right - L.w, wr.top, L.w, L.h, SWP_NOZORDER | SWP_NOACTIVATE);
    RenderNow();
    char b[32]; snprintf(b, sizeof(b), "%.3f", (double)userScale);
    SetCredLine("scale", b);
}
// Double-click toggles between the current custom size and reset (1x),
// remembering the custom size so the next double-click restores it.
static void ToggleResetSize(HWND hwnd) {
    if (std::fabs(g_userScale - 1.0f) > 0.01f) {
        g_stashedScale = g_userScale;
        ApplyUserScale(hwnd, 1.0f);
    } else if (g_stashedScale > 0.01f) {
        ApplyUserScale(hwnd, g_stashedScale);
    }
}

// Keep a window rect fully inside the work area of whichever monitor it is
// nearest to -- so a position saved on a now-missing/resized display can't
// leave the widget stranded off-screen.
static void ClampToWorkArea(int& x, int& y, int w, int h) {
    RECT target = { x, y, x + w, y + h };
    HMONITOR hMon = MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return;
    const RECT& wk = mi.rcWork;
    if (x + w > wk.right)  x = wk.right - w;
    if (y + h > wk.bottom) y = wk.bottom - h;
    if (x < wk.left) x = wk.left;
    if (y < wk.top)  y = wk.top;
}
static void SaveWindowPlacement(HWND hwnd) {
    RECT wr; GetWindowRect(hwnd, &wr);
    char b[32];
    snprintf(b, sizeof(b), "%.3f", (double)g_userScale); SetCredLine("scale", b);
    snprintf(b, sizeof(b), "%d", (int)wr.left);          SetCredLine("pos_x", b);
    snprintf(b, sizeof(b), "%d", (int)wr.top);           SetCredLine("pos_y", b);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: {
        RECT wr; GetWindowRect(hwnd, &wr);
        int gx = (int)(short)LOWORD(lp), gy = (int)(short)HIWORD(lp);
        int x = gx - wr.left, y = gy - wr.top;
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        int grip = (int)(8 * g_dpiScale); if (grip < 6) grip = 6;
        bool L_ = x < grip, R_ = x >= w - grip, T_ = y < grip, B_ = y >= h - grip;
        if (T_ && L_) return HTTOPLEFT;
        if (T_ && R_) return HTTOPRIGHT;
        if (B_ && L_) return HTBOTTOMLEFT;
        if (B_ && R_) return HTBOTTOMRIGHT;
        if (L_) return HTLEFT;
        if (R_) return HTRIGHT;
        if (T_) return HTTOP;
        if (B_) return HTBOTTOM;
        RECT spk = SpeakerRect();
        if (x >= spk.left && x < spk.right && y >= spk.top && y < spk.bottom) return HTCLIENT;
        return HTCAPTION;
    }

    case WM_LBUTTONUP: {
        POINT pt; GetCursorPos(&pt);
        RECT wr; GetWindowRect(hwnd, &wr);
        int x = pt.x - wr.left, y = pt.y - wr.top;
        RECT spk = SpeakerRect();
        if (x >= spk.left && x < spk.right && y >= spk.top && y < spk.bottom) {
            bool nv = !g_soundAlert.load();
            g_soundAlert = nv;
            SetCredLine("sound_alert", nv ? "1" : "0");
            RenderNow();
        }
        return 0;
    }

    case WM_NCLBUTTONDBLCLK:
        if (wp == HTCAPTION) { ToggleResetSize(hwnd); return 0; }
        break;

    case WM_APP_DATA:
        RenderNow();
        return 0;

    case WM_APP_FLASH:
        g_flashRemaining = 6;                       // 3 hide/show cycles
        SetTimer(hwnd, FLASH_TIMER_ID, 110, nullptr);
        return 0;

    case WM_TIMER:
        if (wp == FLASH_TIMER_ID) {
            if (g_flashRemaining > 0) {
                ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOWNOACTIVATE);
                if (--g_flashRemaining == 0) {
                    KillTimer(hwnd, FLASH_TIMER_ID);
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);   // guarantee visible at the end
                    RenderNow();
                }
            }
            return 0;
        }
        break;

    case WM_NCRBUTTONUP: {
        POINT pt; GetCursorPos(&pt);
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING | (g_flashAlert.load() ? MF_CHECKED : MF_UNCHECKED), 4,
                    L"Flash on green\u2192red");
        AppendMenuW(m, MF_STRING, 3, L"Reset size");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 1, L"Exit");
        SetForegroundWindow(hwnd);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (cmd == 1) DestroyWindow(hwnd);
        else if (cmd == 4) { bool nv = !g_flashAlert.load(); g_flashAlert = nv; SetCredLine("flash_alert", nv ? "1" : "0"); }
        else if (cmd == 3) {
            if (std::fabs(g_userScale - 1.0f) > 0.01f) g_stashedScale = g_userScale;
            ApplyUserScale(hwnd, 1.0f);
        }
        return 0;
    }

    case WM_DPICHANGED: {
        g_dpiScale = (float)HIWORD(wp) / 96.0f;
        g_scale = g_dpiScale * g_userScale;
        RECT* prc = (RECT*)lp;
        Layout L = MakeLayout(g_scale);
        SetWindowPos(hwnd, nullptr, prc->left, prc->top, L.w, L.h, SWP_NOZORDER | SWP_NOACTIVATE);
        RenderNow();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        Layout lo = MakeLayout(g_dpiScale * 0.6f), hi = MakeLayout(g_dpiScale * 4.0f);
        mmi->ptMinTrackSize.x = lo.w; mmi->ptMinTrackSize.y = lo.h;
        mmi->ptMaxTrackSize.x = hi.w; mmi->ptMaxTrackSize.y = hi.h;
        return 0;
    }

    case WM_SIZING: {
        RECT* r = (RECT*)lp;
        Layout base = MakeLayout(1.0f);
        int pw = r->right - r->left, ph = r->bottom - r->top;
        float total = (wp == WMSZ_TOP || wp == WMSZ_BOTTOM)
                        ? (float)ph / base.h : (float)pw / base.w;
        float user = total / g_dpiScale;
        if (user < 0.6f) user = 0.6f;
        if (user > 4.0f) user = 4.0f;
        total = g_dpiScale * user;
        Layout L = MakeLayout(total);
        switch (wp) {
            case WMSZ_LEFT:        r->left = r->right - L.w; r->bottom = r->top + L.h; break;
            case WMSZ_RIGHT:       r->right = r->left + L.w; r->bottom = r->top + L.h; break;
            case WMSZ_TOP:         r->top = r->bottom - L.h; r->right = r->left + L.w; break;
            case WMSZ_BOTTOM:      r->bottom = r->top + L.h; r->right = r->left + L.w; break;
            case WMSZ_TOPLEFT:     r->left = r->right - L.w; r->top = r->bottom - L.h; break;
            case WMSZ_TOPRIGHT:    r->right = r->left + L.w; r->top = r->bottom - L.h; break;
            case WMSZ_BOTTOMLEFT:  r->left = r->right - L.w; r->bottom = r->top + L.h; break;
            case WMSZ_BOTTOMRIGHT: r->right = r->left + L.w; r->bottom = r->top + L.h; break;
        }
        g_userScale = user; g_scale = total;
        return TRUE;
    }

    case WM_SIZE:
        RenderNow();
        return 0;

    case WM_EXITSIZEMOVE:
        SaveWindowPlacement(hwnd);
        return 0;

    case WM_DESTROY:
        SaveWindowPlacement(hwnd);
        g_running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    bool login = false;
    int redirectPort = DEFAULT_REDIRECT_PORT;
    std::wstring localArg;
    int intervalArg = 0;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; ++i) {
                std::wstring a = argv[i];
                if (a == L"--login") {
                    login = true;
                    if (i + 1 < argc) { int pv = _wtoi(argv[i + 1]); if (pv > 0) { redirectPort = pv; ++i; } }
                } else if (localArg.empty())      localArg = a;
                else if (intervalArg == 0)        intervalArg = _wtoi(a.c_str());
            }
            LocalFree(argv);
        }
    }

    if (login) return RunLogin(redirectPort);

    InitCloudFromCreds();                       // creds may also set local_host / cloud_*
    if (!localArg.empty()) ParseLocalEndpoint(localArg);   // command line overrides
    if (intervalArg >= 100) g_localIntervalMs = intervalArg;

    ULONG_PTR gpToken;
    GdiplusStartupInput gsi;
    GdiplusStartup(&gpToken, &gsi, nullptr);

    HICON hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_POWERCHK), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    HICON hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_POWERCHK), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PowerChkWidget";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = hIcon;
    wc.hIconSm = hIconSm;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"Power", WS_POPUP | WS_THICKFRAME,
        0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);

    UINT dpi = GetDpiForWindow(g_hwnd);
    g_dpiScale = dpi / 96.0f;
    g_scale = g_dpiScale * g_userScale;
    Layout L = MakeLayout(g_scale);

    int x, y;
    if (g_havePos) {
        x = g_posX; y = g_posY;
        ClampToWorkArea(x, y, L.w, L.h);
    } else {
        RECT wa{};
        SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
        int margin = (int)(24 * g_scale);
        x = wa.right - L.w - margin;
        y = wa.top + margin;
    }
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, L.w, L.h, SWP_NOACTIVATE);

    // If the restored spot is on a monitor with a different DPI, correct the
    // size for that monitor and re-clamp.
    UINT dpi2 = GetDpiForWindow(g_hwnd);
    if (dpi2 != dpi) {
        g_dpiScale = dpi2 / 96.0f;
        g_scale = g_dpiScale * g_userScale;
        L = MakeLayout(g_scale);
        ClampToWorkArea(x, y, L.w, L.h);
        SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, L.w, L.h, SWP_NOACTIVATE);
    }

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    RenderNow();

    std::thread worker(WorkerThread);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running = false;
    if (worker.joinable()) worker.join();
    GdiplusShutdown(gpToken);
    return 0;
}
