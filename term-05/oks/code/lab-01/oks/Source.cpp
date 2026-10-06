// =====================================================================
//  Laboratornaya rabota No.1. Kommunikatsionnaya programma cherez COM-porty.
//  Variant: ChYoTNYY
//    -> odno glavnoe okno, v kotorom raspolozheny:
//         - panel upravleniya (skorost porta + nomer porta + knopka)
//         - panel sostoyaniya  (nomera COM-portov TX/RX + kol-vo bayt)
//         - polye vvoda (Enter - otpravka)
//         - polye vyvoda (prinyatyy potok bayt)
//         - otladochnoye polye (log)
//
//  Sreda: Visual Studio, chistyy Win32 API (bez MFC/ATL), Unicode.
//  Odin i tot zhe .exe zapuskaetsya DVAZhDY (kak "pervyy" i "vtoroy"
//  abonent) - topologiya portov podbiraetsya avtomaticheski.
//
//  Kak nastroit proekt v Visual Studio (kratko):
//   1) Sozdat proekt "Pustoy proekt" (Empty Project) tipa
//      Windows Desktop Application (ne konsolnoe!).
//   2) Dobavit etot fayl v proekt.
//   3) Svoystva proekta -> Character Set -> Use Unicode Character Set.
//   4) Sobrat (Ctrl+Shift+B), zapustit dva raza (Ctrl+F5 dvazhdy).
// =====================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

// ------------------- Polzovatelskie soobshcheniya -------------------
#define WM_APP_RX_CHAR   (WM_APP + 1)   // wParam = prinyatyy bayt
#define WM_APP_RX_LINE   (WM_APP + 2)   // wParam = kol-vo bayt v portsii
#define WM_APP_DEBUG     (WM_APP + 3)   // lParam = LPCWSTR (stroka loga)

// ------------------- Globalnoe sostoyanie portov --------------------
static HANDLE g_hComTx = INVALID_HANDLE_VALUE;   // port na PEREDAChU
static HANDLE g_hComRx = INVALID_HANDLE_VALUE;   // port na PRIYoM
static int    g_txNum = -1;
static int    g_rxNum = -1;
static DWORD  g_baud = CBR_9600;               // skorost po umolchaniyu
static HANDLE g_hReaderThread = NULL;
static volatile BOOL g_bStopReader = FALSE;
static ULONGLONG g_totalRxBytes = 0;

// ------------------- Edinstvennoe glavnoe okno -----------------------
static HWND g_hWndMain = NULL;

// ------------------- Elementy upravleniya (vse - deti glavnogo okna) -
static HWND g_hInputEdit = NULL;
static HWND g_hOutputEdit = NULL;
static HWND g_hDebugEdit = NULL;
static HWND g_hBaudCombo = NULL;
static HWND g_hPortCombo = NULL;
static HWND g_hApplyBtn = NULL;
static HWND g_hStatusPorts = NULL;
static HWND g_hStatusBytes = NULL;

static HWND g_hLblInput = NULL;
static HWND g_hLblOutput = NULL;
static HWND g_hLblDebug = NULL;
static HWND g_hLblBaud = NULL;
static HWND g_hLblPort = NULL;
static HWND g_hGrpControl = NULL;
static HWND g_hGrpStatus = NULL;

static WNDPROC g_oldInputEditProc = NULL;

// ID elementov
#define ID_BAUD_COMBO   101
#define ID_PORT_COMBO   102
#define ID_APPLY_BTN    103

// forward decl
static void LogDebug(const wchar_t* fmt, ...);
static void UpdateStatusPorts();
static void UpdateStatusBytes(DWORD lastPortionBytes);
static void AppendOutputChar(wchar_t ch);
static void ApplyControlSettings();
static void LayoutMainWindow(int width, int height);

// =====================================================================
//  FIZIChESKIY UROVEN: rabota s COM-portami (WinAPI)
//  --- Eta chast - "teoriya" dlya otchyota: parametry initsializatsii porta ---
// =====================================================================

