# The installer: a Qt 6 window over the existing setup scripts (extract-image.py,
# setup-sdk.sh, build.sh). Kept out of the game's own build so the game does not
# need Qt.
#
#   cmake -S installer -B installer/build && cmake --build installer/build
cmake_minimum_required(VERSION 3.21)
project(xerenge_installer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

add_executable(xerenge-installer
    main.cpp
    installer_window.cpp
    installer_window.h
)
target_link_libraries(xerenge-installer PRIVATE Qt6::Widgets Qt6::Concurrent)
