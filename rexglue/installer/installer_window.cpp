#include "installer_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <functional>

namespace {

// The retail European image the recompiled code was made from (see SETUP.md).
constexpr const char* kRetailSha256 =
    "34c1bd4d549c2c53f29d814fa5e5d1c04c533c5ca0c39e57b6c2538f44ff59b4";
// Files extract-image writes for that image, for its progress.
constexpr int kImageFiles = 832;

constexpr const char* kDesktopId = "xerenge-burnout-revenge.desktop";

#ifdef Q_OS_WIN
constexpr const char* kExe = ".exe";
constexpr const char* kLauncherName = "Burnout Revenge.cmd";
#else
constexpr const char* kExe = "";
constexpr const char* kLauncherName = "burnout-revenge";
#endif

// The environment for the programs the installer runs (extract-image): the
// system's, without what an AppImage sets up for itself - its libraries and
// Qt plugins are not meant for them.
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

gamertag=$(value gamertag)
[ -n "$gamertag" ] && gamertag_arg="--gamertag=$gamertag"

# Online: a server address means that server and nothing local; otherwise this copy
# runs a lobby of its own when local multiplayer is on; otherwise no online mode.
server=$(value lobby_server)
online_args=""
if [ -n "$server" ]; then
    online_args="--online --online_fake_lobby=false --lobby_server=$server"
elif [ "$(value online)" = true ]; then
    online_args="--online --online_fake_lobby=false"
fi

lang=$(value language)
[ -n "$lang" ] && export XERENGE_LANGUAGE="$lang"
[ "$(value async_present)" = true ] && export XERENGE_ASYNC_PRESENT=1
[ "$(value early_submit)" = false ] && export XERENGE_ACQUIRE_FIRST=1
[ "$(value packed_vertices)" = false ] && export XERENGE_FULL_VERTICES=1
[ "$(value culling)" = true ] && export XERENGE_CULL=1
res=$(value render_resolution)
[ -n "$res" ] && export XERENGE_RENDER_RESOLUTION="$res"
[ "$(value fps_counter)" = true ] && export XERENGE_FPS_SHOW=1

# Fullscreen unless windowed; the keyboard as a pad with its keybind_* lines.
extra=--fullscreen
[ "$(value windowed)" = true ] && extra=--no-fullscreen
if [ "$(value keyboard)" = true ]; then
    extra="$extra --mnk_mode=true"
    for key in $(sed -n "s/^[[:space:]]*\(keybind_[a-z_]*\)[[:space:]]*=.*/\1/p" "$conf"); do
        extra="$extra --$key=$(value "$key")"
    done
fi

# Debug mode: the debug_* settings decide what is logged.
level=info
if [ "$(value debug)" = true ]; then
    level=$(value debug_log_level)
    [ -n "$level" ] || level=debug
    [ "$(value debug_gpu_trace)" = true ] && export XERENGE_GPU_TRACE=1
    [ "$(value debug_movie_trace)" = true ] && export XERENGE_MOVIE_TRACE=1
    [ "$(value debug_pipeline_log)" = true ] && export XERENGE_PIPELINE_LOG=1
    [ "$(value debug_vertex_trace)" = false ] || export XERENGE_VERTEX_TRACE=1
    video_gpu=$(value debug_video_gpu)
    [ -n "$video_gpu" ] && export XERENGE_VIDEO_GPU="$video_gpu"
    [ "$(value debug_noisy)" = true ] && extra="$extra --log_noisy=true"
fi

export SDL_VIDEODRIVER=x11
export LD_LIBRARY_PATH="$dir/bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
logs="${XDG_STATE_HOME:-$HOME/.local/state}/xerenge-burnout"
mkdir -p "$logs"

# Started from a shortcut there is no terminal to show why a start failed
# before the game's own log opened, so what it prints goes to a file.
if [ ! -t 2 ]; then
    exec >"$logs/launcher.log" 2>&1
    echo "$(date) session=$XDG_SESSION_TYPE DISPLAY=$DISPLAY WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
    echo "args: $extra $*"
fi

cd "$dir"
exec "$dir/bin/burnout" \
    --game_data_root "$dir/game" \
    --gpu_plugin "$renderer" \
    --gpu_backend "$renderer" \
    --no-vulkan_async_skip_incomplete_frames \
    --log_level "$level" \
    --log_file "$logs/burnout.log" \
    --log_max_file_size_mb 32 \
    --log_max_files 3 \
    $extra $gamertag_arg $online_args \
    "$@"
)SH";

