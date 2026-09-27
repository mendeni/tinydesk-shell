# Shell scripts (`.tdsh`)

TinyDesk Shell runs script files ending in `.tdsh`. The script language
(called uScript in the code, version 1.1.1) looks like a Unix shell for
single commands, variables, quoting, pipes and redirection, and uses plain
keywords for blocks: `if … endif`, `while … endwhile`, `for … endfor`,
`function … end`. The same scripts run on every port: the ESP32 boards, the
desktop's Terminal window and the Linux shell.

```sh
#!/bin/tdsh
# blink.tdsh - say hello three times
NAME=$(whoami)
I=1
while $I <= 3
    echo "hello $NAME, round $I"
    I=$((I + 1))
endwhile
```

## Running a script

| How | What happens |
|---|---|
| `tdsh run ~/blink.tdsh` | Runs the script and waits for it. `$?` afterwards is the script's status. |
| `tdsh run ~/tools` | A folder runs its `main.tdsh` (created, empty, if it is missing). |
| `tdsh run ~/logger.tdsh --bg` | Runs it in the background and returns at once (`[tdsh-bg] started …`). On the ESP32 it is its own task with a 32 KB stack. |
| `~/.tdshrc.tdsh` | Each user's startup script on the ESP32 boards. It runs when the local console starts as that user (the board's console, or the desktop's Terminal window), for example to join Wi-Fi with `wificonnect`. An empty one is created on the first start. The PC programs do not run it. |
| TinyDesk desktop: right-click → **Run** | Opens the Terminal window and runs `tdsh run "<file>"` there. Double-click opens the script in the Editor instead. |

A script started with `tdsh run` gets a copy of the caller's session: it
sees its variables, user and current folder, but what it changes (variables,
`cd`) does not leak back. The startup script `~/.tdshrc.tdsh` is the
exception: it runs in the console session itself, so its variables and
`cd` stay. `tdsh run` does not pass arguments to a script; use variables, or
call a function in the script with arguments.

The first line `#!/bin/tdsh` is a comment like any line starting with `#`.
A `#` after a space also starts a comment (`echo hi   # note`); inside
quotes it is an ordinary character.

## Variables and quoting

```sh
NAME="TinyDesk"                        # no spaces around =
export PORT=1883                       # same as PORT=1883
echo "hello $NAME on ${NAME}-board"    # double quotes expand variables
echo 'single quotes keep $NAME'        # single quotes are literal
unset PORT
set                                    # lists them, with USER, HOME, PWD, HOSTNAME
```

* Values are strings. Numbers are strings that arithmetic and the
  comparisons below read as 64-bit integers (decimal, or `0x…` hex).
* `${NAME}` separates a name from following text: `"${A}-${B}"`.
* An unknown variable expands to nothing.

| Special | Value |
|---|---|
| `$?` | Status of the last command or function (0 = success). |
| `$0` | The script's path. |
| `$1` … `$9`, … | Arguments of the current function (empty outside one). |
| `$#` | Number of arguments of the current function. |

## Command substitution and arithmetic

```sh
TODAY=$(date)                      # the command's output
INNER=$(echo $(echo nested))       # they nest
N=7
echo $((N * 3 + 1))                # 22
echo $((N / 2)) $((N % 4))         # 3 3 (integer division)
OK=$(((N > 5) && (N < 10)))        # 1 (true) or 0 (false)
```

`$(…)` keeps up to 8 KB of output; newlines in it become spaces.
`$((…))` understands `+ - * / %`, the comparisons `== != < <= > >=`,
`&& || !`, unary `-` and parentheses. Variables can be written with or
without `$` inside it. Dividing by zero is an error.

## Conditions: `if`, `elseif`, `else`

```sh
if $N == 7
    echo seven
elseif $N > 7
    echo bigger
else
    echo smaller
endif
```

An `if`, `elseif` or `while` condition is one of:

| Form | True when |
|---|---|
| `A == B`, `A != B` | Equal / different: as numbers when both are numbers, otherwise as text. |
| `A < B`, `A <= B`, `A > B`, `A >= B` | Numeric comparison (both sides must be numbers). |
| `(( expression ))` | The arithmetic expression is not 0: `if (( N * 2 == 14 ))`. |
| a command, e.g. `test -f ~/a.txt`, `[ "$A" = "$B" ]` or a script function | Its status is 0. |
| `true`, `false` | Always / never. |
| any other arithmetic expression | Not 0. |

Quote variables that may be empty or contain spaces: `if "$NAME" == ""`.
Up to 16 `if`/`elseif` branches per block.

### `test` and `[ … ]`

`test` (and its other spelling `[ … ]`) is a command, so it also works with
`&&` and `||`:

| Test | True when |
|---|---|
| `-e PATH`, `-f PATH`, `-d PATH` | It exists / is a file / is a folder. |
| `-s PATH` | It is a file and not empty. |
| `-n TEXT`, `-z TEXT` | The text is not empty / is empty. |
| `A = B`, `A != B` | Texts equal / different. |
| `A -eq B`, `-ne`, `-lt`, `-le`, `-gt`, `-ge` | Integer comparisons. |
| `! TEST` | Negation: `test ! -e ~/lock`. |

