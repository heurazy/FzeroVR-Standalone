include(FetchContent)

# Pin the exact libultraship revision referenced by G-Diffuser commit
# 719fd82a3af605b064fb53ad6eecb020090b4c5d. This keeps Fast3D ABI/resource behavior aligned
# with the decomp port we are integrating rather than following a moving main branch.
set(USE_OPENGLES ON CACHE BOOL "Use OpenGL ES on Quest" FORCE)
set(LUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
set(INCLUDE_MPQ_SUPPORT OFF CACHE BOOL "" FORCE)
set(GFX_DEBUG_DISASSEMBLER OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    gdx_libultraship
    GIT_REPOSITORY https://github.com/Zorkats/libultraship.git
    GIT_TAG a4919b181e637193f2b8ae975e31505abbf99e71
    GIT_SHALLOW FALSE
    GIT_PROGRESS TRUE
)

FetchContent_MakeAvailable(gdx_libultraship)

# Quest standalone audio backend. libultraship's SDL Android player expects SDLActivity/JNI setup,
# while this port is an OpenXR NativeActivity. Replace only Android's SDL player construction with
# a factory implemented by the final Quest DSO and force saved channel mode to stereo. The enum
# remains SDL so existing configuration/UI code stays compatible, while GetBackendName reports AAudio.
file(READ "${gdx_libultraship_SOURCE_DIR}/src/ship/audio/Audio.cpp" _gdx_audio_cpp)
set(_gdx_audio_changed FALSE)
if(NOT _gdx_audio_cpp MATCHES "GdxCreateQuestAAudioPlayer")
    string(REPLACE
        "namespace Ship {"
        "namespace Ship {\n#ifdef __ANDROID__\nstd::shared_ptr<AudioPlayer> GdxCreateQuestAAudioPlayer(AudioSettings settings);\n#endif"
        _gdx_audio_cpp "${_gdx_audio_cpp}")
    string(REPLACE
        "        case AudioBackend::SDL:\n            mAudioPlayer = std::make_shared<SDLAudioPlayer>(this->mAudioSettings);\n            break;"
        "        case AudioBackend::SDL:\n#ifdef __ANDROID__\n            mAudioPlayer = GdxCreateQuestAAudioPlayer(this->mAudioSettings);\n#else\n            mAudioPlayer = std::make_shared<SDLAudioPlayer>(this->mAudioSettings);\n#endif\n            break;"
        _gdx_audio_cpp "${_gdx_audio_cpp}")
    set(_gdx_audio_changed TRUE)
endif()
if(NOT _gdx_audio_cpp MATCHES "Quest NativeActivity always outputs stereo")
    string(REPLACE
        "AudioChannelsSetting Audio::GetSavedAudioChannelsSetting() {\n    int32_t channelsSetting ="
        "AudioChannelsSetting Audio::GetSavedAudioChannelsSetting() {\n#ifdef __ANDROID__\n    // Quest NativeActivity always outputs stereo PCM through the native AAudio player.\n    return AudioChannelsSetting::audioStereo;\n#else\n    int32_t channelsSetting ="
        _gdx_audio_cpp "${_gdx_audio_cpp}")
    string(REPLACE
        "            return AudioChannelsSetting::audioStereo;\n    }\n}\n\n} // namespace Ship"
        "            return AudioChannelsSetting::audioStereo;\n    }\n#endif\n}\n\n} // namespace Ship"
        _gdx_audio_cpp "${_gdx_audio_cpp}")
    set(_gdx_audio_changed TRUE)
endif()
if(_gdx_audio_changed)
    file(WRITE "${gdx_libultraship_SOURCE_DIR}/src/ship/audio/Audio.cpp" "${_gdx_audio_cpp}")
endif()

# Quest VR Fast3D hook, reapplied idempotently after FetchContent has populated/reset the pinned
# libultraship checkout. F-Zero's projection matrix already contains the game camera view, so the
# host supplies a complete per-eye combined projection-view. Ordinary G_NOOP tags inside Race_Draw
# switch the hook between rotation-only background and full 6DoF world without splitting Run().
file(READ "${gdx_libultraship_SOURCE_DIR}/src/fast/interpreter.cpp" _gdx_fast3d_interp)
set(_gdx_fast3d_changed FALSE)
if(NOT _gdx_fast3d_interp MATCHES "gdx_vr_fast3d_get_eye_matrix")
    string(REPLACE
        "extern \"C\" void gdx_dbg_logf(const char* fmt, ...);"
        "extern \"C\" void gdx_dbg_logf(const char* fmt, ...);\nextern \"C\" int gdx_vr_fast3d_get_eye_matrix(float* out16) __attribute__((weak));\nextern \"C\" int gdx_vr_fast3d_is_eye_active(void) __attribute__((weak));\nextern \"C\" void gdx_vr_fast3d_noop_tag(uintptr_t tag) __attribute__((weak));"
        _gdx_fast3d_interp "${_gdx_fast3d_interp}")
    set(_gdx_fast3d_changed TRUE)
endif()
if(NOT _gdx_fast3d_interp MATCHES "gdx_vr_fast3d_is_eye_active")
    string(REPLACE
        "extern \"C\" int gdx_vr_fast3d_get_eye_matrix(float* out16) __attribute__((weak));\nextern \"C\" void gdx_vr_fast3d_noop_tag(uintptr_t tag) __attribute__((weak));"
        "extern \"C\" int gdx_vr_fast3d_get_eye_matrix(float* out16) __attribute__((weak));\nextern \"C\" int gdx_vr_fast3d_is_eye_active(void) __attribute__((weak));\nextern \"C\" void gdx_vr_fast3d_noop_tag(uintptr_t tag) __attribute__((weak));"
        _gdx_fast3d_interp "${_gdx_fast3d_interp}")
    set(_gdx_fast3d_changed TRUE)
endif()
if(NOT _gdx_fast3d_interp MATCHES "Quest VR section tag v2")
    set(_gdx_vr_tag_v1 "    // Quest VR section tag: VRSK/VR3D toggle the matrix source inside one uninterrupted task.\n    if (p == 0 && gdx_vr_fast3d_noop_tag != nullptr) {\n        gdx_vr_fast3d_noop_tag(cmd->words.w1);\n    }")
    set(_gdx_vr_tag_v2 "    // Quest VR section tag v2: VRSK/VR3D toggle the matrix source inside one uninterrupted task.\n    // Recompute MP immediately because Course_Draw can emit vertices before another G_MTX.\n    if (p == 0 && gdx_vr_fast3d_noop_tag != nullptr) {\n        Interpreter* gfx = mInstance.lock().get();\n        gdx_vr_fast3d_noop_tag(cmd->words.w1);\n        if (gfx != nullptr && gfx->mRsp != nullptr && gfx->mRsp->modelview_matrix_stack_size > 0 &&\n            gdx_vr_fast3d_get_eye_matrix != nullptr) {\n            float gdxVrEyeVp[4][4];\n            if (gdx_vr_fast3d_get_eye_matrix(&gdxVrEyeVp[0][0]) != 0) {\n                Interpreter::MatrixMul(gfx->mRsp->MP_matrix,\n                                       gfx->mRsp->modelview_matrix_stack[gfx->mRsp->modelview_matrix_stack_size - 1],\n                                       gdxVrEyeVp);\n            }\n        }\n    }")
    if(_gdx_fast3d_interp MATCHES "Quest VR section tag:")
        string(REPLACE "${_gdx_vr_tag_v1}" "${_gdx_vr_tag_v2}" _gdx_fast3d_interp "${_gdx_fast3d_interp}")
    else()
        string(REPLACE
            "    uint32_t l = C0(0, 16);\n    if (p == 7) {"
            "    uint32_t l = C0(0, 16);\n${_gdx_vr_tag_v2}\n    if (p == 7) {"
            _gdx_fast3d_interp "${_gdx_fast3d_interp}")
    endif()
    set(_gdx_fast3d_changed TRUE)
endif()
if(NOT _gdx_fast3d_interp MATCHES "Quest VR per-eye combined projection-view")
    string(REPLACE
        "    MatrixMul(mRsp->MP_matrix, mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1], mRsp->P_matrix);\n}\n\nvoid Interpreter::GfxSpPopMatrix"
        "    // Quest VR per-eye combined projection-view. The host returns 0 outside tagged race sections.\n    float gdxVrEyeVp[4][4];\n    if (gdx_vr_fast3d_get_eye_matrix != nullptr &&\n        gdx_vr_fast3d_get_eye_matrix(&gdxVrEyeVp[0][0]) != 0) {\n        MatrixMul(mRsp->MP_matrix, mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1], gdxVrEyeVp);\n    } else {\n        MatrixMul(mRsp->MP_matrix, mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1], mRsp->P_matrix);\n    }\n}\n\nvoid Interpreter::GfxSpPopMatrix"
        _gdx_fast3d_interp "${_gdx_fast3d_interp}")
    string(REPLACE
        "                MatrixMul(mRsp->MP_matrix, mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1],\n                          mRsp->P_matrix);"
        "                float gdxVrEyeVp[4][4];\n                if (gdx_vr_fast3d_get_eye_matrix != nullptr &&\n                    gdx_vr_fast3d_get_eye_matrix(&gdxVrEyeVp[0][0]) != 0) {\n                    MatrixMul(mRsp->MP_matrix, mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1], gdxVrEyeVp);\n                } else {\n                    MatrixMul(mRsp->MP_matrix, mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1], mRsp->P_matrix);\n                }"
        _gdx_fast3d_interp "${_gdx_fast3d_interp}")
    set(_gdx_fast3d_changed TRUE)
endif()
if(NOT _gdx_fast3d_interp MATCHES "Quest VR aspect bypass")
    string(REPLACE
        "float Interpreter::AdjXForAspectRatio(float x) const {"
        "float Interpreter::AdjXForAspectRatio(float x) const {\n    // Quest VR aspect bypass: the OpenXR per-eye projection already contains the exact asymmetric\n    // eye FOV/aspect. Applying libultraship's normal 4:3->window Hor+ correction again compresses\n    // clip-space X a second time and creates the visible horizontal/fisheye deformation.\n    if (gdx_vr_fast3d_is_eye_active != nullptr && gdx_vr_fast3d_is_eye_active() != 0) {\n        return x;\n    }"
        _gdx_fast3d_interp "${_gdx_fast3d_interp}")
    set(_gdx_fast3d_changed TRUE)
endif()
if(_gdx_fast3d_changed)
    file(WRITE "${gdx_libultraship_SOURCE_DIR}/src/fast/interpreter.cpp" "${_gdx_fast3d_interp}")
endif()

# G-Diffuser's decomp calls the N64 Rumble Pak symbol __osMotorAccess directly. libultraship's
# desktop implementation routes that to ControlDeck/SDL controllers, which the native Quest host
# intentionally bypasses. Rename only libultraship's copy so the Quest DSO can provide the real
# __osMotorAccess symbol backed by OpenXR haptics.
if(TARGET libultraship)
    # The Android Gradle debug variant injects -g but no optimization into fetched libultraship.
    # That left Fast3D's command interpreter/backend at -O0 even though it executes every N64 Gfx
    # command twice for stereo. Preserve debug symbols while compiling the renderer hot path at -O3.
    # The option is intentionally target-local so Quest glue remains independently debuggable.
    target_compile_options(libultraship PRIVATE -O3)
    target_compile_definitions(libultraship PRIVATE
        __osMotorAccess=gdx_lus_desktop_motor_access
        SDL_AndroidGetExternalStoragePath=gdx_quest_android_storage_path
    )
endif()
