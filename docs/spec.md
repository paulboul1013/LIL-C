# LIL-C v1 語言規格

本文件是 LIL-C v1 的**規範性**規格,取代已移除的 `parser_grammar.md`。設計決策來自 `LIC_C_compiler_plan.md` 的「先定下的語言設計決策」一節,全部採用其中的「建議」欄。

每一條規則都有編號(例如 §4.6),測試以編號引用規格:單元測試在 `TEST()` 上方的註解寫 `/* §x.y ... */`,golden fixture 的第一行寫 `% spec: §x.y ...`。修改語言行為時,必須同時修改本文件與對應測試。

## §1 範圍與記號

### §1.1 範圍

LIL-C v1 是一個只有 64 位元有號整數的小型命令式語言。程式由一連串敘述組成,在 Linux x86-64 上編譯為靜態 ELF64 執行檔,不連結 libc。

v1 **不包含**:PDP-11 定址模式、`byte`/`word` 屬性、編譯期運算式 `"..."`、labeled group、`goto`、前處理器、函式定義、陣列、指標、字串常值、`%`(取餘數)運算子、位移運算子、`^`、`!`、`!=`。

### §1.2 EBNF 記號

- `=` 定義產式,`;` 結束產式,`|` 表示擇一。
- `{ X }` 表示 X 重複零次以上,`[ X ]` 表示 X 可省略,`( ... )` 用於群組。
- `,` 表示依序串接。
- `A - B` 表示屬於 A 但不屬於 B。
- 引號內文字是終端符號;關鍵字終端符號不分大小寫比對(§2.5)。
- `source-byte` 表示任意輸入位元組(0–255)。

### §1.3 規範用語

「必須」「不得」是對實作(編譯器與其產生的程式)的要求。「錯誤」表示編譯器必須回報診斷(§6.1)並以結束碼 1 結束,不得產生輸出檔。「警告」表示回報診斷但繼續編譯。

### §1.4 實作狀態

本規格描述 v1 的目標行為。各條目的實作進度如下;尚未實作的條目由對應階段補上,完成時更新本表。

| 條目 | 內容 | 狀態 |
| --- | --- | --- |
| §2 | 詞法 | 已實作(階段 1) |
| §3 | 敘述 | 語法已實作(階段 2);`break`/`continue` 位置檢查(§3.7)與無效果警告(§3.2)已實作(階段 3) |
| §4 | 運算式 | 語法(優先序、結合性、呼叫)已實作(階段 2);呼叫與 `putc` 的語意檢查(§4.4、§4.10)已實作(階段 3);求值語意由 IR 降階與直譯器實作(階段 4,§8),機器碼待階段 5 |
| §5 | 名稱與執行語意 | §5.1–§5.4 與 §5.8(符號表、名稱解析、`putc` 檢查、語意分析的保證)已實作(階段 3);§5.5–§5.6 的執行語意已由 IR 直譯器實作(階段 4);§5.7 的優化(`-O1`)已實作(階段 4 第二部分,§8.7) |
| §6 | 診斷與 CLI | §6.1 錯誤格式(含 `semantic`、`ir`、`verify` 階段)、§6.2 警告、§6.3 的錯誤恢復與兩個深度上限、§6.4 的命令列解析與 `--dump-tokens`、`--dump-ast`(§6.4.3,含符號編號)、`--dump-ir`、`--interp`、`--ir-input`、`--step-limit` 已實作;`--dump-asm` 與產生執行檔回報「尚未實作」 |
| §8 | IR | 結構、文字格式、驗證器、降階、直譯器(階段 4 第一部分)與 `-O1` 的優化 pass(第二部分,§8.7)已實作 |

## §2 詞法

原始檔是一串位元組,不做任何字元編碼解碼。lexer 由左到右切出 token,每個 token 帶有 span:起始 offset(從 0 起算的位元組位置)、長度、行與列。

### §2.1 空白

空白字元為空格(0x20)、水平 tab(0x09)、CR(0x0D)、LF(0x0A)。空白只用來分隔 token,不具語法意義。

行號從 1 開始,每個 LF 之後行號加一。列號從 1 開始,以**位元組**計算:tab 與 CR 都只算一列,多位元組字元的每個位元組各算一列。

### §2.2 註解

`%` 開始一個單行註解,延伸到下一個 LF 之前(不含 LF)或檔案結尾。只有 LF 結束註解:註解中單獨的 CR(後面不是 LF)仍屬於註解。註解等同空白。字元常值內的 `%` 不開始註解。

### §2.3 Token 切分與最長匹配

跳過空白與註解後,lexer 在目前位置取**最長**的合法 token。因此:

- `<=`、`>=`、`==`、`~=`、`&&`、`||` 是單一 token;`<` 後面接 `=` 時一律組成 `<=`。
- `a~=b` 是 `a`、`~=`、`b`;要寫「`a` 等於 `~b`」須寫成 `a == ~b`。`x=~y` 是 `x`、`=`、`~`、`y`(沒有 `=~` token)。
- 沒有 `--` token:`--x` 是兩個 `-` 加上 `x`。
- 數字 token 是最長的十進位數字串;數字之後緊接的字母或 `.` 開始下一個 token,所以 `123abc` 是 `123`、`abc`,`123.4` 是 `123`、`.4`。

檔案結尾不需要換行。檔案結尾產生一個長度為 0 的 end token,其位置是檔案長度。

### §2.4 識別字

```ebnf
identifier-spelling = identifier-start, { identifier-continue } ;
identifier-start    = letter | "_" | "." ;
identifier-continue = letter | digit | "_" | "." ;
letter              = "A" | ... | "Z" | "a" | ... | "z" ;   (* 只有 ASCII *)
digit               = "0" | "1" | "2" | "3" | "4"
                    | "5" | "6" | "7" | "8" | "9" ;
```

識別字可以用 `.` 開頭,也可以只有一個 `.`,例如 `.loop`、`r0.tmp`、`.`。長度沒有上限。非 ASCII 位元組不是字母。

不與任何關鍵字相等(§2.5)的 `identifier-spelling` 是 `identifier` token。token 保留原始拼法;名稱比對規則見 §5.2。

### §2.5 關鍵字

```ebnf
keyword = "if" | "else" | "while" | "do" | "break" | "continue" ;
```

關鍵字不分大小寫:整個 `identifier-spelling` 經 ASCII 大小寫折疊後等於關鍵字時,它就是該關鍵字,例如 `IF`、`If`、`wHiLe`。`ifx`、`if.loop`、`.if` 都是識別字。

`putc` **不是**關鍵字,而是保留的內建名稱(§5.4)。

### §2.6 字元常值

```ebnf
character-literal  = "'", ( ordinary-character | escape-sequence ), "'" ;
ordinary-character = source-byte - ( "'" | "\" | LF ) ;
escape-sequence    = "\", ( "0" | "n" | "r" | "t" | "\" | "'" ) ;
```

字元常值的值是該位元組的**無號**值(0–255),型別仍是 int64。跳脫序列的值:`\0` = 0、`\n` = 10、`\r` = 13、`\t` = 9、`\\` = 92、`\'` = 39。

下列情形是詞法錯誤(§6.1,階段 `lexical`):

| 情形 | 範例 | 訊息 |
| --- | --- | --- |
| 空字元常值 | `''` | `empty character literal` |
| 在結尾引號前遇到 LF 或檔案結尾 | `'a` + 換行 | `unterminated character literal` |
| 不支援的跳脫序列 | `'\q'` | `unsupported escape sequence` |
| 超過一個字元 | `'ab'` | `character literal contains more than one character` |

「超過一個字元」的 span 從開頭引號起算,到第二個字元(含)為止;`'ab` 接 LF 也屬於此情形,因為第二個字元出現時就已確定。

跳脫序列先於結尾引號檢查:`\` 之後的字元不是 `0`、`n`、`r`、`t`、`\`、`'` 時一律回報 `unsupported escape sequence`,即使後面接著 LF 或檔案結尾(例如 `'\q` 接 LF 或檔案結尾)也是如此;只有 `\` 本身就在 LF 或檔案結尾之前時才是 `unterminated character literal`。

「結尾引號前遇到 LF 或檔案結尾」包含:開頭引號之後、跳脫的 `\` 之後、以及字元(或跳脫序列)之後還沒有結尾引號的情形,例如 `'`、`'a`、`'\`、`'\n` 各自接 LF 或檔案結尾。錯誤 span 從開頭引號起算。

### §2.7 整數常值

```ebnf
digit-string     = digit, { digit } ;               (* 最長匹配,§2.3 *)
integer-literal  = decimal-literal | octal-literal ;
decimal-literal  = "0" | nonzero-digit, { digit } ;
octal-literal    = "0", octal-digit, { octal-digit } ;
nonzero-digit    = "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" ;
octal-digit      = "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" ;
```

lexer 先以最長匹配切出整個 `digit-string`,再判斷它是哪一種常值:整個 `digit-string` 必須恰好等於一個 `decimal-literal` 或 `octal-literal`,否則是詞法錯誤(§2.7.2)。因此 `08` **不是** `0` 接 `8` 兩個 token,而是一個不合法的八進位常值。

#### §2.7.1 十進位

不以 `0` 開頭的數字串,或單獨的 `0`,是十進位。

#### §2.7.2 八進位

以 `0` 開頭且長度大於 1 的數字串是八進位(沿用原版 LIL)。例如 `010` = 8、`0777` = 511、`00` = 0。八進位數字串中出現 `8` 或 `9` 是詞法錯誤,訊息為 `invalid digit 'D' in octal literal`(D 是第一個不合法的數字),span 涵蓋整個數字串。這個檢查先於 §2.7.3 的範圍檢查:同時不合法又超出範圍的數字串回報 `invalid digit`。

#### §2.7.3 範圍

整數常值沒有正負號,其值必須介於 0 與 `INT64_MAX` = 9223372036854775807(八進位 0777777777777777777777)之間。超出範圍是詞法錯誤,訊息為 `integer literal out of range`,span 涵蓋整個數字串。

因此 `9223372036854775807` 合法,`9223372036854775808` 是錯誤;八進位 `0777777777777777777777` 合法,`01000000000000000000000` 是錯誤。前導零不影響範圍:`000000000000000000000001` = 1。要得到 `INT64_MIN` 須寫成 `-9223372036854775807 - 1`(負號是一元運算子,§4.5)。

v1 沒有十六進位、二進位、浮點數、指數或數字分隔符號。

### §2.8 標點、運算子與非法字元

```ebnf
punctuator = "(" | ")" | "{" | "}" | ";" | "," ;
operator   = "=" | "+" | "-" | "*" | "/" | "&" | "|" | "~"
           | "&&" | "||" | "<" | "<=" | ">" | ">=" | "==" | "~=" ;
```

`~=` 是不等於。任何不屬於空白、註解、識別字、關鍵字、常值、標點或運算子開頭的位元組(例如 `@`、`!`、`#`、`"`、`^`、`[`、NUL 位元組、非 ASCII 位元組)是詞法錯誤,訊息為 `unexpected character 'C'`。C 為可列印 ASCII(0x20–0x7E)時直接寫出該字元;其他位元組(控制字元、NUL、DEL、0x80–0xFF)寫成 `\xHH`(兩位小寫十六進位),例如 NUL 為 `unexpected character '\x00'`、UTF-8 `é` 的第一個位元組為 `unexpected character '\xc3'`。因此診斷永遠是一行合法的 ASCII 文字。

### §2.9 Token 文法總覽

```ebnf
token  = identifier | keyword | integer-literal | character-literal
       | punctuator | operator ;
ignored = { whitespace | comment } ;
whitespace = " " | TAB | CR | LF ;
comment    = "%", { source-byte - LF } ;
```

## §3 程式與敘述

### §3.1 程式

```ebnf
program = { statement }, END ;
```

程式是零個以上的敘述,解析必須消耗整份輸入直到檔案結尾。空檔案(或只有空白與註解)是合法的空程式。

在程式最外層(不在任何區塊內)出現的 `}` 是語法錯誤 `unexpected '}'`,位置在該 `}`。敘述開頭出現 `else`(沒有可配對的 `if`)是語法錯誤 `'else' without a matching 'if'`,位置在該 `else`。

### §3.2 運算式敘述與空敘述

```ebnf
expression-statement = expression, ";" ;
empty-statement      = ";" ;
```

運算式敘述後面缺少 `;` 是語法錯誤 `expected ';' after expression`,位置在運算式之後的那個 token。

運算式敘述對運算式求值並丟棄結果。求值的所有副作用(賦值、`putc` 輸出、除以零的 trap)都必須發生,即使結果沒被使用。沒有副作用的運算式敘述(例如 `r0 - 1;`)是合法的,但會產生警告(§6.2)。

