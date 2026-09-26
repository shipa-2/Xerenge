#include "installer_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace {

// The retail European image the recompiled code was made from (see SETUP.md).
constexpr const char* kRetailSha256 =
    "34c1bd4d549c2c53f29d814fa5e5d1c04c533c5ca0c39e57b6c2538f44ff59b4";
// Files extract-image writes for that image, for its progress.
constexpr int kImageFiles = 832;

constexpr const char* kDesktopId = "xerenge-burnout-revenge.desktop";

// Where the AppImage fetches the project from. Everything else (the SDK,
// plume, the shader translator) is fetched by the project's own scripts.
constexpr const char* kRepository = "https://github.com/shipa-2/Xerenge.git";

// The environment for the tools the installer runs (git, cmake, clang, the
// scripts): the system's, without what an AppImage sets up for itself - its
// libraries and Qt plugins are not meant for them.
QProcessEnvironment ToolEnvironment() {
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  for (const char* name : {"LD_LIBRARY_PATH", "LD_PRELOAD", "QT_PLUGIN_PATH", "QML2_IMPORT_PATH",
                           "QT_QPA_PLATFORM_PLUGIN_PATH", "PYTHONHOME", "PYTHONPATH"}) {
    env.remove(name);
  }
  // Restore what the AppImage runtime saved, if it did.
  if (env.contains("APPIMAGE_ORIGINAL_LD_LIBRARY_PATH")) {
    env.insert("LD_LIBRARY_PATH", env.value("APPIMAGE_ORIGINAL_LD_LIBRARY_PATH"));
  }
  return env;
}

// Started from the shortcut. Reads the settings the installer wrote - so they
// can be changed later by editing xerenge.conf - and starts the game the way
// run.sh does.
constexpr const char* kLauncher = R"SH(#!/bin/sh
# Burnout Revenge (Xerenge). Written by the installer; the settings are in
# xerenge.conf beside this file and are read at every start.
dir=$(cd "$(dirname "$0")" && pwd)
conf="$dir/xerenge.conf"
value() {
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$conf" 2>/dev/null | tail -n 1
}

[ "$(value bloom)" = true ] || export XERENGE_NO_BLOOM=1
[ "$(value motion_blur)" = true ] || export XERENGE_NO_MOTION_BLUR=1

renderer=$(value renderer)
if [ "$renderer" != xenos ]; then
    renderer=plume
    export XERENGE_D3D_TARGETS=1 XERENGE_D3D_UI=1 XERENGE_SKIP_LOGOS=1 \
           XERENGE_REAL_SHADERS=1 XERENGE_D3D_DRAWS=1 XERENGE_SECONDARY_TICKS=1
fi

export SDL_VIDEODRIVER=x11
export LD_LIBRARY_PATH="$dir/bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
logs="${XDG_STATE_HOME:-$HOME/.local/state}/xerenge-burnout"
mkdir -p "$logs"

# The renderer reads shader sources relative to the working directory.
cd "$dir"
exec "$dir/bin/burnout" \
    --game_data_root "$dir/game" \
    --gpu_plugin "$renderer" \
    --gpu_backend "$renderer" \
    --no-vulkan_async_skip_incomplete_frames \
    --log_level info \
    --log_file "$logs/burnout.log" \
    --log_max_file_size_mb 32 \
    --log_max_files 3 \
    "$@"
)SH";

QString DefaultInstallDir() {
  return QDir::home().filePath("Games/Burnout Revenge");
}

// The first of `candidates` that exists, or an empty string.
QString FirstExisting(const QStringList& candidates) {
  for (const QString& path : candidates) {
    if (QFileInfo::exists(path)) {
      return QDir(path).absolutePath();
    }
  }
  return {};
}

QString CopyFile(const QString& from, const QString& to) {
  QFile::remove(to);
  if (!QFile::copy(from, to)) {
    return QObject::tr("Could not copy %1 to %2").arg(from, to);
  }
  // Keep the permissions: the game and its libraries must stay executable.
  QFile::setPermissions(to, QFile::permissions(from));
  return {};
}

bool WriteText(const QString& path, const QString& text, bool executable) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
    return false;
  }
  file.write(text.toUtf8());
  file.close();
  if (executable) {
    file.setPermissions(file.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup |
                        QFileDevice::ExeOther);
  }
  return true;
}

}  // namespace

