"""
Diatonic Harmony Generator
==========================

入力された歌声に対して、楽曲のキーに合った「スケール準拠ハモリ」を
自動生成し、メインボーカルと重ねて出力するプログラム。

WORLDボコーダーで音声を「ソース(F0=声帯振動由来)」と「フィルター
(スペクトル包絡=声道の共鳴特性)」に分解し、それぞれを音楽理論に
基づいて加工してから再合成する。固定の半音差で機械的にピッチを
ずらす素朴な手法と異なり、各音をキーのスケール上に「スナップ」
させることで、コード進行と協和するハモリを生成できる。

処理パイプライン:
    1. 音声の読み込みと前処理(モノ化・float64化・無音パディング)
    2. tanhソフトクリッピングで声に倍音と存在感を付加
    3. WORLDで F0・sp(スペクトル包絡)・ap(非周期成分)に分解
    4. F0系列を中央値フィルタで平滑化(誤検出の除去)
    5. F0をdiatonicスケールにスナップさせてハモリ用F0を生成
    6. スペクトル包絡を周波数方向に伸縮しハモリの声色を変える
    7. WORLDで再合成し、メインボーカルとミックス

依存:
    pip install sounddevice soundfile pyworld numpy scipy
"""

import sounddevice as sd
import soundfile as sf
import pyworld as pw
import numpy as np
from scipy.interpolate import interp1d
from scipy.signal import medfilt


# =============================================================================
# 楽曲別プリセット
# =============================================================================
# 各曲のキー(調性)・スケール・ハモリの度数を設定。新しい曲を追加する場合は、
# その曲のキーを楽譜やコード譜で確認し、主旋律に対してどの方向(上/下)に
# 何度のハモリを付けたいかを決める。
#
# scale はメジャー/マイナーで以下を使い分ける:
#     メジャースケール  : [0, 2, 4, 5, 7, 9, 11]
#     自然短音階        : [0, 2, 3, 5, 7, 8, 10]
#     和声短音階        : [0, 2, 3, 5, 7, 8, 11]  (導音=半音上げの7度)
#
# degree_shift はスケール上の度数差:
#     +2 = 3度上ハモリ   -2 = 3度下ハモリ
#     +4 = 5度上ハモリ   -4 = 5度下ハモリ
#
# 注: デュエット曲(明星ギャラクティカ、ray等)は本アルゴリズムでは扱えない。
# 入力音声に複数旋律が同時に存在すると、harvest のF0検出が一意に定まらない
# ため。対応するには事前に音源分離(Demucs等)で片パートだけを抽出する必要
# がある。
PRESETS = {
    "soratobe": {
        "name": "空も飛べるはず / スピッツ",
        "key_root_midi": 58,                    # B♭3
        "scale": [0, 2, 4, 5, 7, 9, 11],        # メジャー
        "degree_shift": 2,                      # 3度上
    },
    "ageha": {
        "name": "アゲハ蝶 / ポルノグラフィティ",
        "key_root_midi": 57,                    # A3
        "scale": [0, 2, 3, 5, 7, 8, 10],        # 自然短音階
        "degree_shift": -2,                     # 3度下(原曲再現)
    },
    "elegy": {
        "name": "サヨナラエレジー / 菅田将暉",
        "key_root_midi": 57,                    # A3
        "scale": [0, 2, 3, 5, 7, 8, 10],        # 自然短音階
        "degree_shift": 2,                      # 3度上
    },
    "lalala": {
        "name": "LA・LA・LA LOVE SONG / 久保田利伸",
        "key_root_midi": 56,                    # G#3
        "scale": [0, 2, 3, 5, 7, 8, 10],        # 自然短音階
        "degree_shift": 2,                      # 3度上
    },
    "ray": {
        "name": "ray / BUMP OF CHICKEN",
        "key_root_midi": 51,                    # E♭3
        "scale": [0, 2, 4, 5, 7, 9, 11],        # メジャー
        "degree_shift": 2,                      # 3度上
    },
}


