// Лабораторная работа №3 «Устройства хранения данных»
// Вариант А4. Сравнительный анализатор HDD vs SSD (Windows, Win32 API, C++)
//
// Используются только системные интерфейсы: SetupAPI, DeviceIoControl
// (IOCTL_STORAGE_QUERY_PROPERTY, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
// IOCTL_ATA_PASS_THROUGH, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS), GDI, Common Controls.
// WMI, .NET и сторонние библиотеки не применяются.
//
// Запуск:  HddSsdCompare.exe                 — графический интерфейс
//          HddSsdCompare.exe /report <файл>  — без окна: сформировать отчёт (.txt или .html)

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <nvme.h>
#include <setupapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dbt.h>
#include <shellapi.h>

#include <algorithm>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstddef>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// GUID_DEVINTERFACE_DISK {53F56307-B6BF-11D0-94F2-00A0C91EFB8B}
// GUID класса интерфейса «дисковое устройство». По нему SetupAPI перечисляет все
// физические диски, присутствующие в системе .
static const GUID kGuidDisk = {0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

// ---------------------------------------------------------------------------
// Пороговые значения для рекомендаций
// ---------------------------------------------------------------------------
const int kWearReplace = 90;   // PercentageUsed > 90 % — замена
const int kWearWarn = 80;      // PercentageUsed > 80 % — предупреждение
const int kTempCrit = 75;      // > 75 °C — перегрев
const int kTempWarn = 60;      // > 60 °C — повышенная температура
const int kSpareMin = 10;      // AvailableSpare < 10 % — критично
const long long kHddOldHours = 40000;  // ~4,5 года непрерывной работы
const int kFillWarn = 90;      // заполнение тома > 90 %

// Уровни оценки состояния: используются и для цветовой подсветки ячеек
// (зелёный / жёлтый / красный), и для итоговой оценки диска.
enum Level { LV_NONE = 0, LV_OK = 1, LV_WARN = 2, LV_CRIT = 3 };

// ---------------------------------------------------------------------------
// Модель данных
// ---------------------------------------------------------------------------
// Один атрибут SMART (запись из 12 байт в ответе SMART READ DATA):
//   id     — номер атрибута (например, 0x09 — Power-On Hours);
//   cur    — текущее нормализованное значение (обычно 1..253, чем больше — тем лучше);
//   worst  — наихудшее нормализованное значение за всё время;
//   thresh — порог из SMART READ THRESHOLDS: если cur <= thresh, атрибут «отказал»;
//   raw    — «сырое» 48-битное значение (счётчик часов, секторов, температура и т. п.).
struct SmartAttr {
    BYTE id = 0, cur = 0, worst = 0, thresh = 0;
    ULONGLONG raw = 0;
};

// Логический том (буква диска), расположенный на физическом диске.
struct VolumeInfo {
    wchar_t letter = L'?';
    ULONGLONG total = 0, freeBytes = 0;
    std::wstring fs, label;
    int UsedPercent() const { return total ? (int)((total - freeBytes) * 100 / total) : 0; }
};

// Тип носителя, который определяет функция Classify().
enum class Media { Unknown, HDD, SSD };

// Все сведения об одном физическом диске \\.\PhysicalDriveN.
// Значение -1 (или INT_MIN для температуры) означает «данные не получены».
struct DiskInfo {
    int number = -1;
    std::wstring vendor, model, serial, firmware;
    STORAGE_BUS_TYPE bus = BusTypeUnknown;
    bool removable = false, rwAccess = false;
    ULONGLONG size = 0;
    DWORD sector = 0;

    int seekPenalty = -1;  // StorageDeviceSeekPenaltyProperty: 1 — есть штраф (HDD), 0 — нет
    int trim = -1;         // StorageDeviceTrimProperty: 1 — TRIM включён
    int rpm = -1;          // IDENTIFY word 217: 0 — не сообщается, 1 — не вращается (SSD), >1 — об/мин
    int identTrim = -1;    // IDENTIFY word 169 bit 0 — поддержка DATA SET MANAGEMENT (TRIM)
    int sataSpeed = 0;     // текущая скорость SATA-канала, Мбит/с (IDENTIFY word 77)
    Media kind = Media::Unknown;
    std::wstring kindReason;

    int temp = INT_MIN;
    std::wstring tempSrc;
    long long poh = -1, cycles = -1, realloc = -1, pending = -1, unsafe = -1, mediaErr = -1;
    int wear = -1;
    std::wstring wearSrc;
    int spare = -1, spareThr = -1, critWarn = -1;
    double readGB = -1, writtenGB = -1;
    int smartStatus = -1;  // 1 — норма, 0 — превышен порог (SMART RETURN STATUS)
    bool identifyOk = false, smartOk = false, nvmeOk = false;

    std::vector<SmartAttr> attrs;
    std::vector<VolumeInfo> vols;
    std::vector<std::wstring> notes;

    std::wstring Key() const { return model + L"|" + serial; }
};

// Одна рекомендация: уровень (LV_*) и текст для пользователя.
struct Advice {
    int level;
    std::wstring text;
};

// ---------------------------------------------------------------------------
// Вспомогательные функции форматирования
// ---------------------------------------------------------------------------
// Форматирование строки в стиле printf для широких строк (wchar_t).
static std::wstring Fmt(const wchar_t* f, ...) {
    wchar_t b[2048];
    va_list a;
    va_start(a, f);
    _vsnwprintf_s(b, _countof(b), _TRUNCATE, f, a);
    va_end(a);
    return b;
}

// Удаление пробелов и нулевых символов по краям строки
// (строки в дескрипторах устройств дополняются пробелами).
static std::wstring Trim(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == 0)) s.pop_back();
    size_t p = 0;
    while (p < s.size() && (s[p] == L' ' || s[p] == L'\t')) ++p;
    return s.substr(p);
}

// Преобразование ASCII-строки из ответа драйвера в std::wstring.
static std::wstring FromAscii(const char* s, size_t maxLen) {
    std::wstring r;
    for (size_t i = 0; i < maxLen && s[i]; ++i) r += (wchar_t)(unsigned char)s[i];
    return Trim(r);
}

// Перевод строки в UTF-8 — в этой кодировке сохраняется файл отчёта.
static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Текущее время (и дата) для журнала событий и заголовка отчёта.
static std::wstring NowStr(bool withDate) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (withDate)
        return Fmt(L"%02d.%02d.%04d %02d:%02d:%02d", t.wDay, t.wMonth, t.wYear, t.wHour, t.wMinute, t.wSecond);
    return Fmt(L"%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
}

// Размер в двоичных (ГиБ/ТиБ) и десятичных (GB) единицах. Производители указывают
// объём в десятичных гигабайтах, Windows показывает двоичные — отсюда «512 GB = 476,9 ГБ».
static std::wstring FmtBytes(ULONGLONG b) {
    if (!b) return L"н/д";
    double gib = b / 1073741824.0;
    if (gib >= 1024) return Fmt(L"%.2f ТБ (%.0f GB)", gib / 1024, b / 1e9);
    return Fmt(L"%.1f ГБ (%.0f GB)", gib, b / 1e9);
}

static std::wstring FmtShortBytes(ULONGLONG b) {
    double gib = b / 1073741824.0;
    if (gib >= 1024) return Fmt(L"%.2f ТБ", gib / 1024);
    return Fmt(L"%.1f ГБ", gib);
}

// Название шины по значению STORAGE_BUS_TYPE из STORAGE_DEVICE_DESCRIPTOR.
static std::wstring BusName(STORAGE_BUS_TYPE t) {
    switch (t) {
    case BusTypeScsi: return L"SCSI";
    case BusTypeAtapi: return L"ATAPI";
    case BusTypeAta: return L"ATA (PATA)";
    case BusType1394: return L"IEEE 1394";
    case BusTypeSsa: return L"SSA";
    case BusTypeFibre: return L"Fibre Channel";
    case BusTypeUsb: return L"USB";
    case BusTypeRAID: return L"RAID";
    case BusTypeiScsi: return L"iSCSI";
    case BusTypeSas: return L"SAS";
    case BusTypeSata: return L"SATA";
    case BusTypeSd: return L"SD";
    case BusTypeMmc: return L"MMC";
    case BusTypeVirtual: return L"Virtual";
    case BusTypeFileBackedVirtual: return L"VHD";
    case BusTypeSpaces: return L"Storage Spaces";
    case BusTypeNvme: return L"NVMe (PCIe)";
    case BusTypeSCM: return L"SCM";
    case BusTypeUfs: return L"UFS";
    default: return L"неизвестно";
    }
}

static std::wstring MediaName(Media m) {
    return m == Media::SSD ? L"SSD" : m == Media::HDD ? L"HDD" : L"не определён";
}

// Интерфейс + текущая скорость SATA-канала (если удалось прочитать IDENTIFY).
static std::wstring InterfaceStr(const DiskInfo& d) {
    std::wstring s = BusName(d.bus);
    if (d.sataSpeed) s += Fmt(L", канал %.1f Гбит/с", d.sataSpeed / 1000.0);
    return s;
}

static std::wstring TempStr(const DiskInfo& d) {
    return d.temp == INT_MIN ? L"н/д" : Fmt(L"%d °C", d.temp);
}

static std::wstring LevelName(int l) {
    switch (l) {
    case LV_OK: return L"Исправен";
    case LV_WARN: return L"Предупреждение";
    case LV_CRIT: return L"Критично";
    default: return L"Нет данных";
    }
}

// Названия наиболее распространённых атрибутов SMART (для таблицы в отчёте).
// Атрибуты 0xA9–0xE9 — специфичные для SSD (у разных производителей разные).
static const wchar_t* SmartName(BYTE id) {
    switch (id) {
    case 0x01: return L"Raw Read Error Rate";
    case 0x02: return L"Throughput Performance";
    case 0x03: return L"Spin-Up Time";
    case 0x04: return L"Start/Stop Count";
    case 0x05: return L"Reallocated Sectors Count";
    case 0x07: return L"Seek Error Rate";
    case 0x08: return L"Seek Time Performance";
    case 0x09: return L"Power-On Hours";
    case 0x0A: return L"Spin Retry Count";
    case 0x0C: return L"Power Cycle Count";
    case 0xA9: return L"Remaining Life";
    case 0xAA: return L"Available Reserved Space";
    case 0xAB: return L"Program Fail Count";
    case 0xAC: return L"Erase Fail Count";
    case 0xAD: return L"Wear Leveling Count";
    case 0xAE: return L"Unexpected Power Loss";
    case 0xB1: return L"Wear Range Delta / Wear Leveling";
    case 0xB3: return L"Used Reserved Block Count";
    case 0xB5: return L"Program Fail Count";
    case 0xB6: return L"Erase Fail Count";
    case 0xB7: return L"Runtime Bad Block";
    case 0xB8: return L"End-to-End Error";
    case 0xBB: return L"Reported Uncorrectable Errors";
    case 0xBC: return L"Command Timeout";
    case 0xBD: return L"High Fly Writes";
    case 0xBE: return L"Airflow Temperature";
    case 0xBF: return L"G-Sense Error Rate";
    case 0xC0: return L"Power-off Retract Count";
    case 0xC1: return L"Load Cycle Count";
    case 0xC2: return L"Temperature";
    case 0xC3: return L"Hardware ECC Recovered";
    case 0xC4: return L"Reallocation Event Count";
    case 0xC5: return L"Current Pending Sector Count";
    case 0xC6: return L"Offline Uncorrectable";
    case 0xC7: return L"UltraDMA CRC Error Count";
    case 0xC8: return L"Write Error Rate";
    case 0xCA: return L"Percent Lifetime Remaining";
    case 0xE7: return L"SSD Life Left";
    case 0xE8: return L"Available Reserved Space";
    case 0xE9: return L"Media Wearout Indicator";
    case 0xF1: return L"Total LBAs Written";
    case 0xF2: return L"Total LBAs Read";
    default: return L"Vendor specific";
    }
}

