// Лабораторная работа №4 «Система видеонаблюдения на базе веб-камеры»
// Вариант А6. Скрытый режим видеонаблюдения (Windows, Media Foundation, Win32 API, C++)
//
// Приложение — личный видеорегистратор для собственной веб-камеры пользователя:
//   - главное окно с предпросмотром, сведениями об устройстве, снимком и записью;
//   - кнопка «Свернуть в трей» (окно скрывается, захват продолжается);
//   - две глобальные горячие клавиши: Ctrl+Shift+R — старт записи, Ctrl+Shift+S — стоп;
//   - при старте в фоне запись идёт без отображения окна; значок в трее мигает во время записи;
//   - автоматическое разбиение записи на файлы по заданному интервалу (по умолчанию 5 минут);
//   - полный журнал всех операций с временными метками и экспорт отчёта.
//
// Прозрачность: при каждом сохранении файла показывается всплывающее уведомление, все действия
// пишутся в журнал, при записи значок мигает. Используются только системные интерфейсы:
// Media Foundation (MFEnumDeviceSources, IMFSourceReader, IMFSinkWriter), SetupAPI, WIC, GDI,
// Common Controls, RegisterDeviceNotification, RegisterHotKey, Shell_NotifyIcon.
// OpenCV, FFmpeg, GStreamer, DirectShow.NET и другие сторонние библиотеки не применяются.
//
// Запуск:  CamMonitor.exe                 — графический интерфейс
//          CamMonitor.exe /demo <каталог> — демонстрационный сценарий для проверки и отчёта

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wincodec.h>
#include <setupapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dbt.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

// Классы интерфейсов устройств видеозахвата (ksmedia.h), объявлены вручную, чтобы не подключать initguid.h.
// KSCATEGORY_VIDEO_CAMERA {E5323777-F976-4F5B-9B55-B94699C46E44} — веб-камеры (Windows 10+).
// На него окно подписывается через RegisterDeviceNotification, чтобы узнавать о подключении/отключении.
static const GUID kGuidVideoCamera = {0xe5323777, 0xf976, 0x4f5b, {0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44}};

const int kDefaultSplitMin = 5;  // интервал авторазбиения записи, минуты

// Состояние камеры: определяет цвет индикатора и надпись в окне предпросмотра.
enum CamState { CS_STOPPED = 0, CS_STARTING, CS_RUNNING, CS_ABSENT, CS_LOST, CS_ERROR };

// ===========================================================================
// Вспомогательные функции
// ===========================================================================
template <class T> static void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Форматирование строки в стиле printf для широких строк.
static std::wstring Fmt(const wchar_t* f, ...) {
    wchar_t b[2048];
    va_list a;
    va_start(a, f);
    _vsnwprintf_s(b, _countof(b), _TRUNCATE, f, a);
    va_end(a);
    return b;
}

static std::wstring NowStr(bool withDate) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (withDate)
        return Fmt(L"%02d.%02d.%04d %02d:%02d:%02d", t.wDay, t.wMonth, t.wYear, t.wHour, t.wMinute, t.wSecond);
    return Fmt(L"%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
}

// Метка времени для имени файла: YYYYMMDD_HHMMSS.
static std::wstring FileStamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return Fmt(L"%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Запись текста в файл в UTF-8 с меткой BOM.
static bool WriteUtf8File(const std::wstring& path, const std::wstring& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string u = "\xEF\xBB\xBF" + ToUtf8(text);
    DWORD wr = 0;
    BOOL ok = WriteFile(f, u.data(), (DWORD)u.size(), &wr, nullptr);
    CloseHandle(f);
    return ok && wr == u.size();
}

static bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static ULONGLONG FileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
    return ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

// Уникальное имя файла: если файл с таким именем уже есть, добавляется _2, _3...
static std::wstring UniquePath(const std::wstring& dir, const std::wstring& base, const wchar_t* ext) {
    std::wstring p = dir + L"\\" + base + ext;
    for (int i = 2; FileExists(p); ++i) p = dir + L"\\" + base + Fmt(L"_%d", i) + ext;
    return p;
}

// Текстовое описание HRESULT; для типичных ошибок камеры — пояснение по-русски.
static std::wstring HrText(HRESULT hr) {
    std::wstring hint;
    if (hr == E_ACCESSDENIED)
        hint = L"доступ к камере запрещён (Параметры → Конфиденциальность → Камера → «Разрешить классическим приложениям»)";
#ifdef MF_E_HW_MFT_FAILED_START_STREAMING
    else if (hr == MF_E_HW_MFT_FAILED_START_STREAMING)
        hint = L"камера не запустилась (возможно, занята другим приложением)";
#endif
#ifdef MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED
    else if (hr == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED)
        hint = L"устройство отключено";
#endif
    else if (hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_TOPO_CODEC_NOT_FOUND)
        hint = L"формат не поддерживается";
    if (hint.empty()) {
        wchar_t* msg = nullptr;
        if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, (DWORD)hr, 0, (LPWSTR)&msg, 0, nullptr) && msg) {
            hint = msg;
            LocalFree(msg);
            while (!hint.empty() && (hint.back() == L'\n' || hint.back() == L'\r' || hint.back() == L'.')) hint.pop_back();
        }
    }
    return Fmt(L"0x%08X", (unsigned)hr) + (hint.empty() ? L"" : L" — " + hint);
}

// Название формата пикселей по GUID подтипа Media Foundation. У видеоформатов MF
// первое поле GUID (Data1) — код FourCC ('MJPG', 'YUY2', 'NV12'...), кроме RGB.
static std::wstring SubtypeName(const GUID& g) {
    if (g == MFVideoFormat_RGB32) return L"RGB32";
    if (g == MFVideoFormat_RGB24) return L"RGB24";
    if (g == MFVideoFormat_ARGB32) return L"ARGB32";
    std::wstring s;
    for (int i = 0; i < 4; ++i) {
        wchar_t ch = (wchar_t)((g.Data1 >> (8 * i)) & 0xFF);
        if (ch >= 32 && ch < 127) s += ch;
    }
    return s.empty() ? Fmt(L"0x%08X", (unsigned)g.Data1) : s;
}
// ===========================================================================
// Модель данных
// ===========================================================================
// Режим работы камеры (один «тип медиа» устройства): формат пикселей, разрешение, частота кадров.
// native — индекс типа в IMFSourceReader::GetNativeMediaType.
struct MediaFormat {
    GUID sub = GUID_NULL;
    UINT32 w = 0, h = 0, num = 0, den = 1;
    DWORD native = 0;
    double Fps() const { return den ? (double)num / den : 0; }
    bool SameRes(const MediaFormat& o) const { return sub == o.sub && w == o.w && h == o.h; }
    bool Same(const MediaFormat& o) const { return SameRes(o) && (UINT64)num * o.den == (UINT64)o.num * den; }
    std::wstring ResText() const { return Fmt(L"%s %u×%u", SubtypeName(sub).c_str(), w, h); }
    std::wstring FpsText() const {
        double f = Fps();
        return f == (int)f ? Fmt(L"%d к/с", (int)f) : Fmt(L"%.2f к/с", f);
    }
    std::wstring Text() const { return ResText() + L", " + FpsText(); }
};

// Наложение даты и времени на кадр (для снимков и видео). Текст рисуется средствами GDI
// в DIB-секции; копируется только полоса кадра, на которой расположена надпись.
struct Stamper {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr, oldBmp = nullptr;
    HFONT font = nullptr;
    HGDIOBJ oldFont = nullptr;
    BYTE* bits = nullptr;
    int w = 0, h = 0;

    void Reset() {
        if (dc) {
            SelectObject(dc, oldFont);
            SelectObject(dc, oldBmp);
            DeleteDC(dc);
        }
        if (bmp) DeleteObject(bmp);
        if (font) DeleteObject(font);
        dc = nullptr;
        bmp = nullptr;
        font = nullptr;
        bits = nullptr;
        w = h = 0;
    }
    ~Stamper() { Reset(); }

    void Apply(BYTE* px, int W, int H, const std::wstring& text) {
        if (W != w || H != h) {
            Reset();
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = W;
            bi.bmiHeader.biHeight = -H;  // строки сверху вниз, как в буфере кадра
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            dc = CreateCompatibleDC(nullptr);
            bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
            if (!dc || !bmp || !bits) {
                Reset();
                return;
            }
            oldBmp = (HBITMAP)SelectObject(dc, bmp);
            font = CreateFontW(-std::max(12, H / 28), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY,
                               FIXED_PITCH, L"Consolas");
            oldFont = SelectObject(dc, font);
            w = W;
            h = H;
        }
        SIZE sz = {};
        GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
        int pad = 4, y0 = std::max(0, H - (int)sz.cy - 2 * pad - 6), y1 = H;
        size_t rowBytes = (size_t)W * 4;
        memcpy(bits + y0 * rowBytes, px + y0 * rowBytes, (y1 - y0) * rowBytes);
        RECT box = {6, y0, std::min(W, 6 + (int)sz.cx + 2 * pad), y0 + sz.cy + 2 * pad};
        FillRect(dc, &box, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 230, 80));
        TextOutW(dc, box.left + pad, box.top + pad, text.c_str(), (int)text.size());
        GdiFlush();
        memcpy(px + y0 * rowBytes, bits + y0 * rowBytes, (y1 - y0) * rowBytes);
    }
};

// Камера: сведения об устройстве, поток захвата, последний кадр и запись видео.
// Поля под cs читаются из GUI-потока и изменяются потоком захвата;
// writer и связанные поля защищены recCs (кадры в файл пишет поток захвата).
struct Camera {
    std::wstring name, link;            // FriendlyName и символьная ссылка устройства
    std::wstring vendor, vidpid, mfg, service;
    bool present = false;               // камера сейчас есть в системе
    std::atomic<int> state{CS_STOPPED};
    std::atomic<bool> stop{false};
    HANDLE thread = nullptr;
    std::wstring error;
    bool wantRun = false;               // пользователь хочет, чтобы камера работала (для автопереподключения)

    CRITICAL_SECTION cs;
    std::vector<BYTE> frame;            // последний кадр, BGRA 32 бит, строки сверху вниз
    int fw = 0, fh = 0;
    unsigned long long frames = 0;      // кадров получено с момента запуска
    double fps = 0;                     // измеренная частота кадров
    std::vector<MediaFormat> formats;   // все поддерживаемые режимы
    int cur = -1;                       // текущий режим (индекс в formats)
    MediaFormat want;                   // режим, выбранный пользователем
    bool haveWant = false;

    CRITICAL_SECTION recCs;
    IMFSinkWriter* writer = nullptr;
    DWORD recStream = 0;
    LONGLONG recT0 = 0;                 // QPC первого кадра текущего файла
    LONGLONG recLast = -1;              // метка последнего записанного кадра (100 нс)
    unsigned long long recFrames = 0;
    DWORD recTick = 0;                  // GetTickCount открытия текущего файла (для авторазбиения)
    std::wstring recFile;
    Stamper recStamp;                   // используется только потоком захвата

