# データ処理フロー詳細仕様書

本ドキュメントでは、CEF (Chromium Embedded Framework) から DeckLink への映像出力に至るまでのデータ処理パイプラインの詳細を記述する。
特に、各ステージの **駆動トリガー**、**動作レート**、および **同期メカニズム** に焦点を当てて解説する。

---

## 1. システムアーキテクチャ概要

本システムは、可変レートで動作するWebレンダリングエンジン (CEF) と、厳密な固定フレームレート (59.94i) を要求する放送用ハードウェア (DeckLink) を接続するためのブリッジアプリケーションである。
**DeckLinkからのハードウェアクロックに位相同期**し、CEFに対して正確なタイミングで描画要求を行う「Pacing」メカニズムを特徴とする。

### データフロー図 (概念)

```mermaid
graph LR
    subgraph "DeckLink Thread (30Hz)"
        DL[DeckLink Callback] -->|Signal| MAIN[Main Thread Loop]
        DL -->|SyncWithGPU| VRAM_IN[Texture Upload]
    end

    subgraph "Main Thread (Pacing Control)"
        MAIN -->|Schedule (T)| CEF_T1[Task 1: Immediate]
        MAIN -->|Schedule (T+0.5)| CEF_T2[Task 2: Delayed 14ms]
    end

    subgraph "CEF UI Thread (60Hz)"
        CEF_T1 -->|Trigger| PAINT1[OnPaint (Frame T)]
        CEF_T2 -->|Trigger| PAINT2[OnPaint (Frame T+1)]
        PAINT1 -->|Write| CPU_BUF[m_cpuBuffers]
        PAINT2 -->|Write| CPU_BUF
    end

    style DL fill:#9cf,stroke:#333
    style CEF_T1 fill:#f9f,stroke:#333
    style VRAM_IN fill:#fc9,stroke:#333
```

---

## 2. バッファリング戦略詳細

### バッファ構成: 6連バッファ (Sextuple Buffering)
全てのステージにおいて `kBufferCount = 6` のリングバッファを使用する。
これにより、**「書き込み」「転送」「読み出し」の操作が完全に独立して並列動作**し、突発的な負荷やジッターに対してより堅牢になる。

---

### ステージ 1: CEF レンダリング (Producer)
- **担当クラス**: `CefManager`, `CefRenderHandlerImpl`
- **駆動トリガー**: **DeckLink Pacing Mechanism**
    - DeckLinkの1回のコールバック (29.97Hz) に対し、`CefManager::ScheduleFrames()` が2回描画要求を発行する。
    1.  **即時要求**: `SendExternalBeginFrame()` + `Invalidate()`
    2.  **遅延要求 (14ms後)**: 次のVSyncスロット用 (高精度タイマー `timeBeginPeriod(1)` 使用)
- **強制再描画 (Forced Redraw)**:
    - 静止画やアニメーションがない場合でも、`Invalidate(PET_VIEW)` を呼び出すことで強制的に `OnPaint` を駆動し、60fpsのストリームを維持する。

### ステージ 2: GPU アップロード (Consumer)
- **最適化されたロック機構**:
    - `OnPaint` (書き込み) と `SyncWithGPU` (読み出し) は、バッファ確保とコミット時のみ短時間のロックを行う。
    - **メモリコピー (memcpy) はロック外で実行** されるため、CEFの描画とDeckLinkのアップロードが並列動作可能。

---

### ステージ 3: シェーダー処理 & パイプライン・リードバック (Consumer 2)
- **担当クラス**: `ShaderManager`
- **実行スレッド**: DeckLink Video Output Thread
- **駆動トリガー**: ステージ2完了後
- **動作レート**: **29.97 fps (Interlaced Frame Generation)**
- **ロジック**:
    1.  **Compute Shader Dispatch**:
        - **入力**: 連続する2枚のプログレッシブフレーム (59.94p)
            - `t0`: Frame T (Top Field用)
            - `t1`: Frame T+0.5 (Bottom Field用)
        - **出力**: 1枚のインターレースフレーム (59.94i)
            - 偶数ラインには `t0` のピクセル、奇数ラインには `t1` のピクセルを採用して合成する。
            - これにより、時間解像度が 59.94Hz となり、滑らかな放送品質のモーションが得られる。
    2.  **VRAM内コピー**:
        - Output (Interlaced) -> Staging
    3.  **遅延リードバック (Pipelined Readback)**:
        - 2フレーム前（出力フレーム換算）のデータを読み出す。

---

## 3. フレームレイテンシとシーケンス

**59.94p -> 59.94i 変換フロー**

| 時間軸 (DL Tick) | ソースフレーム (59.94p) | アクション | 出力 (59.94i) |
| :--- | :--- | :--- | :--- |
| **Tick 0** | Frame 0, 1 生成 | CEF描画 | - |
| **Tick 1** | Frame 2, 3 生成 | **Upload**: Frame 0, 1 -> Texture 0, 1<br>**Shader**: Mix(Tex0, Tex1) -> Out 0 | Out 0 生成中 (GPU) |
| **Tick 2** | Frame 4, 5 生成 | **Upload**: Frame 2, 3 -> Texture 2, 3<br>**Shader**: Mix(Tex2, Tex3) -> Out 1 | Out 1 生成中 |
| **Tick 3** | Frame 6, 7 生成 | **Readback**: Out 0 (Tick 1の結果) | **DeckLink Output**: Out 0 |

**合計レイテンシ**:
インターレース合成のため2枚揃うのを待つ必要があり、さらにGPUパイプラインの遅延が加わるため、入力(T=0)から出力まで **約3 Tick (約100ms)** の遅延となる。しかし、これにより **完全な59.94モーション** が保証される。

---

## 4. 安定化とパフォーマンス最適化 (Stability)

### フリーズ防止・FPS低下対策
1.  **プロセス優先度 (Process Priority)**:
    - `HIGH_PRIORITY_CLASS` に設定し、OSスケジューラによる優先的なCPU割り当てを確保。
2.  **バックグラウンド抑制の無効化**:
    - CEFに対し以下のフラグを適用し、ウィンドウ非表示時でもフルスピードで動作させる。
        - `--disable-renderer-backgrounding`
        - `--disable-background-timer-throttling`
        - `--disable-gpu-vsync` (内部リミッター解除)
3.  **待機ロジックの改善**:
    - `WaitForNextFrame` はメインループをブロッキングせず、適切なタイムアウトまたは非ブロッキングチェックを行い、CEFメッセージループの回転速度を維持する。

## 5. モニタリング
- コンソール出力にて、以下のレートを監視可能:
    - `DeckLink`: ハードウェア割り込みレート (理想値: 29.97 / 30.00 fps)
    - `CEF`: 実際の描画完了レート (理想値: 59.94 / 60.00 fps)
- `CEF` の値が `DeckLink` の **ちょうど2倍** であれば、完全な同期状態 (Locked) である。