// Функции «серьёзности» значения: по порогам возвращают LV_OK / LV_WARN / LV_CRIT.
// Используются для цветовой подсветки ячеек таблицы сравнения.
static int SevTemp(int t) { return t == INT_MIN ? LV_NONE : t > kTempCrit ? LV_CRIT : t > kTempWarn ? LV_WARN : LV_OK; }
static int SevWear(int w) { return w < 0 ? LV_NONE : w > kWearReplace ? LV_CRIT : w > kWearWarn ? LV_WARN : LV_OK; }
static int SevSpare(const DiskInfo& d) {
    if (d.spare < 0) return LV_NONE;
    if (d.spare < kSpareMin || (d.spareThr > 0 && d.spare < d.spareThr)) return LV_CRIT;
    return d.spare < 20 ? LV_WARN : LV_OK;
}
static int SevCount(long long v, long long crit) { return v < 0 ? LV_NONE : v >= crit ? LV_CRIT : v > 0 ? LV_WARN : LV_OK; }

// ---------------------------------------------------------------------------
// Низкоуровневый доступ к накопителям ИЩЕМ ВСЕ ФИЗ НАКОПИТЕЛИ
// ---------------------------------------------------------------------------
// Перечисление физических дисков через SetupAPI.
// 1) SetupDiGetClassDevs — получить набор всех присутствующих (DIGCF_PRESENT) устройств
//    с интерфейсом «диск»;
// 2) SetupDiEnumDeviceInterfaces — перебрать интерфейсы по одному;
// 3) SetupDiGetDeviceInterfaceDetail — получить путь к устройству (\\?\scsi#disk&ven_...);
// 4) открыть устройство и запросом IOCTL_STORAGE_GET_DEVICE_NUMBER узнать номер N,
//    по которому диск далее открывается как \\.\PhysicalDriveN.
// Возвращает отсортированный список номеров дисков.
static std::vector<int> EnumPhysicalDisks() {
    std::vector<int> nums;
    HDEVINFO hInfo = SetupDiGetClassDevsW(&kGuidDisk, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hInfo == INVALID_HANDLE_VALUE) return nums;
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hInfo, nullptr, &kGuidDisk, i, &ifd); ++i) {
        DWORD need = 0;
        // Первый вызов с нулевым буфером возвращает требуемый размер структуры.
        SetupDiGetDeviceInterfaceDetailW(hInfo, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need);
        auto det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hInfo, &ifd, det, need, nullptr, nullptr)) continue;
        // Доступ 0 (без чтения/записи) достаточен для служебных запросов IOCTL.
        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER n = {};
        DWORD br = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &n, sizeof(n), &br, nullptr))
            nums.push_back((int)n.DeviceNumber);
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(hInfo);
    std::sort(nums.begin(), nums.end());
    nums.erase(std::unique(nums.begin(), nums.end()), nums.end());
    return nums;
}

// Обёртка над IOCTL_STORAGE_QUERY_PROPERTY: запрашивает у драйвера одно свойство
// устройства (PropertyId) в стандартном режиме (PropertyStandardQuery).
// Результат записывается в out; got — фактическое число полученных байт.
static bool QueryProperty(HANDLE h, STORAGE_PROPERTY_ID id, void* out, DWORD outSize, DWORD* got = nullptr) {
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = id;
    q.QueryType = PropertyStandardQuery;
    DWORD br = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), out, outSize, &br, nullptr);
    if (got) *got = br;
    return ok && br > 0;
}

// ATA_PASS_THROUGH_EX — заголовок запроса, после которого в том же буфере
// располагаются данные. Поле DataBufferOffset указывает смещение данных от начала буфера.
// Буфер для IOCTL_ATA_PASS_THROUGH: заголовок + 512 байт данных
struct AtaBuffer {
    ATA_PASS_THROUGH_EX apt;
    ULONG filler;
    BYTE data[512];
};

// Выполнение одной ATA-команды через IOCTL_ATA_PASS_THROUGH.
// Массив regs — значения «регистров задачи» (task file), как при работе с портами ATA:
// regs: [0]Features [1]SectorCount [2]SectorNumber [3]CylLow [4]CylHigh [5]DevHead [6]Command
static bool AtaCommand(HANDLE h, const BYTE regs[7], bool dataIn, BYTE* data512, BYTE outRegs[8] = nullptr) {
    // Заполняем заголовок запроса:
    //  AtaFlags: DRDY_REQUIRED — устройство должно быть готово (бит DRDY),
    //            DATA_IN — данные передаются от устройства к хосту (чтение 512 байт);
    //  TimeOutValue — тайм-аут выполнения команды в секундах.
    AtaBuffer b = {};
    b.apt.Length = sizeof(ATA_PASS_THROUGH_EX);
    b.apt.AtaFlags = ATA_FLAGS_DRDY_REQUIRED | (dataIn ? ATA_FLAGS_DATA_IN : 0);
    b.apt.DataTransferLength = dataIn ? 512 : 0;
    b.apt.TimeOutValue = 5;
    b.apt.DataBufferOffset = offsetof(AtaBuffer, data);
    memcpy(b.apt.CurrentTaskFile, regs, 7);
    DWORD br = 0;
    // Входной и выходной буферы совпадают: драйвер возвращает в нём и данные,
    // и содержимое регистров после выполнения команды (CurrentTaskFile).
    if (!DeviceIoControl(h, IOCTL_ATA_PASS_THROUGH, &b, sizeof(b), &b, sizeof(b), &br, nullptr)) return false;
    // CurrentTaskFile[6] после выполнения содержит регистр состояния (Status).
    if (b.apt.CurrentTaskFile[6] & 0x01) return false;  // бит ERR регистра состояния
    if (dataIn && data512) memcpy(data512, b.data, 512);
    if (outRegs) memcpy(outRegs, b.apt.CurrentTaskFile, 8);
    return true;
}

// Строки в ответе IDENTIFY DEVICE хранятся словами по 2 символа, причём в каждом
// слове старший байт — первый символ. Поэтому байты внутри слова переставляются.
static std::wstring IdentString(const WORD* w, int first, int last) {
    std::wstring s;
    for (int i = first; i <= last; ++i) {
        s += (wchar_t)(w[i] >> 8);
        s += (wchar_t)(w[i] & 0xFF);
    }
    return Trim(s);
}

// Команда ATA IDENTIFY DEVICE (код 0xEC) возвращает 256 слов (512 байт) с описанием
// накопителя. Используемые слова:
//   10–19  серийный номер,      23–26  версия прошивки,   27–46  модель;
//   76–77  возможности SATA и текущая скорость канала;
//   82, 85 поддержка/включение SMART;
//   169    бит 0 — поддержка DATA SET MANAGEMENT (TRIM);
//   217    Nominal Media Rotation Rate: 1 — не вращается (SSD), 0x0401–0xFFFE — об/мин.
// Регистр Device/Head = 0xA0 (биты 7 и 5 исторически равны 1, бит DEV = 0 — ведущее).
static void ReadAtaIdentify(HANDLE h, DiskInfo& d) {
    BYTE regs[7] = {0, 0, 0, 0, 0, 0xA0, 0xEC};  // IDENTIFY DEVICE
    WORD w[256];
    if (!AtaCommand(h, regs, true, (BYTE*)w)) return;
    d.identifyOk = true;
    if (d.model.empty()) d.model = IdentString(w, 27, 46);
    if (d.serial.empty()) d.serial = IdentString(w, 10, 19);
    if (d.firmware.empty()) d.firmware = IdentString(w, 23, 26);
    // Скорость вращения — главный «аппаратный» признак HDD/SSD (аналог RPM-свойства).
    WORD rot = w[217];
    if (rot == 1) d.rpm = 1;
    else if (rot >= 0x0401 && rot <= 0xFFFE) d.rpm = rot;
    else d.rpm = 0;
    if (w[169] != 0xFFFF) d.identTrim = (w[169] & 1) ? 1 : 0;
    if (w[76] != 0 && w[76] != 0xFFFF) {
        static const int speeds[] = {0, 1500, 3000, 6000};
    // Слово 77, биты 1–3: текущая скорость: 1 — 1,5 Гбит/с, 2 — 3 Гбит/с, 3 — 6 Гбит/с.
        int code = (w[77] >> 1) & 7;
        if (code >= 1 && code <= 3) d.sataSpeed = speeds[code];
    }
    if (!(w[85] & 1) && (w[82] & 1)) d.notes.push_back(L"SMART поддерживается, но выключен в настройках накопителя");
}

