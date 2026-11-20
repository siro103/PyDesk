# PyDesk

PyDesk は、Python のコードと標準入力をひとつのウィンドウに書いて、すぐに実行結果を確認できる Windows アプリです。競技プログラミングのサンプル入力や、短いコードの動作確認に使えます。C++ と Windows 標準の RichEdit で作っています。

## 起動する

1. Windows 10 / 11（x64）で、Python 3 を使える状態にします。
2. [最新版の Windows x64 用 ZIP](https://github.com/siro103/PyDesk/releases/latest/download/PyDesk-windows-x64.zip) を展開し、`PyDesk.exe` を起動します。
3. 左にコード、右上に標準入力を書き、**Ctrl+Enter** で実行します。初回のサンプルは、入力した整数を合計して右下に `14` と表示します。

## 編集と実行

| 操作 | キー |
| --- | --- |
| 実行 / 停止 | Ctrl+Enter または F5 / Shift+F5 |
| 次の一致を選んで同時編集 | Ctrl+D |
| 行の移動 / 複製 | Alt+↑↓ / Alt+Shift+↑↓ |
| 行コメントの切り替え | Ctrl+/ |
| 検索 / 置換 | Ctrl+F / Ctrl+H |
| 単語補完 | Ctrl+Space |

`if` や `for` の行で Enter を押すと、次の行を4スペース字下げします。標準出力とエラーは同じ結果欄に表示されます。無題のコードと標準入力は、ウィンドウを閉じると次回に引き継がれます。[操作ガイド](README.txt)

## ソースからビルドする

Python 3、Visual Studio 2022 Build Tools の「C++ によるデスクトップ開発」、Windows SDK が必要です。展開先のフォルダーで次を実行すると、`build\PyDesk.exe` ができます。

```bat
.\build.cmd
```

編集処理と Python 実行のテスト、および実際のウィンドウを使う操作テストは、次のコマンドで実行できます。

```bat
.\build.cmd test
.\build.cmd ui-test
```

## ライセンス

PyDesk のソースコードと実行ファイルは [MIT License](LICENSE) で配布しています。配布用の ZIP には著作権表示とライセンス全文を同梱しています。
