# toolchain-clang-cl.cmake
#
# FINAL VERSION: Uses Native clang-cl path flags (/vctoolsdir, /winsdkdir)
# and enables Verbose mode (-v) for troubleshooting header discovery.
#

# --- 0. WINDOWS PATH DEFINITIONS (CRITICAL: VERIFY THESE VALUES!) ---
set(MSVC_VERSION "14.44.35207") 
set(SDK_VERSION "10.0.26100.0")

# Root Paths defined in WSL format (Cleaned up whitespace)
set(MSVC_ROOT_DIR "/mnt/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/${MSVC_VERSION}")
set(WINDOWS_SDK_ROOT "/mnt/c/Program Files (x86)/Windows Kits/10") 

# Derived Paths (Still used for CMake's implicit internal settings, but NOT for /I flags)
set(MSVC_INCLUDE_DIR "${MSVC_ROOT_DIR}/include")
set(SDK_INCLUDE_UCRT_DIR "${WINDOWS_SDK_ROOT}/Include/${SDK_VERSION}/ucrt")
set(SDK_INCLUDE_UM_DIR "${WINDOWS_SDK_ROOT}/Include/${SDK_VERSION}/um")
set(MSVC_LIB_DIR "${MSVC_ROOT_DIR}/lib/x64")
set(SDK_LIB_UCRT_DIR "${WINDOWS_SDK_ROOT}/Lib/${SDK_VERSION}/ucrt/x64")
set(SDK_LIB_UM_DIR "${WINDOWS_SDK_ROOT}/Lib/${SDK_VERSION}/um/x64")


# --- 1. SET THE TARGET SYSTEM ---
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(TRIPLE "x86_64-pc-windows-msvc")


# --- 2. CONFIGURE THE COMPILER ---
set(CMAKE_C_COMPILER clang-cl-22)
set(CMAKE_CXX_COMPILER clang-cl-22)
set(CMAKE_RC_COMPILER llvm-rc)

# Disable the GNU-style dependency flags that clash with clang-cl.
set(CMAKE_C_IMPLICIT_DEPEND_FILTERS "")
set(CMAKE_CXX_IMPLICIT_DEPEND_FILTERS "")

# Force CMake to trust the compilers for cross-compilation
set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_CXX_COMPILER_WORKS TRUE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)


# --- 3. CONFIGURE LINKER AND ARCHIVER ---
set(CMAKE_C_LINK_EXECUTABLE   "<CMAKE_C_COMPILER> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_CXX_LINK_EXECUTABLE "<CMAKE_CXX_COMPILER> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

set(CMAKE_AR llvm-lib)
set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> /nologo /out:<TARGET> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_CREATE "${CMAKE_C_ARCHIVE_CREATE}")
set(CMAKE_C_ARCHIVE_CREATE_DEFAULTS "")
set(CMAKE_C_ARCHIVE_APPEND_DEFAULTS "")
set(CMAKE_CXX_ARCHIVE_CREATE_DEFAULTS "")
set(CMAKE_CXX_ARCHIVE_APPEND_DEFAULTS "")


# --- 4. SET IMPLICIT DIRECTORIES (For CMake's internal checks only) ---

# Implicit Includes
set(MSVC_INCLUDES
    "${MSVC_INCLUDE_DIR}"
    "${SDK_INCLUDE_UCRT_DIR}"
    "${SDK_INCLUDE_UM_DIR}"
    "${WINDOWS_SDK_ROOT}/Include/${SDK_VERSION}/shared" 
)
set(CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES "${MSVC_INCLUDES}")
set(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES "${MSVC_INCLUDES}")

# Implicit Link Directories
set(MSVC_LINK_DIRS
    "${MSVC_LIB_DIR}"
    "${SDK_LIB_UCRT_DIR}"
    "${SDK_LIB_UM_DIR}"
)
set(CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES "${MSVC_LINK_DIRS}")
set(CMAKE_C_IMPLICIT_LINK_DIRECTORIES "${MSVC_LINK_DIRS}")

# Implicit Default Libraries: CLEARED
set(CMAKE_CXX_IMPLICIT_LINK_LIBRARIES "")
set(CMAKE_C_IMPLICIT_LINK_LIBRARIES "")