// Чтение SMART SATA-накопителя. Все SMART-команды имеют код 0xB0, подкоманда задаётся
// в регистре Features, а в регистрах Cylinder Low/High обязательно пишется сигнатура
// 0x4F/0xC2 (иначе устройство отвергнет команду):
//   0xD0 — SMART READ DATA       (512 байт: таблица из 30 атрибутов по 12 байт);
//   0xD1 — SMART READ THRESHOLDS (512 байт: пороги атрибутов);
//   0xDA — SMART RETURN STATUS   (без данных: ответ в регистрах CL/CH).
static void ReadAtaSmart(HANDLE h, DiskInfo& d) {
    BYTE values[512], thresh[512] = {};
    BYTE rRead[7] = {0xD0, 1, 1, 0x4F, 0xC2, 0xA0, 0xB0};  // SMART READ DATA
    if (!AtaCommand(h, rRead, true, values)) {
        d.notes.push_back(Fmt(L"SMART READ DATA не выполнена (ошибка %lu)", GetLastError()));
        return;
    }
    d.smartOk = true;
    BYTE rThr[7] = {0xD1, 1, 1, 0x4F, 0xC2, 0xA0, 0xB0};  // SMART READ THRESHOLDS
    AtaCommand(h, rThr, true, thresh);
    BYTE rStat[7] = {0xDA, 0, 0, 0x4F, 0xC2, 0xA0, 0xB0};  // SMART RETURN STATUS
    BYTE out[8] = {};
    // RETURN STATUS: CL/CH = 0x4F/0xC2 — всё в норме, 0xF4/0x2C — превышен порог.
    if (AtaCommand(h, rStat, false, nullptr, out)) {
        if (out[3] == 0x4F && out[4] == 0xC2) d.smartStatus = 1;
        else if (out[3] == 0xF4 && out[4] == 0x2C) d.smartStatus = 0;
    }

    // Разбор таблицы атрибутов: смещение 2 (после номера версии), 30 записей по 12 байт:
    // [0] id, [1..2] флаги, [3] текущее, [4] наихудшее, [5..10] raw (little-endian).
    for (int i = 0; i < 30; ++i) {
        const BYTE* e = values + 2 + i * 12;
        if (!e[0]) continue;
        SmartAttr a;
        a.id = e[0];
        a.cur = e[3];
        a.worst = e[4];
        for (int k = 5; k >= 0; --k) a.raw = (a.raw << 8) | e[5 + k];
        for (int k = 0; k < 30; ++k)
            if (thresh[2 + k * 12] == a.id) a.thresh = thresh[3 + k * 12];
        d.attrs.push_back(a);
    }

    // Поиск атрибута по номеру и извлечение ключевых параметров.
    auto find = [&](BYTE id) -> const SmartAttr* {
        for (auto& a : d.attrs)
            if (a.id == id) return &a;
        return nullptr;
    };
    if (auto a = find(0x09)) d.poh = (long long)(a->raw & 0xFFFFFFFF);
    if (auto a = find(0x0C)) d.cycles = (long long)(a->raw & 0xFFFFFFFF);
    if (auto a = find(0x05)) d.realloc = (long long)(a->raw & 0xFFFFFFFF);
    if (auto a = find(0xC5)) d.pending = (long long)(a->raw & 0xFFFFFFFF);
    if (auto a = find(0xC0)) d.unsafe = (long long)(a->raw & 0xFFFFFFFF);
    else if (auto a2 = find(0xAE)) d.unsafe = (long long)(a2->raw & 0xFFFFFFFF);
    // Температура из SMART — только если её не дал StorageDeviceTemperatureProperty.
    // Значение хранится в младшем байте raw (в старших байтах — min/max).ТЕМПЕРАТУРА 
    if (d.temp == INT_MIN) {
        const SmartAttr* t = find(0xC2);
        if (!t) t = find(0xBE);
        if (t && (t->raw & 0xFF) > 0 && (t->raw & 0xFF) < 120) {
            d.temp = (int)(t->raw & 0xFF);
            d.tempSrc = Fmt(L"SMART 0x%02X", t->id);
        }
    }
    // Износ SATA SSD: нормализованное значение «оставшегося ресурса» (100 → 0)
    const BYTE wearIds[] = {0xE7, 0xA9, 0xB1, 0xE9, 0xCA, 0xAD};
    for (BYTE id : wearIds) {
        const SmartAttr* a = find(id);
        if (a && a->cur >= 1 && a->cur <= 100) {
            d.wear = 100 - a->cur;
            d.wearSrc = Fmt(L"SMART 0x%02X (%s)", id, SmartName(id));
            break;
        }
    }
}

// Чтение 64-битного числа little-endian из массива байт (поля NVMe-журнала 128-битные,
// но реальные значения помещаются в младшие 64 бита).
static unsigned long long Le64(const UCHAR* p) {
    unsigned long long v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// Чтение журнала NVMe SMART/Health Information (Log Page 0x02, 512 байт).
// Для NVMe не нужны ATA-команды: Windows (драйвер stornvme) принимает запрос
// IOCTL_STORAGE_QUERY_PROPERTY со свойством StorageDeviceProtocolSpecificProperty.
// Структура буфера: STORAGE_PROPERTY_QUERY, в её поле AdditionalParameters —
// STORAGE_PROTOCOL_SPECIFIC_DATA (тип протокола, тип данных, номер журнала),
// а за ней — место под сам журнал NVME_HEALTH_INFO_LOG.
static void ReadNvmeHealth(HANDLE h, DiskInfo& d) {
    const DWORD size = FIELD_OFFSET(STORAGE_PROPERTY_QUERY, AdditionalParameters) +
                       sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + sizeof(NVME_HEALTH_INFO_LOG);
    std::vector<BYTE> buf(size);
    auto q = (PSTORAGE_PROPERTY_QUERY)buf.data();
    auto p = (PSTORAGE_PROTOCOL_SPECIFIC_DATA)q->AdditionalParameters;
    q->PropertyId = StorageDeviceProtocolSpecificProperty;
    q->QueryType = PropertyStandardQuery;
    p->ProtocolType = ProtocolTypeNvme;
    p->DataType = NVMeDataTypeLogPage;
    p->ProtocolDataRequestValue = NVME_LOG_PAGE_HEALTH_INFO;
    p->ProtocolDataRequestSubValue = 0;
    p->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    p->ProtocolDataLength = sizeof(NVME_HEALTH_INFO_LOG);
    DWORD br = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf.data(), size, buf.data(), size, &br, nullptr)) {
        d.notes.push_back(Fmt(L"NVMe Health Log недоступен (ошибка %lu)", GetLastError()));
        return;
    }
    // Ответ приходит в том же буфере в виде STORAGE_PROTOCOL_DATA_DESCRIPTOR;
    // журнал лежит по смещению ProtocolDataOffset от структуры ProtocolSpecificData.
    auto desc = (PSTORAGE_PROTOCOL_DATA_DESCRIPTOR)buf.data();
    auto pd = &desc->ProtocolSpecificData;
    if (pd->ProtocolDataOffset < sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) ||
        pd->ProtocolDataLength < sizeof(NVME_HEALTH_INFO_LOG)) {
        d.notes.push_back(L"NVMe Health Log: некорректный ответ драйвера");
        return;
    }
    auto log = (PNVME_HEALTH_INFO_LOG)((BYTE*)pd + pd->ProtocolDataOffset);
    d.nvmeOk = true;
    d.critWarn = log->CriticalWarning.AsUchar;
    // Температура в журнале NVMe хранится в кельвинах (2 байта): °C = K − 273.
    int kelvin = (log->Temperature[1] << 8) | log->Temperature[0];
    if (d.temp == INT_MIN && kelvin > 0) {
        d.temp = kelvin - 273;
        d.tempSrc = L"NVMe Health Log";
    }
    d.spare = log->AvailableSpare;
    d.spareThr = log->AvailableSpareThreshold;
    d.wear = log->PercentageUsed;
    d.wearSrc = L"NVMe PercentageUsed";
    d.poh = (long long)Le64(log->PowerOnHours);
    d.cycles = (long long)Le64(log->PowerCycle);
    d.unsafe = (long long)Le64(log->UnsafeShutdowns);
    d.mediaErr = (long long)Le64(log->MediaErrors);
    // Data Units — в тысячах блоков по 512 байт, т. е. 1 единица = 512 000 байт.
    d.readGB = Le64(log->DataUnitRead) * 512000.0 / 1e9;
    d.writtenGB = Le64(log->DataUnitWritten) * 512000.0 / 1e9;
}

// ОПРЕДЕЛЕНИЕ ТИПА НАКОПИТЕЛЯ (HDD/SSD). Порядок приоритета признаков:
//  1) шина NVMe — всегда SSD (NVMe-накопители бывают только твердотельными);
//  2) скорость вращения из IDENTIFY (word 217): 1 — SSD, > 1 — HDD;
//  3) StorageDeviceSeekPenaltyProperty: нет штрафа позиционирования — SSD, есть — HDD;
//  4) StorageDeviceTrimProperty / IDENTIFY word 169: TRIM включён — SSD.
// Строка kindReason показывает, на основании каких значений сделан вывод.
static void Classify(DiskInfo& d) {
    auto yn = [](int v, const wchar_t* y, const wchar_t* n) { return v < 0 ? L"н/д" : v ? y : n; };
    std::wstring rpm = d.rpm < 0 ? L"н/д" : d.rpm == 0 ? L"не сообщается" : d.rpm == 1 ? L"1 (не вращается)" : Fmt(L"%d", d.rpm);
    d.kindReason = Fmt(L"SeekPenalty: %s; TRIM: %s; RPM: %s", yn(d.seekPenalty, L"есть", L"нет"),
                       yn(d.trim, L"вкл.", L"выкл."), rpm.c_str());
    if (d.bus == BusTypeNvme) {
        d.kind = Media::SSD;
        d.kindReason = L"шина NVMe; " + d.kindReason;
    } else if (d.rpm == 1) d.kind = Media::SSD;
    else if (d.rpm > 1) d.kind = Media::HDD;
    else if (d.seekPenalty == 0) d.kind = Media::SSD;
    else if (d.seekPenalty == 1) d.kind = Media::HDD;
    else if (d.trim == 1 || d.identTrim == 1) d.kind = Media::SSD;
}

