#include "dat_tool_gui.h"

#include "fs/dat_archive.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kWindowClass[] = L"OpenNRDatArchiveBrowser";

enum ControlId : int {
    kOpen = 100,
    kSearch,
    kEntryList,
    kExtractSelected,
    kExtractAll,
    kExit,
    kAbout,
};

constexpr UINT kExtractionProgress = WM_APP + 1;
constexpr UINT kExtractionComplete = WM_APP + 2;
constexpr UINT_PTR kSearchTimer = 1;

struct ExtractionResult {
    fs::path destination;
    std::size_t written = 0;
    std::size_t failed = 0;
    std::vector<std::wstring> errors;
};

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int count = MultiByteToWideChar(code_page, flags, text.data(),
                                    static_cast<int>(text.size()), nullptr, 0);
    if (count == 0) {
        code_page = CP_ACP;
        flags = 0;
        count = MultiByteToWideChar(code_page, flags, text.data(),
                                    static_cast<int>(text.size()), nullptr, 0);
    }
    if (count <= 0) return L"(invalid name)";
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(code_page, flags, text.data(),
                        static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::wstring format_bytes(std::uint64_t bytes) {
    static constexpr const wchar_t* suffixes[] = {
        L"B", L"KB", L"MB", L"GB", L"TB"
    };
    double value = static_cast<double>(bytes);
    std::size_t suffix = 0;
    while (value >= 1024.0 && suffix + 1 < std::size(suffixes)) {
        value /= 1024.0;
        ++suffix;
    }
    wchar_t buffer[64]{};
    if (suffix == 0) {
        swprintf_s(buffer, L"%llu %s",
                   static_cast<unsigned long long>(bytes), suffixes[suffix]);
    } else {
        swprintf_s(buffer, value >= 100.0 ? L"%.0f %s" : L"%.1f %s",
                   value, suffixes[suffix]);
    }
    return buffer;
}

std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return text;
}

std::optional<fs::path> safe_relative_path(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    fs::path input = fs::path(widen(name));
    if (input.empty() || input.has_root_path()) return std::nullopt;

    fs::path result;
    for (const auto& component : input) {
        if (component == "." || component.empty()) continue;
        if (component == "..") return std::nullopt;
#ifdef _WIN32
        if (component.native().find(L':') != std::wstring::npos) {
            return std::nullopt;
        }
#endif
        result /= component;
    }
    if (result.empty()) return std::nullopt;
    return result;
}

