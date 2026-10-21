// main.cpp
// Lab work #2 - Option A4 (Windows)
// Tree view of the PCI bus topology. Everything in a single file.
//
//   Tree:  Bus 0 -> Device 0 -> Function 0 (Vendor: Intel, Device: Host Bridge)
//          PCI-to-PCI bridges (Class 0x06, Subclass 0x04) have their
//          secondary bus as a child node, so the whole topology is shown.
//   Right panel: all information about the selected node (for a function:
//          all header fields, BARs, bridge windows, capabilities and a
//          hex dump of the 256-byte configuration space).
//   Buttons: Scan / Expand all / Collapse all / Save report.
//
// The source is plain C++98 + Win32 API, no special compiler flags.
// The user interface uses the wide-char (Unicode, ...W) Win32 functions,
// so the Russian text is shown correctly even on an English Windows XP.
// Russian strings are written as \uXXXX escapes, so the file itself is
// pure ASCII and does not depend on the editor / compiler code page.
//
// BUILD (Dev-C++ / TDM-GCC 32-bit): project type "Windows Application",
//        g++ main.cpp -o pci_scanner.exe -mwindows
//        (no -std=..., no -municode, no -lcomctl32 / -lcomdlg32 needed)
//
// BUILD (MSVC): empty Win32 project, Platform = x86, SubSystem = Windows.
//
// RUN: 32-bit Windows XP (e.g. in VirtualBox), as Administrator,
//      with giveio.sys placed next to the .exe (see section 1).

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cstring>


// =====================================================================
// 0. User interface strings (Russian)
// =====================================================================

static const std::wstring S_BUS            = L"\u0428\u0438\u043d\u0430";
static const std::wstring S_DEVICE         = L"\u0423\u0441\u0442\u0440\u043e\u0439\u0441\u0442\u0432\u043e";
static const std::wstring S_FUNCTION       = L"\u0424\u0443\u043d\u043a\u0446\u0438\u044f";
static const std::wstring S_ARROW          = L" \u2192 ";   // " -> "
static const std::wstring S_ROOT_BUS       = L"\u043a\u043e\u0440\u043d\u0435\u0432\u0430\u044f";
static const std::wstring S_BRIDGE_TO      = L"\u043c\u043e\u0441\u0442 \u043d\u0430 \u0448\u0438\u043d\u0443";

static const std::wstring S_WINDOW_TITLE   = L"\u041b\u0420 \u21162 - \u0422\u043e\u043f\u043e\u043b\u043e\u0433\u0438\u044f \u0448\u0438\u043d PCI (\u0432\u0430\u0440\u0438\u0430\u043d\u0442 \u04104)";
static const std::wstring S_BTN_SCAN       = L"\u0421\u043a\u0430\u043d\u0438\u0440\u043e\u0432\u0430\u0442\u044c";
static const std::wstring S_BTN_EXPAND     = L"\u0420\u0430\u0437\u0432\u0435\u0440\u043d\u0443\u0442\u044c \u0432\u0441\u0451";
static const std::wstring S_BTN_COLLAPSE   = L"\u0421\u0432\u0435\u0440\u043d\u0443\u0442\u044c \u0432\u0441\u0451";
static const std::wstring S_BTN_SAVE       = L"\u0421\u043e\u0445\u0440\u0430\u043d\u0438\u0442\u044c \u043e\u0442\u0447\u0451\u0442";

static const std::wstring S_HINT           = L"\u0412\u044b\u0431\u0435\u0440\u0438\u0442\u0435 \u0443\u0437\u0435\u043b \u0434\u0435\u0440\u0435\u0432\u0430, \u0447\u0442\u043e\u0431\u044b \u0443\u0432\u0438\u0434\u0435\u0442\u044c \u043f\u043e\u0434\u0440\u043e\u0431\u043d\u0443\u044e \u0438\u043d\u0444\u043e\u0440\u043c\u0430\u0446\u0438\u044e.";
static const std::wstring S_STATUS_READY   = L"\u0414\u0440\u0430\u0439\u0432\u0435\u0440 giveio \u043e\u0442\u043a\u0440\u044b\u0442, \u0434\u043e\u0441\u0442\u0443\u043f \u043a \u043f\u043e\u0440\u0442\u0430\u043c \u043f\u043e\u043b\u0443\u0447\u0435\u043d. \u041d\u0430\u0436\u043c\u0438\u0442\u0435 \"\u0421\u043a\u0430\u043d\u0438\u0440\u043e\u0432\u0430\u0442\u044c\".";
static const std::wstring S_STATUS_NOPORTS = L"\u0414\u043e\u0441\u0442\u0443\u043f \u043a \u043f\u043e\u0440\u0442\u0430\u043c \u041d\u0415 \u043f\u043e\u043b\u0443\u0447\u0435\u043d - \u0441\u043c. \u0441\u043e\u043e\u0431\u0449\u0435\u043d\u0438\u0435 \u043e\u0431 \u043e\u0448\u0438\u0431\u043a\u0435.";
static const std::wstring S_STATUS_START   = L"\u0417\u0430\u043f\u0443\u0441\u043a...";
static const std::wstring S_FOUND_BUSES    = L"\u041d\u0430\u0439\u0434\u0435\u043d\u043e \u0448\u0438\u043d: ";
static const std::wstring S_FOUND_DEVICES  = L", \u0443\u0441\u0442\u0440\u043e\u0439\u0441\u0442\u0432: ";
static const std::wstring S_FOUND_FUNCS    = L", \u0444\u0443\u043d\u043a\u0446\u0438\u0439: ";

static const std::wstring S_CAP_SCAN       = L"\u0421\u043a\u0430\u043d\u0438\u0440\u043e\u0432\u0430\u043d\u0438\u0435";
static const std::wstring S_CAP_REPORT     = L"\u041e\u0442\u0447\u0451\u0442";
static const std::wstring S_ERR_NOPORTS    = L"\u0414\u043e\u0441\u0442\u0443\u043f \u043a \u043f\u043e\u0440\u0442\u0430\u043c \u043d\u0435\u0434\u043e\u0441\u0442\u0443\u043f\u0435\u043d (\u0434\u0440\u0430\u0439\u0432\u0435\u0440 giveio \u043d\u0435 \u043e\u0442\u043a\u0440\u044b\u0442).";
static const std::wstring S_ERR_NOTHING    = L"\u041d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d\u043e \u043d\u0438 \u043e\u0434\u043d\u043e\u0439 \u0444\u0443\u043d\u043a\u0446\u0438\u0438 PCI (\u0432\u0441\u0435 \u0447\u0442\u0435\u043d\u0438\u044f \u0432\u0435\u0440\u043d\u0443\u043b\u0438 0xFFFFFFFF).\r\n"
                                             L"\u0414\u043e\u0441\u0442\u0443\u043f \u043a \u043f\u043e\u0440\u0442\u0430\u043c 0xCF8/0xCFC \u0444\u0430\u043a\u0442\u0438\u0447\u0435\u0441\u043a\u0438 \u043d\u0435 \u0440\u0430\u0431\u043e\u0442\u0430\u0435\u0442.";
static const std::wstring S_ERR_SCAN_FIRST = L"\u0421\u043d\u0430\u0447\u0430\u043b\u0430 \u0432\u044b\u043f\u043e\u043b\u043d\u0438\u0442\u0435 \u0441\u043a\u0430\u043d\u0438\u0440\u043e\u0432\u0430\u043d\u0438\u0435.";
static const std::wstring S_REPORT_SAVED   = L"\u041e\u0442\u0447\u0451\u0442 \u0441\u043e\u0445\u0440\u0430\u043d\u0451\u043d \u0432 \u0444\u0430\u0439\u043b:\r\n";
static const std::wstring S_ERR_WRITE      = L"\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u0437\u0430\u043f\u0438\u0441\u0430\u0442\u044c \u0444\u0430\u0439\u043b:\r\n";

