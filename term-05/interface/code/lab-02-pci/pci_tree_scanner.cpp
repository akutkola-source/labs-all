// pci_tree_scanner.cpp
// Lab work #2 - Option A4 (Windows)
// Tree view of PCI bus topology. Everything in a single file.
//
// This version is written in plain C++98 / ANSI Win32 on purpose:
// no constexpr, no auto, no nullptr, no range-based for, no shared_ptr,
// no in-class member initializers, no wWinMain/wide strings. This means
// it needs NO special compiler flags at all - it builds with plain GCC
// defaults, which avoids all the "my -std=c++11 / -municode flag keeps
// disappearing in the IDE" problems.
//
// BUILD (MinGW/TDM-GCC, e.g. in Dev-C++, command line, or Project Options):
//        g++ pci_tree_scanner.cpp -o pci_scanner.exe -mwindows
//        (no -std=..., no -municode and no -lcomctl32 needed)
//        Project type must be "Windows Application" (not Console Application).
//
// BUILD (MSVC / Visual Studio): empty Win32 project (NOT console),
//        Platform = x86 (Win32), Linker -> System -> SubSystem = Windows,
//        (comctl32 is loaded at run time, no extra library needed).
//
// RUN: 32-bit Windows (e.g. Windows XP in VirtualBox), as Administrator,
//      with inpout32.dll (32-bit) placed next to the .exe.
//      The .exe must be built as 32-bit to be able to load the 32-bit DLL.

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <map>
#include <fstream>


// =====================================================================
// 1. Low-level access to the PCI configuration space
//    (Configuration Mechanism #1, ports 0x0CF8 / 0x0CFC).
//    Plain unsigned char/short/int are used instead of uint8_t/uint16_t/
//    uint32_t so the file does not depend on <stdint.h>/<cstdint> at all.
// =====================================================================

static const unsigned short PCI_CONFIG_ADDRESS = 0x0CF8;
static const unsigned short PCI_CONFIG_DATA    = 0x0CFC;

// ---------------------------------------------------------------------
// Port access through the InpOut32 kernel driver.
//
// On Windows NT/2000/XP the IN/OUT instructions are forbidden in
// user mode (ring 3) - executing them directly raises an exception,
// even for an Administrator. InpOut32 installs a tiny kernel driver
// (hwinterface.sys) which performs IN/OUT on our behalf.
//
// inpout32.dll is loaded dynamically (LoadLibrary/GetProcAddress),
// so no import library and no extra linker flags are required.
// Just put inpout32.dll (32-bit, Win32 folder of the InpOut32 package)
// next to the .exe and run the program as Administrator at least once
// (the first run installs the driver).
// ---------------------------------------------------------------------

typedef void  (__stdcall *PFN_WRITE_ULONG)(ULONG port, ULONG value);
typedef ULONG (__stdcall *PFN_READ_ULONG)(ULONG port);
typedef BOOL  (__stdcall *PFN_IS_DRIVER_OPEN)(void);

static HMODULE          g_hInpOut          = NULL;
static PFN_WRITE_ULONG  g_pfnWritePortUlong = NULL;
static PFN_READ_ULONG   g_pfnReadPortUlong  = NULL;
static bool             g_portsReady        = false;

// Loads inpout32.dll and checks that its kernel driver is running.
// On failure writes a human-readable reason into 'error'.
static bool InitPortAccess(std::string& error)
{
    g_hInpOut = LoadLibraryA("inpout32.dll");
    if (!g_hInpOut) {
        error = "inpout32.dll was not found.\r\n"
                "Copy the 32-bit inpout32.dll into the same folder as this .exe.";
        return false;
    }

    PFN_IS_DRIVER_OPEN pfnIsOpen =
        (PFN_IS_DRIVER_OPEN)GetProcAddress(g_hInpOut, "IsInpOutDriverOpen");
    g_pfnWritePortUlong = (PFN_WRITE_ULONG)GetProcAddress(g_hInpOut, "DlPortWritePortUlong");
    g_pfnReadPortUlong  = (PFN_READ_ULONG) GetProcAddress(g_hInpOut, "DlPortReadPortUlong");

    if (!pfnIsOpen || !g_pfnWritePortUlong || !g_pfnReadPortUlong) {
        error = "inpout32.dll is too old or not the right one.\r\n"
                "Use InpOut32 version 1.5 (Highrez), Win32 folder.";
        return false;
    }

    if (!pfnIsOpen()) {
        error = "InpOut32 kernel driver could not be opened.\r\n"
                "Run this program as Administrator (the first run installs the driver).";
        return false;
    }

    g_portsReady = true;
    return true;
}

