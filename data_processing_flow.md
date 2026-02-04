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
- **駆動トリガー**: Chromiumコンポジター (`OnPaint`)
- **動作レート**: **59.94 fps (Progressive)**
    - 滑らかなインターレース出力 (59.94i) を実現するため、ソースは **59.94p** でレンダリングされる必要がある。
    - 29.97fpsで描画した場合、DeckLink出力はカクつく（Double Framing）ことになるため、前段は倍速で動く必要がある。
- **ロジック**:
    - `OnPaint` にて、描画されたピクセルデータ (BGRA) を受け取る。
    - **インデックス管理**: `m_writeIndex` (Atomic) を毎フレーム進める。
    - **データコピー**: 常に新しいフレームをリングバッファ `m_cpuBuffers` へ書き込む。

---

### ステージ 2: GPU アップロード (Consumer 1)
- **担当関数**: `CefRenderHandlerImpl::SyncWithGPU()`
- **実行スレッド**: DeckLink Video Output Thread
- **駆動トリガー**: `DeckLinkDevice` コールバック (29.97Hz)
- **動作レート**: **スループット 59.94 fps相当** (1コールバックにつき2フレーム処理)
    - DeckLinkからの1回の呼び出しにつき、**2枚のソースフレーム** を処理・アップロードする必要がある。
    - 例: Output Frame N を作るために、Source Frame 2N と 2N+1 を使用する。
- **ロジック**:
    - `m_writeIndex` が十分進んでいる場合、2フレーム分のテクスチャをアップロードする。
    - **Safe SRV更新**: 最新の2枚のSRV (`Current`, `Previous`) を確保し、シェーダーへ渡す準備をする。

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
