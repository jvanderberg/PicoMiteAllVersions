' The VM source compiler must accept the same THEN-less and implicit-goto IF
' forms the interpreter does:
'   IF cond GOTO label     -> IF cond THEN GOTO label
'   IF cond GOTO linenum   -> IF cond THEN GOTO linenum
'   IF cond THEN linenum   -> implicit GOTO to a line number
10 j = 0
20 j = j + 1
30 IF j < 5 GOTO 20
40 PRINT "g1"; j
50 k = 0
60 k = k + 1
70 IF k < 3 THEN 60
80 PRINT "g2"; k
' False condition: GOTO must not be taken, control falls through.
90 IF k > 100 GOTO 130
100 PRINT "fell through"
110 GOTO 140
130 PRINT "should not reach"
' THEN-less GOTO to a named label (the benchmark.bas case).
140 n = 0
150 again:
160 n = n + 1
170 IF n < 3 GOTO again
180 PRINT "g3"; n
190 PRINT "ok"
200 END