static void ShutdownPortAccess()
{
    if (g_hInpOut) { FreeLibrary(g_hInpOut); g_hInpOut = NULL; }
    g_portsReady = false;
}

inline void OutPortD(unsigned short port, unsigned int value)
{
    g_pfnWritePortUlong(port, value);
}

inline unsigned int InPortD(unsigned short port)
{
    return g_pfnReadPortUlong(port);
}

// [31]=Enable, [30:24]=0, [23:16]=Bus, [15:11]=Device, [10:8]=Function,
// [7:2]=Register(>>2), [1:0]=00
inline unsigned int PciMakeAddress(unsigned char bus, unsigned char device, unsigned char function, unsigned char reg)
{
    return 0x80000000u
        | ((unsigned int)bus << 16)
        | ((unsigned int)(device & 0x1F) << 11)
        | ((unsigned int)(function & 0x07) << 8)
        | ((unsigned int)reg & 0xFC);
}

inline unsigned int PciReadDWord(unsigned char bus, unsigned char device, unsigned char function, unsigned char reg)
{
    OutPortD(PCI_CONFIG_ADDRESS, PciMakeAddress(bus, device, function, reg));
    return InPortD(PCI_CONFIG_DATA);
}

inline unsigned short PciReadWord(unsigned char bus, unsigned char device, unsigned char function, unsigned char reg)
{
    unsigned int dw = PciReadDWord(bus, device, function, reg & 0xFC);
    return (unsigned short)((dw >> ((reg & 2) * 8)) & 0xFFFF);
}

inline unsigned char PciReadByte(unsigned char bus, unsigned char device, unsigned char function, unsigned char reg)
{
    unsigned int dw = PciReadDWord(bus, device, function, reg & 0xFC);
    return (unsigned char)((dw >> ((reg & 3) * 8)) & 0xFF);
}

// =====================================================================
// 2. Data structures: one device function and one bus-tree node.
//    Fields are initialized in a constructor (not via in-class
//    initializers, which are a C++11 feature).
// =====================================================================

struct PciFunction
{
    unsigned char  bus, device, function;

    unsigned short vendorId, deviceId;
    unsigned short command, status;
    unsigned char  revisionId, progIf, subclass, classCode;
    unsigned char  cacheLineSize, latencyTimer, headerType, bist;

    // Type 0 (normal device)
    unsigned short subsystemVendorId, subsystemId;
    unsigned char  interruptLine, interruptPin;

    // Type 1 (PCI-to-PCI bridge)
    bool          isBridge;
    unsigned char primaryBus, secondaryBus, subordinateBus;

    bool isMultiFunction; // Header Type bit 7

    PciFunction()
    {
        bus = device = function = 0;
        vendorId = 0xFFFF; deviceId = 0xFFFF;
        command = 0; status = 0;
        revisionId = progIf = subclass = classCode = 0;
        cacheLineSize = latencyTimer = headerType = bist = 0;
        subsystemVendorId = 0; subsystemId = 0;
        interruptLine = interruptPin = 0;
        isBridge = false;
        primaryBus = secondaryBus = subordinateBus = 0;
        isMultiFunction = false;
    }

    std::string Bdf() const
    {
        std::stringstream ss;
        ss << std::hex << std::uppercase << std::setfill('0')
           << std::setw(2) << (int)bus << ":"
           << std::setw(2) << (int)device << ".";
        ss << std::dec << (int)function;
        return ss.str();
    }

    std::string VendorDeviceHex() const
    {
        std::stringstream ss;
        ss << std::hex << std::uppercase << std::setfill('0')
           << std::setw(4) << vendorId << ":" << std::setw(4) << deviceId;
        return ss.str();
    }