    Camera() {
        InitializeCriticalSection(&cs);
        InitializeCriticalSection(&recCs);
    }
    ~Camera() {
        DeleteCriticalSection(&cs);
        DeleteCriticalSection(&recCs);
    }
};

// Созданный материал (снимок или видеофрагмент) — для отчёта.
struct Material {
    std::wstring time, kind, path, info;
};

// ===========================================================================
// Глобальное состояние
// ===========================================================================
enum {
    IDC_STATUS = 101, IDC_CAMSEL, IDC_REENUM, IDC_LBL_INFO, IDC_INFO, IDC_LBL_RES, IDC_RES, IDC_FPS, IDC_APPLY,
    IDC_CONNECT, IDC_DISCONNECT, IDC_SNAP, IDC_SNAPFMT, IDC_RECSTART, IDC_RECSTOP, IDC_LBL_SPLIT, IDC_SPLIT,
    IDC_LBL_DIR, IDC_DIR, IDC_BROWSE, IDC_OPENDIR, IDC_TRAY, IDC_HOTKEYS, IDC_LBL_LOG, IDC_LOG, IDC_SAVEREPORT, IDC_VIEW, IDC_LBL_FPS
};
enum { TIMER_REPAINT = 1, TIMER_SECOND, TIMER_BLINK, TIMER_DEVCHANGE, TIMER_DEMO };
enum { HK_REC_START = 1, HK_REC_STOP = 2 };
const UINT WM_APP_LOG = WM_APP + 1;       // lParam — std::wstring* (сообщение из потока захвата)
const UINT WM_APP_CAMSTATE = WM_APP + 2;  // состояние камеры изменилось
const UINT WM_APP_FORMATS = WM_APP + 3;   // получен список режимов камеры
const UINT WM_APP_TRAY = WM_APP + 4;      // события значка в трее

static HINSTANCE g_inst;
static HWND g_wnd, g_view, g_info, g_res, g_fps, g_snapFmt, g_split, g_dir, g_log, g_status, g_camSel;
static HFONT g_font, g_fontBold, g_fontMono, g_fontView, g_fontViewBig;
static HICON g_icoIdle, g_icoRec, g_icoApp;       // значки трея: обычный и «идёт запись»; значок окна
static std::vector<std::unique_ptr<Camera>> g_cams;
static int g_active = -1;                         // выбранная камера
static std::vector<std::wstring> g_logLines;
static std::vector<Material> g_materials;
static std::vector<MediaFormat> g_resItems, g_fpsItems;
static unsigned long long g_painted = 0;
static std::wstring g_saveDir;
static bool g_hidden = false, g_blinkOn = false;  // g_hidden — окно скрыто, работа в фоне (трей)
static bool g_hotkeysOk = false;
static UINT g_msgTaskbarCreated = 0;              // перезапуск проводника — значок трея добавляется заново
static HDEVNOTIFY g_devNotify[2];

// Сеанс записи. Один сеанс может состоять из нескольких файлов (авторазбиение по интервалу).
static bool g_recWanted = false;   // запись включена пользователем (кнопкой или горячей клавишей)
static int g_segment = 0;          // номер текущего файла в сеансе
static DWORD g_sessionTick = 0;    // начало сеанса (GetTickCount)
static DWORD g_splitSec = kDefaultSplitMin * 60;  // интервал разбиения, с (0 — не разбивать)
static std::wstring g_demoDir;
static int g_demoStep = 0;

// ===========================================================================
// Журнал событий
// ===========================================================================
// Запись в журнал: время, режим (окно / фоновый режим в трее) и текст события.
static void Log(const std::wstring& msg) {
    std::wstring line = NowStr(true) + (g_hidden ? L" [фон]  " : L" [окно] ") + msg;
    g_logLines.push_back(line);
    if (!g_log) return;
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)(line + L"\r\n").c_str());
}

// Сообщение в журнал из потока захвата: передаётся в GUI-поток через очередь сообщений.
static void PostLog(const std::wstring& msg) { PostMessageW(g_wnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(msg)); }

static Camera* Active() { return g_active >= 0 && g_active < (int)g_cams.size() ? g_cams[g_active].get() : nullptr; }

// ===========================================================================
// Сведения об устройстве через SetupAPI
// ===========================================================================
// Производитель по коду VID из символьной ссылки (\\?\usb#vid_04f2&pid_b729&mi_00#...).
static std::wstring VendorByVid(unsigned vid) {
    switch (vid) {
    case 0x046D: return L"Logitech";
    case 0x045E: return L"Microsoft";
    case 0x04F2: return L"Chicony Electronics";
    case 0x0C45: return L"Microdia (Sonix)";
    case 0x13D3: return L"IMC Networks (AzureWave)";
    case 0x5986: return L"Bison Electronics (Acer)";
    case 0x0BDA: return L"Realtek Semiconductor";
    case 0x174F: return L"Syntek";
    case 0x1BCF: return L"Sunplus Innovation";
    case 0x058F: return L"Alcor Micro";
    case 0x04CA: return L"Lite-On Technology";
    case 0x0408: return L"Quanta Computer";
    case 0x30C9: return L"Luxvisions Innotech";
    default: return L"";
    }
}

// VID/PID из символьной ссылки; производитель и служба драйвера из реестра устройства
// (SetupDiOpenDeviceInterface → SetupDiGetDeviceInterfaceDetail → SetupDiGetDeviceRegistryProperty).
static void FillDeviceInfo(Camera& c) {
    std::wstring low = c.link;
    std::transform(low.begin(), low.end(), low.begin(), ::towlower);
    unsigned vid = 0, pid = 0;
    size_t p = low.find(L"vid_");
    if (p != std::wstring::npos) {
        vid = wcstoul(low.c_str() + p + 4, nullptr, 16);
        size_t q = low.find(L"pid_");
        if (q != std::wstring::npos) pid = wcstoul(low.c_str() + q + 4, nullptr, 16);
        c.vidpid = Fmt(L"VID %04X, PID %04X", vid, pid);
    }
    HDEVINFO h = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
        if (SetupDiOpenDeviceInterfaceW(h, c.link.c_str(), 0, &ifd)) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(h, &ifd, nullptr, 0, &need, nullptr);
            std::vector<BYTE> buf(std::max<DWORD>(need, sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)));
            auto det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA dd = {sizeof(dd)};
            if (SetupDiGetDeviceInterfaceDetailW(h, &ifd, det, (DWORD)buf.size(), nullptr, &dd)) {
                auto prop = [&](DWORD id) {
                    wchar_t v[512] = {};
                    if (SetupDiGetDeviceRegistryPropertyW(h, &dd, id, nullptr, (BYTE*)v, sizeof(v) - sizeof(wchar_t), nullptr))
                        return std::wstring(v);
                    return std::wstring();
                };
                c.mfg = prop(SPDRP_MFG);
                c.service = prop(SPDRP_SERVICE);
            }
        }
        SetupDiDestroyDeviceInfoList(h);
    }
    c.vendor = VendorByVid(vid);
    if (c.vendor.empty()) c.vendor = !c.mfg.empty() ? c.mfg : (vid ? Fmt(L"VID %04X", vid) : L"н/д");
}

// ===========================================================================
// Сохранение изображений (снимки)
// ===========================================================================
// BMP: BITMAPFILEHEADER + BITMAPINFOHEADER + строки 24 бит снизу вверх, выровненные на 4 байта.
static bool SaveBmp(const std::wstring& path, const BYTE* bgra, int w, int h) {
    int row = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42;  // 'BM'
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + row * h;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;  // положительная высота — строки снизу вверх
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = row * h;
    std::vector<BYTE> data((size_t)row * h);
    for (int y = 0; y < h; ++y) {
        const BYTE* s = bgra + (size_t)(h - 1 - y) * w * 4;
        BYTE* d = data.data() + (size_t)y * row;
        for (int x = 0; x < w; ++x) {
            d[x * 3] = s[x * 4];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 2];
        }
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(f, &fh, sizeof(fh), &wr, nullptr) && WriteFile(f, &ih, sizeof(ih), &wr, nullptr) &&
              WriteFile(f, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(f);
    return ok != FALSE;
}

// JPEG/PNG через системный компонент WIC: фабрика → поток-файл → кодировщик → кадр → WritePixels → Commit.
static HRESULT SaveWic(const std::wstring& path, const BYTE* bgra, int w, int h, const GUID& container) {
    std::vector<BYTE> bgr((size_t)w * h * 3);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        bgr[i * 3] = bgra[i * 4];
        bgr[i * 3 + 1] = bgra[i * 4 + 1];
        bgr[i * 3 + 2] = bgra[i * 4 + 2];
    }
    IWICImagingFactory* fac = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    IPropertyBag2* props = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    if (SUCCEEDED(hr)) hr = fac->CreateStream(&st);
    if (SUCCEEDED(hr)) hr = st->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = fac->CreateEncoder(container, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, &props);
    if (SUCCEEDED(hr) && container == GUID_ContainerFormatJpeg && props) {
        PROPBAG2 opt = {};
        opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.92f;
        props->Write(1, &opt, &v);
    }
    if (SUCCEEDED(hr)) hr = fr->Initialize(props);
    if (SUCCEEDED(hr)) hr = fr->SetSize(w, h);
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = fr->SetPixelFormat(&pf);
    if (SUCCEEDED(hr)) hr = fr->WritePixels(h, w * 3, (UINT)bgr.size(), bgr.data());
    if (SUCCEEDED(hr)) hr = fr->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    SafeRelease(props);
    SafeRelease(fr);
    SafeRelease(enc);
    SafeRelease(st);
    SafeRelease(fac);
    return hr;
}
// ===========================================================================
// Захват видео (Media Foundation)
// ===========================================================================
static int CamIndex(const Camera* c) {
    for (size_t i = 0; i < g_cams.size(); ++i)
        if (g_cams[i].get() == c) return (int)i;
    return -1;
}

// Перечисление режимов камеры: IMFSourceReader::GetNativeMediaType перебирает типы медиа
// первого видеопотока; берутся подтип (формат пикселей), MF_MT_FRAME_SIZE и MF_MT_FRAME_RATE.
static std::vector<MediaFormat> EnumFormats(IMFSourceReader* rd) {
    std::vector<MediaFormat> v;
    for (DWORD i = 0;; ++i) {
        IMFMediaType* t = nullptr;
        if (FAILED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t))) break;
        MediaFormat f;
        f.native = i;
        t->GetGUID(MF_MT_SUBTYPE, &f.sub);
        MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &f.w, &f.h);
        MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &f.num, &f.den);
        t->Release();
        if (!f.w || !f.h) continue;
        if (std::none_of(v.begin(), v.end(), [&](const MediaFormat& x) { return x.Same(f); })) v.push_back(f);
    }
    std::stable_sort(v.begin(), v.end(), [](const MediaFormat& a, const MediaFormat& b) {
        if ((UINT64)a.w * a.h != (UINT64)b.w * b.h) return (UINT64)a.w * a.h > (UINT64)b.w * b.h;
        if (a.sub != b.sub) return SubtypeName(a.sub) < SubtypeName(b.sub);
        return a.Fps() > b.Fps();
    });
    return v;
}

