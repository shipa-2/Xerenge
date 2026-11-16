# The installer: a Qt 6 window over the existing setup scripts (extract-image,
# setup-sdk.sh, build.sh). Kept out of the game's own build so the game does not
# need Qt.
#
#   cmake -S installer -B installer/build && cmake --build installer/build
cmake_minimum_required(VERSION 3.21)
project(xerenge_installer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Network)

add_executable(xerenge-installer
    main.cpp
    game_icon.cpp
    game_icon.h
    installer_window.cpp
    installer_window.h
)
target_link_libraries(xerenge-installer PRIVATE Qt6::Widgets Qt6::Concurrent Qt6::Network)
# Xerenge's icon: the window's on every system (main.cpp), the stand-in for a
# game whose disc has no logo (CreateShortcuts), and the .exe's on Windows.
qt_add_resources(xerenge-installer "icons" PREFIX "/" FILES icons/xerenge.png)
if(WIN32)
    target_sources(xerenge-installer PRIVATE xerenge.rc)
endif()
if(APPLE)
    # An app bundle, so make-macos-app.sh can hand it to macdeployqt.
    set_target_properties(xerenge-installer PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Burnout Revenge Installer"
        MACOSX_BUNDLE_GUI_IDENTIFIER "xerenge.installer"
        MACOSX_BUNDLE_ICON_FILE xerenge)
endif()

# The Windows launcher (launcher.cpp): "Burnout Revenge.exe", which the
# installer puts beside bin\ and the shortcuts point at. Plain Win32, no Qt.
if(WIN32)
    add_executable(burnout-launcher WIN32 launcher.cpp xerenge.rc)
    set_target_properties(burnout-launcher PROPERTIES OUTPUT_NAME "Burnout Revenge")
    target_link_libraries(burnout-launcher PRIVATE user32)
endif()
