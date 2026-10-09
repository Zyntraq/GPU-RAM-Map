#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

struct Adapter { uint64_t id; std::wstring name; };
static std::vector<Adapter> adapters;
static uint64_t selectedGpu = 0;
static int sortColumn = 2;
static bool sortAscending = false;
static std::wstring adapterIdText(uint64_t id) {
    wchar_t text[24]; swprintf(text, 24, L"%016llx", static_cast<unsigned long long>(id)); return text;
}
static void enumerateAdapters() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (FAILED(factory->EnumAdapters1(i, &adapter))) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            uint64_t id = (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart;
            adapters.push_back({id, desc.Description});
        }
        adapter->Release();
    }
    factory->Release();
}
static bool parseAdapter(const wchar_t* instance, uint64_t& id) {
    const wchar_t* part = instance ? wcsstr(instance, L"_luid_") : nullptr;
    if (!part) return false;
    unsigned long high = 0, low = 0;
    if (swscanf(part, L"_luid_0x%lx_0x%lx", &high, &low) != 2) return false;
    id = (static_cast<uint64_t>(high) << 32) | low; return true;
}
struct Row { DWORD pid = 0; std::wstring name; uint64_t localBytes = 0, nonLocalBytes = 0, adapterId = 0; };
struct Snapshot { std::vector<Row> rows; std::wstring error; unsigned skipped = 0; };

static std::wstring statusText(PDH_STATUS status) {
    wchar_t code[32]; swprintf(code, 32, L"0x%08lX", static_cast<unsigned long>(status));
    return code;
}
static DWORD parsePid(const wchar_t* instance) {
    if (!instance || wcsncmp(instance, L"pid_", 4)) return 0;
    wchar_t* end = nullptr;
    unsigned long value = wcstoul(instance + 4, &end, 10);
    return end != instance + 4 && *end == L'_' ? static_cast<DWORD>(value) : 0;
}
static bool rowLess(const Row& a, const Row& b) {
    int comparison = 0;
    switch (sortColumn) {
    case 0: comparison = _wcsicmp(a.name.c_str(), b.name.c_str()); break;
    case 1: comparison = (a.pid > b.pid) - (a.pid < b.pid); break;
    case 2: comparison = (a.localBytes > b.localBytes) - (a.localBytes < b.localBytes); break;
    case 3: comparison = (a.nonLocalBytes > b.nonLocalBytes) - (a.nonLocalBytes < b.nonLocalBytes); break;
    }
    if (comparison) return sortAscending ? comparison < 0 : comparison > 0;
    // A stable tie breaker stops equal-valued rows jumping between refreshes.
    return a.pid < b.pid;
}
static Snapshot forGpu(const Snapshot& raw, uint64_t id) {
    Snapshot filtered; filtered.error = raw.error; filtered.skipped = raw.skipped;
    for (const auto& row : raw.rows) if (row.adapterId == id) filtered.rows.push_back(row);
    std::sort(filtered.rows.begin(), filtered.rows.end(), rowLess);
    return filtered;
}
class GpuCounters {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER localBytes = nullptr, nonLocalBytes = nullptr;
    void check(PDH_STATUS s, const wchar_t* operation) {
        if (s != ERROR_SUCCESS) throw std::wstring(operation) + L" failed (" + statusText(s) + L"). GPU counters require Windows 10/11 and a compatible WDDM driver.";
    }
    void read(PDH_HCOUNTER counter, bool isLocal, std::map<std::pair<uint64_t, DWORD>, Row>& rows, unsigned& skipped) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            DWORD bytes = 0, count = 0;
            PDH_STATUS s = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &bytes, &count, nullptr);
            if (s == static_cast<PDH_STATUS>(PDH_NO_DATA) || (s == ERROR_SUCCESS && bytes == 0)) return;
            if (s != static_cast<PDH_STATUS>(PDH_MORE_DATA)) check(s, L"Reading GPU memory");
            std::vector<unsigned char> buffer(bytes);
            auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
            s = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &bytes, &count, items);
            if (s == static_cast<PDH_STATUS>(PDH_MORE_DATA)) continue; // Re-query the size if instances changed.
            if (s == static_cast<PDH_STATUS>(PDH_NO_DATA)) return;
            check(s, L"Reading GPU memory");
            for (DWORD i = 0; i < count; ++i) {
                auto& value = items[i].FmtValue;
                if ((value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) || value.largeValue < 0) { ++skipped; continue; }
                DWORD pid = parsePid(items[i].szName);
                if (!pid) continue;
                uint64_t adapterId = 0;
                if (!parseAdapter(items[i].szName, adapterId)) { ++skipped; continue; }
                auto& row = rows[{adapterId, pid}]; row.pid = pid; row.adapterId = adapterId;
                // Sum physical nodes only within the same adapter and PID.
                (isLocal ? row.localBytes : row.nonLocalBytes) += static_cast<uint64_t>(value.largeValue);
            }
            return;
        }
        throw std::wstring(L"GPU counter instances changed repeatedly; retrying on the next refresh.");
    }
