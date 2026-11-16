cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../nema
                         ${CMAKE_CURRENT_LIST_DIR}/../../system
                         ${CMAKE_CURRENT_LIST_DIR}/../../apps
                         ${CMAKE_CURRENT_LIST_DIR}/../../platform/esp-idf/palanu
                         ${CMAKE_CURRENT_LIST_DIR}/../../third_party/wamr/build-scripts/esp-idf)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(palanuos)
