// =============================================================================
// VocalHarmony
// Copyright (C) 2026 totalsky3
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See <https://www.gnu.org/licenses/>.
// =============================================================================
//
// VocalHarmony - Phase 1 第三段階: diatonicハモリ + 音域調整
//
// マイク入力からF0を検出し、曲のキー/スケールに合った「スケール準拠の
// ハモリ音」を計算する。さらに、ハモリ音が歌い手の快適音域を外れる場合は
// 自動でオクターブを補正し、男声の上ハモリが不自然になる問題を回避する。
//
// 第二段階との違い:
//   第二段階 = 固定音程(常に+4半音など)でピッチシフト
//   第三段階 = F0を見て、スケール上の目標音を動的に決め、
//              ピッチ比を毎フレーム更新する。さらに音域補正を行う。
// =============================================================================

#include <juce_audio_utils/juce_audio_utils.h>
#include <aubio/aubio.h>
#include <rubberband/RubberBandStretcher.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>


// =============================================================================
// ★ デバイス設定 ★
// =============================================================================
namespace config
{
    const juce::String deviceType        = "";
    const juce::String inputDeviceName   = "Microphone Array (AMD Audio Device)";
    const juce::String outputDeviceName  = "";
    const double       sampleRate        = 44100.0;
    const int          bufferSize        = 256;
}


// =============================================================================
// ★ ハモリ設定 ★
// =============================================================================
namespace harmony
{
    // --- キーとスケール ---
    // key_root_midi: キーの主音のMIDIノート番号
    //   C4=60, A3=57, B♭3=58, G#3=56, E♭3=51 など
    // scale: スケールの度数(半音単位、長さ7)
    //   メジャー      : {0,2,4,5,7,9,11}
    //   自然短音階    : {0,2,3,5,7,8,10}
    //   和声短音階    : {0,2,3,5,7,8,11}
    const int              keyRootMidi = 57;                     // A3
    const std::vector<int> scale       = {0,2,3,5,7,8,10};       // 自然短音階
    const int              degreeShift = -2;                     // 下3度

    // --- ミックス ---
    const float dryVolume = 0.5f;
    const float wetVolume = 0.5f;
    const bool  preserveFormant = true;

    // --- 音域調整(男声でも破綻しないための自動オクターブ補正)---
    // ハモリ音がこの範囲を外れたら、範囲内に収まるまでオクターブ移動する。
    // 目安:
    //   男声  : comfortMinHz=110(A2), comfortMaxHz=330(E4)
    //   女声  : comfortMinHz=196(G3), comfortMaxHz=587(D5)
    // 自分の歌える快適音域に合わせて調整する。
    const bool  enableRangeAdjust = true;
    const float comfortMinHz      = 110.0f;   // A2
    const float comfortMaxHz      = 330.0f;   // E4
}


// =============================================================================
// diatonicハモリ音の計算(Pythonのdiatonic_harmony_f0をC++移植)
// =============================================================================
// 入力F0(Hz)から、スケール準拠のハモリ音(Hz)を計算する。
// f0<=0(無声)のときは 0 を返す。
static float computeDiatonicHarmony(float f0,
                                    int keyRootMidi,
                                    const std::vector<int>& scale,
                                    int degreeShift)
{
    if (f0 <= 0.0f) return 0.0f;

    const int nScale = static_cast<int>(scale.size());

    // Hz → MIDIノート番号
    const float midi = 69.0f + 12.0f * std::log2(f0 / 440.0f);

    // キー主音からの半音差を「オクターブ + ピッチクラス」に分解
    const int semiFromRoot = static_cast<int>(std::lround(midi - keyRootMidi));
    int octave = semiFromRoot / 12;
    int pc      = semiFromRoot % 12;
    if (pc < 0) { pc += 12; octave -= 1; }   // C++の負数剰余を補正

    // スケール内で最も近い音を探す
    int nearestIdx = 0;
    int bestDist   = 128;
    for (int k = 0; k < nScale; ++k)
    {
        const int d = std::abs(scale[k] - pc);
        if (d < bestDist) { bestDist = d; nearestIdx = k; }
    }

    // スケール度数をdegreeShiftだけ動かす(オクターブまたぎ処理)
    int newIdx    = nearestIdx + degreeShift;
    int newOctave = octave;
    // floor除算でインデックスとオクターブ繰り上げ/繰り下げを算出
    while (newIdx < 0)       { newIdx += nScale; newOctave -= 1; }
    while (newIdx >= nScale) { newIdx -= nScale; newOctave += 1; }

    const int newPc   = scale[newIdx];
    const int newMidi = keyRootMidi + newOctave * 12 + newPc;

    // MIDI → Hz
    return 440.0f * std::pow(2.0f, (newMidi - 69) / 12.0f);
}


