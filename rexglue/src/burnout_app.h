// burnout - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>

#include "online_lobby.h"
#include "plume_d3d.h"

class BurnoutApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<BurnoutApp>(new BurnoutApp(ctx, "burnout",
        PPCImageConfig));
  }

  void OnPostSetup() override {
    xerenge::plume_d3d::InstallPlumeRuntimeHooks();
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
