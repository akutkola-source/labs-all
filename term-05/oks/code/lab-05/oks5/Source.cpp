// =====================================================================
//  Лабораторная работа No.5. Детерминированные методы доступа к моноканалу.
//  Упрощённый алгоритм Token Ring (маркерное кольцо).
//  Базовая основа: Лабораторная работа No.3 (кадр, бит-стаффинг, CRC).
//  Вариант: ЧЁТНЫЙ
//    - Номер студента (n): 10
//    - Флаг: двоичное представление (128 + n) = 138 (0x8A)
//    - Бит-стаффинг (вставка '0' после пяти '1')
//    - Циклический код, G(x) = x^8 + x^4 + x^3 + x^2 + 1, исправление 1 ошибки
//
//  Топология: однонаправленное кольцо из трёх станций
//      Станция 1 --> Станция 2 --> Станция 3 --> Станция 1
//  Станции эмулируются в одной программе: у каждой станции своя пара
//  подпрограмм (передающая StationTransmit + принимающая StationRxThread).
//  Линии кольца - виртуальные COM-порты (если найдено 3 пары), иначе
//  внутренние буферы (эмуляция линии связи).
//
//  Маркер:  Flag(8B) | AC(1B) | Flag(8B)
//  Кадр:    Flag(8B) | AC(1B) | DA(4B) | SA(4B) | Data(n+1=11B) | FCS(1B) | FS(1B) | Flag(8B)
//  AC (Access Control) = P P P T M R R R
//      PPP - приоритет (0 - низкий, 1 - высокий), T - 0 маркер / 1 кадр,
//      M   - бит монитора,  RRR - резервируемый приоритет
//  FS (Frame Status) = 0 0 0 0 0 0 A C  (A - адрес распознан, C - кадр скопирован)
// =====================================================================

#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <clocale>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

// --- ПАРАМЕТРЫ ВАРИАНТА ---
constexpr int STUDENT_N = 10;
constexpr size_t DATA_PAYLOAD_LEN = STUDENT_N + 1;        // 11 байт - длина блока
constexpr size_t FCS_LEN = 1;
constexpr uint8_t FLAG_BYTE = (uint8_t)(128 + STUDENT_N); // 0x8A
constexpr int STATIONS = 3;

// Порождающий полином G(x) = x^8 + x^4 + x^3 + x^2 + 1
static const std::vector<uint8_t> G_POLY = { 1, 0, 0, 0, 1, 1, 1, 0, 1 };

// Поля AC
constexpr uint8_t AC_T = 0x10;  // 1 - кадр с данными, 0 - маркер
constexpr uint8_t AC_M = 0x08;  // бит монитора
static uint8_t MakeAC(int p, bool t, bool m, int r) { return (uint8_t)(((p & 7) << 5) | (t ? AC_T : 0) | (m ? AC_M : 0) | (r & 7)); }
static int AcP(uint8_t ac) { return (ac >> 5) & 7; }
static int AcR(uint8_t ac) { return ac & 7; }
// Поля FS
constexpr uint8_t FS_A = 0x02;
constexpr uint8_t FS_C = 0x01;

// Длина тела кадра (без флагов): AC + DA + SA + Data + FCS + FS
constexpr size_t FRAME_BODY_LEN = 1 + 4 + 4 + DATA_PAYLOAD_LEN + FCS_LEN + 1; // 22 байта

// Пользовательские оконные сообщения
#define WM_APP_RX_TEXT   (WM_APP + 1)   // wParam = номер станции, lParam = wchar_t*
#define WM_APP_DEBUG     (WM_APP + 3)
#define WM_APP_STATUS    (WM_APP + 4)
#define WM_APP_RINGPOS   (WM_APP + 5)   // wParam = откуда, lParam = куда
#define WM_APP_QUEUE     (WM_APP + 6)   // wParam = номер станции

#define ID_START_BTN     201
#define ID_STOP_BTN      202
#define ID_LOSE_BTN      203
#define ID_ORPHAN_BTN    204
#define ID_DELAY_COMBO   205
#define ID_NOISE_CHECK   206
#define ID_ST_ADDR       300   // + 10*i
#define ID_ST_PRIO       301
#define ID_ST_DEST       302
#define ID_ST_MONITOR    303

// =====================================================================
//  ЛИНИЯ СВЯЗИ МЕЖДУ СОСЕДНИМИ СТАНЦИЯМИ
// =====================================================================

struct Line {
    bool   useCom = false;
    HANDLE hTx = INVALID_HANDLE_VALUE;   // в этот порт пишет станция i
    HANDLE hRx = INVALID_HANDLE_VALUE;   // из этого порта читает станция i+1
    int    txNum = -1, rxNum = -1;
    // Эмуляция линии
    CRITICAL_SECTION cs;
    std::deque<uint8_t> q;
    HANDLE hEvent = NULL;
};

static Line g_lines[STATIONS];   // линия i: станция i -> станция (i+1)%3

// =====================================================================
//  СТАНЦИЯ
// =====================================================================

struct OutBlock {
    uint32_t dest;
    int prio;
    std::vector<uint8_t> data;
};

struct Station {
    int idx = 0;
    volatile LONG addr = 0;        // адрес станции (настраивается)
    volatile LONG prio = 0;        // приоритет станции (0/1)
    volatile LONG dest = 0;        // адрес назначения для новых сообщений
    CRITICAL_SECTION cs;           // защита очереди и состояния
    CRITICAL_SECTION txLock;       // защита передатчика
    std::deque<OutBlock> queue;    // очередь блоков на передачу
    bool waitingReturn = false;    // станция передала кадр и ждёт его возврата
    int  capturedP = 0;            // приоритет захваченного маркера
    int  stackedTo = -1;           // станция повысила приоритет маркера до stackedTo
    int  stackedFrom = 0;          // ... со значения stackedFrom
    bool rxLineStart[256];         // для вывода: начало новой строки от адреса
    HANDLE hThread = NULL;

    HWND hGroup = NULL, hLblAddr = NULL, hAddr = NULL, hLblPrio = NULL, hPrio = NULL;
    HWND hLblDest = NULL, hDest = NULL, hMonitor = NULL, hLblIn = NULL, hInput = NULL;
    HWND hLblOut = NULL, hOutput = NULL, hQueue = NULL;
};

static Station g_st[STATIONS];

static volatile LONG g_running = 0;
static volatile LONG g_monitor = 0;          // номер станции-монитора (0..2), -1 - нет
static volatile LONG g_hopDelay = 500;       // задержка на станции, мс
static volatile LONG g_noise = 1;            // внесение помех 35%
static volatile LONG g_loseToken = 0;        // имитация потери маркера
static volatile LONG g_makeOrphan = 0;       // имитация "кадра-сироты"
static volatile LONG g_monitorSeenTick = 0;  // когда монитор последний раз видел маркер/кадр
static volatile BOOL g_stopThreads = FALSE;
static bool g_useCom = false;
static DWORD g_startTick = 0;