InstallerWindow::InstallerWindow(QWidget* parent) : QWidget(parent) {
  setWindowTitle(tr("Burnout Revenge - Xerenge installer"));
  setMinimumWidth(520);

  auto* layout = new QVBoxLayout(this);

  // The disc image.
  layout->addWidget(new QLabel(tr("Select game ISO")));
  auto* image_row = new QHBoxLayout;
  image_edit_ = new QLineEdit;
  image_edit_->setPlaceholderText(tr("Burnout Revenge (Europe) disc image, .iso"));
  auto* image_browse = new QToolButton;
  image_browse->setText("...");
  image_row->addWidget(image_edit_);
  image_row->addWidget(image_browse);
  layout->addLayout(image_row);
  connect(image_browse, &QToolButton::clicked, this, &InstallerWindow::BrowseImage);

  layout->addSpacing(12);

  // Off unless ticked.
  bloom_box_ = new QCheckBox(tr("Bloom on"));
  blur_box_ = new QCheckBox(tr("Blur on"));
  xenia_box_ = new QCheckBox(tr("Xenia render"));
  bloom_box_->setToolTip(tr("Sky bloom and the low-resolution gaussian blur."));
  blur_box_->setToolTip(tr("Motion blur and radial blur."));
  xenia_box_->setToolTip(tr("Draw with the Xenos backend (Xenia's renderer) instead of plume."));
  layout->addWidget(bloom_box_);
  layout->addWidget(blur_box_);
  layout->addWidget(xenia_box_);

  layout->addSpacing(12);

  // Where it goes.
  layout->addWidget(new QLabel(tr("Install path")));
  auto* path_row = new QHBoxLayout;
  path_edit_ = new QLineEdit(DefaultInstallDir());
  auto* path_browse = new QToolButton;
  path_browse->setText("...");
  path_row->addWidget(path_edit_);
  path_row->addWidget(path_browse);
  layout->addLayout(path_row);
  connect(path_browse, &QToolButton::clicked, this, &InstallerWindow::BrowseInstallPath);

  layout->addSpacing(12);

  progress_ = new QProgressBar;
  progress_->setRange(0, 1000);
  progress_->setValue(0);
  progress_->setTextVisible(false);
  layout->addWidget(progress_);

  status_ = new QLabel(tr("Ready."));
  status_->setAlignment(Qt::AlignCenter);
  status_->setWordWrap(true);
  layout->addWidget(status_);

  install_button_ = new QPushButton(tr("Install"));
  layout->addWidget(install_button_);
  connect(install_button_, &QPushButton::clicked, this, &InstallerWindow::StartInstall);
}

InstallerWindow::~InstallerWindow() {
  if (process_) {
    process_->kill();
    process_->waitForFinished(2000);
  }
}

void InstallerWindow::InstallUnattended(const QString& image, const QString& path, bool bloom,
                                        bool blur, bool xenia, bool shortcuts) {
  unattended_ = true;
  image_edit_->setText(image);
  path_edit_->setText(path);
  bloom_box_->setChecked(bloom);
  blur_box_->setChecked(blur);
  xenia_box_->setChecked(xenia);
  shortcuts_ = shortcuts;
  StartInstall();
}

void InstallerWindow::BrowseImage() {
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Select the game's disc image"), QDir::homePath(),
      tr("Disc images (*.iso);;All files (*)"));
  if (!path.isEmpty()) {
    image_edit_->setText(path);
  }
}

void InstallerWindow::BrowseInstallPath() {
  const QString path = QFileDialog::getExistingDirectory(this, tr("Install into"),
                                                         path_edit_->text());
  if (!path.isEmpty()) {
    path_edit_->setText(path);
  }
}

void InstallerWindow::SetStatus(const QString& text) {
  if (unattended_ && text != status_->text()) {
    QTextStream(stdout) << text << Qt::endl;
  }
  status_->setText(text);
}

void InstallerWindow::SetBusy(bool busy) {
  for (QWidget* w : {static_cast<QWidget*>(image_edit_), static_cast<QWidget*>(path_edit_),
                     static_cast<QWidget*>(bloom_box_), static_cast<QWidget*>(blur_box_),
                     static_cast<QWidget*>(xenia_box_), static_cast<QWidget*>(install_button_)}) {
    w->setEnabled(!busy);
  }
}

