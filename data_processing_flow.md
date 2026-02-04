# データ処理フロー詳細仕様書

本ドキュメントでは、CEF (Chromium Embedded Framework) から DeckLink への映像出力に至るまでのデータ処理パイプラインの詳細を記述する。
特に、各ステージの **駆動トリガー**、**動作レート**、および **同期メカニズム** に焦点を当てて解説する。

---

## 1. システムアーキテクチャ概要

本システムは、可変レートで動作するWebレンダリングエンジン (CEF) と、厳密な固定フレームレート (59.94i) を要求する放送用ハードウェア (DeckLink) を接続するためのブリッジアプリケーションである。

この2つの非同期なドメインを調停するため、以下の3段階のリングバッファ構造を採用している。

1.  **CPU Ring Buffer**: CEFの描画データを一時保持 (System Memory)
2.  **Input Texture Ring**: GPUへのアップロード先 (VRAM - Dynamic Texture)
3.  **Output Texture Ring**: シェーダー処理後の出力結果 (VRAM - Staging Texture)

### データフロー図 (概念)

```mermaid
graph LR
    subgraph "CEF Thread (Producer)"
        CEF[CEF Rendering] -->|Write| CPU_BUF[m_cpuBuffers (System Mem)]
    end

    subgraph "DeckLink Thread (Consumer / GPU Dispatch)"
        CPU_BUF -->|Upload (Map/Unmap)| VRAM_IN[m_textures (Next Frame)]
        VRAM_IN -->|Dispatch Compute Shader| VRAM_OUT[m_outputTextures (Current Frame)]
        VRAM_OUT -->|CopyResource| STAGING[m_stagingOutputs (Current Frame)]
        STAGING -->|Readback (Map - 2 Frames Old)| DL_BUF[DeckLink Buffer]
    end

    style CEF fill:#f9f,stroke:#333,stroke-width:2px
    style DL_BUF fill:#9cf,stroke:#333,stroke-width:2px
```

---

## 2. バッファリング戦略詳細

### バッファ構成: 6連バッファ (Sextuple Buffering)
全てのステージにおいて `kBufferCount = 6` のリングバッファを使用する。
これにより、**「書き込み」「転送」「読み出し」の操作が完全に独立して並列動作**し、突発的な負荷やジッターに対してより堅牢になる。

---

### ステージ 1: CEF レンダリング (Producer)
- **担当クラス**: `CefRenderHandlerImpl`
- **実行スレッド**: CEF UI Thread
- **駆動トリガー**: Chromiumコンポジターからの `OnPaint` コールバック
    - ブラウザ内部の描画更新（DOM変更、CSSアニメーション、`requestAnimationFrame`）により発火。
    - 基本的にWindowsのVSync (60Hz) に同期しようとするが、負荷により変動する。
- **動作レート**: 可変 (Max 60fps)
- **ロジック**:
    - `OnPaint` にて、描画されたピクセルデータ (BGRA) を受け取る。
    - **インデックス管理**:
        - `m_writeIndex` (Atomic): 現在書き込むべきバッファのインデックス（単調増加）。
        - `m_readIndex` (Atomic): GPU側が処理を開始したインデックス。
    - **オーバーフロー保護**:
        - `(m_writeIndex - m_readIndex) >= 6` の場合、リングバッファが満杯である。
        - **処理**: 最新のフレームをドロップし、Producer側で待機は行わない（Non-blocking）。
    - **データコピー**:
        - `m_cpuBuffers[m_writeIndex % 6]` に `memcpy` を行う。
        - コピー完了後、`m_writeIndex` をインクリメント (Release Semantics)。

---

### ステージ 2: GPU アップロード (Consumer 1)
- **担当関数**: `CefRenderHandlerImpl::SyncWithGPU()`
- **実行スレッド**: DeckLink Video Output Thread (High Priority)
- **駆動トリガー**: `IDeckLinkVideoOutputCallback::ScheduledFrameCompleted`
    - DeckLinkハードウェアの水晶発振器（ハードウェアクロック）に基づく正確な割り込み。
- **動作レート**: 固定 29.97fps (59.94 fields/sec)
    - 放送規格に厳密に従う。CEF側が遅くても速くても、このリズムは絶対である。