static HFONT g_hAppFont = NULL, g_hMonoFont = NULL, g_hBoldFont = NULL;
static HWND g_hWndMain = NULL;
static HWND g_hGrpRing = NULL, g_hStartBtn = NULL, g_hStopBtn = NULL, g_hLoseBtn = NULL, g_hOrphanBtn = NULL;
static HWND g_hLblDelay = NULL, g_hDelayCombo = NULL, g_hNoiseCheck = NULL, g_hLinesInfo = NULL, g_hRingPos = NULL;
static HWND g_hLblDebug = NULL, g_hDebugEdit = NULL, g_hLblStatus = NULL, g_hStatusEdit = NULL;
static WNDPROC g_oldInputEditProc = NULL;

static void LogDebug(const wchar_t* fmt, ...);
static void LogStatus(const std::wstring& line);
static void StationTransmit(int i, const std::vector<uint8_t>& body, bool isToken);
static void MonitorGenerateToken(const wchar_t* reason);

// =====================================================================
//  ЦИКЛИЧЕСКИЙ КОД (из ЛР No.3): ДЕЛЕНИЕ ПОЛИНОМОВ СТОЛБИКОМ
// =====================================================================

static std::vector<uint8_t> PolyDivRemainder(const std::vector<uint8_t>& dividend, const std::vector<uint8_t>& divisor)
{
    std::vector<uint8_t> rem = dividend;
    size_t degG = divisor.size() - 1;
    for (size_t i = 0; i + degG < rem.size(); ++i) {
        if (rem[i] == 1) {
            for (size_t j = 0; j <= degG; ++j) rem[i + j] ^= divisor[j];
        }
    }
    return std::vector<uint8_t>(rem.end() - degG, rem.end());
}

static std::vector<uint8_t> BytesToBits(const std::vector<uint8_t>& bytes)
{
    std::vector<uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (uint8_t b : bytes)
        for (int i = 7; i >= 0; --i) bits.push_back((b >> i) & 1);
    return bits;
}

static uint8_t BitsToByte(const std::vector<uint8_t>& bits)
{
    uint8_t res = 0;
    for (size_t i = 0; i < bits.size() && i < 8; ++i) res = (res << 1) | (bits[i] ? 1 : 0);
    return res;
}

static uint8_t ComputeFCS(const std::vector<uint8_t>& dataField)
{
    std::vector<uint8_t> bits = BytesToBits(dataField);
    bits.insert(bits.end(), 8, 0); // умножение на x^8
    return BitsToByte(PolyDivRemainder(bits, G_POLY));
}

static std::vector<std::vector<uint8_t>> g_syndromeTable;

static void InitSyndromeTable()
{
    const size_t totalBits = (DATA_PAYLOAD_LEN + FCS_LEN) * 8; // 96 бит
    g_syndromeTable.resize(totalBits);
    for (size_t i = 0; i < totalBits; ++i) {
        std::vector<uint8_t> errPattern(totalBits, 0);
        errPattern[i] = 1;
        g_syndromeTable[i] = PolyDivRemainder(errPattern, G_POLY);
    }
}

// Возвращает true, если данные целы (или ошибка исправлена)
static bool VerifyAndCorrect(int st, std::vector<uint8_t>& dataField, uint8_t rxFCS)
{
    std::vector<uint8_t> codewordBits = BytesToBits(dataField);
    for (int i = 7; i >= 0; --i) codewordBits.push_back((rxFCS >> i) & 1);
    std::vector<uint8_t> syndrome = PolyDivRemainder(codewordBits, G_POLY);

    if (std::all_of(syndrome.begin(), syndrome.end(), [](uint8_t b) { return b == 0; })) {
        LogDebug(L"      [CRC] Ст.%d: синдром = 0, ошибок нет", st + 1);
        return true;
    }
    for (size_t i = 0; i < g_syndromeTable.size(); ++i) {
        if (g_syndromeTable[i] == syndrome) {
            if (i < DATA_PAYLOAD_LEN * 8) {
                int byteIdx = (int)i / 8, bitIdx = 7 - (int)(i % 8);
                dataField[byteIdx] ^= (1 << bitIdx);
                LogDebug(L"      [CRC] Ст.%d: ошибка ОБНАРУЖЕНА и ИСПРАВЛЕНА (бит %d: байт %d, бит %d)",
                    st + 1, (int)i, byteIdx, bitIdx);
            }
            else {
                LogDebug(L"      [CRC] Ст.%d: ошибка в поле FCS, данные целы", st + 1);
            }
            return true;
        }
    }
    LogDebug(L"      [CRC] Ст.%d: неисправимая ошибка!", st + 1);
    return false;
}

// =====================================================================
//  БИТ-СТАФФИНГ И ДЕСТАФФИНГ (из ЛР No.2/No.3)
// =====================================================================

static std::vector<uint8_t> BitStuffing(const std::vector<uint8_t>& rawData, std::wstring& outVisualBits)
{
    std::vector<bool> bits;
    outVisualBits.clear();
    int consecutiveOnes = 0;
    for (uint8_t b : rawData) {
        for (int i = 7; i >= 0; --i) {
            bool bit = (b >> i) & 1;
            bits.push_back(bit);
            outVisualBits += bit ? L"1" : L"0";
            if (bit) {
                if (++consecutiveOnes == 5) {
                    bits.push_back(false);
                    outVisualBits += L"[0]";
                    consecutiveOnes = 0;
                }
            }
            else consecutiveOnes = 0;
        }
    }
    std::vector<uint8_t> stuffedBytes;
    uint8_t curByte = 0;
    int bitCount = 0;
    for (bool b : bits) {
        curByte = (curByte << 1) | (b ? 1 : 0);
        if (++bitCount == 8) { stuffedBytes.push_back(curByte); curByte = 0; bitCount = 0; }
    }
    if (bitCount > 0) { curByte <<= (8 - bitCount); stuffedBytes.push_back(curByte); }
    return stuffedBytes;
}

static std::vector<uint8_t> BitDestuffing(const std::vector<uint8_t>& stuffedBytes)
{
    std::vector<bool> bits;
    int consecutiveOnes = 0;
    for (uint8_t b : stuffedBytes) {
        for (int i = 7; i >= 0; --i) {
            bool bit = (b >> i) & 1;
            if (consecutiveOnes == 5) {
                consecutiveOnes = 0;
                if (!bit) continue;
            }
            bits.push_back(bit);
            if (bit) consecutiveOnes++; else consecutiveOnes = 0;
        }
    }
    std::vector<uint8_t> destuffedBytes;
    uint8_t curByte = 0;
    int bitCount = 0;
    for (bool b : bits) {
        curByte = (curByte << 1) | (b ? 1 : 0);
        if (++bitCount == 8) { destuffedBytes.push_back(curByte); curByte = 0; bitCount = 0; }
    }
    return destuffedBytes;
}

// =====================================================================
//  РАЗБОР / СБОРКА ТЕЛА КАДРА
// =====================================================================