「沒有副作用」以語法判定:運算式中任何位置(包括 `&&`、`||` 的右運算元)都沒有賦值、呼叫,也沒有 `/`。`/` 一律視為有副作用,因為除數可能為 0 而觸發 trap(§4.6),即使除數是非零常數也一樣。例:`r0 - 1;`、`(x);`、`-x < 2 && 'c';` 會產生警告;`r0 / x;`、`putc(x);`、`(x = 1) + 2;`、`x || putc(1);` 不會。空敘述 `;` 不是運算式敘述,不產生警告。本身有語意錯誤的運算式敘述(例如 `putc;`)只回報錯誤,不另外產生這個警告。

### §3.3 區塊

```ebnf
block = "{", { statement }, "}" ;
```

區塊依序執行其中的敘述。區塊**不**建立新的變數作用域(§5.1)。空區塊 `{}` 合法。

區塊到檔案結尾都沒有配對的 `}` 是語法錯誤 `expected '}' before end of file`,位置在 end token。

### §3.4 if / else

```ebnf
if-statement = "if", "(", expression, ")", statement, [ "else", statement ] ;
```

條件的值不為 0 時執行第一個敘述,否則執行 `else` 之後的敘述(若有)。分支可以是單一敘述,不必是區塊。

`if` 的兩個分支、`while` 與 `do` 的主體都必須是一個敘述(可以是空敘述 `;`)。這些位置上出現 `else`、`}` 或 end token 是語法錯誤 `expected statement`,位置在該 token(例:`if (x) else y;`、`if (x) a; else }`、`while (x) }`)。

**dangling else**:`else` 綁定到語法上最近、尚未配對 `else` 的 `if`。`if (a) if (b) x = 1; else x = 2;` 中的 `else` 屬於 `if (b)`。

`if` 之後不是 `(` 是語法錯誤 `expected '(' after 'if'`;條件之後不是 `)` 是語法錯誤 `expected ')' after condition`。兩者的位置都在出錯的那個 token。

### §3.5 while

```ebnf
while-statement = "while", "(", expression, ")", statement ;
```

每次迭代前對條件求值,不為 0 時執行主體。

`while` 之後不是 `(` 是語法錯誤 `expected '(' after 'while'`;條件之後不是 `)` 是 `expected ')' after condition`。

### §3.6 do / while

```ebnf
do-statement = "do", statement, "while", "(", expression, ")", ";" ;
```

先執行主體,再對條件求值,不為 0 時重複。主體至少執行一次。結尾的 `;` 是必要的。

主體之後不是 `while` 是語法錯誤 `expected 'while' after 'do' body`;`while` 之後的 `(` 與條件之後的 `)` 與 §3.5 相同;最後缺少 `;` 是 `expected ';' after 'do' statement`。位置都在出錯的那個 token。

### §3.7 break 與 continue

```ebnf
break-statement    = "break", ";" ;
continue-statement = "continue", ";" ;
```

`break`、`continue` 之後缺少 `;` 是語法錯誤 `expected ';' after 'break'` 與 `expected ';' after 'continue'`。

`break` 結束最內層的 `while` 或 `do` 迴圈。`continue` 跳到最內層迴圈的下一次條件求值(`do` 迴圈也是先求條件)。它們可以出現在迴圈內任意深度的 `if` 或區塊中。

在任何迴圈之外使用 `break` 或 `continue` 是語意錯誤(§6.1,階段 `semantic`),訊息分別為 `'break' outside of a loop` 與 `'continue' outside of a loop`,位置在該關鍵字。「迴圈之內」指位於某個 `while` 或 `do` 的**主體**中(經過任意層的區塊或 `if` 分支);`while` 的條件與 `do` 的條件都只能是運算式,不可能含有敘述。例:`while (x) { if (y) { { break; } } }` 合法;`if (x) { break; }`、迴圈結束之後的 `continue;` 是錯誤。

### §3.8 敘述文法總覽

```ebnf
statement = expression-statement
          | empty-statement
          | block
          | if-statement
          | while-statement
          | do-statement
          | break-statement
          | continue-statement ;
```

## §4 運算式

### §4.1 運算式文法

```ebnf
expression     = assignment ;
assignment     = identifier, "=", assignment
               | logical-or ;
logical-or     = logical-and, { "||", logical-and } ;
logical-and    = bitwise-or, { "&&", bitwise-or } ;
bitwise-or     = bitwise-and, { "|", bitwise-and } ;
bitwise-and    = equality, { "&", equality } ;
equality       = relational, { ( "==" | "~=" ), relational } ;
relational     = additive, { ( "<" | "<=" | ">" | ">=" ), additive } ;
additive       = multiplicative, { ( "+" | "-" ), multiplicative } ;
multiplicative = unary, { ( "*" | "/" ), unary } ;
unary          = ( "-" | "~" ), unary | primary ;
primary        = identifier
               | integer-literal
               | character-literal
               | "(", expression, ")"
               | call ;
call           = identifier, "(", [ argument-list ], ")" ;
argument-list  = expression, { ",", expression } ;
```

`assignment` 的第一個選項要求左側是**單一**識別字(§4.4)。實作上 parser 先照 `logical-or` 解析左側,遇到 `=` 時再檢查左側是否為不帶括號的單一識別字,不是就回報 `invalid assignment target`;接受的語言與上面的文法完全相同。

`primary` 中,識別字後面緊接 `(` 時一律解析為 `call`。parser 接受任何名稱、任何數量的引數;名稱與引數數量由語意分析檢查(§4.10)。

語法錯誤(位置都在出錯的那個 token):

| 情形 | 範例 | 訊息 |
| --- | --- | --- |
| 應出現運算元(含二元運算子的右運算元、一元運算子的運算元、括號內、條件、引數)的位置不是 `primary` 的開頭 | `a + ;`、`x = );` | `expected expression` |
| 括號運算式缺少 `)` | `(a + b;` | `expected ')' after expression` |
| 引數之後既不是 `,` 也不是 `)` | `putc(1 2);` | `expected ')' after arguments` |

括號只用於分組,不產生 AST 節點;括號運算式的 span 包含兩個括號(§6.4.3)。

### §4.2 優先序與結合性

由高到低(與 C 相同的相對次序):

| 層級 | 運算子 | 結合性 | 說明 |
| --- | --- | --- | --- |
| 1 | `( )`、呼叫 | — | 分組、`putc(x)` |
| 2 | 一元 `-`、`~` | 右 | 負號、位元反相 |
| 3 | `*`、`/` | 左 | 乘、除 |
| 4 | `+`、`-` | 左 | 加、減 |
| 5 | `<`、`<=`、`>`、`>=` | 左 | 關係比較 |
| 6 | `==`、`~=` | 左 | 相等比較 |
| 7 | `&` | 左 | 位元 AND |
| 8 | `\|` | 左 | 位元 OR |
| 9 | `&&` | 左 | 短路 AND |
| 10 | `\|\|` | 左 | 短路 OR |
| 11 | `=` | 右 | 賦值 |

例:`a || b && c` 是 `a || (b && c)`;`a = b = c` 是 `a = (b = c)`;`-a * b` 是 `(-a) * b`;`a < b == c` 是 `(a < b) == c`;`a & b == c` 是 `a & (b == c)`(同 C);`a - b - c` 是 `(a - b) - c`。比較可以串接,`a < b < c` 是 `(a < b) < c`。

v1 沒有一元 `+`。

實作:二元運算子的優先序與結合性只定義在一張表(`src/operator.c`),parser 以 Pratt/優先序爬升走訪這張表,pretty-printer(`src/ast_print.c`)也用同一張表決定哪裡需要括號;新增或調整二元運算子只需要改表。左結合的運算子串在迴圈中建樹,不會遞迴;唯一的右結合運算子是 `=`,由專門的賦值解析處理(左邊必須是識別字,§4.4),每一層計入 §6.3.2 的嵌套深度。新增其他右結合運算子時必須同時在 parser 中加上對應的處理(單元測試 `operator_table_has_only_assignment_right_associative` 會提醒)。

### §4.3 求值次序

所有值都是 int64。二元運算子(`&&`、`||`、`=` 除外)先完整求值左運算元,再求值右運算元。呼叫的引數由左到右求值。`&&`、`||` 見 §4.9,`=` 見 §4.4。

### §4.4 賦值

`=` 的左運算元必須是**單一識別字**(不得加括號);否則是語法錯誤,訊息為 `invalid assignment target`,位置在 `=` token。例:`x + y = z;`、`(x) = 1;`、`1 = x;` 都是錯誤。

求值時先求右運算元,再把結果存入變數;整個賦值運算式的值就是存入的值。賦值是 v1 中唯一會改變變數的運算(類 C 語意,不同於原版 LIL)。

對內建名稱 `putc` 賦值是語意錯誤(§5.4)。

### §4.5 算術與溢位

`+`、`-`、`*`、一元 `-` 採二補數環繞,結果取數學結果模 2^64 後解讀為 int64。溢位是**定義過的行為**,例:`9223372036854775807 + 1` = `-9223372036854775808`;`-(-9223372036854775807 - 1)` = `-9223372036854775807 - 1`。

### §4.6 除法

`a / b` 的商向零截斷(`-7 / 2` = `-3`,`7 / -2` = `-3`)。

- `b` = 0 時,執行期由 CPU 觸發 SIGFPE,程式被訊號終止(§5.6)。
- `a` = `INT64_MIN` 且 `b` = -1 時同樣觸發 SIGFPE(與 x86-64 `idiv` 一致)。

這兩種情形在任何優化等級下都不得被常數摺疊、刪除或改寫(§5.7)。v1 沒有取餘數運算子;餘數用 `a - a / b * b` 計算。

### §4.7 位元運算

`&`、`|` 對 64 位元二補數表示逐位元運算;一元 `~` 逐位元反相(`~x` = `-x - 1`)。

### §4.8 比較

`<`、`<=`、`>`、`>=`、`==`、`~=` 做有號比較,結果為 1(真)或 0(假)。

### §4.9 短路邏輯

`a && b`:先求 `a`;若為 0,結果為 0 且**不**求值 `b`;否則求 `b`,不為 0 時結果為 1,否則為 0。

`a || b`:先求 `a`;若不為 0,結果為 1 且**不**求值 `b`;否則求 `b`,不為 0 時結果為 1,否則為 0。

### §4.10 內建 putc

`putc(x)` 對 `x` 求值,透過 Linux `write(1, &byte, 1)` 系統呼叫把 `x & 0xff` 這個位元組寫到標準輸出;寫入失敗被忽略。整個呼叫運算式的值是 `x`(未截斷的 int64)。

下列是語意錯誤(§6.1,階段 `semantic`):

| 情形 | 範例 | 訊息 |
| --- | --- | --- |
| 引數數量不是 1 | `putc();`、`putc(1, 2);` | `'putc' expects 1 argument, got N` |
| 呼叫 `putc` 以外的名稱 | `f(1);` | `unknown function 'f'` |

兩者的位置都在被呼叫的名稱。名稱比對不分大小寫(`PUTC(1)` 是 `putc` 呼叫);`unknown function` 訊息中的名稱是原始拼法。呼叫有錯時其引數仍會被分析,引數中的錯誤照常回報。語意分析把每個合法的 `putc` 呼叫標記為內建呼叫(§5.8)。
## §5 名稱與執行語意

### §5.1 變數

變數不需要宣告:一個名稱第一次出現(不論讀或寫)即隱式宣告為**全域**變數。所有變數在程式開始時初值為 0。整個程式只有一個作用域。

語意分析(階段 3)以符號表記錄所有變數:每個變數有一個唯一的**符號編號**,從 0 開始、依名稱在原始碼中第一次出現的先後連續編號(賦值 `x = y` 中,`x` 先於 `y`)。同一個變數的每次出現(讀取或賦值)都帶有同一個編號,`--dump-ast` 會印出這個編號(§6.4.3)。

### §5.2 名稱不分大小寫

兩個識別字經 ASCII 大小寫折疊後相同,就是同一個變數:`X` 與 `x`、`R0` 與 `r0` 是同一個。關鍵字同樣不分大小寫(§2.5)。診斷訊息與 `--dump-*` 輸出保留原始拼法。

### §5.3 r0–r7

`r0` 到 `r7` 只是一般變數名稱,與 x86-64 暫存器無關;backend 自行分配暫存器。`r0` 唯一的特殊之處是程式結束碼(§5.5)。

### §5.4 保留名稱 putc