    // Table of common Class/Subclass codes (see lab manual, section 3.7;
    // this list is not exhaustive).
    std::string ClassSubclassName() const
    {
        switch (classCode) {
        case 0x00: return "Unclassified device";
        case 0x01:
            switch (subclass) {
            case 0x00: return "SCSI controller";
            case 0x01: return "IDE controller";
            case 0x06: return "SATA controller";
            case 0x08: return "NVMe controller";
            default:   return "Mass storage controller";
            }
        case 0x02:
            return subclass == 0x00 ? "Ethernet controller" : "Network controller";
        case 0x03:
            switch (subclass) {
            case 0x00: return "VGA-compatible controller";
            case 0x02: return "3D controller";
            default:   return "Display controller";
            }
        case 0x06:
            switch (subclass) {
            case 0x00: return "Host bridge";
            case 0x01: return "ISA bridge";
            case 0x04: return "PCI-to-PCI bridge";
            default:   return "Bridge device";
            }
        case 0x0C:
            return subclass == 0x03 ? "USB controller" : "Serial bus controller";
        default: {
            std::stringstream ss;
            ss << "Class " << std::hex << std::uppercase << (int)classCode << "h";
            return ss.str();
        }
        }
    }
};

struct PciBusNode
{
    unsigned char busNumber;
    std::vector<PciFunction>  functions;   // all functions on this bus
    std::vector<PciBusNode*>  childBuses;  // subordinate buses (behind bridges)

    PciBusNode() : busNumber(0) {}
};

static void FreeBusNode(PciBusNode* node)
{
    if (!node) return;
    for (size_t i = 0; i < node->childBuses.size(); ++i)
        FreeBusNode(node->childBuses[i]);
    delete node;
}

// =====================================================================
// 3. Recursive PCI enumeration (Bus -> Device -> Function, bridges)
// =====================================================================

static bool ReadFunction(unsigned char bus, unsigned char device, unsigned char function, PciFunction& f)
{
    unsigned short vendor = PciReadWord(bus, device, function, 0x00);
    if (vendor == 0xFFFF)
        return false; // device/function does not exist

    f.bus = bus; f.device = device; f.function = function;
    f.vendorId = vendor;
    f.deviceId = PciReadWord(bus, device, function, 0x02);
    f.command  = PciReadWord(bus, device, function, 0x04);
    f.status   = PciReadWord(bus, device, function, 0x06);
    f.revisionId = PciReadByte(bus, device, function, 0x08);
    f.progIf     = PciReadByte(bus, device, function, 0x09);
    f.subclass   = PciReadByte(bus, device, function, 0x0A);
    f.classCode  = PciReadByte(bus, device, function, 0x0B);
    f.cacheLineSize = PciReadByte(bus, device, function, 0x0C);
    f.latencyTimer  = PciReadByte(bus, device, function, 0x0D);
    f.headerType    = PciReadByte(bus, device, function, 0x0E);
    f.bist          = PciReadByte(bus, device, function, 0x0F);
    f.isMultiFunction = (f.headerType & 0x80) != 0;

    unsigned char type = f.headerType & 0x7F;
    if (type == 0x01) {
        // Type 1: PCI-to-PCI bridge - Secondary Bus Number (offset 0x19)
        f.isBridge = true;
        unsigned int busNums = PciReadDWord(bus, device, function, 0x18);
        f.primaryBus     = (unsigned char)(busNums & 0xFF);
        f.secondaryBus   = (unsigned char)((busNums >> 8) & 0xFF);
        f.subordinateBus = (unsigned char)((busNums >> 16) & 0xFF);
    } else if (type == 0x00) {
        f.subsystemVendorId = PciReadWord(bus, device, function, 0x2C);
        f.subsystemId       = PciReadWord(bus, device, function, 0x2E);
        f.interruptLine = PciReadByte(bus, device, function, 0x3C);
        f.interruptPin  = PciReadByte(bus, device, function, 0x3D);
    }
    return true;
}