# =============================================================================
# 1. サチュレーション(声に倍音と太さを付加)
# =============================================================================
def add_saturation(data: np.ndarray, drive: float = 3.0,
                   output_level: float = 0.8) -> np.ndarray:
    """
    tanh によるソフトクリッピングで声に倍音を加える。

    数式:  y = tanh(x * drive)

    x が小さい範囲では y ≈ x(線形)、x が大きくなると y は ±1 に
    漸近する(歪み)。この非線形変換が奇数次倍音を発生させ、真空管
    アンプのような温かみと存在感を声に加える。

    Args:
        data:         入力音声(1次元 numpy array)
        drive:        歪みの量。1.0=控えめ、3.0=標準、5.0+=激しめ
        output_level: 正規化後の最大振幅(0〜1)

    Returns:
        サチュレーション後の音声
    """
    processed = np.tanh(data * drive)

    # 振幅を安全レベルに揃える(クリッピング防止)
    max_amp = np.max(np.abs(processed))
    if max_amp > 0:
        processed = (processed / max_amp) * output_level
    return processed


# =============================================================================
# 2. フォルマントシフト(声道の長さを擬似的に変える)
# =============================================================================
def shift_formant(sp: np.ndarray, fs: int, shift_rate: float) -> np.ndarray:
    """
    スペクトル包絡を周波数方向に伸縮し、フォルマントをシフトする。

    フォルマント(母音や声色を決める共鳴ピーク)は声道の長さに依存する。
    包絡を周波数方向に圧縮 = 声道が短くなったように聞こえる(明るい声)。
    伸張 = 声道が長くなったように聞こえる(深い声)。

    実装: 各フレームについて、元の周波数軸上で線形補間を行い、
    shift_rate で割った位置の値を取り出す。これは「もとの fs/2 の
    範囲のスペクトルを、shift_rate 倍に伸縮して読み直す」操作に相当
    する。

    Args:
        sp:         WORLDのスペクトル包絡(2次元: フレーム × 周波数ビン)
        fs:         サンプリング周波数(Hz)
        shift_rate: 1.0で変化なし、>1.0でフォルマント上昇(明るく)、
                    <1.0でフォルマント下降(深く)

    Returns:
        伸縮後のスペクトル包絡
    """
    modified_sp = np.zeros_like(sp)
    freq_bins = sp.shape[1]

    # 元の周波数軸(0 ~ fs/2 を freq_bins 等分)
    original_freqs = np.linspace(0, fs / 2, freq_bins)
    # 補間で参照する位置(shift_rate>1 なら元より低い周波数を参照)
    shifted_freqs = original_freqs / shift_rate

    for i in range(sp.shape[0]):
        interpolator = interp1d(
            original_freqs, sp[i],
            kind='linear',
            fill_value='extrapolate',
            bounds_error=False,
        )
        modified_sp[i] = interpolator(shifted_freqs)
        # 0以下の値はWORLDの再合成でエラーになるため極小値でクリップ
        modified_sp[i] = np.maximum(modified_sp[i], 1e-10)

    return modified_sp


# =============================================================================
# 3. F0(ピッチ)の平滑化
# =============================================================================
def clean_f0(f0: np.ndarray, kernel_size: int = 5) -> np.ndarray:
    """
    F0系列の単発的な誤検出を中央値フィルタで除去する。

    pyworldのharvestは通常は安定しているが、息継ぎ、子音、バック
    コーラスの混入などで瞬間的に「オクターブ上に飛ぶ」「別の旋律に
    ジャンプする」などの誤検出を起こすことがある。これらをそのまま
    使うとハモリがプチッと外れる原因になる。

    medfilt は周辺サンプルの中央値で置き換えるので、外れ値だけを
    取り除きつつ、本来のピッチカーブ(連続的な変化)はほぼ保たれる。
    平均値フィルタと違ってジャンプに引きずられないのがポイント。

    Args:
        f0:          F0配列(0は無声フレーム)
        kernel_size: フィルタ窓のサイズ(奇数)

    Returns:
        平滑化されたF0配列
    """
    voiced_mask = f0 > 0
    cleaned = f0.copy()
    if voiced_mask.sum() > kernel_size:
        cleaned[voiced_mask] = medfilt(f0[voiced_mask], kernel_size=kernel_size)
    return cleaned