`putc`(不分大小寫)只能出現在呼叫位置 `putc(...)`。把它當變數讀取或賦值是語意錯誤,訊息為 `'putc' is a builtin function and cannot be used as a variable`,位置在該名稱(對賦值而言是 `=` 左邊的名稱)。例:`r0 = putc + 1;`、`putc(putc);`、`putc = 1;`、`x = PUTC = 1;`。`putc` 不會出現在符號表中。

`=` 的左邊只能是識別字已由 parser 保證(§4.4),所以「不是變數的賦值對象」在語意分析中只剩對 `putc` 賦值這一種。

### §5.5 程式執行與結束碼

程式從第一個敘述依序執行到最後一個敘述。執行完畢後,程式以 `r0 & 0xff` 作為結束碼呼叫 `exit` 系統呼叫(`r0` = -1 時結束碼為 255,`r0` = 256 時為 0)。程式從未提到 `r0` 時結束碼為 0。

### §5.6 執行期錯誤

v1 唯一的執行期錯誤是除法 trap(§4.6)。程式被 SIGFPE(訊號 8)終止,之前已經 `putc` 寫出的位元組仍然保留。測試以 shell 慣例記錄被訊號 N 終止的結束狀態 128 + N,即 SIGFPE 為 `136`。

### §5.7 優化的可觀察等價

任何優化等級(`-O0`、`-O1`)產生的程式,其可觀察行為必須與 IR 直譯器完全相同:標準輸出的位元組序列、結束碼或終止訊號。據此:

- 常數摺疊使用 §4.5 的環繞語意,`INT64_MAX + 1` 摺疊為 `INT64_MIN`。
- 除數為 0 或 `INT64_MIN / -1` 的除法不得摺疊,也不得因結果未使用而刪除。
- 短路運算(§4.9)的右運算元若有副作用,不得在左運算元決定結果時執行。

### §5.8 語意分析的保證

語意分析只在沒有詞法或語法錯誤時執行。通過語意分析(沒有任何錯誤,可以有警告)的 AST 保證可以降到 IR,後續階段不再檢查使用者錯誤:

1. 每個變數讀取與賦值節點都帶有符號編號,小於符號總數,且符號名稱與節點拼法在大小寫折疊後相同。
2. 符號表中沒有 `putc`。
3. 每個呼叫都是引數恰好一個、已標記為內建的 `putc` 呼叫。
4. 每個 `break`、`continue` 都在某個 `while` 或 `do` 的主體之中。
5. 符號編號是 0 到 總數−1 的連續整數;結束碼讀取 `r0` 的符號(程式從未提到 `r0` 時沒有這個符號,結束碼為 0)。
6. parser 的保證照舊成立:必要的子節點都存在、賦值左邊是識別字、樹高有上限(§6.3.1)。

這份清單的權威版本與檢查函式 `sema_verify()` 在 `include/sema.h`。

## §6 診斷與編譯器行為

### §6.1 錯誤診斷格式

錯誤寫到標準錯誤,一行一個:

```text
<path>:<line>:<column>: <stage> error: <message>
```

`<path>` 是命令列上給的原始檔路徑,`<line>`、`<column>` 依 §2.1 計算,指向錯誤 span 的起點。`<stage>` 是 `lexical`、`syntax`、`semantic` 或 `internal` 之一;以 `--ir-input` 讀入 IR 文字時另有 `ir`(IR 文字的語法錯誤,§8.3)與 `verify`(驗證器拒絕,§8.4)。有任何錯誤時編譯器結束碼為 1,且不產生輸出檔。

語意分析只在沒有詞法與語法錯誤時執行,所以 `semantic` 錯誤不會和 `lexical`、`syntax` 錯誤同時出現。語意錯誤同樣一次最多回報 20 個(`DIAGNOSTIC_MAX_ERRORS`),依原始碼位置嚴格遞增;達到上限時停止分析。訊息中超過 200 位元組的名稱只印前 200 個位元組,後面接 `...`。

### §6.2 警告

警告寫到標準錯誤,一行一個,格式為(沒有階段名稱):

```text
<path>:<line>:<column>: warning: <message>
```

警告不中止編譯,也不改變結束碼:只有警告時 `--dump-ast` 照常輸出並以 0 結束。v1 的警告(都由語意分析產生,所以只在沒有詞法與語法錯誤時出現):

| 情形 | 位置 | 訊息 |
| --- | --- | --- |
| 沒有效果的運算式敘述(§3.2) | 敘述開頭 | `statement has no effect` |
| 變數至少被賦值一次,但從未被讀取 | 第一次賦值的左邊名稱 | `variable 'NAME' is assigned but never read` |

第二種警告的 NAME 是該位置的原始拼法。`r0`(不分大小寫)永遠不產生這個警告,因為程式結束時會讀取它當結束碼(§5.5)。只被讀取、從未被賦值的變數不產生警告(它的值就是 0)。讀取出現在賦值之前也算讀取。

錯誤與警告合併後依原始碼位置先後印出。警告一次最多回報 20 個(`DIAGNOSTIC_MAX_WARNINGS`),即原始碼中最前面的 20 個,位置嚴格遞增。語意錯誤達到上限而提前停止分析時,讀取次數不完整,因此不回報「從未被讀取」警告。

### §6.3 穩健性

對任意位元組輸入,編譯器只能「成功」或「回報錯誤後以結束碼 1 結束」,不得崩潰、讀寫越界、觸發未定義行為、洩漏記憶體或無窮迴圈。這是 fuzz harness(`fuzz/fuzz_frontend.c`)檢查的契約。

- 錯誤恢復:遇到語法錯誤後跳到下一個 `;` 或 `}` 繼續解析,一次最多回報 20 個錯誤(§6.3.4)。
- 輸入大小:編譯時間與記憶體必須與輸入大小大致成線性(不得因敘述數量或運算子數量而出現平方成長)。

#### §6.3.1 為何需要深度上限

每一個走訪 AST 的遞迴 pass(釋放、`--dump-ast`、語意分析、IR lowering)每一層樹高都會用掉一層呼叫堆疊。只限制括號是不夠的:右結合的 `a = a = ... = 1` 與很長的左結合運算子串 `1 + 1 + ... + 1` 都會產生任意高的樹。因此 parser 必須在建樹時就拒絕過深的輸入,後續 pass 可以假設樹高有上限而安全地遞迴。

#### §6.3.2 嵌套深度上限

`=`(每個賦值的右運算元)、括號、呼叫的引數列、一元運算子、區塊與控制敘述(`if`、`while`、`do`)合計的**語法嵌套深度**上限為 256(`PARSER_MAX_NESTING`)。每一層的開啟 token 分別是 `=`、`(`、呼叫的 `(`、一元運算子、`{`、`if`、`while`、`do`。第 257 層是語法錯誤 `nesting too deep`,位置在開啟該層的 token(例如第 257 個 `=` 或第 257 個 `(`)。因此 10000 層括號只會得到一個 `nesting too deep`,不會堆疊溢位。

注意 `else if` 串中的每個 `if` 都各算一層(`else if` 在 AST 中就是巢狀的 `STMT_IF`,後續 pass 也會遞迴走訪),所以超過約 255 個 `else if` 的串是 `nesting too deep`;外層的區塊、控制敘述與運算式也共用同一個 256 的預算。程式產生器(包括後續階段的隨機程式產生器)必須遵守這個上限。不論串有多長,超過時整串只回報一次:§6.3.4 步驟 3 會跳過串中其餘的 `else`,而這些被跳過的部分本身也位於上限之外,其中再次超過上限不再回報(§6.3.4 步驟 3)。

串中較早的錯誤會改變深度錯誤的位置。§6.3.4 步驟 3 解析被放棄的 `if` 的 `else` 後敘述時,嵌套深度不是從那個 `if` 原本的深度起算,而是從被放棄的敘述所在的敘述串列起算(程式最外層為 0,區塊內為該區塊 `{` 所在的層數),也就是當作那個敘述串列的直接成員來解析。所以若第 k 個 `if` 在看到它的 `else` 之前就出錯(例如條件 `(x +)`),而它位於程式最外層,它的 `else` 之後的 `if` 從第 1 層重新算起:第 k + 1 到第 k + 256 個 `if` 佔第 1 到第 256 層,`nesting too deep` 回報在第 k + 257 個 `if`。例:600 個 `else if` 的串中,從第一個 `if` 算起的第 102 個 `if` 的條件出錯,深度錯誤就在第 359 個 `if`(golden `cli/recovery_chain_restarts_depth`)。

只有位於上限之外的結尾才不再回報深度錯誤(§6.3.4 步驟 3);同一個敘述中與那條串無關、本身又超過上限的部分仍各自回報一次。例:`if (` + 300 層括號 + `) a; else` + 300 層區塊,條件中的括號在第 257 個 `(` 回報;`if` 本身在第 1 層、它的 `else` 不在上限之外,所以 `else` 之後的 300 層區塊在第 257 個 `{` 再回報一次。

#### §6.3.3 運算式樹高上限

每個運算式的 AST 樹高(從根到最深葉節點路徑上的節點數;單一識別字或常值高度為 1;括號不產生節點;一元運算子與呼叫節點的高度是運算元或最高引數的高度加 1,沒有引數的呼叫高度為 1)上限為 1024(`PARSER_MAX_EXPR_HEIGHT`)。這也涵蓋二元運算子串:`1 + 1 + ... + 1` 中 n 個運算子產生高度 n + 1 的樹,所以最多 1023 個運算子。超過時是語法錯誤 `nesting too deep`,位置在使樹高超過上限的運算子 token(對賦值而言是 `=`)。

錯誤位置的精確規則(由內而外檢查,回報第一個超過上限的節點):

1. 二元運算子串由左而右建樹,每建一個運算子節點就檢查一次;第一個使高度超過 1024 的運算子就是錯誤位置。
2. 賦值 `NAME = RHS` 先依規則 1 完整解析並檢查 RHS;RHS 本身合法時,再檢查賦值節點(高度為 RHS 高度 + 1),超過時錯誤位置是該賦值的 `=`。
3. 巢狀賦值 `a = b = ... = RHS` 由最內層往外依規則 2 檢查,所以錯誤位置是使樹高超過上限的那個 `=`,也就是從右數來第一個超過的 `=`。

例:`x = 1 + 1 + ... + 1` 有 1023 個 `+` 時 RHS 高度恰為 1024(合法),加上賦值節點後為 1025,錯誤在 `=`;有 1024 個 `+` 時 RHS 自己就超過,錯誤在第 1024 個 `+`。

4. 一元運算子節點超過上限時錯誤位置是該運算子;呼叫節點超過上限時錯誤位置是呼叫的 `(`。

兩個上限都在建樹當下檢查,超過時不得堆疊溢位,也不得先建出過高的樹。

#### §6.3.4 錯誤恢復

parser 遇到語法錯誤時:

1. 回報該錯誤,並放棄目前所在的敘述:也就是最內層區塊(或程式最外層)中包含錯誤的那一個直接成員敘述。
2. 從目前的 token 開始(出錯的 token 尚未被消耗時就從它開始)往後跳過 token,同時計算跳過途中開啟的 `{` 數量 d(起始為 0):
   - `{`:d 加一。
   - `}` 且 d > 0:d 減一;減到 0 時消耗它並停止(跳過了一整個區塊,例如 `if (x +) { ... }` 的主體)。
   - `;` 且 d = 0:消耗它並停止。
   - `}` 且 d = 0:在區塊內時**不**消耗,讓該區塊用它結束;在程式最外層時消耗它並停止。
   - end token:停止。
   - 在 d = 0 處跳過的 `if`(或 `do`)也屬於被放棄的敘述,依步驟 3 視為一個在看到 `else`(或 `while`)之前就失敗的 `if`(或 `do`)。例:`while (x +) if (y) a; else b;` 只回報一個錯誤。
   - 括號:出錯當下未關閉的 `(`(分組、呼叫或條件的括號)個數 p,加上前一個被放棄的敘述留下的未關閉個數,在 d = 0 處跳過 `(` 時加一、跳過 `)` 且 p > 0 時減一;遇到 `{` 或 `}` 時歸零(括號不會跨越大括號)。接下來的敘述中若有完整解析的區塊,區塊內部的 p 從 0 起算,離開區塊後恢復為進入前的值(只有跳過大括號或完整解析一個敘述才會使 p 歸零)。例:`f(a; if (b) { c; } else );` 與 `f(a; if (b) c; else );` 都只回報一個錯誤。停止時 p 仍大於 0,表示被放棄的敘述還沒結束:接下來的敘述若在其自身括號之外的 `)` 出錯(例如 `f(a; b); c = 1;` 中 `b` 後面的 `)`),該錯誤不回報,那個 `)` 屬於被放棄的敘述,接著照步驟 2 跳過(p 隨之減一);步驟 3 的結尾也延到 p 歸零之後才處理。接下來的敘述若完整解析成功,p 與步驟 3 待處理的結尾一併歸零。例:`f(a; b); c = 1;` 與 `if (a; b) c; else d;` 都只回報一個錯誤。