struct Frame {
    uint8_t ac = 0;
    uint32_t da = 0, sa = 0;
    std::vector<uint8_t> data;
    uint8_t fcs = 0, fs = 0;
};

static void PutU32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back((x >> 24) & 0xFF); v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 8) & 0xFF);  v.push_back(x & 0xFF);
}

static uint32_t GetU32(const std::vector<uint8_t>& v, size_t pos)
{
    return ((uint32_t)v[pos] << 24) | ((uint32_t)v[pos + 1] << 16) | ((uint32_t)v[pos + 2] << 8) | v[pos + 3];
}

static std::vector<uint8_t> BuildFrameBody(const Frame& f)
{
    std::vector<uint8_t> body;
    body.push_back(f.ac);
    PutU32(body, f.da);
    PutU32(body, f.sa);
    body.insert(body.end(), f.data.begin(), f.data.end());
    body.push_back(f.fcs);
    body.push_back(f.fs);
    return body;
}

static Frame ParseFrameBody(const std::vector<uint8_t>& raw)
{
    Frame f;
    f.ac = raw[0];
    f.da = GetU32(raw, 1);
    f.sa = GetU32(raw, 5);
    f.data.assign(raw.begin() + 9, raw.begin() + 9 + DATA_PAYLOAD_LEN);
    f.fcs = raw[9 + DATA_PAYLOAD_LEN];
    f.fs = raw[10 + DATA_PAYLOAD_LEN];
    return f;
}

