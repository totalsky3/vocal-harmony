# VocalHarmony — リアルタイム版(C++)

ハモリ生成(`main.cpp`)とボイスチェンジャー(`voice_changer.cpp`)の2つの実行ファイルをビルドします。

## 必要なもの

- CMake 3.22 以降
- C++17 対応コンパイラ(Windows: Visual Studio 2022 / macOS: Xcode / Linux: GCC・Clang)
- aubio, Rubber Band Library(JUCE はビルド時に自動取得されます)

### Windows (vcpkg)

1. Visual Studio 2022 を「C++ によるデスクトップ開発」ワークロード付きでインストール
2. [vcpkg](https://github.com/microsoft/vcpkg) をセットアップ
3. 依存ライブラリをインストール:
   ```cmd
   vcpkg install aubio:x64-windows rubberband:x64-windows
   ```

### macOS

```bash
brew install cmake pkg-config aubio rubberband
```

### Ubuntu / Debian

```bash
sudo apt install build-essential cmake pkg-config git libaubio-dev librubberband-dev
# JUCE の Linux 依存
sudo apt install libasound2-dev libjack-jackd2-dev libcurl4-openssl-dev \
    libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev \
    libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev \
    libwebkit2gtk-4.1-dev libglu1-mesa-dev
```

## ビルド

```bash
# Windows (vcpkg のツールチェインを指定)
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg のパス>/scripts/buildsystems/vcpkg.cmake

# macOS / Linux
cmake -B build -S .

# 共通
cmake --build build --config Release
```

> `<vcpkg のパス>` は実際の絶対パスに置き換えてください(例: `C:/dev/vcpkg`)。`<` `>` は付けません。

**初回ビルドは JUCE の取得・ビルドで 5〜10 分かかります。** 2回目以降は数秒です。

ビルド成果物:
- `build/VocalHarmony_artefacts/Release/VocalHarmony`(ハモリ生成)
- `build/VoiceChanger_artefacts/Release/VoiceChanger`(ボイスチェンジャー)

## 使い方

### 共通: 入出力デバイスの設定

各ソース冒頭の `config` 名前空間で、使用するマイク/スピーカーを指定します。起動時に利用可能なデバイス一覧が表示されるので、その名前を正確にコピーして設定してください。

```cpp
const juce::String inputDeviceName = "Microphone Array (AMD Audio Device)";
```

> **ヒント**: 英数字のみのデバイス名を選ぶと文字コードの問題を避けられます。

### ハモリ生成 (VocalHarmony)

`main.cpp` の `harmony` 名前空間で、曲のキー・スケール・ハモリの度数を設定します。

```cpp
const int              keyRootMidi = 57;                 // A3
const std::vector<int> scale       = {0,2,3,5,7,8,10};   // 自然短音階
const int              degreeShift = -2;                 // 下3度
```

**ヘッドホンを装着して**実行してください。スピーカーだと出力をマイクが拾い直してハウリングします。

### ボイスチェンジャー (VoiceChanger)

実行すると数字キー(1〜9)でボイスプリセットを切り替えられます。`voice_changer.cpp` の `kPresets` 配列で各プリセットの `pitch`(声の高さ)と `formant`(体格の印象)を調整できます。

#### Discord 等で使う場合

1. [VB-CABLE](https://vb-audio.com/Cable/) をインストール(管理者権限・要再起動)
2. `config::outputDeviceName` を `"CABLE Input (VB-Audio Virtual Cable)"` に設定
3. Discord の入力デバイスを `CABLE Output (VB-Audio Virtual Cable)` に設定

これで変換後の声が Discord の相手に届きます。

## トラブルシューティング

| 症状 | 対処 |
|------|------|
| `cmake` が見つからない (Windows) | 「Developer Command Prompt for VS 2022」から実行する |
| 日本語コメントで大量の構文エラー (MSVC) | `/utf-8` フラグが効いているか確認(CMakeLists.txt に設定済み) |
| コンソールの日本語が文字化け | 実行ファイルが `SetConsoleOutputCP(CP_UTF8)` を呼ぶので通常は不要だが、手動なら `chcp 65001` |
| ビルドが謎のパスエラーで壊れる | Visual Studio 更新後によくある。`build/` を削除してクリアビルド |
| F0 が異常値/声を拾わない | 入力に「ステレオミキサー」等のループバックが選ばれていないか確認。本物のマイクを明示指定する |
| `aubio.dll が見つからない` (実行時) | vcpkg ツールチェインが自動コピーするはず。configure からやり直す |

## 開発フェーズ

このリアルタイム版は Phase 1(PC 上プロトタイプ)です。

- [x] 第一段階: マイク → F0 検出
- [x] 第二段階: Rubber Band によるピッチシフト + ミックス
- [x] 第三段階: diatonic スナップ + 音域調整
- [x] 応用: ボイスチェンジャー(ピッチ/フォルマント独立操作)
- [ ] Phase 2: 組み込みデバイス(Daisy Seed 等)への移植
