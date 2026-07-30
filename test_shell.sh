#!/bin/sh
# End-to-end tests: feed wisp a line, compare what it prints.
#
# The unit tests cover the pure pieces (lexing, parsing, segment grouping) and
# passed happily through a shell that dropped every builtin's output into a
# pipe, ignored `echo hi > f`, and exited on Ctrl-C. Anything that only shows
# up when the process really runs belongs here.

WISP=./wisp
pass=0
fail=0
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

# check <name> <expected> <line...>
check() {
    name=$1
    want=$2
    shift 2
    got=$(cd "$tmp" && printf '%s\n' "$@" | "$OLDPWD/$WISP" 2>&1)
    if [ "$got" = "$want" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL %s\n  want: %s\n  got:  %s\n' "$name" "$(echo "$want" | tr '\n' '|')" "$(echo "$got" | tr '\n' '|')"
    fi
}

# check_arg <name> <expected> <argv...>   -- non-interactive invocation
check_arg() {
    name=$1
    want=$2
    shift 2
    got=$(cd "$tmp" && "$OLDPWD/$WISP" "$@" 2>&1)
    if [ "$got" = "$want" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL %s\n  want: %s\n  got:  %s\n' "$name" "$want" "$got"
    fi
}

# --- externals and pipelines -------------------------------------------------
check 'external'            'hi'        'echo hi'
check 'pipe to external'    '3'         'echo hi | wc -c'
check 'pipe both external'  'HI'        'printf hi | tr a-z A-Z'
check 'and-or short circuit' 'yes'      'true && echo yes'
check 'or short circuit'    'no'        'false || echo no'
check 'exit status'         '1'         'false' 'echo $?'
check 'semicolons'          'a
b'                                      'echo a ; echo b'

# --- command substitution ----------------------------------------------------
check 'subst builtin'       '[hi]'      'echo [$(echo hi)]'
check 'subst with spaces'   'a b'       'echo $(echo a b)'
check 'subst with pipe'     'HI'        'echo $(echo hi | tr a-z A-Z)'
check 'subst nested'        'deep'      'echo $(echo $(echo deep))'
check 'subst in quotes'     'x a b y'   'echo "x $(echo a b) y"'

# --- redirects ---------------------------------------------------------------
check 'builtin redirect'    'hello'     'echo hello > f1' 'cat f1'
check 'builtin append'      'a
b'                                      'echo a > f2' 'echo b >> f2' 'cat f2'
check 'external redirect'   'x'         'printf x > f3' 'cat f3'
check 'stderr redirect'     ''          'ls /nonexistent-zz 2> f4'
check 'combined redirect'   'found'     'ls /nonexistent-zz &> f5' 'grep -q nonexistent f5 && echo found'
check 'read redirect'       'in'        'echo in > f6' 'cat < f6'

# --- expansion ---------------------------------------------------------------
check 'var'                 'bar'       'export FOO=bar' 'echo $FOO'
check 'assignment'          'bar'       'FOO=bar' 'echo $FOO'
check 'assignment scoped'   'x=1
[]'                                     'x=1 env | grep ^x=' 'echo [$x]'
check 'default value'       'fallback'  'echo ${NOPE_ZZ:-fallback}'
check 'brace alternatives'  'a1 a2 b1 b2' 'echo {a,b}{1,2}'
check 'brace range'         '1 2 3'     'echo {1..3}'
check 'brace left alone'    'HEAD@{1}'  'echo HEAD@{1}'
check 'single quotes raw'   '$HOME {a,b} *' "echo '\$HOME {a,b} *'"
check 'quote concatenation' '--fmt=%h %s' "echo --fmt='%h %s'"
check 'backslash space'     'a b'       'echo a\ b'
check 'backslash dollar'    '$HOME'     'echo \$HOME'
check 'comment'             'hi'        'echo hi # not printed'
check 'glob no match kept'  '*.nomatch-zz' 'echo *.nomatch-zz'
check 'glob match'          'g1.zz g2.zz' 'printf "" > g1.zz' 'printf "" > g2.zz' 'echo *.zz'

# --- builtins ----------------------------------------------------------------
check 'cd updates PWD'      '/tmp'      'cd /tmp' 'echo $PWD'
check 'cd dash'             'ok'        'cd /tmp' 'cd -' 'test "$PWD" = "/tmp" || echo ok'
check 'type builtin'        'cd is a shell builtin' 'type cd'
check 'echo -n'             'ab'        'echo -n a' 'echo b'
check 'echo -e'             'a
b'                                      'echo -e "a\nb"'
check 'unknown command'     'wisp: definitely-not-a-command-zz: command not found' \
                                        'definitely-not-a-command-zz'

# --- lua ---------------------------------------------------------------------
check 'lua sigil'           '42'        ':print(6 * 7)'
check 'lua expression'      '2'         ':1 + 1'
check 'lua function'        'HI'        ':function up(x) return string.upper(x[1]) end' 'echo hi | up'
check 'lua returns table'   'a
b'                                      ':function two() return {"a","b"} end' 'two'
check 'lua into external'   '2'         ':function two() return {"a","b"} end' 'two | wc -l'
check 'lua value redirect'  'v'         ':function one() return "v" end' 'one > f7' 'cat f7'
check 'sh() from lua'       'from-lua
0'                                      ':sh("echo from-lua")'

# --- argv --------------------------------------------------------------------
check_arg '-c runs shell'   'hi'        -c 'echo hi'
check_arg '-c chains'       'a
b'                                      -c 'echo a && echo b'
check_arg '-c lua sigil'    '42'        -c ':print(42)'
check_arg '-e runs lua'     '42'        -e 'print(42)'
check_arg '-c needs arg'    'wisp: -c requires an argument' -c

printf '%s\n' "test_shell: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
