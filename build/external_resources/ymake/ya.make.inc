IF (NOT HOST_OS_DARWIN AND NOT HOST_OS_LINUX AND NOT HOST_OS_WINDOWS)
    MESSAGE(FATAL_ERROR Unsupported host platform for YMAKE)
ENDIF()

DECLARE_EXTERNAL_HOST_RESOURCES_BUNDLE(
    YMAKE
    sbr:1480080712 FOR DARWIN
    sbr:1480080861 FOR LINUX
    sbr:1480080540 FOR WIN32
)