static std::wstring DataToText(const std::vector<uint8_t>& data)
{
    std::string s;
    for (uint8_t c : data) if (c != 0) s.push_back((char)c);
    if (s.empty()) return L"";
    int wlen = MultiByteToWideChar(1251, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(1251, 0, s.data(), (int)s.size(), &w[0], wlen);
    return w;
}

static std::wstring Printable(const std::wstring& s)
{
    std::wstring r;
    for (wchar_t c : s) r += (c == L'\n') ? L'\x21B5' : c;   // ↵ - конец сообщения
    return r;
}

static int NextSt(int i) { return (i + 1) % STATIONS; }

// =====================================================================
//  ЛИНИИ: ПЕРЕДАЧА И ПРИЁМ БАЙТОВ
// =====================================================================

static void LineWrite(Line& ln, const std::vector<uint8_t>& bytes)
{
    if (ln.useCom) {
        DWORD written = 0;
        WriteFile(ln.hTx, bytes.data(), (DWORD)bytes.size(), &written, NULL);
    }
    else {
        EnterCriticalSection(&ln.cs);
        ln.q.insert(ln.q.end(), bytes.begin(), bytes.end());
        LeaveCriticalSection(&ln.cs);
        SetEvent(ln.hEvent);
    }
}

static int LineRead(Line& ln, uint8_t* buf, int maxLen)
{
    if (ln.useCom) {
        DWORD read = 0;
        if (!ReadFile(ln.hRx, buf, maxLen, &read, NULL) || read == 0) { Sleep(5); return 0; }
        return (int)read;
    }
    WaitForSingleObject(ln.hEvent, 50);
    EnterCriticalSection(&ln.cs);
    int n = 0;
    while (n < maxLen && !ln.q.empty()) { buf[n++] = ln.q.front(); ln.q.pop_front(); }
    LeaveCriticalSection(&ln.cs);
    return n;
}

// =====================================================================
//  ПЕРЕДАЮЩАЯ ПОДПРОГРАММА СТАНЦИИ
// =====================================================================

static void StationTransmit(int i, const std::vector<uint8_t>& body, bool isToken)
{
    std::wstring visualBits;
    std::vector<uint8_t> stuffed = BitStuffing(body, visualBits);

    std::vector<uint8_t> frame;
    frame.insert(frame.end(), 8, FLAG_BYTE);          // SD - начальный флаг
    frame.insert(frame.end(), stuffed.begin(), stuffed.end());
    frame.insert(frame.end(), 8, FLAG_BYTE);          // ED - конечный флаг

    EnterCriticalSection(&g_st[i].txLock);
    LineWrite(g_lines[i], frame);
    LeaveCriticalSection(&g_st[i].txLock);

    PostMessageW(g_hWndMain, WM_APP_RINGPOS, (WPARAM)i, (LPARAM)NextSt(i));
    (void)isToken;
}

static void SendToken(int i, int p, int r)
{
    std::vector<uint8_t> body = { MakeAC(p, false, false, r) };
    LogDebug(L"Ст.%d → Ст.%d : МАРКЕР   [P=%d R=%d]", i + 1, NextSt(i) + 1, p, r);
    StationTransmit(i, body, true);
}

static void ForwardFrame(int i, const Frame& f)
{
    LogDebug(L"Ст.%d → Ст.%d : КАДР %u→%u [P=%d M=%d R=%d FS=A%dC%d] \"%ls\"",
        i + 1, NextSt(i) + 1, f.sa, f.da, AcP(f.ac), (f.ac & AC_M) ? 1 : 0, AcR(f.ac),
        (f.fs & FS_A) ? 1 : 0, (f.fs & FS_C) ? 1 : 0, Printable(DataToText(f.data)).c_str());
    StationTransmit(i, BuildFrameBody(f), false);
}

static void PostQueueUpdate(int i) { PostMessageW(g_hWndMain, WM_APP_QUEUE, (WPARAM)i, 0); }

// Станция захватила маркер и передаёт свой кадр
static void TransmitOwnFrame(int i, const OutBlock& blk, int tokenP, int tokenR)
{
    Station& s = g_st[i];
    Frame f;
    f.ac = MakeAC(blk.prio, true, false, 0);
    f.da = blk.dest;
    f.sa = (uint32_t)s.addr;
    f.data.assign(DATA_PAYLOAD_LEN, 0);
    memcpy(f.data.data(), blk.data.data(), min(blk.data.size(), DATA_PAYLOAD_LEN));
    f.fcs = ComputeFCS(f.data);
    f.fs = 0;
    (void)tokenR;

    // Окно состояния: 1 кадр = 1 строка (структура, FCS, биты стаффинга)
    std::wstring visualBits;
    BitStuffing(BuildFrameBody(f), visualBits);
    wchar_t head[512];
    swprintf_s(head, L"Ст.%d: [FLAG 0x%02X x8] [AC P=%d T=1 M=0 R=0] [DA=%u] [SA=%u] [DATA %dB \"%ls\"] [=== FCS 0x%02X ===] [FS=00] [FLAG x8] | ",
        i + 1, FLAG_BYTE, blk.prio, f.da, f.sa, (int)DATA_PAYLOAD_LEN, Printable(DataToText(f.data)).c_str(), f.fcs);
    LogStatus(std::wstring(head) + visualBits);

    LogDebug(L"   >>> Ст.%d ЗАХВАТИЛА маркер (P=%d), передаёт кадр приоритета %d для адреса %u",
        i + 1, tokenP, blk.prio, blk.dest);
    ForwardFrame(i, f);
}

// =====================================================================
//  ЛОГИКА TOKEN RING: ОБРАБОТКА ПРИНЯТОГО МАРКЕРА / КАДРА
// =====================================================================

static void OnTokenReceived(int i, uint8_t ac)
{
    Station& s = g_st[i];
    int p = AcP(ac), r = AcR(ac);

    if (InterlockedExchange(&g_loseToken, 0)) {
        LogDebug(L"   !!! Маркер ПОТЕРЯН на Ст.%d (имитация сбоя)", i + 1);
        return;
    }

    EnterCriticalSection(&s.cs);
    if (InterlockedExchange(&g_makeOrphan, 0)) {
        LeaveCriticalSection(&s.cs);
        Frame f;
        f.ac = MakeAC(0, true, false, 0);
        f.da = 0; f.sa = 0;                     // адресов 0 в кольце нет - кадр никто не снимет
        f.data.assign(DATA_PAYLOAD_LEN, 0);
        const char* txt = "ORPHAN";
        memcpy(f.data.data(), txt, strlen(txt));
        f.fcs = ComputeFCS(f.data);
        LogDebug(L"   !!! Ст.%d передала кадр и \"вышла из строя\" - в кольце кадр-сирота", i + 1);
        ForwardFrame(i, f);
        return;
    }

    // 1. Есть кадр с приоритетом >= P - захват маркера
    if (!s.waitingReturn && !s.queue.empty() && s.queue.front().prio >= p) {
        OutBlock blk = s.queue.front();
        s.waitingReturn = true;
        s.capturedP = p;
        LeaveCriticalSection(&s.cs);
        TransmitOwnFrame(i, blk, p, r);
        return;
    }

    // 2. Станция ранее повысила приоритет маркера - понижает его обратно
    if (s.stackedTo >= 0 && p == s.stackedTo && (s.queue.empty() || s.queue.front().prio < p)) {
        int newP = max(r, s.stackedFrom);
        if (newP < p) {
            LogDebug(L"   <<< Ст.%d понижает приоритет маркера %d → %d", i + 1, p, newP);
            p = newP;
            s.stackedTo = -1;
        }
        r = 0;                                  // высокий приоритет ещё нужен - маркер идёт дальше
    }
    // 3. Резервирование приоритета
    else if (!s.queue.empty() && s.queue.front().prio > r) {
        r = s.queue.front().prio;
        LogDebug(L"   ... Ст.%d резервирует приоритет R=%d", i + 1, r);
    }
    LeaveCriticalSection(&s.cs);
    SendToken(i, p, r);
}

static void OnFrameReceived(int i, Frame f)
{
    Station& s = g_st[i];
    uint32_t myAddr = (uint32_t)s.addr;

    // --- Кадр вернулся к отправителю: удаление из кольца, выпуск маркера ---
    EnterCriticalSection(&s.cs);
    if (s.waitingReturn && f.sa == myAddr) {
        bool ok = (f.fs & FS_A) && (f.fs & FS_C);
        if (ok || !(f.fs & FS_A)) {
            if (!s.queue.empty()) s.queue.pop_front();
        }
        s.waitingReturn = false;
        int ownNext = s.queue.empty() ? 0 : s.queue.front().prio;   // свой следующий блок
        int r = AcR(f.ac);
        int newP = s.capturedP;
        if (r > s.capturedP) {                      // кто-то зарезервировал более высокий приоритет
            newP = r;
            s.stackedTo = r;
            s.stackedFrom = s.capturedP;
        }
        LeaveCriticalSection(&s.cs);
        PostQueueUpdate(i);
        if (ok)
            LogDebug(L"   <<< Кадр вернулся к Ст.%d: A=1 C=1 - доставлен. Кадр снят с кольца", i + 1);
        else if (f.fs & FS_A)
            LogDebug(L"   <<< Кадр вернулся к Ст.%d: A=1 C=0 - получатель не смог скопировать, повтор", i + 1);
        else
            LogDebug(L"   <<< Кадр вернулся к Ст.%d: A=0 - адрес %u в кольце не найден, кадр снят", i + 1, f.da);
        SendToken(i, newP, ownNext);                // освобождение маркера
        return;
    }
    LeaveCriticalSection(&s.cs);

    // --- Функция монитора: предотвращение бесконечной циркуляции кадра ---
    if (g_monitor == i) {
        if (f.ac & AC_M) {
            LogDebug(L"   ### МОНИТОР Ст.%d: кадр %u→%u прошёл повторно (M=1) - кадр-сирота УДАЛЁН", i + 1, f.sa, f.da);
            MonitorGenerateToken(L"после удаления кадра-сироты");
            return;
        }
        f.ac |= AC_M;
    }

    // --- Кадр адресован этой станции: копирование данных ---
    if (f.da == myAddr) {
        std::vector<uint8_t> data = f.data;
        if (g_noise && (rand() % 100) < 35) {
            int errByte = rand() % DATA_PAYLOAD_LEN, errBit = rand() % 8;
            data[errByte] ^= (1 << errBit);
            LogDebug(L"      [ПОМЕХА 35%%] Ст.%d: искажён бит %d в байте Data[%d]", i + 1, errBit, errByte);
        }
        bool ok = VerifyAndCorrect(i, data, f.fcs);
        f.fs |= FS_A;
        if (ok) {
            f.fs |= FS_C;
            std::wstring text = DataToText(data);
            LogDebug(L"   +++ Ст.%d (адр %u) СКОПИРОВАЛА кадр от адр %u: \"%ls\"", i + 1, myAddr, f.sa, Printable(text).c_str());
            if (!text.empty()) {
                std::wstring out;
                uint8_t src = (uint8_t)f.sa;
                for (wchar_t c : text) {
                    if (s.rxLineStart[src]) {
                        wchar_t pre[32]; swprintf_s(pre, L"[от %u] ", f.sa);
                        out += pre;
                        s.rxLineStart[src] = false;
                    }
                    if (c == L'\n') { out += L"\r\n"; s.rxLineStart[src] = true; }
                    else out += c;
                }
                PostMessageW(g_hWndMain, WM_APP_RX_TEXT, (WPARAM)i, (LPARAM)_wcsdup(out.c_str()));
            }
        }
    }

    // --- Транзитный кадр: возможно резервирование приоритета ---
    EnterCriticalSection(&s.cs);
    if (!s.queue.empty() && !s.waitingReturn && s.queue.front().prio > AcR(f.ac)) {
        f.ac = (uint8_t)((f.ac & ~7) | s.queue.front().prio);
        LogDebug(L"   ... Ст.%d резервирует приоритет R=%d в проходящем кадре", i + 1, AcR(f.ac));
    }
    LeaveCriticalSection(&s.cs);
    ForwardFrame(i, f);
}

// =====================================================================
//  ПРИНИМАЮЩАЯ ПОДПРОГРАММА СТАНЦИИ (отдельный поток на каждую станцию)
// =====================================================================

static DWORD WINAPI StationRxThread(LPVOID param)
{
    int i = (int)(INT_PTR)param;
    Line& in = g_lines[(i + STATIONS - 1) % STATIONS];   // вход станции i - линия от предыдущей
    std::vector<uint8_t> rxBuffer;
    uint8_t buf[256];

    while (!g_stopThreads) {
        int n = LineRead(in, buf, sizeof(buf));
        for (int k = 0; k < n; ++k) {
            rxBuffer.push_back(buf[k]);
            if (rxBuffer.size() < 8) continue;

            bool isFlag = true;
            for (size_t j = rxBuffer.size() - 8; j < rxBuffer.size(); ++j)
                if (rxBuffer[j] != FLAG_BYTE) { isFlag = false; break; }
            if (!isFlag) continue;

            std::vector<uint8_t> candidate(rxBuffer.begin(), rxBuffer.end() - 8);
            rxBuffer.clear();
            // Отбрасываем начальные флаги (AC никогда не равен 0x8A)
            size_t start = 0;
            while (start < candidate.size() && candidate[start] == FLAG_BYTE) ++start;
            candidate.erase(candidate.begin(), candidate.begin() + start);
            if (candidate.empty()) continue;

            std::vector<uint8_t> raw = BitDestuffing(candidate);
            if (raw.empty()) continue;

            if (i == g_monitor) InterlockedExchange(&g_monitorSeenTick, (LONG)GetTickCount());
            if (!g_running) continue;              // кольцо остановлено - всё поглощается

            Sleep((DWORD)g_hopDelay);              // время обработки на станции (наглядность)
            if (!g_running) continue;

            if ((raw[0] & AC_T) == 0) {
                OnTokenReceived(i, raw[0]);
            }
            else if (raw.size() >= FRAME_BODY_LEN) {
                raw.resize(FRAME_BODY_LEN);
                OnFrameReceived(i, ParseFrameBody(raw));
            }
        }
    }
    return 0;
}

// =====================================================================
//  СТАНЦИЯ-МОНИТОР
// =====================================================================

static void MonitorGenerateToken(const wchar_t* reason)
{
    int m = g_monitor;
    if (m < 0) return;
    for (int k = 0; k < STATIONS; ++k) {                       // сброс состояния кольца
        EnterCriticalSection(&g_st[k].cs);
        g_st[k].waitingReturn = false;
        g_st[k].stackedTo = -1;
        LeaveCriticalSection(&g_st[k].cs);
    }
    InterlockedExchange(&g_monitorSeenTick, (LONG)GetTickCount());
    LogDebug(L"   ### МОНИТОР Ст.%d генерирует новый маркер (%ls)", m + 1, reason);
    SendToken(m, 0, 0);
}

// Контроль маркера: вызывается по таймеру в GUI-потоке
static void MonitorCheck()
{
    if (!g_running || g_monitor < 0) return;
    DWORD timeout = (DWORD)g_hopDelay * STATIONS * 2 + 1500;
    DWORD since = GetTickCount() - (DWORD)g_monitorSeenTick;
    if (since > timeout) {
        wchar_t why[96];
        swprintf_s(why, L"маркер не появлялся %u мс - маркер утерян", since);
        MonitorGenerateToken(why);
    }
}

// =====================================================================
//  ПОСТАНОВКА СООБЩЕНИЯ В ОЧЕРЕДЬ (блоки фиксированной длины n+1)
// =====================================================================

static void EnqueueText(int i, const std::wstring& text)
{
    Station& s = g_st[i];
    std::string ansi;
    int len = WideCharToMultiByte(1251, 0, text.c_str(), (int)text.size(), NULL, 0, NULL, NULL);
    if (len > 0) {
        ansi.resize(len);
        WideCharToMultiByte(1251, 0, text.c_str(), (int)text.size(), &ansi[0], len, NULL, NULL);
    }
    ansi.push_back('\n');                    // признак конца сообщения
    std::vector<uint8_t> bytes(ansi.begin(), ansi.end());

    int blocks = 0;
    EnterCriticalSection(&s.cs);
    for (size_t off = 0; off < bytes.size(); off += DATA_PAYLOAD_LEN) {
        size_t cur = min(DATA_PAYLOAD_LEN, bytes.size() - off);
        OutBlock b;
        b.dest = (uint32_t)s.dest;
        b.prio = (int)s.prio;
        b.data.assign(bytes.begin() + off, bytes.begin() + off + cur);
        s.queue.push_back(b);
        ++blocks;
    }
    LeaveCriticalSection(&s.cs);
    LogDebug(L"[Ст.%d] Сообщение \"%ls\" разбито на %d блок(а) по %d байт, адрес назначения %d, приоритет %ls",
        i + 1, text.c_str(), blocks, (int)DATA_PAYLOAD_LEN, (int)s.dest, s.prio ? L"ВЫСОКИЙ" : L"низкий");
    PostQueueUpdate(i);
}

// =====================================================================
//  COM-ПОРТЫ
// =====================================================================

static BOOL ConfigureCommPort(HANDLE h, DWORD baud)
{
    DCB dcb; ZeroMemory(&dcb, sizeof(dcb)); dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) return FALSE;
    dcb.BaudRate = baud; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE; dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) return FALSE;
    COMMTIMEOUTS to = { MAXDWORD, 0, 0, 0, 100 };
    if (!SetCommTimeouts(h, &to)) return FALSE;
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return TRUE;
}

