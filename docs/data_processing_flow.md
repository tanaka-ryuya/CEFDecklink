# データ処理フロー詳細仕様書

本ドキュメントでは、CEF (Chromium Embedded Framework) から DeckLink への映像出力に至るまでのデータ処理パイプラインの詳細を記述する。
現在の実装（CEFフリーラン＋スタッター防止同期機構）に基づき、各ステージの **駆動トリガー**、**動作レート**、および **同期メカニズム** に焦点を当てて解説する。

---

## 1. システムアーキテクチャ概要

本システムは、オフスクリーンで動作するWebレンダリングエンジン (CEF) の出力を、厳密な固定フレームレート (59.94i または 50i) を要求する放送用ハードウェア (DeckLink) へ送り届けるブリッジアプリケーションである。
**CEFスレッド (プロデューサー)** と **DeckLinkスレッド (コンシューマー)** が独立して動作し、タイムスタンプ付きのフレームキューを用いて効率的かつ滑らかに同期するメカニズムを特徴とする。

### データフロー図 (概念)

```mermaid
graph TD
    subgraph "1. Producer: CEF Thread (Free-Run)"
        TIMER["Internal Timer 60Hz/50Hz"] --> CEF["CEF WebCore"]
        CEF -->|OnPaint & Timestamp| CPU_Q["CPU Frame Queue (max 32)"]
    end

    subgraph "2. Consumer: DeckLink Thread 29.97Hz/25Hz"
        CPU_Q -->|SyncWithGPU| GPU_Q["GPU Texture Queue"]
        GPU_Q -->|GetSynchronizedTextures| PAIR["Frame Pair [Top, Bottom]"]
        
        PAIR -->|Top Field| SHADER["Compute Shader"]
        PAIR -->|Bottom Field| SHADER
        
        SHADER -->|Interlace and YUV| STAGING["Staging Buffer"]
        STAGING -->|Readback Delayed| DL_BUF["DeckLink Frame Buffer"]
        
        DL_BUF --> HW["DeckLink Hardware"]
    end

    style CEF fill:#f9f,stroke:#333
    style GPU_Q fill:#cfc,stroke:#333
    style STAGING fill:#cfc,stroke:#333
    style DL_BUF fill:#9cf,stroke:#333
```

---

## 2. バッファリングとスレッド間同期戦略

本アプリケーションは、処理落ち（ドロップフレーム）や遅延を防ぎつつ、最も滑らかなモーションを出力するための同期ロジック（CasparCGアーキテクチャ踏襲）を実装している。

### ステージ 1: CEF レンダリング (Producer)
- **担当クラス**: `CefManager`, `CefRenderHandlerImpl`
- **駆動トリガー**: CEF内部タイマー (フリーラン)
- **動作レート**: **60 fps または 50 fps** (`windowless_frame_rate = 60` or `50`)
- **ロジック**:
    - 以前の手動駆動 (`DriveExternalBeginFrame`) を廃止し、CEF自身の自律的な高精度タイマーでレンダリングを行う（フリーラン方式）。
    - これによりWindowsの15.6msタイマー分解能によるジッター（遅延）を回避し、安定した60fps/50fpsのフレーム供給を実現。
    - `OnPaint` コールバックでピクセルデータをコピーする際、その瞬間の `std::chrono::steady_clock` をタイムスタンプとして記録する。