// depth/maxDepth guard against infinite recursion on malformed topology
static PciBusNode* ScanBus(unsigned char bus, int depth = 0, int maxDepth = 8)
{
    PciBusNode* node = new PciBusNode();
    node->busNumber = bus;
    if (depth > maxDepth)
        return node;

    for (int device = 0; device < 32; ++device) {
        PciFunction f0;
        if (!ReadFunction(bus, (unsigned char)device, 0, f0))
            continue; // function 0 missing -> no device at all

        std::vector<PciFunction> funcs;
        funcs.push_back(f0);

        int maxFunc = f0.isMultiFunction ? 8 : 1;
        for (int fn = 1; fn < maxFunc; ++fn) {
            PciFunction f;
            if (ReadFunction(bus, (unsigned char)device, (unsigned char)fn, f))
                funcs.push_back(f);
        }

        for (size_t i = 0; i < funcs.size(); ++i) {
            node->functions.push_back(funcs[i]);
            if (funcs[i].isBridge && funcs[i].secondaryBus > bus) {
                PciBusNode* child = ScanBus(funcs[i].secondaryBus, depth + 1, maxDepth);
                node->childBuses.push_back(child);
            }
        }
    }
    return node;
}

static int CountDevices(PciBusNode* node)
{
    if (!node) return 0;
    int count = (int)node->functions.size();
    for (size_t i = 0; i < node->childBuses.size(); ++i)
        count += CountDevices(node->childBuses[i]);
    return count;
}

// =====================================================================
// 4. GUI (plain ANSI Win32 API): TreeView + details panel + buttons
// =====================================================================

static HWND g_hTree       = NULL;
static HWND g_hDetails    = NULL;
static HWND g_hStatus     = NULL;
static HWND g_hBtnScan    = NULL;
static HWND g_hBtnSave    = NULL;

static PciBusNode* g_root = NULL;
static std::map<HTREEITEM, PciFunction> g_itemToFunc;

static std::string ToStr(int v)
{
    std::stringstream ss;
    ss << v;
    return ss.str();
}

static std::string Hex(unsigned int v, int width)
{
    std::stringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << v;
    return ss.str();
}

static std::string FormatDetails(const PciFunction& f)
{
    std::stringstream ss;
    ss << "BDF:              " << f.Bdf() << "\r\n";
    ss << "Vendor ID:        0x" << Hex(f.vendorId, 4) << "\r\n";
    ss << "Device ID:        0x" << Hex(f.deviceId, 4) << "\r\n";
    ss << "Revision ID:      0x" << Hex(f.revisionId, 2) << "\r\n";
    ss << "Header Type:      0x" << Hex(f.headerType, 2)
       << (f.isMultiFunction ? "  (multi-function)" : "") << "\r\n";
    ss << "Class Code:       0x" << Hex(f.classCode, 2) << "\r\n";
    ss << "Subclass:         0x" << Hex(f.subclass, 2) << "\r\n";
    ss << "Prog IF:          0x" << Hex(f.progIf, 2) << "\r\n";
    ss << "Class name:       " << f.ClassSubclassName() << "\r\n";
    ss << "Command:          0x" << Hex(f.command, 4) << "\r\n";
    ss << "Status:           0x" << Hex(f.status, 4) << "\r\n";

    if (f.isBridge) {
        ss << "--- PCI-to-PCI Bridge ---\r\n";
        ss << "Primary Bus:      " << (int)f.primaryBus << "\r\n";
        ss << "Secondary Bus:    " << (int)f.secondaryBus << "\r\n";
        ss << "Subordinate Bus:  " << (int)f.subordinateBus << "\r\n";
    } else {
        ss << "Subsystem Vendor: 0x" << Hex(f.subsystemVendorId, 4) << "\r\n";
        ss << "Subsystem ID:     0x" << Hex(f.subsystemId, 4) << "\r\n";
        ss << "Interrupt Line:   " << (int)f.interruptLine << "\r\n";
        ss << "Interrupt PIN:    " << (int)f.interruptPin << "\r\n";
    }
    ss << "Cache Line Size:  " << (int)f.cacheLineSize << "\r\n";
    ss << "Latency Timer:    " << (int)f.latencyTimer << "\r\n";
    return ss.str();
}