static const std::wstring S_YES            = L"\u0434\u0430";
static const std::wstring S_NO             = L"\u043d\u0435\u0442";
static const std::wstring S_BUS_KIND       = L"\u0422\u0438\u043f \u0448\u0438\u043d\u044b";
static const std::wstring S_BUS_ROOT       = L"\u043a\u043e\u0440\u043d\u0435\u0432\u0430\u044f (\u0437\u0430 \u0433\u043b\u0430\u0432\u043d\u044b\u043c \u043c\u043e\u0441\u0442\u043e\u043c Host Bridge)";
static const std::wstring S_BUS_BEHIND     = L"\u0432\u0442\u043e\u0440\u0438\u0447\u043d\u0430\u044f, \u0437\u0430 \u043c\u043e\u0441\u0442\u043e\u043c ";
static const std::wstring S_BUS_RANGE      = L"\u0414\u0438\u0430\u043f\u0430\u0437\u043e\u043d \u0448\u0438\u043d \u043c\u043e\u0441\u0442\u0430";
static const std::wstring S_BUS_DEVCOUNT   = L"\u0423\u0441\u0442\u0440\u043e\u0439\u0441\u0442\u0432 \u043d\u0430 \u0448\u0438\u043d\u0435";
static const std::wstring S_BUS_FNCOUNT    = L"\u0424\u0443\u043d\u043a\u0446\u0438\u0439 \u043d\u0430 \u0448\u0438\u043d\u0435";
static const std::wstring S_BUS_CHILDREN   = L"\u041f\u043e\u0434\u0447\u0438\u043d\u0451\u043d\u043d\u044b\u0435 \u0448\u0438\u043d\u044b";
static const std::wstring S_DEV_MULTI      = L"\u041c\u043d\u043e\u0433\u043e\u0444\u0443\u043d\u043a\u0446\u0438\u043e\u043d\u0430\u043b\u044c\u043d\u043e\u0435";
static const std::wstring S_DEV_FNCOUNT    = L"\u0427\u0438\u0441\u043b\u043e \u0444\u0443\u043d\u043a\u0446\u0438\u0439";
static const std::wstring S_MULTIFUNC      = L"\u043c\u043d\u043e\u0433\u043e\u0444\u0443\u043d\u043a\u0446\u0438\u043e\u043d\u0430\u043b\u044c\u043d\u043e\u0435";

static const std::wstring S_SEC_GENERAL    = L"\u041e\u0431\u0449\u0438\u0435 \u0441\u0432\u0435\u0434\u0435\u043d\u0438\u044f";
static const std::wstring S_SEC_BARS       = L"\u0420\u0435\u0433\u0438\u0441\u0442\u0440\u044b \u0431\u0430\u0437\u043e\u0432\u044b\u0445 \u0430\u0434\u0440\u0435\u0441\u043e\u0432 (BAR)";
static const std::wstring S_SEC_BRIDGE     = L"\u041c\u043e\u0441\u0442 PCI-to-PCI (\u0437\u0430\u0433\u043e\u043b\u043e\u0432\u043e\u043a \u0442\u0438\u043f\u0430 1)";
static const std::wstring S_SEC_CAPS       = L"\u0421\u043f\u0438\u0441\u043e\u043a Capabilities";
static const std::wstring S_SEC_DUMP       = L"\u041a\u043e\u043d\u0444\u0438\u0433\u0443\u0440\u0430\u0446\u0438\u043e\u043d\u043d\u043e\u0435 \u043f\u0440\u043e\u0441\u0442\u0440\u0430\u043d\u0441\u0442\u0432\u043e (256 \u0431\u0430\u0439\u0442)";
static const std::wstring S_SEC_FUNCS      = L"\u0424\u0443\u043d\u043a\u0446\u0438\u0438 \u0443\u0441\u0442\u0440\u043e\u0439\u0441\u0442\u0432\u0430";
static const std::wstring S_BAR_UNUSED     = L"\u043d\u0435 \u0438\u0441\u043f\u043e\u043b\u044c\u0437\u0443\u0435\u0442\u0441\u044f";
static const std::wstring S_BAR_UPPER      = L"\u0441\u0442\u0430\u0440\u0448\u0438\u0435 32 \u0431\u0438\u0442\u0430 \u043f\u0440\u0435\u0434\u044b\u0434\u0443\u0449\u0435\u0433\u043e BAR";
static const std::wstring S_WINDOW_CLOSED  = L"\u0437\u0430\u043a\u0440\u044b\u0442\u043e";

static const std::wstring S_REP_TITLE      = L"\u041e\u0422\u0427\u0401\u0422 \u041e \u041a\u041e\u041d\u0424\u0418\u0413\u0423\u0420\u0410\u0426\u0418\u0418 \u0428\u0418\u041d PCI";
static const std::wstring S_REP_METHOD     = L"\u0414\u043e\u0441\u0442\u0443\u043f: Configuration Mechanism #1, \u043f\u043e\u0440\u0442\u044b 0xCF8 / 0xCFC";
static const std::wstring S_REP_DATE       = L"\u0414\u0430\u0442\u0430: ";
static const std::wstring S_REP_TREE       = L"1. \u0414\u0415\u0420\u0415\u0412\u041e \u0422\u041e\u041f\u041e\u041b\u041e\u0413\u0418\u0418";
static const std::wstring S_REP_LIST       = L"2. \u0421\u041f\u0418\u0421\u041e\u041a \u0424\u0423\u041d\u041a\u0426\u0418\u0419";
static const std::wstring S_REP_DETAILS    = L"3. \u041f\u041e\u0414\u0420\u041e\u0411\u041d\u0410\u042f \u0418\u041d\u0424\u041e\u0420\u041c\u0410\u0426\u0418\u042f \u041e \u0424\u0423\u041d\u041a\u0426\u0418\u042f\u0425";
static const wchar_t      S_SAVE_FILTER[]  = L"\u0422\u0435\u043a\u0441\u0442\u043e\u0432\u044b\u0435 \u0444\u0430\u0439\u043b\u044b (*.txt)\0*.txt\0\u0412\u0441\u0435 \u0444\u0430\u0439\u043b\u044b (*.*)\0*.*\0";


// =====================================================================
// 1. Low-level access to the PCI configuration space
//    (Configuration Mechanism #1, ports 0x0CF8 / 0x0CFC).
// =====================================================================

static const unsigned short PCI_CONFIG_ADDRESS = 0x0CF8;
static const unsigned short PCI_CONFIG_DATA    = 0x0CFC;

// ---------------------------------------------------------------------
// Port access on Windows NT/2000/XP (32-bit) via the giveio.sys driver.
//
// In NT-family Windows the IN/OUT instructions are forbidden in user mode
// (ring 3). giveio.sys is a tiny kernel driver: when a process opens the
// device \\.\giveio, the driver clears that process's I/O permission bitmap,
// so the process may execute IN/OUT on any port. After that the ordinary
// IN/OUT instructions below (Configuration Mechanism #1, 0xCF8/0xCFC)
// work directly - no DLL is involved in the actual port accesses.
//
// The program installs and starts the driver itself through the Service
// Control Manager. Put giveio.sys next to the .exe and run as Administrator.
//
// A vectored exception handler catches "privileged instruction" faults,
// so if port access was NOT granted the program shows a message instead
// of crashing.
// ---------------------------------------------------------------------

static bool          g_portsReady = false;
static volatile bool g_portFault  = false;   // set by the exception handler