static HANDLE TryOpenPort(int num, DWORD baud)
{
    wchar_t path[32]; swprintf_s(path, L"\\\\.\\COM%d", num);
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    if (!ConfigureCommPort(h, baud)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
    return h;
}

static std::vector<int> EnumerateAvailablePorts()
{
    std::vector<int> result;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return result;
    wchar_t val[256], data[256];
    DWORD idx = 0;
    for (;;) {
        DWORD valLen = 256, dataLen = sizeof(data), type;
        if (RegEnumValueW(hKey, idx++, val, &valLen, NULL, &type, (LPBYTE)data, &dataLen) != ERROR_SUCCESS) break;
        int num = 0;
        if (swscanf_s(data, L"COM%d", &num) == 1) result.push_back(num);
    }
    RegCloseKey(hKey);
    std::sort(result.begin(), result.end());
    return result;
}

// Пары виртуальных портов (com0com): (COM a, COM a+1). Линия i: станция i пишет в
// первый порт пары, станция i+1 читает из второго.
static void InitLines()
{
    for (auto& ln : g_lines) {
        InitializeCriticalSection(&ln.cs);
        ln.hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    }
    std::vector<int> avail = EnumerateAvailablePorts();
    bool ok = avail.size() >= 2 * STATIONS;
    std::vector<HANDLE> opened;
    for (int i = 0; ok && i < STATIONS; ++i) {
        HANDLE hTx = TryOpenPort(avail[2 * i], CBR_9600);
        HANDLE hRx = (hTx != INVALID_HANDLE_VALUE) ? TryOpenPort(avail[2 * i + 1], CBR_9600) : INVALID_HANDLE_VALUE;
        if (hTx == INVALID_HANDLE_VALUE || hRx == INVALID_HANDLE_VALUE) {
            if (hTx != INVALID_HANDLE_VALUE) CloseHandle(hTx);
            ok = false;
            break;
        }
        g_lines[i].hTx = hTx; g_lines[i].txNum = avail[2 * i];
        g_lines[i].hRx = hRx; g_lines[i].rxNum = avail[2 * i + 1];
        opened.push_back(hTx); opened.push_back(hRx);
    }
    if (!ok) {
        for (HANDLE h : opened) CloseHandle(h);
        for (auto& ln : g_lines) { ln.hTx = ln.hRx = INVALID_HANDLE_VALUE; ln.useCom = false; }
        g_useCom = false;
    }
    else {
        for (auto& ln : g_lines) ln.useCom = true;
        g_useCom = true;
    }
}

static void CloseLines()
{
    for (auto& ln : g_lines) {
        if (ln.hTx != INVALID_HANDLE_VALUE) CloseHandle(ln.hTx);
        if (ln.hRx != INVALID_HANDLE_VALUE) CloseHandle(ln.hRx);
        ln.hTx = ln.hRx = INVALID_HANDLE_VALUE;
    }
}

// =====================================================================
//  ЖУРНАЛЫ
// =====================================================================

static void LogDebug(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    DWORD t = GetTickCount() - g_startTick;
    wchar_t line[1100];
    _snwprintf_s(line, _TRUNCATE, L"[%3u.%03u] %ls", t / 1000, t % 1000, buf);
    if (g_hWndMain) PostMessageW(g_hWndMain, WM_APP_DEBUG, 0, (LPARAM)_wcsdup(line));
}

static void LogStatus(const std::wstring& line)
{
    if (g_hWndMain) PostMessageW(g_hWndMain, WM_APP_STATUS, 0, (LPARAM)_wcsdup(line.c_str()));
}

static void AppendEdit(HWND h, const wchar_t* text, bool newline)
{
    int len = GetWindowTextLengthW(h);
    if (len > 400000) {                         // ограничение объёма журнала
        SendMessageW(h, EM_SETSEL, 0, 200000);
        SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(h);
    }
    SendMessageW(h, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)text);
    if (newline) SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

// =====================================================================
//  GUI
// =====================================================================

static void UpdateQueueLabel(int i)
{
    Station& s = g_st[i];
    EnterCriticalSection(&s.cs);
    size_t n = s.queue.size();
    bool w = s.waitingReturn;
    LeaveCriticalSection(&s.cs);
    wchar_t buf[96];
    swprintf_s(buf, L"Блоков в очереди: %u%ls", (unsigned)n, w ? L"  (ожидание возврата кадра)" : L"");
    SetWindowTextW(s.hQueue, buf);
}

static void UpdateRingPos(int from, int to)
{
    std::wstring s = L"Маркер/кадр:   ";
    for (int k = 0; k < STATIONS; ++k) {
        wchar_t b[48];
        if (k == from)      swprintf_s(b, L"[Ст.%d%ls]", k + 1, g_monitor == k ? L" M" : L"");
        else                swprintf_s(b, L" Ст.%d%ls ", k + 1, g_monitor == k ? L" M" : L"");
        s += b;
        s += (k == from && to == NextSt(k)) ? L"  ==>  " : L"  →  ";
    }
    s += L"(Ст.1)";
    SetWindowTextW(g_hRingPos, s.c_str());
}

static void ReadStationParams(int i)
{
    Station& s = g_st[i];
    wchar_t buf[16];
    GetWindowTextW(s.hAddr, buf, 16);
    int a = _wtoi(buf);
    if (a >= 1 && a <= 255) InterlockedExchange(&s.addr, a);
    GetWindowTextW(s.hDest, buf, 16);
    int d = _wtoi(buf);
    if (d >= 0 && d <= 255) InterlockedExchange(&s.dest, d);
    int p = (int)SendMessageW(s.hPrio, CB_GETCURSEL, 0, 0);
    InterlockedExchange(&s.prio, p == 1 ? 1 : 0);
}

static LRESULT CALLBACK InputEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        int i = (int)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        int len = GetWindowTextLengthW(hwnd);
        if (len > 0) {
            std::vector<wchar_t> buf(len + 1, L'\0');
            GetWindowTextW(hwnd, buf.data(), len + 1);
            ReadStationParams(i);
            EnqueueText(i, std::wstring(buf.data()));
            SetWindowTextW(hwnd, L"");
        }
        return 0;
    }
    if (msg == WM_CHAR && wParam == VK_RETURN) return 0;
    return CallWindowProcW(g_oldInputEditProc, hwnd, msg, wParam, lParam);
}