// =============================================================================
// 音域調整: ハモリ音が快適音域を外れたらオクターブ単位で補正する
// =============================================================================
static float adjustToVocalRange(float harmonyHz,
                                float minHz, float maxHz)
{
    if (harmonyHz <= 0.0f) return harmonyHz;

    // 上限を超えていたら1オクターブずつ下げる
    while (harmonyHz > maxHz)
        harmonyHz *= 0.5f;
    // 下限を下回っていたら1オクターブずつ上げる
    while (harmonyHz < minHz)
        harmonyHz *= 2.0f;

    return harmonyHz;
}


// =============================================================================
// 利用可能なオーディオデバイスを一覧表示する
// =============================================================================
static void listAudioDevices(juce::AudioDeviceManager& dm)
{
    std::cout << "=============== 利用可能なオーディオデバイス ===============\n";
    const auto& types = dm.getAvailableDeviceTypes();
    for (auto* type : types)
    {
        type->scanForDevices();
        std::cout << "\n[API: " << type->getTypeName().toStdString() << "]\n";

        std::cout << "  -- 入力(マイク) --\n";
        const auto inputs = type->getDeviceNames(true);
        if (inputs.isEmpty()) std::cout << "    (なし)\n";
        for (int i = 0; i < inputs.size(); ++i)
            std::cout << "    [" << i << "] " << inputs[i].toStdString() << "\n";

        std::cout << "  -- 出力(スピーカー) --\n";
        const auto outputs = type->getDeviceNames(false);
        if (outputs.isEmpty()) std::cout << "    (なし)\n";
        for (int i = 0; i < outputs.size(); ++i)
            std::cout << "    [" << i << "] " << outputs[i].toStdString() << "\n";
    }
    std::cout << "==========================================================\n\n";
}


// =============================================================================
// HarmonyProcessor
// =============================================================================
class HarmonyProcessor : public juce::AudioIODeviceCallback
{
public:
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        sampleRate_ = static_cast<int>(device->getCurrentSampleRate());
        bufferSize_ = device->getCurrentBufferSizeSamples();

        // ----- aubio (F0検出) -----
        const uint_t hopSize = static_cast<uint_t>(bufferSize_);
        const uint_t winSize = std::max<uint_t>(2048, hopSize * 2);

        pitchDetector_ = new_aubio_pitch("yin", winSize, hopSize,
                                          static_cast<uint_t>(sampleRate_));
        if (pitchDetector_ != nullptr)
        {
            aubio_pitch_set_unit(pitchDetector_, "Hz");
            aubio_pitch_set_silence(pitchDetector_, -30.0f);
            aubio_pitch_set_tolerance(pitchDetector_, 0.15f);
        }
        inputVec_  = new_fvec(hopSize);
        outputVec_ = new_fvec(1);

        // ----- Rubber Band (ピッチシフト) -----
        using RB = RubberBand::RubberBandStretcher;
        int options =
            RB::OptionProcessRealTime
            | RB::OptionEngineFaster
            | RB::OptionPitchHighConsistency
            | RB::OptionThreadingNever
            | RB::OptionWindowShort;
        if (harmony::preserveFormant)
            options |= RB::OptionFormantPreserved;

        stretcher_ = std::make_unique<RB>(
            static_cast<size_t>(sampleRate_), 1, options);
        stretcher_->setPitchScale(1.0);   // 初期値は等倍