// Vectored exception handler: if IN/OUT is not allowed, skip the
// instruction (1 byte: 0xED = in eax,dx; 0xEF = out dx,eax), make IN
// return 0xFFFFFFFF and remember that a fault happened.
static LONG CALLBACK PortFaultHandler(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_ACCESS_VIOLATION) {
#ifdef _WIN64
        unsigned char* ip = (unsigned char*)ep->ContextRecord->Rip;
        if (*ip == 0xED || *ip == 0xEF) {
            if (*ip == 0xED) ep->ContextRecord->Rax = 0xFFFFFFFF;
            ep->ContextRecord->Rip += 1;
#else
        unsigned char* ip = (unsigned char*)ep->ContextRecord->Eip;
        if (*ip == 0xED || *ip == 0xEF) {
            if (*ip == 0xED) ep->ContextRecord->Eax = 0xFFFFFFFF;
            ep->ContextRecord->Eip += 1;
#endif
            g_portFault = true;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void InstallFaultHandler()
{
    typedef LONG  (CALLBACK *MY_VEH)(PEXCEPTION_POINTERS);
    typedef PVOID (WINAPI   *PFN_ADD_VEH)(ULONG, MY_VEH);
    PFN_ADD_VEH pfnAdd = (PFN_ADD_VEH)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "AddVectoredExceptionHandler");
    if (pfnAdd) pfnAdd(1, PortFaultHandler);
}

static bool OpenGiveio()
{
    HANDLE h = CreateFileA("\\\\.\\giveio", GENERIC_READ, 0, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h);          // the I/O permission stays with our process
    Sleep(10);
    return true;
}

static bool InstallAndStartGiveio(std::string& error)
{
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0;
    strcat(path, "giveio.sys");

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        error = std::string("giveio.sys was not found:\r\n") + path +
                "\r\nCopy giveio.sys into the same folder as this .exe.";
        return false;
    }

    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        error = "OpenSCManager failed. Run this program as Administrator.";
        return false;
    }

    SC_HANDLE svc = CreateServiceA(scm, "giveio", "giveio", SERVICE_ALL_ACCESS,
                                   SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                                   SERVICE_ERROR_NORMAL, path,
                                   NULL, NULL, NULL, NULL, NULL);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS)
        svc = OpenServiceA(scm, "giveio", SERVICE_ALL_ACCESS);
    if (!svc) {
        std::stringstream ss;
        ss << "Cannot create/open the giveio service, error " << GetLastError();
        error = ss.str();
        CloseServiceHandle(scm);
        return false;
    }

    if (!StartServiceA(svc, 0, NULL) &&
        GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        std::stringstream ss;
        ss << "Cannot start the giveio driver, error " << GetLastError();
        error = ss.str();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

static unsigned int InPortD(unsigned short port);
static void OutPortD(unsigned short port, unsigned int value);

static bool InitPortAccess(std::string& error)
{
    InstallFaultHandler();

    if (!OpenGiveio()) {
        if (!InstallAndStartGiveio(error))
            return false;
        if (!OpenGiveio()) {
            error = "The giveio driver is running, but \\\\.\\giveio cannot be opened.";
            return false;
        }
    }

    // Probe: one real access to CONFIG_ADDRESS / CONFIG_DATA
    g_portFault = false;
    OutPortD(PCI_CONFIG_ADDRESS, 0x80000000u);
    InPortD(PCI_CONFIG_DATA);
    if (g_portFault) {
        error = "giveio was opened, but IN/OUT are still not allowed.";
        return false;
    }

    g_portsReady = true;
    return true;
}

#if defined(__GNUC__)
    // MinGW/TDM-GCC: direct IN/OUT instructions via inline assembly (AT&T syntax)
    static void OutPortD(unsigned short port, unsigned int value)
    {
        __asm__ __volatile__ ("outl %0, %w1" : : "a"(value), "d"(port));
    }
    static unsigned int InPortD(unsigned short port)
    {
        unsigned int ret;
        __asm__ __volatile__ ("inl %w1, %0" : "=a"(ret) : "d"(port));
        return ret;
    }
#else
    // MSVC: __outdword/__indword intrinsics
    #include <intrin.h>
    static void OutPortD(unsigned short port, unsigned int value) { __outdword(port, (unsigned long)value); }
    static unsigned int InPortD(unsigned short port) { return (unsigned int)__indword(port); }
#endif

// CONFIG_ADDRESS layout:
// [31]=Enable, [30:24]=0, [23:16]=Bus, [15:11]=Device, [10:8]=Function,
// [7:2]=Register (dword index), [1:0]=00
static unsigned int PciMakeAddress(unsigned char bus, unsigned char device,
                                   unsigned char function, unsigned char reg)
{
    return 0x80000000u
        | ((unsigned int)bus << 16)
        | ((unsigned int)(device & 0x1F) << 11)
        | ((unsigned int)(function & 0x07) << 8)
        | ((unsigned int)reg & 0xFC);
}

static unsigned int PciReadDWord(unsigned char bus, unsigned char device,
                                 unsigned char function, unsigned char reg)
{
    OutPortD(PCI_CONFIG_ADDRESS, PciMakeAddress(bus, device, function, reg));
    return InPortD(PCI_CONFIG_DATA);
}


// =====================================================================
// 2. Data structures:  Bus -> Device -> Function
// =====================================================================

struct PciFunction
{
    unsigned char  bus, device, function;
    unsigned char  cfg[256];          // copy of the whole configuration space

    // Decoded header fields (common part, offsets 0x00..0x0F)
    unsigned short vendorId, deviceId;
    unsigned short command, status;
    unsigned char  revisionId, progIf, subclass, classCode;
    unsigned char  cacheLineSize, latencyTimer, headerType, bist;
    unsigned char  interruptLine, interruptPin;

    // Type 0 (normal device)
    unsigned short subsystemVendorId, subsystemId;

    // PCI-to-PCI bridge (Class 0x06, Subclass 0x04 / header type 1)
    bool          isBridge;
    unsigned char primaryBus, secondaryBus, subordinateBus;

    bool isMultiFunction; // Header Type bit 7

    PciFunction()
    {
        bus = device = function = 0;
        memset(cfg, 0xFF, sizeof(cfg));
        vendorId = 0xFFFF; deviceId = 0xFFFF;
        command = 0; status = 0;
        revisionId = progIf = subclass = classCode = 0;
        cacheLineSize = latencyTimer = headerType = bist = 0;
        interruptLine = interruptPin = 0;
        subsystemVendorId = 0; subsystemId = 0;
        isBridge = false;
        primaryBus = secondaryBus = subordinateBus = 0;
        isMultiFunction = false;
    }

    unsigned char  Cfg8 (int off) const { return cfg[off & 0xFF]; }
    unsigned short Cfg16(int off) const { return (unsigned short)(Cfg8(off) | (Cfg8(off + 1) << 8)); }
    unsigned int   Cfg32(int off) const { return (unsigned int)Cfg16(off) | ((unsigned int)Cfg16(off + 2) << 16); }
    int HeaderKind() const { return headerType & 0x7F; }
};

struct PciDevice
{
    unsigned char            number;      // 0..31
    std::vector<PciFunction> functions;   // 1..8 functions

    PciDevice() : number(0) {}
};

struct PciBusNode
{
    unsigned char             busNumber;
    bool                      hasParentBridge;
    PciFunction               parentBridge;   // valid if hasParentBridge
    std::vector<PciDevice>    devices;
    std::vector<PciBusNode*>  childBuses;     // buses behind bridges on this bus

    PciBusNode() : busNumber(0), hasParentBridge(false) {}
};

static void FreeBusNode(PciBusNode* node)
{
    if (!node) return;
    for (size_t i = 0; i < node->childBuses.size(); ++i)
        FreeBusNode(node->childBuses[i]);
    delete node;
}


// =====================================================================
// 3. Names: vendors and Class/Subclass codes
// =====================================================================

struct VendorEntry { unsigned short id; const wchar_t* name; };

static const VendorEntry g_vendors[] = {
    { 0x8086, L"Intel" },            { 0x8087, L"Intel" },
    { 0x1022, L"AMD" },              { 0x1002, L"AMD/ATI" },
    { 0x10DE, L"NVIDIA" },           { 0x1106, L"VIA" },
    { 0x10EC, L"Realtek" },          { 0x80EE, L"InnoTek (VirtualBox)" },
    { 0x15AD, L"VMware" },           { 0x1234, L"QEMU/Bochs" },
    { 0x1AF4, L"Red Hat (virtio)" }, { 0x1B36, L"Red Hat (QEMU)" },
    { 0x1414, L"Microsoft" },        { 0x14E4, L"Broadcom" },
    { 0x168C, L"Qualcomm Atheros" }, { 0x1969, L"Qualcomm Atheros" },
    { 0x11AB, L"Marvell" },          { 0x1B4B, L"Marvell" },
    { 0x144D, L"Samsung" },          { 0x1039, L"SiS" },
    { 0x1102, L"Creative Labs" },    { 0x1274, L"Ensoniq" },
    { 0x1000, L"LSI Logic" },        { 0x104C, L"Texas Instruments" },
    { 0x1B21, L"ASMedia" },          { 0x1180, L"Ricoh" },
    { 0x10B9, L"ALi" },              { 0x1013, L"Cirrus Logic" },
    { 0x5333, L"S3" },               { 0x102B, L"Matrox" },
    { 0x1033, L"NEC" },              { 0x1095, L"Silicon Image" },
    { 0x197B, L"JMicron" },          { 0x1912, L"Renesas" },
    { 0x1217, L"O2 Micro" },         { 0x13F6, L"C-Media" },
    { 0x1814, L"Ralink" },           { 0x10B7, L"3Com" },
    { 0x1011, L"DEC" },              { 0x9004, L"Adaptec" },
    { 0x9005, L"Adaptec" },          { 0x1043, L"ASUS" },
    { 0x1458, L"Gigabyte" },         { 0x1462, L"MSI" },
    { 0x17AA, L"Lenovo" },           { 0x103C, L"HP" },
    { 0x1028, L"Dell" },             { 0x15B3, L"Mellanox" },
    { 0x1C5C, L"SK hynix" },         { 0x126F, L"Silicon Motion" },
    { 0x1344, L"Micron" },           { 0x15B7, L"SanDisk/WD" },
    { 0x1E0F, L"KIOXIA" },           { 0x10EE, L"Xilinx" },
    { 0x14F1, L"Conexant" },         { 0x1283, L"ITE" }
};

static std::wstring VendorName(unsigned short id)
{
    for (size_t i = 0; i < sizeof(g_vendors) / sizeof(g_vendors[0]); ++i)
        if (g_vendors[i].id == id)
            return g_vendors[i].name;
    return L"Unknown";
}

// Common Class/Subclass codes (see lab manual, section 3.7)
static std::wstring ClassName(const PciFunction& f)
{
    unsigned char s = f.subclass;
    switch (f.classCode) {
    case 0x00: return s == 0x01 ? L"VGA-Compatible Device" : L"Unclassified Device";
    case 0x01:
        switch (s) {
        case 0x00: return L"SCSI Controller";
        case 0x01: return L"IDE Controller";
        case 0x02: return L"Floppy Controller";
        case 0x04: return L"RAID Controller";
        case 0x05: return L"ATA Controller";
        case 0x06: return L"SATA Controller";
        case 0x07: return L"SAS Controller";
        case 0x08: return L"NVMe Controller";
        default:   return L"Mass Storage Controller";
        }
    case 0x02:
        switch (s) {
        case 0x00: return L"Ethernet Controller";
        case 0x01: return L"Token Ring Controller";
        case 0x02: return L"FDDI Controller";
        case 0x03: return L"ATM Controller";
        default:   return L"Network Controller";
        }
    case 0x03:
        switch (s) {
        case 0x00: return L"VGA Compatible Controller";
        case 0x01: return L"XGA Controller";
        case 0x02: return L"3D Controller";
        default:   return L"Display Controller";
        }
    case 0x04:
        switch (s) {
        case 0x00: return L"Multimedia Video Controller";
        case 0x01: return L"Multimedia Audio Controller";
        case 0x03: return L"HD Audio Controller";
        default:   return L"Multimedia Controller";
        }
    case 0x05: return s == 0x00 ? L"RAM Controller" : L"Memory Controller";
    case 0x06:
        switch (s) {
        case 0x00: return L"Host Bridge";
        case 0x01: return L"ISA Bridge";
        case 0x02: return L"EISA Bridge";
        case 0x03: return L"MCA Bridge";
        case 0x04: return L"PCI-to-PCI Bridge";
        case 0x05: return L"PCMCIA Bridge";
        case 0x07: return L"CardBus Bridge";
        case 0x09: return L"Semi-Transparent PCI-to-PCI Bridge";
        default:   return L"Bridge Device";
        }
    case 0x07:
        switch (s) {
        case 0x00: return L"Serial Controller";
        case 0x01: return L"Parallel Controller";
        case 0x03: return L"Modem";
        default:   return L"Communication Controller";
        }
    case 0x08:
        switch (s) {
        case 0x00: return L"PIC";
        case 0x01: return L"DMA Controller";
        case 0x02: return L"Timer";
        case 0x03: return L"RTC Controller";
        case 0x05: return L"SD Host Controller";
        case 0x06: return L"IOMMU";
        default:   return L"System Peripheral";
        }
    case 0x09: return L"Input Device Controller";
    case 0x0A: return L"Docking Station";
    case 0x0B: return L"Processor";
    case 0x0C:
        switch (s) {
        case 0x00: return L"FireWire (IEEE 1394) Controller";
        case 0x03:
            switch (f.progIf) {
            case 0x00: return L"USB UHCI Controller";
            case 0x10: return L"USB OHCI Controller";
            case 0x20: return L"USB EHCI Controller";
            case 0x30: return L"USB xHCI Controller";
            default:   return L"USB Controller";
            }
        case 0x05: return L"SMBus Controller";
        default:   return L"Serial Bus Controller";
        }
    case 0x0D: return L"Wireless Controller";
    case 0x0E: return L"Intelligent Controller";
    case 0x0F: return L"Satellite Communication Controller";
    case 0x10: return L"Encryption Controller";
    case 0x11: return L"Signal Processing Controller";
    case 0x12: return L"Processing Accelerator";
    case 0xFF: return L"Unassigned Class";
    default:   return L"Unknown Class";
    }
}

static std::wstring CapabilityName(unsigned char id)
{
    switch (id) {
    case 0x01: return L"Power Management";
    case 0x02: return L"AGP";
    case 0x03: return L"VPD";
    case 0x04: return L"Slot Identification";
    case 0x05: return L"MSI";
    case 0x06: return L"CompactPCI Hot Swap";
    case 0x07: return L"PCI-X";
    case 0x08: return L"HyperTransport";
    case 0x09: return L"Vendor Specific";
    case 0x0A: return L"Debug Port";
    case 0x0C: return L"PCI Hot-Plug";
    case 0x0D: return L"Bridge Subsystem Vendor ID";
    case 0x0E: return L"AGP 8x";
    case 0x0F: return L"Secure Device";
    case 0x10: return L"PCI Express";
    case 0x11: return L"MSI-X";
    case 0x12: return L"SATA Configuration";
    case 0x13: return L"Advanced Features";
    default:   return L"Unknown";
    }
}


// =====================================================================
// 4. Formatting helpers and node labels
// =====================================================================

static std::wstring Num(int v)
{
    std::wstringstream ss;
    ss << v;
    return ss.str();
}

static std::wstring HexRaw(unsigned int v, int width)
{
    std::wstringstream ss;
    ss << std::hex << std::uppercase << std::setfill(L'0') << std::setw(width) << v;
    return ss.str();
}

static std::wstring Hex(unsigned int v, int width)
{
    return L"0x" + HexRaw(v, width);
}

// "Label ......: value" line with a fixed label column
static void Field(std::wstringstream& ss, const std::wstring& label, const std::wstring& value)
{
    ss << label;
    if (label.size() < 26)
        ss << std::wstring(26 - label.size(), L' ');
    ss << L": " << value << L"\r\n";
}

static void Section(std::wstringstream& ss, const std::wstring& title)
{
    ss << L"\r\n--- " << title << L" ---\r\n";
}

static std::wstring Bdf(const PciFunction& f)
{
    return HexRaw(f.bus, 2) + L":" + HexRaw(f.device, 2) + L"." + Num(f.function);
}

static std::wstring BusLabel(const PciBusNode* b)
{
    std::wstring s = S_BUS + L" " + Num(b->busNumber);
    if (!b->hasParentBridge)
        s += L" (" + S_ROOT_BUS + L")";
    return s;
}

static std::wstring DeviceLabel(const PciDevice& d)
{
    return S_DEVICE + L" " + Num(d.number);
}

// "Function 0 (Vendor: Intel, Device: Host Bridge)"
static std::wstring FunctionLabel(const PciFunction& f)
{
    std::wstring s = S_FUNCTION + L" " + Num(f.function) +
                     L" (Vendor: " + VendorName(f.vendorId) +
                     L", Device: " + ClassName(f) + L")";
    if (f.isBridge)
        s += L" - " + S_BRIDGE_TO + L" " + Num(f.secondaryBus);
    return s;
}

// "Bus 0 -> Device 0 -> Function 0"
static std::wstring FunctionPath(const PciFunction& f)
{
    return S_BUS + L" " + Num(f.bus) + S_ARROW +
           S_DEVICE + L" " + Num(f.device) + S_ARROW +
           S_FUNCTION + L" " + Num(f.function);
}


// =====================================================================
// 5. Recursive PCI enumeration (Bus -> Device -> Function, bridges)
// =====================================================================

static bool                     g_busVisited[256];
static std::vector<PciBusNode*> g_roots;         // root buses (bus 0 + others)

static bool ReadFunction(unsigned char bus, unsigned char device,
                         unsigned char function, PciFunction& f)
{
    unsigned int id = PciReadDWord(bus, device, function, 0x00);
    unsigned short vendor = (unsigned short)(id & 0xFFFF);
    if (vendor == 0xFFFF || vendor == 0x0000)
        return false; // device/function does not exist

    f.bus = bus; f.device = device; f.function = function;

    // Read the whole 256-byte configuration space, dword by dword
    for (int reg = 0; reg < 256; reg += 4) {
        unsigned int v = (reg == 0) ? id : PciReadDWord(bus, device, function, (unsigned char)reg);
        f.cfg[reg + 0] = (unsigned char)(v & 0xFF);
        f.cfg[reg + 1] = (unsigned char)((v >> 8) & 0xFF);
        f.cfg[reg + 2] = (unsigned char)((v >> 16) & 0xFF);
        f.cfg[reg + 3] = (unsigned char)((v >> 24) & 0xFF);
    }

    f.vendorId      = f.Cfg16(0x00);
    f.deviceId      = f.Cfg16(0x02);
    f.command       = f.Cfg16(0x04);
    f.status        = f.Cfg16(0x06);
    f.revisionId    = f.Cfg8(0x08);
    f.progIf        = f.Cfg8(0x09);
    f.subclass      = f.Cfg8(0x0A);
    f.classCode     = f.Cfg8(0x0B);
    f.cacheLineSize = f.Cfg8(0x0C);
    f.latencyTimer  = f.Cfg8(0x0D);
    f.headerType    = f.Cfg8(0x0E);
    f.bist          = f.Cfg8(0x0F);
    f.interruptLine = f.Cfg8(0x3C);
    f.interruptPin  = f.Cfg8(0x3D);
    f.isMultiFunction = (f.headerType & 0x80) != 0;

    // PCI-to-PCI bridge: Class = 0x06, Subclass = 0x04 (header type 1)
    f.isBridge = (f.classCode == 0x06 && f.subclass == 0x04) || f.HeaderKind() == 0x01;
    if (f.isBridge) {
        f.primaryBus     = f.Cfg8(0x18);
        f.secondaryBus   = f.Cfg8(0x19);
        f.subordinateBus = f.Cfg8(0x1A);
    } else if (f.HeaderKind() == 0x00) {
        f.subsystemVendorId = f.Cfg16(0x2C);
        f.subsystemId       = f.Cfg16(0x2E);
    }
    return true;
}

// Scans one bus completely and then, recursively, every bus behind
// the PCI-to-PCI bridges found on it.
static PciBusNode* ScanBus(unsigned char bus, const PciFunction* parentBridge, int depth)
{
    PciBusNode* node = new PciBusNode();
    node->busNumber = bus;
    if (parentBridge) {
        node->hasParentBridge = true;
        node->parentBridge = *parentBridge;
    }
    g_busVisited[bus] = true;

    for (int device = 0; device < 32; ++device) {
        PciFunction f0;
        if (!ReadFunction(bus, (unsigned char)device, 0, f0))
            continue; // function 0 missing -> no device at all

        PciDevice d;
        d.number = (unsigned char)device;
        d.functions.push_back(f0);

        if (f0.isMultiFunction) {
            for (int fn = 1; fn < 8; ++fn) {
                PciFunction f;
                if (ReadFunction(bus, (unsigned char)device, (unsigned char)fn, f))
                    d.functions.push_back(f);
            }
        }
        node->devices.push_back(d);
    }

    // Recursion: descend into the secondary bus of every bridge.
    // g_busVisited protects against loops on a badly configured system.
    if (depth < 32) {
        for (size_t i = 0; i < node->devices.size(); ++i) {
            for (size_t j = 0; j < node->devices[i].functions.size(); ++j) {
                const PciFunction& f = node->devices[i].functions[j];
                if (f.isBridge && f.secondaryBus != 0 && !g_busVisited[f.secondaryBus])
                    node->childBuses.push_back(ScanBus(f.secondaryBus, &f, depth + 1));
            }
        }
    }
    return node;
}

static bool BusHasDevices(unsigned char bus)
{
    for (int device = 0; device < 32; ++device) {
        unsigned short vendor = (unsigned short)(PciReadDWord(bus, (unsigned char)device, 0, 0x00) & 0xFFFF);
        if (vendor != 0xFFFF && vendor != 0x0000)
            return true;
    }
    return false;
}

static void FreeAllBuses()
{
    for (size_t i = 0; i < g_roots.size(); ++i)
        FreeBusNode(g_roots[i]);
    g_roots.clear();
}

// Full topology:
//  1) bus 0 and, recursively, everything behind its bridges;
//  2) buses 1..255 that were not reached through a bridge (additional
//     root buses of other host bridges) - found by trying every number.
static void ScanAllBuses()
{
    FreeAllBuses();
    memset(g_busVisited, 0, sizeof(g_busVisited));

    g_roots.push_back(ScanBus(0, NULL, 0));
    for (int bus = 1; bus < 256; ++bus) {
        if (!g_busVisited[bus] && BusHasDevices((unsigned char)bus))
            g_roots.push_back(ScanBus((unsigned char)bus, NULL, 0));
    }
}

static void CountAll(const PciBusNode* node, int& buses, int& devices, int& functions)
{
    ++buses;
    devices += (int)node->devices.size();
    for (size_t i = 0; i < node->devices.size(); ++i)
        functions += (int)node->devices[i].functions.size();
    for (size_t i = 0; i < node->childBuses.size(); ++i)
        CountAll(node->childBuses[i], buses, devices, functions);
}

static const PciBusNode* FindChildBus(const PciBusNode* bus, const PciFunction& bridge)
{
    for (size_t i = 0; i < bus->childBuses.size(); ++i) {
        const PciBusNode* c = bus->childBuses[i];
        if (c->busNumber == bridge.secondaryBus &&
            c->parentBridge.device == bridge.device &&
            c->parentBridge.function == bridge.function)
            return c;
    }
    return NULL;
}


// =====================================================================
// 6. Detailed information for the right panel / report
// =====================================================================

static std::wstring CommandBits(unsigned short cmd)
{
    std::wstring s;
    s += (cmd & 0x0001) ? L"I/O+ "  : L"I/O- ";
    s += (cmd & 0x0002) ? L"Mem+ "  : L"Mem- ";
    s += (cmd & 0x0004) ? L"BusMaster+ " : L"BusMaster- ";
    s += (cmd & 0x0400) ? L"IntDis+" : L"IntDis-";
    return s;
}

static std::wstring PinName(unsigned char pin)
{
    switch (pin) {
    case 0:  return S_NO;
    case 1:  return L"INTA#";
    case 2:  return L"INTB#";
    case 3:  return L"INTC#";
    case 4:  return L"INTD#";
    default: return Hex(pin, 2);
    }
}

static void AppendBars(std::wstringstream& ss, const PciFunction& f)
{
    int count = (f.HeaderKind() == 0x00) ? 6 : (f.HeaderKind() == 0x01 ? 2 : 0);
    if (count == 0) return;

    Section(ss, S_SEC_BARS);
    for (int i = 0; i < count; ++i) {
        int off = 0x10 + i * 4;
        unsigned int v = f.Cfg32(off);
        std::wstring name = L"BAR" + Num(i) + L" [" + Hex(off, 2) + L"]";
        std::wstring desc;

        if (v == 0) {
            desc = S_BAR_UNUSED;
        } else if (v & 0x1) {
            desc = L"I/O, base " + Hex(v & 0xFFFFFFFCu, 4);
        } else {
            unsigned int type = (v >> 1) & 0x3;
            if (type == 0x2 && i + 1 < count) {
                unsigned int hi = f.Cfg32(off + 4);
                desc = L"Memory 64-bit, base 0x" + HexRaw(hi, 8) + HexRaw(v & 0xFFFFFFF0u, 8);
                if (v & 0x8) desc += L", prefetchable";
                Field(ss, name, Hex(v, 8) + L"  " + desc);
                ++i;
                off = 0x10 + i * 4;
                Field(ss, L"BAR" + Num(i) + L" [" + Hex(off, 2) + L"]",
                      Hex(hi, 8) + L"  " + S_BAR_UPPER);
                continue;
            }
            desc = L"Memory 32-bit, base " + Hex(v & 0xFFFFFFF0u, 8);
            if (v & 0x8) desc += L", prefetchable";
        }
        Field(ss, name, Hex(v, 8) + L"  " + desc);
    }
}

static void AppendBridge(std::wstringstream& ss, const PciFunction& f)
{
    if (!f.isBridge) return;

    Section(ss, S_SEC_BRIDGE);
    Field(ss, L"Primary Bus",     Num(f.primaryBus));
    Field(ss, L"Secondary Bus",   Num(f.secondaryBus));
    Field(ss, L"Subordinate Bus", Num(f.subordinateBus));
    Field(ss, L"Secondary Latency", Num(f.Cfg8(0x1B)));

    // I/O window
    unsigned int ioBase  = (unsigned int)(f.Cfg8(0x1C) & 0xF0) << 8;
    unsigned int ioLimit = ((unsigned int)(f.Cfg8(0x1D) & 0xF0) << 8) | 0xFFF;
    if ((f.Cfg8(0x1C) & 0x0F) == 0x01) {
        ioBase  |= (unsigned int)f.Cfg16(0x30) << 16;
        ioLimit |= (unsigned int)f.Cfg16(0x32) << 16;
    }
    Field(ss, L"I/O window", ioBase <= ioLimit
          ? Hex(ioBase, 4) + L" - " + Hex(ioLimit, 4) : S_WINDOW_CLOSED);

    // Memory window
    unsigned int memBase  = (unsigned int)(f.Cfg16(0x20) & 0xFFF0) << 16;
    unsigned int memLimit = ((unsigned int)(f.Cfg16(0x22) & 0xFFF0) << 16) | 0xFFFFF;
    Field(ss, L"Memory window", memBase <= memLimit
          ? Hex(memBase, 8) + L" - " + Hex(memLimit, 8) : S_WINDOW_CLOSED);

    // Prefetchable memory window
    unsigned int pBase  = (unsigned int)(f.Cfg16(0x24) & 0xFFF0) << 16;
    unsigned int pLimit = ((unsigned int)(f.Cfg16(0x26) & 0xFFF0) << 16) | 0xFFFFF;
    unsigned int pBaseHi = 0, pLimitHi = 0;
    if ((f.Cfg16(0x24) & 0x000F) == 0x0001) {
        pBaseHi  = f.Cfg32(0x28);
        pLimitHi = f.Cfg32(0x2C);
    }
    bool open = (pBaseHi < pLimitHi) || (pBaseHi == pLimitHi && pBase <= pLimit);
    Field(ss, L"Prefetchable window", open
          ? L"0x" + HexRaw(pBaseHi, 8) + HexRaw(pBase, 8) + L" - 0x" + HexRaw(pLimitHi, 8) + HexRaw(pLimit, 8)
          : S_WINDOW_CLOSED);
    Field(ss, L"Bridge Control", Hex(f.Cfg16(0x3E), 4));
}

static void AppendCapabilities(std::wstringstream& ss, const PciFunction& f)
{
    if (!(f.status & 0x0010))     // Status bit 4: capabilities list present
        return;
    int ptrOffset = (f.HeaderKind() == 0x02) ? 0x14 : 0x34;
    int ptr = f.Cfg8(ptrOffset) & 0xFC;

    Section(ss, S_SEC_CAPS);
    for (int guard = 0; ptr >= 0x40 && guard < 48; ++guard) {
        unsigned char id = f.Cfg8(ptr);
        Field(ss, L"Offset " + Hex(ptr, 2), Hex(id, 2) + L"  " + CapabilityName(id));
        ptr = f.Cfg8(ptr + 1) & 0xFC;
    }
}

static void AppendHexDump(std::wstringstream& ss, const PciFunction& f)
{
    Section(ss, S_SEC_DUMP);
    ss << L"     00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\r\n";
    for (int row = 0; row < 256; row += 16) {
        ss << HexRaw(row, 2) << L":  ";
        for (int col = 0; col < 16; ++col)
            ss << HexRaw(f.cfg[row + col], 2) << L" ";
        ss << L"\r\n";
    }
}

static std::wstring FormatFunctionDetails(const PciFunction& f)
{
    std::wstringstream ss;
    ss << FunctionPath(f) << L"\r\n";
    ss << FunctionLabel(f) << L"\r\n";

    Section(ss, S_SEC_GENERAL);
    Field(ss, L"BDF (bus:dev.fn)",  Bdf(f));
    Field(ss, L"Vendor",            VendorName(f.vendorId) + L" (" + Hex(f.vendorId, 4) + L")");
    Field(ss, L"Device ID",         Hex(f.deviceId, 4));
    Field(ss, L"Device (class)",    ClassName(f));
    Field(ss, L"Class Code",        Hex(f.classCode, 2));
    Field(ss, L"Subclass",          Hex(f.subclass, 2));
    Field(ss, L"Prog IF",           Hex(f.progIf, 2));
    Field(ss, L"Revision ID",       Hex(f.revisionId, 2));
    Field(ss, L"Header Type",       Hex(f.headerType, 2) + L" (type " + Num(f.HeaderKind()) + L")" +
                                    (f.isMultiFunction ? L", " + S_MULTIFUNC : std::wstring()));
    Field(ss, L"Command",           Hex(f.command, 4) + L"  " + CommandBits(f.command));
    Field(ss, L"Status",            Hex(f.status, 4));
    Field(ss, L"Cache Line Size",   Num(f.cacheLineSize));
    Field(ss, L"Latency Timer",     Num(f.latencyTimer));
    Field(ss, L"BIST",              Hex(f.bist, 2));
    Field(ss, L"Interrupt Line",    Num(f.interruptLine));
    Field(ss, L"Interrupt Pin",     PinName(f.interruptPin));
    if (f.HeaderKind() == 0x00) {
        Field(ss, L"Subsystem Vendor", VendorName(f.subsystemVendorId) + L" (" + Hex(f.subsystemVendorId, 4) + L")");
        Field(ss, L"Subsystem ID",     Hex(f.subsystemId, 4));
    }

    AppendBars(ss, f);
    AppendBridge(ss, f);
    AppendCapabilities(ss, f);
    AppendHexDump(ss, f);
    return ss.str();
}

static std::wstring FormatBusDetails(const PciBusNode* b)
{
    int buses = 0, devices = 0, functions = 0;
    CountAll(b, buses, devices, functions);

    int fnOnBus = 0;
    for (size_t i = 0; i < b->devices.size(); ++i)
        fnOnBus += (int)b->devices[i].functions.size();

    std::wstringstream ss;
    ss << BusLabel(b) << L"\r\n";
    Section(ss, S_SEC_GENERAL);
    if (b->hasParentBridge) {
        const PciFunction& br = b->parentBridge;
        Field(ss, S_BUS_KIND, S_BUS_BEHIND + Bdf(br));
        Field(ss, L"Bridge", FunctionPath(br));
        Field(ss, S_BUS_RANGE, Num(br.secondaryBus) + L" - " + Num(br.subordinateBus));
    } else {
        Field(ss, S_BUS_KIND, S_BUS_ROOT);
    }
    Field(ss, S_BUS_DEVCOUNT, Num((int)b->devices.size()));
    Field(ss, S_BUS_FNCOUNT,  Num(fnOnBus));

    std::wstring children;
    for (size_t i = 0; i < b->childBuses.size(); ++i) {
        if (i) children += L", ";
        children += Num(b->childBuses[i]->busNumber);
    }
    Field(ss, S_BUS_CHILDREN, children.empty() ? S_NO : children);

    Section(ss, S_SEC_FUNCS);
    for (size_t i = 0; i < b->devices.size(); ++i)
        for (size_t j = 0; j < b->devices[i].functions.size(); ++j) {
            const PciFunction& f = b->devices[i].functions[j];
            ss << Bdf(f) << L"  " << HexRaw(f.vendorId, 4) << L":" << HexRaw(f.deviceId, 4)
               << L"  " << VendorName(f.vendorId) << L", " << ClassName(f) << L"\r\n";
        }
    return ss.str();
}

static std::wstring FormatDeviceDetails(const PciBusNode* b, const PciDevice& d)
{
    std::wstringstream ss;
    ss << S_BUS << L" " << Num(b->busNumber) << S_ARROW << DeviceLabel(d) << L"\r\n";
    Section(ss, S_SEC_GENERAL);
    Field(ss, S_DEV_MULTI,   d.functions[0].isMultiFunction ? S_YES : S_NO);
    Field(ss, S_DEV_FNCOUNT, Num((int)d.functions.size()));
    Field(ss, L"Vendor",     VendorName(d.functions[0].vendorId) + L" (" + Hex(d.functions[0].vendorId, 4) + L")");
    Field(ss, L"Device ID",  Hex(d.functions[0].deviceId, 4));

    Section(ss, S_SEC_FUNCS);
    for (size_t j = 0; j < d.functions.size(); ++j)
        ss << FunctionLabel(d.functions[j]) << L"\r\n";
    return ss.str();
}


// =====================================================================
// 7. Report file (UTF-8 text, opens in Notepad)
// =====================================================================

static void ReportTree(std::wstringstream& ss, const PciBusNode* b, int indent)
{
    std::wstring pad(indent * 4, L' ');
    ss << pad << BusLabel(b) << L"\r\n";
    for (size_t i = 0; i < b->devices.size(); ++i) {
        const PciDevice& d = b->devices[i];
        ss << pad << L"    " << DeviceLabel(d) << L"\r\n";
        for (size_t j = 0; j < d.functions.size(); ++j) {
            const PciFunction& f = d.functions[j];
            ss << pad << L"        " << FunctionLabel(f) << L"\r\n";
            if (f.isBridge) {
                const PciBusNode* child = FindChildBus(b, f);
                if (child) ReportTree(ss, child, indent + 3);
            }
        }
    }
}

// All functions in tree order
static void CollectFunctions(const PciBusNode* b, std::vector<const PciFunction*>& out)
{
    for (size_t i = 0; i < b->devices.size(); ++i)
        for (size_t j = 0; j < b->devices[i].functions.size(); ++j) {
            const PciFunction& f = b->devices[i].functions[j];
            out.push_back(&f);
            if (f.isBridge) {
                const PciBusNode* child = FindChildBus(b, f);
                if (child) CollectFunctions(child, out);
            }
        }
}

static std::wstring BuildReport()
{
    int buses = 0, devices = 0, functions = 0;
    for (size_t i = 0; i < g_roots.size(); ++i)
        CountAll(g_roots[i], buses, devices, functions);

    SYSTEMTIME st;
    GetLocalTime(&st);

    std::wstringstream ss;
    ss << S_REP_TITLE << L"\r\n" << std::wstring(70, L'=') << L"\r\n";
    ss << S_REP_DATE << std::setfill(L'0')
       << std::setw(2) << st.wDay << L"." << std::setw(2) << st.wMonth << L"." << st.wYear << L"  "
       << std::setw(2) << st.wHour << L":" << std::setw(2) << st.wMinute << L":" << std::setw(2) << st.wSecond
       << std::setfill(L' ') << L"\r\n";
    ss << S_REP_METHOD << L"\r\n";
    ss << S_FOUND_BUSES << buses << S_FOUND_DEVICES << devices << S_FOUND_FUNCS << functions << L"\r\n";

    ss << L"\r\n" << S_REP_TREE << L"\r\n" << std::wstring(70, L'-') << L"\r\n";
    for (size_t i = 0; i < g_roots.size(); ++i)
        ReportTree(ss, g_roots[i], 0);

    std::vector<const PciFunction*> all;
    for (size_t i = 0; i < g_roots.size(); ++i)
        CollectFunctions(g_roots[i], all);

    ss << L"\r\n" << S_REP_LIST << L"\r\n" << std::wstring(70, L'-') << L"\r\n";
    for (size_t i = 0; i < all.size(); ++i)
        ss << FunctionPath(*all[i]) << L" (Vendor: " << VendorName(all[i]->vendorId)
           << L", Device: " << ClassName(*all[i]) << L")\r\n";

    ss << L"\r\n" << S_REP_DETAILS << L"\r\n" << std::wstring(70, L'-') << L"\r\n";
    for (size_t i = 0; i < all.size(); ++i)
        ss << L"\r\n" << std::wstring(70, L'=') << L"\r\n" << FormatFunctionDetails(*all[i]);
    return ss.str();
}

static bool WriteUtf8File(const std::wstring& path, const std::wstring& text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), NULL, 0, NULL, NULL);
    std::vector<char> buf(len > 0 ? len : 1);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &buf[0], len, NULL, NULL);

    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };   // lets Notepad detect UTF-8
    DWORD written = 0;
    bool ok = WriteFile(h, bom, 3, &written, NULL) != 0 &&
              WriteFile(h, &buf[0], (DWORD)len, &written, NULL) != 0;
    CloseHandle(h);
    return ok;
}


