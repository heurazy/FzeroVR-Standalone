include(FetchContent)

# Populate the exact G-Diffuser source revision without adding its desktop-oriented top-level
# CMake project. The Quest target selectively compiles the decomp/host pieces instead.
FetchContent_Declare(
    gdx_source
    GIT_REPOSITORY https://github.com/Zorkats/G-Diffuser.git
    GIT_TAG 719fd82a3af605b064fb53ad6eecb020090b4c5d
    GIT_SHALLOW FALSE
    GIT_SUBMODULES ""
    GIT_PROGRESS TRUE
    SOURCE_SUBDIR __quest_no_add_subdirectory__
)
FetchContent_MakeAvailable(gdx_source)

# The decomp is a submodule upstream; fetch the exact gitlink explicitly so the build does not
# depend on mutable submodule state.
FetchContent_Declare(
    gdx_decomp
    GIT_REPOSITORY https://github.com/Zorkats/fzerox.git
    GIT_TAG f7fd0fd0242f8dfb5f357f604bb73b6a4e990809
    GIT_SHALLOW FALSE
    GIT_PROGRESS TRUE
    SOURCE_SUBDIR __quest_no_add_subdirectory__
)
FetchContent_MakeAvailable(gdx_decomp)

# mio0_wrap.c delegates to Torch's small libmio0 decoder. Pin the exact Torch gitlink used by
# the selected G-Diffuser commit, but do not add Torch's extractor/application CMake project.
FetchContent_Declare(
    gdx_torch
    GIT_REPOSITORY https://github.com/Zorkats/Torch.git
    GIT_TAG c1bdc6fde97fbaa4495c9e859f635290840a12d3
    GIT_SHALLOW FALSE
    GIT_PROGRESS TRUE
    SOURCE_SUBDIR __quest_no_add_subdirectory__
)
FetchContent_MakeAvailable(gdx_torch)

set(GDX_SOURCE_DIR "${gdx_source_SOURCE_DIR}" CACHE INTERNAL "Pinned G-Diffuser source")
set(GDX_DECOMP_DIR "${gdx_decomp_SOURCE_DIR}" CACHE INTERNAL "Pinned F-Zero X decomp source")
set(GDX_TORCH_DIR "${gdx_torch_SOURCE_DIR}" CACHE INTERNAL "Pinned Torch source")