// Сбор всей информации об одном физическом диске \\.\PhysicalDriveN.ПОЛУЧЕНИЕ ДАННЫХ О ДИСКЕ
static DiskInfo ReadDisk(int number) {
    DiskInfo d;
    d.number = number;
    std::wstring path = Fmt(L"\\\\.\\PhysicalDrive%d", number);
    // Сначала пытаемся открыть диск с правами чтения/записи — они нужны для ПОЛУЧЕНИЕ ДАННЫХ О ДИСКЕ
    // ATA pass-through (SMART). Без прав администратора это не удастся, тогда
    // открываем с доступом 0: служебные IOCTL (свойства, геометрия) всё равно работают.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    d.rwAccess = h != INVALID_HANDLE_VALUE;
    if (!d.rwAccess) {
        h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            d.notes.push_back(Fmt(L"не удалось открыть %s (ошибка %lu)", path.c_str(), GetLastError()));
            return d;
        }
    }

    // Идентификация: STORAGE_DEVICE_DESCRIPTOR
    std::vector<BYTE> buf(4096);
    DWORD got = 0;
    if (QueryProperty(h, StorageDeviceProperty, buf.data(), (DWORD)buf.size(), &got)) {
        // Строки дескриптора (модель, прошивка, серийный номер) лежат в том же буфере,
        // их положение задаётся смещениями *Offset от начала структуры (0 — строки нет).
        auto sd = (PSTORAGE_DEVICE_DESCRIPTOR)buf.data();
        auto str = [&](DWORD off) { return off && off < got ? FromAscii((char*)buf.data() + off, got - off) : std::wstring(); };
        d.vendor = str(sd->VendorIdOffset);
        d.model = str(sd->ProductIdOffset);
        d.firmware = str(sd->ProductRevisionOffset);
        d.serial = str(sd->SerialNumberOffset);
        d.bus = sd->BusType;
        d.removable = sd->RemovableMedia != FALSE;
    }

    // Признаки типа накопителя
    // Штраф позиционирования: TRUE — механика с перемещением головок (HDD).
    DEVICE_SEEK_PENALTY_DESCRIPTOR sp = {};
    if (QueryProperty(h, StorageDeviceSeekPenaltyProperty, &sp, sizeof(sp)) && sp.Size >= sizeof(sp))
        d.seekPenalty = sp.IncursSeekPenalty ? 1 : 0;
    // TRIM включён — характерно для SSD (операционная система сообщает о свободных блоках).
    DEVICE_TRIM_DESCRIPTOR tr = {};
    if (QueryProperty(h, StorageDeviceTrimProperty, &tr, sizeof(tr)) && tr.Size >= sizeof(tr))
        d.trim = tr.TrimEnabled ? 1 : 0;

    // Объём и геометрия
    // DISK_GEOMETRY_EX имеет переменный размер (за ним идут сведения о разделах),
    // поэтому используется буфер с запасом.
    BYTE geo[256] = {};
    DWORD br = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, geo, sizeof(geo), &br, nullptr)) {
        auto g = (PDISK_GEOMETRY_EX)geo;
        d.size = (ULONGLONG)g->DiskSize.QuadPart;
        d.sector = g->Geometry.BytesPerSector;
    }

    // Температура через StorageDeviceTemperatureProperty
    BYTE tbuf[512] = {};
    // Возвращает массив датчиков; элемент 0 — общая (composite) температура в °C.
    if (QueryProperty(h, StorageDeviceTemperatureProperty, tbuf, sizeof(tbuf))) {
        auto td = (PSTORAGE_TEMPERATURE_DATA_DESCRIPTOR)tbuf;
        if (td->InfoCount > 0) {
            SHORT t = td->TemperatureInfo[0].Temperature;
            if ((USHORT)t != STORAGE_TEMPERATURE_VALUE_NOT_REPORTED && t > -40 && t < 150) {
                d.temp = t;
                d.tempSrc = L"StorageDeviceTemperatureProperty";
            }
        }
    }

    // Выбор способа получения «здоровья» в зависимости от шины: NVMe — журнал Health,
    // SATA/ATA/RAID — ATA-команды. Для USB-мостов, виртуальных дисков и Storage Spaces
    // ATA pass-through обычно не поддерживается.
    // SMART / Health
    if (d.bus == BusTypeNvme) {
        ReadNvmeHealth(h, d);
    } else if (d.rwAccess && d.bus != BusTypeUsb && d.bus != BusTypeVirtual && d.bus != BusTypeFileBackedVirtual &&
               d.bus != BusTypeSpaces) {
        ReadAtaIdentify(h, d);
        if (d.identifyOk) ReadAtaSmart(h, d);
        else d.notes.push_back(Fmt(L"ATA IDENTIFY DEVICE не выполнена (ошибка %lu) — контроллер не поддерживает ATA pass-through", GetLastError()));
    } else if (d.rwAccess) {
        d.notes.push_back(L"SMART через ATA pass-through для шины " + BusName(d.bus) + L" не поддерживается");
    } else {
        d.notes.push_back(L"нет доступа на чтение/запись — SMART недоступен (запустите от имени администратора)");
    }
    CloseHandle(h);
    if (d.model.empty()) d.model = L"(без названия)";
    Classify(d);
    return d;
}

// Привязка логических томов (C:, D:, ...) к физическим дискам и анализ заполнения.ОПРЕДЕЛЕНИЕ ТОМА  
// Для каждой буквы: GetDriveType (только жёсткие и съёмные), открытие тома \\.\X:,
// IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS — на каких физических дисках лежит том
// (том может занимать несколько дисков), GetDiskFreeSpaceEx — общий и свободный объём.
static void AttachVolumes(std::vector<DiskInfo>& disks) {
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = L"A:\\";
        root[0] = (wchar_t)(L'A' + i);
        UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        wchar_t vpath[] = L"\\\\.\\A:";
        vpath[4] = root[0];
        HANDLE h = CreateFileW(vpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        BYTE ext[sizeof(VOLUME_DISK_EXTENTS) + 16 * sizeof(DISK_EXTENT)] = {};
        DWORD br = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, ext, sizeof(ext), &br, nullptr);
        CloseHandle(h);
        if (!ok) continue;
        VolumeInfo v;
        v.letter = root[0];
        ULARGE_INTEGER avail, total, totalFree;
        if (!GetDiskFreeSpaceExW(root, &avail, &total, &totalFree)) continue;
        v.total = total.QuadPart;
        v.freeBytes = totalFree.QuadPart;
        wchar_t label[MAX_PATH] = {}, fs[MAX_PATH] = {};
        if (GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH)) {
            v.label = label;
            v.fs = fs;
        }
        auto vde = (PVOLUME_DISK_EXTENTS)ext;
        for (DWORD e = 0; e < vde->NumberOfDiskExtents; ++e)
            for (auto& d : disks)
                if (d.number == (int)vde->Extents[e].DiskNumber &&
                    std::none_of(d.vols.begin(), d.vols.end(), [&](const VolumeInfo& x) { return x.letter == v.letter; }))
                    d.vols.push_back(v);
    }
}

// Полный цикл опроса: перечислить диски, прочитать каждый, привязать тома.
static std::vector<DiskInfo> CollectAll() {
    std::vector<DiskInfo> disks;
    for (int n : EnumPhysicalDisks()) disks.push_back(ReadDisk(n));
    AttachVolumes(disks);
    return disks;
}

