ADD_EXECUTABLE(obclient)
FIND_PACKAGE(jsoncpp CONFIG REQUIRED)
MESSAGE("Begin to build " obclient)

FILE(GLOB_RECURSE ALL_SRC *.cpp)
FOREACH (F ${ALL_SRC})

    SET(PRJ_SRC ${PRJ_SRC} ${F})
    MESSAGE("Use " ${F})

ENDFOREACH (F)

TARGET_SOURCES(obclient PRIVATE ${PRJ_SRC})
TARGET_LINK_LIBRARIES(obclient common pthread dl JsonCpp::JsonCpp)

ADD_EXECUTABLE(csudb)
TARGET_SOURCES(csudb PRIVATE ${PRJ_SRC})
TARGET_LINK_LIBRARIES(csudb common pthread dl JsonCpp::JsonCpp)

ADD_EXECUTABLE(csudb_client)
TARGET_SOURCES(csudb_client PRIVATE ${PRJ_SRC})
SET_TARGET_PROPERTIES(csudb_client PROPERTIES OUTPUT_NAME csudb-client)
TARGET_LINK_LIBRARIES(csudb_client common pthread dl JsonCpp::JsonCpp)

# Target 必须在定义 ADD_EXECUTABLE 之后， programs 不受这个限制
# TARGETS和PROGRAMS 的默认权限是OWNER_EXECUTE, GROUP_EXECUTE, 和WORLD_EXECUTE，即755权限， programs 都是处理脚步类
# 类型分为RUNTIME／LIBRARY／ARCHIVE, prog
INSTALL(TARGETS csudb RUNTIME DESTINATION bin)

# Copies in build/bin make /web work from a development build; libexec is used
# after cmake --install. Both helpers use only the Python standard library.
CONFIGURE_FILE(web/csudb_web.py ${PROJECT_BINARY_DIR}/bin/csudb-web.py COPYONLY)
CONFIGURE_FILE(web/index.html ${PROJECT_BINARY_DIR}/bin/index.html COPYONLY)
CONFIGURE_FILE(${PROJECT_SOURCE_DIR}/sdk/python/csudb.py ${PROJECT_BINARY_DIR}/bin/csudb.py COPYONLY)
INSTALL(PROGRAMS web/csudb_web.py DESTINATION libexec/csudb RENAME csudb-web)
INSTALL(FILES web/index.html ${PROJECT_SOURCE_DIR}/sdk/python/csudb.py DESTINATION libexec/csudb)
INSTALL(FILES ${PROJECT_SOURCE_DIR}/sdk/python/csudb.py
    ${PROJECT_SOURCE_DIR}/sdk/python/__init__.py
    ${PROJECT_SOURCE_DIR}/sdk/python/README.md
    DESTINATION share/csudb/sdk/python)
