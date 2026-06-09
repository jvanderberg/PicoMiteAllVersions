' Recursion: a reference parameter threaded through recursive calls must keep
' pointing at the original caller variable (chained references collapse).
OPTION EXPLICIT

SUB accum(n, BYVAL depth)
  IF depth = 0 THEN EXIT SUB
  n = n + depth
  accum n, depth - 1
END SUB

DIM s = 0
accum s, 5
PRINT "s="; s              ' 5+4+3+2+1 = 15

' Recursive factorial writing its result through a by-ref out-parameter.
SUB fact(BYVAL k, r)
  IF k <= 1 THEN
    r = 1
  ELSE
    LOCAL sub = 0
    fact k - 1, sub
    r = k * sub
  ENDIF
END SUB

DIM f = 0
fact 6, f
PRINT "f="; f              ' 720
