// samurai_warriors_2_empires - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include "native_renderer/sw2e_native_renderer.h"

#include <rex/rex_app.h>

class SamuraiWarriors2EmpiresApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<SamuraiWarriors2EmpiresApp>(new SamuraiWarriors2EmpiresApp(ctx, "samurai_warriors_2_empires",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  void OnPostSetup() override {
    sw2e::native_renderer::Install(window() ? window()->GetNativeWindowHandle() : nullptr);
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  void OnShutdown() override { sw2e::native_renderer::Shutdown(); }
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
