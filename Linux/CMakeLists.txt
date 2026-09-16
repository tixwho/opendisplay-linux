cmake_minimum_required(VERSION 3.24)

project(opendisplay-linux VERSION 0.1.0 LANGUAGES C CXX)

# C++23 for std::string::resize_and_overwrite(), which keeps the NV12 packing
# from zero-filling megabytes it overwrites immediately.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles([=[
    #include <string>
    int main() {
        std::string value;
        value.resize_and_overwrite(1, [](char* data, std::size_t) {
            data[0] = 'x';
            return std::size_t{1};
        });
    }
]=] OPENDISPLAY_HAS_RESIZE_AND_OVERWRITE)
if(NOT OPENDISPLAY_HAS_RESIZE_AND_OVERWRITE)
    message(FATAL_ERROR
        "OpenDisplay requires a C++23 standard library with std::string::resize_and_overwrite.")
endif()
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core DBus Gui)
find_package(KF6Screen 6.3 REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)
pkg_check_modules(USBMUXD REQUIRED IMPORTED_TARGET libusbmuxd-2.0)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
# libswscale 8.12.100 and later run sws_scale_frame() on an uninitialized
# public SwsContext, which the NV12 converter relies on.
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale>=8.12.100)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(OPENDISPLAY_BUILD_GUI AUTO CACHE STRING
    "Build the Kirigami GUI (AUTO, ON, or OFF)")
set_property(CACHE OPENDISPLAY_BUILD_GUI PROPERTY STRINGS AUTO ON OFF)

