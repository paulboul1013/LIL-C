# LIL 語法規格

LIL 採用精簡的語法：目前可解析的運算式以分號結尾，關鍵字不分大小寫，`%` 表示單行註解；空格、水平 tab、CR 與 LF 不具語法意義。字面值目前只有十進位整數與單一字元。`examples/basic.lil` 另展示類 C 的預定控制流程外形：條件放在圓括號中，區塊以大括號界定。

本文件完整記錄目前儲存庫可驗證的語法。lexer 已支援下列全部 token；parser 會解析完整檔案中的運算式並建立 AST。控制流程依 `examples/basic.lil` 或 token 定義分別標示，尚不能視為已實作語法。

## EBNF 記號

- `=` 定義產式，`|` 表示擇一。
- `{ ... }` 表示重複零次以上，`[ ... ]` 表示可省略。
- `,` 表示前後項目依序串接，`( ... )` 用於群組。
- `A - B` 表示屬於集合 A 但不屬於集合 B。
- 引號內文字是必須原樣出現的終端符號。

`source-character` 表示任意輸入位元組；`letter` 由目前執行環境的 C `isalpha()` 判斷，因此非 ASCII 字母的結果可能隨 locale 改變。

## 詞法規則（已實作）

```ebnf
ignored             = { whitespace | line-comment } ;
whitespace          = " " | "\t" | "\r" | "\n" ;
line-comment        = "%", { source-character - "\n" } ;

identifier-spelling = identifier-start, { identifier-continue } ;
identifier-start    = letter | "_" | "." ;
identifier-continue = letter | digit | "_" | "." ;
decimal-integer     = digit, { digit } ;

character-literal   = "'", (ordinary-character | escape-sequence), "'" ;
ordinary-character  = source-character - ("'" | "\\" | "\n") ;
escape-sequence     = "\\", ("0" | "n" | "r" | "t" | "\\" | "'") ;

keyword             = "if" | "else" | "while" | "do"
                    | "break" | "continue" ;
punctuation         = "(" | ")" | "{" | "}" | ";" | "," ;
operator            = "=" | "+" | "-" | "*" | "/"
                    | "&" | "|" | "~" | "&&" | "||"
                    | "<" | "<=" | ">" | ">=" | "==" | "~=" ;
digit               = "0" | "1" | "2" | "3" | "4"
                    | "5" | "6" | "7" | "8" | "9" ;
```

關鍵字必須與整個 `identifier-spelling` 不分大小寫地完全相等，因此 `if`、`IF` 與 `If` 是關鍵字，`ifx` 與 `if.loop` 則是 identifier token。token 會保留來源中的原始拼法；名稱解析的大小寫語義尚未定義。lexer 使用最長匹配辨識雙字元運算子。LIL 以 `~=` 表示不等於，不支援 `!` 或 `!=`。

識別字允許 `.` 作為第一個或後續字元，例如 `.loop`、`r0.tmp`，甚至單獨的 `.`。數字只接受無正負號的十進位整數；沒有浮點數、進位前綴或指數格式。負號會成為獨立 token。`123abc` 會拆成 `123` 與 `abc`，`123.4` 會拆成 `123` 與 `.4`，而非直接形成單一數值。

字元常值必須恰好包含一個原始字元或一個受支援的跳脫序列，例如 `'A'`、`'\n'`、`'\\'`。空字元、多字元、未閉合字面值及其他跳脫序列皆為詞法錯誤。

任何未列出的字元都會產生 invalid token。lexer 會區分空字元、未閉合字元常值、不支援的跳脫序列，以及超過一個字元等錯誤；parser 遇到第一個 invalid token 即停止。

## Parser 語法（已實作）

```ebnf
program        = { expression, ";" } ;
expression     = assignment ;
assignment     = additive, [ "=", assignment ] ;
additive       = multiplicative, { ("+" | "-"), multiplicative } ;
multiplicative = primary, { ("*" | "/"), primary } ;
primary        = identifier-token | decimal-integer | character-literal ;
```

`=` 的左側必須是單一 identifier。賦值為右結合；算術運算為左結合，且 `*`、`/` 的優先序高於 `+`、`-`。parser 必須消耗整份輸入到 EOF，不會忽略第一個分號後的內容。空檔案會產生不含 expression 的 `Program`。

`identifier-token` 是符合 `identifier-spelling`、但不與任何關鍵字完整相等的 token。

有效範例：

```lil
x;
r0 = x;
r0 = x + 10;
r0 = x + y * z;
```

parser 以 tagged union AST 表示 variable、literal、binary 與 assignment expression。`x + y = z;` 會因左側不是單一 variable 而失敗。括號分組及一元 `+`、`-`、`~` 尚未支援。

## 控制流程（範例顯示，尚未實作）

`examples/basic.lil` 顯示下列非規範性的預定外形；其中 expression statement、一般 statement、空區塊及多敘述區塊都尚未由 parser 定義或驗證：

```ebnf
expression-statement = expression, ";" ;
block                = "{", { statement }, "}" ;
if-example           = "if", "(", expression, ")", block ;
while-example        = "while", "(", expression, ")", block ;
```

```lil
IF (r0 < y) {
    r0 = y;
}

while (r0 ~= 0) {
    r0 - 1;
}
```

lexer 能辨識範例中的 `IF`、`<`、`~=`、圓括號與大括號，但 parser 尚未接受它們。`expression` 的完整優先序也尚未定義。

## 保留但未定義的語法

`else`、`do`、`break`、`continue` 與逗號已有 token，儲存庫中卻沒有對應 parser 或範例，故目前僅屬保留語法。現有資料也未定義下列行為：

- `else` 的結合方式，以及 `if` 或迴圈是否允許不帶大括號的單一敘述。
- `do` 是否採 `do { ... } while (...);` 形式。
- `break`、`continue` 的合法作用域。
- 比較、位元、邏輯、賦值與算術運算子的優先序及結合性。
- 逗號的用途，以及函式、宣告、字串、陣列或其他複合語法。

在這些規則寫入 parser 並補上測試前，不應將推定的類 C 行為當成 LIL 正式文法。