// Проверка, запущен ли процесс с повышенными правами (администратор, UAC).
static bool IsElevated() {
    HANDLE t = nullptr;
    TOKEN_ELEVATION e = {};
    DWORD sz = 0;
    bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) {
        if (GetTokenInformation(t, TokenElevation, &e, sizeof(e), &sz)) r = e.TokenIsElevated != 0;
        CloseHandle(t);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Анализ: рекомендации по замене и сравнение
// ---------------------------------------------------------------------------
// Формирование рекомендаций по замене диска. Каждое правило сравнивает показатель
// с порогом (константы k* в начале файла) и добавляет рекомендацию с уровнем
// LV_WARN или LV_CRIT. Итоговый уровень — максимальный из всех рекомендаций.
static std::vector<Advice> Assess(const DiskInfo& d, int* overall) {
    std::vector<Advice> r;
    if (d.wear > kWearReplace)
        r.push_back({LV_CRIT, Fmt(L"Износ %d%% (> %d%%) — ресурс флеш-памяти практически исчерпан, рекомендуется замена накопителя", d.wear, kWearReplace)});
    else if (d.wear > kWearWarn)
        r.push_back({LV_WARN, Fmt(L"Износ %d%% (> %d%%) — запланируйте замену, регулярно делайте резервные копии", d.wear, kWearWarn)});
    if (d.temp != INT_MIN && d.temp > kTempCrit)
        r.push_back({LV_CRIT, Fmt(L"Температура %d °C (> %d °C) — перегрев! Проверьте охлаждение; длительная работа при такой температуре сокращает ресурс", d.temp, kTempCrit)});
    else if (d.temp != INT_MIN && d.temp > kTempWarn)
        r.push_back({LV_WARN, Fmt(L"Температура %d °C (> %d °C) — повышенная, улучшите обдув", d.temp, kTempWarn)});
    if (d.spare >= 0 && (d.spare < kSpareMin || (d.spareThr > 0 && d.spare < d.spareThr)))
        r.push_back({LV_CRIT, Fmt(L"Резервная область %d%% ниже порога — накопитель может перейти в режим только чтения, замените его", d.spare)});
    if (d.critWarn > 0) {
        std::wstring bits;
        if (d.critWarn & 0x01) bits += L" мало резерва;";
        if (d.critWarn & 0x02) bits += L" температура вне порогов;";
        if (d.critWarn & 0x04) bits += L" снижена надёжность;";
        if (d.critWarn & 0x08) bits += L" режим только чтения;";
        if (d.critWarn & 0x10) bits += L" отказ резервного питания;";
        r.push_back({LV_CRIT, L"NVMe Critical Warning:" + bits + L" — рекомендуется замена"});
    }
    if (d.smartStatus == 0)
        r.push_back({LV_CRIT, L"SMART RETURN STATUS: превышен порог одного из атрибутов — накопитель прогнозирует отказ, срочно сохраните данные"});
    for (auto& a : d.attrs)
        if (a.thresh > 0 && a.cur <= a.thresh)
            r.push_back({LV_CRIT, Fmt(L"Атрибут 0x%02X (%s): значение %d ≤ порога %d", a.id, SmartName(a.id), a.cur, a.thresh)});
    if (d.realloc >= 100)
        r.push_back({LV_CRIT, Fmt(L"Переназначено %lld секторов — поверхность деградирует, рекомендуется замена", d.realloc)});
    else if (d.realloc > 0)
        r.push_back({LV_WARN, Fmt(L"Переназначено %lld секторов — следите за ростом значения", d.realloc)});
    if (d.pending > 0)
        r.push_back({LV_WARN, Fmt(L"Нестабильных секторов (0xC5): %lld — возможна потеря данных", d.pending)});
    if (d.mediaErr > 0)
        r.push_back({LV_WARN, Fmt(L"Ошибок целостности данных (Media Errors): %lld", d.mediaErr)});
    if (d.kind == Media::HDD && d.poh > kHddOldHours)
        r.push_back({LV_WARN, Fmt(L"Наработка %lld ч (~%.1f лет) превышает типичный ресурс HDD — рассмотрите замену на SSD", d.poh, d.poh / 8760.0)});
    for (auto& v : d.vols)
        if (v.UsedPercent() > kFillWarn)
            r.push_back({LV_WARN, Fmt(L"Том %c: заполнен на %d%% — освободите место", v.letter, v.UsedPercent())});

    // Итоговая оценка: «Исправен», если данных достаточно и нет замечаний.
    int lvl = LV_NONE;
    for (auto& a : r) lvl = std::max(lvl, a.level);
    bool haveData = d.smartOk || d.nvmeOk || d.temp != INT_MIN;
    if (lvl == LV_NONE && haveData) {
        lvl = LV_OK;
        r.push_back({LV_OK, L"Показатели в норме — замена не требуется"});
    }
    if (!haveData) r.push_back({LV_NONE, L"Данные SMART/Health недоступны — оценить состояние невозможно"});
    if (overall) *overall = lvl;
    return r;
}

// Только итоговый уровень состояния диска (без текстов рекомендаций).
static int OverallLevel(const DiskInfo& d) {
    int l;
    Assess(d, &l);
    return l;
}

// Строка таблицы сравнения: название параметра, значения для дисков A и B,
// столбец «Лучше» и уровни подсветки ячеек.
struct CmpRow {
    std::wstring name, a, b, better;
    int sevA = LV_NONE, sevB = LV_NONE;
};

// lowerBetter: 1 — меньше лучше, 0 — больше лучше
static std::wstring Better(double a, double b, bool aOk, bool bOk, bool lowerBetter) {
    if (!aOk || !bOk) return L"";
    if (a == b) return L"=";
    return ((a < b) == lowerBetter) ? L"A" : L"B";
}

// Построение таблицы сравнения двух дисков. Для каждого параметра формируются
// значения A и B, отметка лучшего диска и цветовая «серьёзность» значения.
// Параметры, специфичные для типа (обороты — HDD, TRIM и износ — SSD), выводятся
// с пояснением, если к диску неприменимы.
static std::vector<CmpRow> BuildComparison(const DiskInfo& a, const DiskInfo& b) {
    std::vector<CmpRow> rows;
    auto add = [&](const std::wstring& n, const std::wstring& va, const std::wstring& vb, const std::wstring& bt = L"",
                   int sa = LV_NONE, int sb = LV_NONE) { rows.push_back({n, va, vb, bt, sa, sb}); };
    auto num = [](long long v, const wchar_t* unit) { return v < 0 ? std::wstring(L"н/д") : Fmt(L"%lld%s", v, unit); };
    auto rpmStr = [](const DiskInfo& d) -> std::wstring {
        if (d.kind == Media::SSD) return L"— (SSD, не вращается)";
        if (d.rpm > 1) return Fmt(L"%d об/мин", d.rpm);
        return L"н/д";
    };
    auto trimStr = [](const DiskInfo& d) -> std::wstring {
        if (d.kind == Media::HDD) return d.trim == 1 ? L"включён (HDD)" : L"— (HDD)";
        if (d.trim < 0) return d.identTrim == 1 ? L"поддерживается" : L"н/д";
        return d.trim ? L"включён" : L"выключен";
    };
    auto volStr = [](const DiskInfo& d) {
        std::wstring s;
        for (auto& v : d.vols) s += Fmt(L"%s%c: %d%% (своб. %s)", s.empty() ? L"" : L"; ", v.letter, v.UsedPercent(), FmtShortBytes(v.freeBytes).c_str());
        return s.empty() ? std::wstring(L"нет томов") : s;
    };
    auto smartStr = [](const DiskInfo& d) -> std::wstring {
        if (d.nvmeOk) return L"NVMe Health Log получен";
        if (!d.smartOk) return L"недоступен";
        return d.smartStatus == 1 ? L"OK (порог не превышен)" : d.smartStatus == 0 ? L"ОТКАЗ (порог превышен)" : L"прочитан, статус н/д";
    };
    int la = OverallLevel(a), lb = OverallLevel(b);

    add(L"Физический диск", Fmt(L"PhysicalDrive%d", a.number), Fmt(L"PhysicalDrive%d", b.number));
    add(L"Модель", a.model, b.model);
    add(L"Серийный номер", a.serial.empty() ? L"н/д" : a.serial, b.serial.empty() ? L"н/д" : b.serial);
    add(L"Версия прошивки", a.firmware.empty() ? L"н/д" : a.firmware, b.firmware.empty() ? L"н/д" : b.firmware);
    add(L"Тип накопителя", MediaName(a.kind), MediaName(b.kind),
        a.kind == b.kind ? L"=" : a.kind == Media::SSD ? L"A" : b.kind == Media::SSD ? L"B" : L"");
    add(L"Основание определения типа", a.kindReason, b.kindReason);
    {
        auto busRank = [](const DiskInfo& d) { return d.bus == BusTypeNvme ? 3 : d.sataSpeed ? d.sataSpeed / 3000 + 1 : 1; };
        add(L"Интерфейс", InterfaceStr(a), InterfaceStr(b), Better(busRank(a), busRank(b), true, true, false));
    }
    add(L"Объём", FmtBytes(a.size), FmtBytes(b.size), Better((double)a.size, (double)b.size, a.size > 0, b.size > 0, false));
    add(L"Размер сектора", a.sector ? Fmt(L"%lu байт", a.sector) : L"н/д", b.sector ? Fmt(L"%lu байт", b.sector) : L"н/д");
    add(L"Скорость вращения (HDD)", rpmStr(a), rpmStr(b));
    add(L"TRIM (SSD)", trimStr(a), trimStr(b));
    add(L"Штраф позиционирования (Seek Penalty)",
        a.seekPenalty < 0 ? L"н/д" : a.seekPenalty ? L"есть" : L"нет",
        b.seekPenalty < 0 ? L"н/д" : b.seekPenalty ? L"есть" : L"нет",
        Better(a.seekPenalty, b.seekPenalty, a.seekPenalty >= 0, b.seekPenalty >= 0, true));
    add(L"Температура", TempStr(a), TempStr(b), Better(a.temp, b.temp, a.temp != INT_MIN, b.temp != INT_MIN, true),
        SevTemp(a.temp), SevTemp(b.temp));
    auto pohStr = [](const DiskInfo& d) { return d.poh < 0 ? std::wstring(L"н/д") : Fmt(L"%lld ч (%.1f сут.)", d.poh, d.poh / 24.0); };
    add(L"Часы работы", pohStr(a), pohStr(b), Better((double)a.poh, (double)b.poh, a.poh >= 0, b.poh >= 0, true),
        a.kind == Media::HDD && a.poh > kHddOldHours ? LV_WARN : LV_NONE,
        b.kind == Media::HDD && b.poh > kHddOldHours ? LV_WARN : LV_NONE);
    add(L"Циклы включения", num(a.cycles, L""), num(b.cycles, L""));
    auto wearStr = [](const DiskInfo& d) {
        if (d.wear < 0) return std::wstring(d.kind == Media::HDD ? L"— (HDD, см. переназначения)" : L"н/д");
        return Fmt(L"%d%% (%s)", d.wear, d.wearSrc.c_str());
    };
    add(L"Износ (Percentage Used)", wearStr(a), wearStr(b), Better(a.wear, b.wear, a.wear >= 0, b.wear >= 0, true),
        SevWear(a.wear), SevWear(b.wear));
    if (a.spare >= 0 || b.spare >= 0)
        add(L"Доступный резерв (Available Spare)",
            a.spare < 0 ? L"н/д" : Fmt(L"%d%% (порог %d%%)", a.spare, a.spareThr),
            b.spare < 0 ? L"н/д" : Fmt(L"%d%% (порог %d%%)", b.spare, b.spareThr),
            Better(a.spare, b.spare, a.spare >= 0, b.spare >= 0, false), SevSpare(a), SevSpare(b));
    add(L"Переназначенные секторы (0x05)", num(a.realloc, L""), num(b.realloc, L""),
        Better((double)a.realloc, (double)b.realloc, a.realloc >= 0, b.realloc >= 0, true), SevCount(a.realloc, 100),
        SevCount(b.realloc, 100));
    if (a.pending >= 0 || b.pending >= 0)
        add(L"Нестабильные секторы (0xC5)", num(a.pending, L""), num(b.pending, L""), L"", SevCount(a.pending, 1),
            SevCount(b.pending, 1));
    add(L"Небезопасные отключения", num(a.unsafe, L""), num(b.unsafe, L""));
    if (a.readGB >= 0 || b.readGB >= 0) {
        auto tb = [](double gb) { return gb < 0 ? std::wstring(L"н/д") : Fmt(L"%.2f ТБ", gb / 1000); };
        add(L"Прочитано данных (за всё время)", tb(a.readGB), tb(b.readGB));
        add(L"Записано данных (за всё время)", tb(a.writtenGB), tb(b.writtenGB));
    }
    add(L"SMART-статус", smartStr(a), smartStr(b), L"", a.smartStatus == 0 ? LV_CRIT : LV_NONE,
        b.smartStatus == 0 ? LV_CRIT : LV_NONE);
    add(L"Логические тома (заполнение)", volStr(a), volStr(b));
    add(L"Общая оценка состояния", LevelName(la), LevelName(lb),
        (la == LV_NONE || lb == LV_NONE) ? L"" : la == lb ? L"=" : la < lb ? L"A" : L"B", la, lb);
    return rows;
}

// Текст панели «Рекомендации по замене»: рекомендации по каждому диску,
// служебные замечания (нет прав, не поддерживается pass-through) и общий вывод.
static std::wstring BuildRecommendations(const DiskInfo* a, const DiskInfo* b) {
    static const wchar_t* tag[] = {L"[ i ]", L"[OK ]", L"[ ! ]", L"[!!!]"};
    std::wstring s;
    auto one = [&](const wchar_t* title, const DiskInfo& d) {
        int lvl;
        auto adv = Assess(d, &lvl);
        s += Fmt(L"%s — PhysicalDrive%d, %s (%s): %s\r\n", title, d.number, d.model.c_str(), MediaName(d.kind).c_str(),
                 LevelName(lvl).c_str());
        for (auto& x : adv) s += Fmt(L"   %s %s\r\n", tag[x.level], x.text.c_str());
        for (auto& n : d.notes) s += Fmt(L"   [ i ] %s\r\n", n.c_str());
    };
    if (a) one(L"Диск A", *a);
    if (b && b != a) {
        s += L"\r\n";
        one(L"Диск B", *b);
    }
    if (a && b && a != b) {
        s += L"\r\nВывод сравнения:\r\n";
        if (a->kind != b->kind && a->kind != Media::Unknown && b->kind != Media::Unknown) {
            const DiskInfo& ssd = a->kind == Media::SSD ? *a : *b;
            const DiskInfo& hdd = a->kind == Media::SSD ? *b : *a;
            s += Fmt(L"   • %s (SSD) не имеет механических задержек позиционирования — его лучше использовать под ОС и программы;\r\n",
                     ssd.model.c_str());
            s += Fmt(L"   • %s (HDD) — для хранения больших объёмов данных и резервных копий.\r\n", hdd.model.c_str());
        }
        int la = OverallLevel(*a), lb = OverallLevel(*b);
        if (la != LV_NONE && lb != LV_NONE && la != lb)
            s += Fmt(L"   • В лучшем техническом состоянии диск %s (%s).\r\n", la < lb ? L"A" : L"B",
                     (la < lb ? a : b)->model.c_str());
        else if (la == lb && la != LV_NONE)
            s += Fmt(L"   • Оба диска в состоянии «%s».\r\n", LevelName(la).c_str());
    }
    return s;
}

// ---------------------------------------------------------------------------
// Отчёт (.txt / .html)
// ---------------------------------------------------------------------------
// Экранирование спецсимволов для HTML-отчёта.
static std::wstring HtmlEsc(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'<') r += L"&lt;";
        else if (c == L'>') r += L"&gt;";
        else if (c == L'&') r += L"&amp;";
        else if (c == L'\r') continue;
        else if (c == L'\n') r += L"<br>";
        else r += c;
    }
    return r;
}

// Дополнение строки пробелами до ширины столбца (для текстового отчёта).
static std::wstring PadRight(const std::wstring& s, size_t w) {
    return s.size() >= w ? s + L" " : s + std::wstring(w - s.size(), L' ');
}

