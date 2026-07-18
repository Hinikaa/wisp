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

Type "nums | sorted"
Sleep 0.3
Enter
Sleep 1

Type "hello world"
Sleep 0.3
Enter
Sleep 0.8

Type "grpe --version"
Sleep 0.3
Enter
Sleep 1

Type "cd /tmp"
Sleep 0.3
Enter
Sleep 0.5

Type "cd -"
Sleep 0.3
Enter
Sleep 0.5

Type "pwd"
Sleep 0.3
Enter
Sleep 0.5

Type "echo -n one two"
Sleep 0.3
Enter
Sleep 0.5

Type "echo -e 'line1\\nline2'"
Sleep 0.3
Enter
Sleep 0.5

Type "exit"
Sleep 0.3
Enter
Sleep 1