// =====================================================================
// 8. GUI (Win32 API, Unicode): TreeView + details panel + buttons
// =====================================================================

enum {
    ID_TREE = 100, ID_DETAILS = 101,
    ID_BTN_SCAN = 200, ID_BTN_EXPAND = 201, ID_BTN_COLLAPSE = 202, ID_BTN_SAVE = 203,
    ID_STATUS = 300
};

enum { NODE_BUS = 1, NODE_DEVICE = 2, NODE_FUNCTION = 3 };

// What a tree item stands for; the item's lParam is an index in g_items
struct TreeItemInfo
{
    int                kind;
    const PciBusNode*  bus;
    const PciDevice*   device;
    const PciFunction* function;
};

static HWND  g_hTree        = NULL;
static HWND  g_hDetails     = NULL;
static HWND  g_hStatus      = NULL;
static HWND  g_hBtnScan     = NULL;
static HWND  g_hBtnExpand   = NULL;
static HWND  g_hBtnCollapse = NULL;
static HWND  g_hBtnSave     = NULL;
static HFONT g_hFontUi      = NULL;
static HFONT g_hFontMono    = NULL;

static std::vector<TreeItemInfo> g_items;

static HTREEITEM InsertTreeItem(HTREEITEM parent, const std::wstring& text, int kind,
                                const PciBusNode* b, const PciDevice* d, const PciFunction* f)
{
    TreeItemInfo info;
    info.kind = kind; info.bus = b; info.device = d; info.function = f;
    g_items.push_back(info);

    TVINSERTSTRUCTW tv;
    ZeroMemory(&tv, sizeof(tv));
    tv.hParent = parent;
    tv.hInsertAfter = TVI_LAST;
    tv.item.mask = TVIF_TEXT | TVIF_PARAM;
    tv.item.pszText = const_cast<wchar_t*>(text.c_str());
    tv.item.lParam = (LPARAM)(g_items.size() - 1);
    return (HTREEITEM)SendMessageW(g_hTree, TVM_INSERTITEMW, 0, (LPARAM)&tv);
}

