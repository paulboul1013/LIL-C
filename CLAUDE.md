# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 回答語言

**所有回答一律使用繁體中文(台灣用語)**,包含說明、計畫、程式碼審查意見與 commit 以外的對話內容。程式碼、識別字、指令與錯誤訊息原文維持原樣。

## 專案概述

LIL-C 是以 1974 年 LIL 語言為藍本的編譯器,以 C 撰寫,目標是 `lilc prog.lil -o prog` 直接產生 Linux x86-64 靜態 ELF64 執行檔(不依賴 libc,最終版不呼叫外部 `as`/`ld`)。

- 整體路線圖與每個階段的「額外測試」與完成條件:`LIC_C_compiler_plan.md`(里程碑 M0–M6,每個里程碑有閘門,沒過不往下做)。
- 語言規格以 `docs/spec.md` 為準(含完整 EBNF 與編號條款);測試應能對應到規格條款。舊的 `parser_grammar.md` 已刪除,不要還原。
- 已定案的語意(勿重新討論):類 C 語意(只有 `=` 修改變數)、只有 int64、變數首次出現即隱式全域且初值 0、識別字與關鍵字皆不分大小寫、二補數環繞、除以零於執行期 SIGFPE 且常數摺疊不摺(`INT64_MIN / -1` 同理)、結束碼為 `r0 & 0xff`、內建 `putc(x)`、`0` 開頭為八進位、超過 int64 的數字是詞法錯誤。

## 常用指令

```sh
make                 # 建置編譯器 ./lilc
make test            # 單元測試 + golden 測試 + 小型差分測試(60 個程式)
make difftest        # 差分測試:300 個隨機程式比對 oracle、-O0、-O1(DIFFTEST_COUNT/DIFFTEST_SEED 可調,需 python3)
make sanitize        # ASan + UBSan 建置並跑測試
make update-golden   # 依實際輸出重生 golden 期望檔(之後務必檢視 diff)
make coverage        # gcov 行覆蓋率(每階段目標 ≥ 90%)
make fuzz && make fuzz-run FUZZ_TIME=60   # libFuzzer(clang);前端與 IR 文字兩個目標,FUZZ_TARGET=frontend|ir 只跑一個
make clean
```

只跑單一 golden fixture 或單一目錄:

```sh
sh tests/test_cli.sh ./lilc tests/golden/ast/NAME.lil
sh tests/test_cli.sh ./lilc tests/golden/tokens
```

## 架構

管線:`Source` → `Lexer` → `Parser` → `Program`(AST)→ 語意分析(`src/sema.c`,符號表在 `src/symbol.c`)→ IR 降階(`src/ir_lower.c`)→ 驗證(`src/ir_verify.c`)→ `-O1` 優化(`src/ir_opt.c`,規格 §8.7)→(規劃中)x86-64 backend → 自製組譯器 / ELF 寫入器。每個階段都要能用 CLI 旗標單獨傾印中間產物(`--dump-tokens`、`--dump-ast`、`--dump-ir`、`--dump-asm`,以及 `-O0`/`-O1`),這些輸出就是 golden 測試的比對對象,所以輸出格式必須穩定。

跨檔案才看得出來的設計重點:

