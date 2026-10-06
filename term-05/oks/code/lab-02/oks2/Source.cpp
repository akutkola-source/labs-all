// =====================================================================
//  Laboratornaya rabota No.2. Paketnaya peredacha dannykh cherez COM-porty.
//  Variant: ChYoTNYY
//    - Struktura kadra: Flag (8B) | Dest (4B) | Src (4B) | Data (n+1 B) | FCS (1B)
//    - Flag: dvoichnoe predstavlenie (128 + n)
//    - Algoritm: Bit-staffing (vstavka '0' posle pyati '1')
//    - Okno sostoyaniya: vyvodit strukturu kadra s vydeleniem bitov staffinga
// =====================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdint>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

// --- UKAZhITE VASH NOMER PO SPISKU GRUPPY ---
constexpr int STUDENT_N = 10;                      // n[cite: 1, 2]
constexpr size_t DATA_PAYLOAD_LEN = STUDENT_N + 1; // n + 1 bayt[cite: 1, 2]
constexpr uint8_t FLAG_BYTE = (uint8_t)(128 + STUDENT_N); //[cite: 1, 2]

// Soobshcheniya
#define WM_APP_RX_PACKET (WM_APP + 1)
#define WM_APP_DEBUG     (WM_APP + 3)

static HANDLE g_hComTx = INVALID_HANDLE_VALUE;
static HANDLE g_hComRx = INVALID_HANDLE_VALUE;
static int    g_txNum = -1;
static int    g_rxNum = -1;
static DWORD  g_baud = CBR_9600;
static HANDLE g_hReaderThread = NULL;
static volatile BOOL g_bStopReader = FALSE;

static HWND g_hWndMain = NULL;
static HWND g_hInputEdit = NULL;
static HWND g_hOutputEdit = NULL;
static HWND g_hDebugEdit = NULL;
static HWND g_hBaudCombo = NULL;
static HWND g_hPortCombo = NULL;
static HWND g_hApplyBtn = NULL;
static HWND g_hStatusPorts = NULL;
static HWND g_hStatusFrame = NULL;

static HWND g_hLblInput = NULL;
static HWND g_hLblOutput = NULL;
static HWND g_hLblDebug = NULL;
static HWND g_hLblBaud = NULL;
static HWND g_hLblPort = NULL;
static HWND g_hGrpControl = NULL;
static HWND g_hGrpStatus = NULL;

static WNDPROC g_oldInputEditProc = NULL;

#define ID_BAUD_COMBO   101
#define ID_PORT_COMBO   102
#define ID_APPLY_BTN    103

static void LogDebug(const wchar_t* fmt, ...);
static void UpdateStatusPorts();
static void LayoutMainWindow(int width, int height);
static HANDLE TryOpenPort(int num, DWORD baud);
static void ClosePorts();

// =====================================================================
//  BIT-STAFFING I DESTAFING
// =====================================================================

// Vypolnyaet bit-staffing: posle 5 edinits podryad vstavlyaetsya bit '0'[cite: 1, 2]
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
                consecutiveOnes++;
                if (consecutiveOnes == 5) {
                    bits.push_back(false);   // Vstavka sluzhebnogo 0[cite: 1, 2]
                    outVisualBits += L"[0]"; // Vydelenie dlya interfeysa[cite: 1, 2]
                    consecutiveOnes = 0;
                }
            }
            else {
                consecutiveOnes = 0;
            }
        }
    }

    // Upakovka obratno v bayty
    std::vector<uint8_t> stuffedBytes;
    uint8_t curByte = 0;
    int bitCount = 0;

    for (bool b : bits) {
        curByte = (curByte << 1) | (b ? 1 : 0);
        bitCount++;
        if (bitCount == 8) {
            stuffedBytes.push_back(curByte);
            curByte = 0;
            bitCount = 0;
        }
    }
    if (bitCount > 0) {
        curByte <<= (8 - bitCount);
        stuffedBytes.push_back(curByte);
    }

    return stuffedBytes;
}