std::optional<fs::path> choose_archive(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                reinterpret_cast<void**>(&dialog)))) {
        return std::nullopt;
    }
    const COMDLG_FILTERSPEC filters[] = {
        {L"Papyrus DAT archives (*.dat)", L"*.dat"},
        {L"All files (*.*)", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"dat");
    dialog->SetTitle(L"Open a Papyrus DAT archive");

    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = fs::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

std::optional<fs::path> choose_folder(HWND owner, const fs::path& initial) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                reinterpret_cast<void**>(&dialog)))) {
        return std::nullopt;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                       FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose an extraction folder");
    if (!initial.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                initial.c_str(), nullptr, IID_IShellItem,
                reinterpret_cast<void**>(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = fs::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

class DatBrowser {
public:
    explicit DatBrowser(HINSTANCE instance) : instance_(instance) {}

    ~DatBrowser() {
        if (worker_.joinable()) worker_.join();
        if (font_) DeleteObject(font_);
        if (title_font_) DeleteObject(title_font_);
        if (accelerators_) DestroyAcceleratorTable(accelerators_);
    }

    bool create() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &DatBrowser::window_proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = wc.hIcon;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        ACCEL shortcuts[] = {
            {FVIRTKEY | FCONTROL, 'O', kOpen},
            {FVIRTKEY | FCONTROL, 'E', kExtractSelected},
            {FVIRTKEY | FCONTROL | FSHIFT, 'E', kExtractAll},
        };
        accelerators_ = CreateAcceleratorTableW(shortcuts,
                                                static_cast<int>(std::size(shortcuts)));

        window_ = CreateWindowExW(
            WS_EX_APPWINDOW | WS_EX_ACCEPTFILES, kWindowClass,
            L"OpenNR DAT Archive Browser", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 1040, 700, nullptr, nullptr, instance_, this);
        if (!window_) return false;
        ShowWindow(window_, SW_SHOWDEFAULT);
        UpdateWindow(window_);
        return true;
    }

    int run(const fs::path& initial_archive) {
        if (!initial_archive.empty()) load_archive(initial_archive);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!TranslateAcceleratorW(window_, accelerators_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        DatBrowser* self = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<DatBrowser*>(create->lpCreateParams);
            self->window_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<DatBrowser*>(
                GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->handle_message(message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE:
                create_controls();
                return 0;
            case WM_SIZE:
                layout_controls(LOWORD(lparam), HIWORD(lparam));
                return 0;
            case WM_GETMINMAXINFO: {
                auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
                info->ptMinTrackSize = {760, 500};
                return 0;
            }
            case WM_COMMAND:
                handle_command(LOWORD(wparam), HIWORD(wparam));
                return 0;
            case WM_TIMER:
                KillTimer(window_, kSearchTimer);
                if (wparam == kSearchTimer && archive_ && !extracting_ &&
                    current_search_text() != applied_filter_) {
                    rebuild_list();
                }
                return 0;
            case WM_NOTIFY:
                return handle_notify(reinterpret_cast<NMHDR*>(lparam));
            case WM_CTLCOLORSTATIC: {
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            case WM_DROPFILES:
                handle_drop(reinterpret_cast<HDROP>(wparam));
                return 0;
            case WM_CONTEXTMENU:
                if (reinterpret_cast<HWND>(wparam) == list_) {
                    show_list_menu(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                    return 0;
                }
                break;
            case kExtractionProgress:
                show_progress(static_cast<std::size_t>(wparam),
                              static_cast<std::size_t>(lparam));
                return 0;
            case kExtractionComplete:
                finish_extraction(reinterpret_cast<ExtractionResult*>(lparam));
                return 0;
            case WM_CLOSE:
                if (extracting_) {
                    MessageBoxW(window_,
                        L"Files are still being extracted. Please wait for the operation to finish.",
                        L"Extraction in progress", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                DestroyWindow(window_);
                return 0;
            case WM_DESTROY:
                KillTimer(window_, kSearchTimer);
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    void create_controls() {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                              &metrics, 0);
        metrics.lfMessageFont.lfHeight = -16;
        wcscpy_s(metrics.lfMessageFont.lfFaceName, L"Segoe UI");
        font_ = CreateFontIndirectW(&metrics.lfMessageFont);
        LOGFONTW title_log = metrics.lfMessageFont;
        title_log.lfHeight = -24;
        title_log.lfWeight = FW_SEMIBOLD;
        title_font_ = CreateFontIndirectW(&title_log);

        title_ = make_control(L"STATIC", L"DAT Archive Browser",
                              SS_LEFT, 0);
        subtitle_ = make_control(L"STATIC",
            L"Browse, search, and safely extract Papyrus game archives.",
            SS_LEFT, 0);
        open_button_ = make_control(L"BUTTON", L"Open archive...",
                                    BS_PUSHBUTTON, kOpen);
        archive_name_ = make_control(L"STATIC", L"No archive open",
                                     SS_LEFT, 0);
        archive_path_label_ = make_control(L"STATIC",
            L"Open a .dat file or drop one anywhere in this window.",
            SS_LEFT | SS_PATHELLIPSIS, 0);
        summary_ = make_control(L"STATIC", L"", SS_LEFT, 0);
        search_label_ = make_control(L"STATIC", L"Search entries",
                                     SS_LEFT, 0);
        search_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kSearch), instance_, nullptr);
        SendMessageW(search_, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"Type a file name or extension"));

        list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEntryList), instance_, nullptr);
        ListView_SetExtendedListViewStyleEx(list_, 0,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP |
            LVS_EX_LABELTIP);
        SetWindowTheme(list_, L"Explorer", nullptr);
        add_column(0, L"Name", LVCFMT_LEFT);
        add_column(1, L"Storage", LVCFMT_LEFT);
        add_column(2, L"Original size", LVCFMT_RIGHT);
        add_column(3, L"Stored size", LVCFMT_RIGHT);
        add_column(4, L"Space saved", LVCFMT_RIGHT);

        empty_state_ = make_control(L"STATIC",
            L"Open a DAT archive to see its contents\r\n\r\nYou can also drag and drop a .dat file here.",
            SS_CENTER, 0);
        extract_selected_ = make_control(L"BUTTON", L"Extract selected...",
                                         BS_PUSHBUTTON, kExtractSelected);
        extract_all_ = make_control(L"BUTTON", L"Extract all...",
                                    BS_DEFPUSHBUTTON, kExtractAll);
        status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"Ready",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
            window_, nullptr, instance_, nullptr);

        const HWND controls[] = {
            title_, subtitle_, open_button_, archive_name_, archive_path_label_,
            summary_, search_label_, search_, list_, empty_state_,
            extract_selected_, extract_all_, status_
        };
        for (HWND control : controls) {
            SendMessageW(control, WM_SETFONT,
                reinterpret_cast<WPARAM>(control == title_ ? title_font_ : font_), TRUE);
        }
        EnableWindow(search_, FALSE);
        EnableWindow(extract_selected_, FALSE);
        EnableWindow(extract_all_, FALSE);
        create_menu();
        DragAcceptFiles(window_, TRUE);
        update_selection_state();
    }

    HWND make_control(const wchar_t* type, const wchar_t* text,
                      DWORD style, int id) const {
        return CreateWindowExW(0, type, text,
            WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window_,
            id ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
            instance_, nullptr);
    }

    void create_menu() const {
        HMENU bar = CreateMenu();
        HMENU file = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, kOpen, L"&Open archive...\tCtrl+O");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, kExit, L"E&xit");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

        HMENU extract = CreatePopupMenu();
        AppendMenuW(extract, MF_STRING, kExtractSelected,
                    L"Extract &selected...\tCtrl+E");
        AppendMenuW(extract, MF_STRING, kExtractAll,
                    L"Extract &all...\tCtrl+Shift+E");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(extract), L"&Extract");

        HMENU help = CreatePopupMenu();
        AppendMenuW(help, MF_STRING, kAbout, L"&About");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
        SetMenu(window_, bar);
    }

    void add_column(int index, const wchar_t* name, int format) const {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_FMT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(name);
        column.fmt = format;
        column.cx = 120;
        ListView_InsertColumn(list_, index, &column);
    }

    void layout_controls(int width, int height) const {
        if (!status_) return;
        SendMessageW(status_, WM_SIZE, 0, 0);
        RECT status_rect{};
        GetWindowRect(status_, &status_rect);
        const int status_height = status_rect.bottom - status_rect.top;
        const int margin = 24;
        const int button_width = 138;
        const int open_width = 146;
        const int footer_height = 62;
        const int list_top = 186;
        const int list_bottom = height - status_height - footer_height;

        MoveWindow(title_, margin, 16, width - open_width - margin * 3, 31, TRUE);
        MoveWindow(subtitle_, margin, 50, width - open_width - margin * 3, 24, TRUE);
        MoveWindow(open_button_, width - margin - open_width, 24,
                   open_width, 38, TRUE);
        MoveWindow(archive_name_, margin, 89, width - margin * 2, 23, TRUE);
        MoveWindow(archive_path_label_, margin, 113, width - margin * 2, 21, TRUE);
        MoveWindow(summary_, margin, 139, width / 2, 22, TRUE);
        MoveWindow(search_label_, width - margin - 360, 139, 105, 22, TRUE);
        MoveWindow(search_, width - margin - 250, 134, 250, 29, TRUE);
        MoveWindow(list_, margin, list_top, width - margin * 2,
                   std::max(80, list_bottom - list_top), TRUE);
        MoveWindow(empty_state_, margin + 2, list_top + 90,
                   width - margin * 2 - 4, 90, TRUE);
        MoveWindow(extract_selected_, width - margin - button_width * 2 - 12,
                   list_bottom + 12, button_width, 36, TRUE);
        MoveWindow(extract_all_, width - margin - button_width,
                   list_bottom + 12, button_width, 36, TRUE);

        const int list_width = std::max(300, width - margin * 2 - 4);
        ListView_SetColumnWidth(list_, 0, list_width * 43 / 100);
        ListView_SetColumnWidth(list_, 1, list_width * 13 / 100);
        ListView_SetColumnWidth(list_, 2, list_width * 15 / 100);
        ListView_SetColumnWidth(list_, 3, list_width * 15 / 100);
        ListView_SetColumnWidth(list_, 4, list_width * 12 / 100);
    }

    void handle_command(int id, int notification) {
        if (id == kSearch && notification == EN_CHANGE) {
            // Rebuilding the list from inside the edit control's synchronous
            // notification can disturb the text operation that raised it.
            // Restarting this one-shot timer also avoids re-sorting hundreds
            // of rows for every keystroke in a quick paste.
            SetTimer(window_, kSearchTimer, 180, nullptr);
            return;
        }
        switch (id) {
            case kOpen:
                if (!extracting_) {
                    if (auto path = choose_archive(window_)) load_archive(*path);
                }
                break;
            case kExtractSelected:
                extract_selected();
                break;
            case kExtractAll:
                extract_all();
                break;
            case kExit:
                SendMessageW(window_, WM_CLOSE, 0, 0);
                break;
            case kAbout:
                MessageBoxW(window_,
                    L"OpenNR DAT Archive Browser\r\n\r\n"
                    L"Browse and extract Papyrus .dat game archives. The existing "
                    L"list, extract, and extract-one command-line commands remain available.",
                    L"About", MB_OK | MB_ICONINFORMATION);
                break;
        }
    }

    LRESULT handle_notify(NMHDR* header) {
        if (!header || header->hwndFrom != list_) return 0;
        if (header->code == LVN_ITEMCHANGED) {
            update_selection_state();
        } else if (header->code == LVN_COLUMNCLICK) {
            const auto* info = reinterpret_cast<NMLISTVIEW*>(header);
            if (sort_column_ == info->iSubItem) {
                sort_ascending_ = !sort_ascending_;
            } else {
                sort_column_ = info->iSubItem;
                sort_ascending_ = true;
            }
            rebuild_list();
        } else if (header->code == NM_DBLCLK &&
                   ListView_GetSelectedCount(list_) > 0) {
            extract_selected();
        }
        return 0;
    }

    void handle_drop(HDROP drop) {
        const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
        if (length > 0 && DragQueryFileW(drop, 0, path.data(), length + 1) > 0 &&
            !extracting_) {
            path.resize(length);
            load_archive(fs::path(path));
        }
        DragFinish(drop);
    }

    void load_archive(const fs::path& path) {
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        set_status(L"Opening " + path.filename().wstring() + L"...");
        try {
            auto loaded = std::make_shared<opennr::DatArchive>(
                opennr::DatArchive::load(path));
            archive_ = std::move(loaded);
            archive_path_ = path;
            SetWindowTextW(search_, L"");
            SetWindowTextW(archive_name_, path.filename().c_str());
            SetWindowTextW(archive_path_label_, path.c_str());
            SetWindowTextW(window_,
                (path.filename().wstring() + L" — OpenNR DAT Archive Browser").c_str());
            EnableWindow(search_, TRUE);
            EnableWindow(extract_all_, !archive_->entries().empty());
            rebuild_list();
            ShowWindow(empty_state_, archive_->entries().empty() ? SW_SHOW : SW_HIDE);
        } catch (const std::exception& error) {
            set_status(L"Could not open archive");
            const std::wstring detail = L"OpenNR could not read this archive.\r\n\r\n" +
                                        widen(error.what());
            MessageBoxW(window_, detail.c_str(), L"Unable to open DAT archive",
                        MB_OK | MB_ICONERROR);
        }
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }

    void rebuild_list() {
        if (!list_) return;
        ListView_DeleteAllItems(list_);
        visible_entries_.clear();
        if (!archive_) return;

        applied_filter_ = current_search_text();
        const std::wstring needle = lower(applied_filter_);
        const auto& entries = archive_->entries();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (needle.empty() || lower(widen(entries[i].name)).find(needle) !=
                                      std::wstring::npos) {
                visible_entries_.push_back(i);
            }
        }
        std::sort(visible_entries_.begin(), visible_entries_.end(),
            [&](std::size_t a, std::size_t b) {
                const auto& left = entries[a];
                const auto& right = entries[b];
                int order = 0;
                switch (sort_column_) {
                    case 1:
                        order = static_cast<int>(left.is_compressed()) -
                                static_cast<int>(right.is_compressed());
                        break;
                    case 2:
                        order = left.uncompressed_size < right.uncompressed_size ? -1 :
                                left.uncompressed_size > right.uncompressed_size ? 1 : 0;
                        break;
                    case 3:
                        order = left.compressed_size < right.compressed_size ? -1 :
                                left.compressed_size > right.compressed_size ? 1 : 0;
                        break;
                    case 4: {
                        const double lr = left.uncompressed_size == 0 ? 0.0 :
                            1.0 - static_cast<double>(left.compressed_size) /
                                  left.uncompressed_size;
                        const double rr = right.uncompressed_size == 0 ? 0.0 :
                            1.0 - static_cast<double>(right.compressed_size) /
                                  right.uncompressed_size;
                        order = lr < rr ? -1 : lr > rr ? 1 : 0;
                        break;
                    }
                    default:
                        order = CompareStringOrdinal(widen(left.name).c_str(), -1,
                                                     widen(right.name).c_str(), -1,
                                                     TRUE) - CSTR_EQUAL;
                        break;
                }
                if (order == 0 && sort_column_ != 0) {
                    order = CompareStringOrdinal(widen(left.name).c_str(), -1,
                                                 widen(right.name).c_str(), -1,
                                                 TRUE) - CSTR_EQUAL;
                }
                return sort_ascending_ ? order < 0 : order > 0;
            });

        SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
        for (std::size_t row = 0; row < visible_entries_.size(); ++row) {
            const std::size_t index = visible_entries_[row];
            const auto& entry = entries[index];
            std::wstring name = widen(entry.name);
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = static_cast<int>(row);
            item.pszText = name.data();
            item.lParam = static_cast<LPARAM>(index);
            const int inserted = ListView_InsertItem(list_, &item);

            std::wstring storage = entry.is_compressed() ? L"Compressed" : L"Stored";
            std::wstring original = format_bytes(entry.uncompressed_size);
            std::wstring stored = format_bytes(entry.compressed_size);
            std::wstring saved = L"—";
            if (entry.is_compressed() && entry.uncompressed_size != 0) {
                wchar_t ratio[32]{};
                const double percent = 100.0 *
                    (1.0 - static_cast<double>(entry.compressed_size) /
                           entry.uncompressed_size);
                swprintf_s(ratio, L"%.1f%%", percent);
                saved = ratio;
            }
            ListView_SetItemText(list_, inserted, 1, storage.data());
            ListView_SetItemText(list_, inserted, 2, original.data());
            ListView_SetItemText(list_, inserted, 3, stored.data());
            ListView_SetItemText(list_, inserted, 4, saved.data());
        }
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list_, nullptr, TRUE);
        update_sort_indicator();
        update_archive_summary();
        update_selection_state();
    }

    std::wstring current_search_text() const {
        wchar_t search_buffer[1024]{};
        // Resolve by control ID so this reads the live, displayed edit field.
        const HWND search = GetDlgItem(window_, kSearch);
        if (search) {
            GetWindowTextW(search, search_buffer,
                           static_cast<int>(std::size(search_buffer)));
        }
        return search_buffer;
    }

    void update_sort_indicator() const {
        HWND header = ListView_GetHeader(list_);
        const int count = Header_GetItemCount(header);
        for (int i = 0; i < count; ++i) {
            HDITEMW item{};
            item.mask = HDI_FORMAT;
            Header_GetItem(header, i, &item);
            item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            if (i == sort_column_) {
                item.fmt |= sort_ascending_ ? HDF_SORTUP : HDF_SORTDOWN;
            }
            Header_SetItem(header, i, &item);
        }
    }

    void update_archive_summary() const {
        if (!archive_) return;
        std::uint64_t original = 0;
        std::uint64_t stored = 0;
        std::size_t compressed = 0;
        for (const auto& entry : archive_->entries()) {
            original += entry.uncompressed_size;
            stored += entry.compressed_size;
            if (entry.is_compressed()) ++compressed;
        }
        std::wstring entry_count = applied_filter_.empty()
            ? std::to_wstring(archive_->entries().size()) + L" entries"
            : std::to_wstring(visible_entries_.size()) + L" of " +
              std::to_wstring(archive_->entries().size()) + L" entries";
        std::wstring text = entry_count + L"  •  " +
            format_bytes(original) + L" unpacked  •  " +
            format_bytes(stored) + L" stored  •  " +
            std::to_wstring(compressed) + L" compressed";
        SetWindowTextW(summary_, text.c_str());
    }

    void update_selection_state() const {
        const int selected = list_ ? ListView_GetSelectedCount(list_) : 0;
        EnableWindow(extract_selected_, archive_ && selected > 0 && !extracting_);
        HMENU menu = GetMenu(window_);
        if (menu) {
            EnableMenuItem(menu, kExtractSelected, MF_BYCOMMAND |
                (archive_ && selected > 0 && !extracting_ ? MF_ENABLED : MF_GRAYED));
            EnableMenuItem(menu, kExtractAll, MF_BYCOMMAND |
                (archive_ && !archive_->entries().empty() && !extracting_ ?
                    MF_ENABLED : MF_GRAYED));
        }
        if (archive_ && !extracting_) {
            std::wstring status = std::to_wstring(visible_entries_.size()) + L" of " +
                std::to_wstring(archive_->entries().size()) + L" entries shown";
            if (selected > 0) {
                status += L"  •  " + std::to_wstring(selected) + L" selected";
            }
            set_status(status);
        }
    }

    std::vector<std::size_t> selected_entries() const {
        std::vector<std::size_t> result;
        int row = -1;
        while ((row = ListView_GetNextItem(list_, row, LVNI_SELECTED)) != -1) {
            LVITEMW item{};
            item.mask = LVIF_PARAM;
            item.iItem = row;
            if (ListView_GetItem(list_, &item)) {
                result.push_back(static_cast<std::size_t>(item.lParam));
            }
        }
        return result;
    }

    void extract_selected() {
        if (extracting_ || !archive_) return;
        auto entries = selected_entries();
        if (entries.empty()) return;
        auto folder = choose_folder(window_, archive_path_.parent_path());
        if (folder) begin_extraction(std::move(entries), *folder);
    }

    void extract_all() {
        if (extracting_ || !archive_ || archive_->entries().empty()) return;
        std::vector<std::size_t> entries(archive_->entries().size());
        for (std::size_t i = 0; i < entries.size(); ++i) entries[i] = i;
        auto folder = choose_folder(window_, archive_path_.parent_path());
        if (folder) begin_extraction(std::move(entries), *folder);
    }

    void begin_extraction(std::vector<std::size_t> indices,
                          const fs::path& destination) {
        if (worker_.joinable()) worker_.join();
        std::size_t existing_files = 0;
        for (const std::size_t index : indices) {
            const auto relative = safe_relative_path(archive_->entries()[index].name);
            std::error_code error;
            if (relative && fs::is_regular_file(destination / *relative, error)) {
                ++existing_files;
            }
        }
        if (existing_files > 0) {
            const std::wstring message = std::to_wstring(existing_files) +
                (existing_files == 1 ? L" file already exists" : L" files already exist") +
                L" in that folder and will be replaced.\r\n\r\nContinue extracting?";
            if (MessageBoxW(window_, message.c_str(), L"Replace existing files?",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return;
            }
        }
        extracting_ = true;
        EnableWindow(open_button_, FALSE);
        EnableWindow(search_, FALSE);
        EnableWindow(extract_selected_, FALSE);
        EnableWindow(extract_all_, FALSE);
        DragAcceptFiles(window_, FALSE);
        EnableMenuItem(GetMenu(window_), kOpen, MF_BYCOMMAND | MF_GRAYED);
        update_selection_state();
        set_status(L"Preparing to extract " + std::to_wstring(indices.size()) +
                   L" entries...");

        HWND notify = window_;
        auto archive = archive_;
        worker_ = std::thread([notify, archive, indices = std::move(indices),
                               destination]() {
            auto result = std::make_unique<ExtractionResult>();
            result->destination = destination;
            try {
                fs::create_directories(destination);
            } catch (const std::exception& error) {
                result->failed = indices.size();
                result->errors.push_back(widen(error.what()));
                PostMessageW(notify, kExtractionComplete, 0,
                             reinterpret_cast<LPARAM>(result.release()));
                return;
            }

            for (std::size_t position = 0; position < indices.size(); ++position) {
                const auto& entry = archive->entries()[indices[position]];
                try {
                    const auto relative = safe_relative_path(entry.name);
                    if (!relative) throw std::runtime_error("unsafe entry path");
                    const fs::path output_path = destination / *relative;
                    if (output_path.has_parent_path()) {
                        fs::create_directories(output_path.parent_path());
                    }
                    const auto bytes = archive->read(entry);
                    std::ofstream output(output_path, std::ios::binary);
                    if (!output) throw std::runtime_error("cannot create output file");
                    output.write(reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<std::streamsize>(bytes.size()));
                    if (!output) throw std::runtime_error("write failed");
                    ++result->written;
                } catch (const std::exception& error) {
                    ++result->failed;
                    if (result->errors.size() < 5) {
                        result->errors.push_back(widen(entry.name) + L": " +
                                                 widen(error.what()));
                    }
                }
                if (position == 0 || (position + 1) % 10 == 0 ||
                    position + 1 == indices.size()) {
                    PostMessageW(notify, kExtractionProgress,
                                 static_cast<WPARAM>(position + 1),
                                 static_cast<LPARAM>(indices.size()));
                }
            }
            PostMessageW(notify, kExtractionComplete, 0,
                         reinterpret_cast<LPARAM>(result.release()));
        });
    }

    void show_progress(std::size_t complete, std::size_t total) const {
        set_status(L"Extracting... " + std::to_wstring(complete) + L" of " +
                   std::to_wstring(total) + L" entries");
    }

    void finish_extraction(ExtractionResult* raw_result) {
        std::unique_ptr<ExtractionResult> result(raw_result);
        if (worker_.joinable()) worker_.join();
        extracting_ = false;
        EnableWindow(open_button_, TRUE);
        EnableWindow(search_, archive_ != nullptr);
        EnableWindow(extract_all_, archive_ && !archive_->entries().empty());
        DragAcceptFiles(window_, TRUE);
        EnableMenuItem(GetMenu(window_), kOpen, MF_BYCOMMAND | MF_ENABLED);
        update_selection_state();

        if (!result) return;
        std::wstring message = L"Extracted " + std::to_wstring(result->written) +
            L" entries to:\r\n" + result->destination.wstring();
        if (result->failed != 0) {
            message += L"\r\n\r\n" + std::to_wstring(result->failed) +
                       L" entries could not be extracted.";
            for (const auto& error : result->errors) message += L"\r\n• " + error;
            set_status(L"Extraction completed with errors");
            MessageBoxW(window_, message.c_str(), L"Extraction completed",
                        MB_OK | MB_ICONWARNING);
        } else {
            set_status(L"Extraction complete");
            message += L"\r\n\r\nOpen the destination folder?";
            if (MessageBoxW(window_, message.c_str(), L"Extraction complete",
                            MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                ShellExecuteW(window_, L"open", result->destination.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
    }

    void show_list_menu(int x, int y) const {
        if (!archive_) return;
        if (x == -1 && y == -1) {
            RECT rect{};
            GetWindowRect(list_, &rect);
            x = rect.left + 40;
            y = rect.top + 40;
        }
        HMENU menu = CreatePopupMenu();
        const bool has_selection = ListView_GetSelectedCount(list_) > 0;
        AppendMenuW(menu, MF_STRING | (has_selection ? MF_ENABLED : MF_GRAYED),
                    kExtractSelected, L"Extract selected...");
        AppendMenuW(menu, MF_STRING, kExtractAll, L"Extract all...");
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    void set_status(const std::wstring& text) const {
        if (status_) SetWindowTextW(status_, text.c_str());
    }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND title_ = nullptr;
    HWND subtitle_ = nullptr;
    HWND open_button_ = nullptr;
    HWND archive_name_ = nullptr;
    HWND archive_path_label_ = nullptr;
    HWND summary_ = nullptr;
    HWND search_label_ = nullptr;
    HWND search_ = nullptr;
    HWND list_ = nullptr;
    HWND empty_state_ = nullptr;
    HWND extract_selected_ = nullptr;
    HWND extract_all_ = nullptr;
    HWND status_ = nullptr;
    HFONT font_ = nullptr;
    HFONT title_font_ = nullptr;
    HACCEL accelerators_ = nullptr;

    std::shared_ptr<opennr::DatArchive> archive_;
    fs::path archive_path_;
    std::vector<std::size_t> visible_entries_;
    std::wstring applied_filter_;
    int sort_column_ = 0;
    bool sort_ascending_ = true;
    bool extracting_ = false;
    std::thread worker_;
};

} // namespace

int run_dat_tool_gui(const fs::path& initial_archive) {
    // A console-subsystem executable retains full CLI behaviour, but Explorer
    // creates a redundant console when it is double-clicked. Detach only when
    // this process is the console's sole client; an inherited terminal stays
    // attached when the GUI is launched from a prompt.
    DWORD console_processes[2]{};
    if (GetConsoleProcessList(console_processes,
                              static_cast<DWORD>(std::size(console_processes))) == 1) {
        FreeConsole();
    }
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                       COINIT_DISABLE_OLE1DDE);
    DatBrowser browser(GetModuleHandleW(nullptr));
    if (!browser.create()) {
        MessageBoxW(nullptr, L"The DAT Archive Browser could not be started.",
                    L"OpenNR DAT Tool", MB_OK | MB_ICONERROR);
        if (SUCCEEDED(com_result)) CoUninitialize();
        return 1;
    }
    const int result = browser.run(initial_archive);
    if (SUCCEEDED(com_result)) CoUninitialize();
    return result;
}