        latencySamples_ = stretcher_->getLatency();
        wetBuffer_.assign(bufferSize_, 0.0f);
    }

    void audioDeviceStopped() override
    {
        if (pitchDetector_) { del_aubio_pitch(pitchDetector_); pitchDetector_ = nullptr; }
        if (inputVec_)      { del_fvec(inputVec_);             inputVec_      = nullptr; }
        if (outputVec_)     { del_fvec(outputVec_);            outputVec_     = nullptr; }
        stretcher_.reset();
    }

    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData, int numInputChannels,
        float* const* outputChannelData, int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context) override
    {
        juce::ignoreUnused(context);

        for (int ch = 0; ch < numOutputChannels; ++ch)
            std::memset(outputChannelData[ch], 0, sizeof(float) * (size_t)numSamples);

        if (numInputChannels == 0 || pitchDetector_ == nullptr || !stretcher_)
            return;

        const float* dry = inputChannelData[0];

        // ===== 1. F0検出 =====
        const int copySize = std::min(numSamples, (int)inputVec_->length);
        for (int i = 0; i < copySize; ++i)
            inputVec_->data[i] = dry[i];

        aubio_pitch_do(pitchDetector_, inputVec_, outputVec_);
        const float f0   = outputVec_->data[0];
        const float conf = aubio_pitch_get_confidence(pitchDetector_);
        latestPitch_.store(f0);
        latestConfidence_.store(conf);

        // ===== 2. diatonicハモリ音の計算 + 音域調整 =====
        // 信頼度が十分あり、かつF0が人間の声の範囲内のときだけ目標を更新する。
        // 低信頼(子音・息継ぎ)や異常値では直前のpitchScaleを維持して暴れを防ぐ。
        //
        // F0_MIN/F0_MAX: 人間の歌声の妥当な範囲。aubioがまれに出す
        //   超高値(数万Hz)や極端な低値を弾き、Rubber Bandが極端な
        //   ピッチ比でバッファ再確保する事故を防ぐ。
        constexpr float kF0Min = 65.0f;    // C2付近(低い男声)
        constexpr float kF0Max = 1200.0f;  // D6付近(高い女声・裏声)

        if (f0 >= kF0Min && f0 <= kF0Max && conf > 0.5f)
        {
            float targetHz = computeDiatonicHarmony(
                f0, harmony::keyRootMidi, harmony::scale, harmony::degreeShift);

            if (harmony::enableRangeAdjust && targetHz > 0.0f)
                targetHz = adjustToVocalRange(
                    targetHz, harmony::comfortMinHz, harmony::comfortMaxHz);

            if (targetHz > 0.0f)
            {
                double ratio = static_cast<double>(targetHz) / static_cast<double>(f0);

                // ピッチ比を安全な範囲にクランプ(念のための二重の安全装置)
                ratio = std::min(std::max(ratio, 0.25), 4.0);

                // 微小な変化ではsetPitchScaleを呼ばない。
                // Rubber Bandはピッチ比変更のたびに内部を再構成しうるので、
                // 意味のある変化(0.5%以上)があるときだけ更新して警告を防ぐ。
                if (std::abs(ratio - currentRatio_) / currentRatio_ > 0.005)
                {
                    stretcher_->setPitchScale(ratio);
                    currentRatio_ = ratio;
                }

                latestTargetHz_.store(targetHz);
                latestRatio_.store(static_cast<float>(ratio));
            }
        }

        // ===== 3. ピッチシフト実行 =====
        if ((int)wetBuffer_.size() < numSamples)
            wetBuffer_.resize(numSamples);

        const float* inputPtrs[1]  = { dry };
        float*       wetPtr        = wetBuffer_.data();
        float*       outputPtrs[1] = { wetPtr };

        stretcher_->process(inputPtrs, (size_t)numSamples, false);

        const size_t available  = stretcher_->available();
        const size_t toRetrieve = std::min(available, (size_t)numSamples);
        if (toRetrieve > 0)
            stretcher_->retrieve(outputPtrs, toRetrieve);
        for (size_t i = toRetrieve; i < (size_t)numSamples; ++i)
            wetPtr[i] = 0.0f;

        // ===== 4. ミックス出力 =====
        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            float* out = outputChannelData[ch];
            for (int i = 0; i < numSamples; ++i)
                out[i] = harmony::dryVolume * dry[i]
                       + harmony::wetVolume * wetPtr[i];
        }
    }

    float  getLatestPitch()      const { return latestPitch_.load(); }
    float  getLatestConfidence() const { return latestConfidence_.load(); }
    float  getLatestTargetHz()   const { return latestTargetHz_.load(); }
    float  getLatestRatio()      const { return latestRatio_.load(); }
    size_t getLatencySamples()   const { return latencySamples_; }

private:
    aubio_pitch_t* pitchDetector_ = nullptr;
    fvec_t*        inputVec_      = nullptr;
    fvec_t*        outputVec_     = nullptr;

    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher_;
    std::vector<float> wetBuffer_;
    size_t             latencySamples_ = 0;
    double             currentRatio_   = 1.0;   // 直近に設定したピッチ比

    int sampleRate_ = 0;
    int bufferSize_ = 0;

    std::atomic<float> latestPitch_      { 0.0f };
    std::atomic<float> latestConfidence_ { 0.0f };
    std::atomic<float> latestTargetHz_   { 0.0f };
    std::atomic<float> latestRatio_      { 1.0f };
};


