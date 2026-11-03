// UWP (the Xbox One in Dev Mode): the first run asks for the disc image and
// extracts it into LocalState\game, as the desktop installer does into its
// own folder. The image is picked with the system's file picker - a USB drive,
// on the console - and read through the handle the picker grants; the app may
// not open it by path. Only the European release: the game is built from its
// default.xex (the American one gets a build of its own on the desktop).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
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
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
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

// A bar across the screen while the image is extracted: D3D11 on the
// CoreWindow, cleared in rectangles - nothing else is up yet to draw with.
// Gone, swap chain and all, before the game makes its own on the same window.
class ProgressBar {
 public:
  ProgressBar() {
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1,
                                 D3D11_SDK_VERSION, device.put(), nullptr, context.put()))) {
      return;
    }
    device_ = device;
    context_ = context.as<ID3D11DeviceContext1>();
    winrt::com_ptr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
      return;
    }
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.Scaling = DXGI_SCALING_STRETCH;
    auto window = CoreWindow::GetForCurrentThread();
    if (FAILED(factory->CreateSwapChainForCoreWindow(
            device_.get(), winrt::get_unknown(window), &desc, nullptr, swap_chain_.put()))) {
      swap_chain_ = nullptr;
    }
  }

  ~ProgressBar() {
    target_ = nullptr;
    swap_chain_ = nullptr;
    if (context_) {
      context_->ClearState();
      context_->Flush();
    }
  }

  void Draw(double fraction) {
    if (!swap_chain_) {
      return;
    }
    winrt::com_ptr<ID3D11Texture2D> buffer;
    swap_chain_->GetBuffer(0, IID_PPV_ARGS(buffer.put()));
    target_ = nullptr;
    device_->CreateRenderTargetView(buffer.get(), nullptr, target_.put());
    const float background[4] = {0.06f, 0.06f, 0.06f, 1.0f};
    const float track[4] = {0.2f, 0.2f, 0.2f, 1.0f};
    const float fill[4] = {0.95f, 0.45f, 0.1f, 1.0f};
    context_->ClearView(target_.get(), background, nullptr, 0);
    const LONG left = kWidth / 8, right = kWidth - kWidth / 8;
    const LONG top = kHeight / 2 - 12, bottom = kHeight / 2 + 12;
    const D3D11_RECT whole = {left, top, right, bottom};
    context_->ClearView(target_.get(), track, &whole, 1);
    const LONG done = left + LONG((right - left) * std::clamp(fraction, 0.0, 1.0));
    if (done > left) {
      const D3D11_RECT part = {left, top, done, bottom};
      context_->ClearView(target_.get(), fill, &part, 1);
    }
    swap_chain_->Present(1, 0);
  }

 private:
  static constexpr UINT kWidth = 1920, kHeight = 1080;
  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11DeviceContext1> context_;
  winrt::com_ptr<IDXGISwapChain1> swap_chain_;
  winrt::com_ptr<ID3D11RenderTargetView> target_;
};

// Extracts the image behind `handle` into `destination`, drawing the progress.
// Returns the error, or empty.
std::string Extract(HANDLE handle, const std::filesystem::path& destination) {
  std::string error;
  std::atomic<int> files_done{0};
  std::atomic<int> files_total{0};
  std::atomic<bool> finished{false};
  std::thread worker([&] {
    try {
      XdvdfsImage image(handle);
      uint32_t root_sector = 0, root_size = 0;
      image.Root(&root_sector, &root_size);
      int total = 0;
      XdvdfsExtract(image, root_sector, root_size, destination.string(), true,
                    [&](const std::string& path, uint64_t) {
                      if (!path.empty() && path.back() != '/') ++total;
                    });
      files_total = total;
      XdvdfsExtract(image, root_sector, root_size, destination.string(), false,
                    [&](const std::string& path, uint64_t) {
                      if (!path.empty() && path.back() != '/') ++files_done;
                    });
    } catch (const std::exception& e) {
      error = e.what();
    }
    finished = true;
  });
  {
    ProgressBar bar;
    auto dispatcher = CoreWindow::GetForCurrentThread().Dispatcher();
    while (!finished) {
      dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
      const int total = files_total;
      bar.Draw(total ? double(files_done) / total : 0.0);
      std::this_thread::sleep_for(50ms);
    }
  }
  worker.join();
  return error;
}

std::wstring Widen(const std::string& text) {
  return std::wstring(text.begin(), text.end());
}

bool InstallGame(const std::filesystem::path& game_dir) {
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
    std::string error = Extract(handle, partial);
    CloseHandle(handle);

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
    return true;
  }
}

struct Register {
  Register() { rex::ui::SetUwpGameSetup(InstallGame); }
} register_install;

}  // namespace
