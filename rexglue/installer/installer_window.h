#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>

class QCheckBox;
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
  bool shortcuts_ = true;
  QString log_path_;
  bool unattended_ = false;
};