## Loops

```sh
I=0
while $I < 3
    I=$((I + 1))
endwhile

for F in ~/logs/*.txt              # a list of words, globs expanded
    echo "$F: $(cat $F)"
endfor

for W in red green blue
    if $W == green
        continue                   # next item
    endif
    if $W == blue
        break                      # leave the loop
    endif
    echo $W
endfor
```

## Functions

```sh
function greet
    echo "greet got $# arguments: $1 and $2"
    RESULT="greeted $1"            # functions share the script's variables
    return 3                       # the status, read with $?
end

greet alice bob
echo "status $?, $RESULT"
```

Functions are found anywhere in the file, so a script may call one above
its definition. Call them like commands, with arguments. There are no
local variables: every variable a function sets is visible to the rest of
the script. `return` without a number returns 0; `return` outside a
function ends the script with that status. A function can call others up
to 8 levels deep.

## Pipes, redirection and chaining

```sh
echo alpha > ~/a.txt               # write
echo beta >> ~/a.txt               # append
cat < ~/a.txt                      # read a file as input
cat < ~/a.txt | cat > ~/copy.txt   # both, with a pipe
> ~/empty.txt                      # create or empty a file
ls ~ | cat                         # up to 8 commands in a pipe
cp ~/logs/q?.log ~/backup          # * and ? expand to matching files
echo "~/logs/*.log"                # quoted: stays literal
true && echo "ran after success"
false || echo "ran after failure"
A=1; B=2; echo $A $B               # ; separates commands
```

Pipes are held in memory (there are no processes). A pattern that matches
nothing stays as it is. Everything in this section also works at the
interactive prompt; the block keywords (`if`, `while`, `for`, `function`)
only in scripts.

## Errors

A command that fails does not stop the script: check `$?`, `&&` or `||`.
Syntax errors (an `if` without `endif`, `break` outside a loop, a line that
is too long) stop the script before or while it runs, with a message such
as `uscript: /root/x.tdsh:12: …` naming the file and line.

## Limits

| Limit | Value |
|---|---|
| Line length | 512 bytes |
| Lines per script | 1024 |
| Words per command | 48 |
| Variables per session | 64; names up to 31 characters, values up to 255 bytes |
| Functions per script | 24, nested calls up to 8 deep |
| `if` / `elseif` branches | 16 per block |
| Pipe stages | 8 |
| `$(…)` output | 8 KB |

## Not supported

Compared with a Unix shell: no `then`/`do`/`done`/`fi` (use the keywords
above), no `case`, here-documents, `$@`, `local`, arrays, `&` for background
jobs (use `tdsh run … --bg`), or arguments to `tdsh run`.

## A complete example

This script uses every feature above; it runs unchanged on the boards and
in the Linux shell (`tdsh run ~/example.tdsh`):

```sh
#!/bin/tdsh
NAME="TinyDesk"
echo "hello from $NAME, script $0"
echo 'single quotes keep $NAME'
TODAY=$(echo 28-09)
echo "captured: $TODAY, joined: ${NAME}-${TODAY}"
N=7
echo "arithmetic: $((N * 3 + 1)) $((N / 2)) $((N % 4)) $(((N > 5) && (N < 10)))"

function greet
    echo "greet got $# args: $1 $2"
    return 3
end
greet alice bob
echo "greet returned $?"

if $N == 7
    echo "if: N is seven"
elseif $N > 7
    echo "if: bigger"
else
    echo "if: smaller"
endif
if (( N * 2 == 14 ))
    echo "(( )): true"
endif
if [ "$NAME" = "TinyDesk" ]
    echo "[ ]: names match"
endif

I=0
while $I < 3
    I=$((I + 1))
endwhile
echo "while: I=$I"

mkdir -p ~/doc_demo
echo one > ~/doc_demo/a.txt
echo two > ~/doc_demo/b.txt
echo three >> ~/doc_demo/b.txt
for F in ~/doc_demo/*.txt
    echo "for: $F holds $(cat $F)"
endfor

COUNT=$(cat < ~/doc_demo/b.txt | cat)
echo "pipe + redirect: $COUNT"
true && echo "&&: ran" || echo "never"
false || echo "||: ran"
rm -r -y ~/doc_demo
return 0
```

Output:

```text
hello from TinyDesk, script /root/example.tdsh
single quotes keep $NAME
captured: 28-09, joined: TinyDesk-28-09
arithmetic: 22 3 3 1
greet got 2 args: alice bob
greet returned 3
if: N is seven
(( )): true
[ ]: names match
while: I=3
for: ~/doc_demo/a.txt holds one
for: ~/doc_demo/b.txt holds two three
pipe + redirect: two three
&&: ran
||: ran
```

The test scripts in `tests/scripts/` check every feature with pass/fail output:
`test_full_uscript_1_1.tdsh` (run by the host build's `ctest`),
`test_shell_features.tdsh` (redirection, globs, `test`),
`test_script_scope.tdsh` (variables and `cd` do not leak) and
`stack_test.tdsh` (a quick check on a board). Run one on a board with
`tdsh run ~/test_shell_features.tdsh` after copying it there.