// Режим по умолчанию: 1280×720 от 25 к/с, иначе 640×480 от 25 к/с, иначе первый в списке.
static int PickDefault(const std::vector<MediaFormat>& v) {
    const UINT32 pref[][2] = {{1280, 720}, {640, 480}};
    for (auto& r : pref)
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].w == r[0] && v[i].h == r[1] && v[i].Fps() >= 25) return (int)i;
    return v.empty() ? -1 : 0;
}

// Копирование кадра из буфера MF в буфер камеры (BGRA, строки сверху вниз). IMF2DBuffer::Lock2D
// сообщает адрес первой строки и шаг (pitch может быть отрицательным для «перевёрнутых» RGB).
static bool CopyFrame(IMFSample* s, int w, int h, LONG defStride, std::vector<BYTE>& out) {
    IMFMediaBuffer* buf = nullptr;
    if (FAILED(s->ConvertToContiguousBuffer(&buf))) return false;
    out.resize((size_t)w * h * 4);
    bool ok = false;
    IMF2DBuffer* b2 = nullptr;
    if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2)))) {
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
            for (int y = 0; y < h; ++y) memcpy(&out[(size_t)y * w * 4], scan0 + (LONG_PTR)y * pitch, (size_t)w * 4);
            b2->Unlock2D();
            ok = true;
        }
        b2->Release();
    }
    if (!ok) {
        BYTE* p = nullptr;
        DWORD maxLen = 0, len = 0;
        if (SUCCEEDED(buf->Lock(&p, &maxLen, &len))) {
            LONG st = defStride ? defStride : w * 4;
            if (len >= (DWORD)(std::abs(st) * h))
                for (int y = 0; y < h; ++y) {
                    const BYTE* r = st > 0 ? p + (size_t)y * st : p + (size_t)(h - 1 - y) * (-st);
                    memcpy(&out[(size_t)y * w * 4], r, (size_t)w * 4);
                }
            ok = len >= (DWORD)(std::abs(st) * h);
            buf->Unlock();
        }
    }
    buf->Release();
    return ok;
}

// объявления, нужные потоку захвата (определены ниже)
static void WriteVideoFrame(Camera* c, std::vector<BYTE>& px, int w, int h, double fps);

// Поток захвата одной камеры.
// 1) MFCreateDeviceSource — источник по символьной ссылке устройства;
// 2) MFCreateSourceReaderFromMediaSource с MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING —
//    Source Reader сам подключает декодер MJPEG и преобразователь YUY2/NV12 → RGB32;
// 3) выбор режима устройства (SetCurrentMediaType) и выходного формата RGB32;
// 4) цикл синхронного чтения ReadSample: кадр копируется для показа и, если идёт запись, кодируется.
static DWORD WINAPI CaptureThread(LPVOID param) {
    Camera* c = (Camera*)param;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMFAttributes* attr = nullptr;
    IMFMediaSource* src = nullptr;
    IMFSourceReader* rd = nullptr;
    HRESULT hr = MFCreateAttributes(&attr, 2);
    if (SUCCEEDED(hr)) hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr)) hr = attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, c->link.c_str());
    if (SUCCEEDED(hr)) hr = MFCreateDeviceSource(attr, &src);
    SafeRelease(attr);
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attr, 1);
    if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(src, attr, &rd);
    SafeRelease(attr);

    int w = 0, h = 0;
    LONG stride = 0;
    double nominalFps = 30;
    if (SUCCEEDED(hr)) {
        auto formats = EnumFormats(rd);
        int sel = -1;
        EnterCriticalSection(&c->cs);
        if (c->haveWant)
            for (size_t i = 0; i < formats.size(); ++i)
                if (formats[i].Same(c->want)) sel = (int)i;
        LeaveCriticalSection(&c->cs);
        if (sel < 0) sel = PickDefault(formats);
        if (sel < 0) hr = MF_E_INVALIDMEDIATYPE;
        IMFMediaType* t = nullptr;
        if (SUCCEEDED(hr)) hr = rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, formats[sel].native, &t);
        if (SUCCEEDED(hr)) hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t);
        SafeRelease(t);
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&t);
        if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(hr)) hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t);
        SafeRelease(t);
        if (SUCCEEDED(hr)) hr = rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &t);
        if (SUCCEEDED(hr)) {
            UINT32 uw = 0, uh = 0, st = 0;
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &uw, &uh);
            w = (int)uw;
            h = (int)uh;
            stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? (LONG)(INT32)st : w * 4;
            SafeRelease(t);
        }
        if (sel >= 0 && sel < (int)formats.size()) nominalFps = formats[sel].Fps();
        EnterCriticalSection(&c->cs);
        c->formats = formats;
        c->cur = SUCCEEDED(hr) ? sel : -1;
        c->fw = w;
        c->fh = h;
        c->frames = 0;
        c->fps = 0;
        LeaveCriticalSection(&c->cs);
        PostMessageW(g_wnd, WM_APP_FORMATS, (WPARAM)CamIndex(c), 0);
        if (SUCCEEDED(hr))
            PostLog(Fmt(L"Захват запущен: режим %s, вывод RGB32 %d×%d; режимов: %zu",
                        formats[sel].Text().c_str(), w, h, formats.size()));
    }

    if (FAILED(hr)) {
        c->error = HrText(hr);
        PostLog(L"Ошибка открытия камеры: " + c->error);
        c->state = CS_ERROR;
    } else {
        c->state = CS_RUNNING;
        PostMessageW(g_wnd, WM_APP_CAMSTATE, (WPARAM)CamIndex(c), 0);
        std::vector<BYTE> px;
        DWORD fpsTick = GetTickCount();
        unsigned fpsCount = 0;
        while (!c->stop) {
            DWORD stream = 0, flags = 0;
            LONGLONG ts = 0;
            IMFSample* s = nullptr;
            hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &s);
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
                SafeRelease(s);
                if (SUCCEEDED(hr)) hr = MF_E_END_OF_STREAM;
                break;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                IMFMediaType* t = nullptr;
                if (SUCCEEDED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &t))) {
                    UINT32 uw = 0, uh = 0, st = 0;
                    MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &uw, &uh);
                    w = (int)uw;
                    h = (int)uh;
                    stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? (LONG)(INT32)st : w * 4;
                    t->Release();
                }
            }
            if (!s) continue;
            bool got = CopyFrame(s, w, h, stride, px);
            s->Release();
            if (!got) continue;
            ++fpsCount;
            DWORD now = GetTickCount();
            EnterCriticalSection(&c->cs);
            c->frame = px;
            c->fw = w;
            c->fh = h;
            ++c->frames;
            if (now - fpsTick >= 1000) {
                c->fps = fpsCount * 1000.0 / (now - fpsTick);
                fpsTick = now;
                fpsCount = 0;
            }
            LeaveCriticalSection(&c->cs);
            EnterCriticalSection(&c->recCs);
            if (c->writer) WriteVideoFrame(c, px, w, h, nominalFps);
            LeaveCriticalSection(&c->recCs);
        }
        if (c->stop) c->state = c->present ? CS_STOPPED : CS_ABSENT;
        else {
            c->error = HrText(hr);
            PostLog(L"Поток видео прерван: " + c->error);
            c->state = CS_LOST;
        }
    }
    SafeRelease(rd);
    if (src) src->Shutdown();
    SafeRelease(src);
    CoUninitialize();
    PostMessageW(g_wnd, WM_APP_CAMSTATE, (WPARAM)CamIndex(c), 0);
    return 0;
}
// ===========================================================================
// Запись видео и снимки
// ===========================================================================
static void EnsureDir() { SHCreateDirectoryExW(nullptr, g_saveDir.c_str(), nullptr); }
static void AddMaterial(const wchar_t* kind, const std::wstring& path, const std::wstring& info) {
    g_materials.push_back({NowStr(true), kind, path, info});
}
static void Balloon(const std::wstring& title, const std::wstring& text);

// Передача кадра кодировщику: буфер MF + сэмпл с меткой времени (в единицах 100 нс)
// от начала текущего файла и длительностью одного кадра. Вызывается в потоке захвата под recCs.
static void WriteVideoFrame(Camera* c, std::vector<BYTE>& px, int w, int h, double fps) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (!c->recT0) c->recT0 = now.QuadPart;
    LONGLONG t = (LONGLONG)((now.QuadPart - c->recT0) * 10000000.0 / freq.QuadPart);
    if (t <= c->recLast) t = c->recLast + 1;  // метки времени должны строго возрастать
    c->recStamp.Apply(px.data(), w, h, NowStr(true) + L"  REC");  // дата и время на каждом кадре
    IMFMediaBuffer* mb = nullptr;
    IMFSample* smp = nullptr;
    DWORD size = (DWORD)px.size();
    HRESULT hr = MFCreateMemoryBuffer(size, &mb);
    BYTE* dst = nullptr;
    if (SUCCEEDED(hr)) hr = mb->Lock(&dst, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(dst, px.data(), size);
        mb->Unlock();
        mb->SetCurrentLength(size);
        hr = MFCreateSample(&smp);
    }
    if (SUCCEEDED(hr)) hr = smp->AddBuffer(mb);
    if (SUCCEEDED(hr)) hr = smp->SetSampleTime(t);
    if (SUCCEEDED(hr)) hr = smp->SetSampleDuration((LONGLONG)(10000000.0 / (fps > 0 ? fps : 30)));
    if (SUCCEEDED(hr)) hr = c->writer->WriteSample(c->recStream, smp);
    if (SUCCEEDED(hr)) {
        c->recLast = t;
        ++c->recFrames;
    }
    SafeRelease(smp);
    SafeRelease(mb);
}

