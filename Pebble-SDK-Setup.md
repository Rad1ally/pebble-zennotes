# ビルドと導入の手順

ZenNotes for Pebble をソースからビルドし、時計に入れるまでの手順。Linux と macOS で使える。

## 必要なもの

- Pebble Time 2(emery)か、gabbro の時計
- 時計とペアリングしたスマホの Pebble アプリ(Android)
- ノートを置いた ZenNotes のサーバーと、その API トークン

## SDK のインストール

CLI は `pebble-tool` を使う。[uv](https://docs.astral.sh/uv/) でインストールし、Python のバージョンを指定する(3.13 で動作を確認済み)。

```bash
uv tool install pebble-tool --python 3.13
pebble --version
pebble sdk install latest
pebble sdk list
```

`pebble sdk install` を実行すると、SDK・ARM ツールチェーン・JS の依存関係を `~/.local/share/pebble-sdk/` にダウンロードする。ビルドだけなら、追加のシステムパッケージは要らない。

## ビルド

```bash
git clone <このリポジトリの URL>
cd pebble-zennotes
pebble build    # -> build/pebble-zennotes.pbw
```

## 時計に入れる

スマホの Pebble アプリで開発者接続(Dev Connection)を ON にしてから、次のどちらかでインストールする。

```bash
# スマホと同じ LAN から
pebble install --phone <スマホの IP アドレス>

# クラウド経由(先に pebble login が必要)
pebble login
pebble install --cloudpebble
```

ログを見るには `pebble logs` を、画面を撮るには `pebble screenshot` を使う。オプションは `pebble install` と同じ。

エミュレータ(`pebble install --emulator emery`)でも動くが、エミュレータには日本語のフォントがない。日本語の表示は実機で確かめる。

## 初回の設定

1. スマホの Pebble アプリで、このアプリの設定画面を開く。
2. ZenNotes サーバーの URL と API トークンを入れて保存する。
3. 時計でアプリを開くと、ノートの一覧が出る。

サーバーには、スマホの Pebble アプリから届く必要がある。外から使うなら HTTPS で公開するか、VPN を使う。