// Where the built game, the SDK's libraries and the setup scripts are, found
// by walking up from the installer: it lives in the project's installer/
// directory.
bool InstallerWindow::LocateProject(QString* error) {
  const QString env_project = qEnvironmentVariable("XERENGE_PROJECT");
  QDir dir(QCoreApplication::applicationDirPath());
  project_dir_.clear();
  if (!env_project.isEmpty()) {
    project_dir_ = QDir(env_project).absolutePath();
  } else {
    for (int up = 0; up < 5; ++up) {
      if (QFileInfo::exists(dir.filePath("run.sh")) &&
          QFileInfo::exists(dir.filePath("burnout_manifest.toml"))) {
        project_dir_ = dir.absolutePath();
        break;
      }
      if (!dir.cdUp()) {
        break;
      }
    }
  }
  download_ = project_dir_.isEmpty();
  if (download_) {
    // Nothing beside the installer: fetch and build under the install
    // directory. The paths are filled in before the sources exist.
    const QDir source(QDir(install_dir_).filePath("source"));
    project_dir_ = source.filePath("rexglue");
    scripts_dir_ = source.filePath("scripts");
    sdk_lib_dir_ = source.filePath("rexglue-sdk/out/linux-amd64");
    if (QStandardPaths::findExecutable("git").isEmpty()) {
      *error = tr("git is needed to fetch the game's source code. Install it and try again.");
      return false;
    }
    return true;
  }
  const QDir project(project_dir_);
  scripts_dir_ = FirstExisting({project.filePath("../scripts"),
                                project.filePath("../Xerenge/scripts")});
  sdk_lib_dir_ = FirstExisting({project.filePath("../rexglue-sdk/out/linux-amd64"),
                                project.filePath("../Xerenge/rexglue-sdk/out/linux-amd64")});
  return true;
}

void InstallerWindow::Refuse(const QString& message) {
  if (unattended_) {
    QTextStream(stderr) << message << Qt::endl;
    QCoreApplication::exit(1);
    return;
  }
  QMessageBox::warning(this, windowTitle(), message);
}