- **ロジック**:
    - **キャッチアップ戦略 (Frame Skipping/Duplication)**:
        - `m_writeIndex > m_readIndex` (新データあり): キューにある未処理フレームを消費する。
        - `m_writeIndex == m_readIndex` (新データなし): 前回のフレームを再送する（実質的にフリーズ画）。
        - アニメーションの滑らかさを維持するため、1回のコールバックにつき1フレームずつ進める (`m_readIndex` + 1)。
    - **H2D 転送**:
        - `ID3D11DeviceContext::Map` (`D3D11_MAP_WRITE_DISCARD`) でテクスチャ転送。
    - **Safe SRV更新**:
        - 転送完了後、変数 `m_lastUploadedSRV` を更新。これはアトミック操作ではないが、DeckLinkスレッド内での順序性が保証されているため安全。

---

### ステージ 3: シェーダー処理 & パイプライン・リードバック (Consumer 2)
- **担当クラス**: `ShaderManager`
- **実行スレッド**: DeckLink Video Output Thread (ステージ2と同一)
- **駆動トリガー**: ステージ2の完了直後 (同期待ちなしで連続実行)
- **動作レート**: 固定 29.97fps
- **ロジック**:
    1.  **Compute Shader Dispatch**:
        - 入力: CEF画像 (`t0`) + 前フレーム画像 (`t1`: インターレース生成用)
        - 出力: `m_outputTextures[frame % 6]` (UAV `u0`)
        - 処理: 色変換 (BGRA -> ARGB)、インターレース処理など。
    2.  **VRAM内コピー**:
        - `CopyResource` を発行 (UAV -> Staging)。GPUコマンドキューに積まれるのみで、CPUはこの時点ではブロックされない。
    3.  **遅延リードバック (Pipelined Readback) - 同期の肝**:
        - **戦略**: 「2フレーム前」のステージングテクスチャ (`frame - 2`) をマップする。
        - **同期の仕組み**:
            - D3D11の `Map` は、GPUがそのリソースへの書き込みを終えるまでCPUをブロックする仕様である。
            - 直前のフレームをMapするとCPUが数ミリ秒停止してしまう。
            - 2フレーム前のリソースであれば、GPU処理は（高負荷でない限り）確実に完了しているため、`Map` は即座にリターンする。
            - これにより、DeckLinkコールバック内での処理時間を最小化し、音声ドロップやフレーム遅延を防ぐ。

---

## 3. フレームレイテンシとシーケンス

このパイプライン構成における、ある1フレーム "N" のライフサイクルとレイテンシ。

| 時間軸 (DeckLink Tick) | アクション | トリガー | バッファの状態 |
| :--- | :--- | :--- | :--- |
| **T = 0** | **CEF描画** | Browser VSync | CEFが `CPUBuffer[0]` にフレームNを書き込み (Writer=1) |
| **T = 1** | **GPUアップロード** | DeckLink IRQ | D3Dが `CPUBuffer[0]` を `Texture[0]` に転送 (Reader=1) |
| **T = 1** | **GPU計算** | (Sequential) | シェーダーが `Texture[0]` を処理 → `Staging[0]` へコピー命令 |
| **T = 1** | **(Readback)** | (Sequential) | *フレーム N-2* を読み出し中 (フレームNはまだGPU内にあるため触らない) |
| **T = 2** | ... | DeckLink IRQ | 他のフレームの処理 ... |
| **T = 3** | **CPUリードバック** | DeckLink IRQ | `m_frameIndex` が進み、`Staging[0]` (フレームN) が対象になる。<br>この時点で `Staging[0]` のGPU書き込みは完了しており、即座にメモリ取得可能。DeckLinkへ出力。 |

**合計レイテンシ**:
CEFでの描画完了からDeckLink出力まで、**約 2〜3 フレーム (約66ms〜100ms)** の遅延が発生する。これは放送用途としては許容範囲内であり、引き換えに **高い安定性（ジッター耐性）** を獲得している。

---

## 4. 同期とスレッドセーフティ

### 使用している同期プリミティブ
- **std::mutex**:
    - `CefRenderHandler` 内のメンバ変数 (`m_width`, `m_height` 等) およびSRVポインタの保護に使用。
    - CEFスレッドとDeckLinkスレッドの競合を防ぐ。
- **std::atomic<size_t>**:
    - `m_writeIndex`, `m_readIndex` に使用。
    - ロックフリーに近い形でプロデューサー・コンシューマー間の進捗を確認する。

### D3D11 リソースフラグ
- **m_textures**: `D3D11_USAGE_DYNAMIC` + `D3D11_CPU_ACCESS_WRITE`
    - CPU（System Memory）から頻繁に書き換える用途に最適化されている。
- **m_stagingOutputs**: `D3D11_USAGE_STAGING` + `D3D11_CPU_ACCESS_READ`
    - GPUからCPUへのデータ転送専用。GPUはこのリソースに対してレンダリングできないが、`CopyResource` の宛先にはなれる。
