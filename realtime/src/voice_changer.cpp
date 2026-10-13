// =============================================================================
// VocalHarmony / VoiceChanger
// Copyright (C) 2026 totalsky3
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See <https://www.gnu.org/licenses/>.
// =============================================================================
//
// VoiceChanger - リアルタイム・ボイスチェンジャー(TRPG/Discord用途)
//
// VocalHarmony のハモリ生成基盤を転用し、ピッチとフォルマントを独立に
// 操作してキャラクターボイスを作る。数字キー(1〜9)でプリセットを即時に
// 切り替えられるので、TRPGのKPが複数NPCを演じ分けるのに使える。
//
// 設計の要点:
//   - ハモリ版と同じ「マイク→ピッチシフト→出力」経路を使う
//   - ただしドライ(地声)は混ぜず、変換後の声だけを出力する
//   - OptionFormantPreserved は使わず、setFormantScale で声道長を
//     独立に操作する(= 体格の印象を変える)
//
// 出力先を VB-CABLE にすると、Discordの入力として使える。
//   config::outputDeviceName = "CABLE Input (VB-Audio Virtual Cable)"
// =============================================================================

#include <juce_audio_utils/juce_audio_utils.h>
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
// Discordに流すなら outputDeviceName を CABLE Input にする。
// 自分の耳でモニターしたいだけなら、ヘッドホンのデバイス名にする。
// =============================================================================
namespace config
{
    const juce::String deviceType       = "";
    const juce::String inputDeviceName  = "Microphone Array (AMD Audio Device)";
    const juce::String outputDeviceName = "CABLE Input (VB-Audio Virtual Cable)";
    const double       sampleRate       = 44100.0;
    const int          bufferSize       = 256;
}


// =============================================================================
// ★ ボイスプリセット ★
// pitch:   声の高さ。>1で高く、<1で低く(1.0=変化なし)
// formant: 体格の印象。>1で小柄/子供っぽく、<1で大柄/重厚に(1.0=変化なし)
// 実用範囲は pitch/formant とも 0.5〜1.6 程度。外れると金属的に歪む。
// =============================================================================
struct VoicePreset
{
    const char* name;
    double      pitch;
    double      formant;
};

// 数字キー1〜9に対応(配列インデックス0〜8)
static const VoicePreset kPresets[] = {
    // name                     pitch  formant
    { "[1] 素通し(原音)",         1.00,  1.00 },
    { "[2] 巨漢",                 0.78,  0.85 },
    { "[3] 怪物",                 0.65,  0.78 },
    { "[4] 邪神・人外",           0.52,  0.70 },
    { "[5] 老人",                 0.90,  1.00 },
    { "[6] 少年",                 1.18,  1.05 },
    { "[7] 少女",                 1.30,  1.15 },
    { "[8] 小妖精",               1.45,  1.28 },
    { "[9] 威圧(低く大きく)",     0.70,  0.80 },
};
static const int kNumPresets = sizeof(kPresets) / sizeof(kPresets[0]);


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
// VoiceChangeProcessor
// =============================================================================
class VoiceChangeProcessor : public juce::AudioIODeviceCallback
{
public:
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        sampleRate_ = static_cast<int>(device->getCurrentSampleRate());
        bufferSize_ = device->getCurrentBufferSizeSamples();

        using RB = RubberBand::RubberBandStretcher;
        // ハモリ版と違い OptionFormantPreserved は付けない。
        // フォルマントは setFormantScale で能動的に動かすため。
        // 会話用途はタイミング制約が緩いので、WindowShortを外して
        // 音質寄り(Standard)にしてある。
        int options =
            RB::OptionProcessRealTime
            | RB::OptionEngineFaster
            | RB::OptionPitchHighConsistency
            | RB::OptionThreadingNever;

        stretcher_ = std::make_unique<RB>(
            static_cast<size_t>(sampleRate_), 1, options);

        applyPreset(currentPreset_.load());

