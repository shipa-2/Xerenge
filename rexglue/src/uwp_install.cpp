// UWP (the Xbox One in Dev Mode): the first run asks for the disc image and
// extracts it into the folder it is given. That folder is the developer
// scratch drive (D:\DevelopmentFiles) when the app can write there - the
// space Dev Home assigns - and LocalState\game only when it cannot. The
// image is picked with the system's file picker - a USB drive, on the
// console - and read through the handle the picker grants; the app may not
// open it by path. Only the European release: the game is built from its
// default.xex (the American one gets a build of its own on the desktop).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <WindowsStorageCOM.h>
#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Popups.h>

#include <rex/ui/uwp_game_setup.h>

#include "sha256.hpp"
#include "xdvdfs.hpp"

namespace {

using namespace std::chrono_literals;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;

// The American image's default.xex (installer_window.cpp).
constexpr const char* kXexSha256Us =
    "5258c75d9ae649ed4fb71d4e2fe8808545db16092cfe764ea1a2504f5aae2e2d";

// Waits for an operation on the UI thread, keeping its events going: the
// dialog and the picker are drawn by them.
template <typename Operation>
auto Wait(Operation operation) {
  auto dispatcher = CoreWindow::GetForCurrentThread().Dispatcher();
  while (operation.Status() == AsyncStatus::Started) {
    dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
    std::this_thread::sleep_for(10ms);
  }
  return operation.GetResults();
}

// True for OK, false for the second button (or none).
bool Ask(const wchar_t* text, const wchar_t* ok, const wchar_t* cancel) {
  winrt::Windows::UI::Popups::MessageDialog dialog(text, L"Xerenge");
  dialog.Commands().Append(winrt::Windows::UI::Popups::UICommand(ok, nullptr, winrt::box_value(1)));
  if (cancel) {
    dialog.Commands().Append(
        winrt::Windows::UI::Popups::UICommand(cancel, nullptr, winrt::box_value(0)));
  }
  dialog.DefaultCommandIndex(0);
  dialog.CancelCommandIndex(cancel ? 1 : 0);
  auto chosen = Wait(dialog.ShowAsync());
  return chosen && winrt::unbox_value<int>(chosen.Id()) == 1;
}

std::wstring Widen(const std::string& text) {
  return std::wstring(text.begin(), text.end());
}

// The installer's steps, in LocalState\logs\install.log: it runs before the
// game's own log is up.
void InstallLog(const std::string& line) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  const std::filesystem::path logs =
      std::filesystem::path(std::wstring(
          winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())) /
      "logs";
  std::error_code ec;
  std::filesystem::create_directories(logs, ec);
  std::ofstream out(logs / "install.log", std::ios::app);
  char stamp[32];
  std::snprintf(stamp, sizeof(stamp), "[%10.3f] ", double(GetTickCount64()) / 1000.0);
  out << stamp << line << "\n";
}

// Logs a failed step with its HRESULT; true when it succeeded.
bool Ok(const char* step, HRESULT result) {
  if (FAILED(result)) {
    char text[160];
    std::snprintf(text, sizeof(text), "install screen: %s failed, HRESULT %08lX", step,
                  static_cast<unsigned long>(result));
    InstallLog(text);
    return false;
  }
  return true;
}

// What the install screen shows; filled in by Extract as the copy goes.
struct InstallProgress {
  uint64_t done = 0;
  uint64_t total = 0;  // 0 while the image is still being listed
  double seconds_left = -1;  // below 0 until there is a rate to go on
  std::wstring file;
};