3. 若被放棄的敘述中有 `if` 在看到它的 `else` 之前就失敗(條件或 then 分支出錯,或該 `if` 本身超過 §6.3.2 的深度上限),或有 `do` 的主體失敗,停止處接著的 `else` 或 `while ( ... ) ;` 屬於那個已放棄的敘述,而不是新的錯誤:
   - 每個這樣的 `if` 最多抵掉一個緊接著的 `else`:消耗 `else`,照常解析其後的敘述(其中真正的錯誤照常回報,失敗時依步驟 2 跳過)後丟棄。
   - 結尾的嵌套深度從被放棄的敘述所在的敘述串列起算(§6.3.2)。以下的 `if` 或 `do` 稱為**位於上限之外**:(a)步驟 2 在一個因 §6.3.2 而被拒絕的 `if`、`while` 或 `do`(即第 257 層由它開啟)之後跳過的 `if`/`do`,包含被拒絕的那個 `if`/`do` 本身;(b)本身就在第 256 層而被放棄的 `if`/`do`(它的結尾會開啟第 257 層);(c)在解析位於上限之外的結尾時被放棄的任何 `if`/`do`。位於上限之外的 `if`/`do` 的結尾也位於上限之外:解析它時(包含其中被放棄的敘述的恢復與結尾)嵌套深度照樣受限,但超過上限不再回報 `nesting too deep`,其他錯誤照常回報。這是對每一個結尾分別判斷的,所以即使串是在先前某個結尾中才第一次超過上限(例:`if (x +) a;` 之後接 2000 個 `else if (y) a;`),也只回報那一次。其他結尾照常回報深度錯誤:因括號、運算式或區塊超過上限而被放棄的外層 `if`,其 `else` 不在上限之外。所以任意長的 `else if` 串只回報一個 `nesting too deep`,而不相關的深度錯誤仍各回報一次。
   - 每個這樣的 `do` 最多抵掉一個緊接著的 `while`:消耗 `while`,解析 `( expression )` 後要求 `;`;缺少時回報 `expected ';' after 'do' statement`。
   - 步驟 2 跳過 token 時,若在 d = 0 處跳過了 `else`(或 `while`),該 `else`(或 `while`)就是一個這樣的 `if`(或 `do`)的結尾,已隨步驟 2 一起被跳過:那個 `if`(或 `do`)不再抵掉任何東西。例:`do a b while (x); while (y) z = 1;` 只回報一個錯誤,第二個 `while` 是新的敘述(其中真正的錯誤照常回報);`if (x) a b else c; else d;` 的第二個 `else` 回報 `'else' without a matching 'if'`。
   - 不是緊接著出現的 `else`/`while` 不會被抵掉;多出來的 `else` 仍回報 `'else' without a matching 'if'`。區塊內被放棄的 `if` 不會抵掉區塊外的 `else`。
   例:`if (x +) { a; } else { b; }` 只回報一個錯誤;`do x = ; while (y)` 回報 `expected expression` 與 `expected ';' after 'do' statement`。
4. 從停止的位置繼續解析下一個敘述。

為避免連鎖誤報:同一個位置(offset)不會回報兩個錯誤;跳過 token 的過程中不回報語法錯誤。

詞法錯誤(§2)會被回報,並**中止**解析:之前已回報的語法錯誤保留,之後不再回報任何錯誤。例外是位置在出錯 token 之前、但要讀到下一個 token 才能確定的錯誤:§6.3.3 的樹高檢查在運算元(以及它後面的那個 token)讀完後才進行,這時的 `nesting too deep` 仍照常回報,排在詞法錯誤之前。例:`x = 1 + 1 + ... + 1 @` 有 1024 個 `+` 時回報第 1024 個 `+` 的 `nesting too deep` 與 `@` 的詞法錯誤。回報的錯誤達到 20 個(`DIAGNOSTIC_MAX_ERRORS`)時也立即停止解析,不再回報更多錯誤。

錯誤依原始碼中的位置先後順序印出,每個錯誤一行(§6.1)。只要有任何錯誤,就不會產生 AST,`--dump-ast` 不輸出任何東西到標準輸出,結束碼為 1。

### §6.4 命令列

```text
lilc <source.lil> [-o <file>] [--dump-tokens | --dump-ast | --dump-ir | --dump-asm | --interp]
     [--ir-input] [--step-limit <n>] [-O0 | -O1 | --passes=<list>]
```

選項與原始檔可以任意順序出現。以 `-` 開頭且不只一個字元的參數是選項;其他參數(包含單獨的 `-`)是原始檔路徑。單獨的 `-` **不**代表標準輸入,而是名為 `-` 的檔案;要從管線讀取原始檔請用 `/dev/stdin`。

| 選項 | 行為 |
| --- | --- |
| (沒有 `--dump-*`) | 編譯成 ELF 執行檔,寫到 `-o` 指定的檔案(預設 `a.out`)。**尚未實作**:前端成功後回報 `generating an executable is not implemented yet`,結束碼 1,不產生檔案 |
| `-o <file>` | 輸出檔路徑;下一個參數一律視為檔名(即使以 `-` 開頭) |
| `--dump-tokens` | 只跑 lexer,依 §6.4.1 印出 token,不做語法分析 |
| `--dump-ast` | 跑完前端(含語意分析),依 §6.4.3 印出 AST;警告(§6.2)寫到標準錯誤,不影響結束碼 |
| `--dump-ir` | 跑完前端並降階到 IR(§8.5),驗證(§8.4)、依 `-O` 等級優化(§8.7)後依 §8.3 印出 IR;警告寫到標準錯誤 |
| `--interp` | 與 `--dump-ir` 相同地產生 IR,再以 IR 直譯器執行(§8.6):`putc` 寫到標準輸出,結束碼為 `r0 & 0xff`,除法 trap 時以 SIGFPE 終止(shell 狀態 136)。與 `--dump-*` 互斥(視為同一組模式) |
| `--ir-input` | 原始檔不是 LIL 而是 §8.3 的 IR 文字:解析、驗證後交給 `--dump-ir`、`--interp`(或回報「尚未實作」的 `--dump-asm` 與產生執行檔)。不得與 `--dump-tokens`、`--dump-ast` 併用 |
| `--step-limit <n>` | `--interp` 最多執行 n 個 IR 指令(十進位,0 表示不限制;預設 1000000000)。超過時已輸出的位元組保留,標準錯誤印出 `interpreter step limit of N exceeded`,結束碼 1。只能與 `--interp` 併用 |
| `--dump-asm` | **尚未實作**:前端成功後回報 `--dump-asm is not implemented yet`,結束碼 1 |
| `-O0`、`-O1` | 優化等級,預設 `-O0`;出現多次時以最後一個為準。對 `--dump-tokens`、`--dump-ast` 沒有影響;對 `--dump-ir`、`--interp` 決定是否執行 IR 優化(§8.7) |
| `--passes=LIST` | 測試與除錯用:以逗號分隔的 pass 名稱(§8.7 的 `loadfwd`、`copyprop`、`fold`、`simplify`、`branch`、`dce`、`thread`、`unreachable`、`merge`,可重複),取代 `-O0`/`-O1`(不論先後):依列出的順序一輪一輪執行到不再變化為止(§8.7 的驅動;相鄰的值 pass 合成一次掃描),但**不**做暫存器重新編號。出現多次時以最後一個為準。只能與 `--dump-ir` 或 `--interp` 併用;每個 pass 的「輸入 IR → 期望 IR」golden(`ir/opt_*.ir`)用它 |

同一個 `--dump-*` 重複出現等同出現一次。`--dump-*` 的輸出一律寫到標準輸出。原始檔有錯誤時,先依 §6.1 回報錯誤(結束碼 1),不會再回報「尚未實作」。

#### §6.4.1 `--dump-tokens` 格式

每個 token 一行,包含最後的 end token:

```text
<Kind> <line>:<column> <spelling>
```

`<Kind>` 是 token 種類名稱(`Identifier`、`Number`、`Character`、`If`、`Else`、`While`、`Do`、`Break`、`Continue`、`LeftParen`、`RightParen`、`LeftBrace`、`RightBrace`、`Semicolon`、`Comma`、`Assign`、`Plus`、`Minus`、`Star`、`Slash`、`Ampersand`、`Pipe`、`Tilde`、`LogicalAnd`、`LogicalOr`、`Less`、`LessEqual`、`Greater`、`GreaterEqual`、`EqualEqual`、`NotEqual`、`End`)。`<spelling>` 是 token 在原始檔中的原始位元組,其中 0x20–0x7E 以外的位元組寫成 `\xHH`(兩位小寫十六進位),所以輸出永遠是 ASCII 文字;因為 `\x` 不是合法的跳脫序列(§2.6),這個寫法不會和原始拼法混淆。end token 的拼法是空的,該行寫成 `End <line>:<column>`,結尾沒有空白。

遇到詞法錯誤時,錯誤之前的 token 照常印到標準輸出,接著依 §6.1 回報錯誤,結束碼 1;不合法的 token 本身不印出。

#### §6.4.2 命令列錯誤

命令列錯誤不帶位置,結束碼為 1。下表中標示「+ 用法」的錯誤在訊息之後另印一行用法。用法行為 `usage: NAME <source.lil> [-o <file>] [--dump-tokens|--dump-ast|--dump-ir|--dump-asm|--interp] [--ir-input] [--step-limit <n>] [-O0|-O1|--passes=<list>]`,NAME 是 `argv[0]` 最後一個 `/` 之後的部分(與從哪個目錄執行無關)。

| 情形 | 訊息 |
| --- | --- |
| 沒有參數或沒有原始檔 | 只印用法行 |
| 不認得的選項 | `unknown option: ARG` + 用法 |
| `-o` 是最後一個參數 | `missing file name after -o` + 用法 |
| `-o` 出現兩次以上 | `-o given more than once` + 用法 |
| 兩個以上的原始檔 | `more than one source file: PATH`(PATH 是第二個原始檔)+ 用法 |
| 兩種不同的 `--dump-*`(含 `--interp`) | `conflicting options: A and B` + 用法 |
| `-o` 與 `--dump-*`(含 `--interp`)同時使用 | `-o cannot be combined with A` + 用法 |
| `--ir-input` 與 `--dump-tokens` 或 `--dump-ast` 同時使用 | `--ir-input cannot be combined with A` + 用法 |
| `--step-limit` 是最後一個參數 | `missing number after --step-limit` + 用法 |
| `--step-limit` 之後不是十進位數字串(或超出 64 位元無號整數) | `invalid step limit: ARG` + 用法 |
| 沒有 `--interp` 卻給了 `--step-limit` | `--step-limit requires --interp` + 用法 |
| `--passes=` 清單中有不認得的名稱(含空清單、多餘的逗號) | `unknown pass: 'NAME'` + 用法 |
| `--passes=` 清單超過 64 項 | `too many passes (the limit is 64)` + 用法 |
| 沒有 `--dump-ir` 或 `--interp` 卻給了 `--passes` | `--passes requires --dump-ir or --interp` + 用法 |
| 無法開啟或讀取原始檔(含目錄) | `failed to read file: PATH` |
| 無法寫入標準輸出(例如 `--dump-tokens` 導向 `/dev/full`) | `failed to write output` |

命令列檢查在讀檔之前完成。原始檔一律讀到 EOF 為止,不先查詢檔案大小,所以管線與 `/dev/stdin` 也可以當原始檔;目錄會在讀取時失敗,依上表回報。`--dump-*` 的輸出在結束前一律 flush 並檢查寫入錯誤;寫入失敗時回報上表的 `failed to write output`,結束碼 1,不會以 0 結束而留下不完整的輸出。

#### §6.4.3 `--dump-ast` 格式

AST 以 S 運算式印出,每個節點一行,子節點縮排兩個空白,節點的右括號接在最後一個子節點那一行的結尾。每個節點都帶有 span,寫成 `@<line>:<column>+<length>`:起點的行、列(§2.1)與長度(位元組數)。

```text
(program @1:1+N STATEMENT...)
```

`program` 的 span 是整份原始檔(offset 0,長度為檔案長度)。敘述節點:

