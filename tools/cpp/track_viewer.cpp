#include "viewer/track_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using opennr::viewer::TrackCamera;
using opennr::viewer::TrackFrame;
using opennr::viewer::TrackViewMode;
using opennr::viewer::TrackViewModes;
using opennr::viewer::TrackViewModel;

namespace {

struct Options {
    std::optional<fs::path> input;
    std::optional<fs::path> snapshot;
    std::optional<fs::path> shared_folder;
    std::optional<fs::path> log;
    bool load_only = false;
    TrackViewModes modes = opennr::viewer::track_view_mode(TrackViewMode::Solid);
    int width = 1600;
    int height = 900;
};

fs::path default_log_path(const fs::path& input) {
    fs::path folder = fs::is_directory(input) ? input : input.parent_path();
    if (folder.empty()) folder = fs::current_path();
    return folder / "opennr_track_viewer_missing.log";
}

void write_load_log(const fs::path& path,
                    const fs::path& input,
                    const std::optional<fs::path>& shared_folder,
                    const TrackViewModel& model) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write the load log: " + path.string());
    output << "OpenNR Track Viewer load log\n"
           << "Track input: " << input.string() << "\n"
           << "PTF source: " << model.source_path.string() << "\n"
           << "Shared folder: "
           << (shared_folder ? shared_folder->string() : std::string("None")) << "\n"
           << "Loaded object instances: " << model.object_count << "\n"
           << "Missing object instances: " << model.missing_object_count << "\n\n"
           << "Resource search order:\n";
    for (const auto& location : model.resource_search_order) {
        output << "- " << location << "\n";
    }
    output << "\nMissing objects:\n";
    if (model.missing_objects.empty()) {
        output << "- None\n";
    } else {
        auto missing = model.missing_objects;
        std::stable_sort(missing.begin(), missing.end(), [](const auto& a, const auto& b) {
            return a.name < b.name;
        });
        for (const auto& object : missing) {
            std::string reason = object.reason;
            std::replace(reason.begin(), reason.end(), '\r', ' ');
            std::replace(reason.begin(), reason.end(), '\n', ' ');
            output << "- " << object.name
                   << " | instances=" << object.instance_count
                   << " | reason=" << reason << "\n";
        }
    }
    if (!output) throw std::runtime_error("cannot write the load log: " + path.string());
}

TrackViewModel load_and_log(const Options& options) {
    if (!options.input) throw std::runtime_error("track loading needs an input path");
    const fs::path log_path = options.log.value_or(default_log_path(*options.input));
    try {
        auto model = opennr::viewer::load_track_view(*options.input, options.shared_folder);
        write_load_log(log_path, *options.input, options.shared_folder, model);
        return model;
    } catch (const std::exception& error) {
        std::ofstream output(log_path, std::ios::binary | std::ios::trunc);
        if (output) {
            output << "OpenNR Track Viewer load log\n"
                   << "Track input: " << options.input->string() << "\n"
                   << "Shared folder: "
                   << (options.shared_folder ? options.shared_folder->string()
                                             : std::string("None")) << "\n"
                   << "Load error: " << error.what() << "\n";
        }
        throw;
    }
}

TrackViewModes parse_modes(std::string_view value) {
    TrackViewModes modes = 0;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto split = value.find_first_of("+,|", start);
        const std::string_view token = value.substr(
            start, split == std::string_view::npos ? value.size() - start : split - start);
        if (token == "solid") {
            modes |= opennr::viewer::track_view_mode(TrackViewMode::Solid);
        } else if (token == "wire" || token == "wireframe") {
            modes |= opennr::viewer::track_view_mode(TrackViewMode::Wireframe);
        } else if (token == "collision") {
            modes |= opennr::viewer::track_view_mode(TrackViewMode::Collision);
        } else if (token == "all") {
            modes |= opennr::viewer::track_view_mode(TrackViewMode::Solid) |
                     opennr::viewer::track_view_mode(TrackViewMode::Wireframe) |
                     opennr::viewer::track_view_mode(TrackViewMode::Collision);
        } else {
            throw std::runtime_error(
                "mode must contain solid, wire, collision, or all");
        }
        if (split == std::string_view::npos) break;
        start = split + 1;
    }
    return modes;
}