// Parametry porta, zafiksirovannye zadaniem (p.5 - "ostalnye parametry
// portov dolzhny byt fiksirovannymi"):
//   ByteSize            = 8    (bit dannykh v simvole)
//   StopBits            = ONESTOPBIT (1 stop-bit)
//   Parity              = NOPARITY   (bez kontrolya chyotnosti)
//   fBinary             = TRUE (obyazatelno dlya Win32, dvoichnyy rezhim)
//   fOutxCtsFlow/fRtsControl = FALSE/RTS_CONTROL_DISABLE (bez apparatnogo
//                              upravleniya potokom)
//   fOutX/fInX          = FALSE (bez programmnogo XON/XOF)
//   ReadIntervalTimeout = MAXDWORD, ostalnye ReadTotalTimeout* = 0
//                         -> ReadFile blokiruetsya, poka ne pridyot khotya by
//                            odin bayt (posimvolnoe "syroe" chtenie)
static BOOL ConfigureCommPort(HANDLE h, DWORD baud)
{
    DCB dcb;
    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);

    if (!GetCommState(h, &dcb)) return FALSE;

    dcb.BaudRate = baud;         // vybiraetsya polzovatelem (panel upravleniya)
    dcb.ByteSize = 8;            // fiksirovano
    dcb.Parity = NOPARITY;     // fiksirovano
    dcb.StopBits = ONESTOPBIT;   // fiksirovano
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(h, &dcb)) return FALSE;

    COMMTIMEOUTS to;
    ZeroMemory(&to, sizeof(to));
    to.ReadIntervalTimeout = MAXDWORD; // sm. kommentariy vyshe
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = 0;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 100;
    if (!SetCommTimeouts(h, &to)) return FALSE;

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return TRUE;
}

