cmake_minimum_required(VERSION 3.16)

project(apu_rpu_ctl VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

# Generic rpmsg char-device transport (git submodule)
add_subdirectory(libs/openamp-helper)

add_executable(apu-rpu-ctl
    src/main.cpp
)

target_compile_features(apu-rpu-ctl PRIVATE cxx_std_17)
target_include_directories(apu-rpu-ctl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(apu-rpu-ctl PRIVATE mnc::rpmsg_chrdev)

install(TARGETS apu-rpu-ctl
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