public:
    ~GpuCounters() { if (query) PdhCloseQuery(query); }
    Snapshot sample() {
        Snapshot result;
        try {
            if (!query) {
                check(PdhOpenQueryW(nullptr, 0, &query), L"Opening GPU counters");
                check(PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Local Usage", 0, &localBytes), L"Adding local GPU counter");
                check(PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Non Local Usage", 0, &nonLocalBytes), L"Adding non-local GPU counter");
            }
            PDH_STATUS s = PdhCollectQueryData(query);
            if (s == static_cast<PDH_STATUS>(PDH_NO_DATA)) return result;
            check(s, L"Sampling GPU memory");
            std::map<std::pair<uint64_t, DWORD>, Row> rows;
            read(localBytes, true, rows, result.skipped);
            read(nonLocalBytes, false, rows, result.skipped);
            HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            std::map<DWORD, std::wstring> names;
            if (processes != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
                if (Process32FirstW(processes, &entry)) do {
                    names[entry.th32ProcessID] = entry.szExeFile;
                } while (Process32NextW(processes, &entry));
                CloseHandle(processes);
            }
            for (auto& pair : rows) {
                auto& row = pair.second;
                if (!row.localBytes && !row.nonLocalBytes) continue;
                row.name = names[row.pid];
                if (row.name.empty()) row.name = L"Unavailable / exited process";
                result.rows.push_back(std::move(row));
            }
            std::sort(result.rows.begin(), result.rows.end(), [](const Row& a, const Row& b) {
                if (a.localBytes != b.localBytes) return a.localBytes > b.localBytes;
                if (a.nonLocalBytes != b.nonLocalBytes) return a.nonLocalBytes > b.nonLocalBytes;
                return a.pid < b.pid;
            });
        } catch (const std::wstring& e) {
            result.error = e;
            if (query) PdhCloseQuery(query);
            query = nullptr; localBytes = nonLocalBytes = nullptr;
        } catch (...) { result.error = L"Unable to read GPU memory counters."; }
        return result;
    }
};

static std::string utf8(const std::wstring& value) {
    int count = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string text(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), text.data(), count, nullptr, nullptr);
    return text;
}
static bool writeCsv(const std::wstring& path, const Snapshot& snapshot) {
    std::ofstream file{std::filesystem::path(path)};
    if (!file) return false;
    if (!snapshot.error.empty()) { file << "Error: " << utf8(snapshot.error) << '\n'; return false; }
    file << "Process,PID,LocalBytes,NonLocalBytes,AdapterId\n";
    for (const auto& row : snapshot.rows) {
        std::string name = utf8(row.name), escaped;
        for (char c : name) { escaped += c; if (c == '"') escaped += '"'; }
        file << '"' << escaped << "\"," << row.pid << ',' << row.localBytes << ',' << row.nonLocalBytes << ',' << utf8(adapterIdText(row.adapterId)) << '\n';
    }
    return static_cast<bool>(file);
}
static std::wstring memoryKiB(uint64_t bytes) {
    std::wstring digits = std::to_wstring(bytes / 1024);
    for (int position = static_cast<int>(digits.size()) - 3; position > 0; position -= 3)
        digits.insert(position, L",");
    return digits;
}

