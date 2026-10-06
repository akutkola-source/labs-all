// =====================================================================
//  Лабораторная работа No.3. Контроль целостности и исправление ошибок.
//  Базовая основа: Лабораторная работа No.2 (COM-порты, Бит-стаффинг).
//  Вариант: ЧЁТНЫЙ
//    - Номер студента (n): 10
//    - Структура кадра: Flag (8B) | Dest (4B) | Src (4B) | Data (n+1 B) | FCS (1B)
//    - Флаг: двоичное представление (128 + n) = 138 (0x8A)
//    - Контрольный код: Циклический код (деление полиномов "как на бумаге")
//    - Порождающий полином: G(x) = x^8 + x^4 + x^3 + x^2 + 1 (0x11D)
//    - Внесение помех: случайное искажение 1 бита в поле Data после приёма (35%)
//    - Окно состояния: 1 кадр = 1 строка, выделение поля FCS и битов стаффинга
// =====================================================================

#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
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
constexpr size_t DATA_PAYLOAD_LEN = STUDENT_N + 1; // 11 байт (88 бит)
constexpr size_t FCS_LEN = 1;                      // Округлено до 1 байта (8 бит)
constexpr uint8_t FLAG_BYTE = (uint8_t)(128 + STUDENT_N); // 0x8A

// Порождающий полином G(x) = x^8 + x^4 + x^3 + x^2 + 1 (степень 8 -> 9 бит)
static const std::vector<uint8_t> G_POLY = { 1, 0, 0, 0, 1, 1, 1, 0, 1 };

// Пользовательские оконные сообщения
#define WM_APP_RX_PACKET (WM_APP + 1)
#define WM_APP_DEBUG     (WM_APP + 3)

static HANDLE g_hComTx = INVALID_HANDLE_VALUE;
static HANDLE g_hComRx = INVALID_HANDLE_VALUE;
static int    g_txNum = -1;
static int    g_rxNum = -1;
static DWORD  g_baud = CBR_9600;
static HANDLE g_hReaderThread = NULL;
static volatile BOOL g_bStopReader = FALSE;

// Шрифт с поддержкой русского языка
static HFONT  g_hAppFont = NULL;

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
//  ЦИКЛИЧЕСКИЙ КОД: ДЕЛЕНИЕ ПОЛИНОМОВ СТОЛБИКОМ ("КАК НА БУМАГЕ")
// =====================================================================

static std::vector<uint8_t> PolyDivRemainder(const std::vector<uint8_t>& dividend, const std::vector<uint8_t>& divisor)
{
    std::vector<uint8_t> rem = dividend;
    size_t degG = divisor.size() - 1;

    for (size_t i = 0; i + degG < rem.size(); ++i) {
        if (rem[i] == 1) {
            for (size_t j = 0; j <= degG; ++j) {
                rem[i + j] ^= divisor[j];
            }
        }
    }
    return std::vector<uint8_t>(rem.end() - degG, rem.end());
}

static std::vector<uint8_t> BytesToBits(const std::vector<uint8_t>& bytes)
{
    std::vector<uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (uint8_t b : bytes) {
        for (int i = 7; i >= 0; --i) {
            bits.push_back((b >> i) & 1);
        }
    }
    return bits;
}

static uint8_t BitsToByte(const std::vector<uint8_t>& bits)
{
    uint8_t res = 0;
    for (size_t i = 0; i < bits.size() && i < 8; ++i) {
        res = (res << 1) | (bits[i] ? 1 : 0);
    }
    return res;
}