// Bus -> Device -> Function; a bridge function gets its secondary bus as a child
static void AddBusToTree(HTREEITEM parent, const PciBusNode* b)
{
    HTREEITEM hBus = InsertTreeItem(parent, BusLabel(b), NODE_BUS, b, NULL, NULL);

    for (size_t i = 0; i < b->devices.size(); ++i) {
        const PciDevice& d = b->devices[i];
        HTREEITEM hDev = InsertTreeItem(hBus, DeviceLabel(d), NODE_DEVICE, b, &d, NULL);

        for (size_t j = 0; j < d.functions.size(); ++j) {
            const PciFunction& f = d.functions[j];
            HTREEITEM hFn = InsertTreeItem(hDev, FunctionLabel(f), NODE_FUNCTION, b, &d, &f);
            if (f.isBridge) {
                const PciBusNode* child = FindChildBus(b, f);
                if (child) AddBusToTree(hFn, child);
            }
        }
    }
}

static void ExpandBranch(HTREEITEM item, UINT code)
{
    while (item) {
        HTREEITEM child = TreeView_GetChild(g_hTree, item);
        if (child) {
            ExpandBranch(child, code);
            TreeView_Expand(g_hTree, item, code);
        }
        item = TreeView_GetNextSibling(g_hTree, item);
    }
}