// Destaffing: udalenie '0' posle 5 edinits podryad[cite: 1, 2]
static std::vector<uint8_t> BitDestuffing(const std::vector<uint8_t>& stuffedBytes)
{
    std::vector<bool> bits;
    int consecutiveOnes = 0;

    for (uint8_t b : stuffedBytes) {
        for (int i = 7; i >= 0; --i) {
            bool bit = (b >> i) & 1;
            if (consecutiveOnes == 5) {
                consecutiveOnes = 0;
                if (!bit) continue; // Propusk vstavlennogo nulya[cite: 1, 2]
            }
            bits.push_back(bit);
            if (bit) consecutiveOnes++;
            else consecutiveOnes = 0;
        }
    }

    std::vector<uint8_t> destuffedBytes;
    uint8_t curByte = 0;
    int bitCount = 0;
    for (bool b : bits) {
        curByte = (curByte << 1) | (b ? 1 : 0);
        bitCount++;
        if (bitCount == 8) {
            destuffedBytes.push_back(curByte);
            curByte = 0;
            bitCount = 0;
        }
    }
    return destuffedBytes;
}

// =====================================================================
//  SBORKA I OTPRAVKA KADRA
// =====================================================================

static void SendFrame(const std::vector<uint8_t>& chunk)
{
    if (g_hComTx == INVALID_HANDLE_VALUE) return;

    // 1. Pole dannykh n+1 bayt[cite: 1, 2]
    std::vector<uint8_t> dataField(DATA_PAYLOAD_LEN, 0x00);
    size_t copyLen = min(chunk.size(), DATA_PAYLOAD_LEN);
    memcpy(dataField.data(), chunk.data(), copyLen);

    // 2. Telo kadra: DestAddr(4) + SrcAddr(4) + Data(n+1) + FCS(1)[cite: 1, 2]
    std::vector<uint8_t> body;
    body.insert(body.end(), 4, 0x00); // Dest = 0[cite: 1, 2]

    uint32_t src = (uint32_t)g_txNum; // Src = nomer COM-porta[cite: 1, 2]
    body.push_back((src >> 24) & 0xFF);
    body.push_back((src >> 16) & 0xFF);
    body.push_back((src >> 8) & 0xFF);
    body.push_back(src & 0xFF);

    body.insert(body.end(), dataField.begin(), dataField.end());
    body.push_back(0x00); // FCS = 0[cite: 1, 2]

    // 3. Bit-staffing[cite: 1, 2]
    std::wstring visualBits;
    std::vector<uint8_t> stuffedBody = BitStuffing(body, visualBits);

    // 4. Polnyy kadr: Flag (8B) + Telo posle staffinga + Flag (8B dlya mgnovennogo zakrytiya kadra)[cite: 1, 2]
    std::vector<uint8_t> frame;
    frame.insert(frame.end(), 8, FLAG_BYTE); //[cite: 1, 2]
    frame.insert(frame.end(), stuffedBody.begin(), stuffedBody.end());
    frame.insert(frame.end(), 8, FLAG_BYTE); //[cite: 1, 2]

    // 5. Vyvod v okno sostoyaniya s vydeleniem[cite: 1, 2]
    std::wstring statusStr = L"TX(COM" + std::to_wstring(g_txNum) + L"): " + visualBits;
    SetWindowTextW(g_hStatusFrame, statusStr.c_str());

    DWORD written = 0;
    WriteFile(g_hComTx, frame.data(), (DWORD)frame.size(), &written, NULL);
    LogDebug(L"Otpravlen kadr: %d bayt (posle staffinga)", (int)frame.size());
}

static void SendPacketizedText(const std::wstring& text)
{
    std::string ansi;
    int len = WideCharToMultiByte(1251, 0, text.c_str(), (int)text.size(), NULL, 0, NULL, NULL);
    ansi.resize(len);
    WideCharToMultiByte(1251, 0, text.c_str(), (int)text.size(), &ansi[0], len, NULL, NULL);

    std::vector<uint8_t> bytes(ansi.begin(), ansi.end());
    if (bytes.empty()) bytes.push_back('\0');

    for (size_t offset = 0; offset < bytes.size(); offset += DATA_PAYLOAD_LEN) {
        size_t curSize = min(DATA_PAYLOAD_LEN, bytes.size() - offset);
        std::vector<uint8_t> chunk(bytes.begin() + offset, bytes.begin() + offset + curSize);
        SendFrame(chunk);
        Sleep(35);
    }
}