// Pytaetsya monopolno otkryt COM-port s zadannym nomerom.
static HANDLE TryOpenPort(int num, DWORD baud)
{
    wchar_t path[32];
    // \\.\COMn - obyazatelnaya forma dlya portov s nomerom >= 10
    swprintf_s(path, L"\\\\.\\COM%d", num);

    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
        0,              // bez sovmestnogo dostupa - monopolno
        NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    if (!ConfigureCommPort(h, baud)) {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

// Spisok realno prisutstvuyushchikh v sisteme COM-portov (virtualnykh v tom
// chisle) - beryotsya iz reestra HKLM\HARDWARE\DEVICEMAP\SERIALCOMM.
static std::vector<int> EnumerateAvailablePorts()
{
    std::vector<int> result;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return result;

    wchar_t valueName[256];
    wchar_t data[256];
    DWORD idx = 0;
    for (;;) {
        DWORD valueNameLen = 256, dataLen = sizeof(data), type;
        LONG rc = RegEnumValueW(hKey, idx, valueName, &valueNameLen, NULL,
            &type, (LPBYTE)data, &dataLen);
        if (rc != ERROR_SUCCESS) break;
        int num = 0;
        if (swscanf_s(data, L"COM%d", &num) == 1) result.push_back(num);
        idx++;
    }
    RegCloseKey(hKey);
    std::sort(result.begin(), result.end());
    return result;
}

// -----------------------------------------------------------------
//  Avtomaticheskiy algoritm vybora nomerov portov (p.3 trebovaniy).
//  Logika: dostupnye porty sortiruyutsya i gruppiruyutsya poparno
//  (p0,p1), (p2,p3), ...  Dlya kazhdoy pary probuem monopolno otkryt
//  oba porta: esli pervyy ekzemplyar programmy ikh uzhe zanyal - popytka
//  ne udastsya, i algoritm avtomaticheski perekhodit k sleduyushchey pare.
//  Pervyy port pary stanovitsya portom PEREDAChI (TX), vtoroy - PRIYoMA (RX).
// -----------------------------------------------------------------
static BOOL OpenPortsAuto(DWORD baud)
{
    std::vector<int> avail = EnumerateAvailablePorts();
    LogDebug(L"Naideno COM-portov v sisteme: %d", (int)avail.size());

    for (size_t i = 0; i + 1 < avail.size(); i += 2) {
        int candTx = avail[i];
        int candRx = avail[i + 1];

        HANDLE hTx = TryOpenPort(candTx, baud);
        if (hTx == INVALID_HANDLE_VALUE) {
            LogDebug(L"COM%d zanyat/nedostupen, propusk pary", candTx);
            continue;
        }
        HANDLE hRx = TryOpenPort(candRx, baud);
        if (hRx == INVALID_HANDLE_VALUE) {
            CloseHandle(hTx);
            LogDebug(L"COM%d zanyat/nedostupen, propusk pary", candRx);
            continue;
        }

        g_hComTx = hTx; g_txNum = candTx;
        g_hComRx = hRx; g_rxNum = candRx;
        LogDebug(L"Avtomaticheski vybrany porty: TX=COM%d, RX=COM%d", candTx, candRx);
        return TRUE;
    }
    return FALSE;
}

// Zakryt tekushchie porty (ispolzuetsya pri smene skorosti/portov vruchnuyu).
static void ClosePorts()
{
    if (g_hComTx != INVALID_HANDLE_VALUE) { CloseHandle(g_hComTx); g_hComTx = INVALID_HANDLE_VALUE; }
    if (g_hComRx != INVALID_HANDLE_VALUE) { CloseHandle(g_hComRx); g_hComRx = INVALID_HANDLE_VALUE; }
}

// =====================================================================
//  Potok priyoma (posimvolnoe "syroe" chtenie potoka)
// =====================================================================
static DWORD WINAPI ReaderThreadProc(LPVOID)
{
    int linePos = 0;

    while (!g_bStopReader) {
        if (g_hComRx == INVALID_HANDLE_VALUE) { Sleep(50); continue; }

        BYTE b;
        DWORD read = 0;
        BOOL ok = ReadFile(g_hComRx, &b, 1, &read, NULL);
        if (!ok || read == 0) {
            // port mog byt pereotkryt (smena skorosti/nomera) - podozhdyom
            Sleep(20);
            continue;
        }

        g_totalRxBytes++;
        // Obratnoe preobrazovanie: poluchennyy bayt traktuetsya kak simvol
        // kodovoy stranitsy Windows-1251 (sm. kommentariy v SendLineRaw).
        wchar_t wc = L'?';
        MultiByteToWideChar(1251, 0, (LPCCH)&b, 1, &wc, 1);

        // Otobrazit prinyatyy simvol srazu zhe (trebovanie k polyu vyvoda)
        PostMessage(g_hWndMain, WM_APP_RX_CHAR, (WPARAM)wc, 0);

        if (wc == L'\r' || wc == L'\n') {
            if (linePos > 0) {
                PostMessage(g_hWndMain, WM_APP_RX_LINE, (WPARAM)linePos, 0);
                linePos = 0;
            }
        }
        else if (linePos < 4095) {
            linePos++;
        }
    }
    return 0;
}

// =====================================================================
//  Vspomogatelnye funktsii GUI
// =====================================================================
static void LogDebug(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);

    if (g_hWndMain) {
        wchar_t* copy = _wcsdup(buf);
        PostMessage(g_hWndMain, WM_APP_DEBUG, 0, (LPARAM)copy);
    }
}

static void UpdateStatusPorts()
{
    if (!g_hStatusPorts) return;
    wchar_t buf[128];
    swprintf_s(buf, L"TX = COM%d      RX = COM%d", g_txNum, g_rxNum);
    SetWindowTextW(g_hStatusPorts, buf);
}

static void UpdateStatusBytes(DWORD lastPortionBytes)
{
    if (!g_hStatusBytes) return;
    wchar_t buf[128];
    swprintf_s(buf, L"Bait v posledney prinyatoy portsii: %u", lastPortionBytes);
    SetWindowTextW(g_hStatusBytes, buf);
}

static void AppendOutputChar(wchar_t ch)
{
    int len = GetWindowTextLengthW(g_hOutputEdit);
    SendMessageW(g_hOutputEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    wchar_t s[2] = { ch, 0 };
    SendMessageW(g_hOutputEdit, EM_REPLACESEL, FALSE, (LPARAM)s);
}

// Otpravka "syrogo" potoka bayt POSIMVOLNO (bez paketov/zagolovkov).
// Russkie (i drugie ne-ASCII) simvoly kodiruyutsya v odnobaytovuyu
// kodovuyu stranitsu Windows-1251, t.k. po COM-portu idyot imenno potok
// BAYT, a ne Unicode-simvolov.
static void SendLineRaw(const std::wstring& text)
{
    if (g_hComTx == INVALID_HANDLE_VALUE) return;
    for (wchar_t wc : text) {
        BYTE b = '?';
        WideCharToMultiByte(1251, 0, &wc, 1, (LPSTR)&b, 1, NULL, NULL);
        DWORD written = 0;
        WriteFile(g_hComTx, &b, 1, &written, NULL);
    }
    BYTE nl = '\n';
    DWORD written = 0;
    WriteFile(g_hComTx, &nl, 1, &written, NULL);
    LogDebug(L"Otpravleno %d simvol(ov) + perevod stroki", (int)text.size());
}

// =====================================================================
//  Podklass polya vvoda: lovim nazhatie Enter
// =====================================================================
static LRESULT CALLBACK InputEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        int len = GetWindowTextLengthW(hwnd);
        std::wstring text(len, L'\0');
        if (len > 0) GetWindowTextW(hwnd, &text[0], len + 1);
        text.resize(len);

        SendLineRaw(text);
        SetWindowTextW(hwnd, L""); // ochistit pole vvoda posle otpravki
        return 0; // ne peredayom Enter dalshe (inache budet sistemnyy "bip")
    }
    return CallWindowProcW(g_oldInputEditProc, hwnd, msg, wParam, lParam);
}