// The same for Windows, as a batch file the shortcuts point at.
constexpr const char* kWindowsLauncher = R"CMD(@echo off
rem Burnout Revenge (Xerenge). Written by the installer; the settings are in
rem xerenge.conf beside this file and are read at every start.
setlocal
set "dir=%~dp0"
set "bloom=false"
set "motion_blur=false"
set "renderer=plume"
if exist "%dir%xerenge.conf" (
    for /f "usebackq eol=# tokens=1,2 delims== " %%a in ("%dir%xerenge.conf") do set "%%a=%%b"
)

if /i not "%bloom%"=="true" set "XERENGE_NO_BLOOM=1"
if /i not "%motion_blur%"=="true" set "XERENGE_NO_MOTION_BLUR=1"

if /i not "%renderer%"=="xenos" set "renderer=plume"
if "%renderer%"=="plume" (
    set "XERENGE_D3D_TARGETS=1"
    set "XERENGE_D3D_UI=1"
    set "XERENGE_SKIP_LOGOS=1"
    set "XERENGE_REAL_SHADERS=1"
    set "XERENGE_D3D_DRAWS=1"
    set "XERENGE_SECONDARY_TICKS=1"
)

set "gamertag_arg="
if defined gamertag set "gamertag_arg=--gamertag=%gamertag%"

rem Online: a server address means that server and nothing local; otherwise this copy
rem runs a lobby of its own when local multiplayer is on; otherwise no online mode.
set "online_args="
if /i "%online%"=="true" set "online_args=--online --online_fake_lobby=false"
if defined lobby_server set "online_args=--online --online_fake_lobby=false --lobby_server=%lobby_server%"

if defined language set "XERENGE_LANGUAGE=%language%"
if /i "%async_present%"=="true" set "XERENGE_ASYNC_PRESENT=1"
if /i "%early_submit%"=="false" set "XERENGE_ACQUIRE_FIRST=1"
if /i "%packed_vertices%"=="false" set "XERENGE_FULL_VERTICES=1"
if /i "%culling%"=="true" set "XERENGE_CULL=1"
if defined render_resolution set "XERENGE_RENDER_RESOLUTION=%render_resolution%"
if /i "%fps_counter%"=="true" set "XERENGE_FPS_SHOW=1"

rem Fullscreen unless windowed; the keyboard as a pad with its keybind_* lines.
set "extra=--fullscreen"
if /i "%windowed%"=="true" set "extra=--no-fullscreen"
if /i not "%keyboard%"=="true" goto :keys_done
set "extra=%extra% --mnk_mode=true"
for /f "usebackq eol=# tokens=1,2 delims== " %%a in ("%dir%xerenge.conf") do (
    echo %%a| findstr /b "keybind_" >nul && call set "extra=%%extra%% --%%a=%%b"
)
:keys_done

rem Debug mode: the debug_* settings decide what is logged.
set "level=info"
if /i not "%debug%"=="true" goto :debug_done
set "level=debug"
if defined debug_log_level set "level=%debug_log_level%"
if /i "%debug_gpu_trace%"=="true" set "XERENGE_GPU_TRACE=1"
if /i "%debug_movie_trace%"=="true" set "XERENGE_MOVIE_TRACE=1"
if /i "%debug_pipeline_log%"=="true" set "XERENGE_PIPELINE_LOG=1"
if /i not "%debug_vertex_trace%"=="false" set "XERENGE_VERTEX_TRACE=1"
if defined debug_video_gpu set "XERENGE_VIDEO_GPU=%debug_video_gpu%"
if /i "%debug_noisy%"=="true" set "extra=%extra% --log_noisy=true"
:debug_done

set "logs=%LOCALAPPDATA%\xerenge-burnout"
if not exist "%logs%" mkdir "%logs%"

cd /d "%dir%"
start "" "%dir%bin\burnout.exe" ^
    --game_data_root "%dir%game" ^
    --gpu_plugin %renderer% ^
    --gpu_backend %renderer% ^
    --no-vulkan_async_skip_incomplete_frames ^
    --log_level %level% ^
    --log_file "%logs%\burnout.log" ^
    --log_max_file_size_mb 32 ^
    --log_max_files 3 ^
    %extra% %gamertag_arg% %online_args% ^
    %*
)CMD";