// =============================================================================
// Hz → 音名
// =============================================================================
static std::string hzToNoteName(float hz)
{
    if (hz <= 0.0f) return "---";
    static const char* names[] =
        { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    const float midiF  = 69.0f + 12.0f * std::log2(hz / 440.0f);
    const int   midi   = static_cast<int>(std::round(midiF));
    const int   octave = midi / 12 - 1;
    const int   pc     = ((midi % 12) + 12) % 12;
    return std::string(names[pc]) + std::to_string(octave);
}


// =============================================================================
// main
// =============================================================================
int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::AudioDeviceManager deviceManager;

    listAudioDevices(deviceManager);

    const auto initErr = deviceManager.initialise(1, 2, nullptr, true);
    if (initErr.isNotEmpty())
    {
        std::cerr << "オーディオデバイス初期化失敗: " << initErr << std::endl;
        return 1;
    }

    if (config::deviceType.isNotEmpty())
        deviceManager.setCurrentAudioDeviceType(config::deviceType, true);

    auto setup = deviceManager.getAudioDeviceSetup();
    if (config::inputDeviceName.isNotEmpty())  setup.inputDeviceName  = config::inputDeviceName;
    if (config::outputDeviceName.isNotEmpty()) setup.outputDeviceName = config::outputDeviceName;
    if (config::sampleRate > 0)                setup.sampleRate       = config::sampleRate;
    if (config::bufferSize > 0)                setup.bufferSize       = config::bufferSize;
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    setup.inputChannels.setBit(0);

    const auto setupErr = deviceManager.setAudioDeviceSetup(setup, true);
    if (setupErr.isNotEmpty())
    {
        std::cerr << "デバイス設定失敗: " << setupErr << std::endl;
        std::cerr << "config のデバイス名が正しいか、上の一覧と照合してください。\n";
        return 1;
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        std::cerr << "利用可能なオーディオデバイスがありません" << std::endl;
        return 1;
    }

    std::cout << "===== VocalHarmony - Diatonic Harmony (Stage 3) =====\n";
    std::cout << "Device        : " << device->getName().toStdString() << "\n";
    std::cout << "Sample rate   : " << device->getCurrentSampleRate() << " Hz\n";
    std::cout << "Buffer size   : " << device->getCurrentBufferSizeSamples() << " samples\n";

    std::cout << "\n--- Harmony Settings ---\n";
    std::cout << "Key root MIDI : " << harmony::keyRootMidi
              << " (" << hzToNoteName(440.0f * std::pow(2.0f, (harmony::keyRootMidi - 69) / 12.0f))
              << ")\n";
    std::cout << "Degree shift  : " << std::showpos << harmony::degreeShift << std::noshowpos
              << " (スケール度数)\n";
    std::cout << "Mix (dry/wet) : " << std::setprecision(2)
              << harmony::dryVolume << " / " << harmony::wetVolume << "\n";
    std::cout << "Formant       : "
              << (harmony::preserveFormant ? "保持" : "シフト") << "\n";
    std::cout << "Range adjust  : "
              << (harmony::enableRangeAdjust ? "有効" : "無効")
              << " [" << hzToNoteName(harmony::comfortMinHz)
              << " - " << hzToNoteName(harmony::comfortMaxHz) << "]\n";
    std::cout << "=====================================================\n\n";

    std::cout << "【重要】ヘッドホン/イヤホンを必ず装着してください(ハウリング防止)。\n\n";
    std::cout << "マイクに向かって歌ってください。Enter キーで終了します。\n\n";

    HarmonyProcessor processor;
    deviceManager.addAudioCallback(&processor);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const size_t rbLat = processor.getLatencySamples();
    std::cout << "Rubber Band latency: " << rbLat << " samples ("
              << std::fixed << std::setprecision(1)
              << 1000.0 * rbLat / device->getCurrentSampleRate() << " ms)\n\n";

    std::atomic<bool> running { true };
    std::thread printer([&]() {
        while (running.load())
        {
            const float hz     = processor.getLatestPitch();
            const float conf   = processor.getLatestConfidence();
            const float target = processor.getLatestTargetHz();
            const float ratio  = processor.getLatestRatio();
            if (hz > 0.0f && conf > 0.5f)
            {
                std::cout << "\rmain: "
                          << std::fixed << std::setw(7) << std::setprecision(2) << hz
                          << "Hz(" << std::setw(4) << std::left << hzToNoteName(hz) << ")"
                          << " → harmony: "
                          << std::right << std::setw(7) << std::setprecision(2) << target
                          << "Hz(" << std::setw(4) << std::left << hzToNoteName(target) << ")"
                          << " x" << std::setprecision(3) << ratio
                          << " conf=" << std::setprecision(2) << conf
                          << "   " << std::flush;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    std::cin.get();
    running.store(false);
    printer.join();

    deviceManager.removeAudioCallback(&processor);
    std::cout << "\n終了しました。\n";
    return 0;
}
