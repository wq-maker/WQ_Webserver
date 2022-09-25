CMAKE_MINIMUM_REQUIRED(VERSION 2.7)

PROJECT(tinyWebServer)

FILE(GLOB SRC_FILES
    "${PROJECT_SOURCE_DIR}/*.h"
    "${PROJECT_SOURCE_DIR}/*.cpp"
    "${PROJECT_SOURCE_DIR}/CGImysql/*.h"
    "${PROJECT_SOURCE_DIR}/CGImysql/*.cpp"
    "${PROJECT_SOURCE_DIR}/http/*.h"
    "${PROJECT_SOURCE_DIR}/http/*.cpp"
    "${PROJECT_SOURCE_DIR}/lock/*.h"
    "${PROJECT_SOURCE_DIR}/log/*.h"
    "${PROJECT_SOURCE_DIR}/log/*.cpp"
    "${PROJECT_SOURCE_DIR}/threadpool/*.h"
    "${PROJECT_SOURCE_DIR}/timer/*.h"
    "${PROJECT_SOURCE_DIR}/timer/*.cpp")

ADD_EXECUTABLE(${CMAKE_PROJECT_NAME} ${SRC_FILES})