static void LayoutMainWindow(int width, int height)
{
    const int m = 8;
    const int topH = 92;
    MoveWindow(g_hGrpRing, m, m, width - 2 * m, topH, TRUE);
    int x = m + 10, y = m + 22;
    MoveWindow(g_hStartBtn, x, y, 150, 26, TRUE);          x += 156;
    MoveWindow(g_hStopBtn, x, y, 110, 26, TRUE);           x += 116;
    MoveWindow(g_hLoseBtn, x, y, 170, 26, TRUE);           x += 176;
    MoveWindow(g_hOrphanBtn, x, y, 170, 26, TRUE);         x += 180;
    MoveWindow(g_hLblDelay, x, y + 4, 130, 20, TRUE);      x += 132;
    MoveWindow(g_hDelayCombo, x, y, 80, 200, TRUE);        x += 92;
    MoveWindow(g_hNoiseCheck, x, y + 3, 190, 20, TRUE);
    MoveWindow(g_hLinesInfo, m + 10, y + 32, (width - 2 * m) / 2 - 20, 20, TRUE);
    MoveWindow(g_hRingPos, m + (width - 2 * m) / 2, y + 32, (width - 2 * m) / 2 - 10, 20, TRUE);

    int stY = m + topH + m;
    int stH = max(220, (height - stY) * 50 / 100);
    int stW = (width - m * (STATIONS + 1)) / STATIONS;
    for (int i = 0; i < STATIONS; ++i) {
        Station& s = g_st[i];
        int sx = m + i * (stW + m);
        int ix = sx + 10, iw = stW - 20;
        MoveWindow(s.hGroup, sx, stY, stW, stH, TRUE);
        int yy = stY + 22;
        int cw = (iw - 20) / 3;
        MoveWindow(s.hLblAddr, ix, yy, cw, 18, TRUE);
        MoveWindow(s.hLblPrio, ix + cw + 10, yy, cw, 18, TRUE);
        MoveWindow(s.hLblDest, ix + 2 * (cw + 10), yy, cw, 18, TRUE);
        yy += 19;
        MoveWindow(s.hAddr, ix, yy, cw, 24, TRUE);
        MoveWindow(s.hPrio, ix + cw + 10, yy, cw, 200, TRUE);
        MoveWindow(s.hDest, ix + 2 * (cw + 10), yy, cw, 24, TRUE);
        yy += 30;
        MoveWindow(s.hMonitor, ix, yy, iw / 2, 20, TRUE);
        MoveWindow(s.hQueue, ix + iw / 2, yy + 2, iw / 2, 18, TRUE);
        yy += 24;
        MoveWindow(s.hLblIn, ix, yy, iw, 18, TRUE);   yy += 19;
        MoveWindow(s.hInput, ix, yy, iw, 24, TRUE);   yy += 30;
        MoveWindow(s.hLblOut, ix, yy, iw, 18, TRUE);  yy += 19;
        MoveWindow(s.hOutput, ix, yy, iw, max(30, stY + stH - yy - 10), TRUE);
    }

    int bY = stY + stH + m;
    int bH = max(80, height - bY - m);
    int dW = (width - 3 * m) * 55 / 100;
    MoveWindow(g_hLblDebug, m, bY, dW, 18, TRUE);
    MoveWindow(g_hDebugEdit, m, bY + 19, dW, bH - 19, TRUE);
    MoveWindow(g_hLblStatus, 2 * m + dW, bY, width - 3 * m - dW, 18, TRUE);
    MoveWindow(g_hStatusEdit, 2 * m + dW, bY + 19, width - 3 * m - dW, bH - 19, TRUE);
}

