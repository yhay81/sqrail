# コンセプト

## 仮説

コーディングエージェントは既にSQLを書くことができます。必要なのは自然言語を
SQLへ変換する別のAIではありません。ローカルファイルを名前付きSQLテーブルとして
扱い、短い説明だけで完全に予測できるコマンドです。

sqrailが最適化するのは、LLMでもクエリエンジンでもなく、その間にあるCLI境界です。

```text
AIが書いたSQL
      |
      v
name=file束縛 -> DuckDB -> JSONL標準出力または単一出力ファイル
```

## 設計原則

1. **エンジンは一つ**
   配布物に含めるのはDuckDBだけです。エージェントへbackend選択を見せません。

2. **SQLを独自言語へ変えない**
   独自DSLを追加せず、固定したバージョンのDuckDB SQLを使います。受け付けるのは
   一つのread-onlyな`SELECT`、`VALUES`、`WITH`だけです。

3. **全インターフェースを短い説明へ収める**
   `sqrail --help`は規範的かつ完全な説明です。

4. **stdoutはデータ専用**
   成功時の行はstdout、診断は一つのJSONとしてstderrへ出します。

5. **収集せずstreamingする**
   stdoutはchunk単位、ファイル出力はDuckDBの`COPY` pipelineを使います。
   C++側で全結果をメモリへ複製しません。

6. **入力はread-only、出力は明示的**
   `-t`の入力は一時viewとして公開します。既存出力は上書きせず拒否し、
   query成功後に同一directory内のhard linkを排他的に作ることでのみ出力を
   確定します。

7. **資源制約も公開契約に含める**
   メモリ、並列数、spill先、deadline、最終行数をコマンドごとに指定できます。

8. **軽量性を複数軸で測る**
   圧縮サイズ、展開サイズ、cold start、idle RSS、peak RSS、spill量、実行時間を
   計測します。

9. **隠れた知能を持たせない**
   LLM、prompt、agent loop、daemon、telemetry、cloud依存を内蔵しません。

## 対象外

- DuckDBそのものの置換
- 自然言語による質問
- 対話型database shell
- 永続databaseの管理
- 敵対的コードやfilesystemに対するsandbox
- backendの自動切替
- 全DuckDB extensionの同梱

## 性能目標

最高速度と最低メモリは競合するため、目的を次のように定義します。

```text
peak RSS <= M と結果一致を満たしながら、実行時間を最小化する
```

最初は512MB、1GB、4GBを基準にします。scan、filter、projection、
CSV→Parquet、低・高cardinality集計、大小join、sort、distinct、windowを
対象にします。

DuckDB以外は開発時のbenchmark対象にはできますが、同時には配布しません。

## C++を選ぶ理由

DuckDB本体と同じC++で小さなfrontendを書けば、別runtimeとbinding層を避け、
link対象を制御できます。profilingによって再現可能なbottleneckが見つかった場合は、
table functionやoperatorをC++で実装する道も残ります。

ただし、最初からSQLエンジンを自作しません。schema、SQL、設定では解消できない
狭いbottleneckがbenchmarkで証明された場合だけ、独自実装へ進みます。