// The pad controls the keyboard can stand in for: the game's keybind_* setting,
// the label shown, and the key given to it by default.
struct PadControl {
  const char* setting;
  const char* label;
  const char* key;
};
constexpr PadControl kPadControls[] = {
    {"keybind_a", "A", "Space"},
    {"keybind_b", "B", "Backspace"},
    {"keybind_x", "X", "E"},
    {"keybind_y", "Y", "R"},
    {"keybind_left_shoulder", "Left bumper (LB)", "Q"},
    {"keybind_right_shoulder", "Right bumper (RB)", "F"},
    {"keybind_left_trigger", "Left trigger (LT) - brake", "S"},
    {"keybind_right_trigger", "Right trigger (RT) - accelerate", "W"},
    {"keybind_lstick_up", "Left stick up", ""},
    {"keybind_lstick_down", "Left stick down", ""},
    {"keybind_lstick_left", "Left stick left - steer", "A"},
    {"keybind_lstick_right", "Left stick right - steer", "D"},
    {"keybind_lstick_press", "Left stick press", "C"},
    {"keybind_rstick_up", "Right stick up", "I"},
    {"keybind_rstick_down", "Right stick down", "K"},
    {"keybind_rstick_left", "Right stick left", "J"},
    {"keybind_rstick_right", "Right stick right", "L"},
    {"keybind_rstick_press", "Right stick press", "V"},
    {"keybind_dpad_up", "D-pad up", "Up"},
    {"keybind_dpad_down", "D-pad down", "Down"},
    {"keybind_dpad_left", "D-pad left", "Left"},
    {"keybind_dpad_right", "D-pad right", "Right"},
    {"keybind_start", "Start", "Enter"},
    {"keybind_back", "Back", "Escape"},
    {"keybind_guide", "Guide", ""},
};

// A key as the game's keybind settings name it, or empty for a key they do
// not know. A bare Shift, Ctrl or Alt is never a key there - the game matches
// modifiers exactly, so one alone could never fire - but held with another key
// it is written as a prefix: "Shift+W".
QString GameKeyName(const QKeyEvent* event) {
  const int key = event->key();
  const bool keypad = event->modifiers() & Qt::KeypadModifier;
  QString name;
  if (key >= Qt::Key_A && key <= Qt::Key_Z) {
    name = QChar('A' + (key - Qt::Key_A));
  } else if (key >= Qt::Key_0 && key <= Qt::Key_9) {
    name = (keypad ? QString("Numpad") : QString()) + QChar('0' + (key - Qt::Key_0));
  } else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
    name = QString("F%1").arg(key - Qt::Key_F1 + 1);
  } else {
    switch (key) {
      case Qt::Key_Space: name = "Space"; break;
      case Qt::Key_Return: name = "Enter"; break;
      case Qt::Key_Enter: name = keypad ? "NumpadEnter" : "Enter"; break;
      case Qt::Key_Tab: name = "Tab"; break;
      case Qt::Key_Backspace: name = "Backspace"; break;
      case Qt::Key_Escape: name = "Escape"; break;
      case Qt::Key_Delete: name = "Delete"; break;
      case Qt::Key_Insert: name = "Insert"; break;
      case Qt::Key_Home: name = "Home"; break;
      case Qt::Key_End: name = "End"; break;
      case Qt::Key_PageUp: name = "PageUp"; break;
      case Qt::Key_PageDown: name = "PageDown"; break;
      case Qt::Key_Left: name = "Left"; break;
      case Qt::Key_Right: name = "Right"; break;
      case Qt::Key_Up: name = "Up"; break;
      case Qt::Key_Down: name = "Down"; break;
      case Qt::Key_Minus: name = keypad ? "NumpadMinus" : "Minus"; break;
      case Qt::Key_Plus: name = keypad ? "NumpadPlus" : "Plus"; break;
      case Qt::Key_Equal: name = "Plus"; break;
      case Qt::Key_Asterisk: name = "NumpadStar"; break;
      case Qt::Key_Slash: name = keypad ? "NumpadSlash" : "Slash"; break;
      case Qt::Key_Comma: name = "Comma"; break;
      case Qt::Key_Period: name = "Period"; break;
      case Qt::Key_Semicolon: name = "Semicolon"; break;
      case Qt::Key_Backslash: name = "Backslash"; break;
      case Qt::Key_BracketLeft: name = "LBracket"; break;
      case Qt::Key_BracketRight: name = "RBracket"; break;
      case Qt::Key_Apostrophe: name = "Quote"; break;
      case Qt::Key_QuoteLeft: name = "Backtick"; break;
      case Qt::Key_CapsLock: name = "CapsLock"; break;
      default: return {};
    }
  }
  QString prefix;
  if (event->modifiers() & Qt::ControlModifier) prefix += "Ctrl+";
  if (event->modifiers() & Qt::AltModifier) prefix += "Alt+";
  if (event->modifiers() & Qt::ShiftModifier) prefix += "Shift+";
  return prefix + name;
}