// =====================================================================
//  POTOK PRIYoMA
// =====================================================================
static DWORD WINAPI ReaderThreadProc(LPVOID)
{
    std::vector<uint8_t> rxBuffer;
    const size_t rawBodyLen = 4 + 4 + DATA_PAYLOAD_LEN + 1; //[cite: 1, 2]

    while (!g_bStopReader) {
        if (g_hComRx == INVALID_HANDLE_VALUE) { Sleep(50); continue; }

        BYTE b;
        DWORD read = 0;
        if (!ReadFile(g_hComRx, &b, 1, &read, NULL) || read == 0) {
            Sleep(10);
            continue;
        }

        rxBuffer.push_back(b);

        // Opredelenie konca kadra po 8 baytam flaga[cite: 1, 2]
        if (rxBuffer.size() >= 8) {
            bool isFlag = true;
            for (size_t i = rxBuffer.size() - 8; i < rxBuffer.size(); ++i) {
                if (rxBuffer[i] != FLAG_BYTE) { isFlag = false; break; } //[cite: 1, 2]
            }

            if (isFlag) {
                std::vector<uint8_t> candidate(rxBuffer.begin(), rxBuffer.end() - 8);
                rxBuffer.clear();

                // Otsekaem starshie flagi, esli oni est v nachale
                while (candidate.size() >= 8) {
                    bool leadingFlag = true;
                    for (int i = 0; i < 8; ++i) {
                        if (candidate[i] != FLAG_BYTE) { leadingFlag = false; break; } //[cite: 1, 2]
                    }
                    if (leadingFlag) {
                        candidate.erase(candidate.begin(), candidate.begin() + 8);
                    }
                    else {
                        break;
                    }
                }

                if (!candidate.empty()) {
                    std::vector<uint8_t> raw = BitDestuffing(candidate); //[cite: 1, 2]
                    if (raw.size() >= rawBodyLen) {
                        size_t dataStart = 8; // Dest(4) + Src(4)[cite: 1, 2]
                        std::string chunkStr;
                        for (size_t i = 0; i < DATA_PAYLOAD_LEN; ++i) {
                            if (raw[dataStart + i] != '\0') chunkStr += (char)raw[dataStart + i];
                        }
                        if (!chunkStr.empty()) {
                            int wlen = MultiByteToWideChar(1251, 0, chunkStr.c_str(), (int)chunkStr.size(), NULL, 0);
                            std::wstring wmsg(wlen, 0);
                            MultiByteToWideChar(1251, 0, chunkStr.c_str(), (int)chunkStr.size(), &wmsg[0], wlen);

                            wchar_t* copy = _wcsdup(wmsg.c_str());
                            PostMessage(g_hWndMain, WM_APP_RX_PACKET, 0, (LPARAM)copy);
                            LogDebug(L"Prinyat kadr: %d simvolov", (int)chunkStr.size());
                        }
                    }
                }
            }
        }
    }
    return 0;
}

// =====================================================================
//  RABOTA S COM-PORTAMI I REESTROM
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

static BOOL OpenPortsAuto(DWORD baud)
{
    std::vector<int> avail = EnumerateAvailablePorts();
    for (size_t i = 0; i + 1 < avail.size(); i += 2) {
        HANDLE hTx = TryOpenPort(avail[i], baud);
        if (hTx == INVALID_HANDLE_VALUE) continue;
        HANDLE hRx = TryOpenPort(avail[i + 1], baud);
        if (hRx == INVALID_HANDLE_VALUE) { CloseHandle(hTx); continue; }

        g_hComTx = hTx; g_txNum = avail[i];
        g_hComRx = hRx; g_rxNum = avail[i + 1];
        return TRUE;
    }
    return FALSE;
}

static void ClosePorts()
{
    if (g_hComTx != INVALID_HANDLE_VALUE) { CloseHandle(g_hComTx); g_hComTx = INVALID_HANDLE_VALUE; }
    if (g_hComRx != INVALID_HANDLE_VALUE) { CloseHandle(g_hComRx); g_hComRx = INVALID_HANDLE_VALUE; }
}

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
    swprintf_s(buf, L"TX = COM%d     RX = COM%d  (Flag=0x%02X, DataLen=%d)",
        g_txNum, g_rxNum, FLAG_BYTE, (int)DATA_PAYLOAD_LEN);
    SetWindowTextW(g_hStatusPorts, buf);
}

// =====================================================================
//  RUCHNOE UPRAVLENIE NASTROJKAMI
// =====================================================================

