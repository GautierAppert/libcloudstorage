find_package(Qt5 COMPONENTS Core Gui Quick QuickControls2 Widgets Qml Svg)
if(Qt5Core_FOUND)
    get_target_property(QMAKE_EXECUTABLE Qt5::qmake IMPORTED_LOCATION)
    execute_process(COMMAND ${QMAKE_EXECUTABLE} -query QT_HOST_DATA
        OUTPUT_VARIABLE QT_HOST_DATA
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${QT_HOST_DATA})
endif()

if(Qt5Core_FOUND AND Qt5Gui_FOUND AND Qt5Quick_FOUND AND Qt5QuickControls2_FOUND)
    if(CMAKE_SYSTEM_NAME STREQUAL "Android")
        find_package(Qt5 COMPONENTS AndroidExtras)
        if(Qt5AndroidExtras_FOUND)
            add_subdirectory(cloudbrowser)
        endif()
    else()
        add_subdirectory(cloudbrowser)
    endif()
endif()

if("${CMAKE_SIZEOF_VOID_P}" STREQUAL "4")
    set(osxfuse_LIBRARY_NAME osxfuse)
elseif("${CMAKE_SIZEOF_VOID_P}" STREQUAL "8")
    set(osxfuse_LIBRARY_NAME osxfuse_i64)
endif()

cloudstorage_find_library(fuse INCLUDE_PATH_SUFFIXES fuse3 INCLUDE fuse.h NAMES fuse3)
cloudstorage_find_library(fuse-legacy INCLUDE_PATH_SUFFIXES osxfuse/fuse fuse INCLUDE fuse.h NAMES fuse ${osxfuse_LIBRARY_NAME})
cloudstorage_find_library(dokan2 INCLUDE_PATH_SUFFIXES dokan INCLUDE dokan.h NAMES dokan2)
cloudstorage_find_library(winfsp INCLUDE winfsp/winfsp.h NAMES winfsp)
if(fuse_LIBRARY OR fuse-legacy_LIBRARY OR dokan2_LIBRARY OR winfsp_LIBRARY)
    add_subdirectory(fuse)
endif()