// Открытие нового файла записи: IMFSinkWriter → MP4 (H.264). Выходной тип — H.264 с заданным
// битрейтом, входной — RGB32 того же размера; преобразование RGB32 → NV12 и кодирование делает
// Media Foundation. seg — номер файла в сеансе (для авторазбиения). Вызывается из GUI-потока.
static bool OpenSegment(Camera* c, int seg) {
    EnterCriticalSection(&c->cs);
    int w = c->fw, h = c->fh;
    MediaFormat f = c->cur >= 0 && c->cur < (int)c->formats.size() ? c->formats[c->cur] : MediaFormat();
    LeaveCriticalSection(&c->cs);
    if (!w || !h) return false;
    double fps = f.Fps() > 0 ? f.Fps() : 30;
    UINT32 num = f.num ? f.num : 30, den = f.num ? f.den : 1;
    EnsureDir();
    std::wstring path = UniquePath(g_saveDir, Fmt(L"video_%s_part%02d", FileStamp().c_str(), seg + 1), L".mp4");

    IMFAttributes* a = nullptr;
    IMFSinkWriter* sw = nullptr;
    IMFMediaType* out = nullptr;
    IMFMediaType* in = nullptr;
    DWORD stream = 0;
    HRESULT hr = MFCreateAttributes(&a, 1);
    if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, a, &sw);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&out);
    if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)std::min(20000000.0, w * h * fps * 0.25));
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(out, MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(out, MF_MT_FRAME_RATE, num, den);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = sw->AddStream(out, &stream);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(w * 4));  // строки сверху вниз
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(in, MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(in, MF_MT_FRAME_RATE, num, den);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = sw->SetInputMediaType(stream, in, nullptr);
    if (SUCCEEDED(hr)) hr = sw->BeginWriting();
    SafeRelease(a);
    SafeRelease(out);
    SafeRelease(in);
    if (FAILED(hr)) {
        SafeRelease(sw);
        DeleteFileW(path.c_str());
        Log(L"Ошибка открытия файла записи: " + HrText(hr));
        return false;
    }
    EnterCriticalSection(&c->recCs);
    c->writer = sw;
    c->recStream = stream;
    c->recT0 = 0;
    c->recLast = -1;
    c->recFrames = 0;
    c->recFile = path;
    c->recTick = GetTickCount();
    LeaveCriticalSection(&c->recCs);
    Log(Fmt(L"Файл записи открыт: %s (H.264, %d×%d, %s)", path.c_str(), w, h, f.FpsText().c_str()));
    return true;
}

// Закрытие текущего файла: writer отсоединяется от потока захвата, Finalize дописывает индекс MP4.
static void CloseSegment(Camera* c, const wchar_t* note) {
    EnterCriticalSection(&c->recCs);
    IMFSinkWriter* sw = c->writer;
    c->writer = nullptr;
    unsigned long long frames = c->recFrames;
    LONGLONG dur = c->recLast;
    std::wstring path = c->recFile;
    LeaveCriticalSection(&c->recCs);
    if (!sw) return;
    HRESULT hr = frames ? sw->Finalize() : E_FAIL;
    sw->Release();
    if (SUCCEEDED(hr)) {
        std::wstring info = Fmt(L"%.1f с, %llu кадров, %.2f МБ", dur / 1e7, frames, FileSize(path) / 1048576.0);
        AddMaterial(L"Видео", path, info);
        Log(Fmt(L"Файл записи закрыт%s: %s (%s)", note, path.c_str(), info.c_str()));
        if (g_hidden) Balloon(L"Фрагмент видео сохранён", path);
    } else {
        DeleteFileW(path.c_str());
        Log(Fmt(L"Файл записи закрыт%s без сохранения (%s)", note, frames ? HrText(hr).c_str() : L"нет кадров"));
    }
}

static void StartCamera(Camera* c);

static std::wstring SplitText() {
    if (!g_splitSec) return L"без разбиения";
    if (g_splitSec < 60 || g_splitSec % 60) return Fmt(L"каждые %lu с", g_splitSec);
    return Fmt(L"каждые %lu мин", g_splitSec / 60);
}

// Старт сеанса записи (кнопка или горячая клавиша Ctrl+Shift+R).
// Если камера ещё не запущена (например, клавиша нажата в фоне сразу после запуска), она
// запускается, а запись начнётся автоматически, когда пойдут кадры (см. WM_APP_CAMSTATE).
// resume = true — продолжение того же сеанса после переподключения камеры (номера файлов идут дальше).
static void StartRecording(bool resume = false) {
    Camera* c = Active();
    if (!c) {
        Log(L"Запись невозможна: камера не выбрана");
        return;
    }
    if (c->writer) {
        Log(L"Запись уже идёт");
        return;
    }
    if (c->state != CS_RUNNING) {
        if (!c->present) {
            Log(L"Запись невозможна: камера отключена");
            return;
        }
        g_recWanted = true;
        if (!resume) {
            g_segment = 0;
            g_sessionTick = GetTickCount();
        }
        c->wantRun = true;
        StartCamera(c);
        Log(L"Камера ещё не работает — запись начнётся после её запуска");
        return;
    }
    if (resume) ++g_segment;
    else {
        g_segment = 0;
        g_sessionTick = GetTickCount();
    }
    if (OpenSegment(c, g_segment)) {
        g_recWanted = true;
        Log(L"● Запись " + std::wstring(resume ? L"возобновлена" : L"начата") + L". Авторазбиение: " + SplitText());
        Balloon(resume ? L"Запись возобновлена" : L"Запись начата", L"Фоновая запись, разбиение: " + SplitText() + L". Ctrl+Shift+S — стоп");
    } else {
        g_recWanted = false;
    }
}

// Стоп сеанса записи (кнопка или горячая клавиша Ctrl+Shift+S).
static void StopRecording(const wchar_t* note) {
    Camera* c = Active();
    bool was = g_recWanted;
    g_recWanted = false;
    if (!c || !c->writer) {
        if (was) Log(L"Ожидание записи отменено");
        return;
    }
    CloseSegment(c, note);
    DWORD secs = (GetTickCount() - g_sessionTick) / 1000;
    Log(Fmt(L"■ Запись остановлена%s. Сеанс: %d файл(ов), %lu:%02lu", note, g_segment + 1, secs / 60, secs % 60));
    Balloon(L"Запись остановлена", Fmt(L"Файлов: %d, длительность %lu:%02lu", g_segment + 1, secs / 60, secs % 60));
}

// Авторазбиение: по таймеру в GUI-потоке текущий файл закрывается и сразу открывается следующий,
// поэтому запись не прерывается на границе фрагментов.
static void RollSegmentIfDue() {
    Camera* c = Active();
    if (!c || !c->writer || !g_splitSec) return;
    if ((GetTickCount() - c->recTick) / 1000 < g_splitSec) return;
    Log(L"Авторазбиение: достигнут интервал (" + SplitText() + L")");
    CloseSegment(c, L" (авторазбиение)");
    ++g_segment;
    if (!OpenSegment(c, g_segment)) {
        g_recWanted = false;
        Log(L"Не удалось открыть следующий файл — запись прекращена");
    }
}

