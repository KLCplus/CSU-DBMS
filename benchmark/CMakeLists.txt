find_package(benchmark CONFIG REQUIRED)

INCLUDE_DIRECTORIES(${PROJECT_SOURCE_DIR}/src/observer)

FILE(GLOB_RECURSE ALL_SRC *.cpp)
# AUX_SOURCE_DIRECTORY 类似功能
FOREACH (F ${ALL_SRC})
    get_filename_component(prjName ${F} NAME_WE)
    MESSAGE("Build ${prjName} according to ${F}")
    ADD_EXECUTABLE(${prjName} ${F})
    TARGET_LINK_LIBRARIES(${prjName} common pthread dl benchmark::benchmark observer_static oblsm)
    SET_TARGET_PROPERTIES(${prjName} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/benchmark)
ENDFOREACH (F)
