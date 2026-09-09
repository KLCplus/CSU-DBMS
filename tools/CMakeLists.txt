ADD_EXECUTABLE(clog_dump clog_dump.cpp)
TARGET_LINK_LIBRARIES(clog_dump observer_static)
TARGET_INCLUDE_DIRECTORIES(clog_dump PRIVATE ${PROJECT_SOURCE_DIR}/src/observer/)

# Target 必须在定义 ADD_EXECUTABLE 之后， programs 不受这个限制
# TARGETS和PROGRAMS 的默认权限是OWNER_EXECUTE, GROUP_EXECUTE, 和WORLD_EXECUTE，即755权限， programs 都是处理脚步类
# 类型分为RUNTIME／LIBRARY／ARCHIVE, prog
# clog_dump remains a developer build target but is not part of the CSUDB product install.
