include(FetchContent)

enable_language(ASM)

# Android/Bionic intentionally does not provide the legacy ucontext functions used by the
# desktop POSIX fiber backend. Pin libucontext 1.5.2 and build only its AArch64 context switch core.
FetchContent_Declare(
    quest_libucontext_src
    GIT_REPOSITORY https://github.com/kaniini/libucontext.git
    GIT_TAG 49e671dd52ff6791295d8161ad3b6da7dc5f6f9d
    GIT_SHALLOW FALSE
    GIT_PROGRESS TRUE
    SOURCE_SUBDIR __quest_no_add_subdirectory__
)
FetchContent_MakeAvailable(quest_libucontext_src)

add_library(quest_libucontext STATIC
    "${quest_libucontext_src_SOURCE_DIR}/arch/aarch64/getcontext.S"
    "${quest_libucontext_src_SOURCE_DIR}/arch/aarch64/setcontext.S"
    "${quest_libucontext_src_SOURCE_DIR}/arch/aarch64/swapcontext.S"
    "${quest_libucontext_src_SOURCE_DIR}/arch/aarch64/makecontext.c"
    "${quest_libucontext_src_SOURCE_DIR}/arch/aarch64/trampoline.c")

set_target_properties(quest_libucontext PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(quest_libucontext PUBLIC
    "${quest_libucontext_src_SOURCE_DIR}/include"
    "${quest_libucontext_src_SOURCE_DIR}/arch/aarch64"
    "${quest_libucontext_src_SOURCE_DIR}/arch/aarch64/include"
    "${quest_libucontext_src_SOURCE_DIR}/arch/common"
    "${quest_libucontext_src_SOURCE_DIR}/arch/common/include")
target_compile_definitions(quest_libucontext PRIVATE _DEFAULT_SOURCE=1)
