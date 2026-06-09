' By-VALUE cases: the callee must NOT change the caller's variable. A reference
' is only taken for a bare variable (or array element) of matching type passed
' to a non-BYVAL parameter; everything else is a copy.
OPTION EXPLICIT

SUB setit(n)
  n = 999
END SUB

SUB setval(BYVAL n)
  n = 999
END SUB

' Expression argument -> by value.
DIM a = 1
setit a + 0
PRINT a

' Parenthesised expression -> by value.
DIM b = 2
setit (b * 1)
PRINT b

' BYVAL parameter -> by value even for a bare variable.
DIM c = 3
setval c
PRINT c

' Type mismatch (integer variable into a float parameter) -> by value.
DIM di% = 4
setit di%
PRINT di%

' Literal argument -> nothing to propagate to.
setit 5
PRINT "lit"

' Constant-folding expression -> by value.
DIM e = 6
setit e + 1 - 1
PRINT e