// =====================================================================
//  Raspolozhenie elementov v edinstvennom glavnom okne
//  Struktura sverkhu vniz:
//   [ Panel upravleniya (skorost/port/knopka) ]  [ Panel sostoyaniya ]
//   [ Polye vvoda ]
//   [ Polye vyvoda (rastyagivaetsya) ] [ Otladochnoye polye ]
// =====================================================================
static void LayoutMainWindow(int width, int height)
{
    const int margin = 10;
    const int ctrlPanelH = 130;
    const int inputRowH = 60;
    const int lowerY = margin + ctrlPanelH + margin + inputRowH + margin;
    int lowerH = height - lowerY - margin;
    if (lowerH < 100) lowerH = 100;

    int leftW = (width - margin * 3) * 2 / 3;
    int rightW = (width - margin * 3) - leftW;

    // --- Panel upravleniya (sleva sverkhu) ---
    int ctrlPanelW = leftW / 2 - margin / 2;
    if (g_hGrpControl) MoveWindow(g_hGrpControl, margin, margin, ctrlPanelW, ctrlPanelH, TRUE);
    if (g_hLblBaud)    MoveWindow(g_hLblBaud, margin + 12, margin + 22, ctrlPanelW - 24, 20, TRUE);
    if (g_hBaudCombo)  MoveWindow(g_hBaudCombo, margin + 12, margin + 44, ctrlPanelW - 24, 24, TRUE);
    if (g_hLblPort)    MoveWindow(g_hLblPort, margin + 12, margin + 74, ctrlPanelW - 24, 20, TRUE);
    if (g_hPortCombo)  MoveWindow(g_hPortCombo, margin + 12, margin + 96, ctrlPanelW - 24 - 100, 24, TRUE);
    if (g_hApplyBtn)   MoveWindow(g_hApplyBtn, margin + 12 + (ctrlPanelW - 24 - 96), margin + 95, 96, 26, TRUE);

    // --- Panel sostoyaniya (sprava sverkhu, ryadom s panelyu upravleniya) ---
    int statusX = margin + ctrlPanelW + margin;
    int statusW = leftW - ctrlPanelW - margin;
    if (statusW < 150) statusW = 150;
    if (g_hGrpStatus)   MoveWindow(g_hGrpStatus, statusX, margin, statusW, ctrlPanelH, TRUE);
    if (g_hStatusPorts) MoveWindow(g_hStatusPorts, statusX + 12, margin + 30, statusW - 24, 24, TRUE);
    if (g_hStatusBytes) MoveWindow(g_hStatusBytes, statusX + 12, margin + 64, statusW - 24, 40, TRUE);

    // --- Polye vvoda (na vsyu shirinu, pod panelyami) ---
    int inputY = margin + ctrlPanelH + margin;
    int fullW = width - margin * 2;
    if (g_hLblInput)  MoveWindow(g_hLblInput, margin, inputY, fullW, 20, TRUE);
    if (g_hInputEdit) MoveWindow(g_hInputEdit, margin, inputY + 22, fullW, 26, TRUE);

    // --- Polye vyvoda (slева, rastyagivaetsya) i otladochnoye (sprava) ---
    if (g_hLblOutput)  MoveWindow(g_hLblOutput, margin, lowerY, leftW, 18, TRUE);
    if (g_hOutputEdit) MoveWindow(g_hOutputEdit, margin, lowerY + 20, leftW, lowerH - 20, TRUE);

    int debugX = margin + leftW + margin;
    if (g_hLblDebug)  MoveWindow(g_hLblDebug, debugX, lowerY, rightW, 18, TRUE);
    if (g_hDebugEdit) MoveWindow(g_hDebugEdit, debugX, lowerY + 20, rightW, lowerH - 20, TRUE);
}