void InstallerWindow::StartInstall() {
  image_path_ = image_edit_->text().trimmed();
  install_dir_ = QDir(path_edit_->text().trimmed()).absolutePath();
  bloom_ = bloom_box_->isChecked();
  blur_ = blur_box_->isChecked();
  xenia_ = xenia_box_->isChecked();

  QString error;
  if (!LocateProject(&error)) {
    Refuse(error);
    return;
  }
  const QString game_dir = QDir(install_dir_).filePath("game");
  const bool extracted = QFileInfo::exists(QDir(game_dir).filePath("default.xex"));
  if (!extracted && !QFileInfo(image_path_).isFile()) {
    Refuse(tr("Select the game's disc image first."));
    return;
  }
  if (!QDir().mkpath(install_dir_)) {
    Refuse(tr("Cannot create %1.").arg(install_dir_));
    return;
  }
  log_path_ = QDir(install_dir_).filePath("install.log");
  QFile::remove(log_path_);
  // Fetched sources are always built: an update pulled in has to be.
  const bool built = !download_ && QFileInfo::exists(QDir(project_dir_).filePath("build/burnout"));
  if (!download_ && (!extracted || !built) && scripts_dir_.isEmpty()) {
    Refuse(tr("The setup scripts (extract-image, build.sh) were not found next to %1.")
               .arg(project_dir_));
    return;
  }

  steps_.clear();
  if (download_) {
    const QString source = QDir(install_dir_).filePath("source");
    steps_.append({tr("Downloading the source code"), 3, [this, source] {
                     const QRegularExpression receiving("Receiving objects:\\s*(\\d+)%");
                     const auto on_line = [this, receiving](const QString& line) {
                       if (auto m = receiving.match(line); m.hasMatch()) {
                         SetStepProgress(m.captured(1).toInt() / 100.0);
                       }
                     };
                     if (QFileInfo::exists(QDir(source).filePath(".git"))) {
                       RunProcess("git", {"-C", source, "pull", "--ff-only", "--progress"},
                                  on_line);
                     } else {
                       RunProcess("git", {"clone", "--depth", "1", "--progress", kRepository,
                                          source},
                                  on_line);
                     }
                   }});
  }
  // The disc image, checked against the retail hash, into <install>/game.
  if (!extracted) {
    const QRegularExpression percent("\\[\\s*(\\d+)%\\]");
    const auto build_output = [this, percent](const QString& line) {
      if (auto m = percent.match(line); m.hasMatch()) {
        SetStepProgress(m.captured(1).toInt() / 100.0);
      }
      if (!line.trimmed().isEmpty()) {
        SetStatus(line.trimmed().left(120));
      }
    };
    steps_.append({tr("Building setup tools"), 1, [this, build_output] {
                     RunProcess(QDir(scripts_dir_).filePath("build-tools.sh"), {}, build_output);
                   }});
    steps_.append({tr("Extracting the disc image"), 30, [this, game_dir] {
                     const QRegularExpression checking("checking the image:\\s*(\\d+)%");
                     const QRegularExpression written("files written:\\s*(\\d+)");
                     const QString extract_tool = FirstExisting(
                         {QDir(scripts_dir_).filePath("extract-image"),
                          QDir(scripts_dir_).filePath("../tools/bin/extract-image")});
                     if (extract_tool.isEmpty()) {
                       StepFinished(tr("extract-image was not built"));
                       return;
                     }
                     RunProcess(extract_tool,
                                {"--sha256", kRetailSha256, image_path_, game_dir},
                                [this, checking, written](const QString& line) {
                                  if (auto m = checking.match(line); m.hasMatch()) {
                                    SetStepProgress(0.5 * m.captured(1).toInt() / 100.0);
                                    SetStatus(tr("Checking the disc image: %1%")
                                                  .arg(m.captured(1)));
                                  } else if (auto w = written.match(line); w.hasMatch()) {
                                    const int files = w.captured(1).toInt();
                                    SetStepProgress(0.5 + 0.5 * std::min(files, kImageFiles) /
                                                              double(kImageFiles));
                                    SetStatus(tr("Extracting: %1 of %2 files")
                                                  .arg(files)
                                                  .arg(kImageFiles));
                                  }
                                });
                   }});
  }
  // No build yet: the SDK and the game, with the repository's own scripts.
  if (!built) {
    const QRegularExpression percent("\\[\\s*(\\d+)%\\]");
    const auto build_output = [this, percent](const QString& line) {
      if (auto m = percent.match(line); m.hasMatch()) {
        SetStepProgress(m.captured(1).toInt() / 100.0);
      }
      if (!line.trimmed().isEmpty()) {
        SetStatus(line.trimmed().left(120));
      }
    };
    steps_.append({tr("Checking the build tools"), 1, [this, build_output] {
                     RunProcess(QDir(scripts_dir_).filePath("check-prerequisites.sh"), {},
                                build_output);
                   }});
    steps_.append({tr("Building the SDK"), 15, [this, build_output] {
                     RunProcess(QDir(scripts_dir_).filePath("setup-sdk.sh"), {}, build_output);
                   }});
    steps_.append({tr("Building the shader translator"), 5, [this, build_output] {
                     RunProcess(QDir(scripts_dir_).filePath("setup-deps.sh"), {}, build_output);
                   }});
    steps_.append({tr("Building the game (this takes a while)"), 45, [this, game_dir,
                                                                        build_output] {
                     RunProcess(QDir(scripts_dir_).filePath("build.sh"), {"--game", game_dir},
                                build_output);
                   }});
    // build.sh builds the repository's own project.
    if (!download_) steps_.append({tr("Locating the build"), 1, [this] {
                     project_dir_ = QDir(QDir(scripts_dir_).filePath("../rexglue")).absolutePath();
                     sdk_lib_dir_ = FirstExisting(
                         {QDir(scripts_dir_).filePath("../rexglue-sdk/out/linux-amd64")});
                     StepFinished({});
                   }});
  }
  steps_.append({tr("Copying the game"), 8, [this] {
                   RunInBackground([this](std::function<void(double)> progress) {
                     return CopyRuntime(progress);
                   });
                 }});
  steps_.append({tr("Writing the settings"), 1, [this] { StepFinished(WriteSettings()); }});
  if (shortcuts_) {
    steps_.append({tr("Creating the shortcuts"), 1, [this] { StepFinished(CreateShortcuts()); }});
  }

  weight_total_ = 0;
  for (const Step& step : steps_) {
    weight_total_ += step.weight;
  }
  weight_done_ = 0;
  step_index_ = 0;
  progress_->setValue(0);
  SetBusy(true);
  RunNextStep();
}