# =============================================================================
# 4. Diatonic ハモリ F0 生成(本プログラムの核)
# =============================================================================
def diatonic_harmony_f0(f0: np.ndarray, key_root_midi: int,
                        scale: list, degree_shift: int) -> np.ndarray:
    """
    スケール上の音にスナップさせながらハモリのF0を生成する。

    アルゴリズム:
      1. 各フレームのF0(Hz)をMIDIノート番号に変換
      2. キーの主音からの半音差を「オクターブ + ピッチクラス(0〜11)」に分解
      3. スケール内で最も近い音にスナップ
      4. degree_shift だけスケール度数を動かす(オクターブまたぎも処理)
      5. 結果のMIDIノートをHzに戻す

    重要なポイント:
      - 半音単位ではなくスケール度数単位でシフトするので、たとえば
        「3度上」と言ったときに自動的に「短3度と長3度の混在」になる
        (これが音楽的に正しい)
      - 無声フレーム(f0=0)はハモリも0のままにする
      - 例: Cメジャー(主音=60)で主旋律がC→D→E→Fと動くと、3度上
        ハモリは E→F→G→A となる(C-E は長3度、D-F は短3度、E-G は
        短3度、F-A は長3度)

    Args:
        f0:            主旋律のF0配列
        key_root_midi: キーの主音のMIDIノート番号(60=C4, 57=A3, など)
        scale:         スケールの度数(半音単位、長さ7)
        degree_shift:  シフトするスケール度数(+2=3度上、-2=3度下、+4=5度上)

    Returns:
        ハモリ用F0配列
    """
    harmony = np.zeros_like(f0)
    n_scale = len(scale)

    for i, freq in enumerate(f0):
        if freq <= 0:
            continue  # 無声フレーム

        # Hz → MIDIノート番号(A4=69, 440Hz基準)
        midi = 69 + 12 * np.log2(freq / 440.0)

        # キー主音からの半音差を「オクターブ + ピッチクラス」に分解
        semi_from_root = midi - key_root_midi
        octave, pc = divmod(round(semi_from_root), 12)

        # スケール内で最も近い音を探す(自然短音階で半音差がGの場合は
        # G(scale[5]=8)とA(scale[6]=10、または1オクターブ上の0)の
        # どちらが近いかを判定する)
        nearest_idx = min(range(n_scale), key=lambda k: abs(scale[k] - pc))

        # スケール度数をdegree_shiftだけシフト(オクターブ繰り上げあり)
        new_idx = nearest_idx + degree_shift
        new_octave = octave + new_idx // n_scale
        new_pc = scale[new_idx % n_scale]

        # MIDIノート → Hz
        new_midi = key_root_midi + new_octave * 12 + new_pc
        harmony[i] = 440.0 * (2 ** ((new_midi - 69) / 12))

    return harmony