// Снимок: копия последнего кадра + дата и время на изображении, сохранение в JPEG или BMP.
static void TakeSnapshot(bool jpeg) {
    Camera* c = Active();
    if (!c) return;
    std::vector<BYTE> px;
    int w, h;
    EnterCriticalSection(&c->cs);
    px = c->frame;
    w = c->fw;
    h = c->fh;
    LeaveCriticalSection(&c->cs);
    if (c->state != CS_RUNNING || px.empty() || px.size() < (size_t)w * h * 4) {
        Log(L"Снимок невозможен: нет изображения с камеры");
        return;
    }
    Stamper st;
    st.Apply(px.data(), w, h, NowStr(true));
    EnsureDir();
    std::wstring path = UniquePath(g_saveDir, L"snapshot_" + FileStamp(), jpeg ? L".jpg" : L".bmp");
    HRESULT hr = jpeg ? SaveWic(path, px.data(), w, h, GUID_ContainerFormatJpeg)
                      : (SaveBmp(path, px.data(), w, h) ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
    if (SUCCEEDED(hr)) {
        std::wstring info = Fmt(L"%d×%d, %.0f КБ", w, h, FileSize(path) / 1024.0);
        AddMaterial(L"Снимок", path, info);
        Log(L"Снимок сохранён: " + path + L" (" + info + L")");
        if (g_hidden) Balloon(L"Снимок сохранён", path);
    } else {
        Log(L"Ошибка сохранения снимка: " + HrText(hr));
    }
}
// ===========================================================================
// Управление камерой и перечисление (GUI-поток)
// ===========================================================================
static void StartCamera(Camera* c) {
    if (c->thread || !c->present) return;
    c->stop = false;
    c->state = CS_STARTING;
    c->error.clear();
    c->thread = CreateThread(nullptr, 0, CaptureThread, c, 0, nullptr);
    if (!c->thread) c->state = CS_ERROR;
}

// Остановка потока захвата. ReadSample возвращает управление с каждым кадром, поэтому поток
// замечает флаг stop в течение одного кадрового интервала.
static void StopCamera(Camera* c) {
    if (c->writer && c == Active()) StopRecording(L" (камера остановлена)");
    if (!c->thread) return;
    c->stop = true;
    if (WaitForSingleObject(c->thread, 5000) == WAIT_TIMEOUT) Log(L"Поток захвата не завершился за 5 с");
    CloseHandle(c->thread);
    c->thread = nullptr;
    if (c->state == CS_RUNNING || c->state == CS_STARTING) c->state = c->present ? CS_STOPPED : CS_ABSENT;
}

static std::wstring StateName(int s) {
    switch (s) {
    case CS_STOPPED: return L"Остановлена";
    case CS_STARTING: return L"Подключение…";
    case CS_RUNNING: return L"Работает";
    case CS_ABSENT: return L"Отключена";
    case CS_LOST: return L"Связь потеряна";
    case CS_ERROR: return L"Ошибка";
    default: return L"";
    }
}

static COLORREF StateColor(int s) {
    switch (s) {
    case CS_RUNNING: return RGB(40, 180, 70);
    case CS_STARTING: return RGB(230, 170, 0);
    case CS_STOPPED: return RGB(140, 140, 140);
    default: return RGB(220, 50, 50);
    }
}

static void UpdateAll();
static void FillFormatCombos();

// MFEnumDeviceSources возвращает IMFActivate для каждой камеры; из него читаются имя и ссылка.
// Список сопоставляется с известными камерами по ссылке: новые добавляются, пропавшие помечаются
// отключёнными, вернувшиеся — переподключаются автоматически (в т. ч. возобновляется запись).
static void EnumerateCameras(const wchar_t* reason) {
    IMFAttributes* a = nullptr;
    IMFActivate** devs = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFCreateAttributes(&a, 1);
    if (SUCCEEDED(hr)) hr = a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr)) hr = MFEnumDeviceSources(a, &devs, &count);
    SafeRelease(a);
    if (FAILED(hr)) {
        Log(L"Ошибка перечисления камер: " + HrText(hr));
        return;
    }
    struct Dev {
        std::wstring name, link;
    };
    std::vector<Dev> found;
    for (UINT32 i = 0; i < count; ++i) {
        wchar_t* s = nullptr;
        UINT32 len = 0;
        Dev d;
        if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &s, &len))) {
            d.name = s;
            CoTaskMemFree(s);
        }
        if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &s, &len))) {
            d.link = s;
            CoTaskMemFree(s);
        }
        devs[i]->Release();
        if (!d.link.empty()) found.push_back(d);
    }
    CoTaskMemFree(devs);
    Log(Fmt(L"Перечисление камер (%s): найдено %zu", reason, found.size()));

    for (auto& uc : g_cams) {
        Camera* c = uc.get();
        bool still = std::any_of(found.begin(), found.end(), [&](const Dev& d) { return _wcsicmp(d.link.c_str(), c->link.c_str()) == 0; });
        if (c->present && !still) {
            c->present = false;
            // Файл закрывается, но сеанс записи (g_recWanted) сохраняется — при возврате камеры он продолжится
            if (c->writer) CloseSegment(c, L" (камера отключена)");
            StopCamera(c);
            c->state = CS_ABSENT;
            Log(L"Камера отключена от компьютера: " + c->name);
        }
    }
    for (auto& d : found) {
        int idx = -1;
        for (size_t i = 0; i < g_cams.size(); ++i)
            if (_wcsicmp(g_cams[i]->link.c_str(), d.link.c_str()) == 0) idx = (int)i;
        if (idx < 0) {
            auto cam = std::make_unique<Camera>();
            cam->name = d.name.empty() ? L"(без названия)" : d.name;
            cam->link = d.link;
            cam->present = true;
            FillDeviceInfo(*cam);
            g_cams.push_back(std::move(cam));
            idx = (int)g_cams.size() - 1;
            Camera* c = g_cams[idx].get();
            Log(Fmt(L"Обнаружена камера: %s — %s; %s", c->name.c_str(), c->vendor.c_str(), c->link.c_str()));
            if (g_active < 0) g_active = idx;
            if (g_active == idx) {  // автоматически запускается активная (первая) камера
                c->wantRun = true;
                StartCamera(c);
            }
        } else {
            Camera* c = g_cams[idx].get();
            if (!c->present) {
                c->present = true;
                c->state = CS_STOPPED;
                Log(L"Камера снова подключена: " + c->name);
                if (c->wantRun) {
                    // Запись, прерванная отключением, возобновится, когда пойдут кадры (WM_APP_CAMSTATE)
                    Log(idx == g_active && g_recWanted ? L"Автоматическое переподключение, запись будет продолжена"
                                                       : L"Автоматическое переподключение");
                    StartCamera(c);
                }
            } else if (c->wantRun && !c->thread && (c->state == CS_LOST || c->state == CS_ERROR)) {
                Log(L"Повторная попытка подключения");
                StartCamera(c);
            }
        }
    }
    if (g_active >= (int)g_cams.size()) g_active = g_cams.empty() ? -1 : 0;
    FillFormatCombos();
    UpdateAll();
}
// ===========================================================================
// Обновление интерфейса
// ===========================================================================
// Панель «Сведения о камере»: устройство, драйвер, текущий режим, фактический FPS, запись.
static void UpdateInfo() {
    Camera* c = Active();
    if (!c) {
        SetWindowTextW(g_info, L"Камеры не обнаружены.\r\nПодключите веб-камеру — список обновится автоматически.");
        return;
    }
    EnterCriticalSection(&c->cs);
    std::wstring mode = c->cur >= 0 && c->cur < (int)c->formats.size() ? c->formats[c->cur].Text() : L"н/д";
    std::wstring fmt = c->cur >= 0 && c->cur < (int)c->formats.size() ? SubtypeName(c->formats[c->cur].sub) : L"н/д";
    std::wstring res = c->fw ? Fmt(L"%d×%d", c->fw, c->fh) : L"н/д";
    double fps = c->fps;
    unsigned long long frames = c->frames;
    size_t nf = c->formats.size();
    LeaveCriticalSection(&c->cs);
    std::wstring rec = L"нет";
    if (c->writer)
        rec = Fmt(L"идёт, файл %d, %lu с → %s", g_segment + 1, (GetTickCount() - c->recTick) / 1000, c->recFile.c_str());
    std::wstring s;
    s += L"Камера: " + c->name + L"\r\n";
    s += L"Производитель: " + c->vendor + (c->vidpid.empty() ? L"" : L" (" + c->vidpid + L")") + L"\r\n";
    if (!c->service.empty()) s += L"Драйвер: " + c->service + (c->mfg.empty() ? L"" : L" (" + c->mfg + L")") + L"\r\n";
    s += L"Состояние: " + StateName(c->state) + (c->error.empty() || c->state == CS_RUNNING ? L"" : L" — " + c->error) + L"\r\n";
    s += L"Формат пикселей: " + fmt + L" → RGB32\r\n";
    s += L"Разрешение: " + res + L"\r\n";
    s += L"Режим устройства: " + mode + L"\r\n";
    s += Fmt(L"Фактическая частота: %.1f к/с, кадров получено: %llu\r\n", fps, frames);
    s += Fmt(L"Поддерживаемых режимов: %zu\r\n", nf);
    s += L"Запись: " + rec + L"\r\n";
    s += L"Ссылка устройства: " + c->link;
    SetWindowTextW(g_info, s.c_str());
}

// Список «Частота кадров» для выбранного разрешения.
static void FillFpsCombo() {
    Camera* c = Active();
    SendMessageW(g_fps, CB_RESETCONTENT, 0, 0);
    g_fpsItems.clear();
    int r = (int)SendMessageW(g_res, CB_GETCURSEL, 0, 0);
    if (!c || r < 0 || r >= (int)g_resItems.size()) return;
    EnterCriticalSection(&c->cs);
    MediaFormat cur = c->cur >= 0 && c->cur < (int)c->formats.size() ? c->formats[c->cur] : MediaFormat();
    for (auto& f : c->formats)
        if (f.SameRes(g_resItems[r])) g_fpsItems.push_back(f);
    LeaveCriticalSection(&c->cs);
    int sel = 0;
    for (size_t i = 0; i < g_fpsItems.size(); ++i) {
        SendMessageW(g_fps, CB_ADDSTRING, 0, (LPARAM)g_fpsItems[i].FpsText().c_str());
        if (g_fpsItems[i].Same(cur)) sel = (int)i;
    }
    SendMessageW(g_fps, CB_SETCURSEL, sel, 0);
}

// Список «Формат и разрешение» активной камеры (уникальные сочетания формата пикселей и размера).
static void FillFormatCombos() {
    Camera* c = Active();
    SendMessageW(g_res, CB_RESETCONTENT, 0, 0);
    g_resItems.clear();
    if (c) {
        EnterCriticalSection(&c->cs);
        MediaFormat cur = c->cur >= 0 && c->cur < (int)c->formats.size() ? c->formats[c->cur] : MediaFormat();
        int sel = 0;
        for (auto& f : c->formats) {
            if (std::any_of(g_resItems.begin(), g_resItems.end(), [&](const MediaFormat& x) { return x.SameRes(f); })) continue;
            if (f.SameRes(cur)) sel = (int)g_resItems.size();
            g_resItems.push_back(f);
        }
        LeaveCriticalSection(&c->cs);
        for (auto& f : g_resItems) SendMessageW(g_res, CB_ADDSTRING, 0, (LPARAM)f.ResText().c_str());
        SendMessageW(g_res, CB_SETCURSEL, sel, 0);
    }
    FillFpsCombo();
}

// Выпадающий список выбора камеры.
static void FillCamSelect() {
    int old = (int)SendMessageW(g_camSel, CB_GETCURSEL, 0, 0);
    SendMessageW(g_camSel, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_cams.size(); ++i) {
        std::wstring t = Fmt(L"%zu: %s [%s]", i + 1, g_cams[i]->name.c_str(), StateName(g_cams[i]->state).c_str());
        SendMessageW(g_camSel, CB_ADDSTRING, 0, (LPARAM)t.c_str());
    }
    if (g_active >= 0) SendMessageW(g_camSel, CB_SETCURSEL, g_active, 0);
    else if (old >= 0) SendMessageW(g_camSel, CB_SETCURSEL, 0, 0);
}

static void UpdateButtons() {
    Camera* c = Active();
    bool running = c && c->state == CS_RUNNING;
    EnableWindow(GetDlgItem(g_wnd, IDC_CONNECT), c && c->present && !c->thread);
    EnableWindow(GetDlgItem(g_wnd, IDC_DISCONNECT), c && c->thread != nullptr);
    EnableWindow(GetDlgItem(g_wnd, IDC_SNAP), running);
    EnableWindow(GetDlgItem(g_wnd, IDC_RECSTART), running && c && !c->writer);
    EnableWindow(GetDlgItem(g_wnd, IDC_RECSTOP), c && c->writer != nullptr);
    EnableWindow(GetDlgItem(g_wnd, IDC_APPLY), c && c->present && !g_resItems.empty());
}

static COLORREF g_statusColor = RGB(140, 140, 140);
static void UpdateStatus() {
    Camera* c = Active();
    std::wstring s;
    int present = 0;
    for (auto& x : g_cams) present += x->present;
    if (!c) {
        s = L"●  Камеры не подключены";
        g_statusColor = RGB(220, 50, 50);
    } else if (c->writer) {
        DWORD secs = (GetTickCount() - g_sessionTick) / 1000;
        s = Fmt(L"●  ЗАПИСЬ ИДЁТ — файл %d, сеанс %lu:%02lu%s", g_segment + 1, secs / 60, secs % 60,
                g_hidden ? L"  (скрытый режим)" : L"");
        g_statusColor = RGB(210, 40, 40);
    } else {
        s = Fmt(L"●  %s — %s. Камер в системе: %d", c->name.c_str(), StateName(c->state).c_str(), present);
        g_statusColor = c->state == CS_RUNNING ? RGB(30, 150, 60) : RGB(210, 140, 0);
    }
    SetWindowTextW(g_status, s.c_str());
    InvalidateRect(g_status, nullptr, TRUE);
}

static void UpdateAll() {
    UpdateStatus();
    FillCamSelect();
    UpdateInfo();
    UpdateButtons();
    if (!g_hidden) InvalidateRect(g_view, nullptr, FALSE);
}

static void SetActive(int i) {
    if (i < 0 || i >= (int)g_cams.size() || i == g_active) return;
    g_active = i;
    Log(L"Выбрана активная камера: " + g_cams[i]->name);
    FillFormatCombos();
    UpdateAll();
}
// ===========================================================================
// Окно предпросмотра
// ===========================================================================
static void ShadowText(HDC dc, int x, int y, const std::wstring& t, COLORREF col, UINT align = TA_LEFT) {
    SetTextAlign(dc, align | TA_TOP);
    SetTextColor(dc, RGB(0, 0, 0));
    TextOutW(dc, x + 1, y + 1, t.c_str(), (int)t.size());
    SetTextColor(dc, col);
    TextOutW(dc, x, y, t.c_str(), (int)t.size());
}