# --------------------------------------------------------------------------------------------------
# --- 5. COMPILER FLAGS (CRITICAL PATH FLAGS + VERBOSE) ---
# --------------------------------------------------------------------------------------------------
if(NOT CMAKE_IN_LOCAL_ONLY_MODE)
    set(COMPILER_FLAGS
        "--target=${TRIPLE}" 
        "/MD" 
        "/O2" 
        "/Ob2" 
        "/DNDEBUG" 
        "/std:c++17" 
        "/Zc:__cplusplus" 
        "/EHsc"
        
        # FIX: Separating flags from their paths and quoting the paths to handle spaces.
        "/vctoolsdir" "\"${MSVC_ROOT_DIR}\"" 
        "/winsdkdir" "\"${WINDOWS_SDK_ROOT}\"" 
        "/winsdkversion:${SDK_VERSION}"

        # DEBUG: Enable verbose output to print include search paths
        "-v"
    )
    
    # We rely on the /vctoolsdir and /winsdkdir to resolve paths now, 
    # so we explicitly remove the manual /I path logic.
    
    string(REPLACE ";" " " FINAL_COMPILER_FLAGS "${COMPILER_FLAGS}")
    
    set(CMAKE_C_FLAGS "${FINAL_COMPILER_FLAGS}" CACHE STRING "Compiler Flags for C" FORCE)
    set(CMAKE_CXX_FLAGS "${FINAL_COMPILER_FLAGS}" CACHE STRING "Compiler Flags for CXX" FORCE) 
endif() 


# --------------------------------------------------------------------------------------------------
# --- 6. LINKER FLAGS (Must use the new paths for lld-link) ---
# --------------------------------------------------------------------------------------------------
set(LINKER_FLAGS
    # General Linker Directives
    "/Brepro"

    # Linker Path Discovery using /LIBPATH for lld-link
    "/LIBPATH:\"${MSVC_LIB_DIR}\""
    "/LIBPATH:\"${SDK_LIB_UCRT_DIR}\""
    "/LIBPATH:\"${SDK_LIB_UM_DIR}\""

    # Explicit Default Libraries (Crucial for Windows compilation)
    "/DEFAULTLIB:vcruntime.lib"
    "/DEFAULTLIB:ucrt.lib"
    "/DEFAULTLIB:libcmt.lib"
    "/DEFAULTLIB:oldnames.lib"
    "/DEFAULTLIB:kernel32.lib"
    "/DEFAULTLIB:user32.lib"
    "/DEFAULTLIB:gdi32.lib"
    "/DEFAULTLIB:winspool.lib"
    "/DEFAULTLIB:shell32.lib"
    "/DEFAULTLIB:ole32.lib"
    "/DEFAULTLIB:oleaut32.lib"
    "/DEFAULTLIB:uuid.lib"
    "/DEFAULTLIB:comdlg32.lib"
    "/DEFAULTLIB:advapi32.lib"
)

string(REPLACE ";" " " FINAL_LINKER_FLAGS "${LINKER_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS "${FINAL_LINKER_FLAGS}" CACHE STRING "Shared Library Linker Flags" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS "${FINAL_LINKER_FLAGS}" CACHE STRING "Module Linker Flags" FORCE)


# --- 7. CLEANUP LINKAGE VARIABLES ---
set(CMAKE_CXX_LINK_FLAGS "" CACHE STRING "Linker Flags for CXX" FORCE)
set(CMAKE_C_LINK_FLAGS "" CACHE STRING "Linker Flags for C" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "" CACHE STRING "Executable Linker Flags" FORCE) 
set(CMAKE_EXE_LINKER_LIBS "" CACHE STRING "Executable Linker Libraries" FORCE) 
set(CMAKE_SHARED_LIBRARY_LINK_EXCLUDE_TAGS "GNU;UNIX")


# --- 8. CONFIGURE FIND BEHAVIOR ---
set(CMAKE_FIND_ROOT_PATH ${MSVC_ROOT_DIR} ${WINDOWS_SDK_ROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