# =============================================================================
# 5. メイン処理(全工程の統合)
# =============================================================================
def generate_harmonized_audio(
    input_path: str,
    preset_key: str,
    saturation_drive: float = 3.0,
    harmony_formant_rate: float = 1.15,
    main_volume: float = 0.8,
    harmony_volume: float = 0.25,
    f0_floor: float = 80.0,
    f0_ceil: float = 600.0,
) -> tuple:
    """
    入力音声を読み込んでハモリを生成し、メインボーカルとミックスして返す。

    Args:
        input_path:           入力音声ファイルのパス(.wav推奨)
        preset_key:           PRESETSのキー(例: 'ageha', 'soratobe')
        saturation_drive:     サチュレーション量(1.0〜5.0推奨)
        harmony_formant_rate: ハモリのフォルマントシフト率
                              1.0  = 主旋律と同じ声色(同一人物が重なる感)
                              1.15 = 別人が歌っているような分離感
                              1.3+ = チップマンク的で不自然
        main_volume:          メインボーカルのミックス音量(0〜1)
        harmony_volume:       ハモリのミックス音量(0〜1)
        f0_floor/f0_ceil:     F0探索範囲(Hz)。男性ボーカルなら80〜400、
                              女性ボーカルなら130〜600が目安

    Returns:
        (mixed_audio, fs): ミックス済み音声とサンプリング周波数のタプル
    """
    preset = PRESETS[preset_key]
    print(f"処理対象: {preset['name']}")

    # --- 音声の読み込み ---
    data, fs = sf.read(input_path)
    if data.ndim > 1:
        data = data[:, 0]   # ステレオならLチャンネルだけ使う
    data = np.ascontiguousarray(data, dtype=np.float64)

    # 境界効果を抑えるため前後に0.5秒の無音を付加
    # (短いサビ素材ではharvestの先頭末尾でF0推定が不安定になりやすい)
    pad_samples = int(fs * 0.5)
    padded = np.pad(data, pad_samples, mode='constant')

    # --- 1. サチュレーション ---
    print("[1/5] サチュレーションを適用...")
    main_data = add_saturation(padded, drive=saturation_drive)

    # --- 2. WORLD分析(ソース・フィルター分離) ---
    # f0: 基本周波数(1次元、フレーム数分)
    # sp: スペクトル包絡(2次元、フレーム × 周波数ビン)
    # ap: 非周期成分(2次元、息や子音などのノイズ成分)
    print("[2/5] WORLDで分析中...")
    f0, t = pw.harvest(main_data, fs, f0_floor=f0_floor, f0_ceil=f0_ceil)
    f0 = clean_f0(f0, kernel_size=5)              # F0平滑化
    sp = pw.cheaptrick(main_data, f0, t, fs)      # スペクトル包絡推定
    ap = pw.d4c(main_data, f0, t, fs)             # 非周期成分推定

    # --- 3. ハモリF0を生成 ---
    print("[3/5] ハモリF0を計算中...")
    harmony_f0 = diatonic_harmony_f0(
        f0,
        key_root_midi=preset['key_root_midi'],
        scale=preset['scale'],
        degree_shift=preset['degree_shift'],
    )

    # --- 4. ハモリ用にスペクトル包絡をシフト ---
    # 主旋律と同じ包絡をそのまま使うと「自分自身が高い音で歌っている」
    # 感じになり、ハモリと主旋律が分離しない。フォルマントを少し上げる
    # ことで、別人の声が重なっているような厚みが出る。
    print("[4/5] ハモリの声色を調整中...")
    harmony_sp = shift_formant(sp, fs, harmony_formant_rate)

    # --- 5. WORLDで再合成 ---
    # ap(非周期成分)はメインのものをそのまま流用する。本人の息遣いや
    # 子音特性を保つことで自然な仕上がりになる。
    print("[5/5] ハモリを合成中...")
    harmony_data = pw.synthesize(harmony_f0, harmony_sp, ap, fs)

    # パディング部分を削除して元の長さに戻す
    main_data = main_data[pad_samples:-pad_samples]
    harmony_data = harmony_data[pad_samples:pad_samples + len(main_data)]

    # --- ミックス ---
    min_len = min(len(main_data), len(harmony_data))
    mixed = (main_data[:min_len] * main_volume
             + harmony_data[:min_len] * harmony_volume)

    # 最終正規化(クリッピング防止)
    peak = np.max(np.abs(mixed))
    if peak > 0:
        mixed = mixed / peak * 0.9

    print("完了。")
    return mixed, fs


# =============================================================================
# エントリーポイント
# =============================================================================
if __name__ == "__main__":
    # ----- 設定 -----
    INPUT_FILE = "ageha.wav"      # 入力ファイル(自分のアカペラ録音)
    PRESET = "ageha"              # PRESETSのキーから選択
    OUTPUT_FILE = "output.wav"    # 保存先
    PLAY_AFTER = True             # 処理後に再生する
    SAVE_FILE = True              # ファイルにも保存する

    # ----- 実行 -----
    audio, fs = generate_harmonized_audio(INPUT_FILE, PRESET)

    if SAVE_FILE:
        sf.write(OUTPUT_FILE, audio, fs)
        print(f"保存しました: {OUTPUT_FILE}")

    if PLAY_AFTER:
        print("再生中...")
        sd.play(audio, fs)
        sd.wait()
        print("再生完了。")