// Кадр масштабируется с сохранением пропорций (StretchDIBits), поверх выводятся имя камеры,
// индикатор состояния, дата и время, разрешение, FPS и отметка записи.
static void DrawView(HDC dc, const RECT& rc) {
    HBRUSH bg = CreateSolidBrush(RGB(24, 26, 30));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    Camera* c = Active();
    int W = rc.right, H = rc.bottom;
    SelectObject(dc, g_fontViewBig);
    if (!c) {
        ShadowText(dc, W / 2, H / 2 - 10, L"Камера не подключена", RGB(150, 150, 150), TA_CENTER);
    } else {
        int st = c->state, fw = 0, fh = 0;
        double fps = 0;
        EnterCriticalSection(&c->cs);
        fw = c->fw;
        fh = c->fh;
        fps = c->fps;
        if (st == CS_RUNNING && !c->frame.empty() && c->frame.size() >= (size_t)fw * fh * 4) {
            double k = std::min((double)W / fw, (double)H / fh);
            int dw = (int)(fw * k), dh = (int)(fh * k);
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = fw;
            bi.bmiHeader.biHeight = -fh;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, (W - dw) / 2, (H - dh) / 2, dw, dh, 0, 0, fw, fh, c->frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            g_painted = c->frames;
        }
        LeaveCriticalSection(&c->cs);
        if (st != CS_RUNNING) {
            std::wstring msg = st == CS_STARTING ? L"Подключение к камере…"
                             : st == CS_STOPPED  ? L"Камера остановлена"
                             : st == CS_ABSENT   ? L"Камера отключена"
                             : st == CS_LOST     ? L"Связь с камерой потеряна"
                                                 : L"Ошибка открытия камеры";
            ShadowText(dc, W / 2, H / 2 - 12, msg, StateColor(st), TA_CENTER);
            if (!c->error.empty() && st != CS_STOPPED) {
                SelectObject(dc, g_fontView);
                ShadowText(dc, W / 2, H / 2 + 16, c->error.substr(0, 80), RGB(200, 200, 200), TA_CENTER);
            }
        }
        SelectObject(dc, g_fontView);
        ShadowText(dc, 10, 8, c->name, RGB(255, 255, 255));
        HBRUSH dot = CreateSolidBrush(StateColor(st));
        HGDIOBJ ob = SelectObject(dc, dot), op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, W - 24, 10, W - 10, 24);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(dot);
        ShadowText(dc, W - 30, 8, StateName(st), RGB(255, 255, 255), TA_RIGHT);
        if (c->writer) {
            DWORD sec = (GetTickCount() - g_sessionTick) / 1000;
            bool blink = (GetTickCount() / 500) % 2 == 0;
            ShadowText(dc, W - 10, 30, Fmt(L"%s REC %02lu:%02lu (файл %d)", blink ? L"●" : L"   ", sec / 60, sec % 60, g_segment + 1),
                       RGB(255, 60, 60), TA_RIGHT);
        }
        if (st == CS_RUNNING) {
            ShadowText(dc, 10, H - 26, NowStr(true), RGB(255, 230, 80));
            ShadowText(dc, W - 10, H - 26, Fmt(L"%d×%d  %.1f к/с", fw, fh, fps), RGB(255, 255, 255), TA_RIGHT);
        }
    }
    HPEN pen = CreatePen(PS_INSIDEFRAME, 1, RGB(90, 90, 90));
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 0, 0, W, H);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

static LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            // Двойная буферизация: рисуем в памяти, затем одно копирование на экран (без мерцания)
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            DrawView(mem, rc);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (Active() && Active()->state == CS_RUNNING) TakeSnapshot(SendMessageW(g_snapFmt, CB_GETCURSEL, 0, 0) == 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
// Системный трей и уведомления
// ===========================================================================
// Небольшой значок, рисуемый программно: серый кружок в покое, красный — во время записи.
static HICON MakeDotIcon(COLORREF col, int size) {
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, color);
    HBRUSH b = CreateSolidBrush(col);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, 1, 1, size - 1, size - 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
    SelectObject(dc, old);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON ico = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    DeleteDC(dc);
    return ico;
}

static void TraySet(DWORD op, HICON ico, const std::wstring& tip) {
    NOTIFYICONDATAW n = {sizeof(n)};
    n.hWnd = g_wnd;
    n.uID = 1;
    n.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    n.uCallbackMessage = WM_APP_TRAY;
    n.hIcon = ico ? ico : g_icoIdle;
    wcsncpy_s(n.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(op, &n);
}

static void Balloon(const std::wstring& title, const std::wstring& text) {
    if (!g_hidden) return;  // уведомления нужны именно когда окно скрыто
    NOTIFYICONDATAW n = {sizeof(n)};
    n.hWnd = g_wnd;
    n.uID = 1;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void HideToTray() {
    if (g_hidden) return;
    g_hidden = true;
    TraySet(NIM_ADD, nullptr, L"Видеонаблюдение — двойной щелчок открывает окно");
    ShowWindow(g_wnd, SW_HIDE);
    Log(L"Окно свёрнуто в трей. Горячие клавиши активны: Ctrl+Shift+R — старт, Ctrl+Shift+S — стоп записи");
    Balloon(L"Свёрнуто в трей", L"Захват продолжается. Ctrl+Shift+R — старт записи, Ctrl+Shift+S — стоп");
}

static void RestoreFromTray() {
    if (!g_hidden) return;
    g_hidden = false;
    TraySet(NIM_DELETE, nullptr, L"");
    ShowWindow(g_wnd, SW_SHOW);
    ShowWindow(g_wnd, SW_RESTORE);
    SetForegroundWindow(g_wnd);
    Log(L"Окно восстановлено из трея");
    UpdateAll();
}

// Мигание значка в трее во время записи (по таймеру TIMER_BLINK).
static void TrayBlink() {
    if (!g_hidden) return;
    Camera* c = Active();
    if (c && c->writer) {
        g_blinkOn = !g_blinkOn;
        TraySet(NIM_MODIFY, g_blinkOn ? g_icoRec : g_icoIdle, L"● Идёт запись (Ctrl+Shift+S — стоп)");
    } else if (g_blinkOn) {
        g_blinkOn = false;
        TraySet(NIM_MODIFY, g_icoIdle, L"Видеонаблюдение — двойной щелчок открывает окно");
    }
}
// ===========================================================================
// Отчёт и диалоги
// ===========================================================================
static std::wstring BuildReport() {
    wchar_t comp[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD csz = _countof(comp);
    GetComputerNameW(comp, &csz);
    std::wstring s = L"ОТЧЁТ СЕАНСА ВИДЕОНАБЛЮДЕНИЯ\r\n";
    s += Fmt(L"Компьютер: %s · Дата: %s\r\n", comp, NowStr(true).c_str());
    s += L"Каталог материалов: " + g_saveDir + L"\r\nАвторазбиение записи: " + SplitText() + L"\r\n";
    s += L"Горячие клавиши: Ctrl+Shift+R — старт записи, Ctrl+Shift+S — стоп" +
         std::wstring(g_hotkeysOk ? L"" : L" (не зарегистрированы)") + L"\r\n\r\n";
    s += L"1. КАМЕРЫ\r\n";
    for (size_t i = 0; i < g_cams.size(); ++i) {
        Camera* c = g_cams[i].get();
        EnterCriticalSection(&c->cs);
        auto formats = c->formats;
        int cur = c->cur;
        double fps = c->fps;
        LeaveCriticalSection(&c->cs);
        s += Fmt(L"  Камера %zu: %s\r\n", i + 1, c->name.c_str());
        s += L"    Производитель: " + c->vendor + (c->vidpid.empty() ? L"" : L" (" + c->vidpid + L")") + L"\r\n";
        if (!c->service.empty()) s += L"    Драйвер: " + c->service + L"\r\n";
        s += L"    Ссылка: " + c->link + L"\r\n";
        s += L"    Состояние: " + StateName(c->state) + L"\r\n";
        if (cur >= 0 && cur < (int)formats.size())
            s += Fmt(L"    Текущий режим: %s, фактически %.1f к/с\r\n", formats[cur].Text().c_str(), fps);
        s += Fmt(L"    Поддерживаемые режимы (%zu):\r\n", formats.size());
        for (auto& f : formats) s += L"      " + f.Text() + L"\r\n";
    }
    if (g_cams.empty()) s += L"  Камеры не обнаружены\r\n";
    s += L"\r\n2. СОЗДАННЫЕ МАТЕРИАЛЫ\r\n";
    for (auto& m : g_materials) s += L"  " + m.time + L" | " + m.kind + L" | " + m.path + L" | " + m.info + L"\r\n";
    if (g_materials.empty()) s += L"  нет\r\n";
    s += L"\r\n3. ЖУРНАЛ СЕАНСА\r\n";
    for (auto& l : g_logLines) s += L"  " + l + L"\r\n";
    return s;
}

static void SaveReport() {
    wchar_t file[MAX_PATH];
    swprintf_s(file, L"camera_report_%s.txt", FileStamp().c_str());
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"Текстовый отчёт (*.txt)\0*.txt\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = g_saveDir.c_str();
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Сохранение отчёта";
    if (!GetSaveFileNameW(&ofn)) return;
    if (WriteUtf8File(file, BuildReport())) Log(std::wstring(L"Отчёт сохранён: ") + file);
    else Log(Fmt(L"Ошибка записи отчёта (код %lu)", GetLastError()));
}

// Выбор каталога: стандартный диалог IFileOpenDialog в режиме FOS_PICKFOLDERS.
static void BrowseDir() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD opt = 0;
    d->GetOptions(&opt);
    d->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Каталог для сохранения снимков и видео");
    IShellItem* start = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(g_saveDir.c_str(), nullptr, IID_PPV_ARGS(&start)))) {
        d->SetFolder(start);
        start->Release();
    }
    if (SUCCEEDED(d->Show(g_wnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            wchar_t* p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                g_saveDir = p;
                CoTaskMemFree(p);
                SetWindowTextW(g_dir, g_saveDir.c_str());
                Log(L"Каталог сохранения: " + g_saveDir);
            }
            it->Release();
        }
    }
    d->Release();
}

// Скриншот главного окна (PrintWindow) в PNG — используется в демонстрационном режиме.
static void SaveWindowShot(const std::wstring& path) {
    RECT r;
    GetWindowRect(g_wnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC sdc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(g_wnd, mem, PW_RENDERFULLCONTENT);
    GdiFlush();
    HRESULT hr = SaveWic(path, (BYTE*)bits, w, h, GUID_ContainerFormatPng);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, sdc);
    Log(SUCCEEDED(hr) ? L"Скриншот окна: " + path : L"Ошибка скриншота: " + HrText(hr));
}

