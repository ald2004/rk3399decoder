cmake_minimum_required(VERSION 3.18)

project(boerkdecoder)
set (CMAKE_CXX_STANDARD 11)
set(CMAKE_BUILD_TYPE "Debug")


set(DRM_INCLUDE_DIR
        /usr/include/drm
)

include_directories( ${DRM_INCLUDE_DIR})


add_executable(${PROJECT_NAME}   rkmppdec.cc)

target_link_libraries(${PROJECT_NAME} avcodec avformat avutil rockchip_mpp pthread)