// Primenit nastroyki iz panely upravleniya: peresozdat porty po vybrannoy
// polzovatelem skorosti, i, esli vybrana drugaya para portov - po ney.
static void ApplyControlSettings()
{
    int baudIdx = (int)SendMessageW(g_hBaudCombo, CB_GETCURSEL, 0, 0);
    static const DWORD baudValues[] = { 1200,2400,4800,9600,19200,38400,57600,115200 };
    DWORD newBaud = (baudIdx >= 0) ? baudValues[baudIdx] : CBR_9600;

    int portSel = (int)SendMessageW(g_hPortCombo, CB_GETCURSEL, 0, 0);
    int newTx = g_txNum, newRx = g_rxNum;
    if (portSel >= 0) {
        wchar_t buf[64];
        SendMessageW(g_hPortCombo, CB_GETLBTEXT, portSel, (LPARAM)buf);
        swscanf_s(buf, L"TX=COM%d RX=COM%d", &newTx, &newRx);
    }

    g_bStopReader = TRUE;
    if (g_hReaderThread) { WaitForSingleObject(g_hReaderThread, 500); CloseHandle(g_hReaderThread); g_hReaderThread = NULL; }
    ClosePorts();

    HANDLE hTx = TryOpenPort(newTx, newBaud);
    HANDLE hRx = TryOpenPort(newRx, newBaud);
    if (hTx != INVALID_HANDLE_VALUE && hRx != INVALID_HANDLE_VALUE) {
        g_hComTx = hTx; g_hComRx = hRx; g_txNum = newTx; g_rxNum = newRx; g_baud = newBaud;
        LogDebug(L"Porty peresozdany vruchnuyu: TX=COM%d RX=COM%d, skorost=%u", newTx, newRx, newBaud);
    }
    else {
        LogDebug(L"Ne udalos otkryt vybrannye porty, ostavlyaem prezhnie nastroiki");
        if (hTx != INVALID_HANDLE_VALUE) CloseHandle(hTx);
        if (hRx != INVALID_HANDLE_VALUE) CloseHandle(hRx);
        // vosstanovit prezhnie porty na prezhney skorosti
        g_hComTx = TryOpenPort(g_txNum, g_baud);
        g_hComRx = TryOpenPort(g_rxNum, g_baud);
    }
    UpdateStatusPorts();

    g_bStopReader = FALSE;
    g_hReaderThread = CreateThread(NULL, 0, ReaderThreadProc, NULL, 0, NULL);
}