| 節點 | 寫法 | span |
| --- | --- | --- |
| 運算式敘述 | `(expr @S EXPRESSION)` | 運算式開頭到 `;` |
| 空敘述 | `(empty @S)` | `;` |
| 區塊 | `(block @S STATEMENT...)` | `{` 到 `}` |
| if | `(if @S CONDITION THEN)` 或 `(if @S CONDITION THEN ELSE)` | `if` 到最後一個分支的結尾 |
| while | `(while @S CONDITION BODY)` | `while` 到主體結尾 |
| do | `(do @S BODY CONDITION)` | `do` 到結尾的 `;` |
| break / continue | `(break @S)`、`(continue @S)` | 關鍵字到 `;` |

運算式節點:

| 節點 | 寫法 | 說明 |
| --- | --- | --- |
| 識別字 | `(name SPELLING #ID @S)` | 原始拼法;ID 為符號編號(§5.1) |
| 整數常值 | `(int VALUE @S)` | 十進位的值,例如 `010` 印成 `(int 8 ...)` |
| 字元常值 | `(char VALUE @S)` | 位元組值,例如 `'a'` 印成 `(char 97 ...)` |
| 一元運算 | `(unary OP @S OPERAND)` | OP 為 `-` 或 `~` |
| 二元運算 | `(binary OP @S LEFT RIGHT)` | OP 為運算子原始拼法,含 `&&`、`\|\|` |
| 賦值 | `(assign NAME #ID @S VALUE)` | NAME 為左側識別字的原始拼法;ID 為其符號編號 |
| 呼叫 | `(call NAME @S ARGUMENT...)` | 引數依序列出 |

運算式節點的 span 從第一個 token 到最後一個 token;括號運算式的 span 包含兩個括號(所以 `(a + b) * c` 的 `binary *` 從 `(` 開始)。例:`x = 1;` 印成

```text
(program @1:1+7
  (expr @1:1+6
    (assign x #0 @1:1+5
      (int 1 @1:5+1))))
```

(檔案以換行結尾,所以 program 的長度是 7。)這個程式同時在標準錯誤印出警告 `variable 'x' is assigned but never read`。

## §7 測試與規格的對應

- 單元測試:`tests/test_lil.c`(詞法、語法、往返),`tests/test_sema.c`(符號表與語意分析,§3.2、§3.7、§4.10、§5、§6.2),框架為 `tests/test_framework.h`。`tests/test_lil.c` 透過只做詞法與語法分析的 `lil_run_source()` 測試 parser;`tests/test_sema.c` 透過完整前端 `lil_analyze_source()`。
- Golden 測試:`tests/golden/<kind>/NAME.lil` 搭配 `.stdout`、`.stderr`、`.status`,可選 `.args`;`cli/` 的 fixture 可用 `.cmdline` 取代整個參數列(用於沒有參數、檔案不存在等 CLI 錯誤);`run/` 的 fixture 另有 `.compile-stderr` 比對編譯器的警告。`<kind>` 為 `tokens`、`ast`、`ir`、`interp`、`asm`、`run`、`cli`;fixture 也可以是 IR 文字 `NAME.ir`(§8.3),執行時另加 `--ir-input`;執行方式見 `tests/test_cli.sh` 開頭的說明。`make update-golden` 重新產生期望檔,有任何 fixture 無法更新時以非零結束碼結束。
- 每個新增的錯誤訊息至少要有一個負面 golden,比對訊息、行號、列號與結束碼。
- 詞法屬性測試(`lexer_property_spans_partition_random_input`):以固定種子的 PRNG 產生輸入,檢查 token span 依序、不重疊、不越界,行列與 offset 一致,token 之間只有空白與註解(§2.1–§2.3)。
- `tests/check_examples.sh`:`examples/` 中每個檔案都必須能被 `--dump-tokens` 與 `--dump-ast` 處理(`make test`、`make sanitize` 都會執行);`--dump-ast` 可以有警告,但不得有錯誤。
- 優先序:`parser_binary_operator_pairs_follow_precedence_table` 以獨立於 `src/operator.c` 的表(照 §4.2 抄寫)產生所有 15×15 個 `a OP1 b OP2 c` 組合並比對樹形;`parser_unary_operators_bind_tighter_than_binary` 檢查一元運算子與每個二元運算子的組合。
- 往返(round trip):`ast_print_round_trips_goldens_and_examples` 對所有能解析的 golden fixture 與 `examples/`,`ast_print_round_trips_random_programs` 對固定種子的隨機程式產生器,檢查「AST → `ast_print_program()` → 再解析」得到相同的 AST(不比 span),而且再印一次文字完全相同。`parser_random_parenthesized_expressions_parse_exactly` 以完全加括號的隨機運算式檢查樹形。fuzz harness 對每個被接受的輸入也檢查同樣的往返性質,並檢查錯誤數量為 1–20 且位置嚴格遞增(§6.3.4)。
- 深度防護:`parser_rejects_10000_levels_of_nesting` 對每一種嵌套結構(括號、區塊、一元運算子、呼叫、`if`、`while`、`do`)各建 10000 層,只得到一個 `nesting too deep`;`make sanitize` 會在 ASan + UBSan 下執行同一組測試與 `cli/deep_parentheses`、`cli/deep_blocks`。

- 語意分析:fuzz harness 對每個輸入另跑完整前端,檢查語意錯誤只在程式能解析時出現、錯誤與警告各自位置嚴格遞增且不超過 20 個,並對每個通過的程式呼叫 `sema_verify()`(§5.8)。`sema_verify_detects_broken_invariants` 檢查 `sema_verify()` 本身會抓到被破壞的不變量。

- IR(§8):`tests/test_ir.c` 涵蓋 opcode 表、降階的形狀(`&&`、`||`、迴圈、`break`/`continue`、結束碼)、文字格式的解析與每一種解析錯誤、驗證器每一條規則的負面測試(以手寫 IR 文字建立;只能經由 API 違反的規則另以 API 建立)、直譯器語意(環繞、除法 trap、短路、`putc` 的值、步數上限,以及在 fork 出的子行程中確認 trap 以 SIGFPE 終止)。`ir_round_trips_goldens_and_examples` 對所有通過前端的 golden 與 `examples/` 檢查「降階 → 驗證 → dump → 解析 → dump」完全相同。golden 目錄 `ir/`(`--dump-ir`;`NAME.ir` 以 `--ir-input` 執行,含每一種 IR 解析錯誤 `parse_*` 與驗證器錯誤 `verify_*`)與 `interp/`(`--interp`,比對程式輸出、警告與結束狀態)。fuzz harness 對每個通過語意分析的程式降階、驗證、檢查 dump 往返,並以 10000 步的上限執行直譯器。第二個 fuzz 目標 `fuzz/fuzz_ir.c` 直接餵任意位元組給 IR 文字解析器(`--ir-input` 的路徑):解析失敗必須是 `ir` 診斷、驗證失敗必須是 `verify` 診斷;通過驗證的程式必須 dump/解析/dump 完全相同、往返前後以 2000 步上限執行的結果相同,且 `-O1` 之後仍通過驗證、在直譯器上與 `-O1` 之前行為相同(見下面的優化項),而且再跑一次 `-O1` 不變。種子是所有 `.ir` golden 與每個能編譯的範例/golden 程式的 `--dump-ir`。`make fuzz-run` 依序跑兩個目標,`FUZZ_TARGET=frontend` 或 `ir` 只跑一個。驗證器規則 4 另有效能測試(`ir_verify_is_fast_on_long_chains_and_ladders`:200000 個區塊的鏈、100000 個菱形的梯子,各須在 2 秒內)與多批位元集合的測試(`ir_verify_handles_many_tracked_vregs`)。
- IR 優化(§8.7):每個 pass 的「輸入 IR → 期望 IR」golden 是 `ir/opt_PASS_*.ir`(`.args` 為 `--passes=PASS`),包含「不該改寫」的反例(`x/0`、`INT64_MIN / -1` 不摺疊也不刪、結果沒用到的 trap 除法保留、`x/x`、`0/x`、`x/-1` 不化簡、不進入有兩條入邊的區塊、走訪中死掉的區塊不把事實傳出去(`ir/opt_branch_dead_self_loop`)、重新定義後不傳播、跳進非空區塊不 thread、空區塊循環會停、進入區塊不被合併);`ir/opt_o1_*.lil` 與 `interp/opt_o1_*.lil` 從 LIL 原始碼跑完整的 `-O1`。`make test` 另以 `sh tests/test_cli.sh --interp-flag -O1` 把每個 `interp/` fixture(含所有驗收程式)以 `-O1` 再跑一次,比對**同一份**期望檔(`.args` 已指定 `-O`、`--passes` 或 `--step-limit` 的除外)。`tests/test_ir_opt.c`:每個 pass 單獨執行後跑驗證器、再跑一次不再變化(冪等)、在直譯器上行為相同;所有 pass 跑到不動點一定停(`reached_fixpoint`),結果通過驗證、指令數不增加、再跑 `-O1` 不變;對象是所有 golden 與範例、3000 個隨機 IR 程式(固定種子,含非 SSA 的重複定義、除以 0、空區塊循環、無窮迴圈)、1500 組隨機的 pass 順序與子集,以及對抗性 IR(100000 個區塊的跳轉鏈正反兩種順序、50000 個空區塊的循環、2000 個菱形的常數梯子須在 4 輪內完成、50000 個 copy 的鏈、500 與 2000 層的 LIL 常數 `if` 梯子須在 2 輪內完成且 `-O1` 冪等、死碼中 70000 個區塊的反序依賴鏈、`--passes` 用完 1000 輪時的警告)。`fold_matches_the_interpreter` 對每個運算與邊界值(含 `INT64_MAX + 1`)比對摺疊結果、`ir_eval_*` 與直譯器執行未摺疊程式印出的 8 個位元組。「行為相同」的定義:未優化的程式在步數上限內結束時,優化後以相同方式(結束碼或 trap)結束、輸出相同、步數不多於它;未優化的程式用完步數時,它的輸出是優化後輸出的前綴。fuzz 的兩個目標都以同樣的規則比對 `-O0` 與 `-O1`(`fuzz/fuzz_opt.h`)。
- 差分測試(`make difftest`,`tests/tools/`):`gen_lil.py` 以固定種子產生保證會結束的隨機 LIL 程式(含常數子運算式、`if (1)`/`if (0)`/`while (0)`/`do … while (0)`、`x*1`、`x+0`、`x-x` 等恆等式、存後讀、`break`/`continue` 之後的死碼、可能 trap 的除法);`diff_test.py` 對每個程式要求獨立的 oracle(`lil_oracle.py`,只依本規格撰寫)、`--interp -O0`、`--interp -O1` 的標準輸出與狀態完全相同,`-O0` 與 `-O1` 的 dump 經 `--ir-input` 以同等級再處理一次完全不變(往返與 `-O1` 冪等),而且 `-O1` 的指令數不多於 `-O0`。`make difftest` 預設 300 個程式(`DIFFTEST_COUNT`、`DIFFTEST_SEED` 可調),`make test` 跑 60 個。
- 驗收程式(M4 閘門):`examples/accept/*.lil`(`max`、`sum100`、`fact`、`fib`、`gcd`、`primes`、`collatz`、`shortcircuit`、`divzero`、`pressure`),期望結果以獨立計算手動確認,放在 `tests/golden/interp/accept_NAME.*`;`examples/NAME.lil` 對應 `interp/example_NAME.*`。`tests/check_examples.sh` 要求這些 golden 的 `.lil` 與範例逐位元組相同,並檢查每個範例都能 `--dump-ir`;它們在 `-O1` 下也由 `--interp-flag -O1` 比對同一份期望結果。

### §7.1 文法產式與測試對應

附錄 A 的每一條語法產式至少有一個正面與一個負面測試。golden 路徑相對於 `tests/golden/`,單元測試在 `tests/test_lil.c`。