// A field that takes the key pressed in it rather than text. Several keys for
// one control can still be typed into xerenge.conf by hand ("A,Left").
class KeyEdit : public QLineEdit {
 public:
  using QLineEdit::QLineEdit;

  // Called with the key's name when one is pressed in the field.
  std::function<void(const QString&)> on_captured;

 protected:
  void keyPressEvent(QKeyEvent* event) override {
    const QString name = GameKeyName(event);
    if (!name.isEmpty()) {
      setText(name);
      if (on_captured) {
        on_captured(name);
      }
    }
    event->accept();
  }
};

// The keys of a keybind value: "A,Left" -> {A, Left}.
QStringList BindKeys(const QString& value) {
  QStringList keys;
  for (const QString& key : value.split(',', Qt::SkipEmptyParts)) {
    keys.append(key.trimmed());
  }
  return keys;
}

QString DefaultInstallDir() {
  return QDir::home().filePath("Games/Burnout Revenge");
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

// Every file under `from`, as paths relative to it.
QStringList FilesUnder(const QString& from) {
  QStringList files;
  QDirIterator it(from, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    files.append(QDir(from).relativeFilePath(it.next()));
  }
  return files;
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
  setMinimumWidth(720);

  // Two tabs over the progress and the button: the install itself, and the
  // keyboard controls.
  auto* outer = new QVBoxLayout(this);
  auto* tabs = new QTabWidget;
  outer->addWidget(tabs);
  auto* setup_page = new QWidget;
  auto* layout = new QVBoxLayout(setup_page);
  tabs->addTab(setup_page, tr("Install"));

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

  windowed_box_ = new QCheckBox(tr("Windowed"));
  windowed_box_->setToolTip(tr("Run in a window. Fullscreen otherwise."));
  layout->addWidget(windowed_box_);
  debug_box_ = new QCheckBox(tr("Debug mode"));
  debug_box_->setToolTip(tr("Detailed logs, for reporting a problem. What is logged is set by the "
                            "debug_* lines in xerenge.conf."));
  layout->addWidget(debug_box_);

  // The game asks for its language at every start unless one is set here.
  auto* language_row = new QHBoxLayout;
  language_row->addWidget(new QLabel(tr("Language")));
  language_combo_ = new QComboBox;
  language_combo_->addItem(tr("Ask at every start"), QString());
  language_combo_->addItem("English", "0");
  language_combo_->addItem("English (US)", "1");
  language_combo_->addItem("Espa\u00f1ol", "5");
  language_combo_->addItem("Nederlands", "10");
  language_combo_->addItem("Svenska", "11");
  language_combo_->addItem("Suomi", "12");
  language_combo_->setCurrentIndex(1);
  language_row->addWidget(language_combo_, 1);
  layout->addLayout(language_row);

  // Hacks: folded away, each trading something for speed on a slow machine.
  auto* hacks_toggle = new QToolButton;
  hacks_toggle->setText(tr("Hacks"));
  hacks_toggle->setCheckable(true);
  hacks_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  hacks_toggle->setArrowType(Qt::RightArrow);
  hacks_toggle->setAutoRaise(true);
  layout->addWidget(hacks_toggle);
  auto* hacks = new QWidget;
  auto* hacks_layout = new QVBoxLayout(hacks);
  hacks_layout->setContentsMargins(18, 0, 0, 0);
  async_box_ = new QCheckBox(tr("Asynchronous presentation"));
  async_box_->setToolTip(tr("The game starts its next frame while the GPU still draws this one. "
                            "Faster; once showed a black frame now and then."));
  hacks_layout->addWidget(async_box_);
  early_submit_box_ = new QCheckBox(tr("Early submit"));
  early_submit_box_->setToolTip(
      tr("The frame goes to the GPU before the game waits for the display to take the next "
         "one, so the two overlap. About 50 -> 60 fps in a race on a Ryzen 5 4500U."));
  early_submit_box_->setChecked(true);
  hacks_layout->addWidget(early_submit_box_);
  packed_vertices_box_ = new QCheckBox(tr("Packed vertices"));
  packed_vertices_box_->setToolTip(
      tr("Only the vertex data each shader reads goes to the GPU, not a layout wide enough "
         "for every shader. Less memory traffic; turn off if models look broken."));
  packed_vertices_box_->setChecked(true);
  hacks_layout->addWidget(packed_vertices_box_);
  culling_box_ = new QCheckBox(tr("Back-face culling"));
  culling_box_->setToolTip(
      tr("Skip the faces the game culls itself, as the console does. Experimental."));
  hacks_layout->addWidget(culling_box_);
  auto* resolution_row = new QHBoxLayout;
  resolution_row->addWidget(new QLabel(tr("Render resolution")));
  resolution_combo_ = new QComboBox;
  resolution_combo_->addItem(tr("The window's"), QString());
  resolution_combo_->addItem("1280x720", "1280x720");
  resolution_combo_->addItem("1600x900", "1600x900");
  resolution_combo_->setToolTip(tr("Draw the frame at this size and scale it onto the window. "
                                   "1280x720 is the size the game renders at on the console."));
  resolution_row->addWidget(resolution_combo_, 1);
  hacks_layout->addLayout(resolution_row);
  fps_box_ = new QCheckBox(tr("Frame counter"));
  fps_box_->setToolTip(tr("Frames per second in the top left corner."));
  hacks_layout->addWidget(fps_box_);
  hacks->setVisible(false);
  layout->addWidget(hacks);
  connect(hacks_toggle, &QToolButton::toggled, this, [hacks, hacks_toggle](bool open) {
    hacks->setVisible(open);
    hacks_toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
  });

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

  layout->addStretch(1);

  // The keyboard as a pad.
  auto* keys_page = new QWidget;
  auto* keys_layout = new QVBoxLayout(keys_page);
  keyboard_box_ = new QCheckBox(tr("Play with the keyboard"));
  keyboard_box_->setToolTip(tr("The keyboard stands in for a pad. A connected pad still works."));
  keys_layout->addWidget(keyboard_box_);
  auto* hint = new QLabel(tr("Click a field and press a key. Several keys for one control can "
                             "be written into xerenge.conf by hand, comma separated."));
  hint->setWordWrap(true);
  keys_layout->addWidget(hint);
  auto* grid = new QGridLayout;
  int row = 0;
  for (const PadControl& control : kPadControls) {
    auto* edit = new KeyEdit(QString::fromLatin1(control.key));
    edit->setClearButtonEnabled(true);
    // Two columns of controls, so the tab is not taller than the install one.
    constexpr int kRows = (int(std::size(kPadControls)) + 1) / 2;
    const int column = (row / kRows) * 2;
    grid->addWidget(new QLabel(tr(control.label)), row % kRows, column);
    grid->addWidget(edit, row % kRows, column + 1);
    key_edits_.append({QString::fromLatin1(control.setting), edit});
    ++row;
  }
  // One key, one control: a key held for two controls presses both buttons at
  // once, and the menus take neither (V on both A and the right stick press
  // left A dead in them). A key chosen for a control is taken off any other,
  // and a key still on two - from a hand-edited xerenge.conf - shows in red.
  for (const auto& [setting, edit] : key_edits_) {
    auto* key_edit = static_cast<KeyEdit*>(edit);
    key_edit->on_captured = [this, key_edit](const QString& name) {
      for (const auto& [other_setting, other] : key_edits_) {
        if (other == key_edit) {
          continue;
        }
        QStringList keys = BindKeys(other->text());
        if (keys.removeAll(name) > 0) {
          other->setText(keys.join(','));
        }
      }
    };
    connect(edit, &QLineEdit::textChanged, this, [this] {
      QMap<QString, int> uses;
      for (const auto& [setting, other] : key_edits_) {
        for (const QString& key : BindKeys(other->text())) {
          ++uses[key];
        }
      }
      for (const auto& [setting, other] : key_edits_) {
        bool shared = false;
        for (const QString& key : BindKeys(other->text())) {
          shared = shared || uses.value(key) > 1;
        }
        other->setStyleSheet(shared ? "color: #d03030;" : QString());
        other->setToolTip(shared ? tr("This key is on another control as well.") : QString());
      }
    });
  }
  keys_layout->addLayout(grid);
  keys_layout->addStretch(1);
  for (const auto& [setting, edit] : key_edits_) {
    edit->setEnabled(false);
  }
  connect(keyboard_box_, &QCheckBox::toggled, this, [this](bool on) {
    for (const auto& [setting, edit] : key_edits_) {
      edit->setEnabled(on);
    }
  });
  tabs->addTab(keys_page, tr("Controls"));

  // The network tab, second: the name shown to others, local multiplayer, and a server of your own.
  auto* network_page = new QWidget;
  auto* network_layout = new QVBoxLayout(network_page);
  auto* gamertag_row = new QHBoxLayout;
  gamertag_row->addWidget(new QLabel(tr("Gamertag")));
  gamertag_edit_ = new QLineEdit;
  gamertag_edit_->setMaxLength(15);
  gamertag_edit_->setPlaceholderText(tr("Player"));
  gamertag_edit_->setValidator(
      new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9_-]*"), gamertag_edit_));
  gamertag_edit_->setToolTip(tr("Your name online: up to 15 letters, digits, - and _."));
  gamertag_row->addWidget(gamertag_edit_, 1);
  network_layout->addLayout(gamertag_row);

  online_box_ = new QCheckBox(tr("Local multiplayer"));
  online_box_->setToolTip(
      tr("Turns the online menus on. Every copy of the game on your network runs a lobby of its own; "
         "the games one player creates show up for the others, and any player can host."));
  network_layout->addWidget(online_box_);

  auto* server_row = new QHBoxLayout;
  server_row->addWidget(new QLabel(tr("Server address")));
  server_edit_ = new QLineEdit;
  server_edit_->setPlaceholderText(tr("empty: no server, local multiplayer only"));
  server_edit_->setValidator(
      new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9._-]*"), server_edit_));
  server_edit_->setToolTip(tr("The host or IP of a lobby server of your own (the release carries one). "
                              "With an address the game plays online through that server, and local "
                              "multiplayer is off."));
  server_row->addWidget(server_edit_, 1);
  network_layout->addLayout(server_row);

  server_note_ = new QLabel;
  server_note_->setWordWrap(true);
  network_layout->addWidget(server_note_);
  network_layout->addStretch(1);
  // A server address replaces local multiplayer: the box is off and not to be ticked while it is set.
  const auto sync_network = [this] {
    const bool server = !server_edit_->text().trimmed().isEmpty();
    if (server && online_box_->isEnabled()) {
      online_saved_ = online_box_->isChecked();
      online_box_->setChecked(false);
    } else if (!server && !online_box_->isEnabled()) {
      online_box_->setChecked(online_saved_);
    }
    online_box_->setEnabled(!server);
    server_note_->setText(server ? tr("Playing online through the server %1; local multiplayer is off.")
                                       .arg(server_edit_->text().trimmed())
                                 : tr("Local multiplayer finds the games of the other copies on your "
                                      "network. To play through a server, enter its address."));
  };
  connect(server_edit_, &QLineEdit::textChanged, this, sync_network);
  sync_network();
  tabs->insertTab(1, network_page, tr("Network"));

  layout = outer;
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
  connect(path_edit_, &QLineEdit::textChanged, this, &InstallerWindow::RefreshInstallState);
  RefreshInstallState();
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

void InstallerWindow::RefreshInstallState() {
  const QDir install(QDir(path_edit_->text().trimmed()).absolutePath());
  const bool installed = QFileInfo::exists(install.filePath("game/default.xex")) &&
                         QFileInfo::exists(install.filePath("xerenge.conf"));
  static QString loaded_from;
  if (installed && loaded_from != install.path()) {
    loaded_from = install.path();
    LoadSettings(install.filePath("xerenge.conf"));
  } else if (!installed) {
    loaded_from.clear();
  }
  install_button_->setText(installed ? tr("Update") : tr("Install"));
  image_edit_->setEnabled(!installed);
  SetStatus(installed ? tr("The game is installed here: Update replaces the program and keeps "
                           "the game files and these settings. No disc image is needed.")
                      : tr("Ready."));
}

// The settings of an installed copy, back into the controls, so an update
// writes them out again as they were unless changed here.
void InstallerWindow::LoadSettings(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return;
  }
  preserved_debug_settings_.clear();
  QMap<QString, QString> values;
  while (!file.atEnd()) {
    const QString line = QString::fromUtf8(file.readLine()).trimmed();
    if (line.isEmpty() || line.startsWith('#')) {
      continue;
    }
    const int equals = line.indexOf('=');
    if (equals > 0) {
      values.insert(line.left(equals).trimmed(), line.mid(equals + 1).trimmed());
      if (line.startsWith("debug_")) {
        preserved_debug_settings_ += line + "\n";
      }
    }
  }
  const auto flag = [&](const char* key, QCheckBox* box) {
    if (values.contains(key)) {
      box->setChecked(values.value(key) == "true");
    }
  };
  const auto choice = [&](const char* key, QComboBox* combo) {
    if (values.contains(key)) {
      const int index = combo->findData(values.value(key));
      if (index >= 0) {
        combo->setCurrentIndex(index);
      }
    }
  };
  flag("bloom", bloom_box_);
  flag("motion_blur", blur_box_);
  if (values.contains("renderer")) {
    xenia_box_->setChecked(values.value("renderer") == "xenos");
  }
  flag("windowed", windowed_box_);
  flag("debug", debug_box_);
  choice("language", language_combo_);
  if (values.contains("gamertag")) {
    gamertag_edit_->setText(values.value("gamertag"));
  }
  flag("online", online_box_);
  if (values.contains("lobby_server")) {
    server_edit_->setText(values.value("lobby_server"));
  }
  flag("async_present", async_box_);
  flag("early_submit", early_submit_box_);
  flag("packed_vertices", packed_vertices_box_);
  flag("culling", culling_box_);
  choice("render_resolution", resolution_combo_);
  flag("fps_counter", fps_box_);
  flag("keyboard", keyboard_box_);
  for (const auto& [setting, edit] : key_edits_) {
    if (values.contains(setting)) {
      edit->setText(values.value(setting));
    }
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

// Where the built game that ships with the installer is: payload/ beside it,
// or inside the AppImage under usr/share/xerenge/payload. XERENGE_PAYLOAD
// names another one (a CI package unpacked by hand, say).
bool InstallerWindow::LocatePayload(QString* error) {
  const QDir here(QCoreApplication::applicationDirPath());
  QStringList candidates;
  if (const QString env = qEnvironmentVariable("XERENGE_PAYLOAD"); !env.isEmpty()) {
    candidates.append(env);
  }
  candidates.append(here.filePath("payload"));
  candidates.append(here.filePath("../share/xerenge/payload"));
  payload_dir_.clear();
  for (const QString& candidate : candidates) {
    if (QFileInfo::exists(QDir(candidate).filePath(QString("bin/burnout") + kExe))) {
      payload_dir_ = QDir(candidate).absolutePath();
      return true;
    }
  }
  *error = tr("The game files that come with the installer were not found (looked in %1).")
               .arg(candidates.join(", "));
  return false;
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
  {
    QString extra;
    QTextStream out(&extra);
    out << "# Fullscreen unless true\n"
        << "windowed = " << (windowed_box_->isChecked() ? "true" : "false") << "\n"
        << "# The name shown to others online (empty: Player)\n"
        << "gamertag = " << gamertag_edit_->text().trimmed() << "\n"
        << "# Local multiplayer: the copies on one network find each other's games\n"
        << "online = "
        << ((server_edit_->text().trimmed().isEmpty() ? online_box_->isChecked() : online_saved_) ? "true"
                                                                                                    : "false")
        << "\n"
        << "# A server of your own (host or IP); set, it is the only lobby and local multiplayer is off\n"
        << "lobby_server = " << server_edit_->text().trimmed() << "\n"
        << "# The language set ahead of time (empty: the game asks at every start)\n"
        << "language = " << language_combo_->currentData().toString() << "\n"
        << "# Hacks\n"
        << "async_present = " << (async_box_->isChecked() ? "true" : "false") << "\n"
        << "early_submit = " << (early_submit_box_->isChecked() ? "true" : "false") << "\n"
        << "packed_vertices = " << (packed_vertices_box_->isChecked() ? "true" : "false") << "\n"
        << "culling = " << (culling_box_->isChecked() ? "true" : "false") << "\n"
        << "render_resolution = " << resolution_combo_->currentData().toString() << "\n"
        << "fps_counter = " << (fps_box_->isChecked() ? "true" : "false") << "\n"
        << "# The keyboard as a pad, and a key (or several, comma separated) per control\n"
        << "keyboard = " << (keyboard_box_->isChecked() ? "true" : "false") << "\n";
    for (const auto& [setting, edit] : key_edits_) {
      out << setting << " = " << edit->text().trimmed() << "\n";
    }
    out << "# Debug mode: detailed logs for reporting a problem. They go to\n"
        << "# ~/.local/state/xerenge-burnout (Linux) or %LOCALAPPDATA%\\xerenge-burnout (Windows).\n"
        << "debug = " << (debug_box_->isChecked() ? "true" : "false") << "\n"
        << "# What debug mode logs. Edited here only; an update keeps these lines.\n"
        << "#   debug_log_level: trace, debug, info, warn, error\n"
        << "#   debug_gpu_trace: the renderer's per-draw and per-frame diagnostics (large)\n"
        << "#   debug_movie_trace: the video player's states\n"
        << "#   debug_pipeline_log: every render pipeline built\n"
        << "#   debug_vertex_trace: vertex attributes that come out as NaN or absurd, and then who writes those vertices (on unless false)\n"
        << "#   debug_video_gpu: 0 turns menu video into RGB on the CPU, 3 (the default) on the GPU; empty: the default\n"
        << "#   debug_noisy: the per-frame log lines as well (very large)\n";
    if (!preserved_debug_settings_.isEmpty()) {
      out << preserved_debug_settings_;
    } else {
      out << "debug_log_level = debug\n"
          << "debug_gpu_trace = true\n"
          << "debug_movie_trace = false\n"
          << "debug_pipeline_log = false\n"
          << "debug_vertex_trace = true\n"
          << "debug_video_gpu = \n"
          << "debug_noisy = false\n";
    }
    out.flush();
    extra_settings_ = extra;
  }

  QString error;
  if (!LocatePayload(&error)) {
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

  steps_.clear();
  // The disc image, checked against the retail hash, into <install>/game.
  if (!extracted) {
    steps_.append({tr("Extracting the disc image"), 30, [this, game_dir] {
                     const QRegularExpression checking("checking the image:\\s*(\\d+)%");
                     const QRegularExpression written("files written:\\s*(\\d+)");
                     const QString extract_tool =
                         QDir(payload_dir_).filePath(QString("tools/extract-image") + kExe);
                     if (!QFileInfo::exists(extract_tool)) {
                       StepFinished(tr("%1 is missing").arg(extract_tool));
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
  steps_.append({tr("Copying the game"), 3, [this] {
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

// The game and what it loads - the executable, the SDK's runtime library and
// the GPU plugins beside it (the SDK looks for them there) - as the payload's
// bin/ has them.
QString InstallerWindow::CopyRuntime(const std::function<void(double)>& progress) const {
  const QDir from(QDir(payload_dir_).filePath("bin"));
  const QDir to(QDir(install_dir_).filePath("bin"));
  const QStringList files = FilesUnder(from.absolutePath());
  if (files.isEmpty()) {
    return tr("%1 is empty").arg(from.absolutePath());
  }
  for (int i = 0; i < files.size(); ++i) {
    const QString target = to.filePath(files[i]);
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
      return tr("cannot create directories in %1").arg(install_dir_);
    }
    if (const QString error = CopyFile(from.filePath(files[i]), target); !error.isEmpty()) {
      return error;
    }
    progress(double(i + 1) / files.size());
  }
  return {};
}

QString InstallerWindow::WriteSettings() const {
  const QDir install(install_dir_);
  const QString settings =
      QString("# Burnout Revenge (Xerenge) settings, read by the launcher at every start.\n"
              "# true or false\n"
              "bloom = %1\n"
              "motion_blur = %2\n"
              "# plume (this project's renderer) or xenos (Xenia's)\n"
              "renderer = %3\n")
          .arg(bloom_ ? "true" : "false", blur_ ? "true" : "false", xenia_ ? "xenos" : "plume") +
      extra_settings_;
  if (!WriteText(install.filePath("xerenge.conf"), settings, false)) {
    return tr("cannot write %1").arg(install.filePath("xerenge.conf"));
  }
#ifdef Q_OS_WIN
  const char* launcher = kWindowsLauncher;
#else
  const char* launcher = kLauncher;
#endif
  if (!WriteText(install.filePath(kLauncherName), QString::fromUtf8(launcher), true)) {
    return tr("cannot write %1").arg(install.filePath(kLauncherName));
  }
  return {};
}

QString InstallerWindow::CreateShortcuts() const {
  const QDir install(install_dir_);
  const QString launcher = install.filePath(kLauncherName);
#ifdef Q_OS_WIN
  // On Windows QFile::link makes a .lnk shortcut: one in the Start menu's
  // programs and one on the desktop.
  for (const auto location :
       {QStandardPaths::ApplicationsLocation, QStandardPaths::DesktopLocation}) {
    const QString dir = QStandardPaths::writableLocation(location);
    if (dir.isEmpty() || !QDir().mkpath(dir)) {
      continue;
    }
    const QString shortcut = QDir(dir).filePath("Burnout Revenge.lnk");
    QFile::remove(shortcut);
    if (!QFile::link(launcher, shortcut)) {
      return tr("cannot create %1").arg(shortcut);
    }
  }
  return {};
#else
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
#endif
}