void parse_size(std::string_view value, int& width, int& height) {
    const auto split = value.find_first_of("xX");
    if (split == std::string_view::npos) throw std::runtime_error("size must use WIDTHxHEIGHT");
    width = std::stoi(std::string(value.substr(0, split)));
    height = std::stoi(std::string(value.substr(split + 1)));
    if (width < 64 || height < 64 || width > 8192 || height > 8192) {
        throw std::runtime_error("snapshot size must be from 64 through 8192 pixels");
    }
}

std::string mode_name(TrackViewModes modes) {
    std::string result;
    const auto append = [&](std::string_view name) {
        if (!result.empty()) result += " + ";
        result += name;
    };
    if (opennr::viewer::has_track_view_mode(modes, TrackViewMode::Solid)) {
        append("Solid");
    }
    if (opennr::viewer::has_track_view_mode(modes, TrackViewMode::Wireframe)) {
        append("Wireframe");
    }
    if (opennr::viewer::has_track_view_mode(modes, TrackViewMode::Collision)) {
        append("Collision wireframe");
    }
    return result.empty() ? "No view" : result;
}

void save_snapshot(const Options& options) {
    if (!options.input || !options.snapshot) {
        throw std::runtime_error("snapshot mode requires an output BMP and an input path");
    }
    const auto model = load_and_log(options);
    TrackCamera camera;
    camera.target = model.bounds.center();
    const auto frame = opennr::viewer::render_track_view(
        model, camera, options.modes, options.width, options.height);
    opennr::viewer::write_track_view_bmp(*options.snapshot, frame);
}

}  // namespace

#ifdef _WIN32

#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>

namespace {

constexpr wchar_t kWindowClass[] = L"OpenNRTrackViewerWindow";
constexpr UINT kOpenFolder = 1001;
constexpr UINT kOpenPtf = 1002;
constexpr UINT kExit = 1003;
constexpr UINT kSharedFolder = 1004;
constexpr UINT kClearSharedFolder = 1005;
constexpr UINT kSolid = 1101;
constexpr UINT kWireframe = 1102;
constexpr UINT kCollision = 1103;
constexpr UINT kResetView = 1104;

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(0, count)), L'\0');
    if (count > 0) {
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                            result.data(), count);
    }
    return result;
}

std::string narrow(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(0, count)), '\0');
    if (count > 0) {
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                            result.data(), count, nullptr, nullptr);
    }
    return result;
}

std::optional<fs::path> choose_path(HWND owner, bool folder,
                                    const wchar_t* title = nullptr) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                reinterpret_cast<void**>(&dialog)))) {
        return std::nullopt;
    }
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    flags |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder) {
        flags |= FOS_PICKFOLDERS;
        dialog->SetTitle(title ? title : L"Open a track folder");
    } else {
        flags |= FOS_FILEMUSTEXIST;
        const COMDLG_FILTERSPEC filters[] = {
            {L"Papyrus track files (*.ptf)", L"*.ptf"},
            {L"All files (*.*)", L"*.*"},
        };
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetFileTypeIndex(1);
        dialog->SetDefaultExtension(L"ptf");
        dialog->SetTitle(L"Open a PTF track file");
    }
    dialog->SetOptions(flags);

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

struct ViewerWindow {
    HWND window = nullptr;
    HMENU view_menu = nullptr;
    std::optional<TrackViewModel> model;
    std::optional<fs::path> input_path;
    std::optional<fs::path> shared_folder;
    std::optional<fs::path> requested_log_path;
    std::optional<fs::path> last_log_path;
    TrackCamera camera;
    TrackViewModes modes = opennr::viewer::track_view_mode(TrackViewMode::Solid);
    TrackFrame frame;
    bool frame_dirty = true;
    bool orbiting = false;
    bool panning = false;
    bool loading = false;
    POINT last_mouse{};