static void ApplyControlSettings()
{
    int baudIdx = (int)SendMessageW(g_hBaudCombo, CB_GETCURSEL, 0, 0);
    static const DWORD baudValues[] = { 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200 };
    DWORD newBaud = (baudIdx >= 0) ? baudValues[baudIdx] : CBR_9600;

    int portSel = (int)SendMessageW(g_hPortCombo, CB_GETCURSEL, 0, 0);
    int newTx = g_txNum, newRx = g_rxNum;
    if (portSel >= 0) {
        wchar_t buf[64];
        SendMessageW(g_hPortCombo, CB_GETLBTEXT, portSel, (LPARAM)buf);
        swscanf_s(buf, L"TX=COM%d RX=COM%d", &newTx, &newRx);
    }

    g_bStopReader = TRUE;
    if (g_hReaderThread) {
        WaitForSingleObject(g_hReaderThread, 500);
        CloseHandle(g_hReaderThread);
        g_hReaderThread = NULL;
    }
    ClosePorts();

    HANDLE hTx = TryOpenPort(newTx, newBaud);
    HANDLE hRx = TryOpenPort(newRx, newBaud);
    if (hTx != INVALID_HANDLE_VALUE && hRx != INVALID_HANDLE_VALUE) {
        g_hComTx = hTx; g_hComRx = hRx;
        g_txNum = newTx; g_rxNum = newRx;
        g_baud = newBaud;
        LogDebug(L"Porty perenastroeny: TX=COM%d RX=COM%d, skorost=%u", newTx, newRx, newBaud);
    }
    else {
        LogDebug(L"Oshibka otkrytiya portov. Vosstanavlivaem starye.");
        if (hTx != INVALID_HANDLE_VALUE) CloseHandle(hTx);
        if (hRx != INVALID_HANDLE_VALUE) CloseHandle(hRx);
        g_hComTx = TryOpenPort(g_txNum, g_baud);
        g_hComRx = TryOpenPort(g_rxNum, g_baud);
    }
    UpdateStatusPorts();

    g_bStopReader = FALSE;
    g_hReaderThread = CreateThread(NULL, 0, ReaderThreadProc, NULL, 0, NULL);
}

// =====================================================================
//  GUI И ОБРАБОТКА СООБЩЕНИЙ
// =====================================================================

static LRESULT CALLBACK InputEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        int len = GetWindowTextLengthW(hwnd);
        std::wstring text(len, L'\0');
        if (len > 0) GetWindowTextW(hwnd, &text[0], len + 1);
        text.resize(len);

        SendPacketizedText(text);
        SetWindowTextW(hwnd, L"");
        return 0;
    }
    return CallWindowProcW(g_oldInputEditProc, hwnd, msg, wParam, lParam);
}

static void LayoutMainWindow(int width, int height)
{
    const int margin = 10;
    const int ctrlH = 140;
    const int inputH = 55;
    const int lowerY = margin + ctrlH + margin + inputH + margin;
    int lowerH = max(100, height - lowerY - margin);

    int leftW = (width - margin * 3) * 3 / 5;
    int rightW = (width - margin * 3) - leftW;

    int ctrlW = 320;
    if (g_hGrpControl) MoveWindow(g_hGrpControl, margin, margin, ctrlW, ctrlH, TRUE);
    if (g_hLblBaud)    MoveWindow(g_hLblBaud, margin + 12, margin + 20, ctrlW - 24, 18, TRUE);
    if (g_hBaudCombo)  MoveWindow(g_hBaudCombo, margin + 12, margin + 40, ctrlW - 24, 24, TRUE);
    if (g_hLblPort)    MoveWindow(g_hLblPort, margin + 12, margin + 70, ctrlW - 24, 18, TRUE);
    if (g_hPortCombo)  MoveWindow(g_hPortCombo, margin + 12, margin + 92, ctrlW - 130, 24, TRUE);
    if (g_hApplyBtn)   MoveWindow(g_hApplyBtn, margin + ctrlW - 110, margin + 90, 95, 26, TRUE);

    int statusX = margin + ctrlW + margin;
    int statusW = width - statusX - margin;
    if (g_hGrpStatus)   MoveWindow(g_hGrpStatus, statusX, margin, statusW, ctrlH, TRUE);
    if (g_hStatusPorts) MoveWindow(g_hStatusPorts, statusX + 12, margin + 22, statusW - 24, 20, TRUE);
    if (g_hStatusFrame) MoveWindow(g_hStatusFrame, statusX + 12, margin + 46, statusW - 24, ctrlH - 56, TRUE);

    int inputY = margin + ctrlH + margin;
    if (g_hLblInput)  MoveWindow(g_hLblInput, margin, inputY, width - margin * 2, 18, TRUE);
    if (g_hInputEdit) MoveWindow(g_hInputEdit, margin, inputY + 20, width - margin * 2, 26, TRUE);

    if (g_hLblOutput)  MoveWindow(g_hLblOutput, margin, lowerY, leftW, 18, TRUE);
    if (g_hOutputEdit) MoveWindow(g_hOutputEdit, margin, lowerY + 20, leftW, lowerH - 20, TRUE);

    int debugX = margin + leftW + margin;
    if (g_hLblDebug)  MoveWindow(g_hLblDebug, debugX, lowerY, rightW, 18, TRUE);
    if (g_hDebugEdit) MoveWindow(g_hDebugEdit, debugX, lowerY + 20, rightW, lowerH - 20, TRUE);
}