### ステージ 2: GPU アップロードと同期キュー (Consumer 1)
- **担当関数**: `CefRenderHandlerImpl::SyncWithGPU()`, `GetSynchronizedTextures()`
- **駆動トリガー**: DeckLinkのコールバック (`ScheduledFrameCompleted`) 内から毎フレーム間隔（59.94i時は約33.36ms、50i時は40.0ms）でポーリング実行。
- **ロジック**:
    - **アップロード**: CEF側で溜まっている未アップロードのフレームをD3D11テクスチャへアップロードし、タイムスタンプと共にキュー (`m_readyTextures`) に追加する。
    - **ペアの取り出し (時間ベース・プレロール機構)**:
        - 新たなフレーム供給（キュー0枚から1枚以上の増加）を検知すると、直ちに消費を開始するのではなく **一定の意図的な遅延（プレロール）** を設ける。
        - **通常アニメーション時**: このプレロール中にCEFから数枚のフレームがキューに貯金される。消費開始後は、OSタイマーのジッターによってCEFの生成が一瞬遅れた際も、この貯金から安定してフレームを引き出せるため、不規則なカクつき（微小ジッター）が完全に吸収される。
        - **静止画（カット出し）時**: CEFから1枚のフレームのみが供給され後続が来ない場合も、プレロール遅延後にその1枚が取り出され、TopとBottomの両フィールドに複製されて静止画として正しく出力される。
        - **アニメーション停止時 (キュー空渇)**: CEFからの供給が止まりキューが空（0枚）になると、消費フェーズを終了し、再びプレロール待機タイマーをリセットする。待機中およびプレロール進行中は「前回最後に表示したフレーム」を両フィールドに複製出力し、画面のブレ（時間逆行）を防ぎ完全静止（Freeze）を維持する。

---

## 3. ビューモード (g_viewMode) と TUI コントロール

本システムは、コンソール上の TUI (Text User Interface) にて修飾キー（`Ctrl`）を用いたホットキー操作で、レンダリングやシェーダーの動作モード、フィルタ等のパラメータをリアルタイムに切り替える機能を持つ。パラメータ変更時は `config.json` に自動保存され、次回起動時に復元される。

| モード値 | 出力モード名称 | 動作仕様 |
| :---: | :--- | :--- |
| **0** | **Interlace (標準)** | CEFは60fps(または50fps)フリーラン。シェーダーで前後フレームをWeave合成し、1080i映像として出力する。 |
| **1** | **Diff (差分)** | CEFは60fps(または50fps)フリーラン。前後フレームの差分(`abs(p1 - p2)`)を出力し、動いているピクセルのみを可視化する。静止時は真っ黒になる。 |
| **2** | **Progressive** | CEFは60fps(または50fps)フリーラン。実機ではFrame1のみを使った29.97p/25p出力。ウィンドウプレビュー表示時はダブルパンプによる60p/50pプレビューとなる。 |
| **3** | **30p Blend** | CEF描画の消費を半分に間引き、シェーダー側で前後のコマを50%ずつブレンドして滑らかなモーションブラーを生成する。 |

**TUI ホットキー一覧:**
- **`Ctrl + O`** : Output Mode (上記0〜3) を順番に切り替え。
- **`Ctrl + F`** : Internal Filter Mode (None -> 3tap -> 5tap) を順番に切り替え。
- **`Ctrl + K`** : Keyer Mode (Internal / External) の切り替え。
- **`Ctrl + A` / `Ctrl + Z`** : Unmultアルファ閾値 (`g_alphaThreshold`) の微調整 (+0.001 / -0.001)。
- **`Ctrl + Up` / `Ctrl + Down`** : Unmultアルファ閾値の粗調整 (+0.1 / -0.1)。
- **`Ctrl + R`** : 現在のURLをリロード。
- **`Ctrl + C`** : アプリケーションを安全に終了。

※ダッシュボードの表示は、現在のフォーマット(59.94i / 50i)やFPS、キューの枯渇状況（skips）に応じて緑・黄・赤とリアルタイムにカラーリングされる。

---

## 4. シェーダー処理とインターレース合成 (Consumer 2)