static uint8_t ComputeFCS(const std::vector<uint8_t>& dataField)
{
    std::vector<uint8_t> bits = BytesToBits(dataField);
    bits.insert(bits.end(), 8, 0); // Умножение на x^8
    std::vector<uint8_t> remainder = PolyDivRemainder(bits, G_POLY);
    return BitsToByte(remainder);
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

static bool VerifyAndCorrect(std::vector<uint8_t>& dataField, uint8_t rxFCS)
{
    std::vector<uint8_t> codewordBits = BytesToBits(dataField);
    for (int i = 7; i >= 0; --i) codewordBits.push_back((rxFCS >> i) & 1);

    std::vector<uint8_t> syndrome = PolyDivRemainder(codewordBits, G_POLY);

    bool isZero = true;
    for (uint8_t b : syndrome) {
        if (b != 0) { isZero = false; break; }
    }

    if (isZero) {
        LogDebug(L"[CRC] Синдром равен 0: Ошибок нет.");
        return true;
    }

    int errorBitPos = -1;
    for (size_t i = 0; i < g_syndromeTable.size(); ++i) {
        if (g_syndromeTable[i] == syndrome) {
            errorBitPos = (int)i;
            break;
        }
    }

    if (errorBitPos >= 0) {
        if (errorBitPos < (int)(DATA_PAYLOAD_LEN * 8)) {
            int byteIdx = errorBitPos / 8;
            int bitIdx = 7 - (errorBitPos % 8);
            dataField[byteIdx] ^= (1 << bitIdx);
            LogDebug(L"[CRC] ОБНАРУЖЕНА И ИСПРАВЛЕНА ошибка! Бит: %d (Байт %d, бит %d)",
                errorBitPos, byteIdx, bitIdx);
        }
        else {
            LogDebug(L"[CRC] Ошибка в FCS (бит %d). Данные целы.", errorBitPos);
        }
        return true;
    }

    LogDebug(L"[CRC] Неисправимая многократная ошибка!");
    return false;
}

// =====================================================================
//  БИТ-СТАФФИНГ И ДЕСТАФФИНГ
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
                consecutiveOnes++;
                if (consecutiveOnes == 5) {
                    bits.push_back(false);
                    outVisualBits += L"[0]";
                    consecutiveOnes = 0;
                }
            }
            else {
                consecutiveOnes = 0;
            }
        }
    }

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
//  СБОРКА, ОТОБРАЖЕНИЕ И ПЕРЕДАЧА КАДРА
// =====================================================================

static void SendFrame(const std::vector<uint8_t>& chunk)
{
    if (g_hComTx == INVALID_HANDLE_VALUE) return;

    std::vector<uint8_t> dataField(DATA_PAYLOAD_LEN, 0x00);
    size_t copyLen = min(chunk.size(), DATA_PAYLOAD_LEN);
    memcpy(dataField.data(), chunk.data(), copyLen);

    uint8_t fcs = ComputeFCS(dataField);

    std::vector<uint8_t> body;
    body.insert(body.end(), 4, 0x00);

    uint32_t src = (uint32_t)g_txNum;
    body.push_back((src >> 24) & 0xFF);
    body.push_back((src >> 16) & 0xFF);
    body.push_back((src >> 8) & 0xFF);
    body.push_back(src & 0xFF);

    body.insert(body.end(), dataField.begin(), dataField.end());
    body.push_back(fcs);

    std::wstring visualBits;
    std::vector<uint8_t> stuffedBody = BitStuffing(body, visualBits);

    std::vector<uint8_t> frame;
    frame.insert(frame.end(), 8, FLAG_BYTE);
    frame.insert(frame.end(), stuffedBody.begin(), stuffedBody.end());
    frame.insert(frame.end(), 8, FLAG_BYTE);

    wchar_t statusLine[1024];
    swprintf_s(statusLine,
        L"[FLAG: 0x%02X x8] [DEST: 0] [SRC: COM%d] [DATA: %d B] [=== FCS: 0x%02X ===] | Биты: %ls\r\n",
        FLAG_BYTE, g_txNum, (int)DATA_PAYLOAD_LEN, fcs, visualBits.c_str());

    int sLen = GetWindowTextLengthW(g_hStatusFrame);
    SendMessageW(g_hStatusFrame, EM_SETSEL, (WPARAM)sLen, (LPARAM)sLen);
    SendMessageW(g_hStatusFrame, EM_REPLACESEL, FALSE, (LPARAM)statusLine);

    DWORD written = 0;
    WriteFile(g_hComTx, frame.data(), (DWORD)frame.size(), &written, NULL);
    LogDebug(L"Отправлен кадр: %d байт (FCS=0x%02X)", (int)frame.size(), fcs);
}

static void SendPacketizedText(const std::wstring& text)
{
    std::string ansi;
    int len = WideCharToMultiByte(1251, 0, text.c_str(), (int)text.size(), NULL, 0, NULL, NULL);
    if (len > 0) {
        ansi.resize(len);
        WideCharToMultiByte(1251, 0, text.c_str(), (int)text.size(), &ansi[0], len, NULL, NULL);
    }

    std::vector<uint8_t> bytes(ansi.begin(), ansi.end());
    if (bytes.empty()) bytes.push_back('\0');

    for (size_t offset = 0; offset < bytes.size(); offset += DATA_PAYLOAD_LEN) {
        size_t curSize = min(DATA_PAYLOAD_LEN, bytes.size() - offset);
        std::vector<uint8_t> chunk(bytes.begin() + offset, bytes.begin() + offset + curSize);
        SendFrame(chunk);
        Sleep(40);
    }
}