| 產式 | 正面測試 | 負面測試 |
| --- | --- | --- |
| `program` | `ast/multiple_expressions`、`ast/empty_program`、`run_source_accepts_empty_program` | `cli/unexpected_right_brace` |
| `statement` | `ast/keywords_any_case`、`parser_parses_statements` | `cli/else_without_if`、`cli/if_missing_body`、`cli/branch_missing_statement` |
| `expression-statement` | `ast/multiple_expressions` | `cli/missing_semicolon` |
| `empty-statement` | `ast/empty_statement` | `cli/if_missing_body`(主體連空敘述都沒有) |
| `block` | `ast/empty_block`、`ast/nested_loops` | `cli/unclosed_block`、`cli/deep_blocks` |
| `if-statement` | `ast/dangling_else` | `cli/if_missing_open`、`cli/if_missing_close` |
| `while-statement` | `ast/nested_loops` | `cli/while_missing_open`、`cli/while_missing_close` |
| `do-statement` | `ast/nested_loops`、`ast/empty_statement` | `cli/do_missing_while`、`cli/do_missing_semicolon` |
| `break-statement` | `ast/nested_loops`、`ast/keywords_any_case` | `cli/break_missing_semicolon` |
| `continue-statement` | `ast/nested_loops`、`ast/keywords_any_case` | `cli/continue_missing_semicolon` |
| `expression` / `assignment` | `ast/precedence`(`a = b = c`) | `cli/invalid_assignment_target`、`cli/invalid_assignment_parenthesized`、`cli/assignment_nesting_too_deep` |
| `logical-or` | `ast/precedence` | `cli/missing_operands`(`a \|\| ;`) |
| `logical-and` | `ast/precedence` | `cli/missing_operands`(`a && ;`) |
| `bitwise-or` | `ast/precedence` | `cli/missing_operands`(`a \| ;`) |
| `bitwise-and` | `ast/precedence` | `cli/missing_operands`(`a & ;`) |
| `equality` | `ast/precedence` | `cli/missing_operands`(`a == ;`) |
| `relational` | `ast/precedence` | `cli/missing_operands`(`a < ;`) |
| `additive` | `ast/precedence`、`ast/multiple_expressions` | `cli/syntax_error`、`cli/missing_operands`、`cli/expression_too_deep` |
| `multiplicative` | `ast/precedence` | `cli/missing_operands`(`a * ;`) |
| `unary` | `ast/precedence`(`-a * b`、`~-y`) | `cli/missing_operands`(`- ;`、`~ ;`) |
| `primary` | `ast/literal_values`、`ast/parenthesized_spans` | `cli/paren_missing_close`、`cli/deep_parentheses` |
| `call` | `ast/calls` | `cli/call_missing_close` |
| `argument-list` | `parser_parses_calls`、`cli/sema_unknown_function`(`g(1, x = 2, -y)` 通過語法分析,才在語意分析被拒絕) | `cli/call_trailing_comma` |

錯誤恢復(§6.3.4)另有 `cli/three_errors`(恰好三個錯誤)、`cli/recovery_skips_block`、`cli/recovery_if_else`(錯誤後的 `else` 不連鎖誤報)、`cli/recovery_do_tail`、`cli/recovery_skipped_tail`(步驟 2 已跳過的結尾不再抵掉後面的 `while`/`else`)、`cli/else_if_chain_too_deep`(300 個 `else if` 只有一個錯誤)、`cli/else_if_chain_600`(超過上限兩倍的串仍只有一個錯誤)、`cli/recovery_skipped_if`(步驟 2 跳過的 `if`/`do` 其結尾不誤報)、`cli/recovery_unclosed_paren`(未關閉括號的 `)` 不誤報)、`cli/recovery_do_tail_condition`、`cli/height_then_lexical_error`、`cli/too_many_errors`(25 個錯誤只回報 20 個)、`cli/syntax_then_lexical_error`,以及單元測試 `parser_recovers_and_reports_independent_errors`、`parser_recovery_does_not_cascade`、`parser_recovery_skips_abandoned_tails`、`parser_recovery_skipped_tail_is_not_skipped_again`、`parser_too_deep_else_if_chain_reports_once`(含 600、2000、100000 個 `else if`,以及兩種加區塊的寫法)、`parser_recovery_counts_skipped_ifs_and_dos`、`parser_recovery_matches_unclosed_parentheses`、`parser_recovery_reports_errors_in_do_tail_condition`、`parser_reports_height_error_before_lexical_error`、`parser_caps_error_count`、`parser_stops_at_lexical_error`。CRLF 換行的 span 由 `ast/crlf_spans` 檢查。

### §7.2 語意規則與測試對應

golden 路徑相對於 `tests/golden/`,單元測試在 `tests/test_sema.c`。

| 規則 | 正面測試 | 負面測試 |
| --- | --- | --- |
| §3.2 無效果警告 | `ast/sema_no_effect`、`sema_effects_suppress_the_no_effect_warning` | `ast/sema_no_effect`、`sema_warns_about_statements_without_effect` |
| §3.7 `break`/`continue` 位置 | `ast/sema_break_nested`、`sema_accepts_break_and_continue_inside_loops` | `cli/sema_break_outside_loop`、`cli/sema_continue_outside_loop`、`sema_rejects_break_and_continue_outside_loops` |
| §4.10 `putc` 引數數量 | `ast/calls`、`sema_marks_putc_as_builtin` | `cli/sema_putc_no_arguments`、`cli/sema_putc_two_arguments`、`sema_checks_putc_argument_count` |
| §4.10 未知函式 | `ast/calls` | `cli/sema_unknown_function`、`sema_rejects_unknown_functions` |
| §5.1、§5.2 符號表與大小寫 | `ast/sema_case_insensitive`、`sema_resolves_names_case_insensitively`、`sema_keywords_in_any_case_are_not_variables`、`symbol_table_*` | — |
| §5.4 `putc` 當變數 | `ast/calls` | `cli/sema_assign_to_putc`、`cli/sema_putc_as_value`、`sema_rejects_putc_as_variable` |
| §6.2 從未讀取的變數 | `ast/sema_assigned_never_read`(`r0` 不報) | `ast/sema_assigned_never_read`、`sema_warns_about_variables_never_read` |
| §6.1、§6.2 合併輸出與上限 | `cli/sema_errors_and_warnings`、`result_prints_errors_and_warnings_merged` | `sema_caps_error_count`、`sema_caps_warning_count` |

## §8 中間表示(IR)

IR 是前端(§2–§5)與 backend 之間唯一的介面:降階(§8.5)產生 IR,優化(§8.7)改寫 IR,直譯器(§8.6)定義 IR 的執行結果,backend(階段 5)把 IR 翻成機器碼。資料結構與 API 在 `include/ir.h`。

### §8.1 結構

- 程式由**全域變數**清單與**基本區塊**清單組成。第一個區塊是進入點(entry)。
- 全域變數 `@name` 編號 0..N−1,與符號表(§5.1)的編號、名稱(大小寫折疊後)一一對應;初值 0,只能經由 `load`/`store` 讀寫。
- **虛擬暫存器** `%0`、`%1`、… 數量不限,**不是** SSA:同一個暫存器可以被定義多次(降階只對 `&&`、`||` 的結果這麼做,在每條進入合流區塊的路徑上各定義一次)。暫存器沒有初值;每次使用前都必須已被定義(§8.4 規則 4)。暫存器可以跨區塊使用:除了 `&&`/`||` 的結果,在短路運算之前求出的運算元也會在合流區塊裡使用(例如 `r0 = x + (a && b);` 的 `%0 = load @x` 在進入區塊定義、在 `and.end.1` 使用)。
- 暫存器編號可以不連續,但 `vreg_count` 是「最大編號 + 1」,直譯器與驗證器都依 `vreg_count` 配置每個暫存器的空間(直譯器每個 8 位元組,驗證器約 8 位元組多一點),所以稀疏的大編號(例如只用 `%16777215`)也會配置整個範圍。文字格式的編號上限是 16777215(§8.3),所以 `--ir-input` 最多約配置 128 MiB 給直譯器、130 MiB 給驗證器;`-O1` 在優化前後各把暫存器依原順序重新編號為 `%0`..`%(n−1)`(`ir_compact_vregs()`),所以優化時配置的空間與實際用到的暫存器數成正比。
- 每個區塊是一串三位址指令,**最後一個指令是唯一的終結指令**(`jmp`、`br`、`exit`)。控制流圖由終結指令的目標給出。區塊名稱(標籤)在程式中唯一。

### §8.2 指令與語意

所有值都是 64 位元二補數整數。

| 指令 | 語意 |
| --- | --- |
| `%d = const IMM` | `d = IMM` |
| `%d = copy %a` | `d = a` |
| `%d = add/sub/mul %a, %b` | 加、減、乘,取模 2^64 環繞(§4.5) |
| `%d = div %a, %b` | 向零截斷(§4.6);`b = 0` 或 `a = INT64_MIN` 且 `b = -1` 時 **trap**(原生程式為 SIGFPE,§5.6) |
| `%d = and/or %a, %b` | 逐位元 AND、OR(§4.7) |
| `%d = neg %a`、`%d = not %a` | `-a`(環繞)、`~a` |
| `%d = eq/ne/lt/le/gt/ge %a, %b` | 有號比較,結果 1 或 0(§4.8) |
| `%d = load @g`、`store @g, %a` | 讀、寫全域變數 |
| `putc %a` | 把位元組 `a & 0xff` 寫到標準輸出(沒有結果值;LIL 的 `putc(x)` 的值就是 `x` 本身,§4.10) |
| `jmp L` | 跳到區塊 L |
| `br %c, L1, L2` | `c ≠ 0` 時跳到 L1,否則 L2 |
| `exit %a` | 以 `a & 0xff` 結束程式(§5.5) |

`div` 是唯一可能 trap 的指令;只有在能證明不會 trap 時才可以摺疊或刪除(§5.7)。運算語意只定義在一個地方:`ir_eval_binary()`/`ir_eval_unary()`(`include/ir_interp.h`),直譯器與常數摺疊共用,以免兩者不一致;實作以無號整數運算,編譯器本身沒有 signed overflow。

### §8.3 文字格式

`--dump-ir` 印出、`--ir-input` 讀入的格式。一行一個項目:

```ebnf
ir-file     = { line } ;
line        = [ global-decl | label | instruction ], [ comment ], NEWLINE ;
global-decl = "global", global ;
label       = name, ":" ;
instruction = vreg, "=", "const", integer
            | vreg, "=", ( "copy" | "neg" | "not" ), vreg
            | vreg, "=", binary-op, vreg, ",", vreg
            | vreg, "=", "load", global
            | "store", global, ",", vreg
            | ( "putc" | "exit" ), vreg
            | "jmp", name
            | "br", vreg, ",", name, ",", name ;
binary-op   = "add" | "sub" | "mul" | "div" | "and" | "or"
            | "eq" | "ne" | "lt" | "le" | "gt" | "ge" ;
name        = name-start, { name-start | digit } ;
name-start  = letter | "_" | "." ;
global      = "@", name ;
vreg        = "%", digit, { digit } ;          (* 0..16777215 *)
integer     = [ "-" ], digit, { digit } ;      (* 十進位,int64 範圍內 *)
comment     = ";", { 任意位元組 - LF } ;
```

- 空白(空格、tab、CR)可出現在任何 token 之間;空行與註解被忽略;最後一行不需要換行。名稱區分大小寫。整數一律是十進位(`007` = 7,沒有八進位)。
- 全域宣告必須在第一個標籤之前。`name` 後面緊接 `:` 一律是標籤(所以標籤也可以叫 `exit`)。指令屬於最近的標籤。
- 標籤可以在定義之前被引用。引用不存在的標籤或未宣告的全域變數**不是**解析錯誤,而是由驗證器回報(§8.4 規則 2)。
- `--dump-ir` 的輸出是**正規形式**:每個全域一行 `global @name`(依編號),接著每個區塊前一個空行(第一個區塊之前若沒有全域則沒有),標籤一行 `name:`,指令縮排兩個空白,運算元以 `, ` 分隔。對正規形式「解析再印出」得到完全相同的文字(往返性質)。

解析在第一個錯誤就停止,以 `ir` 階段回報(§6.1),結束碼 1:

| 情形 | 訊息 |
| --- | --- |
| 不能開始任何 token 的位元組 | `unexpected character 'C'`(不可列印時寫成 `\xHH`,同 §2.8) |
| `@` 後面不是名稱 | `expected a name after '@'` |
| `%` 後面不是數字 | `expected a number after '%'` |
| 暫存器編號大於 16777215 | `virtual register number too large (the limit is 16777215)` |
| `-` 後面不是數字 | `expected a digit after '-'` |
| 整數超出 int64 | `integer out of range` |
| 應為暫存器、`,`、全域、區塊名稱、整數、`=`、指令名稱之處不是 | `expected a virtual register`、`expected ','`、`expected a global (@name)`、`expected a block name`、`expected an integer`、`expected '='`、`expected an instruction name` |
| 不認得的指令 | `unknown instruction 'NAME'` |
| 產生值的指令沒有目的暫存器 | `'OP' needs a destination register ('%N = OP ...')` |
| `store`、`putc`、終結指令前面有 `%N =` | `'OP' does not produce a value` |
| 第一個標籤之前的指令 | `instruction outside of a block` |
| 第一個標籤之後的全域宣告 | `global declaration after the first block` |
| 重複的全域、標籤 | `duplicate global '@NAME'`、`duplicate block 'NAME'` |
| 行首不是標籤、全域宣告或指令 | `expected an instruction, a label or a global declaration` |
| 項目之後還有東西 | `expected end of line` |