// Формирование сравнительного отчёта: список дисков, таблица сравнения A/B,
// рекомендации, таблицы атрибутов SMART и журнал событий.
// html = true — HTML-страница с таблицами и цветовой подсветкой, иначе — текст.
static std::wstring BuildReport(const std::vector<DiskInfo>& disks, const DiskInfo* a, const DiskInfo* b,
                                const std::vector<std::wstring>& log, bool html) {
    wchar_t comp[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD csz = _countof(comp);
    GetComputerNameW(comp, &csz);
    std::wstring header = Fmt(L"Компьютер: %s · Дата: %s · Права администратора: %s", comp, NowStr(true).c_str(),
                              IsElevated() ? L"да" : L"нет");
    static const wchar_t* cls[] = {L"", L"ok", L"warn", L"crit"};
    std::wstring s;
    if (html) {
        s += L"<!DOCTYPE html><html lang=\"ru\"><head><meta charset=\"utf-8\"><title>Сравнение HDD vs SSD</title><style>"
             L"body{font-family:Segoe UI,Arial,sans-serif;margin:24px;color:#222;max-width:1200px}"
             L"h1{font-size:22px}h2{font-size:17px;margin-top:28px;border-bottom:1px solid #ccc;padding-bottom:4px}"
             L"table{border-collapse:collapse;margin:8px 0;width:100%}td,th{border:1px solid #bbb;padding:4px 8px;font-size:13px;vertical-align:top}"
             L"th{background:#eef1f5;text-align:left}.ok{background:#dcf5dc}.warn{background:#fff1c4}.crit{background:#ffd5d5}"
             L".muted{color:#666;font-size:13px}pre{background:#f6f6f6;padding:10px;font-size:12px;white-space:pre-wrap}"
             L"</style></head><body>";
        s += L"<h1>Сравнительный анализ накопителей HDD vs SSD</h1><p class=\"muted\">" + HtmlEsc(header) + L"</p>";
        s += L"<h2>1. Обнаруженные накопители</h2><table><tr><th>№</th><th>Модель</th><th>Тип</th><th>Интерфейс</th>"
             L"<th>Объём</th><th>Температура</th><th>Часы</th><th>Износ</th><th>Состояние</th></tr>";
        for (auto& d : disks) {
            int l = OverallLevel(d);
            s += Fmt(L"<tr><td>%d</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td>"
                     L"<td class=\"%s\">%s</td></tr>",
                     d.number, HtmlEsc(d.model).c_str(), MediaName(d.kind).c_str(), InterfaceStr(d).c_str(),
                     FmtShortBytes(d.size).c_str(), TempStr(d).c_str(),
                     d.poh < 0 ? L"н/д" : Fmt(L"%lld", d.poh).c_str(), d.wear < 0 ? L"н/д" : Fmt(L"%d%%", d.wear).c_str(),
                     cls[l], LevelName(l).c_str());
        }
        s += L"</table>";
        if (a && b) {
            s += L"<h2>2. Сравнение выбранных дисков</h2><table><tr><th>Параметр</th><th>Диск A</th><th>Диск B</th><th>Лучше</th></tr>";
            for (auto& r : BuildComparison(*a, *b))
                s += Fmt(L"<tr><td>%s</td><td class=\"%s\">%s</td><td class=\"%s\">%s</td><td>%s</td></tr>",
                         HtmlEsc(r.name).c_str(), cls[r.sevA], HtmlEsc(r.a).c_str(), cls[r.sevB], HtmlEsc(r.b).c_str(),
                         r.better.c_str());
            s += L"</table><h2>3. Рекомендации по замене</h2><pre>" + HtmlEsc(BuildRecommendations(a, b)) + L"</pre>";
            s += L"<h2>4. Атрибуты SMART</h2>";
            const DiskInfo* pair[] = {a, b};
            for (int k = 0; k < 2; ++k) {
                const DiskInfo& d = *pair[k];
                if (k == 1 && b == a) break;
                s += Fmt(L"<p><b>Диск %c — %s</b></p>", L'A' + k, HtmlEsc(d.model).c_str());
                if (d.attrs.empty()) {
                    s += d.nvmeOk ? L"<p class=\"muted\">NVMe: атрибуты ATA не применяются, данные NVMe Health Log приведены в таблице сравнения.</p>"
                                  : L"<p class=\"muted\">Атрибуты SMART недоступны.</p>";
                    continue;
                }
                s += L"<table><tr><th>ID</th><th>Атрибут</th><th>Текущее</th><th>Наихудшее</th><th>Порог</th><th>Raw</th><th>Статус</th></tr>";
                for (auto& at : d.attrs) {
                    bool bad = at.thresh > 0 && at.cur <= at.thresh;
                    s += Fmt(L"<tr><td>0x%02X</td><td>%s</td><td>%d</td><td>%d</td><td>%d</td><td>%llu</td><td class=\"%s\">%s</td></tr>",
                             at.id, SmartName(at.id), at.cur, at.worst, at.thresh, at.raw, bad ? L"crit" : L"ok", bad ? L"Отказ" : L"OK");
                }
                s += L"</table>";
            }
        }
        s += L"<h2>5. Журнал событий</h2><pre>";
        for (auto& l : log) s += HtmlEsc(l) + L"\n";
        s += L"</pre></body></html>";
        return s;
    }

    s += L"СРАВНИТЕЛЬНЫЙ АНАЛИЗ НАКОПИТЕЛЕЙ HDD vs SSD\r\n" + header + L"\r\n\r\n";
    s += L"1. ОБНАРУЖЕННЫЕ НАКОПИТЕЛИ\r\n";
    for (auto& d : disks)
        s += Fmt(L"  PhysicalDrive%d: %s | %s | %s | %s | t=%s | %s\r\n", d.number, d.model.c_str(),
                 MediaName(d.kind).c_str(), InterfaceStr(d).c_str(), FmtShortBytes(d.size).c_str(), TempStr(d).c_str(),
                 LevelName(OverallLevel(d)).c_str());
    if (a && b) {
        s += L"\r\n2. СРАВНЕНИЕ ВЫБРАННЫХ ДИСКОВ\r\n";
        s += PadRight(L"Параметр", 40) + PadRight(L"Диск A", 46) + PadRight(L"Диск B", 46) + L"Лучше\r\n";
        s += std::wstring(137, L'-') + L"\r\n";
        for (auto& r : BuildComparison(*a, *b))
            s += PadRight(r.name, 40) + PadRight(r.a, 46) + PadRight(r.b, 46) + r.better + L"\r\n";
        s += L"\r\n3. РЕКОМЕНДАЦИИ ПО ЗАМЕНЕ\r\n" + BuildRecommendations(a, b);
        s += L"\r\n4. АТРИБУТЫ SMART\r\n";
        const DiskInfo* pair[] = {a, b};
        for (int k = 0; k < 2; ++k) {
            const DiskInfo& d = *pair[k];
            if (k == 1 && b == a) break;
            s += Fmt(L"  Диск %c — %s\r\n", L'A' + k, d.model.c_str());
            if (d.attrs.empty()) s += d.nvmeOk ? L"    (NVMe — см. NVMe Health Log в таблице сравнения)\r\n" : L"    (недоступны)\r\n";
            for (auto& at : d.attrs)
                s += Fmt(L"    0x%02X %-34s cur=%3d worst=%3d thr=%3d raw=%llu%s\r\n", at.id, SmartName(at.id), at.cur,
                         at.worst, at.thresh, at.raw, at.thresh > 0 && at.cur <= at.thresh ? L"  <-- ОТКАЗ" : L"");
        }
    }
    s += L"\r\n5. ЖУРНАЛ СОБЫТИЙ\r\n";
    for (auto& l : log) s += L"  " + l + L"\r\n";
    return s;
}

// Запись отчёта в файл в кодировке UTF-8 с меткой BOM (правильно открывается
// и в браузере, и в Блокноте).
static bool WriteUtf8File(const std::wstring& path, const std::wstring& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string u = "\xEF\xBB\xBF" + ToUtf8(text);
    DWORD wr = 0;
    BOOL ok = WriteFile(f, u.data(), (DWORD)u.size(), &wr, nullptr);
    CloseHandle(f);
    return ok && wr == u.size();
}

static bool EndsWithI(const std::wstring& s, const wchar_t* suf) {
    size_t n = wcslen(suf);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suf) == 0;
}

// ---------------------------------------------------------------------------
// Графический интерфейс
// ---------------------------------------------------------------------------
// Идентификаторы элементов управления главного окна.
enum {
    IDC_DISKLIST = 101, IDC_COMBO_A, IDC_COMBO_B, IDC_REFRESH, IDC_EXPORT, IDC_AUTO, IDC_CMP, IDC_RECS, IDC_LOG,
    IDC_LBL_DISKS, IDC_LBL_A, IDC_LBL_B, IDC_LBL_CMP, IDC_LBL_RECS, IDC_LBL_LOG, IDC_ADMIN
};
// Таймеры: автообновление (10 с) и отложенное обновление после подключения/отключения диска.
enum { TIMER_AUTO = 1, TIMER_DEVCHANGE = 2 };
const UINT kAutoIntervalMs = 10000;

// Глобальное состояние окна: дескрипторы элементов, шрифты, последние данные о дисках,
// строки таблицы сравнения, журнал событий и номера выбранных дисков A и B.
static HINSTANCE g_inst;
static HWND g_wnd, g_diskList, g_comboA, g_comboB, g_cmp, g_recs, g_log, g_auto;
static HFONT g_font, g_fontBold, g_fontMono;
static std::vector<DiskInfo> g_disks;
static std::vector<CmpRow> g_rows;
static std::vector<std::wstring> g_logLines;
static int g_selA = -1, g_selB = -1;  // номера физических дисков

// Добавление строки в журнал событий: с отметкой времени, в память (для отчёта)
// и в конец многострочного поля журнала.
static void Log(const std::wstring& msg) {
    std::wstring line = NowStr(false) + L"  " + msg;
    g_logLines.push_back(line);
    if (!g_log) return;
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)(line + L"\r\n").c_str());
}

// Поиск диска по номеру среди последних полученных данных.
static const DiskInfo* FindDisk(int number) {
    for (auto& d : g_disks)
        if (d.number == number) return &d;
    return nullptr;
}

// Вспомогательные функции ListView: добавить столбец / добавить строку.
// В lParam строки хранится номер диска или индекс строки сравнения.
static void LvColumn(HWND lv, int i, const wchar_t* t, int w) {
    LVCOLUMNW c = {};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = (LPWSTR)t;
    c.cx = w;
    c.iSubItem = i;
    ListView_InsertColumn(lv, i, &c);
}

static void LvRow(HWND lv, const std::vector<std::wstring>& cells, LPARAM param) {
    LVITEMW it = {};
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = ListView_GetItemCount(lv);
    it.pszText = (LPWSTR)cells[0].c_str();
    it.lParam = param;
    int r = ListView_InsertItem(lv, &it);
    for (size_t k = 1; k < cells.size(); ++k) ListView_SetItemText(lv, r, (int)k, (LPWSTR)cells[k].c_str());
}