        wetBuffer_.assign(bufferSize_, 0.0f);
    }

    void audioDeviceStopped() override
    {
        stretcher_.reset();
    }

    // プリセットのpitch/formantをRubber Bandに反映する。
    // コールバック外(キー入力スレッド)からも呼ばれるので、
    // pendingPreset_経由で次回コールバックに伝える方式にしている。
    void applyPreset(int index)
    {
        if (index < 0 || index >= kNumPresets || !stretcher_) return;
        stretcher_->setPitchScale(kPresets[index].pitch);
        stretcher_->setFormantScale(kPresets[index].formant);
    }

    void requestPreset(int index)
    {
        if (index >= 0 && index < kNumPresets)
            pendingPreset_.store(index);
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

        if (numInputChannels == 0 || !stretcher_)
            return;

        // キー入力で要求されたプリセットがあれば、ここで反映する。
        // (Rubber Bandの設定変更はオーディオスレッドで行うのが安全)
        const int pending = pendingPreset_.load();
        if (pending != currentPreset_.load())
        {
            applyPreset(pending);
            currentPreset_.store(pending);
        }

        const float* dry = inputChannelData[0];

        // ===== ピッチ+フォルマントシフト =====
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

        // ===== 出力(変換後の声のみ。地声=dryは混ぜない)=====
        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            float* out = outputChannelData[ch];
            for (int i = 0; i < numSamples; ++i)
                out[i] = wetPtr[i];
        }
    }

    int getCurrentPreset() const { return currentPreset_.load(); }

private:
    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher_;
    std::vector<float> wetBuffer_;

    int sampleRate_ = 0;
    int bufferSize_ = 0;

    std::atomic<int> currentPreset_ { 0 };
    std::atomic<int> pendingPreset_ { 0 };
};


// =============================================================================
// プリセット一覧を表示
// =============================================================================
static void printPresetMenu(int current)
{
    std::cout << "\n--- ボイスプリセット(数字キーで切替)---\n";
    for (int i = 0; i < kNumPresets; ++i)
    {
        std::cout << "  " << (i == current ? "→ " : "  ")
                  << kPresets[i].name
                  << "  (pitch=" << std::fixed << std::setprecision(2) << kPresets[i].pitch
                  << ", formant=" << kPresets[i].formant << ")\n";
    }
    std::cout << "  [0] 終了\n";
    std::cout << "--------------------------------------\n";
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
        std::cerr << "(出力をCABLE Inputにしている場合、VB-CABLEが入っているか確認)\n";
        return 1;
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        std::cerr << "利用可能なオーディオデバイスがありません" << std::endl;
        return 1;
    }

    std::cout << "===== VoiceChanger (TRPG / Discord) =====\n";
    std::cout << "Input  : " << config::inputDeviceName.toStdString() << "\n";
    std::cout << "Output : " << config::outputDeviceName.toStdString() << "\n";
    std::cout << "Sample : " << device->getCurrentSampleRate() << " Hz / "
              << device->getCurrentBufferSizeSamples() << " samples\n";
    std::cout << "=========================================\n";

    VoiceChangeProcessor processor;
    deviceManager.addAudioCallback(&processor);

    printPresetMenu(processor.getCurrentPreset());
    std::cout << "\nキーを押して Enter で切替(例: 3 → Enter)。0 → Enter で終了。\n";

    // キー入力ループ(メインスレッド)。
    // コンソールからの入力なので「数字 + Enter」で受け付ける。
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line.empty()) continue;
        const char c = line[0];

        if (c == '0')
            break;

        if (c >= '1' && c <= '9')
        {
            const int index = c - '1';   // '1'→0, '2'→1, ...
            if (index < kNumPresets)
            {
                processor.requestPreset(index);
                // 反映を少し待ってから表示を更新
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                printPresetMenu(index);
                std::cout << "→ 『" << kPresets[index].name << "』に切り替えました。\n";
            }
        }
    }

    deviceManager.removeAudioCallback(&processor);
    std::cout << "終了しました。\n";
    return 0;
}