例:`r0 = a && b;` 的 `--dump-ir`:

```text
global @r0
global @a
global @b

entry:
  %0 = load @a
  %1 = const 0
  br %0, and.rhs.1, and.end.1

and.rhs.1:
  %2 = load @b
  %3 = const 0
  %1 = ne %2, %3
  jmp and.end.1

and.end.1:
  store @r0, %1
  %4 = load @r0
  exit %4
```

### §8.4 驗證器

`ir_verify()`(`include/ir_verify.h`)依下列順序檢查,回報第一個違規(`verify` 階段,位置是出錯的指令或區塊標籤):

1. 程式至少有一個區塊(`program has no blocks`);區塊名稱唯一(`duplicate block 'NAME'`)。
2. opcode 合法(`invalid opcode N`);指令需要的暫存器都存在且小於暫存器總數(`'OP' is missing a virtual register`、`virtual register %N is out of range (the program has M)`);`load`/`store` 的全域存在(`'OP' refers to a global that does not exist`);跳轉目標存在(`'OP' jumps to a block that does not exist`)。
3. 每個區塊恰好以一個終結指令結尾:最後一個指令是終結指令(`block 'NAME' does not end with a terminator`,空區塊亦同),其他指令都不是(`terminator 'OP' is not the last instruction of block 'NAME'`)。
4. **確定定義**:在每個**從進入區塊可以到達**的區塊中,每次使用暫存器時,從進入區塊到這次使用的每一條路徑上,之前都有該暫存器的定義(`virtual register %N may be used before it is defined`;有多個違規時回報程式順序中的第一個)。進入區塊開始執行時沒有任何暫存器被定義,即使它有前驅(迴圈回到進入區塊)。**到不了的區塊**(沒有前驅的孤兒區塊、只有彼此互連的循環、只被到不了的區塊跳到的區塊)永遠不會執行,**不檢查**規則 4,也不算進合流:到不了的前驅不會讓使用變成合法或不合法。規則 1–3 仍然檢查所有區塊。因此把常數條件的 `br` 改成 `jmp` 之後留下的孤兒區塊是合法的 IR(例如 `r0 = 7 + (0 && b);` 摺疊後的 `and.rhs.1`)。backend 與後續 pass 必須能容忍或略過到不了的區塊:它們的暫存器可能沒有定義,不能假設任何事。
   實作分兩層。只有需要的暫存器進入檢查:在同一區塊先定義後使用的不需要;進入區塊裡的向上暴露使用一定是錯誤;在進入區塊定義的暫存器在所有其他可到達區塊都已定義。其餘的(在進入區塊以外有向上暴露的使用、而且不在進入區塊定義)是「追蹤的」暫存器。
   - **稀疏檢查**(先做):暫存器 v 在區塊 b 開頭不是確定已定義,正好是「有一條從進入區塊到 b 的某個可到達前驅的路徑,完全不經過定義 v 的區塊」(進入區塊從不定義追蹤的暫存器)。所以對每個 v,從它每個使用所在區塊的前驅往回搜尋,遇到定義 v 的區塊就停;走到進入區塊就表示這個使用有錯。v 的使用依程式順序處理並共用同一個已走訪集合(已走訪區塊後面的部分都搜過了),所以每個 v 的成本約是它的定義與使用之間那一段控制流圖的大小。降階產生的跨區塊值都在它跨過的短路運算或 `if` 之後馬上使用,這一段很小,所以實際上是線性時間。
   - **位元集合資料流**(後備):稀疏檢查的總步數有上限(約 16 ×(可到達區塊數 + 邊數)),超過時(例如上萬個暫存器都在開頭定義、在很遠的地方使用)改用標準的前向「確定已定義」資料流:只看可到達的區塊,`IN[entry] = ∅`、`IN[b] = ∩ OUT[p]`(p 為可到達的前驅)、`OUT[b] = IN[b] ∪ b 定義的暫存器`,從「全部已定義」開始,依反向後序以工作串列迭代到不動點。追蹤的暫存器重新編成緊湊的編號,所以位元集合大小與暫存器編號的稀疏程度無關;位元集合總共最多 64 MiB,超過時把暫存器分批各跑一次資料流,每批的成本是「可到達區塊數 × 批的字數」,所以這條路徑在最壞情況下是「區塊數 × 追蹤的暫存器數 / 64」。
   兩層回報同一個違規(程式順序中的第一個)。實測(未開優化的建置,只計驗證器):每個菱形用自己的條件暫存器(下一個菱形開頭再用一次)的 100000 與 200000 個菱形梯子 0.4 與 0.8 秒,`x = a && b;` 重複 200000 行的 -O0 降階結果(400000 個區塊、200000 個追蹤的暫存器)0.5 秒;只有位元集合資料流時這三者是 2.7、17 與約 18 秒,因為批數隨區塊數成長(單元測試 `ir_verify_is_fast_with_a_vreg_per_diamond` 要求各在 5 秒內,為負載高的機器與 sanitizer 留餘裕,仍遠低於二次方的 17 秒)。走後備路徑的 30000 個在同一區塊定義、沿 30000 個區塊的鏈依序使用的暫存器 0.1 秒(`ir_verify_handles_many_tracked_vregs` 同時以 50 個暫存器測稀疏路徑,兩者回報同一個違規)。

`--dump-ir` 與 `--interp` 在降階後、優化後都會執行驗證器;降階的結果被拒絕是編譯器內部錯誤(`internal error: IR verification failed after lowering: ...`)。`--ir-input` 的 IR 被拒絕則是一般錯誤(結束碼 1)。

### §8.5 降階

前提:語意分析成功,`sema_verify()` 成立(§5.8);降階不再檢查使用者錯誤。

- 每個符號成為同編號的全域。變數讀取是 `load`,常值是 `const`,一元 `-`、`~` 是 `neg`、`not`,二元運算子先完整求左運算元再求右運算元(§4.3),然後一個對應的指令。
- 賦值先求右邊,再 `store`;運算式的值就是右邊的暫存器(§4.4)。`putc(x)` 先求 `x`,再 `putc`;值就是 `x` 的暫存器(§4.10)。
- `a && b`:求 `a`,結果暫存器設為 0,`br a, and.rhs.N, and.end.N`;`and.rhs.N` 求 `b`,把結果設為 `b ≠ 0`,跳到 `and.end.N`。`a || b` 相同,但結果先設為 1、`br` 的兩個目標對調(`or.rhs.N`、`or.end.N`)。短路語意(§4.9)只在這裡處理。
- `if`:條件後 `br` 到 `if.then.N` 與 `if.else.N`(沒有 `else` 時直接到 `if.end.N`),兩個分支結尾都 `jmp if.end.N`。
- `while`:`jmp while.cond.N`;條件區塊 `br` 到 `while.body.N` 或 `while.end.N`;主體結尾跳回條件。`do`:`jmp do.body.N`;主體結尾跳到 `do.cond.N`;條件 `br` 回主體或到 `do.end.N`。
- `break` 跳到最內層迴圈的 end 區塊,`continue` 跳到它的 cond 區塊(`do` 迴圈也是,§3.7);之後同一串敘述的程式碼放進新的、無法到達的 `dead.N` 區塊。
- 程式結尾:`load @r0`(沒有 `r0` 時為 `const 0`)後 `exit`(§5.5)。
- 標籤的 N 是每個建構(每個 `if`、迴圈、`&&`、`||`、`break`/`continue`)依原始碼順序遞增的編號;區塊依**開始產生指令的順序**排列,所以輸出由上往下對應原始碼。暫存器依產生順序編號。

### §8.6 直譯器

`ir_interp_run()`(`include/ir_interp.h`)從進入區塊開始執行,所有全域與暫存器初值 0。它是後續所有階段的「正確答案」:`-O0`、`-O1`、backend 與自製組譯器的可觀察行為都必須與它相同(§5.7)。

- `putc` 寫一個位元組到標準輸出,寫入錯誤被忽略。
- `exit %a`:以 `a & 0xff` 結束。
- `div` trap:先 flush 標準輸出,把 SIGFPE 的處理方式恢復為預設並解除封鎖,再 `raise(SIGFPE)`,所以行程就像原生程式一樣被 SIGFPE 終止(shell 狀態 136)。直譯器本身以無號運算實作,沒有未定義行為。
- **步數上限**:每執行一個指令(含終結指令)算一步;執行了上限數量的指令後還沒結束就停止。CLI 預設上限 1000000000,可用 `--step-limit` 調整(§6.4);單元測試與 fuzz harness 使用小的上限,因此無窮迴圈不會讓測試卡住。

### §8.7 優化(`-O1`)

`ir_optimize()`(`include/ir_opt.h`、`src/ir_opt.c`)在驗證之後、`--dump-ir`/`--interp` 之前執行。`-O0` 不改變程式。`-O1` 依序做:暫存器重新編號(`ir_compact_vregs()`:依原順序改成 `%0`..`%(n−1)`)→ 下列 pass 依表中順序一輪一輪執行,直到某一輪完全沒有改變(不動點,最多 `IR_OPT_MAX_ROUNDS` = 1000 輪)→ 再重新編號一次。`--passes=LIST`(§6.4)只跑列出的 pass 到不動點,不重新編號。

前五個 pass(`loadfwd`、`copyprop`、`fold`、`simplify`、`branch`)是**值 pass**:它們共用一次走訪,在 pass 清單中相鄰時合成**一次掃描**(-O1 一輪只掃一次,對每個指令依序做 loadfwd → copyprop → fold → simplify,終結指令再做 branch);其餘 pass 各自執行。一輪中的每一步(一個 pass,或一組相鄰的值 pass)重複到不再改變才進下一步。

值 pass **只走訪從進入區塊可以到達的區塊**:到不了的區塊不會執行,由 `unreachable` 刪除,值 pass 完全不動它們(所以掃描是線性的,不會等待死碼中的事實一層層傳過來)。走訪順序與「鏈」:

- 依反向後序走訪,所以每個區塊都在支配它的區塊之後。
- 區塊 B 走完後,若 B 的終結指令跳到區塊 S,S 不是進入區塊、還沒走過、沒有死,而且 S **剩下的入邊只有這一條**,就接著走 S,B 結尾的事實在 S 開頭仍然成立(每次執行 S 都是直接從 B 的結尾進來)。否則事實清空,繼續反向後序中下一個沒走過的區塊。
- 入邊只計算「可能執行」的區塊:一開始是所有可到達的區塊;`branch` 把 `br` 改成 `jmp` 時去掉的那條邊不再計算;入邊數變成 0 的區塊(進入區塊除外)是死的:不走訪,它自己的邊也不再計算(遞迴)。掃描中邊只會減少,所以被接上鏈時入邊數為 1 的區塊確實只從 B 進入。

因此一連串常數條件的 `if`(`x = 1; if (x == 1) x = 2; else …; if (x == 2) x = 3; else …`)在一次掃描中就全部解開:每個 `br` 被改成 `jmp` 後 else 區塊死掉,合流區塊只剩 then 一個入邊,接上鏈,下一個條件的 `load` 被轉送、比較被摺疊。整個梯子不論多長都在 2 輪內到達不動點(golden `ir/opt_o1_ladder`;單元測試 `lil_if_ladder_folds_in_two_rounds` 量 500 與 2000 層,20000 層實測 0.5 秒)。

所有 pass 都是保守的。IR 不是 SSA,暫存器又會跨區塊使用(§8.1),所以「已知的值」(常數、copy、全域變數目前的值)只沿著鏈追蹤,而且在該暫存器被重新定義時立刻失效。唯一跨鏈的事實是「這個暫存器在整個程式(含到不了的區塊)只有一個定義,而且是 `const C`」:由 §8.4 規則 4,每個可到達的使用之前一定執行過那唯一的定義,所以值必定是 `C`;那個定義所在的區塊支配每個使用,依上述順序一定先被走訪,所以 `fold` 新產生的這種事實一次掃描就傳到所有使用。

