cmake_minimum_required(VERSION 3.20)
project(KharvoxBlenderContacts LANGUAGES NONE)

include(CTest)
find_program(KHARVOX_BLENDER_EXECUTABLE NAMES blender blender.exe
    HINTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../.tools/blender-5.2.3/blender-5.2.3-windows-x64"
    REQUIRED)
add_test(NAME shotgun-contacts-offline
    COMMAND "${KHARVOX_BLENDER_EXECUTABLE}"
        --background --factory-startup --python-exit-code 1
        --python "${CMAKE_CURRENT_SOURCE_DIR}/test_shotgun_contacts.py")
set_tests_properties(shotgun-contacts-offline PROPERTIES TIMEOUT 60)