void InstallerWindow::RunNextStep() {
  if (step_index_ >= steps_.size()) {
    progress_->setValue(progress_->maximum());
    SetStatus(tr("Installed. Start Burnout Revenge from the desktop shortcut or the "
                 "applications menu."));
    SetBusy(false);
    if (unattended_) {
      QCoreApplication::exit(0);
    }
    return;
  }
  SetStatus(steps_[step_index_].name + "...");
  SetStepProgress(0.0);
  steps_[step_index_].run();
}

void InstallerWindow::StepFinished(const QString& error) {
  if (!error.isEmpty()) {
    SetStatus(tr("%1 failed: %2").arg(steps_[step_index_].name, error) +
              (log_path_.isEmpty() ? QString() : tr("\nFull output: %1").arg(log_path_)));
    SetBusy(false);
    if (unattended_) {
      QCoreApplication::exit(1);
      return;
    }
    QMessageBox::critical(this, windowTitle(), status_->text());
    return;
  }
  weight_done_ += steps_[step_index_].weight;
  ++step_index_;
  RunNextStep();
}

void InstallerWindow::SetStepProgress(double fraction) {
  if (step_index_ >= steps_.size()) {
    return;
  }
  fraction = std::clamp(fraction, 0.0, 1.0);
  const double done = weight_done_ + steps_[step_index_].weight * fraction;
  progress_->setValue(int(progress_->maximum() * done / std::max(weight_total_, 1)));
}

void InstallerWindow::RunProcess(const QString& program, const QStringList& arguments,
                                 std::function<void(const QString&)> on_line,
                                 const QString& working_directory) {
  process_ = new QProcess(this);
  process_->setProcessChannelMode(QProcess::MergedChannels);
  process_->setProcessEnvironment(ToolEnvironment());
  if (!log_path_.isEmpty()) {
    QFile log(log_path_);
    if (log.open(QIODevice::Append | QIODevice::Text)) {
      log.write(QString("\n$ %1 %2\n").arg(program, arguments.join(' ')).toUtf8());
    }
  }
  if (!working_directory.isEmpty()) {
    process_->setWorkingDirectory(working_directory);
  }
  auto pending = std::make_shared<QString>();
  auto last_line = std::make_shared<QString>();
  connect(process_, &QProcess::readyRead, this, [this, pending, last_line, on_line] {
    const QByteArray chunk = process_->readAll();
    if (!log_path_.isEmpty()) {
      QFile log(log_path_);
      if (log.open(QIODevice::Append)) {
        log.write(chunk);
      }
    }
    *pending += QString::fromUtf8(chunk);
    // The scripts report progress on one line rewritten with \r.
    static const QRegularExpression breaks("[\r\n]");
    QStringList lines = pending->split(breaks);
    *pending = lines.takeLast();
    for (const QString& line : lines) {
      if (!line.trimmed().isEmpty()) {
        *last_line = line.trimmed();
      }
      on_line(line);
    }
  });
  connect(process_, &QProcess::finished, this,
          [this, last_line](int code, QProcess::ExitStatus status) {
            process_->deleteLater();
            process_ = nullptr;
            if (status != QProcess::NormalExit || code != 0) {
              StepFinished(last_line->isEmpty() ? tr("exit code %1").arg(code) : *last_line);
            } else {
              StepFinished({});
            }
          });
  connect(process_, &QProcess::errorOccurred, this, [this, program](QProcess::ProcessError e) {
    if (e == QProcess::FailedToStart && process_) {
      process_->deleteLater();
      process_ = nullptr;
      StepFinished(tr("could not start %1").arg(program));
    }
  });
  process_->start(program, arguments);
}

void InstallerWindow::RunInBackground(std::function<QString(std::function<void(double)>)> work) {
  auto* watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
    const QString error = watcher->result();
    watcher->deleteLater();
    StepFinished(error);
  });
  const auto report = [this](double fraction) {
    QMetaObject::invokeMethod(this, [this, fraction] { SetStepProgress(fraction); },
                              Qt::QueuedConnection);
  };
  watcher->setFuture(QtConcurrent::run([work, report] { return work(report); }));
}