- **担当クラス**: `ShaderManager`
- **実行スレッド**: DeckLink Video Output Thread
- **動作レート**: **29.97 fps または 25 fps (Interlaced Frame Generation)**
- **ロジック**:
    1.  **Compute Shader Dispatch (`YUVConvert.hlsl`)**:
        - **入力**: `GetSynchronizedTextures()` から取得した連続する2枚のプログレッシブフレーム。
            - `t0`: 少し古いフレーム (Top Field に適用)
            - `t1`: 最新のフレーム (Bottom Field に適用)
        - **処理内容**: 
            - 偶数ラインは `t0` のピクセル、奇数ラインは `t1` のピクセルからサンプリングを行う（Weave合成）。
            - 指定されたフィルタモード(3tap, 5tap)に応じて、フリッカー低減のための垂直ブラーを適用可能。
            - ARGB (RGB + アルファ) から UYVY または v210 等のYUV形式へ色空間変換を行う。
            - 引数 `--alpha` (閾値) に基づいて、Unmultiplied アルファ（FillとKeyの分離）処理の調整を行う。
        - **出力**: 1枚のインターレースフレーム (59.94i または 50i)用テクスチャ。
    
    2.  **遅延リードバック (Pipelined Readback)**:
        - GPUからCPUメモリ（DeckLinkバッファ）への読み出し（Readback）は極めて重い処理である。
        - パフォーマンス低下を防ぐため、**2フレーム前（出力フレーム換算）** に処理が完了したステージング・バッファを `Map` して読み出す。
        - 読み出したデータを DeckLink の出力バッファ (`pBuffer`) へ `memcpy` する。

---

## 5. フレームレイテンシとシーケンス

**60p/50p -> 59.94i/50i 変換および出力フロー**

全体として、CEFで描画されたフレームが実際にDeckLinkハードウェアから出力されるまでには、**意図的なパイプライン遅延（約2 Tick + CEFバッファ同期プレロール遅延）** が存在する。

| 時間軸 (DL Tick) | アクション (DLスレッド内) | 状態 |
| :--- | :--- | :--- |
| **Tick N** | CEFキューからペア(A, B)を取得し VRAM へアップロード<br>`ShaderManager` が合成して `Staging[0]` へ出力 | `Staging[0]` 生成開始 (GPU上) |
| **Tick N+1** | 次のペア(C, D)を取得し VRAM へアップロード<br>`ShaderManager` が合成して `Staging[1]` へ出力 | `Staging[1]` 生成開始 |
| **Tick N+2** | 次のペア(E, F)を取得し VRAM へアップロード<br>**`Staging[0]` からCPUへ非同期 Readback**<br>DeckLinkへスケジュール | **`Staging[0]` (Tick N生成分) がハードウェアへ送出される** |

これにより、時間解像度が 60Hz/50Hz の滑らかな放送品質モーションが保証されたまま、スレッド群がブロックされることなくスループット 29.97fps/25fps (Interlaced) を達成する。

---

## 6. 安定化とパフォーマンス最適化

1.  **プロセス優先度とスレッド制御**:
    - `HIGH_PRIORITY_CLASS` に設定し、OSスケジューラによる優先的なCPU割り当てを確保。
    - CEFレンダリングスレッド（フリーラン）と DeckLink 出力コールバックスレッド（29.97Hz/25Hzタイマー駆動）は完全に独立して走り、タイムスタンプ付きキューにより非同期にデータを授受する。
2.  **バックグラウンド抑制の無効化**:
    - ウィンドウ非表示時でもCEFが動作するよう、起動オプションに `--disable-renderer-backgrounding`, `--disable-background-timer-throttling` 等を適用。
    - **注意**: CEFの内部タイマー（60fps/50fps）を正確に維持するため、`--disable-frame-rate-limit` や `--disable-gpu-vsync` は使用しない。
3.  **ロード完了後のレンダリング維持（Keep-Alive）**:
    - CEFは画面更新がないとレンダリングを停止する性質がある。確実なフレーム供給を保証するため、ページのロード完了時 (`OnLoadEnd`) に微小な要素をアニメーションさせるダミーのスクリプトを注入し、強制的かつ継続的に再描画イベント（OnPaint）を発生させている。
4.  **UIモニタリングとログ**:
    - 毎秒のシステム状態（FPS, Queueサイズ, skips回数など）や現在のURL設定は `main.cpp` のメインループ (`RenderFrame`) で集計され、コンソールにリアルタイム表示される。
    - フリーラン時は、CEF側でアニメーションが行われている間はCEF FPSが指定レート(60または50)をキープし、キューサイズが安定して消費される状態が理想的である。