static HWND windowHandle, table, statusLabel, intervalLabel, intervalEdit, secondsLabel, gpuLabel, gpuDropdown;
static uint64_t totalLocalBytes = 0, totalNonLocalBytes = 0;
static bool totalsAvailable = false;
static HFONT font;
static HANDLE wakeEvent;
static std::thread worker;
static std::atomic<bool> stopping{false};
static std::mutex resultMutex;
static Snapshot latest;
static Snapshot displayedRaw;
static bool pending = false;
static std::wstring testReport;
static bool testFixture = false;
static int testSamples = 0, exitResult = 0;
static constexpr UINT intervalId = 100;
static std::atomic<DWORD> refreshMilliseconds{2000};
static std::atomic<bool> intervalChanged{false};
static std::atomic<ULONGLONG> sampleSpacing{0};
static int timingStage = 0;
static constexpr UINT gpuId = 101;

// Capture this application's own window for visual verification in --ui-test mode.
static bool captureWindow(const std::wstring& path) {
    RECT r; GetClientRect(windowHandle, &r);
    HDC dc = GetDC(windowHandle), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, r.right, r.bottom);
    HGDIOBJ old = SelectObject(memory, bitmap);
    BOOL printed = PrintWindow(windowHandle, memory, PW_CLIENTONLY);
    SelectObject(memory, old);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = r.right; info.bmiHeader.biHeight = -r.bottom;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<size_t>(r.right) * r.bottom * 4);
    bool ok = printed && GetDIBits(memory, bitmap, 0, r.bottom, pixels.data(), &info, DIB_RGB_COLORS);
    if (ok) {
        // PrintWindow can succeed for a hidden window but produce no image.
        ok = std::any_of(pixels.begin(), pixels.end(), [](unsigned char byte) { return byte != 0; });
    }
    if (ok) {
        BITMAPFILEHEADER header{}; header.bfType = 0x4D42;
        header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
        std::ofstream f{std::filesystem::path(path), std::ios::binary};
        f.write(reinterpret_cast<char*>(&header), sizeof(header));
        f.write(reinterpret_cast<char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
        f.write(reinterpret_cast<char*>(pixels.data()), pixels.size()); ok = static_cast<bool>(f);
    }
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(windowHandle, dc); return ok;
}
static void layout() {
    RECT r; GetClientRect(windowHandle, &r);
    MoveWindow(gpuLabel, 16, 22, 40, 24, TRUE);
    MoveWindow(gpuDropdown, 58, 16, r.right - 74, 220, TRUE);
    MoveWindow(table, 16, 55, r.right - 32, r.bottom - 169, TRUE);
    MoveWindow(statusLabel, 16, r.bottom - 76, r.right - 32, 42, TRUE);
    MoveWindow(intervalLabel, 16, r.bottom - 29, 94, 22, TRUE);
    MoveWindow(intervalEdit, 112, r.bottom - 33, 54, 24, TRUE);
    MoveWindow(secondsLabel, 172, r.bottom - 29, 70, 22, TRUE);
    ListView_SetColumnWidth(table, 0, std::max(210L, r.right - 32 - 80 - 170 - 180 - GetSystemMetrics(SM_CXVSCROLL) - 4));
    RECT band{16, r.bottom - 108, r.right - 16, r.bottom - 82};
    InvalidateRect(windowHandle, &band, TRUE);
}
static RECT totalBand() {
    RECT r; GetClientRect(windowHandle, &r);
    return RECT{16, r.bottom - 108, r.right - 16, r.bottom - 82};
}
static void drawTotals(HDC dc) {
    RECT band = totalBand();
    FillRect(dc, &band, GetSysColorBrush(COLOR_3DFACE));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT label = band; label.left += 8;
    DrawTextW(dc, L"Total (listed processes)", -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    HWND header = ListView_GetHeader(table);
    for (int column = 2; column <= 3; ++column) {
        RECT area{};
        if (!Header_GetItemRect(header, column, &area)) continue;
        MapWindowPoints(header, windowHandle, reinterpret_cast<POINT*>(&area), 2);
        area.top = band.top; area.bottom = band.bottom; area.right -= 8;
        RECT clipped{};
        if (!IntersectRect(&clipped, &area, &band)) continue;
        std::wstring value = totalsAvailable ? memoryKiB(column == 2 ? totalLocalBytes : totalNonLocalBytes) : L"--";
        DrawTextW(dc, value.c_str(), -1, &clipped, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, oldFont);
}
static void displaySnapshot(const Snapshot& snapshot) {
    totalLocalBytes = totalNonLocalBytes = 0;
    totalsAvailable = snapshot.error.empty();
    if (totalsAvailable) for (const auto& row : snapshot.rows) {
        totalLocalBytes += row.localBytes;
        totalNonLocalBytes += row.nonLocalBytes;
    }
    RECT band = totalBand(); InvalidateRect(windowHandle, &band, FALSE);
    int selected = ListView_GetNextItem(table, -1, LVNI_SELECTED);
    DWORD selectedPid = 0;
    if (selected >= 0) { LVITEMW item{}; item.mask = LVIF_PARAM; item.iItem = selected; ListView_GetItem(table, &item); selectedPid = static_cast<DWORD>(item.lParam); }
    int firstVisible = ListView_GetTopIndex(table);
    SendMessageW(table, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(table);
    for (size_t i = 0; i < snapshot.rows.size(); ++i) {
        const Row& row = snapshot.rows[i];
        LVITEMW item{}; item.mask = LVIF_TEXT | LVIF_PARAM; item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(row.name.c_str()); item.lParam = row.pid;
        ListView_InsertItem(table, &item);
        std::wstring columns[] = {std::to_wstring(row.pid), memoryKiB(row.localBytes), memoryKiB(row.nonLocalBytes)};
        for (int column = 1; column <= 3; ++column) ListView_SetItemText(table, static_cast<int>(i), column, columns[column - 1].data());
        if (selectedPid == row.pid) ListView_SetItemState(table, static_cast<int>(i), LVIS_SELECTED, LVIS_SELECTED);
    }
    if (firstVisible > 0 && !snapshot.rows.empty()) {
        int index = std::min(firstVisible, static_cast<int>(snapshot.rows.size()) - 1);
        RECT itemRect{}; if (ListView_GetItemRect(table, 0, &itemRect, LVIR_BOUNDS)) ListView_Scroll(table, 0, index * (itemRect.bottom - itemRect.top));
    }
    SendMessageW(table, WM_SETREDRAW, TRUE, 0); InvalidateRect(table, nullptr, TRUE);
    std::wstring text;
    if (!snapshot.error.empty()) text = snapshot.error;
    else {
        SYSTEMTIME time; GetLocalTime(&time); wchar_t timestamp[24];
        swprintf(timestamp, 24, L"%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
        text = std::to_wstring(snapshot.rows.size()) + L" processes | Updated " + timestamp;
        if (snapshot.rows.empty()) text += L" | No GPU memory usage reported";
        if (snapshot.skipped) text += L" | Some counter samples unavailable";
        text += L"\r\nSelected GPU only. Local = VRAM on discrete GPUs; system RAM on integrated GPUs. Process values are not additive.";
    }
    SetWindowTextW(statusLabel, text.c_str());
}
static bool tableMatches(const Snapshot& sample) {
    if (!sample.error.empty() || !std::is_sorted(sample.rows.begin(), sample.rows.end(), rowLess) || ListView_GetItemCount(table) != static_cast<int>(sample.rows.size())) return false;
    for (size_t i = 0; i < sample.rows.size(); ++i) {
        const auto& row = sample.rows[i];
        if (row.adapterId != selectedGpu) return false;
        wchar_t text[256];
        LVITEMW item{}; item.mask = LVIF_PARAM; item.iItem = static_cast<int>(i); ListView_GetItem(table, &item);
        if (static_cast<DWORD>(item.lParam) != row.pid) return false;
        ListView_GetItemText(table, static_cast<int>(i), 0, text, 256); if (text != row.name) return false;
        ListView_GetItemText(table, static_cast<int>(i), 2, text, 256); if (text != memoryKiB(row.localBytes)) return false;
        ListView_GetItemText(table, static_cast<int>(i), 3, text, 256); if (text != memoryKiB(row.nonLocalBytes)) return false;
    }
    uint64_t local = 0, nonLocal = 0;
    for (const auto& row : sample.rows) { local += row.localBytes; nonLocal += row.nonLocalBytes; }
    return totalsAvailable && totalLocalBytes == local && totalNonLocalBytes == nonLocal;
}
static void updateSortIndicator() {
    HWND header = ListView_GetHeader(table);
    for (int column = 0; column < 4; ++column) {
        HDITEMW item{}; item.mask = HDI_FORMAT;
        if (!Header_GetItem(header, column, &item)) continue;
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (column == sortColumn) item.fmt |= sortAscending ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(header, column, &item);
    }
}
static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE: {
        windowHandle = hwnd;
        NONCLIENTMETRICSW metrics{}; metrics.cbSize = sizeof(metrics); SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        font = CreateFontIndirectW(&metrics.lfMessageFont);
        intervalLabel = CreateWindowW(L"STATIC", L"Refreshes every", WS_CHILD | WS_VISIBLE, 0,0,0,0, hwnd, nullptr, nullptr, nullptr);
        intervalEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"2", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 0,0,0,0, hwnd, reinterpret_cast<HMENU>(intervalId), nullptr, nullptr);
        SendMessageW(intervalEdit, EM_SETLIMITTEXT, 4, 0);
        secondsLabel = CreateWindowW(L"STATIC", L"seconds", WS_CHILD | WS_VISIBLE, 0,0,0,0, hwnd, nullptr, nullptr, nullptr);
        gpuLabel = CreateWindowW(L"STATIC", L"GPU:", WS_CHILD | WS_VISIBLE, 0,0,0,0, hwnd, nullptr, nullptr, nullptr);
        gpuDropdown = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0,0,0,0, hwnd, reinterpret_cast<HMENU>(gpuId), nullptr, nullptr);
        for (const auto& adapter : adapters) SendMessageW(gpuDropdown, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(adapter.name.c_str()));
        SendMessageW(gpuDropdown, CB_SETCURSEL, 0, 0);
        statusLabel = CreateWindowW(L"STATIC", L"Reading GPU memory counters...", WS_CHILD | WS_VISIBLE, 0,0,0,0, hwnd, nullptr, nullptr, nullptr);
        table = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL, 0,0,0,0, hwnd, nullptr, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(table, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
        for (HWND control : {intervalLabel, intervalEdit, secondsLabel, statusLabel, table, gpuLabel, gpuDropdown}) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        const wchar_t* labels[] = {L"Process", L"PID", L"VRAM / local (KiB)", L"RAM / non-local (KiB)"};
        int widths[] = {390, 80, 170, 180};
        for (int i = 0; i < 4; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; c.pszText = const_cast<wchar_t*>(labels[i]); c.cx = widths[i]; c.fmt = i ? LVCFMT_RIGHT : LVCFMT_LEFT; ListView_InsertColumn(table, i, &c); }
        updateSortIndicator();
        wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wakeEvent) { SetWindowTextW(statusLabel, L"Unable to create the sampling worker."); return 0; }
        worker = std::thread([] {
            GpuCounters counters;
            unsigned fixtureStep = 0;
            ULONGLONG previousSample = 0;
            while (!stopping) {
                ULONGLONG now = GetTickCount64();
                if (previousSample) sampleSpacing = now - previousSample;
                previousSample = now;
                Snapshot sample;
                if (testFixture) {
                    ++fixtureStep;
                    if (!adapters.empty()) {
                        sample.rows.push_back(Row{101, L"ExampleA.exe", (100 + fixtureStep) * 1048576ULL, 10 * 1048576ULL, adapters[0].id});
                        sample.rows.push_back(Row{102, L"ExampleB.exe", 20 * 1048576ULL, (30 + fixtureStep) * 1048576ULL, adapters[0].id});
                    }
                    if (adapters.size() > 1) sample.rows.push_back(Row{103, L"ExampleC.exe", 3 * 1048576ULL, 7 * 1048576ULL, adapters[1].id});
                } else sample = counters.sample();
                { std::lock_guard<std::mutex> lock(resultMutex); latest = std::move(sample); pending = true; }
                do {
                    DWORD result = WaitForSingleObject(wakeEvent, refreshMilliseconds.load());
                    if (stopping || result == WAIT_TIMEOUT || !intervalChanged.exchange(false)) break;
                    // Restart the wait with the new interval without sampling immediately.
                } while (!stopping);
            }
        });
        SetTimer(hwnd, 1, 100, nullptr);
        if (!testReport.empty()) SetTimer(hwnd, 2, 20000, nullptr);
        return 0;
    }
    case WM_SIZE: layout(); return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint);
        if (table && font) drawTotals(dc);
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {760, 380}; return 0;
    case WM_NOTIFY:
        if (table && reinterpret_cast<NMHDR*>(lp)->hwndFrom == ListView_GetHeader(table)) {
            RECT band = totalBand(); InvalidateRect(hwnd, &band, FALSE);
        }
        if (reinterpret_cast<NMHDR*>(lp)->hwndFrom == table && reinterpret_cast<NMHDR*>(lp)->code == LVN_COLUMNCLICK) {
            int column = reinterpret_cast<NMLISTVIEW*>(lp)->iSubItem;
            if (column >= 0 && column < 4) {
                if (column == sortColumn) sortAscending = !sortAscending;
                else { sortColumn = column; sortAscending = column <= 1; }
                updateSortIndicator();
                displaySnapshot(forGpu(displayedRaw, selectedGpu));
            }
        }
        return 0;
    case WM_HSCROLL: {
        RECT band = totalBand(); InvalidateRect(hwnd, &band, FALSE);
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    case WM_COMMAND:
        if (LOWORD(wp) == intervalId && HIWORD(wp) == EN_CHANGE) {
            wchar_t text[32]; GetWindowTextW(intervalEdit, text, 32);
            wchar_t* end = nullptr; unsigned long seconds = wcstoul(text, &end, 10);
            if (end != text && !*end && seconds >= 1 && seconds <= 3600) {
                if (refreshMilliseconds.exchange(seconds * 1000) != seconds * 1000) {
                    intervalChanged = true;
                    if (wakeEvent) SetEvent(wakeEvent);
                }
            }
        }
        if (LOWORD(wp) == intervalId && HIWORD(wp) == EN_KILLFOCUS)
            SetWindowTextW(intervalEdit, std::to_wstring(refreshMilliseconds.load() / 1000).c_str());
        if (LOWORD(wp) == gpuId && HIWORD(wp) == CBN_SELCHANGE) {
            LRESULT index = SendMessageW(gpuDropdown, CB_GETCURSEL, 0, 0);
            if (index >= 0 && static_cast<size_t>(index) < adapters.size()) {
                selectedGpu = adapters[index].id;
                ListView_DeleteAllItems(table); // Reset selection/scroll when changing GPUs.
                displaySnapshot(forGpu(displayedRaw, selectedGpu));
                if (wakeEvent) SetEvent(wakeEvent);
            }
        }
        return 0;
    case WM_TIMER: {
        if (wp == 2) { std::ofstream f{std::filesystem::path(testReport)}; f << "FAIL: GUI sampling timed out\n"; exitResult = 1; DestroyWindow(hwnd); return 0; }
        Snapshot sample;
        { std::lock_guard<std::mutex> lock(resultMutex); if (!pending) return 0; sample = latest; pending = false; }
        displayedRaw = sample;
        // Performance counters can expose virtual/unmapped adapter LUIDs. Only
        // DXGI-enumerated hardware adapters are offered in the GPU selector.
        displaySnapshot(forGpu(sample, selectedGpu));
        if (!testReport.empty() && timingStage) {
            ULONGLONG spacing = sampleSpacing.load();
            DWORD expected = timingStage == 1 ? 1000 : 3000;
            // GPU-switch requests may already have queued a sample before the interval edit.
            if (spacing < 200) return 0;
            if (spacing < expected - 200 || spacing > expected + 700) {
                std::ofstream f{std::filesystem::path(testReport)};
                f << "FAIL: refresh interval " << expected << "ms; observed " << spacing << "ms\n";
                exitResult = 1; DestroyWindow(hwnd); return 0;
            }
            if (timingStage == 1) { timingStage = 2; SetWindowTextW(intervalEdit, L"3"); return 0; }
            SetWindowTextW(intervalEdit, L"2");
            RedrawWindow(hwnd, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_INVALIDATE);
            bool captured = captureWindow(testReport + L".bmp");
            std::ofstream f{std::filesystem::path(testReport)};
            f << "PASS: GPU filtering, sorting, KiB values, totals and resize; editable interval measured at 1 and 3 seconds; capture=" << captured << '\n';
            exitResult = captured ? 0 : 1; DestroyWindow(hwnd); return 0;
        }
        if (!testReport.empty()) {
            bool ok = memoryKiB(1024) == L"1" && memoryKiB(1048576) == L"1,024" &&
                memoryKiB(540127232) == L"527,468" && memoryKiB(1023) == L"0" &&
                !adapters.empty() && !sample.rows.empty() &&
                SendMessageW(gpuDropdown, CB_GETCOUNT, 0, 0) == static_cast<LRESULT>(adapters.size());
            LRESULT original = SendMessageW(gpuDropdown, CB_GETCURSEL, 0, 0);
            for (size_t i = 0; i < adapters.size() && ok; ++i) {
                SendMessageW(gpuDropdown, CB_SETCURSEL, i, 0);
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(gpuId, CBN_SELCHANGE), reinterpret_cast<LPARAM>(gpuDropdown));
                ok = tableMatches(forGpu(sample, selectedGpu));
                if (testSamples == 0) {
                    ok = writeCsv(testReport + L".gpu-" + adapterIdText(selectedGpu) + L".csv", forGpu(sample, selectedGpu)) && ok;
                    RedrawWindow(hwnd, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_INVALIDATE);
                    ok = captureWindow(testReport + L".gpu-" + adapterIdText(selectedGpu) + L".bmp") && ok;
                }
            }
            SendMessageW(gpuDropdown, CB_SETCURSEL, original, 0);
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(gpuId, CBN_SELCHANGE), reinterpret_cast<LPARAM>(gpuDropdown));
            if (ok && testSamples == 0) {
                for (int column = 0; column < 4 && ok; ++column) {
                    for (int click = 0; click < 2 && ok; ++click) {
                        NMLISTVIEW event{}; event.hdr.hwndFrom = table; event.hdr.code = LVN_COLUMNCLICK; event.iSubItem = column;
                        SendMessageW(hwnd, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&event));
                        HDITEMW header{}; header.mask = HDI_FORMAT;
                        ok = tableMatches(forGpu(sample, selectedGpu)) && Header_GetItem(ListView_GetHeader(table), column, &header) &&
                            !!(header.fmt & (sortAscending ? HDF_SORTUP : HDF_SORTDOWN));
                    }
                }
                // Keep the chosen sort while switching between adapters.
                for (size_t i = 0; i < adapters.size() && ok; ++i) {
                    SendMessageW(gpuDropdown, CB_SETCURSEL, i, 0);
                    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(gpuId, CBN_SELCHANGE), reinterpret_cast<LPARAM>(gpuDropdown));
                    ok = sortColumn == 3 && sortAscending && tableMatches(forGpu(sample, selectedGpu));
                }
                SendMessageW(gpuDropdown, CB_SETCURSEL, original, 0);
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(gpuId, CBN_SELCHANGE), reinterpret_cast<LPARAM>(gpuDropdown));
                // Restore the initial local-memory descending sort for later samples.
                if (sortColumn != 2) {
                    NMLISTVIEW event{}; event.hdr.hwndFrom = table; event.hdr.code = LVN_COLUMNCLICK; event.iSubItem = 2;
                    SendMessageW(hwnd, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&event));
                }
            }
            if (!ok) { std::ofstream f{std::filesystem::path(testReport)}; f << "FAIL: live GUI rows or counters invalid: " << utf8(sample.error) << '\n'; exitResult = 1; DestroyWindow(hwnd); }
            else if (++testSamples >= 3) {
                ResetEvent(wakeEvent);
                timingStage = 1;
                SetWindowTextW(intervalEdit, L"1");
            } else if (testSamples == 1) { SetWindowPos(hwnd, nullptr, 0, 0, 940, 560, SWP_NOMOVE | SWP_NOZORDER); }

        }
        return 0;
    }
    case WM_DESTROY:
        stopping = true; if (wakeEvent) SetEvent(wakeEvent);
        if (worker.joinable()) worker.join();
        if (wakeEvent) CloseHandle(wakeEvent);
        if (font) DeleteObject(font);
        PostQuitMessage(exitResult); return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    enumerateAdapters();
    if (!adapters.empty()) selectedGpu = adapters.front().id;
    if (argc == 3 && std::wstring(argv[1]) == L"--adapters") {
        std::ofstream file{std::filesystem::path(argv[2])}; LocalFree(argv);
        file << "AdapterId,Name\n";
        for (const auto& adapter : adapters) {
            std::string name = utf8(adapter.name), escaped;
            for (char c : name) { escaped += c; if (c == '"') escaped += '"'; }
            file << utf8(adapterIdText(adapter.id)) << ",\"" << escaped << "\"\n";
        }
        return file && !adapters.empty() ? 0 : 1;
    }
    if ((argc == 3 || argc == 5) && std::wstring(argv[1]) == L"--snapshot") {
        if (argc == 5) {
            wchar_t* end = nullptr; uint64_t id = wcstoull(argv[4], &end, 16);
            if (std::wstring(argv[3]) != L"--gpu" || end == argv[4] || *end != L'\0' || std::none_of(adapters.begin(), adapters.end(), [&](const Adapter& a) { return a.id == id; })) { LocalFree(argv); return 1; }
            selectedGpu = id;
        }
        std::wstring output = argv[2]; LocalFree(argv);
        GpuCounters counters; auto sample = counters.sample();
        return writeCsv(output, forGpu(sample, selectedGpu)) ? 0 : 1;
    }
    if (argc == 3 && (std::wstring(argv[1]) == L"--ui-test" || std::wstring(argv[1]) == L"--ui-test-fixture")) {
        testReport = argv[2]; testFixture = std::wstring(argv[1]) == L"--ui-test-fixture";
    }
    else if (argc != 1) { LocalFree(argv); MessageBoxW(nullptr, L"Double-click to open. Optional: --snapshot output.csv [--gpu adapter-id], --adapters adapters.csv, or --ui-test report.txt", L"GPU VRAM Map", MB_ICONINFORMATION); return 1; }
    LocalFree(argv);
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSW cls{}; cls.lpfnWndProc = windowProc; cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); cls.lpszClassName = L"GpuVramMapWindow";
    if (!RegisterClassW(&cls)) return 1;
    HWND hwnd = CreateWindowW(cls.lpszClassName, L"GPU VRAM Map", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 920, 600, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, testReport.empty() ? show : SW_SHOWNORMAL);
    // The first ShowWindow may be overridden by STARTUPINFO (e.g. a test runner).
    if (!testReport.empty()) ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    return static_cast<int>(message.wParam);
}

