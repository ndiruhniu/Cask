cmake_minimum_required(VERSION 3.16)
project(Cask VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

# Core (no GUI) and translation resources.
set(CASK_CORE_SOURCES
    src/core/Config.h          src/core/Config.cpp
    src/core/Component.h       src/core/Component.cpp
    src/core/DesktopIntegration.h  src/core/DesktopIntegration.cpp
    src/core/I18n.h            src/core/I18n.cpp
    src/core/LaunchSettings.h  src/core/LaunchSettings.cpp
    src/core/Runner.h          src/core/Runner.cpp
    src/core/Prefix.h          src/core/Prefix.cpp
    src/core/Program.h         src/core/Program.cpp
    src/core/PrefixManager.h   src/core/PrefixManager.cpp
    src/core/Winetricks.h      src/core/Winetricks.cpp
    resources/cask.qrc
)

add_executable(cask
    src/main.cpp
    ${CASK_CORE_SOURCES}
    src/ui/MainWindow.h      src/ui/MainWindow.cpp
    src/ui/ProgramsPanel.h   src/ui/ProgramsPanel.cpp
    src/ui/IconButton.h
    src/ui/ElidedLabel.h
    src/ui/NewPrefixDialog.h
    src/ui/SettingsDialog.h
    src/ui/EnvDialog.h
    src/ui/WinetricksDialog.h
    src/ui/PrefixPickerDialog.h
)

# Version shown in "About": -DCASK_VERSION=<x> (CI passes the release version or the short commit hash),
# otherwise the short hash of the local checkout, otherwise "dev".
if(NOT CASK_VERSION)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE CASK_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(NOT CASK_VERSION)
        set(CASK_VERSION "dev")
    endif()
endif()
target_compile_definitions(cask PRIVATE CASK_VERSION="${CASK_VERSION}")

target_include_directories(cask PRIVATE src)
target_link_libraries(cask PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)

# Portable layout: Cask/{cask, lib/, data/, prefixes/}
# The binary looks for dependencies in ./lib next to itself.
set_target_properties(cask PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Cask
    BUILD_RPATH   "$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/lib"
)

set(CASK_LAYOUT_DIRS
    lib
    data/runners
    data/dxvk
    data/d7vk
    data/vkd3d-proton
    data/winetricks
    data/mangohud
    prefixes
)

foreach(d ${CASK_LAYOUT_DIRS})
    add_custom_command(TARGET cask POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:cask>/${d}")
    install(DIRECTORY DESTINATION ${d})
endforeach()

install(TARGETS cask RUNTIME DESTINATION .)