- **錯誤回報一套到底**:`Source_Span`(offset、length、line、column)指回原始碼;`Diagnostic` 帶階段標籤(`DIAGNOSTIC_LEXICAL`/`SYNTAX`/`SEMANTIC`/`INTERNAL`)與等級(`DIAGNOSTIC_ERROR`/`WARNING`)。錯誤與警告各放一個 `Diagnostic_List`(上限各 20),輸出時依位置合併。新階段沿用同一套,不要另起爐灶。
- **前端進入點在 `src/lil.c`**:`lil_analyze_source()` 是完整前端(lexer → parser → 語意分析),`src/main.c` 與後續階段用它;`lil_run_source()` 只做詞法與語法分析(parser 測試用,會接受 `f();` 之類語意錯誤)。兩者都複製原始碼進 `Lil_Result`,無論成功或失敗都要呼叫 `lil_result_free()`;各 `*_free` 可重複呼叫。`src/main.c` 只負責讀檔、呼叫它、依旗標輸出。
- **語意分析後的 AST 保證可降到 IR**:每個變數節點帶符號編號(`Expr.symbol`)、`putc` 呼叫標成 `BUILTIN_PUTC`、符號表在 `Lil_Result.symbols`。完整不變量清單與檢查函式 `sema_verify()` 在 `include/sema.h`(規格 §5.8);後續階段不再檢查使用者錯誤。
- **Parser 從 Lexer 拉 token**(`parser_create(&lexer)`),不是先產生完整 token 陣列。
- **`src/ast_debug.c`、`src/token_debug.c`(`DEBUG_SRCS`)只連結進 CLI 與 fuzz harness**,不在單元測試的 `CORE_SRCS` 裡;新增原始檔時記得同時更新 `Makefile` 的對應清單。
- **IR(規格 §8,`include/ir.h`)**:線性三位址碼、無限虛擬暫存器(非 SSA,只有 `&&`/`||` 的結果在兩條路徑各定義一次)、全域變數編號等於符號編號、每個區塊恰好以一個終結指令(`jmp`/`br`/`exit`)結尾。`--dump-ir` 的文字格式由 `src/ir_parse.c` 原樣讀回(dump → parse → dump 完全相同)。降階後與優化後一律跑 `ir_verify()`;新的 pass 必須維持驗證器的所有規則。
- **`-O1`(規格 §8.7)**:pass 全是區塊內、保守的(非 SSA,已知值遇重新定義即失效;唯一跨區塊事實是「唯一定義為 const」的暫存器)。可能 trap 的 `div` 永不摺疊、化簡或刪除。pass 回傳是否改變;驅動跑到不動點(有終止證明與輪數上限),除錯建置每個 pass 後都跑驗證器。新增 pass 時更新 `Ir_Pass`、§8.7 的規則表與終止量度,並加 `ir/opt_NAME_*.ir` golden(`--passes=NAME`)與反例。
- **IR 直譯器(`src/ir_interp.c`)是「正確答案」來源**:直譯器、`-O0`、`-O1`、系統 `as` 與自製組譯器的結果必須完全一致(差分測試)。運算語意只寫在 `ir_eval_binary()`/`ir_eval_unary()`,常數摺疊必須共用它們。CLI:`--interp`(執行,trap 時以 SIGFPE 結束,狀態 136)、`--ir-input`(原始檔是 IR 文字,可配 `--dump-ir`/`--interp`/`-O1`,供 pass 的「輸入 IR → 期望 IR」golden)、`--step-limit N`(直譯器步數上限)。

## 測試

- 單元測試:`tests/test_lil.c`(lexer/parser)、`tests/test_sema.c`(符號表、語意分析)、`tests/test_ir.c`(IR、降階、驗證器、直譯器)與 `tests/test_ir_opt.c`(`-O1`:每個 pass 後驗證、冪等、不動點會停、隨機與對抗性 IR、行為與 `-O0` 相同),使用 `tests/test_framework.h` 的 `TEST()`/`EXPECT_EQ()` 巨集(失敗時印出測試名、檔案、行號、期望與實際值並繼續執行)。
- Golden 測試:`tests/test_cli.sh` 掃描 `tests/golden/{tokens,ast,ir,interp,asm,cli,run}/`。每個 fixture 是 `NAME.lil`(或 IR 文字 `NAME.ir`,自動加 `--ir-input`)加上 `NAME.stdout`/`.stderr`/`.status`,可選的 `NAME.args` 放額外 CLI 參數。所在目錄決定執行方式(例如 `ast/` 會加 `--dump-ast`、`interp/` 會加 `--interp`;`run/` 會先編譯再執行產出的程式並比對其輸出)。fixture 以 repo 相對路徑執行,所以 `.stderr` 中的路徑是 `tests/golden/<dir>/NAME.lil`。被訊號終止時,狀態碼記為 128+N。
- 每一種錯誤訊息都要有負面 fixture,比對訊息、行、列與結束碼。
- `-O1`:`interp/` 的 fixture(含驗收程式)會再以 `sh tests/test_cli.sh --interp-flag -O1 ./lilc tests/golden/interp` 比對同一份期望檔;單一 pass 的 golden 用 `--passes=fold,dce` 之類的 `.args`。
- 差分測試工具在 `tests/tools/`:`gen_lil.py`(隨機且保證會結束的程式產生器)、`lil_oracle.py`(只依規格寫成的獨立 oracle)、`diff_test.py`(`python3 tests/tools/diff_test.py ./lilc [--count N] [--seed S] [--keep DIR] [FILE.lil ...]`)。發現不一致時用 `--keep` 保存程式,修正後加成 golden。
- 驗收程式在 `examples/accept/`,手動驗證過的期望結果在 `tests/golden/interp/accept_NAME.*`(`examples/NAME.lil` 對應 `example_NAME.*`);兩邊的 `.lil` 必須逐位元組相同(`tests/check_examples.sh` 會檢查)。`make update-golden` 會覆寫這些期望檔,務必對照獨立計算的數值檢視 diff。
- 編譯旗標為 `-Wall -Wextra -Wpedantic`,新程式碼不得產生新警告;編譯器本身不可有 signed overflow 等 UB(sanitize 會抓)。
