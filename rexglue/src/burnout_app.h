// burnout - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/logging.h>
#include <rex/rex_app.h>

#include "online_lobby.h"
#include "plume_d3d.h"
#include "xerenge_settings.h"

void XerengeAchievementToastStart();

class BurnoutApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<BurnoutApp>(new BurnoutApp(ctx, "burnout",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig&) override { xerenge::settings::ApplyAtStart(); }

  // Which build this is, first in the log: a report then says it by itself.
  void OnPostInitLogging() override { REXLOG_INFO("Xerenge {} ({})", XERENGE_VERSION, XERENGE_COMMIT); }

  void OnPostSetup() override {
    XerengeAchievementToastStart();
    xerenge::plume_d3d::InstallPlumeRuntimeHooks();
    // No mouse of our own: the arrow over the game is a distraction (issue
    // 104). Hidden outright, not auto-hidden: auto-hide brings the cursor back
    // on every mouse motion, and a pad that emits pointer events (its gyro or
    // touchpad) would keep it on screen for the whole session.
    if (window()) {
      window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
      REXLOG_INFO("xerenge: cursor hidden over the game window");
    }
    // --online: this copy's lobby is up (and heard on the LAN) from the start,
    // not only once its own player signs in.
    xerenge::online::StartLobbyNetwork();
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
