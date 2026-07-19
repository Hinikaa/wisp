Output wisp-demo.gif
Set FontSize 16
Set Width 800
Set Height 600
Set Theme "Dark+"
Set Padding 20

Sleep 1
Type "./wisp"
Sleep 0.5
Enter
Sleep 1

Type "ls /etc/hostname"
Sleep 0.3
Enter
Sleep 0.8

Type "echo 'hello from wisp'"
Sleep 0.3
Enter
Sleep 0.8

Type "touch /tmp/report_a.csv /tmp/report_b.csv /tmp/report_c.csv"
Sleep 0.3
Enter
Sleep 0.5

Type "ls /tmp/report_*.csv"
Sleep 0.3
Enter
Sleep 0.8

Type "rm /tmp/report_a.csv /tmp/report_b.csv /tmp/report_c.csv"
Sleep 0.3
Enter
Sleep 0.5

Type "echo {a,b,c}{1,2}"
Sleep 0.3
Enter
Sleep 0.8

Type "nums | sorted"
Sleep 0.3
Enter
Sleep 1

Type "hello world"
Sleep 0.3
Enter
Sleep 0.8

Type "type cd"
Sleep 0.3
Enter
Sleep 0.8

Type "ls /nonexistent 2>&1"
Sleep 0.3
Enter
Sleep 0.8

Type "cd /tmp && cd - && pwd"
Sleep 0.3
Enter
Sleep 0.8

Type "exit"
Sleep 0.3
Enter
Sleep 1
