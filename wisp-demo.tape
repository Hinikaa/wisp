# vhs wisp-demo.tape  ->  wisp-demo.gif
#
# Self-contained: writes its own init.lua into a throwaway XDG_CONFIG_HOME so
# the recording reproduces anywhere, instead of depending on whatever config
# the person recording happens to have.

Output wisp-demo.gif
Set FontSize 16
Set Width 900
Set Height 640
Set Theme "Dark+"
Set Padding 20
Set TypingSpeed 35ms

Hide
Type "export XDG_CONFIG_HOME=$(mktemp -d); mkdir -p $XDG_CONFIG_HOME/wisp"
Enter
Type "cp example-init.lua $XDG_CONFIG_HOME/wisp/init.lua; clear"
Enter
Show

Sleep 1
Type "./wisp"
Enter
Sleep 1.2

Type "echo hi | wc -c"
Enter
Sleep 1

Type "echo $(uname -s) build > /tmp/w && cat /tmp/w"
Enter
Sleep 1.3

Type "echo {1..5} and {a,b}{1,2}"
Enter
Sleep 1.3

Type "echo HEAD@{1} stays literal"
Enter
Sleep 1.3

Type ":1 + 1"
Enter
Sleep 1

Type "nums | sorted"
Enter
Sleep 1.6

Type "nums | sorted | wc -l"
Enter
Sleep 1.6

Type "greet wisp"
Enter
Sleep 1.3

Type "grpe pattern"
Enter
Sleep 1.6

Type "exit"
Enter
Sleep 0.8

Type "./wisp -c 'ls *.cpp | wc -l'   # -c is shell syntax: usable as $SHELL"
Enter
Sleep 2.5
