cmake_minimum_required(VERSION 3.22)
project(VocalHarmony VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -----------------------------------------------------------------------------
# JUCE を FetchContent で取得(初回ビルド時にgit cloneされる)
# -----------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# -----------------------------------------------------------------------------
# aubio と Rubber Band を検索
# - Windows + vcpkg: find_path / find_library で直接探す
# - macOS / Linux: pkg-config 経由(brewやaptのインストール先を自動取得)
# -----------------------------------------------------------------------------
add_library(audio_libs INTERFACE)

if(WIN32)
    # vcpkgのtoolchainが指定されていれば、include/libのパスが
    # 自動でCMAKE_PREFIX_PATHに追加される。find_path/find_libraryで取得。
    find_path(AUBIO_INCLUDE_DIR
        NAMES aubio/aubio.h
        REQUIRED
    )
    find_library(AUBIO_LIBRARY
        NAMES aubio
        REQUIRED
    )
    find_path(RUBBERBAND_INCLUDE_DIR
        NAMES rubberband/RubberBandStretcher.h
        REQUIRED
    )
    find_library(RUBBERBAND_LIBRARY
        NAMES rubberband
        REQUIRED
    )

    message(STATUS "aubio include: ${AUBIO_INCLUDE_DIR}")
    message(STATUS "aubio library: ${AUBIO_LIBRARY}")
    message(STATUS "rubberband include: ${RUBBERBAND_INCLUDE_DIR}")
    message(STATUS "rubberband library: ${RUBBERBAND_LIBRARY}")

    target_include_directories(audio_libs INTERFACE
        ${AUBIO_INCLUDE_DIR}
        ${RUBBERBAND_INCLUDE_DIR}
    )
    target_link_libraries(audio_libs INTERFACE
        ${AUBIO_LIBRARY}
        ${RUBBERBAND_LIBRARY}
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(AUBIO      REQUIRED IMPORTED_TARGET aubio)
    pkg_check_modules(RUBBERBAND REQUIRED IMPORTED_TARGET rubberband)

    target_link_libraries(audio_libs INTERFACE
        PkgConfig::AUBIO
        PkgConfig::RUBBERBAND
    )
endif()

# -----------------------------------------------------------------------------
# 実行ファイル(コンソールアプリ)
# -----------------------------------------------------------------------------
juce_add_console_app(VocalHarmony
    PRODUCT_NAME "VocalHarmony"
)

target_sources(VocalHarmony PRIVATE
    src/main.cpp
)

target_compile_definitions(VocalHarmony PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    DONT_SET_USING_JUCE_NAMESPACE=1
)

# -----------------------------------------------------------------------------
# MSVC(Visual Studio)向け: ソースをUTF-8として扱わせる
# 日本語版WindowsのMSVCは、UTF-8ソースをShift-JIS(CP932)と誤認することが
# あり、日本語コメントのバイト列がコードとして誤解釈されてコンパイルが
# 壊れる。/utf-8 フラグで「ソースも実行文字セットもUTF-8」と明示して回避。
# -----------------------------------------------------------------------------
if(MSVC)
    target_compile_options(VocalHarmony PRIVATE /utf-8)
endif()

target_link_libraries(VocalHarmony PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    audio_libs
)

# -----------------------------------------------------------------------------
# 2つ目の実行ファイル: VoiceChanger(ボイスチェンジャー / TRPG・Discord用途)
# 同じ音声処理基盤(JUCE + Rubber Band)を使う。aubio は使わないが、
# audio_libs にまとめてリンクしても害はない。
# -----------------------------------------------------------------------------
juce_add_console_app(VoiceChanger
    PRODUCT_NAME "VoiceChanger"
)

target_sources(VoiceChanger PRIVATE
    src/voice_changer.cpp
)

target_compile_definitions(VoiceChanger PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    DONT_SET_USING_JUCE_NAMESPACE=1
)

if(MSVC)
    target_compile_options(VoiceChanger PRIVATE /utf-8)
endif()

target_link_libraries(VoiceChanger PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    audio_libs
)

# -----------------------------------------------------------------------------
# 注: 実行に必要なDLL(aubio.dll/rubberband.dll等)のコピーは、
# vcpkgツールチェインが applocal.ps1 で自動的に行うため、ここでは何もしない。
# (以前あった手動コピーは TARGET_RUNTIME_DLLS が空になり機能しなかったため削除)
# -----------------------------------------------------------------------------
