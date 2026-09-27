#pragma once

#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include "ealobby/server.h"
#include "plume_d3d.h"

REXCVAR_DECLARE(bool, online);
REXCVAR_DECLARE(bool, online_fake_lobby);
REXCVAR_DECLARE(std::string, lobby_server);

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

    if (REXCVAR_GET(online) && !REXCVAR_GET(online_fake_lobby)) {
      const std::string host = REXCVAR_GET(lobby_server);
      if (host.empty() || host == "127.0.0.1" || host == "localhost" || host == "0.0.0.0") {
        ealobby::Options options;
        options.host = "0.0.0.0";
        options.directory_port = 31860;
        options.lobby_port = 31861;
        lobby_server_ = std::make_unique<ealobby::Server>(
            options, [](const std::string& line) {
              REXLOG_INFO("[ealobby] {}", line);
            });
        if (lobby_server_->Start()) {
          REXLOG_INFO("--online: Embedded ealobby server started on ports 31860/31861");
        } else {
          REXLOG_WARN("--online: Embedded ealobby server failed to start (ports in use?); proceeding with external server");
          lobby_server_.reset();
        }
      }
    }
  }

  void OnShutdown() override {
    if (lobby_server_) {
      REXLOG_INFO("--online: Stopping embedded ealobby server");
      lobby_server_->Stop();
      lobby_server_.reset();
    }
  }

 private:
  std::unique_ptr<ealobby::Server> lobby_server_;
};
