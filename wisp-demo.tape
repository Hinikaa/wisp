# vhs wisp-demo.tape  ->  wisp-demo.gif
#
# Self-contained: writes its own init.lua into a throwaway XDG_CONFIG_HOME so
# the recording reproduces anywhere.

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
Type "clear"
Enter
Show

Sleep 0.8
Type "./wisp"
Enter
Sleep 1.0

Type "files | where type == file | sort_by size desc | take 5 | cols name size"
Enter
Sleep 2.0

Type "files | where name ~= %.cpp$ | count"
Enter
Sleep 1.4

Type "files | where name ~= %.h$ | cols name size | take 2 | to_json"
Enter
Sleep 1.8

Type "echo {a,b}{1..3} && echo $(uname -s)"
Enter
Sleep 1.4

Type ":for i = 1, 3 do print('lua #' .. i) end"
Enter
Sleep 1.4

Type "grpe pattern"
Enter
Sleep 1.5

Type "exit"
Enter
Sleep 0.6

Type "./wisp -c 'files | where name ~= %.cpp$ | count'"
Enter
Sleep 2.0