// =====================================================================
//  ПОТОК ПРИЁМА
// =====================================================================

static DWORD WINAPI ReaderThreadProc(LPVOID)
{
    std::vector<uint8_t> rxBuffer;
    const size_t rawBodyLen = 4 + 4 + DATA_PAYLOAD_LEN + FCS_LEN;

    while (!g_bStopReader) {
        if (g_hComRx == INVALID_HANDLE_VALUE) { Sleep(50); continue; }

        BYTE b;
        DWORD read = 0;
        if (!ReadFile(g_hComRx, &b, 1, &read, NULL) || read == 0) {
            Sleep(10);
            continue;
        }

        rxBuffer.push_back(b);

        if (rxBuffer.size() >= 8) {
            bool isFlag = true;
            for (size_t i = rxBuffer.size() - 8; i < rxBuffer.size(); ++i) {
                if (rxBuffer[i] != FLAG_BYTE) { isFlag = false; break; }
            }

            if (isFlag) {
                std::vector<uint8_t> candidate(rxBuffer.begin(), rxBuffer.end() - 8);
                rxBuffer.clear();

                while (candidate.size() >= 8) {
                    bool leadingFlag = true;
                    for (int i = 0; i < 8; ++i) {
                        if (candidate[i] != FLAG_BYTE) { leadingFlag = false; break; }
                    }
                    if (leadingFlag) candidate.erase(candidate.begin(), candidate.begin() + 8);
                    else break;
                }

                if (!candidate.empty()) {
                    std::vector<uint8_t> raw = BitDestuffing(candidate);

                    if (raw.size() > rawBodyLen) {
                        raw.resize(rawBodyLen);
                    }

                    if (raw.size() == rawBodyLen) {
                        size_t dataStart = 8;
                        std::vector<uint8_t> dataField(raw.begin() + dataStart, raw.begin() + dataStart + DATA_PAYLOAD_LEN);
                        uint8_t rxFCS = raw[dataStart + DATA_PAYLOAD_LEN];

                        // Случайная помеха (вероятность 35%)
                        if ((rand() % 100) < 35) {
                            int errByte = rand() % DATA_PAYLOAD_LEN;
                            int errBit = rand() % 8;
                            dataField[errByte] ^= (1 << errBit);
                            LogDebug(L"[ПОМЕХА 35%%] Искажён бит %d в байте Data[%d]!", errBit, errByte);
                        }

                        VerifyAndCorrect(dataField, rxFCS);

                        std::string chunkStr;
                        for (size_t i = 0; i < DATA_PAYLOAD_LEN; ++i) {
                            if (dataField[i] != '\0') chunkStr.push_back((char)dataField[i]);
                        }

                        if (!chunkStr.empty()) {
                            int wlen = MultiByteToWideChar(1251, 0, chunkStr.data(), (int)chunkStr.size(), NULL, 0);
                            if (wlen > 0) {
                                std::wstring wmsg(wlen, L'\0');
                                MultiByteToWideChar(1251, 0, chunkStr.data(), (int)chunkStr.size(), &wmsg[0], wlen);

                                wchar_t* copy = _wcsdup(wmsg.c_str());
                                PostMessage(g_hWndMain, WM_APP_RX_PACKET, 0, (LPARAM)copy);
                                LogDebug(L"Принят пакет: \"%ls\"", wmsg.c_str());
                            }
                        }
                    }
                }
            }
        }
    }
    return 0;
}