static void ShowNodeDetails(LPARAM index)
{
    if (index < 0 || (size_t)index >= g_items.size()) {
        SetWindowTextW(g_hDetails, S_HINT.c_str());
        return;
    }
    const TreeItemInfo& info = g_items[(size_t)index];
    std::wstring text;
    if (info.kind == NODE_BUS)           text = FormatBusDetails(info.bus);
    else if (info.kind == NODE_DEVICE)   text = FormatDeviceDetails(info.bus, *info.device);
    else                                 text = FormatFunctionDetails(*info.function);
    SetWindowTextW(g_hDetails, text.c_str());
}

static void DoScan(HWND hwnd)
{
    if (!g_portsReady) {
        MessageBoxW(hwnd, S_ERR_NOPORTS.c_str(), S_CAP_SCAN.c_str(), MB_ICONERROR);
        return;
    }

    HCURSOR oldCursor = SetCursor(LoadCursor(NULL, IDC_WAIT));

    TreeView_DeleteAllItems(g_hTree);
    g_items.clear();
    SetWindowTextW(g_hDetails, S_HINT.c_str());

    ScanAllBuses();

    SendMessageW(g_hTree, WM_SETREDRAW, FALSE, 0);
    for (size_t i = 0; i < g_roots.size(); ++i)
        AddBusToTree(TVI_ROOT, g_roots[i]);
    ExpandBranch(TreeView_GetRoot(g_hTree), TVE_EXPAND);
    SendMessageW(g_hTree, WM_SETREDRAW, TRUE, 0);

    HTREEITEM first = TreeView_GetRoot(g_hTree);
    if (first) {
        TreeView_SelectItem(g_hTree, first);
        TreeView_EnsureVisible(g_hTree, first);
    }
    InvalidateRect(g_hTree, NULL, TRUE);
    SetCursor(oldCursor);

    int buses = 0, devices = 0, functions = 0;
    for (size_t i = 0; i < g_roots.size(); ++i)
        CountAll(g_roots[i], buses, devices, functions);

    std::wstringstream ss;
    ss << S_FOUND_BUSES << buses << S_FOUND_DEVICES << devices << S_FOUND_FUNCS << functions;
    SetWindowTextW(g_hStatus, ss.str().c_str());

    if (functions == 0)
        MessageBoxW(hwnd, S_ERR_NOTHING.c_str(), S_CAP_SCAN.c_str(), MB_ICONWARNING);
}