// The install progress as a pop-up in the console's dialog style: a card over
// the dimmed screen with a title, a thin green bar, the share copied and the
// time left. The system's own MessageDialog cannot hold a bar, and this app has
// no XAML for a ContentDialog, so it is drawn - Direct2D and DirectWrite on a
// swap chain on the CoreWindow. Gone, swap chain and all, before the game makes
// its own on the same window.
class InstallScreen {
 public:
  explicit InstallScreen(std::wstring where) : where_(std::move(where)) {
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    winrt::com_ptr<ID3D11Device> device;
    // Direct2D draws only on a device made with BGRA support.
    if (!Ok("D3D11CreateDevice",
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION,
                              device.put(), nullptr, context_.put()))) {
      return;
    }
    auto dxgi_device = device.as<IDXGIDevice>();
    // The swap chain must come from the factory that made the device's adapter.
    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::com_ptr<IDXGIFactory2> factory;
    if (!Ok("GetAdapter", dxgi_device->GetAdapter(adapter.put())) ||
        !Ok("adapter GetParent", adapter->GetParent(IID_PPV_ARGS(factory.put())))) {
      return;
    }
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = UINT(kWidth);
    desc.Height = UINT(kHeight);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.Scaling = DXGI_SCALING_STRETCH;
    auto window = CoreWindow::GetForCurrentThread();
    if (!Ok("CreateSwapChainForCoreWindow",
            factory->CreateSwapChainForCoreWindow(device.get(), winrt::get_unknown(window), &desc,
                                                  nullptr, swap_chain_.put()))) {
      swap_chain_ = nullptr;
      return;
    }