// =====================================================================
//  РАБОТА С COM-ПОРТАМИ
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
    swprintf_s(buf, L"TX = COM%d     RX = COM%d  (Flag=0x%02X, DataLen=%d, FCS=1B)",
        g_txNum, g_rxNum, FLAG_BYTE, (int)DATA_PAYLOAD_LEN);
    SetWindowTextW(g_hStatusPorts, buf);
}

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
        LogDebug(L"Порты настроены: TX=COM%d RX=COM%d, %u бод", newTx, newRx, newBaud);
    }
    else {
        LogDebug(L"Ошибка открытия портов. Восстановление исходных настроек.");
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
        if (len > 0) {
            std::vector<wchar_t> buf(len + 1, L'\0');
            GetWindowTextW(hwnd, buf.data(), len + 1);
            SendPacketizedText(std::wstring(buf.data()));
            SetWindowTextW(hwnd, L"");
        }
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
        // Создание шрифта с поддержкой кириллицы (Segoe UI)
        g_hAppFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        g_hGrpControl = CreateWindowW(L"BUTTON", L"Управление", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hLblBaud = CreateWindowW(L"STATIC", L"Скорость порта:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hBaudCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hWnd, (HMENU)ID_BAUD_COMBO, NULL, NULL);
        const wchar_t* bauds[] = { L"1200", L"2400", L"4800", L"9600", L"19200", L"38400", L"57600", L"115200" };
        for (auto b : bauds) SendMessageW(g_hBaudCombo, CB_ADDSTRING, 0, (LPARAM)b);
        SendMessageW(g_hBaudCombo, CB_SETCURSEL, 3, 0);

        g_hLblPort = CreateWindowW(L"STATIC", L"Пара COM-портов:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hPortCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hWnd, (HMENU)ID_PORT_COMBO, NULL, NULL);
        g_hApplyBtn = CreateWindowW(L"BUTTON", L"Применить", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)ID_APPLY_BTN, NULL, NULL);

        g_hGrpStatus = CreateWindowW(L"BUTTON", L"Окно состояния (структура кадра, FCS и стаффинг)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hStatusPorts = CreateWindowW(L"STATIC", L"TX = ?   RX = ?", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hStatusFrame = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_HSCROLL | ES_AUTOHSCROLL | ES_AUTOVSCROLL,
            0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(g_hStatusFrame, EM_SETLIMITTEXT, 0, 0);

        g_hLblInput = CreateWindowW(L"STATIC", L"Ввод сообщения (Enter — отправка пакетами):", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hInputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_oldInputEditProc = (WNDPROC)SetWindowLongPtrW(g_hInputEdit, GWLP_WNDPROC, (LONG_PTR)InputEditSubclassProc);

        g_hLblOutput = CreateWindowW(L"STATIC", L"Принятые данные:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hOutputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);

        g_hLblDebug = CreateWindowW(L"STATIC", L"Журнал операций (CRC и исправление ошибок):", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hDebugEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);

        // Установка шрифта Segoe UI во все элементы управления
        const HWND controls[] = {
            g_hGrpControl, g_hLblBaud, g_hBaudCombo, g_hLblPort, g_hPortCombo, g_hApplyBtn,
            g_hGrpStatus, g_hStatusPorts, g_hStatusFrame, g_hLblInput, g_hInputEdit,
            g_hLblOutput, g_hOutputEdit, g_hLblDebug, g_hDebugEdit
        };
        for (HWND hCtrl : controls) {
            if (hCtrl) SendMessageW(hCtrl, WM_SETFONT, (WPARAM)g_hAppFont, TRUE);
        }

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
        if (g_hAppFont) DeleteObject(g_hAppFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    // Инициализация русской системной локали
    setlocale(LC_ALL, "Russian");
    srand((unsigned int)time(NULL));
    InitSyndromeTable();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Lab3MainWnd";
    RegisterClassExW(&wc);

    g_hWndMain = CreateWindowW(L"Lab3MainWnd", L"Лабораторная работа №3: Циклический код и контроль ошибок",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1020, 680, NULL, NULL, hInstance, NULL);

    ShowWindow(g_hWndMain, nCmdShow);
    UpdateWindow(g_hWndMain);

    if (!OpenPortsAuto(g_baud)) {
        MessageBoxW(g_hWndMain, L"Не удалось открыть виртуальные COM-порты!", L"Ошибка", MB_ICONERROR);
        return 1;
    }
    UpdateStatusPorts();

    std::vector<int> avail = EnumerateAvailablePorts();
    for (size_t i = 0; i + 1 < avail.size(); i += 2) {
        wchar_t buf[64];
        swprintf_s(buf, L"TX=COM%d RX=COM%d", avail[i], avail[i + 1]);
        int idx = (int)SendMessageW(g_hPortCombo, CB_ADDSTRING, 0, (LPARAM)buf);
        if (avail[i] == g_txNum && avail[i + 1] == g_rxNum)
            SendMessageW(g_hPortCombo, CB_SETCURSEL, idx, 0);
    }

    g_hReaderThread = CreateThread(NULL, 0, ReaderThreadProc, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}