// The game and what it loads: the executable, the GPU plugins beside it (the
// SDK looks for them there), the SDK's runtime library, and the shader sources
// the plume renderer reads while it runs.
QString InstallerWindow::CopyRuntime(const std::function<void(double)>& progress) const {
  const QDir project(project_dir_);
  const QDir sdk(sdk_lib_dir_);
  const QDir install(install_dir_);
  if (sdk_lib_dir_.isEmpty()) {
    return tr("the SDK's libraries (rexglue-sdk/out/linux-amd64) were not found");
  }
  if (!install.mkpath("bin") || !install.mkpath("generated/xenos-hlsl")) {
    return tr("cannot create directories in %1").arg(install_dir_);
  }
  struct Item {
    QString from, to;
  };
  QList<Item> items = {
      {project.filePath("build/burnout"), install.filePath("bin/burnout")},
      {project.filePath("build/librexgpu-plume.so"), install.filePath("bin/librexgpu-plume.so")},
      {sdk.filePath("librexgpu-xenos.so"), install.filePath("bin/librexgpu-xenos.so")},
      {sdk.filePath("librexruntime.so"), install.filePath("bin/librexruntime.so")},
  };
  if (QFileInfo::exists(sdk.filePath("libTracyClient.so"))) {
    items.append({sdk.filePath("libTracyClient.so"), install.filePath("bin/libTracyClient.so")});
  }
  QDirIterator shaders(project.filePath("generated/xenos-hlsl"), {"*.hlsl"}, QDir::Files);
  while (shaders.hasNext()) {
    const QString path = shaders.next();
    items.append({path, install.filePath("generated/xenos-hlsl/" + QFileInfo(path).fileName())});
  }
  for (int i = 0; i < items.size(); ++i) {
    if (!QFileInfo::exists(items[i].from)) {
      return tr("%1 is missing - is the game built?").arg(items[i].from);
    }
    if (const QString error = CopyFile(items[i].from, items[i].to); !error.isEmpty()) {
      return error;
    }
    progress(double(i + 1) / items.size());
  }
  return {};
}

QString InstallerWindow::WriteSettings() const {
  const QDir install(install_dir_);
  const QString settings =
      QString("# Burnout Revenge (Xerenge) settings, read by burnout-revenge at every start.\n"
              "# true or false\n"
              "bloom = %1\n"
              "motion_blur = %2\n"
              "# plume (this project's renderer) or xenos (Xenia's)\n"
              "renderer = %3\n")
          .arg(bloom_ ? "true" : "false", blur_ ? "true" : "false", xenia_ ? "xenos" : "plume");
  if (!WriteText(install.filePath("xerenge.conf"), settings, false)) {
    return tr("cannot write %1").arg(install.filePath("xerenge.conf"));
  }
  if (!WriteText(install.filePath("burnout-revenge"), QString::fromUtf8(kLauncher), true)) {
    return tr("cannot write %1").arg(install.filePath("burnout-revenge"));
  }
  return {};
}

QString InstallerWindow::CreateShortcuts() const {
  const QDir install(install_dir_);
  const QString launcher = install.filePath("burnout-revenge");
  const QString entry = QString(
                            "[Desktop Entry]\n"
                            "Type=Application\n"
                            "Name=Burnout Revenge\n"
                            "Comment=Burnout Revenge, recompiled (Xerenge)\n"
                            "Exec=\"%1\"\n"
                            "Path=%2\n"
                            "Icon=applications-games\n"
                            "Terminal=false\n"
                            "Categories=Game;\n")
                            .arg(launcher, install_dir_);

  // The applications menu.
  const QString applications =
      QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
  if (applications.isEmpty() || !QDir().mkpath(applications)) {
    return tr("no applications directory to put the menu entry in");
  }
  const QString menu_entry = QDir(applications).filePath(kDesktopId);
  if (!WriteText(menu_entry, entry, true)) {
    return tr("cannot write %1").arg(menu_entry);
  }
  QProcess::execute("update-desktop-database", {applications});

  // The desktop, where there is one.
  const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
  if (!desktop.isEmpty() && QDir().mkpath(desktop)) {
    const QString shortcut = QDir(desktop).filePath(kDesktopId);
    if (!WriteText(shortcut, entry, true)) {
      return tr("cannot write %1").arg(shortcut);
    }
    // GNOME will not start a desktop file it has not been told to trust.
    QProcess::execute("gio", {"set", shortcut, "metadata::trusted", "true"});
  }
  return {};
}