// Демонстрационный сценарий (/demo): шаги по секундному таймеру — снимки, запись в окне с коротким
// авторазбиением, сворачивание в трей, восстановление, скриншоты окна и отчёт. Пока окно в трее
// (шаги 10–24), запись запускается и останавливается реальными нажатиями Ctrl+Shift+R / Ctrl+Shift+S
// (нажимает пользователь или внешний скрипт) — так проверяются глобальные горячие клавиши.
static void DemoTick() {
    ++g_demoStep;
    switch (g_demoStep) {
    case 4:
        TakeSnapshot(true);
        TakeSnapshot(false);
        break;
    case 5:
        g_splitSec = 4;  // короткий интервал, чтобы показать авторазбиение
        StartRecording();
        break;
    case 8: SaveWindowShot(g_demoDir + L"\\screenshot_window_rec.png"); break;
    case 9: StopRecording(L""); break;
    case 10: HideToTray(); break;
    case 25: RestoreFromTray(); break;
    case 27: SaveWindowShot(g_demoDir + L"\\screenshot_after_tray.png"); break;
    case 28:
        WriteUtf8File(g_demoDir + L"\\report.txt", BuildReport());
        PostMessageW(g_wnd, WM_CLOSE, 0, 0);
        break;
    }
    UpdateAll();
}
// ===========================================================================
// Главное окно: элементы управления и раскладка
// ===========================================================================
static HWND g_grpCam, g_grpInfo, g_grpMode, g_grpCtl, g_grpBg, g_lblLog;

static HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}
static HWND Btn(const wchar_t* text, int id) { return Ctl(L"BUTTON", text, WS_TABSTOP | BS_PUSHBUTTON, id); }
static HWND Group(const wchar_t* text) {
    HWND h = Ctl(L"BUTTON", text, BS_GROUPBOX, -1);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    return h;
}

static void CreateControls() {
    g_status = Ctl(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, IDC_STATUS);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    g_view = CreateWindowExW(0, L"CamViewWnd", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, g_wnd, (HMENU)IDC_VIEW, g_inst, nullptr);

    g_grpCam = Group(L"Камера");
    g_camSel = Ctl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_CAMSEL);
    Btn(L"Обновить список", IDC_REENUM);
    Btn(L"Подключить камеру", IDC_CONNECT);
    Btn(L"Отключить", IDC_DISCONNECT);

    g_grpInfo = Group(L"Сведения об устройстве");
    g_info = Ctl(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, IDC_INFO, WS_EX_CLIENTEDGE);

    g_grpMode = Group(L"Режим захвата");
    Ctl(L"STATIC", L"Формат, разрешение:", 0, IDC_LBL_RES);
    g_res = Ctl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_RES);
    Ctl(L"STATIC", L"Частота кадров:", 0, IDC_LBL_FPS);
    g_fps = Ctl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_FPS);
    Btn(L"Применить", IDC_APPLY);

    g_grpCtl = Group(L"Снимки и запись");
    Btn(L"Сделать снимок", IDC_SNAP);
    g_snapFmt = Ctl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, IDC_SNAPFMT);
    SendMessageW(g_snapFmt, CB_ADDSTRING, 0, (LPARAM)L"JPEG");
    SendMessageW(g_snapFmt, CB_ADDSTRING, 0, (LPARAM)L"BMP");
    SendMessageW(g_snapFmt, CB_SETCURSEL, 0, 0);
    Btn(L"● Начать запись", IDC_RECSTART);
    Btn(L"■ Остановить запись", IDC_RECSTOP);
    Ctl(L"STATIC", L"Авторазбиение записи:", 0, IDC_LBL_SPLIT);
    g_split = Ctl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_SPLIT);
    const wchar_t* splits[] = {L"каждую 1 мин", L"каждые 2 мин", L"каждые 5 мин", L"каждые 10 мин",
                               L"каждые 15 мин", L"каждые 30 мин", L"не разбивать"};
    for (auto s : splits) SendMessageW(g_split, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessageW(g_split, CB_SETCURSEL, 2, 0);
    Ctl(L"STATIC", L"Каталог для сохранения:", 0, IDC_LBL_DIR);
    g_dir = Ctl(L"EDIT", g_saveDir.c_str(), ES_AUTOHSCROLL | ES_READONLY, IDC_DIR, WS_EX_CLIENTEDGE);
    Btn(L"Обзор…", IDC_BROWSE);
    Btn(L"Открыть", IDC_OPENDIR);

    g_grpBg = Group(L"Скрытый режим");
    Btn(L"Свернуть в трей", IDC_TRAY);
    Ctl(L"STATIC", L"", 0, IDC_HOTKEYS);

    g_lblLog = Ctl(L"STATIC", L"Журнал событий:", 0, IDC_LBL_LOG);
    SendMessageW(g_lblLog, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    g_log = Ctl(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL, IDC_LOG, WS_EX_CLIENTEDGE);
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
    SendMessageW(g_log, EM_SETLIMITTEXT, 0, 0);
    Btn(L"Сохранить отчёт…", IDC_SAVEREPORT);
}

static void Place(int id, int x, int y, int w, int h) { MoveWindow(GetDlgItem(g_wnd, id), x, y, w, h, TRUE); }
static void PlaceH(HWND hw, int x, int y, int w, int h) { MoveWindow(hw, x, y, w, h, TRUE); }

// Слева — предпросмотр, справа — панель (камера, сведения, режим, снимки/запись, скрытый режим),
// внизу — журнал событий. Размеры пересчитываются при изменении размера окна.
static void Layout() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    const int M = 8, PW = 410, RH = 26;
    int W = rc.right, H = rc.bottom;
    int logH = std::max(110, H / 5);
    int topH = H - logH - M;
    PlaceH(g_status, M, M, W - 2 * M, 28);
    int y0 = M + 28 + M;
    int px = W - PW - M;
    PlaceH(g_view, M, y0, std::max(50, px - 2 * M), topH - y0);

    const int camH = 92, modeH = 92, ctlH = 186, bgH = 86, gap = 6;
    int y = y0, x = px + 10, w = PW - 20;
    PlaceH(g_grpCam, px, y, PW, camH);
    PlaceH(g_camSel, x, y + 22, w - 130, 300);
    Place(IDC_REENUM, x + w - 124, y + 21, 124, RH);
    Place(IDC_CONNECT, x, y + 54, (w - 6) / 2, RH);
    Place(IDC_DISCONNECT, x + (w + 6) / 2, y + 54, (w - 6) / 2, RH);
    y += camH + gap;

    int infoH = std::max(100, topH - y - modeH - ctlH - bgH - 3 * gap);
    PlaceH(g_grpInfo, px, y, PW, infoH);
    PlaceH(g_info, x, y + 22, w, infoH - 30);
    y += infoH + gap;

    const int lw = 160;  // ширина подписей слева от списков
    PlaceH(g_grpMode, px, y, PW, modeH);
    Place(IDC_LBL_RES, x, y + 25, lw, 20);
    PlaceH(g_res, x + lw, y + 21, w - lw, 300);
    Place(IDC_LBL_FPS, x, y + 59, lw, 20);
    PlaceH(g_fps, x + lw, y + 55, w - lw - 110, 300);
    Place(IDC_APPLY, x + w - 104, y + 54, 104, RH);
    y += modeH + gap;

    PlaceH(g_grpCtl, px, y, PW, ctlH);
    Place(IDC_SNAP, x, y + 21, w - 90, RH);
    PlaceH(g_snapFmt, x + w - 84, y + 22, 84, 200);
    Place(IDC_RECSTART, x, y + 53, (w - 6) / 2, RH);
    Place(IDC_RECSTOP, x + (w + 6) / 2, y + 53, (w - 6) / 2, RH);
    Place(IDC_LBL_SPLIT, x, y + 91, 190, 20);
    PlaceH(g_split, x + 190, y + 87, w - 190, 300);
    Place(IDC_LBL_DIR, x, y + 121, w, 18);
    PlaceH(g_dir, x, y + 141, w - 160, 24);
    Place(IDC_BROWSE, x + w - 154, y + 140, 76, RH);
    Place(IDC_OPENDIR, x + w - 74, y + 140, 74, RH);
    y += ctlH + gap;

    PlaceH(g_grpBg, px, y, PW, bgH);
    Place(IDC_TRAY, x, y + 24, 130, 48);
    Place(IDC_HOTKEYS, x + 140, y + 22, w - 140, 58);

    int ly = topH + M;
    PlaceH(g_lblLog, M, ly, 200, 20);
    Place(IDC_SAVEREPORT, W - M - 160, ly - 4, 160, RH);
    PlaceH(g_log, M, ly + 24, W - 2 * M, H - ly - 24 - M);
}

static void UpdateHotkeyLabel() {
    SetWindowTextW(GetDlgItem(g_wnd, IDC_HOTKEYS),
                   g_hotkeysOk ? L"Горячие клавиши (и в трее):\r\nCtrl+Shift+R — начать запись\r\nCtrl+Shift+S — остановить запись"
                               : L"Горячие клавиши не зарегистрированы (заняты другой программой)");
}

// Регистрация двух глобальных горячих клавиш. MOD_NOREPEAT — удержание клавиши не порождает повторов.
static void RegisterHotkeys() {
    BOOL a = RegisterHotKey(g_wnd, HK_REC_START, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'R');
    DWORD ea = a ? 0 : GetLastError();
    BOOL b = RegisterHotKey(g_wnd, HK_REC_STOP, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'S');
    DWORD eb = b ? 0 : GetLastError();
    g_hotkeysOk = a && b;
    Log(a ? std::wstring(L"Горячая клавиша Ctrl+Shift+R (старт записи) зарегистрирована")
          : Fmt(L"Не удалось зарегистрировать Ctrl+Shift+R (код %lu)", ea));
    Log(b ? std::wstring(L"Горячая клавиша Ctrl+Shift+S (стоп записи) зарегистрирована")
          : Fmt(L"Не удалось зарегистрировать Ctrl+Shift+S (код %lu)", eb));
    UpdateHotkeyLabel();
}

// Подписка на уведомления о подключении/отключении устройств видеозахвата (WM_DEVICECHANGE).
// KSCATEGORY_VIDEO_CAMERA — веб-камеры Windows 10+, KSCATEGORY_VIDEO — все видеоустройства (запасной класс).
static void RegisterDevNotify() {
    static const GUID kGuidVideo = {0x6994ad05, 0x93ef, 0x11d0, {0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}};
    const GUID* g[2] = {&kGuidVideoCamera, &kGuidVideo};
    for (int i = 0; i < 2; ++i) {
        DEV_BROADCAST_DEVICEINTERFACE_W f = {};
        f.dbcc_size = sizeof(f);
        f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        f.dbcc_classguid = *g[i];
        g_devNotify[i] = RegisterDeviceNotificationW(g_wnd, &f, DEVICE_NOTIFY_WINDOW_HANDLE);
    }
    Log(g_devNotify[0] || g_devNotify[1] ? std::wstring(L"Подписка на WM_DEVICECHANGE (классы видеоустройств) оформлена")
                                         : Fmt(L"RegisterDeviceNotification: ошибка %lu", GetLastError()));
}