static void DoSaveReport(HWND hwnd)
{
    if (g_roots.empty()) {
        MessageBoxW(hwnd, S_ERR_SCAN_FIRST.c_str(), S_CAP_REPORT.c_str(), MB_ICONWARNING);
        return;
    }

    // GetSaveFileNameW is loaded at run time, so no -lcomdlg32 is needed
    typedef BOOL (WINAPI *PFN_GETSAVE)(LPOPENFILENAMEW);
    HMODULE hDlg = LoadLibraryA("comdlg32.dll");
    PFN_GETSAVE pfnSave = hDlg ? (PFN_GETSAVE)GetProcAddress(hDlg, "GetSaveFileNameW") : NULL;
    if (!pfnSave) return;

    wchar_t path[MAX_PATH] = L"pci_report.txt";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = S_SAVE_FILTER;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"txt";
    if (!pfnSave(&ofn))
        return;

    if (WriteUtf8File(path, BuildReport()))
        MessageBoxW(hwnd, (S_REPORT_SAVED + path).c_str(), S_CAP_REPORT.c_str(), MB_ICONINFORMATION);
    else
        MessageBoxW(hwnd, (S_ERR_WRITE + path).c_str(), S_CAP_REPORT.c_str(), MB_ICONERROR);
}

static HWND MakeButton(HWND parent, const std::wstring& text, int id)
{
    HWND h = CreateWindowW(L"BUTTON", text.c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_hFontUi, TRUE);
    return h;
}

