# CEFDecklink

CEFDecklinkは、**Chromium Embedded Framework (CEF)** を使用してHTML/CSS/JSコンテンツをレンダリングし、**Blackmagic DeckLink** SDI経由で**Unmultiplied Fill & Key**信号として出力するアプリケーションです。

特に以下の設定に合わせて構成されています：
- **デバイス**: **UltraStudio HD Mini** (および同様のDeckLinkデバイス) と互換性があります。
- **フォーマット**: **1080i59.94** (NTSC)。
- **出力**:
  - **SDI A**: Fill信号
  - **SDI B**: Key信号

## 特徴
- **Unmultiplied Keying**: 放送用スイッチャー向けの適切なFillとKey信号を生成します。
- **CEF統合**: 最新のWebコンテンツをオフスクリーンでレンダリングします。
- **DirectX 11**: 効率的なGPUベースのテクスチャ共有と色空間変換を行います。
- **外部キーイング**: ハードウェア外部キーイングモードをサポートします。
- **シミュレーターモード**: DeckLinkデバイスが見つからない場合、ウィンドウプレビューにフォールバックします。

## 前提条件
- **Windows 10/11** (x64)
- **Visual Studio 2022** (標準的なMSVC C++ツールチェーン)
- **CMake** (3.20以降)
- **Blackmagic Desktop Video** ドライバがインストールされていること。
- **Inno Setup 6** (インストーラー作成用・オプション)

## クローンとビルド手順

### 1. リポジトリのクローン
```powershell
git clone <repository-url>
cd CEFDecklink
```

### 2. 依存関係のダウンロード

**A. Chromium Embedded Framework (CEF)**
1. [CEF Builds](https://cef-builds.spotifycdn.com/index.html) から **CEF 132.0.26+gea273c5+chromium-132.0.6834.83 (Windows 64-bit)** (または類似の互換バージョン) をダウンロードします。
2. アーカイブを展開します。
3. 展開したフォルダ名を `cef` に変更し、`vender` ディレクトリに配置します。
   - 構成: `vender/cef/LICENSE.txt`, `vender/cef/libcef_dll` など。

**B. Blackmagic DeckLink SDK**
1. [Blackmagic Design Developer Website](https://www.blackmagicdesign.com/developer/) から **Desktop Video SDK 14.0+** をダウンロードします。
2. アーカイブを展開します。
3. SDKフォルダを `vender` ディレクトリに配置します。
   - 構成: `vender/Blackmagic DeckLink SDK 15.3/Win/include/...`

### 3. プロジェクトのビルド
プロジェクトの構成とビルドを自動的に行う便利なバッチスクリプトを提供しています。

プロジェクトのルートから `build.bat` を実行してください：
```powershell
.\build.bat
```
これにより、以下の処理が行われます：
1. Visual Studio Build ToolsとCMakeを特定します。
2. CMakeを使用してプロジェクトを構成します (`build` フォルダ)。
3. プロジェクトを **Release** モードでコンパイルします。
4. 必要なリソース (シェーダー、CEFバイナリ) を出力ディレクトリにコピーします。
5. **インストーラーを作成** します (Inno Setupがインストールされている場合)。インストーラーは `build\CEFDecklink-Setup.exe` に生成されます。

#### オプション：インストーラーの作成
ビルド時にインストーラーを自動作成するには：
1. [Inno Setup 6](https://jrsoftware.org/isdl.php) をダウンロードしてインストールします。
2. 通常通り `build.bat` を実行します。
3. `build\CEFDecklink-Setup.exe` にインストーラーが作成されます。

**注意**: インストーラーにはサイズ削減のため、必須のロケールファイル（日本語と英語）のみが含まれます。Inno Setupがなくてもビルドプロセスは動作しますが、インストーラーは生成されません。

## インストールと実行

### インストーラーを使用する場合 (推奨)
1. 生成された `build\CEFDecklink-Setup.exe` を実行します。
2. 画面の指示に従ってインストールします。デスクトップショートカットを作成するオプションもあります。
3. スタートメニューまたはデスクトップのショートカットからアプリケーションを起動します。

### インストーラーを使用しない場合
ビルド成功後、実行ファイルはリソースとの相対パス関係に依存します。
```powershell
.\build\Release\DeckLinkDX11.exe
```

## 設定

アプリケーションは以下の優先順位で設定（URL、アルファ閾値）を決定します：
1.  **コマンドライン引数**
2.  **`config.json`** (実行ファイルと同じディレクトリ)
3.  **デフォルト値**

### 1. コマンドライン引数
```powershell
.\DeckLinkDX11.exe --url "http://localhost:3000/cg" --alpha 0.5
```
- `--url`: 読み込む初期URLを指定します。
- `--alpha`: 初期のアルファ閾値を設定します (0.0 - 1.0)。

### 2. config.json
`DeckLinkDX11.exe` の隣に `config.json` ファイルを作成します：
```json
{
    "url": "http://localhost:9090/graphics/on_air.html",
    "alpha": 0.01
}
```

## 操作と機能
- **プロジェクト構造**:
    - `src/`: モジュールごとに整理されたソースコード (core, render, decklink, cef)。
    - `scripts/`: ユーティリティスクリプト。
    - `docs/`: ドキュメントとログ。
- **操作**:
    - **F11**: 全画面表示の切り替え。
    - **D**: **Diffモード** の切り替え (フレームTとT+1の差分を可視化)。
    - **P**: **プログレッシブモード** の切り替え (高フレームレートウィンドウ出力ロジック)。
    - **+ / -**: アルファ閾値の調整 (キーイング用)。
    - **Ctrl + C**: アプリケーションの終了。

## トラブルシューティング
- **"Shader Compile Failed"**: 実行ファイルの隣に `shaders` フォルダがあることを確認してください。`build.bat` はこのコピーを自動的に行います。
- **"Decklink not found"**: Blackmagic Desktop Videoドライバがインストールされていることを確認してください。デバイスが見つからない場合、アプリは**シミュレーターモード**で起動します。
- **"Invalid file descriptor to ICU data"**: 実行ファイルを誤ったディレクトリから実行した場合に発生します。