// Заполнение верхней таблицы — список всех физических дисков.
// WM_SETREDRAW отключает перерисовку на время заполнения (нет мерцания).
static void FillDiskList() {
    SendMessageW(g_diskList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_diskList);
    for (auto& d : g_disks) {
        std::wstring vols;
        for (auto& v : d.vols) vols += Fmt(L"%s%c: %d%%", vols.empty() ? L"" : L", ", v.letter, v.UsedPercent());
        LvRow(g_diskList,
              {Fmt(L"%d", d.number), d.model, MediaName(d.kind), InterfaceStr(d), FmtShortBytes(d.size), TempStr(d),
               d.poh < 0 ? L"н/д" : Fmt(L"%lld", d.poh), d.wear < 0 ? L"н/д" : Fmt(L"%d%%", d.wear), vols,
               LevelName(OverallLevel(d))},
              d.number);
    }
    SendMessageW(g_diskList, WM_SETREDRAW, TRUE, 0);
}

// Заполнение выпадающих списков «Диск A» и «Диск B». По умолчанию A — первый диск,
// B — второй (если он есть). Выбор сохраняется между обновлениями по номеру диска.
static void FillCombos() {
    if (!FindDisk(g_selA)) g_selA = g_disks.empty() ? -1 : g_disks[0].number;
    if (!FindDisk(g_selB)) {
        g_selB = g_selA;
        for (auto& d : g_disks)
            if (d.number != g_selA) {
                g_selB = d.number;
                break;
            }
    }
    HWND combos[] = {g_comboA, g_comboB};
    int sel[] = {g_selA, g_selB};
    for (int k = 0; k < 2; ++k) {
        SendMessageW(combos[k], CB_RESETCONTENT, 0, 0);
        for (auto& d : g_disks) {
            std::wstring t = Fmt(L"Диск %d: %s (%s, %s)", d.number, d.model.c_str(), MediaName(d.kind).c_str(),
                                 FmtShortBytes(d.size).c_str());
            int i = (int)SendMessageW(combos[k], CB_ADDSTRING, 0, (LPARAM)t.c_str());
            SendMessageW(combos[k], CB_SETITEMDATA, i, d.number);
            if (d.number == sel[k]) SendMessageW(combos[k], CB_SETCURSEL, i, 0);
        }
    }
}

// Перестроение таблицы сравнения и панели рекомендаций для выбранных дисков.
static void UpdateComparison() {
    const DiskInfo* a = FindDisk(g_selA);
    const DiskInfo* b = FindDisk(g_selB);
    SendMessageW(g_cmp, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_cmp);
    g_rows.clear();
    if (a && b) {
        g_rows = BuildComparison(*a, *b);
        for (size_t i = 0; i < g_rows.size(); ++i)
            LvRow(g_cmp, {g_rows[i].name, g_rows[i].a, g_rows[i].b, g_rows[i].better}, (LPARAM)i);
    }
    SendMessageW(g_cmp, WM_SETREDRAW, TRUE, 0);
    SetWindowTextW(g_recs, a ? BuildRecommendations(a, b).c_str() : L"Накопители не обнаружены.");
}

// Отслеживание изменений между двумя опросами: подключение/отключение дисков,
// изменение температуры (на 2 °C и более), износа, переназначенных секторов,
// итогового состояния и заполнения томов. Диски сопоставляются по модели и серийному номеру.
static void LogChanges(const std::vector<DiskInfo>& oldD, const std::vector<DiskInfo>& newD) {
    if (oldD.empty()) {
        Log(Fmt(L"Обнаружено физических накопителей: %zu", newD.size()));
        for (auto& d : newD) {
            Log(Fmt(L"  PhysicalDrive%d: %s — %s, %s, %s", d.number, d.model.c_str(), MediaName(d.kind).c_str(),
                    BusName(d.bus).c_str(), FmtShortBytes(d.size).c_str()));
            for (auto& n : d.notes) Log(Fmt(L"    PhysicalDrive%d: %s", d.number, n.c_str()));
        }
        return;
    }
    std::map<std::wstring, const DiskInfo*> om, nm;
    for (auto& d : oldD) om[d.Key()] = &d;
    for (auto& d : newD) nm[d.Key()] = &d;
    for (auto& [k, d] : nm)
        if (!om.count(k)) Log(Fmt(L"Подключён накопитель: PhysicalDrive%d — %s (%s)", d->number, d->model.c_str(), MediaName(d->kind).c_str()));
    for (auto& [k, d] : om)
        if (!nm.count(k)) Log(Fmt(L"Отключён накопитель: %s", d->model.c_str()));
    for (auto& [k, n] : nm) {
        auto it = om.find(k);
        if (it == om.end()) continue;
        const DiskInfo* o = it->second;
        if (o->temp != INT_MIN && n->temp != INT_MIN && std::abs(o->temp - n->temp) >= 2)
            Log(Fmt(L"PhysicalDrive%d: температура %d → %d °C", n->number, o->temp, n->temp));
        if (o->wear != n->wear) Log(Fmt(L"PhysicalDrive%d: износ %d%% → %d%%", n->number, o->wear, n->wear));
        if (o->realloc != n->realloc)
            Log(Fmt(L"PhysicalDrive%d: переназначенные секторы %lld → %lld", n->number, o->realloc, n->realloc));
        int lo = OverallLevel(*o), ln = OverallLevel(*n);
        if (lo != ln)
            Log(Fmt(L"PhysicalDrive%d: состояние «%s» → «%s»", n->number, LevelName(lo).c_str(), LevelName(ln).c_str()));
        for (auto& nv : n->vols)
            for (auto& ov : o->vols)
                if (ov.letter == nv.letter && ov.UsedPercent() != nv.UsedPercent())
                    Log(Fmt(L"Том %c: заполнение %d%% → %d%%", nv.letter, ov.UsedPercent(), nv.UsedPercent()));
    }
}

// Обновление всех данных: опрос дисков, запись изменений в журнал, перерисовка интерфейса.
static void Refresh(const wchar_t* reason) {
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    DWORD t0 = GetTickCount();
    auto fresh = CollectAll();
    LogChanges(g_disks, fresh);
    g_disks = std::move(fresh);
    FillDiskList();
    FillCombos();
    UpdateComparison();
    Log(Fmt(L"Обновление данных (%s): %zu дисков, %lu мс", reason, g_disks.size(), GetTickCount() - t0));
    SetCursor(old);
}

// Экспорт отчёта: стандартный диалог «Сохранить как» (GetSaveFileName),
// формат выбирается по расширению файла (.html или .txt).
static void ExportReport() {
    wchar_t file[MAX_PATH];
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf_s(file, L"HDD_vs_SSD_%04d%02d%02d_%02d%02d.html", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"HTML-отчёт (*.html)\0*.html\0Текстовый отчёт (*.txt)\0*.txt\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"html";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Экспорт сравнительного отчёта";
    if (!GetSaveFileNameW(&ofn)) return;
    std::wstring path = file;
    bool html = !EndsWithI(path, L".txt");
    Log(L"Экспорт отчёта: " + path);
    if (WriteUtf8File(path, BuildReport(g_disks, FindDisk(g_selA), FindDisk(g_selB), g_logLines, html))) {
        Log(L"Отчёт сохранён");
        if (MessageBoxW(g_wnd, L"Отчёт сохранён. Открыть его?", L"Экспорт", MB_YESNO | MB_ICONINFORMATION) == IDYES)
            ShellExecuteW(g_wnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        Log(Fmt(L"Ошибка записи отчёта (код %lu)", GetLastError()));
        MessageBoxW(g_wnd, L"Не удалось сохранить отчёт.", L"Ошибка", MB_ICONERROR);
    }
}

// Размещение элементов при изменении размеров окна (WM_SIZE):
// сверху кнопки и список дисков, ниже выбор A/B и таблица сравнения,
// внизу — рекомендации (слева) и журнал (справа).
static void Layout(int w, int h) {
    const int m = 8, rowH = 24, lblH = 18;
    int y = m;
    MoveWindow(GetDlgItem(g_wnd, IDC_LBL_DISKS), m, y + 4, 330, lblH, TRUE);
    int x = w - m - 150;
    MoveWindow(GetDlgItem(g_wnd, IDC_EXPORT), x, y, 150, rowH + 2, TRUE);
    x -= 160 + 6;
    MoveWindow(GetDlgItem(g_wnd, IDC_REFRESH), x, y, 160, rowH + 2, TRUE);
    x -= 210 + 10;
    MoveWindow(g_auto, x, y + 2, 210, rowH, TRUE);
    x -= 280 + 10;
    MoveWindow(GetDlgItem(g_wnd, IDC_ADMIN), x, y, 280, rowH + 2, TRUE);
    y += rowH + 8;
    int listH = std::max(90, h / 7);
    MoveWindow(g_diskList, m, y, w - 2 * m, listH, TRUE);
    y += listH + 10;
    int half = (w - 3 * m) / 2;
    MoveWindow(GetDlgItem(g_wnd, IDC_LBL_A), m, y + 4, 55, lblH, TRUE);
    MoveWindow(g_comboA, m + 58, y, half - 58, 300, TRUE);
    MoveWindow(GetDlgItem(g_wnd, IDC_LBL_B), 2 * m + half, y + 4, 55, lblH, TRUE);
    MoveWindow(g_comboB, 2 * m + half + 58, y, half - 58, 300, TRUE);
    y += rowH + 10;
    int bottomH = std::max(130, h / 5);
    MoveWindow(GetDlgItem(g_wnd, IDC_LBL_CMP), m, y, 400, lblH, TRUE);
    y += lblH + 2;
    int cmpH = std::max(120, h - y - bottomH - lblH - 2 * m - 4);
    MoveWindow(g_cmp, m, y, w - 2 * m, cmpH, TRUE);
    y += cmpH + m;
    int recW = (w - 3 * m) * 55 / 100;
    MoveWindow(GetDlgItem(g_wnd, IDC_LBL_RECS), m, y, recW, lblH, TRUE);
    MoveWindow(GetDlgItem(g_wnd, IDC_LBL_LOG), 2 * m + recW, y, w - 3 * m - recW, lblH, TRUE);
    y += lblH + 2;
    int bh = h - y - m;
    MoveWindow(g_recs, m, y, recW, bh, TRUE);
    MoveWindow(g_log, 2 * m + recW, y, w - 3 * m - recW, bh, TRUE);
    // Колонки таблицы сравнения растягиваются по ширине
    int cw = w - 2 * m - GetSystemMetrics(SM_CXVSCROLL) - 4;
    ListView_SetColumnWidth(g_cmp, 0, cw * 28 / 100);
    ListView_SetColumnWidth(g_cmp, 1, cw * 33 / 100);
    ListView_SetColumnWidth(g_cmp, 2, cw * 33 / 100);
    ListView_SetColumnWidth(g_cmp, 3, cw * 6 / 100);
}

// Цвета фона ячеек: зелёный — норма, жёлтый — предупреждение, красный — критично.
static COLORREF LevelColor(int l) {
    switch (l) {
    case LV_OK: return RGB(220, 245, 220);
    case LV_WARN: return RGB(255, 241, 196);
    case LV_CRIT: return RGB(255, 213, 213);
    default: return GetSysColor(COLOR_WINDOW);
    }
}

// Пользовательская отрисовка ListView (NM_CUSTOMDRAW): позволяет задать цвет фона
// каждой ячейки отдельно. Схема: PREPAINT → запрашиваем уведомления по строкам,
// ITEMPREPAINT → по ячейкам (subitem), ITEMPREPAINT|SUBITEM → задаём цвет и шрифт.
static LRESULT OnCustomDraw(LPNMLVCUSTOMDRAW cd) {
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        int lvl = LV_NONE;
        size_t idx = (size_t)cd->nmcd.lItemlParam;
        if (cd->nmcd.hdr.hwndFrom == g_cmp && idx < g_rows.size()) {
            if (cd->iSubItem == 1) lvl = g_rows[idx].sevA;
            if (cd->iSubItem == 2) lvl = g_rows[idx].sevB;
            SelectObject(cd->nmcd.hdc, cd->iSubItem == 0 ? g_fontBold : g_font);
        } else if (cd->nmcd.hdr.hwndFrom == g_diskList && cd->iSubItem == 9) {
            if (auto d = FindDisk((int)cd->nmcd.lItemlParam)) lvl = OverallLevel(*d);
        }
        cd->clrTextBk = LevelColor(lvl);
        cd->clrText = GetSysColor(COLOR_WINDOWTEXT);
        return CDRF_NEWFONT;
    }
    }
    return CDRF_DODEFAULT;
}