static void AddBusToTree(HTREEITEM parentItem, PciBusNode* busNode)
{
    std::string busLabel = "Bus " + ToStr((int)busNode->busNumber);

    TVINSERTSTRUCT tvBus;
    ZeroMemory(&tvBus, sizeof(tvBus));
    tvBus.hParent = parentItem;
    tvBus.hInsertAfter = TVI_LAST;
    tvBus.item.mask = TVIF_TEXT;
    tvBus.item.pszText = const_cast<char*>(busLabel.c_str());
    HTREEITEM hBusItem = TreeView_InsertItem(g_hTree, &tvBus);

    for (size_t i = 0; i < busNode->functions.size(); ++i) {
        PciFunction& f = busNode->functions[i];
        std::string label = "Device " + ToStr((int)f.device) +
                             " / Function " + ToStr((int)f.function) +
                             "  [" + f.VendorDeviceHex() + "]  " +
                             f.ClassSubclassName();

        TVINSERTSTRUCT tvFn;
        ZeroMemory(&tvFn, sizeof(tvFn));
        tvFn.hParent = hBusItem;
        tvFn.hInsertAfter = TVI_LAST;
        tvFn.item.mask = TVIF_TEXT;
        tvFn.item.pszText = const_cast<char*>(label.c_str());
        HTREEITEM hFnItem = TreeView_InsertItem(g_hTree, &tvFn);
        g_itemToFunc[hFnItem] = f; // store a copy - safe even if the vector reallocates later

        if (f.isBridge) {
            for (size_t j = 0; j < busNode->childBuses.size(); ++j) {
                if (busNode->childBuses[j]->busNumber == f.secondaryBus) {
                    AddBusToTree(hFnItem, busNode->childBuses[j]);
                }
            }
        }
    }

    TreeView_Expand(g_hTree, hBusItem, TVE_EXPAND);
}

static void WriteReport(std::ofstream& out, PciBusNode* node, int indent)
{
    std::string pad(indent * 2, ' ');
    out << pad << "Bus " << (int)node->busNumber << "\r\n";
    for (size_t i = 0; i < node->functions.size(); ++i) {
        PciFunction& f = node->functions[i];
        out << pad << "  " << f.Bdf() << "  " << f.VendorDeviceHex()
            << "  " << f.ClassSubclassName() << "\r\n";
    }
    for (size_t i = 0; i < node->childBuses.size(); ++i)
        WriteReport(out, node->childBuses[i], indent + 1);
}

static void UpdateStatus(int count)
{
    std::string s = "Functions found: " + ToStr(count);
    SetWindowText(g_hStatus, s.c_str());
}

static void DoScan(HWND hwnd)
{
    if (!g_portsReady) {
        MessageBox(hwnd, "Port access is not available (InpOut32 driver not loaded).",
                   "Scan", MB_ICONERROR);
        return;
    }

    TreeView_DeleteAllItems(g_hTree);
    g_itemToFunc.clear();
    SetWindowText(g_hDetails, "");

    if (g_root) { FreeBusNode(g_root); g_root = NULL; }
    g_root = ScanBus(0);
    AddBusToTree(TVI_ROOT, g_root);
    UpdateStatus(CountDevices(g_root));
}

