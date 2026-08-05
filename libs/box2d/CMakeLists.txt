cmake_minimum_required(VERSION 3.7)
project(Box2D LANGUAGES C)

file(GLOB source_files src/*.c)

add_library(box2d STATIC ${source_files})

target_include_directories(box2d
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

install(
    TARGETS box2d
    EXPORT box2d
    DESTINATION lib
)
install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/box2d
    DESTINATION include
)
install(
    EXPORT box2d
    DESTINATION share/box2d
)
