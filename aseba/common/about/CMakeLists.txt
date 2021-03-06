if (Qt5Svg_FOUND)
	set(QT_USE_QTSVG ON)
	include_directories(${CMAKE_BINARY_DIR})

	if (QWT_LIBRARIES AND QWT_INCLUDE_DIR)
		include_directories (${QWT_INCLUDE_DIR})
		add_definitions(-DHAVE_QWT)
	endif (QWT_LIBRARIES AND QWT_INCLUDE_DIR)
	if (ENKI_FOUND)
		add_definitions(-DHAVE_ENKI)
	endif (ENKI_FOUND)

	qt5_add_resources(resfiles asebaqtabout.qrc)

	file(GLOB TRANSLATIONS_SOURCES "qtabout*.ts")
	set_source_files_properties(${TRANSLATIONS_SOURCES} PROPERTIES OUTPUT_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}")
	qt5_add_translation(TRANSLATIONS ${TRANSLATIONS_SOURCES} )

	qt5_wrap_cpp(qt_about_MOCS AboutDialog.h)
    add_library(asebaqtabout STATIC AboutDialog.cpp ${qt_about_MOCS} ${resfiles})
	target_link_libraries(asebaqtabout dashel Qt5::Core Qt5::Widgets Qt5::Svg)

	if (HAS_DYN_VERSION)
		add_dependencies(asebaqtabout versionheader)
	endif (HAS_DYN_VERSION)
	set_target_properties(asebaqtabout PROPERTIES VERSION ${LIB_VERSION_STRING})

endif ()
add_feature_info(ABOUT_QT QT_USE_QTSVG "About Windows")