    winrt::com_ptr<ID2D1Factory1> d2d;
    D2D1_FACTORY_OPTIONS options = {};
    winrt::com_ptr<ID2D1Device> d2d_device;
    winrt::com_ptr<IDXGISurface> surface;
    // Buffer 0 of a flip-model swap chain is always the one being drawn, so the
    // bitmap made from it holds for every frame.
    const auto target_properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (!Ok("D2D1CreateFactory", D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                                   __uuidof(ID2D1Factory1), &options,
                                                   d2d.put_void())) ||
        !Ok("ID2D1Factory1::CreateDevice", d2d->CreateDevice(dxgi_device.get(), d2d_device.put())) ||
        !Ok("CreateDeviceContext",
            d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc_.put())) ||
        !Ok("swap chain GetBuffer", swap_chain_->GetBuffer(0, IID_PPV_ARGS(surface.put()))) ||
        !Ok("CreateBitmapFromDxgiSurface",
            dc_->CreateBitmapFromDxgiSurface(surface.get(), &target_properties, target_.put()))) {
      return;
    }
    dc_->SetTarget(target_.get());

    winrt::com_ptr<IDWriteFactory> dwrite;
    if (!Ok("DWriteCreateFactory",
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(dwrite.put()))) ||
        !MakeFormat(dwrite.get(), 32, DWRITE_FONT_WEIGHT_SEMI_BOLD, title_) ||
        !MakeFormat(dwrite.get(), 22, DWRITE_FONT_WEIGHT_NORMAL, body_) ||
        !MakeFormat(dwrite.get(), 22, DWRITE_FONT_WEIGHT_NORMAL, body_right_) ||
        !MakeFormat(dwrite.get(), 18, DWRITE_FONT_WEIGHT_NORMAL, small_)) {
      return;
    }
    body_right_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (!Ok("brushes",
            dc_->CreateSolidColorBrush(D2D1::ColorF(0xF2F2F2), white_.put())) ||
        !Ok("brushes", dc_->CreateSolidColorBrush(D2D1::ColorF(0xA6A6A6), grey_.put())) ||
        !Ok("brushes", dc_->CreateSolidColorBrush(D2D1::ColorF(0x2B2B2B), card_.put())) ||
        !Ok("brushes", dc_->CreateSolidColorBrush(D2D1::ColorF(0x474747), track_.put())) ||
        !Ok("brushes", dc_->CreateSolidColorBrush(D2D1::ColorF(0x107C10), fill_.put()))) {
      return;
    }
    ready_ = true;
    InstallLog("install screen: ready");
  }

  ~InstallScreen() {
    if (dc_) {
      dc_->SetTarget(nullptr);
    }
    target_ = nullptr;
    dc_ = nullptr;
    swap_chain_ = nullptr;
    if (context_) {
      context_->ClearState();
      context_->Flush();
    }
  }

  void Draw(const InstallProgress& progress) {
    if (!ready_) {
      return;
    }
    constexpr float card_left = kWidth / 2 - kCardWidth / 2, card_right = kWidth / 2 + kCardWidth / 2;
    constexpr float card_top = kHeight / 2 - kCardHeight / 2, card_bottom = card_top + kCardHeight;
    constexpr float left = card_left + kPadding, right = card_right - kPadding;
    constexpr float title_top = card_top + kPadding;
    constexpr float bar_top = title_top + 120, bar_bottom = bar_top + 6;
    dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(0x0A0A0A));
    dc_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(card_left, card_top, card_right, card_bottom), 8, 8),
        card_.get());
    Text(L"Installing Burnout Revenge", title_.get(), white_.get(), left, title_top, right,
         title_top + 44);
    Text(where_, body_.get(), grey_.get(), left, title_top + 54, right, title_top + 86);

    dc_->FillRectangle(D2D1::RectF(left, bar_top, right, bar_bottom), track_.get());
    std::wstring status, detail;
    if (progress.total == 0) {
      status = L"Preparing\u2026";
    } else {
      const double fraction = std::clamp(double(progress.done) / double(progress.total), 0.0, 1.0);
      if (fraction > 0) {
        dc_->FillRectangle(
            D2D1::RectF(left, bar_top, left + float((right - left) * fraction), bar_bottom),
            fill_.get());
      }
      status = std::to_wstring(int(fraction * 100)) + L"%";
      wchar_t amount[64];
      std::swprintf(amount, 64, L"%.1f GB of %.1f GB", double(progress.done) / double(1ull << 30),
                    double(progress.total) / double(1ull << 30));
      detail = amount;
      if (progress.seconds_left >= 0) {
        detail += L"  \u00B7  " + TimeLeft(progress.seconds_left);
      }
    }
    Text(status, body_.get(), white_.get(), left, bar_top + 22, right, bar_top + 54);
    Text(detail, body_right_.get(), grey_.get(), left, bar_top + 22, right, bar_top + 54);
    Text(progress.file, small_.get(), grey_.get(), left, bar_top + 64, right, bar_top + 92);

    if (!Ok("EndDraw", dc_->EndDraw())) {
      ready_ = false;
      return;
    }
    const HRESULT presented = swap_chain_->Present(1, 0);
    if (!Ok("Present", presented)) {
      ready_ = false;
      return;
    }
    if (++frames_ == 1) {
      InstallLog("install screen: first frame presented");
    }
  }

 private:
  static constexpr float kWidth = 1920, kHeight = 1080;
  static constexpr float kCardWidth = 960, kCardHeight = 312, kPadding = 48;

  static bool MakeFormat(IDWriteFactory* dwrite, float size, DWRITE_FONT_WEIGHT weight,
                         winrt::com_ptr<IDWriteTextFormat>& format) {
    if (!Ok("CreateTextFormat",
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", format.put()))) {
      return false;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return true;
  }

  static std::wstring TimeLeft(double seconds) {
    if (seconds < 60) {
      return L"less than a minute left";
    }
    const int minutes = int(seconds / 60 + 0.5);
    return L"about " + std::to_wstring(minutes) + (minutes == 1 ? L" minute left" : L" minutes left");
  }

  void Text(const std::wstring& text, IDWriteTextFormat* format, ID2D1Brush* brush, float left,
            float top, float right, float bottom) {
    if (!text.empty()) {
      dc_->DrawText(text.c_str(), UINT32(text.size()), format,
                    D2D1::RectF(left, top, right, bottom), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
  }

  std::wstring where_;
  bool ready_ = false;
  uint64_t frames_ = 0;
  winrt::com_ptr<ID3D11DeviceContext> context_;
  winrt::com_ptr<IDXGISwapChain1> swap_chain_;
  winrt::com_ptr<ID2D1DeviceContext> dc_;
  winrt::com_ptr<ID2D1Bitmap1> target_;
  winrt::com_ptr<IDWriteTextFormat> title_, body_, body_right_, small_;
  winrt::com_ptr<ID2D1SolidColorBrush> white_, grey_, card_, track_, fill_;
};

// Extracts the image behind `handle` into `destination`, drawing the progress.
// Returns the error, or empty.
std::string Extract(HANDLE handle, const std::filesystem::path& destination) {
  std::string error;
  std::atomic<uint64_t> bytes_total{0};
  std::atomic<bool> finished{false};
  // The file being written, and the bytes in the files before it: the
  // extractor speaks only as each file begins, so the screen reads how far
  // the current one has grown on disk.
  std::mutex current_mutex;
  std::string current_path;
  uint64_t current_base = 0, current_length = 0;
  const std::string root = destination.string();

  std::thread worker([&] {
    try {
      XdvdfsImage image(handle);
      uint32_t root_sector = 0, root_size = 0;
      image.Root(&root_sector, &root_size);
      uint64_t total = 0;
      XdvdfsExtract(image, root_sector, root_size, root, true,
                    [&](const std::string&, uint64_t length) { total += length; });
      bytes_total = total;
      uint64_t started = 0;
      XdvdfsExtract(image, root_sector, root_size, root, false,
                    [&](const std::string& path, uint64_t length) {
                      if (!path.empty() && path.back() == '/') {
                        return;
                      }
                      std::lock_guard<std::mutex> lock(current_mutex);
                      current_path = path;
                      current_base = started;
                      current_length = length;
                      started += length;
                    });
      std::lock_guard<std::mutex> lock(current_mutex);
      current_path.clear();
      current_base = started;
      current_length = 0;
    } catch (const std::exception& e) {
      error = e.what();
    }
    finished = true;
  });

  const bool on_scratch = destination.root_name() == L"D:" || destination.root_name() == L"d:";
  {
    InstallScreen screen(on_scratch ? L"Copying the disc to the developer drive"
                                    : L"Copying the disc to the app's storage");
    auto dispatcher = CoreWindow::GetForCurrentThread().Dispatcher();
    // The time left is reckoned from when copying starts, not from the listing.
    std::chrono::steady_clock::time_point began;
    InstallProgress progress;
    while (!finished) {
      dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
      std::string path;
      uint64_t base, length;
      {
        std::lock_guard<std::mutex> lock(current_mutex);
        path = current_path;
        base = current_base;
        length = current_length;
      }
      uint64_t written = 0;
      if (!path.empty()) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        written = ec ? 0 : std::min<uint64_t>(size, length);
      }
      if (!progress.total && bytes_total) {
        began = std::chrono::steady_clock::now();
      }
      progress.total = bytes_total;
      progress.done = std::max(progress.done, base + written);
      progress.file = path.size() > root.size() ? Widen(path.substr(root.size() + 1)) : L"";
      const double elapsed =
          progress.total
              ? std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count()
              : 0;
      progress.seconds_left =
          progress.total && progress.done && elapsed > 5
              ? double(progress.total - std::min(progress.done, progress.total)) *
                    elapsed / double(progress.done)
              : -1;
      screen.Draw(progress);
      std::this_thread::sleep_for(50ms);
    }
  }
  worker.join();
  return error;
}

// The settings file the desktop installer writes (installer_window.cpp,
// WriteSettings), with its defaults, beside the game: the game reads it there
// (xerenge_settings.cpp) and its menu writes into it. One already there - from
// an earlier install, or written by hand - is left as it is.
void WriteInitialSettings(const std::filesystem::path& path) {
  if (std::filesystem::exists(path)) {
    return;
  }
  const std::filesystem::path partial = path.string() + ".part";
  {
    std::ofstream out(partial, std::ios::trunc);
    out << "# Burnout Revenge (Xerenge) settings, read by the launcher at every start.\n"
           "# true or false\n"
           "bloom = false\n"
           "motion_blur = false\n"
           "# plume (this project's renderer) or xenos (Xenia's)\n"
           "renderer = plume\n";
    if (!out) {
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(partial, path, ec);
  if (ec) {
    std::filesystem::remove(partial, ec);
  }
}

bool InstallGame(const std::filesystem::path& game_dir) {
  InstallLog("install: no game found; installing into " + game_dir.string());
  for (;;) {
    if (!Ask(L"Xerenge needs the Burnout Revenge disc image (the European release, .iso). "
             L"Put it on a USB drive and choose it next; the game is copied out of it once.",
             L"Choose the image", L"Quit")) {
      return false;
    }
    winrt::Windows::Storage::Pickers::FileOpenPicker picker;
    picker.ViewMode(winrt::Windows::Storage::Pickers::PickerViewMode::List);
    picker.SuggestedStartLocation(
        winrt::Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
    picker.FileTypeFilter().Append(L".iso");
    winrt::Windows::Storage::StorageFile file = Wait(picker.PickSingleFileAsync());
    if (!file) {
      continue;
    }

    // Room for the game on the volume it is extracted to. Only the scratch
    // drive (D:) grows with Dev Home's "Manage developer storage"; LocalState
    // keeps its few GB whatever that is set to.
    const uint64_t needed = uint64_t(Wait(file.GetBasicPropertiesAsync()).Size()) + (256ull << 20);
    const bool on_scratch = game_dir.root_name() == L"D:" || game_dir.root_name() == L"d:";
    std::error_code space_ec;
    std::filesystem::create_directories(game_dir.parent_path(), space_ec);
    ULARGE_INTEGER free_bytes{};
    const bool have_space =
        GetDiskFreeSpaceExW(game_dir.parent_path().c_str(), &free_bytes, nullptr, nullptr);
    InstallLog("install: needs " + std::to_string(needed) + " bytes; " +
               (have_space ? std::to_string(free_bytes.QuadPart) + " free"
                           : "free space unknown (error " + std::to_string(GetLastError()) + ")") +
               " on " + game_dir.parent_path().string());
    if (have_space && free_bytes.QuadPart < needed) {
      const auto gb = [](uint64_t bytes) { return std::to_wstring((bytes * 10 >> 30) / 10) + L"." +
                                                  std::to_wstring((bytes * 10 >> 30) % 10); };
      const wchar_t* advice =
          on_scratch ? L" GB is free on the developer drive. On the console, open Dev Home and give "
                       L"developer storage more space (Manage developer storage), then choose the "
                       L"image again."
                     : L" GB is free in the app's own storage, which developer storage does not "
                       L"enlarge: Xerenge could not write to D:\\DevelopmentFiles.";
      Ask((L"There is not room for the game: it needs " + gb(needed) + L" GB, and " +
           gb(free_bytes.QuadPart) + advice).c_str(),
          L"Back", nullptr);
      continue;
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    auto access = file.as<IStorageItemHandleAccess>();
    if (FAILED(access->Create(HAO_READ, HSO_SHARE_READ, HO_NONE, nullptr, &handle)) ||
        handle == INVALID_HANDLE_VALUE) {
      Ask(L"The image could not be opened.", L"Back", nullptr);
      continue;
    }
    const std::filesystem::path partial = game_dir.parent_path() / "game.part";
    std::error_code ec;
    std::filesystem::remove_all(partial, ec);
    InstallLog("install: extracting into " + partial.string());
    std::string error = Extract(handle, partial);
    CloseHandle(handle);
    InstallLog("install: extraction " + (error.empty() ? std::string("finished") : "failed: " + error));

    if (error.empty() && !std::filesystem::exists(partial / "default.xex")) {
      error = "it has no default.xex - not Burnout Revenge?";
    }
    if (error.empty()) {
      std::string digest;
      if (Sha256FileHex((partial / "default.xex").string(), &digest, nullptr) &&
          digest == kXexSha256Us) {
        error = "it is the American release; this build is made from the European one";
      }
    }
    if (!error.empty()) {
      std::filesystem::remove_all(partial, ec);
      Ask((L"The image could not be used: " + Widen(error)).c_str(), L"Back", nullptr);
      continue;
    }
    std::filesystem::remove_all(game_dir, ec);
    std::filesystem::rename(partial, game_dir, ec);
    if (ec) {
      Ask(L"The extracted game could not be moved into place.", L"Back", nullptr);
      continue;
    }
    WriteInitialSettings(game_dir.parent_path() / "xerenge.conf");
    InstallLog("install: done; settings in " + (game_dir.parent_path() / "xerenge.conf").string() +
               (std::filesystem::exists(game_dir.parent_path() / "xerenge.conf") ? "" : " (NOT written)"));
    return true;
  }
}

struct Register {
  Register() { rex::ui::SetUwpGameSetup(InstallGame); }
} register_install;

}  // namespace