// Применение выбранного режима: камера перезапускается с новым типом медиа.
static void ApplyMode() {
    Camera* c = Active();
    int i = (int)SendMessageW(g_fps, CB_GETCURSEL, 0, 0);
    if (!c || i < 0 || i >= (int)g_fpsItems.size()) return;
    if (c->writer) {
        Log(L"Смена режима во время записи недоступна — сначала остановите запись");
        return;
    }
    EnterCriticalSection(&c->cs);
    c->want = g_fpsItems[i];
    c->haveWant = true;
    LeaveCriticalSection(&c->cs);
    Log(L"Выбран режим: " + g_fpsItems[i].Text() + L" — перезапуск захвата");
    StopCamera(c);
    c->wantRun = true;
    StartCamera(c);
}

static void OnCommand(int id, int code) {
    Camera* c = Active();
    switch (id) {
    case IDC_CAMSEL:
        if (code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(g_camSel, CB_GETCURSEL, 0, 0);
            if (c && c->writer && i != g_active) {
                Log(L"Во время записи сменить камеру нельзя");
                SendMessageW(g_camSel, CB_SETCURSEL, g_active, 0);
            } else
                SetActive(i);
        }
        return;
    case IDC_RES:
        if (code == CBN_SELCHANGE) FillFpsCombo();
        return;
    case IDC_SPLIT:
        if (code == CBN_SELCHANGE) {
            static const DWORD mins[] = {1, 2, 5, 10, 15, 30, 0};
            int i = (int)SendMessageW(g_split, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < (int)_countof(mins)) g_splitSec = mins[i] * 60;
            Log(L"Интервал авторазбиения: " + SplitText());
        }
        return;
    case IDC_FPS:
    case IDC_SNAPFMT:
    case IDC_INFO:
    case IDC_LOG:
    case IDC_DIR:
        return;
    case IDC_REENUM: EnumerateCameras(L"по кнопке"); break;
    case IDC_APPLY: ApplyMode(); break;
    case IDC_CONNECT:
        if (c) {
            c->wantRun = true;
            Log(L"Подключение камеры: " + c->name);
            StartCamera(c);
        }
        break;
    case IDC_DISCONNECT:
        if (c) {
            c->wantRun = false;
            StopCamera(c);
            Log(L"Камера отключена пользователем: " + c->name);
        }
        break;
    case IDC_SNAP: TakeSnapshot(SendMessageW(g_snapFmt, CB_GETCURSEL, 0, 0) == 0); break;
    case IDC_RECSTART: StartRecording(); break;
    case IDC_RECSTOP: StopRecording(L""); break;
    case IDC_BROWSE: BrowseDir(); break;
    case IDC_OPENDIR:
        EnsureDir();
        ShellExecuteW(g_wnd, L"open", g_saveDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case IDC_TRAY: HideToTray(); break;
    case IDC_SAVEREPORT: SaveReport(); break;
    }
    UpdateAll();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
        if (g_hidden) TraySet(NIM_ADD, nullptr, L"Видеонаблюдение — двойной щелчок открывает окно");  // проводник перезапущен
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        g_wnd = hwnd;
        CreateControls();
        Layout();
        Log(L"Программа запущена. Каталог сохранения: " + g_saveDir);
        RegisterHotkeys();
        RegisterDevNotify();
        EnumerateCameras(L"при запуске");
        SetTimer(hwnd, TIMER_REPAINT, 33, nullptr);
        SetTimer(hwnd, TIMER_SECOND, 1000, nullptr);
        SetTimer(hwnd, TIMER_BLINK, 500, nullptr);
        if (!g_demoDir.empty()) SetTimer(hwnd, TIMER_DEMO, 1000, nullptr);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) Layout();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lp)->ptMinTrackSize = {1000, 780};
        return 0;
    case WM_COMMAND:
        OnCommand(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_status) {
            static HBRUSH br = nullptr;
            static COLORREF brColor = 0;
            if (!br || brColor != g_statusColor) {
                if (br) DeleteObject(br);
                br = CreateSolidBrush(g_statusColor);
                brColor = g_statusColor;
            }
            HDC dc = (HDC)wp;
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkColor(dc, g_statusColor);
            return (LRESULT)br;
        }
        break;
    case WM_HOTKEY:
        // Глобальные клавиши приходят и при скрытом окне: окно существует, просто не отображается
        if (wp == HK_REC_START) {
            Log(L"Нажата Ctrl+Shift+R — старт записи");
            StartRecording();
        } else if (wp == HK_REC_STOP) {
            Log(L"Нажата Ctrl+Shift+S — стоп записи");
            StopRecording(L" (горячая клавиша)");
        }
        UpdateAll();
        return 0;
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
            auto hdr = (DEV_BROADCAST_HDR*)lp;
            if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                Log(wp == DBT_DEVICEARRIVAL ? L"WM_DEVICECHANGE: DBT_DEVICEARRIVAL (подключено видеоустройство)"
                                            : L"WM_DEVICECHANGE: DBT_DEVICEREMOVECOMPLETE (видеоустройство отключено)");
                // Одно подключение даёт несколько уведомлений (по одному на класс) — перечисляем один раз, с задержкой
                SetTimer(hwnd, TIMER_DEVCHANGE, 700, nullptr);
            }
        }
        return TRUE;
    case WM_APP_LOG: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        Log(*s);
        return 0;
    }
    case WM_APP_FORMATS:
        if ((int)wp == g_active) FillFormatCombos();
        UpdateAll();
        return 0;
    case WM_APP_CAMSTATE: {
        int i = (int)wp;
        if (i >= 0 && i < (int)g_cams.size()) {
            Camera* c = g_cams[i].get();
            if (c->state != CS_RUNNING && c->state != CS_STARTING && c->thread) {
                // поток захвата завершился сам (ошибка/потеря связи) — освобождаем его дескриптор
                WaitForSingleObject(c->thread, 2000);
                CloseHandle(c->thread);
                c->thread = nullptr;
            }
            if (i == g_active) {
                if (c->state == CS_RUNNING && g_recWanted && !c->writer) StartRecording(c->recTick != 0);
                else if (c->state != CS_RUNNING && c->writer) CloseSegment(c, L" (поток камеры прерван)");
            }
        }
        UpdateAll();
        return 0;
    }
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) RestoreFromTray();
        else if (LOWORD(lp) == WM_RBUTTONUP) {
            HMENU m = CreatePopupMenu();
            Camera* c = Active();
            AppendMenuW(m, MF_STRING, 1, L"Открыть окно");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING | (c && c->writer ? MF_GRAYED : 0), 2, L"Начать запись\tCtrl+Shift+R");
            AppendMenuW(m, MF_STRING | (c && c->writer ? 0 : MF_GRAYED), 3, L"Остановить запись\tCtrl+Shift+S");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 4, L"Выход");
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);
            if (cmd == 1) RestoreFromTray();
            else if (cmd == 2) StartRecording();
            else if (cmd == 3) StopRecording(L" (меню трея)");
            else if (cmd == 4) DestroyWindow(hwnd);
        }
        return 0;
    case WM_TIMER:
        switch (wp) {
        case TIMER_REPAINT: {
            Camera* c = Active();
            if (!g_hidden && c && c->frames != g_painted) InvalidateRect(g_view, nullptr, FALSE);
            break;
        }
        case TIMER_SECOND:
            RollSegmentIfDue();
            UpdateStatus();
            if (!g_hidden) {
                UpdateInfo();
                UpdateButtons();
            }
            break;
        case TIMER_BLINK: TrayBlink(); break;
        case TIMER_DEVCHANGE:
            KillTimer(hwnd, TIMER_DEVCHANGE);
            EnumerateCameras(L"по WM_DEVICECHANGE");
            break;
        case TIMER_DEMO: DemoTick(); break;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_recWanted) StopRecording(L" (выход из программы)");
        for (auto& c : g_cams) StopCamera(c.get());
        UnregisterHotKey(hwnd, HK_REC_START);
        UnregisterHotKey(hwnd, HK_REC_STOP);
        for (auto n : g_devNotify)
            if (n) UnregisterDeviceNotification(n);
        if (g_hidden) TraySet(NIM_DELETE, nullptr, L"");
        Log(L"Программа завершена");
        EnsureDir();  // журнал сеанса всегда сохраняется рядом с материалами
        WriteUtf8File(g_saveDir + L"\\session_log_" + FileStamp() + L".txt", BuildReport());
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
// Точка входа
// ===========================================================================
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    g_inst = inst;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], L"/demo") == 0 && i + 1 < argc) g_demoDir = argv[++i];
    LocalFree(argv);

    // Единственный экземпляр: повторный запуск показывает окно уже работающей программы
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Lab4_CamMonitor_A6");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"CamMonitorWnd", nullptr);
        if (other) PostMessageW(other, WM_APP_TRAY, 0, WM_LBUTTONDBLCLK);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, (L"Media Foundation недоступна: " + HrText(hr)).c_str(), L"Видеонаблюдение", MB_ICONERROR);
        return 1;
    }
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // Каталог по умолчанию: «Видео\CamMonitor» пользователя
    if (!g_demoDir.empty()) g_saveDir = g_demoDir;
    else {
        wchar_t* vid = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &vid))) {
            g_saveDir = std::wstring(vid) + L"\\CamMonitor";
            CoTaskMemFree(vid);
        } else
            g_saveDir = L"C:\\CamMonitor";
    }

    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontBold = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    g_fontView = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontViewBig = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    int is = GetSystemMetrics(SM_CXSMICON);
    g_icoIdle = MakeDotIcon(RGB(120, 140, 160), is);
    g_icoRec = MakeDotIcon(RGB(230, 30, 30), is);
    g_icoApp = MakeDotIcon(RGB(40, 120, 200), GetSystemMetrics(SM_CXICON));

    WNDCLASSEXW vc = {sizeof(vc)};
    vc.style = CS_DBLCLKS;
    vc.lpfnWndProc = ViewProc;
    vc.hInstance = inst;
    vc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    vc.lpszClassName = L"CamViewWnd";
    RegisterClassExW(&vc);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"CamMonitorWnd";
    wc.hIcon = g_icoApp;
    wc.hIconSm = g_icoIdle;
    RegisterClassExW(&wc);

    RECT wa;  // размер окна — по рабочей области экрана, но не больше 1400×960
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int ww = std::min(1400, (int)(wa.right - wa.left)), wh = std::min(960, (int)(wa.bottom - wa.top));
    HWND w = CreateWindowExW(0, L"CamMonitorWnd", L"ЛР №4 — Скрытый режим видеонаблюдения (вариант А6)", WS_OVERLAPPEDWINDOW,
                             wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh,
                             nullptr, nullptr, inst, nullptr);
    if (!w) return 1;
    ShowWindow(w, show);
    UpdateWindow(w);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(w, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_cams.clear();
    MFShutdown();
    CoUninitialize();
    ReleaseMutex(mtx);
    CloseHandle(mtx);
    return 0;
}