// =====================================================================
//  Edinstvennaya okonnaya protsedura glavnogo okna
// =====================================================================
static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        // --- Gruppa "Panel upravleniya" ---
        g_hGrpControl = CreateWindowW(L"BUTTON", L"Okno upravleniya (skorost / port)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);

        g_hLblBaud = CreateWindowW(L"STATIC", L"Skorost porta (bod):",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hBaudCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)ID_BAUD_COMBO, NULL, NULL);
        {
            const wchar_t* bauds[] = { L"1200", L"2400", L"4800", L"9600",
                                        L"19200", L"38400", L"57600", L"115200" };
            for (auto b : bauds) SendMessageW(g_hBaudCombo, CB_ADDSTRING, 0, (LPARAM)b);
            SendMessageW(g_hBaudCombo, CB_SETCURSEL, 3, 0); // 9600 po umolchaniyu
        }

        g_hLblPort = CreateWindowW(L"STATIC", L"Nomer porta (para TX/RX):",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hPortCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)ID_PORT_COMBO, NULL, NULL);

        g_hApplyBtn = CreateWindowW(L"BUTTON", L"Primenit",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)ID_APPLY_BTN, NULL, NULL);

        // --- Gruppa "Panel sostoyaniya" ---
        g_hGrpStatus = CreateWindowW(L"BUTTON", L"Okno sostoyaniya",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hStatusPorts = CreateWindowW(L"STATIC", L"TX = ?   RX = ?",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hStatusBytes = CreateWindowW(L"STATIC", L"Bait v posledney prinyatoy portsii: 0",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);

        // --- Polye vvoda ---
        g_hLblInput = CreateWindowW(L"STATIC", L"Okno vvoda - vvedite tekst i nazhmite Enter:",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hInputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_oldInputEditProc = (WNDPROC)SetWindowLongPtrW(
            g_hInputEdit, GWLP_WNDPROC, (LONG_PTR)InputEditSubclassProc);

        // --- Polye vyvoda ---
        g_hLblOutput = CreateWindowW(L"STATIC", L"Okno vyvoda (prinyatyy potok):",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hOutputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            0, 0, 0, 0, hWnd, NULL, NULL, NULL);

        // --- Otladochnoye polye ---
        g_hLblDebug = CreateWindowW(L"STATIC", L"Otladochnoye okno (log):",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hDebugEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            0, 0, 0, 0, hWnd, NULL, NULL, NULL);

        SetFocus(g_hInputEdit);
        return 0;
    }

    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        LayoutMainWindow(w, h);
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) SetFocus(g_hInputEdit);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_APPLY_BTN && HIWORD(wParam) == BN_CLICKED) {
            ApplyControlSettings();
            SetFocus(g_hInputEdit);
        }
        return 0;

    case WM_APP_RX_CHAR:
        AppendOutputChar((wchar_t)wParam);
        return 0;

    case WM_APP_RX_LINE:
        UpdateStatusBytes((DWORD)wParam);
        return 0;

    case WM_APP_DEBUG: {
        wchar_t* text = (wchar_t*)lParam;
        int len = GetWindowTextLengthW(g_hDebugEdit);
        SendMessageW(g_hDebugEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageW(g_hDebugEdit, EM_REPLACESEL, FALSE, (LPARAM)text);
        SendMessageW(g_hDebugEdit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
        free(text);
        return 0;
    }

    case WM_DESTROY:
        g_bStopReader = TRUE;
        ClosePorts();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// =====================================================================
//  Registratsiya klassa okna i sozdanie ODNOGO glavnogo okna
// =====================================================================
static ATOM RegisterSimpleClass(HINSTANCE hInst, const wchar_t* name, WNDPROC proc)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = name;
    return RegisterClassExW(&wc);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    RegisterSimpleClass(hInstance, L"Lab1MainWnd", MainWndProc);

    g_hWndMain = CreateWindowW(L"Lab1MainWnd",
        L"Lab1: Kommunikatsionnaya programma cherez COM-porty",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 640,
        NULL, NULL, hInstance, NULL);

    if (!g_hWndMain) return 1;
    ShowWindow(g_hWndMain, nCmdShow);
    UpdateWindow(g_hWndMain);

    // ---- Avtomaticheskiy vybor portov (p.3 trebovaniy) ----
    if (!OpenPortsAuto(g_baud)) {
        MessageBoxW(g_hWndMain,
            L"Ne udalos nayti dve svobodnye pary COM-portov.\n"
            L"Proverte, chto emulyator COM-portov zapushchen i sozdana\n"
            L"topologiya soglasno risunku zadaniya.",
            L"Oshibka initsializatsii", MB_ICONERROR);
        return 1;
    }
    UpdateStatusPorts();

    // Zapolnim komboboks dostupnoy paroy (dlya naglyadnosti i dlya
    // vozmozhnosti vybrat druguyu paru vruchnuyu).
    {
        std::vector<int> avail = EnumerateAvailablePorts();
        for (size_t i = 0; i + 1 < avail.size(); i += 2) {
            wchar_t buf[64];
            swprintf_s(buf, L"TX=COM%d RX=COM%d", avail[i], avail[i + 1]);
            int idx = (int)SendMessageW(g_hPortCombo, CB_ADDSTRING, 0, (LPARAM)buf);
            if (avail[i] == g_txNum && avail[i + 1] == g_rxNum)
                SendMessageW(g_hPortCombo, CB_SETCURSEL, idx, 0);
        }
    }

    g_hReaderThread = CreateThread(NULL, 0, ReaderThreadProc, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_bStopReader = TRUE;
    if (g_hReaderThread) { WaitForSingleObject(g_hReaderThread, 500); CloseHandle(g_hReaderThread); }
    ClosePorts();
    return 0;
}