static void InitCommonControlsDynamic()
{
    // InitCommonControlsEx is loaded dynamically from comctl32.dll,
    // so the program links without the -lcomctl32 flag.
    typedef BOOL (WINAPI *PFN_INITCC)(const INITCOMMONCONTROLSEX*);
    HMODULE hComCtl = LoadLibraryA("comctl32.dll");
    if (!hComCtl) return;
    PFN_INITCC pfnInit = (PFN_INITCC)GetProcAddress(hComCtl, "InitCommonControlsEx");
    if (pfnInit) {
        INITCOMMONCONTROLSEX icc;
        ZeroMemory(&icc, sizeof(icc));
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_TREEVIEW_CLASSES;
        pfnInit(&icc);
    }
    // comctl32.dll stays loaded for the lifetime of the program
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        InitCommonControlsDynamic();

        g_hFontUi = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Tahoma");
        g_hFontMono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Courier New");

        g_hTree = CreateWindowExW(WS_EX_CLIENTEDGE, L"SysTreeView32", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES | TVS_HASBUTTONS |
            TVS_LINESATROOT | TVS_SHOWSELALWAYS,
            0, 0, 10, 10, hwnd, (HMENU)ID_TREE, GetModuleHandleW(NULL), NULL);
        SendMessageW(g_hTree, WM_SETFONT, (WPARAM)g_hFontUi, TRUE);

        g_hDetails = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", S_HINT.c_str(),
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
            ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
            0, 0, 10, 10, hwnd, (HMENU)ID_DETAILS, GetModuleHandleW(NULL), NULL);
        SendMessageW(g_hDetails, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);
        SendMessageW(g_hDetails, EM_SETLIMITTEXT, 0x100000, 0);

        g_hBtnScan     = MakeButton(hwnd, S_BTN_SCAN,     ID_BTN_SCAN);
        g_hBtnExpand   = MakeButton(hwnd, S_BTN_EXPAND,   ID_BTN_EXPAND);
        g_hBtnCollapse = MakeButton(hwnd, S_BTN_COLLAPSE, ID_BTN_COLLAPSE);
        g_hBtnSave     = MakeButton(hwnd, S_BTN_SAVE,     ID_BTN_SAVE);

        g_hStatus = CreateWindowW(L"STATIC", S_STATUS_START.c_str(), WS_CHILD | WS_VISIBLE,
            0, 0, 10, 10, hwnd, (HMENU)ID_STATUS, GetModuleHandleW(NULL), NULL);
        SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_hFontUi, TRUE);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_BTN_SCAN:     DoScan(hwnd); break;
        case ID_BTN_EXPAND:   ExpandBranch(TreeView_GetRoot(g_hTree), TVE_EXPAND); break;
        case ID_BTN_COLLAPSE: ExpandBranch(TreeView_GetRoot(g_hTree), TVE_COLLAPSE); break;
        case ID_BTN_SAVE:     DoSaveReport(hwnd); break;
        }
        return 0;

    case WM_NOTIFY: {
        LPNMHDR hdr = (LPNMHDR)lParam;
        if (hdr->hwndFrom == g_hTree &&
            (hdr->code == TVN_SELCHANGEDW || hdr->code == TVN_SELCHANGEDA)) {
            LPNMTREEVIEWW nmtv = (LPNMTREEVIEWW)lParam;
            ShowNodeDetails(nmtv->itemNew.lParam);
        }
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lParam), h = HIWORD(lParam);
        int treeW = w * 45 / 100;
        int btnY = h - 64;
        if (g_hTree)        MoveWindow(g_hTree, 10, 10, treeW - 15, h - 84, TRUE);
        if (g_hDetails)     MoveWindow(g_hDetails, treeW + 5, 10, w - treeW - 15, h - 84, TRUE);
        if (g_hBtnScan)     MoveWindow(g_hBtnScan,      10, btnY, 130, 28, TRUE);
        if (g_hBtnExpand)   MoveWindow(g_hBtnExpand,   150, btnY, 130, 28, TRUE);
        if (g_hBtnCollapse) MoveWindow(g_hBtnCollapse, 290, btnY, 130, 28, TRUE);
        if (g_hBtnSave)     MoveWindow(g_hBtnSave,     430, btnY, 150, 28, TRUE);
        if (g_hStatus)      MoveWindow(g_hStatus, 10, h - 28, w - 20, 20, TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 640;
        mmi->ptMinTrackSize.y = 400;
        return 0;
    }
    case WM_DESTROY:
        FreeAllBuses();
        if (g_hFontUi)   DeleteObject(g_hFontUi);
        if (g_hFontMono) DeleteObject(g_hFontMono);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow)
{
    const wchar_t* clsName = L"PciTreeScannerWnd";
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = clsName;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, clsName, S_WINDOW_TITLE.c_str(),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 650,
        NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    std::string portError;
    if (!InitPortAccess(portError)) {
        MessageBoxA(hwnd, portError.c_str(), "Port access", MB_ICONERROR);
        SetWindowTextW(g_hStatus, S_STATUS_NOPORTS.c_str());
    } else {
        SetWindowTextW(g_hStatus, S_STATUS_READY.c_str());
        DoScan(hwnd);   // scan right away; "Scan" button re-scans
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