static void DoSaveReport(HWND hwnd)
{
    if (!g_root) {
        MessageBox(hwnd, "Please run a scan first.", "Report", MB_ICONWARNING);
        return;
    }

    char path[MAX_PATH] = "pci_report.txt";
    OPENFILENAME ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "Text files (*.txt)\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = "txt";
    if (!GetSaveFileName(&ofn))
        return;

    std::ofstream out(path);
    out << "PCI system configuration report\r\n";
    out << "================================\r\n";
    WriteReport(out, g_root, 0);
    out.close();

    MessageBox(hwnd, "Report saved.", "Done", MB_ICONINFORMATION);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        // InitCommonControlsEx is loaded dynamically from comctl32.dll,
        // so the program links without the -lcomctl32 flag.
        // (TreeView_* are SendMessage macros and need no library.)
        {
            typedef BOOL (WINAPI *PFN_INITCC)(const INITCOMMONCONTROLSEX*);
            HMODULE hComCtl = LoadLibraryA("comctl32.dll");
            if (hComCtl) {
                PFN_INITCC pfnInit =
                    (PFN_INITCC)GetProcAddress(hComCtl, "InitCommonControlsEx");
                if (pfnInit) {
                    INITCOMMONCONTROLSEX icc;
                    ZeroMemory(&icc, sizeof(icc));
                    icc.dwSize = sizeof(icc);
                    icc.dwICC = ICC_TREEVIEW_CLASSES;
                    pfnInit(&icc);
                }
                // comctl32.dll stays loaded for the lifetime of the program
            }
        }

        g_hTree = CreateWindowEx(WS_EX_CLIENTEDGE, WC_TREEVIEW, "",
            WS_CHILD | WS_VISIBLE | WS_BORDER | TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT,
            10, 10, 420, 400, hwnd, (HMENU)100, GetModuleHandle(NULL), NULL);

        g_hDetails = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            440, 10, 340, 400, hwnd, (HMENU)101, GetModuleHandle(NULL), NULL);
        HFONT hFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, FIXED_PITCH, "Consolas");
        SendMessage(g_hDetails, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hBtnScan = CreateWindow("BUTTON", "Scan System",
            WS_CHILD | WS_VISIBLE, 10, 420, 180, 30, hwnd, (HMENU)200, GetModuleHandle(NULL), NULL);
        g_hBtnSave = CreateWindow("BUTTON", "Save Report",
            WS_CHILD | WS_VISIBLE, 200, 420, 180, 30, hwnd, (HMENU)201, GetModuleHandle(NULL), NULL);

        g_hStatus = CreateWindow("STATIC", "Functions found: 0",
            WS_CHILD | WS_VISIBLE, 10, 460, 500, 20, hwnd, (HMENU)300, GetModuleHandle(NULL), NULL);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 200) DoScan(hwnd);
        if (LOWORD(wParam) == 201) DoSaveReport(hwnd);
        return 0;

    case WM_NOTIFY: {
        LPNMHDR hdr = (LPNMHDR)lParam;
        if (hdr->hwndFrom == g_hTree && hdr->code == TVN_SELCHANGED) {
            LPNMTREEVIEW nmtv = (LPNMTREEVIEW)lParam;
            std::map<HTREEITEM, PciFunction>::iterator it = g_itemToFunc.find(nmtv->itemNew.hItem);
            if (it != g_itemToFunc.end()) {
                std::string text = FormatDetails(it->second);
                SetWindowText(g_hDetails, text.c_str());
            } else {
                SetWindowText(g_hDetails, "(bus node - select a device function)");
            }
        }
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lParam), h = HIWORD(lParam);
        if (g_hTree)    MoveWindow(g_hTree, 10, 10, w / 2 - 20, h - 90, TRUE);
        if (g_hDetails) MoveWindow(g_hDetails, w / 2 + 10, 10, w / 2 - 20, h - 90, TRUE);
        if (g_hBtnScan) MoveWindow(g_hBtnScan, 10, h - 70, 180, 30, TRUE);
        if (g_hBtnSave) MoveWindow(g_hBtnSave, 200, h - 70, 180, 30, TRUE);
        if (g_hStatus)  MoveWindow(g_hStatus, 10, h - 30, w - 20, 20, TRUE);
        return 0;
    }
    case WM_DESTROY:
        if (g_root) { FreeBusNode(g_root); g_root = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow)
{
    const char* clsName = "PciTreeScannerWnd";
    WNDCLASS wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = clsName;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, clsName, "Lab #2 - PCI Bus Topology (Option A4)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 820, 560,
        NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    std::string portError;
    if (!InitPortAccess(portError)) {
        MessageBox(hwnd, portError.c_str(), "InpOut32", MB_ICONERROR);
        SetWindowText(g_hStatus, "Port access NOT available - see error message");
    } else {
        SetWindowText(g_hStatus, "Driver OK. Press 'Scan System'.");
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    ShutdownPortAccess();
    return 0;
}
