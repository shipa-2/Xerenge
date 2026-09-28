#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProcess;
class QProgressBar;
class QPushButton;

// Installs Burnout Revenge from a disc image: checks the image against the
// retail release and extracts it, copies the game that came built with the
// installer (its payload) into the chosen directory, writes the settings the
// launcher reads at every start, and creates a desktop shortcut and a menu
// entry. Nothing is compiled on this machine.
class InstallerWindow : public QWidget {
  Q_OBJECT

 public:
  explicit InstallerWindow(QWidget* parent = nullptr);
  ~InstallerWindow() override;

  // Without the window: fill the fields and install at once, quitting with
  // the result. `shortcuts` false leaves the desktop and the menu alone.
  void InstallUnattended(const QString& image, const QString& path, bool bloom, bool blur,
                         bool xenia, bool shortcuts);

 private:
  struct Step {
    QString name;
    int weight = 1;
    std::function<void()> run;
  };

  void BrowseImage();
  void BrowseInstallPath();
  // An installed copy at the install path turns Install into Update: the disc
  // image is not needed, and its xerenge.conf settings are shown to keep.
  void RefreshInstallState();
  void LoadSettings(const QString& path);
  void StartInstall();
  void Refuse(const QString& message);
  void RunNextStep();
  void StepFinished(const QString& error);
  void SetStepProgress(double fraction);
  void SetStatus(const QString& text);
  void SetBusy(bool busy);

  // A step that is a program: its output goes through `on_line`, which may
  // report progress; a non-zero exit fails the step.
  void RunProcess(const QString& program, const QStringList& arguments,
                  std::function<void(const QString&)> on_line,
                  const QString& working_directory = {});
  // A step that is work on another thread; it returns an error or nothing.
  void RunInBackground(std::function<QString(std::function<void(double)>)> work);

  bool LocatePayload(QString* error);
  QString CopyRuntime(const std::function<void(double)>& progress) const;
  QString WriteSettings() const;
  QString CreateShortcuts() const;

  QLineEdit* image_edit_ = nullptr;
  QCheckBox* bloom_box_ = nullptr;
  QCheckBox* blur_box_ = nullptr;
  QCheckBox* xenia_box_ = nullptr;
  // The language set ahead of time (XERENGE_LANGUAGE), windowed, the hacks.
  QComboBox* language_combo_ = nullptr;
  QCheckBox* windowed_box_ = nullptr;
  // Debug mode: the launcher logs what the debug_* lines of xerenge.conf ask.
  QCheckBox* debug_box_ = nullptr;
  // Those lines as an installed copy had them, kept through an update.
  QString preserved_debug_settings_;
  QCheckBox* async_box_ = nullptr;
  QCheckBox* early_submit_box_ = nullptr;
  QComboBox* resolution_combo_ = nullptr;
  QCheckBox* fps_box_ = nullptr;
  // The keyboard as a pad: on or off, and a key per pad control (keybind_*).
  QCheckBox* keyboard_box_ = nullptr;
  QList<QPair<QString, QLineEdit*>> key_edits_;
  QLineEdit* path_edit_ = nullptr;
  QProgressBar* progress_ = nullptr;
  QLabel* status_ = nullptr;
  QPushButton* install_button_ = nullptr;

  QList<Step> steps_;
  int step_index_ = 0;
  int weight_done_ = 0;
  int weight_total_ = 1;
  QProcess* process_ = nullptr;

  // Resolved when an installation starts.
  QString image_path_;
  QString install_dir_;
  // What ships with the installer: bin/ (the game and its libraries) and
  // tools/extract-image.
  QString payload_dir_;
  bool bloom_ = false;
  bool blur_ = false;
  bool xenia_ = false;
  // The xerenge.conf lines for everything above but bloom, blur and renderer.
  QString extra_settings_;
  bool shortcuts_ = true;
  QString log_path_;
  bool unattended_ = false;
};
