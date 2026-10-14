// fm3 - Forza Motorsport 3 recompiled for PC.

#pragma once

#include <rex/rex_app.h>

class Fm3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  // Defined in main.cpp, the only TU that sees the generated PPCImageConfig.
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

 protected:
  // Loads fm3.toml and fm3.user.toml (the launcher's) and takes the game and
  // user-data folders from them.
  void OnConfigurePaths(rex::PathConfig& paths) override;
  // Installs the crash handler (logs/crash_*.txt + .dmp next to the exe).
  void OnPostInitLogging() override;
  // Loads the xenos GPU plugin with the backend chosen in the launcher.
  void OnPreSetup(rex::RuntimeConfig& config) override;
  // Recomputes the protected-file signature table from the files on disk, so
  // modified data files (Project Forza Plus 3) are accepted.
  void OnPostLoadXexImage() override;
  // Signs in the profile selected in the launcher (--profile_name / --profile_xuid).
  void OnPostSetup() override;
  // No achievement toasts. While the ImGui drawer has any dialog registered
  // (this one stays registered even with nothing on screen) the presenter
  // repaints the window on its own at the monitor's refresh rate or faster,
  // out of step with the game's 60 frames: high "FPS" and visible judder.
  // Without it the window is presented once per game frame (same as the FM2
  // recompilation).
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override;
};