    void reset_camera() {
        camera = {};
        if (model) camera.target = model->bounds.center();
        mark_dirty();
    }

    void mark_dirty() {
        frame_dirty = true;
        if (window) InvalidateRect(window, nullptr, FALSE);
    }

    void update_menu() {
        if (!view_menu) return;
        const auto check = [&](UINT item, TrackViewMode mode) {
            CheckMenuItem(view_menu, item, MF_BYCOMMAND |
                (opennr::viewer::has_track_view_mode(modes, mode)
                    ? MF_CHECKED : MF_UNCHECKED));
        };
        check(kSolid, TrackViewMode::Solid);
        check(kWireframe, TrackViewMode::Wireframe);
        check(kCollision, TrackViewMode::Collision);
    }

    void toggle_mode(TrackViewMode mode) {
        const TrackViewModes bit = opennr::viewer::track_view_mode(mode);
        const TrackViewModes next = modes ^ bit;
        if (next == 0) return;
        modes = next;
        update_menu();
        mark_dirty();
    }

    void show_error(const std::exception& error) {
        MessageBoxW(window, widen(error.what()).c_str(), L"Track viewer error",
                    MB_OK | MB_ICONERROR);
    }

    void open(const fs::path& path) {
        loading = true;
        std::wstring loading_title = L"OpenNR Track Viewer - Loading ";
        loading_title += path.filename().wstring();
        SetWindowTextW(window, loading_title.c_str());
        InvalidateRect(window, nullptr, FALSE);
        UpdateWindow(window);
        HCURSOR previous_cursor = SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32514)));
        try {
            auto next = opennr::viewer::load_track_view(path, shared_folder);
            const fs::path load_log = requested_log_path.value_or(
                default_log_path(path));
            write_load_log(load_log, path, shared_folder, next);
            model = std::move(next);
            input_path = path;
            last_log_path = load_log;
            loading = false;
            reset_camera();
            std::wstring title = L"OpenNR Track Viewer - ";
            title += widen(model->source_description);
            SetWindowTextW(window, title.c_str());
        } catch (const std::exception& error) {
            loading = false;
            show_error(error);
        }
        SetCursor(previous_cursor);
        mark_dirty();
    }

    void set_shared_folder(const std::optional<fs::path>& path) {
        shared_folder = path;
        if (input_path) {
            open(*input_path);
        } else {
            mark_dirty();
        }
    }

    void draw_overlay(HDC dc, int width, int height) const {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 239, 242));
        RECT text_rect{14, 12, width - 12, height - 12};
        std::wstring text;
        if (loading) {
            text = L"Loading track resources...";
        } else if (!model) {
            text = L"Open a track folder or PTF file.\n\n"
                   L"File > Open Track Folder\n"
                   L"File > Open PTF File\n"
                   L"File > Select Shared Folder\n\n"
                   L"You can also drag a folder or PTF file into this window.";
        } else {
            text = widen(model->source_description) + L"\n";
            text += L"Shared folder: ";
            text += shared_folder ? shared_folder->wstring() : L"None";
            text += L"\n";
            if (last_log_path) {
                text += L"Load log: " + last_log_path->wstring() + L"\n";
            }
            text += widen(mode_name(modes));
            text += L"  |  Segments: " + std::to_wstring(model->segment_count);
            text += L"  Materials: " + std::to_wstring(model->material_count);
            text += L"  Walls: " + std::to_wstring(model->wall_count);
            text += L"  Objects: " + std::to_wstring(model->object_count);
            text += L"  Textures: " + std::to_wstring(model->textures.size());
            if (model->missing_object_count != 0) {
                text += L"  Missing objects: " +
                        std::to_wstring(model->missing_object_count);
            }
            text += L"\n1 Toggle solid   2 Toggle wireframe   3 Toggle collision   F Reset view\n"
                    L"Left drag Orbit   Right drag Pan   Mouse wheel Zoom";
        }
        DrawTextW(dc, text.c_str(), -1, &text_rect, DT_LEFT | DT_TOP | DT_NOPREFIX);
    }

    void paint() {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const int width = std::max(1L, client.right - client.left);
        const int height = std::max(1L, client.bottom - client.top);
        const bool moving = orbiting || panning;
        const int maximum_width = moving ? 720 : 1100;
        const int maximum_height = moving ? 450 : 700;
        const float render_scale = std::min({1.0f,
            static_cast<float>(maximum_width) / width,
            static_cast<float>(maximum_height) / height});
        const int render_width = std::max(1, static_cast<int>(width * render_scale));
        const int render_height = std::max(1, static_cast<int>(height * render_scale));
        if (model && (frame_dirty || frame.width != render_width ||
                      frame.height != render_height)) {
            try {
                frame = opennr::viewer::render_track_view(*model, camera, modes,
                                                          render_width, render_height);
                frame_dirty = false;
            } catch (const std::exception& error) {
                show_error(error);
                model.reset();
            }
        }
        if (model && !frame.pixels.empty()) {
            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = frame.width;
            bitmap.bmiHeader.biHeight = -frame.height;
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(dc, 0, 0, width, height, 0, 0, frame.width, frame.height,
                          frame.pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
        } else {
            HBRUSH brush = CreateSolidBrush(RGB(12, 16, 21));
            FillRect(dc, &client, brush);
            DeleteObject(brush);
        }
        draw_overlay(dc, width, height);
        EndPaint(window, &paint);
    }

    void pan(int dx, int dy) {
        if (!model) return;
        const float cp = std::cos(camera.pitch);
        const opennr::Vec3 direction{std::cos(camera.yaw) * cp,
                                     std::sin(camera.yaw) * cp,
                                     std::sin(camera.pitch)};
        const opennr::Vec3 forward = direction * -1.0f;
        auto right = opennr::cross(forward, {0.0f, 0.0f, 1.0f});
        const float right_length = right.length();
        if (right_length > 1.0e-6f) right = right * (1.0f / right_length);
        auto up = opennr::cross(right, forward);
        const float up_length = up.length();
        if (up_length > 1.0e-6f) up = up * (1.0f / up_length);
        RECT client{};
        GetClientRect(window, &client);
        const float scale = model->bounds.radius() /
            (std::max(100L, std::min(client.right, client.bottom)) * camera.zoom);
        camera.target = camera.target + right * (-dx * scale) + up * (dy * scale);
    }

    LRESULT message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_COMMAND:
                switch (LOWORD(wparam)) {
                    case kOpenFolder:
                        if (auto path = choose_path(window, true)) open(*path);
                        return 0;
                    case kOpenPtf:
                        if (auto path = choose_path(window, false)) open(*path);
                        return 0;
                    case kSharedFolder:
                        if (auto path = choose_path(
                                window, true, L"Select a shared resource folder")) {
                            set_shared_folder(*path);
                        }
                        return 0;
                    case kClearSharedFolder:
                        set_shared_folder(std::nullopt);
                        return 0;
                    case kExit: DestroyWindow(window); return 0;
                    case kSolid: toggle_mode(TrackViewMode::Solid); return 0;
                    case kWireframe: toggle_mode(TrackViewMode::Wireframe); return 0;
                    case kCollision: toggle_mode(TrackViewMode::Collision); return 0;
                    case kResetView: reset_camera(); return 0;
                }
                break;
            case WM_KEYDOWN:
                if (wparam == '1') toggle_mode(TrackViewMode::Solid);
                else if (wparam == '2') toggle_mode(TrackViewMode::Wireframe);
                else if (wparam == '3') toggle_mode(TrackViewMode::Collision);
                else if (wparam == 'F' || wparam == 'R') reset_camera();
                return 0;
            case WM_LBUTTONDOWN:
                orbiting = true;
                last_mouse = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                SetCapture(window);
                return 0;
            case WM_RBUTTONDOWN:
                panning = true;
                last_mouse = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                SetCapture(window);
                return 0;
            case WM_LBUTTONUP:
                orbiting = false;
                if (!panning) ReleaseCapture();
                mark_dirty();
                return 0;
            case WM_RBUTTONUP:
                panning = false;
                if (!orbiting) ReleaseCapture();
                mark_dirty();
                return 0;
            case WM_MOUSEMOVE: {
                const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                const int dx = point.x - last_mouse.x;
                const int dy = point.y - last_mouse.y;
                if (orbiting) {
                    camera.yaw -= dx * 0.008f;
                    camera.pitch = std::clamp(camera.pitch + dy * 0.008f,
                                              0.08f, 1.50f);
                    mark_dirty();
                }
                if (panning) {
                    pan(dx, dy);
                    mark_dirty();
                }
                last_mouse = point;
                return 0;
            }
            case WM_MOUSEWHEEL:
                camera.zoom *= std::pow(1.15f,
                    static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA);
                camera.zoom = std::clamp(camera.zoom, 0.08f, 20.0f);
                mark_dirty();
                return 0;
            case WM_DROPFILES: {
                HDROP drop = reinterpret_cast<HDROP>(wparam);
                wchar_t path[MAX_PATH]{};
                if (DragQueryFileW(drop, 0, path, MAX_PATH) > 0) open(path);
                DragFinish(drop);
                return 0;
            }
            case WM_SIZE: mark_dirty(); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: paint(); return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* viewer = reinterpret_cast<ViewerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        viewer = static_cast<ViewerWindow*>(create->lpCreateParams);
        viewer->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(viewer));
    }
    return viewer ? viewer->message(message, wparam, lparam)
                  : DefWindowProcW(window, message, wparam, lparam);
}

