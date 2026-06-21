cmake_minimum_required(VERSION 3.16)

project(apu_rpu_ctl VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

add_executable(apu-rpu-ctl
    src/main.cpp
)

target_compile_features(apu-rpu-ctl PRIVATE cxx_std_17)
target_include_directories(apu-rpu-ctl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

install(TARGETS apu-rpu-ctl
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
