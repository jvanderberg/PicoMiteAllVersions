' By-reference must work when the SUB/FUNCTION is defined AFTER the call site
' (a forward reference) — the common "main code on top, subs below" layout.
' The reference decision needs the callee's parameter metadata, which must be
' available from the predeclare pass, not only once the body is compiled.
OPTION EXPLICIT

DIM a = 5
doub a
PRINT "a="; a              ' 10

DIM arr(3)
arr(1) = 7
doub arr(1)
PRINT "arr1="; arr(1)      ' 14

DIM s$ = "hi"
tack s$
PRINT "s="; s$            ' hi!

DIM r = bump(4)
PRINT "bump="; r           ' 9

SUB doub(n)
  n = n * 2
END SUB

SUB tack(t$)
  t$ = t$ + "!"
END SUB

FUNCTION bump(x)
  x = x + 5
  bump = x
END FUNCTION
