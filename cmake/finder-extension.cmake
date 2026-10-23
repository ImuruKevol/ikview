# Native Finder Sync extension; no Qt runtime, filesystem scans, or daemon.
# Include after the ikview target and include(CTest) have been declared.
if(NOT APPLE)
  return()
endif()
set(IKVIEW_FINDER_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/IKViewFinder.appex")
target_sources(ikview PRIVATE assets/integration/finder-integration assets/finder/entitlements.plist)
set_source_files_properties(assets/integration/finder-integration assets/finder/entitlements.plist
  PROPERTIES MACOSX_PACKAGE_LOCATION Resources/integration)
file(MAKE_DIRECTORY "${IKVIEW_FINDER_BUNDLE}/Contents")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/finder/Info.plist.in"
               "${IKVIEW_FINDER_BUNDLE}/Contents/Info.plist" @ONLY)
add_executable(ikview_finder src/finder/IKViewFinder.mm src/finder/IKViewFinder.h)
set_target_properties(ikview_finder PROPERTIES
  OUTPUT_NAME IKViewFinder
  AUTOMOC OFF AUTORCC OFF
  LINK_DEPENDS "${IKVIEW_FINDER_BUNDLE}/Contents/Info.plist;${CMAKE_CURRENT_SOURCE_DIR}/assets/finder/entitlements.plist"
  RUNTIME_OUTPUT_DIRECTORY "${IKVIEW_FINDER_BUNDLE}/Contents/MacOS")
target_compile_options(ikview_finder PRIVATE -fobjc-arc -fapplication-extension -Wall -Wextra -Wpedantic)
target_link_libraries(ikview_finder PRIVATE "-framework AppKit" "-framework FinderSync" "-framework Foundation")
target_link_options(ikview_finder PRIVATE -fapplication-extension "-Wl,-e,_NSExtensionMain")
add_custom_command(TARGET ikview_finder POST_BUILD
  COMMAND /usr/bin/codesign --force --sign - --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/assets/finder/entitlements.plist" "${IKVIEW_FINDER_BUNDLE}"
  VERBATIM)
add_dependencies(ikview ikview_finder)
add_custom_command(TARGET ikview POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:ikview>/Contents/PlugIns"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${IKVIEW_FINDER_BUNDLE}" "$<TARGET_BUNDLE_DIR:ikview>/Contents/PlugIns/IKViewFinder.appex"
  VERBATIM)
# Keep an existing app current when only extension code or its plist changed.
add_custom_target(ikview_finder_embed ALL
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${IKVIEW_FINDER_BUNDLE}" "$<TARGET_BUNDLE_DIR:ikview>/Contents/PlugIns/IKViewFinder.appex"
  DEPENDS ikview ikview_finder VERBATIM)
if(BUILD_TESTING)
  add_executable(finder_extension_tests tests/finder_extension_tests.mm src/finder/IKViewFinder.mm src/finderhandoff.cpp)
  set_target_properties(finder_extension_tests PROPERTIES AUTOMOC OFF AUTORCC OFF)
  target_include_directories(finder_extension_tests PRIVATE src/finder src)
  target_compile_options(finder_extension_tests PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
  target_link_libraries(finder_extension_tests PRIVATE Qt6::Core "-framework AppKit" "-framework FinderSync" "-framework Foundation")
  add_test(NAME finder_extension_tests COMMAND finder_extension_tests)
  set_tests_properties(finder_extension_tests PROPERTIES TIMEOUT 20)
endif()