static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        g_hGrpControl = CreateWindowW(L"BUTTON", L"Upravlenie", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hLblBaud = CreateWindowW(L"STATIC", L"Skorost porta:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hBaudCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hWnd, (HMENU)ID_BAUD_COMBO, NULL, NULL);
        const wchar_t* bauds[] = { L"1200", L"2400", L"4800", L"9600", L"19200", L"38400", L"57600", L"115200" };
        for (auto b : bauds) SendMessageW(g_hBaudCombo, CB_ADDSTRING, 0, (LPARAM)b);
        SendMessageW(g_hBaudCombo, CB_SETCURSEL, 3, 0);

        g_hLblPort = CreateWindowW(L"STATIC", L"Para COM-portov:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hPortCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hWnd, (HMENU)ID_PORT_COMBO, NULL, NULL);
        g_hApplyBtn = CreateWindowW(L"BUTTON", L"Primenit", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)ID_APPLY_BTN, NULL, NULL);

        g_hGrpStatus = CreateWindowW(L"BUTTON", L"Okno sostoyaniya (struktura kadra)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hStatusPorts = CreateWindowW(L"STATIC", L"TX = ?   RX = ?", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hStatusFrame = CreateWindowW(L"EDIT", L"Zdes otobrazhaetsya tekushchiy kadr posle staffinga...",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);

        g_hLblInput = CreateWindowW(L"STATIC", L"Vvod soobshcheniya (Enter - otpravka paketami):", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hInputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_oldInputEditProc = (WNDPROC)SetWindowLongPtrW(g_hInputEdit, GWLP_WNDPROC, (LONG_PTR)InputEditSubclassProc);

        g_hLblOutput = CreateWindowW(L"STATIC", L"Prinyatye dannye:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hOutputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);

        g_hLblDebug = CreateWindowW(L"STATIC", L"Log operatsiy:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hDebugEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);

        SetFocus(g_hInputEdit);
        return 0;
    }

    case WM_SIZE:
        LayoutMainWindow(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_APPLY_BTN && HIWORD(wParam) == BN_CLICKED) {
            ApplyControlSettings();
            SetFocus(g_hInputEdit);
        }
        return 0;

    case WM_APP_RX_PACKET: {
        wchar_t* text = (wchar_t*)lParam;
        int len = GetWindowTextLengthW(g_hOutputEdit);
        SendMessageW(g_hOutputEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageW(g_hOutputEdit, EM_REPLACESEL, FALSE, (LPARAM)text);
        free(text);
        return 0;
    }

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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Lab2MainWnd";
    RegisterClassExW(&wc);

    g_hWndMain = CreateWindowW(L"Lab2MainWnd", L"Lab 2: Paketnaya peredacha i Bit-staffing",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 980, 650, NULL, NULL, hInstance, NULL);

    ShowWindow(g_hWndMain, nCmdShow);
    UpdateWindow(g_hWndMain);

    if (!OpenPortsAuto(g_baud)) {
        MessageBoxW(g_hWndMain, L"Ne udalos otkryt svobodnye COM-porty!", L"Oshibka", MB_ICONERROR);
        return 1;
    }
    UpdateStatusPorts();

    // Zapolnenie vypadayushchego spiska parami portov
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
    return 0;
}