HMENU create_menu(ViewerWindow& viewer) {
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, kOpenFolder, L"Open Track &Folder...");
    AppendMenuW(file, MF_STRING, kOpenPtf, L"Open &PTF File...");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, kSharedFolder, L"Select &Shared Folder...");
    AppendMenuW(file, MF_STRING, kClearSharedFolder, L"&Clear Shared Folder");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, kExit, L"E&xit");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

    viewer.view_menu = CreatePopupMenu();
    AppendMenuW(viewer.view_menu, MF_STRING, kSolid, L"&Solid\t1");
    AppendMenuW(viewer.view_menu, MF_STRING, kWireframe, L"&Wireframe\t2");
    AppendMenuW(viewer.view_menu, MF_STRING, kCollision, L"&Collision wireframe\t3");
    AppendMenuW(viewer.view_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(viewer.view_menu, MF_STRING, kResetView, L"&Reset view\tF");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(viewer.view_menu), L"&View");
    viewer.update_menu();
    return menu;
}

Options parse_windows_options(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument = argv[index];
        if (argument == L"--snapshot") {
            if (++index >= argc) throw std::runtime_error("--snapshot needs an output BMP path");
            options.snapshot = fs::path(argv[index]);
        } else if (argument == L"--load") {
            options.load_only = true;
        } else if (argument == L"--log") {
            if (++index >= argc) throw std::runtime_error("--log needs a file path");
            options.log = fs::path(argv[index]);
        } else if (argument == L"--mode") {
            if (++index >= argc) throw std::runtime_error("--mode needs a value");
            options.modes = parse_modes(narrow(argv[index]));
        } else if (argument == L"--size") {
            if (++index >= argc) throw std::runtime_error("--size needs WIDTHxHEIGHT");
            parse_size(narrow(argv[index]), options.width, options.height);
        } else if (argument == L"--shared") {
            if (++index >= argc) throw std::runtime_error("--shared needs a folder path");
            options.shared_folder = fs::path(argv[index]);
        } else if (!argument.empty() && argument.front() == L'-') {
            throw std::runtime_error("unknown option " + narrow(argument));
        } else if (options.input) {
            throw std::runtime_error("only one input path is supported");
        } else {
            options.input = fs::path(argv[index]);
        }
    }
    return options;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    try {
        const Options options = parse_windows_options(argc, argv);
        LocalFree(argv);
        argv = nullptr;
        if (options.snapshot && options.load_only) {
            throw std::runtime_error("use --load or --snapshot, not both");
        }
        if (options.load_only) {
            const auto model = load_and_log(options);
            std::printf("loaded %s: %u objects, %u missing\n",
                        model.source_description.c_str(), model.object_count,
                        model.missing_object_count);
            CoUninitialize();
            return 0;
        }
        if (options.snapshot) {
            save_snapshot(options);
            CoUninitialize();
            return 0;
        }

        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&window_class)) throw std::runtime_error("cannot register the viewer window");

        ViewerWindow viewer;
        viewer.modes = options.modes;
        viewer.shared_folder = options.shared_folder;
        viewer.requested_log_path = options.log;
        HMENU menu = create_menu(viewer);
        HWND window = CreateWindowExW(
            WS_EX_ACCEPTFILES, kWindowClass, L"OpenNR Track Viewer",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
            nullptr, menu, instance, &viewer);
        if (!window) throw std::runtime_error("cannot create the viewer window");
        DragAcceptFiles(window, TRUE);
        ShowWindow(window, show);
        UpdateWindow(window);
        if (options.input) viewer.open(*options.input);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        CoUninitialize();
        return static_cast<int>(message.wParam);
    } catch (const std::exception& error) {
        if (argv) LocalFree(argv);
        MessageBoxW(nullptr, widen(error.what()).c_str(), L"Track viewer error",
                    MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
}

#else

namespace {

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--snapshot") {
            if (++index >= argc) throw std::runtime_error("--snapshot needs an output BMP path");
            options.snapshot = fs::path(argv[index]);
        } else if (argument == "--load") {
            options.load_only = true;
        } else if (argument == "--log") {
            if (++index >= argc) throw std::runtime_error("--log needs a file path");
            options.log = fs::path(argv[index]);
        } else if (argument == "--mode") {
            if (++index >= argc) throw std::runtime_error("--mode needs a value");
            options.modes = parse_modes(argv[index]);
        } else if (argument == "--size") {
            if (++index >= argc) throw std::runtime_error("--size needs WIDTHxHEIGHT");
            parse_size(argv[index], options.width, options.height);
        } else if (argument == "--shared") {
            if (++index >= argc) throw std::runtime_error("--shared needs a folder path");
            options.shared_folder = fs::path(argv[index]);
        } else if (!argument.empty() && argument.front() == '-') {
            throw std::runtime_error("unknown option " + std::string(argument));
        } else if (options.input) {
            throw std::runtime_error("only one input path is supported");
        } else {
            options.input = fs::path(argv[index]);
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.snapshot && options.load_only) {
            throw std::runtime_error("use --load or --snapshot, not both");
        }
        if ((!options.snapshot && !options.load_only) || !options.input) {
            std::fprintf(stderr,
                "usage: opennr_track_viewer (--load | --snapshot output.bmp) "
                "[--mode solid|wire|collision|solid+collision|all] "
                "[--size WIDTHxHEIGHT] "
                "[--shared shared-folder] "
                "[--log load-log] "
                "track-folder|file.ptf\n");
            return 2;
        }
        if (options.load_only) {
            const auto model = load_and_log(options);
            std::printf("loaded %s: %u objects, %u missing\n",
                        model.source_description.c_str(), model.object_count,
                        model.missing_object_count);
            return 0;
        }
        save_snapshot(options);
        std::printf("wrote %s (%s)\n", options.snapshot->string().c_str(),
                    mode_name(options.modes).c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}

#endif