| pass | 改寫規則 | 不做的事(反例) |
| --- | --- | --- |
| `loadfwd` | 鏈內,`store @g, %a`(或 `%a = load @g`)之後、`%a` 沒被重新定義、`@g` 沒被再 store 之前的 `%d = load @g` → `%d = copy %a`。全域只能經 `load`/`store` 讀寫(§8.1),`putc` 等都碰不到它們 | 不進入有兩條以上入邊的區塊(golden `ir/opt_loadfwd` 的 `join`);`%a` 重新定義後不轉送 |
| `copyprop` | 鏈內,`%d = copy %r` 之後、`%d` 與 `%r` 都沒被重新定義之前,對 `%d` 的使用改用 `%r`(copy 鏈一次解到根)。copy 本身留下(其他區塊可能用 `%d`),由 `dce` 處理。與 `fold` 同一次掃描時,被摺疊成 `const C` 的指令若有運算元此刻的值就是 `C`(被摺疊的 `copy %r`、已知的 `x * 1`、`x + 0` 等),它也當作那個運算元的 copy,之後的使用改用該運算元,多出來的 const 由 `dce` 刪除 | 不進入有兩條以上入邊的區塊;`%d = copy %d` 不建立事實 |
| `fold` | 運算元都已知的 `copy`/`neg`/`not`/二元/比較 → `%d = const V`,V 由 `ir_eval_unary()`/`ir_eval_binary()` 計算,即直譯器的運算(環繞:`INT64_MAX + 1` → `INT64_MIN`)。支配者先被走訪,「唯一定義是 const」一次掃描就沿著整條鏈傳下去 | `ir_eval_binary()` 回傳 false(`div` 的除數為 0,或 `INT64_MIN / -1`)時**完全不動**那個 `div`,也不因結果沒用到而刪除(§5.7) |
| `simplify` | 恰好一個運算元已知(或兩個運算元是同一個暫存器)時:`x+0`、`0+x`、`x-0`、`x*1`、`1*x`、`x/1`、`x&-1`、`-1&x`、`x\|0`、`0\|x`、`x&x`、`x\|x` → `copy x`;`x*0`、`0*x`、`x&0`、`0&x`、`x-x` → `const 0`;`x\|-1`、`-1\|x` → `const -1`;`0-x`、`x*-1`、`-1*x` → `neg x`(兩者都是取模 2^64 的環繞,結果相同);`x==x`、`x<=x`、`x>=x` → `const 1`;`x~=x`、`x<x`、`x>x` → `const 0`。暫存器沒有副作用,所以 `x*0` 可以丟掉 `x` | 兩個運算元都已知時不做(留給 `fold`,trap 的 `div` 保持原樣);`div` 只化簡 `x/1`:**不**化簡 `x/x`、`0/x`(x = 0 時 trap)、`x/-1`(x = `INT64_MIN` 時 trap)|
| `branch` | `br %c, L1, L2` 中 `%c` 已知 → `jmp L1`(`c ≠ 0`)或 `jmp L2`;`br %c, L, L` → `jmp L`。去掉的邊立刻從走訪的入邊數扣掉(見上) | 失去前驅的區塊留下(由 `unreachable` 刪除);`%c` 未知時不動 |
| `dce` | 從「根」標記活的暫存器:根是 `store`、`putc`、終結指令,以及**可能 trap 的 `div`**;活暫存器的**每一個**定義(非 SSA)都是活的,它的運算元也是活的。刪除定義死暫存器的非根指令(包括 `load` 與證明不會 trap 的 `div`:除數已知且不是 0 與 −1,或除數是 −1 而被除數已知不是 `INT64_MIN`),以及任何 `%d = copy %d`。所有區塊(含到不了的)的使用都算數 | 不刪 `store`、`putc`、終結指令、可能 trap 的 `div`;不做跨區塊的活性分析(只看「整個程式有沒有用到」)|
| `thread` | 跳到「只有一個 `jmp L`」的區塊的跳轉(`jmp` 與 `br` 的兩個目標)改成直接跳到這種區塊鏈的終點。每個區塊的終點以明確的堆疊只算一次,長鏈是線性時間。跳到進入區塊是合法的 IR(§8.4),所以進入區塊可以是終點(golden `ir/opt_thread_into_entry`);只有 `jmp L` 的進入區塊也和其他轉送區塊一樣被跳過(`ir/opt_thread_through_entry`),它本身仍是 `blocks[0]` | 跳進非空的區塊不改;鏈的終點是空區塊組成的循環(例如 `a: jmp a`、`a: jmp b` / `b: jmp a`)時不改,所以一定會停,無窮迴圈仍是無窮迴圈 |
| `unreachable` | 刪除從進入區塊到不了的區塊(孤兒、只彼此互連的循環、只被它們跳到的區塊);其餘區塊保持順序,所以 `blocks[0]` 仍是進入區塊,名稱仍唯一,跳轉目標重新編號。排在 `merge` 之前,所以 `branch` 留下的死區塊不會讓合流區塊多算一條入邊 | — |
| `merge` | 區塊 B 以 `jmp S` 結尾,S 不是進入區塊、不是 B,且這個 `jmp` 是 S 唯一的入邊(計算所有區塊,含到不了的):把 S 的指令接在 B 後面取代 `jmp`,刪除 S;B 繼續吸收新的終結指令所指的區塊 | 有兩個以上入邊的區塊、自我迴圈都不合併;進入區塊即使唯一的入邊是 `jmp entry` 也不合併(golden `ir/opt_merge_entry_back_edge`) |

**正確性**:每個 pass 都維持 §8.4 的所有規則:改寫只會移除使用或把使用換成「在同一點已定義」的暫存器,刪除的定義都沒有使用,移除邊只會讓合流處「確定已定義」的集合變大。可觀察行為(§5.7)不變;而且沒有任何 pass 會讓任何一次執行多走一步(只有一換一的改寫或刪除指令、略過空區塊),所以優化後的直譯器步數不多於優化前。除錯與測試建置(沒有定義 `NDEBUG`,目前所有建置皆是)在每個有改變的步驟之後執行 `ir_verify()`,失敗時以 `internal` 錯誤回報 `IR verification failed after pass 'NAME': ...`(編譯器的 bug;一起掃描的值 pass 以 `+` 連接,例如 `loadfwd+copyprop+fold+simplify+branch`)。

**冪等與終止**:`ir_run_pass()` 與驅動中的每一步都重複執行到不再改變為止(沒有次數上限),所以每個 pass 跑兩次與跑一次相同。每次有改變的執行都會讓下列量依字典序嚴格變小,而且沒有 pass 讓排在前面的分量變大:(指令總數,權重和,`copyprop` 可改寫的使用數,`thread` 可改寫的跳轉目標數),權重是二元與比較 3、`neg`/`not`/`load` 2、`copy` 與 `br` 1、其他 0。`dce`、`merge`、`unreachable` 減少指令數;`loadfwd`(`load` → `copy`)、`fold`(非 const → const)、`simplify`(二元 → const/copy/neg)、`branch`(`br` → `jmp`)減少權重;只有 `copyprop` 改變的一次掃描把可改寫的使用變為 0(它的事實只取決於定義與控制流圖,這兩者 `copyprop` 都不改,而且記錄 copy 事實時運算元已經解到根);`thread` 把可改寫的跳轉目標變為 0。這些都是自然數,所以重複與不動點迴圈一定停。實際上值 pass 大多一次就不再改變:`fold`、`simplify`、`copyprop`、`loadfwd` 不改控制流圖,支配者先走訪,一次就到不動點;`branch` 只有在去掉迴圈的回邊、讓先前走訪過的迴圈標頭變成只剩一個入邊時才需要第二次。到不了的區塊不走訪,所以像「死碼中依索引反序排列的依賴鏈」這種輸入也是一次完成(單元測試 `fold_ignores_unreachable_chains`:70 與 70000 個區塊)。

1000 輪的上限只是保險:-O1 在所有 golden、範例、隨機程式與梯子上最多 5 輪。真的用完 1000 輪(只可能是刻意排出不利順序的 `--passes`,例如 `branch` 與 `fold` 分在不同步驟、`merge` 夾在中間的梯子)時程式仍然正確,只是沒優化完:`ir_optimize_passes()` 回傳成功,但 `reached_fixpoint` 為 false,並設定 `internal` 警告 `the optimizer stopped after 1000 rounds without reaching a fixpoint`,CLI 把它印到標準錯誤(`FILE: internal warning: ...`)而結束碼不變。單元測試、差分測試與兩個 fuzz 目標都要求 `-O1` 到達不動點。

給優化與 backend 的注意事項:

- 到不了的區塊不受 §8.4 規則 4 檢查(見上),pass 可以留下它們(例如常數條件分支之後),也可以刪除;backend 必須略過或容忍它們,不能依賴其中暫存器的定義。
- IR 的全域名稱與區塊標籤是使用者可控的任意 `name`(§8.3,`.` 開頭也可以),例如全域可以叫 `.text`、`_start`、`rax`,標籤可以叫 `exit`。backend 與 pass **不可**把 IR 的全域名稱或標籤直接當成組譯器符號或標籤使用;一律依編號改名(例如全域 id 3 → `lil_g3`、區塊 index 7 → `.Lb7`),原名最多放在註解裡。

## 附錄 A:v1 完整 EBNF

```ebnf
(* ---- 詞法 (§2) ---- *)
ignored             = { whitespace | comment } ;
whitespace          = " " | TAB | CR | LF ;
comment             = "%", { source-byte - LF } ;

identifier          = identifier-spelling - keyword ;
identifier-spelling = identifier-start, { identifier-continue } ;
identifier-start    = letter | "_" | "." ;
identifier-continue = letter | digit | "_" | "." ;
keyword             = "if" | "else" | "while" | "do"
                    | "break" | "continue" ;         (* 不分大小寫 *)

(* 整個最長匹配的 digit-string 必須恰好是下列之一,否則為錯誤(§2.7) *)
integer-literal     = decimal-literal | octal-literal ;   (* <= INT64_MAX *)
decimal-literal     = "0" | nonzero-digit, { digit } ;
octal-literal       = "0", octal-digit, { octal-digit } ;

character-literal   = "'", ( ordinary-character | escape-sequence ), "'" ;
ordinary-character  = source-byte - ( "'" | "\" | LF ) ;
escape-sequence     = "\", ( "0" | "n" | "r" | "t" | "\" | "'" ) ;

punctuator          = "(" | ")" | "{" | "}" | ";" | "," ;
operator            = "=" | "+" | "-" | "*" | "/" | "&" | "|" | "~"
                    | "&&" | "||" | "<" | "<=" | ">" | ">="
                    | "==" | "~=" ;

letter              = "A" | ... | "Z" | "a" | ... | "z" ;
digit               = "0" | nonzero-digit ;
nonzero-digit       = "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" ;
octal-digit         = "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" ;

(* ---- 敘述 (§3) ---- *)
program             = { statement }, END ;
statement           = expression-statement
                    | empty-statement
                    | block
                    | if-statement
                    | while-statement
                    | do-statement
                    | break-statement
                    | continue-statement ;
expression-statement = expression, ";" ;
empty-statement     = ";" ;
block               = "{", { statement }, "}" ;
if-statement        = "if", "(", expression, ")", statement,
                      [ "else", statement ] ;       (* else 綁定最近的 if *)
while-statement     = "while", "(", expression, ")", statement ;
do-statement        = "do", statement, "while", "(", expression, ")", ";" ;
break-statement     = "break", ";" ;
continue-statement  = "continue", ";" ;

(* ---- 運算式 (§4) ---- *)
expression          = assignment ;
assignment          = identifier, "=", assignment     (* §4.4 *)
                    | logical-or ;
logical-or          = logical-and, { "||", logical-and } ;
logical-and         = bitwise-or, { "&&", bitwise-or } ;
bitwise-or          = bitwise-and, { "|", bitwise-and } ;
bitwise-and         = equality, { "&", equality } ;
equality            = relational, { ( "==" | "~=" ), relational } ;
relational          = additive, { ( "<" | "<=" | ">" | ">=" ), additive } ;
additive            = multiplicative, { ( "+" | "-" ), multiplicative } ;
multiplicative      = unary, { ( "*" | "/" ), unary } ;
unary               = ( "-" | "~" ), unary | primary ;
primary             = identifier | integer-literal | character-literal
                    | "(", expression, ")" | call ;
call                = identifier, "(", [ argument-list ], ")" ;
                                                     (* identifier 後緊接 "(" 一律是 call *)
argument-list       = expression, { ",", expression } ;
```

## 附錄 B:與 1974 年原版 LIL 的差異

| 項目 | 原版 LIL | LIL-C v1 |
| --- | --- | --- |
| 運算語意 | 運算子等優先、由左到右,`x + y;` 寫回 `x` | 類 C 優先序,只有 `=` 改變變數 |
| 名稱 | `r0`–`r7` 是 PDP-11 暫存器 | 一般全域變數,初值 0 |
| 目標機 | PDP-11,單趟輸出 | x86-64 ELF64,分離的階段與 IR |
| 沿用 | 關鍵字與名稱不分大小寫、`%` 註解、`~=`、八進位常值、`.` 開頭名稱 | 同左 |