static void SetMonitor(int i)
{
    InterlockedExchange(&g_monitor, i);
    for (int k = 0; k < STATIONS; ++k)
        SendMessageW(g_st[k].hMonitor, BM_SETCHECK, k == i ? BST_CHECKED : BST_UNCHECKED, 0);
    InterlockedExchange(&g_monitorSeenTick, (LONG)GetTickCount());
    LogDebug(L"Станция-монитор: Ст.%d", i + 1);
}

static HWND MakeCtl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id = 0, DWORD ex = 0)
{
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_hAppFont, TRUE);
    return h;
}

static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        g_hAppFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_hBoldFont = CreateFontW(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_hMonoFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        g_hGrpRing = MakeCtl(hWnd, L"BUTTON", L"Кольцо Token Ring", BS_GROUPBOX);
        g_hStartBtn = MakeCtl(hWnd, L"BUTTON", L"Запустить кольцо", BS_PUSHBUTTON, ID_START_BTN);
        g_hStopBtn = MakeCtl(hWnd, L"BUTTON", L"Остановить", BS_PUSHBUTTON, ID_STOP_BTN);
        g_hLoseBtn = MakeCtl(hWnd, L"BUTTON", L"Сбой: потеря маркера", BS_PUSHBUTTON, ID_LOSE_BTN);
        g_hOrphanBtn = MakeCtl(hWnd, L"BUTTON", L"Сбой: кадр-сирота", BS_PUSHBUTTON, ID_ORPHAN_BTN);
        g_hLblDelay = MakeCtl(hWnd, L"STATIC", L"Задержка на станции:", 0);
        g_hDelayCombo = MakeCtl(hWnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_DELAY_COMBO);
        const wchar_t* delays[] = { L"100 мс", L"300 мс", L"500 мс", L"1000 мс" };
        for (auto d : delays) SendMessageW(g_hDelayCombo, CB_ADDSTRING, 0, (LPARAM)d);
        SendMessageW(g_hDelayCombo, CB_SETCURSEL, 2, 0);
        g_hNoiseCheck = MakeCtl(hWnd, L"BUTTON", L"Помехи 35% (CRC из ЛР3)", BS_AUTOCHECKBOX, ID_NOISE_CHECK);
        SendMessageW(g_hNoiseCheck, BM_SETCHECK, BST_CHECKED, 0);
        g_hLinesInfo = MakeCtl(hWnd, L"STATIC", L"", 0);
        g_hRingPos = MakeCtl(hWnd, L"STATIC", L"Маркер: кольцо остановлено", 0);
        SendMessageW(g_hRingPos, WM_SETFONT, (WPARAM)g_hBoldFont, TRUE);
        EnableWindow(g_hStopBtn, FALSE);

        for (int i = 0; i < STATIONS; ++i) {
            Station& s = g_st[i];
            wchar_t title[64];
            swprintf_s(title, L"Станция %d  (принимает от Ст.%d, передаёт на Ст.%d)", i + 1,
                (i + STATIONS - 1) % STATIONS + 1, NextSt(i) + 1);
            s.hGroup = MakeCtl(hWnd, L"BUTTON", title, BS_GROUPBOX);
            s.hLblAddr = MakeCtl(hWnd, L"STATIC", L"Свой адрес:", 0);
            s.hLblPrio = MakeCtl(hWnd, L"STATIC", L"Приоритет:", 0);
            s.hLblDest = MakeCtl(hWnd, L"STATIC", L"Адрес назначения:", 0);
            wchar_t num[8];
            swprintf_s(num, L"%d", i + 1);
            s.hAddr = MakeCtl(hWnd, L"EDIT", num, ES_NUMBER | ES_AUTOHSCROLL, ID_ST_ADDR + 10 * i, WS_EX_CLIENTEDGE);
            s.hPrio = MakeCtl(hWnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST, ID_ST_PRIO + 10 * i);
            SendMessageW(s.hPrio, CB_ADDSTRING, 0, (LPARAM)L"0 - низкий");
            SendMessageW(s.hPrio, CB_ADDSTRING, 0, (LPARAM)L"1 - высокий");
            SendMessageW(s.hPrio, CB_SETCURSEL, 0, 0);
            swprintf_s(num, L"%d", NextSt(i) + 1);
            s.hDest = MakeCtl(hWnd, L"EDIT", num, ES_NUMBER | ES_AUTOHSCROLL, ID_ST_DEST + 10 * i, WS_EX_CLIENTEDGE);
            s.hMonitor = MakeCtl(hWnd, L"BUTTON", L"Станция-монитор", BS_AUTOCHECKBOX, ID_ST_MONITOR + 10 * i);
            s.hQueue = MakeCtl(hWnd, L"STATIC", L"Блоков в очереди: 0", 0);
            s.hLblIn = MakeCtl(hWnd, L"STATIC", L"Ввод сообщения (Enter - в очередь на передачу):", 0);
            s.hInput = MakeCtl(hWnd, L"EDIT", L"", ES_AUTOHSCROLL, 0, WS_EX_CLIENTEDGE);
            SetWindowLongPtrW(s.hInput, GWLP_USERDATA, i);
            g_oldInputEditProc = (WNDPROC)SetWindowLongPtrW(s.hInput, GWLP_WNDPROC, (LONG_PTR)InputEditSubclassProc);
            s.hLblOut = MakeCtl(hWnd, L"STATIC", L"Принятые сообщения:", 0);
            s.hOutput = MakeCtl(hWnd, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, 0, WS_EX_CLIENTEDGE);
            ReadStationParams(i);
        }

        g_hLblDebug = MakeCtl(hWnd, L"STATIC", L"Отладочное окно: продвижение маркера и пакетов с данными", 0);
        g_hDebugEdit = MakeCtl(hWnd, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_HSCROLL | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 0, WS_EX_CLIENTEDGE);
        SendMessageW(g_hDebugEdit, WM_SETFONT, (WPARAM)g_hMonoFont, TRUE);
        SendMessageW(g_hDebugEdit, EM_SETLIMITTEXT, 0, 0);
        g_hLblStatus = MakeCtl(hWnd, L"STATIC", L"Окно состояния: структура кадра (FCS, биты стаффинга [0])", 0);
        g_hStatusEdit = MakeCtl(hWnd, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_HSCROLL | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 0, WS_EX_CLIENTEDGE);
        SendMessageW(g_hStatusEdit, WM_SETFONT, (WPARAM)g_hMonoFont, TRUE);
        SendMessageW(g_hStatusEdit, EM_SETLIMITTEXT, 0, 0);

        SetTimer(hWnd, 1, 250, NULL);
        return 0;
    }

    case WM_SIZE:
        LayoutMainWindow(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 1100; mmi->ptMinTrackSize.y = 650;
        return 0;
    }

    case WM_TIMER:
        MonitorCheck();
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wParam), code = HIWORD(wParam);
        if (id == ID_START_BTN && code == BN_CLICKED) {
            if (g_monitor < 0) SetMonitor(0);
            for (int k = 0; k < STATIONS; ++k) ReadStationParams(k);
            InterlockedExchange(&g_running, 1);
            EnableWindow(g_hStartBtn, FALSE);
            EnableWindow(g_hStopBtn, TRUE);
            LogDebug(L"===== Кольцо запущено =====");
            MonitorGenerateToken(L"начальная генерация");
        }
        else if (id == ID_STOP_BTN && code == BN_CLICKED) {
            InterlockedExchange(&g_running, 0);
            EnableWindow(g_hStartBtn, TRUE);
            EnableWindow(g_hStopBtn, FALSE);
            SetWindowTextW(g_hRingPos, L"Маркер: кольцо остановлено");
            LogDebug(L"===== Кольцо остановлено =====");
        }
        else if (id == ID_LOSE_BTN && code == BN_CLICKED) {
            InterlockedExchange(&g_loseToken, 1);
            LogDebug(L"Команда: следующая станция, получившая маркер, потеряет его");
        }
        else if (id == ID_ORPHAN_BTN && code == BN_CLICKED) {
            InterlockedExchange(&g_makeOrphan, 1);
            LogDebug(L"Команда: следующая станция, получившая маркер, создаст кадр-сироту");
        }
        else if (id == ID_DELAY_COMBO && code == CBN_SELCHANGE) {
            static const LONG d[] = { 100, 300, 500, 1000 };
            int sel = (int)SendMessageW(g_hDelayCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0) InterlockedExchange(&g_hopDelay, d[sel]);
        }
        else if (id == ID_NOISE_CHECK && code == BN_CLICKED) {
            InterlockedExchange(&g_noise, SendMessageW(g_hNoiseCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
        }
        else if (id >= ID_ST_ADDR && id < ID_ST_ADDR + 10 * STATIONS) {
            int i = (id - ID_ST_ADDR) / 10, sub = (id - ID_ST_ADDR) % 10;
            if (sub == ID_ST_MONITOR - ID_ST_ADDR && code == BN_CLICKED) {
                if (SendMessageW(g_st[i].hMonitor, BM_GETCHECK, 0, 0) == BST_CHECKED) SetMonitor(i);
                else { InterlockedExchange(&g_monitor, -1); LogDebug(L"Станция-монитор не назначена"); }
            }
            else if ((sub == 0 || sub == 2) && code == EN_CHANGE) ReadStationParams(i);
            else if (sub == 1 && code == CBN_SELCHANGE) ReadStationParams(i);
        }
        return 0;
    }

    case WM_APP_RX_TEXT: {
        wchar_t* text = (wchar_t*)lParam;
        AppendEdit(g_st[wParam].hOutput, text, false);
        free(text);
        return 0;
    }
    case WM_APP_DEBUG: {
        wchar_t* text = (wchar_t*)lParam;
        AppendEdit(g_hDebugEdit, text, true);
        free(text);
        return 0;
    }
    case WM_APP_STATUS: {
        wchar_t* text = (wchar_t*)lParam;
        AppendEdit(g_hStatusEdit, text, true);
        free(text);
        return 0;
    }
    case WM_APP_RINGPOS:
        if (g_running) UpdateRingPos((int)wParam, (int)lParam);
        for (int k = 0; k < STATIONS; ++k) UpdateQueueLabel(k);
        return 0;
    case WM_APP_QUEUE:
        UpdateQueueLabel((int)wParam);
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, 1);
        InterlockedExchange(&g_running, 0);
        g_stopThreads = TRUE;
        for (auto& s : g_st) if (s.hThread) { WaitForSingleObject(s.hThread, 1500); CloseHandle(s.hThread); }
        CloseLines();
        DeleteObject(g_hAppFont); DeleteObject(g_hMonoFont); DeleteObject(g_hBoldFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    setlocale(LC_ALL, "Russian");
    srand((unsigned int)time(NULL));
    InitSyndromeTable();
    g_startTick = GetTickCount();

    for (int i = 0; i < STATIONS; ++i) {
        g_st[i].idx = i;
        InitializeCriticalSection(&g_st[i].cs);
        InitializeCriticalSection(&g_st[i].txLock);
        std::fill(std::begin(g_st[i].rxLineStart), std::end(g_st[i].rxLineStart), true);
    }
    InitLines();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Lab5TokenRingWnd";
    RegisterClassExW(&wc);

    g_hWndMain = CreateWindowW(L"Lab5TokenRingWnd", L"Лабораторная работа №5: Token Ring (маркерное кольцо из 3 станций)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860, NULL, NULL, hInstance, NULL);

    wchar_t info[160];
    if (g_useCom)
        swprintf_s(info, L"Линии: COM%d→COM%d (1→2),  COM%d→COM%d (2→3),  COM%d→COM%d (3→1)",
            g_lines[0].txNum, g_lines[0].rxNum, g_lines[1].txNum, g_lines[1].rxNum, g_lines[2].txNum, g_lines[2].rxNum);
    else
        swprintf_s(info, L"Линии: эмуляция (3 пары виртуальных COM-портов не найдены)");
    SetWindowTextW(g_hLinesInfo, info);

    SetMonitor(0);
    LogDebug(L"Программа запущена. %ls", info);
    LogDebug(L"Кадр: Flag(8B)|AC(1B)|DA(4B)|SA(4B)|Data(%dB)|FCS(1B)|FS(1B)|Flag(8B); маркер: Flag|AC|Flag; флаг 0x%02X",
        (int)DATA_PAYLOAD_LEN, FLAG_BYTE);
    LogDebug(L"Нажмите \"Запустить кольцо\" - монитор сгенерирует маркер.");

    ShowWindow(g_hWndMain, nCmdShow);
    UpdateWindow(g_hWndMain);

    for (int i = 0; i < STATIONS; ++i)
        g_st[i].hThread = CreateThread(NULL, 0, StationRxThread, (LPVOID)(INT_PTR)i, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