// Создание дочернего элемента окна с общим шрифтом.
static HWND MakeChild(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

// Создание всех элементов управления главного окна.
static void CreateControls() {
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = -15;
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    g_fontBold = CreateFontIndirectW(&ncm.lfMessageFont);
    g_fontMono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

    MakeChild(L"STATIC", L"Обнаруженные физические накопители:", 0, IDC_LBL_DISKS);
    SendMessageW(GetDlgItem(g_wnd, IDC_LBL_DISKS), WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    MakeChild(L"BUTTON", L"Обновить данные", BS_PUSHBUTTON, IDC_REFRESH);
    MakeChild(L"BUTTON", L"Экспорт отчёта…", BS_PUSHBUTTON, IDC_EXPORT);
    g_auto = MakeChild(L"BUTTON", L"Автообновление (10 с)", BS_AUTOCHECKBOX, IDC_AUTO);
    // Кнопка нужна только без прав администратора: SMART SATA-дисков требует доступа на запись к диску
    HWND admin = MakeChild(L"BUTTON", L"Перезапустить от администратора", BS_PUSHBUTTON, IDC_ADMIN);
    SendMessageW(admin, BCM_SETSHIELD, 0, TRUE);
    if (IsElevated()) ShowWindow(admin, SW_HIDE);

    DWORD lvStyle = LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP;
    DWORD lvEx = LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER;
    g_diskList = MakeChild(WC_LISTVIEWW, L"", lvStyle, IDC_DISKLIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_diskList, lvEx);
    const wchar_t* dcols[] = {L"№", L"Модель", L"Тип", L"Интерфейс", L"Объём", L"Темп.", L"Часы", L"Износ", L"Тома", L"Состояние"};
    int dw[] = {35, 250, 60, 190, 90, 65, 70, 60, 170, 120};
    for (int i = 0; i < 10; ++i) LvColumn(g_diskList, i, dcols[i], dw[i]);

    MakeChild(L"STATIC", L"Диск A:", 0, IDC_LBL_A);
    MakeChild(L"STATIC", L"Диск B:", 0, IDC_LBL_B);
    g_comboA = MakeChild(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_COMBO_A);
    g_comboB = MakeChild(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_COMBO_B);

    MakeChild(L"STATIC", L"Сравнение выбранных дисков:", 0, IDC_LBL_CMP);
    SendMessageW(GetDlgItem(g_wnd, IDC_LBL_CMP), WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    g_cmp = MakeChild(WC_LISTVIEWW, L"", lvStyle | LVS_NOSORTHEADER, IDC_CMP, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_cmp, lvEx);
    LvColumn(g_cmp, 0, L"Параметр", 260);
    LvColumn(g_cmp, 1, L"Диск A", 380);
    LvColumn(g_cmp, 2, L"Диск B", 380);
    LvColumn(g_cmp, 3, L"Лучше", 60);

    MakeChild(L"STATIC", L"Рекомендации по замене:", 0, IDC_LBL_RECS);
    SendMessageW(GetDlgItem(g_wnd, IDC_LBL_RECS), WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    MakeChild(L"STATIC", L"Журнал событий:", 0, IDC_LBL_LOG);
    SendMessageW(GetDlgItem(g_wnd, IDC_LBL_LOG), WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    DWORD edStyle = ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL;
    g_recs = MakeChild(L"EDIT", L"", edStyle, IDC_RECS, WS_EX_CLIENTEDGE);
    g_log = MakeChild(L"EDIT", L"", edStyle, IDC_LOG, WS_EX_CLIENTEDGE);
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
    SendMessageW(g_recs, EM_SETLIMITTEXT, 0, 0);
    SendMessageW(g_log, EM_SETLIMITTEXT, 0, 0);
}

// Оконная процедура главного окна — обработка сообщений Windows.
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    // Создание окна: элементы управления, подписка на уведомления о дисках,
    // первое обновление данных (через PostMessage — после показа окна).
    case WM_CREATE: {
        g_wnd = hwnd;
        CreateControls();
        DEV_BROADCAST_DEVICEINTERFACE_W f = {sizeof(f), DBT_DEVTYP_DEVICEINTERFACE};
        f.dbcc_classguid = kGuidDisk;
        RegisterDeviceNotificationW(hwnd, &f, DEVICE_NOTIFY_WINDOW_HANDLE);
        Log(L"Запуск. Права администратора: " + std::wstring(IsElevated() ? L"да" : L"нет (SMART будет недоступен)"));
        PostMessageW(hwnd, WM_COMMAND, IDC_REFRESH, 0);
        return 0;
    }
    case WM_SIZE:
        Layout(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lp)->ptMinTrackSize = {1200, 650};
        return 0;
    // Команды от кнопок, флажка и выпадающих списков.
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_REFRESH:
            Refresh(lp ? L"вручную" : L"запуск");
            return 0;
        case IDC_EXPORT:
            ExportReport();
            return 0;
        case IDC_ADMIN: {
            // Повторный запуск с запросом UAC («runas»); текущий экземпляр закрывается
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            if ((INT_PTR)ShellExecuteW(hwnd, L"runas", exe, nullptr, nullptr, SW_SHOWNORMAL) > 32)
                DestroyWindow(hwnd);
            else
                Log(L"Повышение прав отменено пользователем");
            return 0;
        }
        case IDC_AUTO:
            if (SendMessageW(g_auto, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                SetTimer(hwnd, TIMER_AUTO, kAutoIntervalMs, nullptr);
                Log(L"Автообновление включено (каждые 10 с)");
            } else {
                KillTimer(hwnd, TIMER_AUTO);
                Log(L"Автообновление выключено");
            }
            return 0;
        case IDC_COMBO_A:
        case IDC_COMBO_B:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                HWND c = (HWND)lp;
                int i = (int)SendMessageW(c, CB_GETCURSEL, 0, 0);
                if (i >= 0) {
                    int n = (int)SendMessageW(c, CB_GETITEMDATA, i, 0);
                    (LOWORD(wp) == IDC_COMBO_A ? g_selA : g_selB) = n;
                    Log(Fmt(L"Выбран диск %c: PhysicalDrive%d", LOWORD(wp) == IDC_COMBO_A ? L'A' : L'B', n));
                    UpdateComparison();
                }
            }
            return 0;
        }
        break;
    // Уведомления от ListView: пользовательская отрисовка и двойной щелчок.
    case WM_NOTIFY: {
        auto hdr = (LPNMHDR)lp;
        if (hdr->code == NM_CUSTOMDRAW && (hdr->hwndFrom == g_cmp || hdr->hwndFrom == g_diskList))
            return OnCustomDraw((LPNMLVCUSTOMDRAW)lp);
        if (hdr->code == NM_DBLCLK && hdr->hwndFrom == g_diskList) {
            // Двойной щелчок по накопителю — выбрать его диском A
            auto ia = (LPNMITEMACTIVATE)lp;
            if (ia->iItem >= 0) {
                LVITEMW it = {LVIF_PARAM, ia->iItem};
                ListView_GetItem(g_diskList, &it);
                g_selA = (int)it.lParam;
                FillCombos();
                UpdateComparison();
            }
        }
        break;
    }
    // Срабатывание таймеров: автообновление и обновление после смены конфигурации.
    case WM_TIMER:
        if (wp == TIMER_AUTO) Refresh(L"по таймеру");
        if (wp == TIMER_DEVCHANGE) {
            KillTimer(hwnd, TIMER_DEVCHANGE);
            Refresh(L"изменение конфигурации устройств");
        }
        return 0;
    // Система сообщает о подключении/отключении устройства. Обновление откладывается
    // на 1,5 с, чтобы драйвер успел инициализировать новый диск.
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
            Log(wp == DBT_DEVICEARRIVAL ? L"Система сообщила о подключении устройства" : L"Система сообщила об отключении устройства");
            SetTimer(hwnd, TIMER_DEVCHANGE, 1500, nullptr);
        }
        return TRUE;
    // Белый фон для полей «только для чтения» (по умолчанию они серые).
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_recs || (HWND)lp == g_log) {
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Режим без окна: HddSsdCompare.exe /report <файл>
static int RunConsoleReport(const std::wstring& path) {
    Log(L"Пакетный режим: формирование отчёта");
    g_disks = CollectAll();
    LogChanges({}, g_disks);
    const DiskInfo* a = g_disks.empty() ? nullptr : &g_disks[0];
    const DiskInfo* b = g_disks.size() > 1 ? &g_disks[1] : a;
    return WriteUtf8File(path, BuildReport(g_disks, a, b, g_logLines, !EndsWithI(path, L".txt"))) ? 0 : 1;
}

// Точка входа. Без параметров — графический режим, с параметром /report — пакетный.
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    g_inst = hInst;
    SetErrorMode(SEM_FAILCRITICALERRORS);  // без системных окон «Вставьте диск» для пустых кардридеров

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 3 && (_wcsicmp(argv[1], L"/report") == 0 || _wcsicmp(argv[1], L"-report") == 0)) {
        int rc = RunConsoleReport(argv[2]);
        LocalFree(argv);
        return rc;
    }
    if (argv) LocalFree(argv);

    // Инициализация библиотеки Common Controls (ListView, ComboBox) версии 6.
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    // Регистрация класса и создание главного окна.
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"HddSsdCompareWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Сравнительный анализатор HDD vs SSD — ЛР №3, вариант А4",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 880, nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    // Цикл обработки сообщений.
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}
