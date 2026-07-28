# For ESP-IDF build only
# (Top-level project CMakeLists — invokes IDF's project() macro)

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(hunter-roam-espidf)