set(VIRTUAL_POINTER_XML
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-virtual-pointer-unstable-v1.xml)
set(PROTOCOL_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(VIRTUAL_POINTER_HEADER
    ${PROTOCOL_OUTPUT_DIR}/wlr-virtual-pointer-unstable-v1-client-protocol.h)
set(VIRTUAL_POINTER_CODE
    ${PROTOCOL_OUTPUT_DIR}/wlr-virtual-pointer-unstable-v1-protocol.c)
add_custom_command(
    OUTPUT ${VIRTUAL_POINTER_HEADER} ${VIRTUAL_POINTER_CODE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTOCOL_OUTPUT_DIR}
    COMMAND ${WAYLAND_SCANNER} client-header ${VIRTUAL_POINTER_XML} ${VIRTUAL_POINTER_HEADER}
    COMMAND ${WAYLAND_SCANNER} private-code ${VIRTUAL_POINTER_XML} ${VIRTUAL_POINTER_CODE}
    DEPENDS ${VIRTUAL_POINTER_XML}
    VERBATIM
)

set(OPENDISPLAY_CORE_SOURCES
    include/opendisplay/kde_portal.hpp
    include/opendisplay/hyprland_portal.hpp
    include/opendisplay/xdg_portal.hpp
    ${VIRTUAL_POINTER_HEADER}
    ${VIRTUAL_POINTER_CODE}
    src/desktop_backend_factory.cpp
    src/display_layout.cpp
    src/discovery.cpp
    src/ffmpeg_encoder.cpp
    src/kde_output_controller.cpp
    src/kde_portal.cpp
    src/hyprland_output_controller.cpp
    src/hyprland_portal.cpp
    src/nv12_converter.cpp
    src/pipewire_capture.cpp
    src/pipewire_format.cpp
    src/session.cpp
    src/socket.cpp
    src/wire.cpp
    src/wayland_virtual_pointer.cpp
    src/xdg_portal.cpp
)

add_library(opendisplay-core STATIC ${OPENDISPLAY_CORE_SOURCES})
set_target_properties(opendisplay-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(opendisplay-core PUBLIC include PRIVATE ${PROTOCOL_OUTPUT_DIR})
target_link_libraries(opendisplay-core PUBLIC
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    KF6::Screen
    PkgConfig::PIPEWIRE
    PkgConfig::AVAHI
    PkgConfig::USBMUXD
    PkgConfig::WAYLAND
    PkgConfig::FFMPEG
    Threads::Threads
)

target_compile_options(opendisplay-core PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

if(KF6Screen_VERSION VERSION_GREATER_EQUAL "6.6")
    target_compile_definitions(opendisplay-core PRIVATE OD_KSCREEN_HAS_CUSTOM_MODES=1)
else()
    target_compile_definitions(opendisplay-core PRIVATE OD_KSCREEN_HAS_CUSTOM_MODES=0)
endif()
target_compile_definitions(opendisplay-core PRIVATE
    OD_KSCREEN_VERSION_STRING="${KF6Screen_VERSION}"
)

add_executable(opendisplay-linux src/main.cpp)
target_link_libraries(opendisplay-linux PRIVATE opendisplay-core)
target_compile_options(opendisplay-linux PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

if(NOT OPENDISPLAY_BUILD_GUI STREQUAL "OFF")
    find_package(Qt6 6.5 QUIET COMPONENTS Quick Qml QuickControls2 Widgets)
    find_package(KF6Kirigami 6.3 QUIET)
    if(TARGET Qt6::Quick AND TARGET Qt6::Qml AND TARGET Qt6::QuickControls2
       AND TARGET Qt6::Widgets AND TARGET KF6::Kirigami)
        qt_policy(SET QTP0001 NEW)
        if(Qt6_VERSION VERSION_GREATER_EQUAL "6.8")
            qt_policy(SET QTP0004 NEW)
        endif()
        set_source_files_properties(gui/qml/Main.qml PROPERTIES
            QT_RESOURCE_ALIAS Main.qml)
        qt_add_executable(opendisplay-gui
            gui/gui_controller.cpp
            gui/gui_controller.hpp
            gui/gui_main.cpp
            gui/session_worker.cpp
            gui/session_worker.hpp
        )
        qt_add_qml_module(opendisplay-gui
            URI org.opendisplay.desktop
            VERSION 1.0
            NO_IMPORT_SCAN
            QML_FILES gui/qml/Main.qml
        )
        target_link_libraries(opendisplay-gui PRIVATE
            opendisplay-core
            Qt6::Quick
            Qt6::Qml
            Qt6::QuickControls2
            Qt6::Widgets
            KF6::Kirigami
        )
        target_compile_options(opendisplay-gui PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        )
        install(TARGETS opendisplay-gui RUNTIME DESTINATION bin)
        install(FILES gui/org.opendisplay.desktop.desktop
            DESTINATION share/applications)
        install(FILES ../public/icon-256.png
            DESTINATION share/icons/hicolor/256x256/apps
            RENAME org.opendisplay.desktop.png)
    elseif(OPENDISPLAY_BUILD_GUI STREQUAL "ON")
        message(FATAL_ERROR
            "The GUI requires Qt 6 Quick/Widgets and KF6Kirigami. "
            "Install qt6-declarative and kirigami on Arch Linux.")
    else()
        message(STATUS
            "Qt Quick or KF6Kirigami was not found; building the CLI only")
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(opendisplay-display-layout-tests
        tests/display_layout_tests.cpp
        src/display_layout.cpp
    )
    target_include_directories(opendisplay-display-layout-tests PRIVATE include)
    target_compile_options(opendisplay-display-layout-tests PRIVATE -UNDEBUG)
    add_test(NAME display-layout COMMAND opendisplay-display-layout-tests)

    add_executable(opendisplay-desktop-backend-tests
        tests/desktop_backend_tests.cpp
        src/desktop_backend_factory.cpp
        src/hyprland_output_controller.cpp
    )
    target_include_directories(opendisplay-desktop-backend-tests PRIVATE include)
    target_link_libraries(opendisplay-desktop-backend-tests PRIVATE Qt6::Core)
    target_compile_definitions(opendisplay-desktop-backend-tests PRIVATE
        OD_DESKTOP_FACTORY_TEST_ONLY=1)
    target_compile_options(opendisplay-desktop-backend-tests PRIVATE -UNDEBUG)
    add_test(NAME desktop-backend COMMAND opendisplay-desktop-backend-tests)

    add_executable(opendisplay-wire-tests
        tests/wire_tests.cpp
        src/wire.cpp
    )
    target_include_directories(opendisplay-wire-tests PRIVATE include)
    target_link_libraries(opendisplay-wire-tests PRIVATE Qt6::Core)
    target_compile_options(opendisplay-wire-tests PRIVATE -UNDEBUG)
    add_test(NAME wire COMMAND opendisplay-wire-tests)

    add_executable(opendisplay-encoder-tests
        tests/encoder_tests.cpp
        src/ffmpeg_encoder.cpp
        src/wire.cpp
    )
    target_include_directories(opendisplay-encoder-tests PRIVATE include)
    target_link_libraries(opendisplay-encoder-tests PRIVATE
        Qt6::Core PkgConfig::FFMPEG Threads::Threads)
    target_compile_options(opendisplay-encoder-tests PRIVATE -UNDEBUG)
    add_test(NAME encoder COMMAND opendisplay-encoder-tests)
    set_tests_properties(encoder PROPERTIES TIMEOUT 15)

    add_executable(opendisplay-nv12-converter-tests
        tests/nv12_converter_tests.cpp
        src/nv12_converter.cpp
    )
    target_include_directories(opendisplay-nv12-converter-tests PRIVATE include)
    target_link_libraries(opendisplay-nv12-converter-tests PRIVATE PkgConfig::FFMPEG)
    target_compile_options(opendisplay-nv12-converter-tests PRIVATE -UNDEBUG)
    add_test(NAME nv12-converter COMMAND opendisplay-nv12-converter-tests)
    set_tests_properties(nv12-converter PROPERTIES TIMEOUT 30)

    add_executable(opendisplay-pipewire-format-tests
        tests/pipewire_format_tests.cpp
        src/pipewire_format.cpp
    )
    target_include_directories(opendisplay-pipewire-format-tests PRIVATE include)
    target_link_libraries(opendisplay-pipewire-format-tests PRIVATE PkgConfig::PIPEWIRE)
    target_compile_options(opendisplay-pipewire-format-tests PRIVATE -UNDEBUG)
    add_test(NAME pipewire-format COMMAND opendisplay-pipewire-format-tests)

    add_executable(opendisplay-socket-tests
        tests/socket_tests.cpp
        src/socket.cpp
    )
    target_include_directories(opendisplay-socket-tests PRIVATE include)
    target_link_libraries(opendisplay-socket-tests PRIVATE PkgConfig::USBMUXD Threads::Threads)
    target_compile_options(opendisplay-socket-tests PRIVATE -UNDEBUG)
    add_test(NAME socket COMMAND opendisplay-socket-tests)
    set_tests_properties(socket PROPERTIES TIMEOUT 5)
endif()

install(TARGETS opendisplay-linux RUNTIME DESTINATION bin)
