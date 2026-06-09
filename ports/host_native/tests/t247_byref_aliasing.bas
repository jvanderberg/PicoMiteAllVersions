' Aliasing: the same variable passed to several reference parameters must behave
' like the interpreter's true pointers (writes are visible through every alias;
' the final value is the last write).
OPTION EXPLICIT

SUB two(x, y)
  x = 10
  y = x + 5
END SUB

DIM a = 0
two a, a
PRINT "a="; a

SUB swap(x, y)
  LOCAL t = x
  x = y
  y = t
END SUB

' Swapping a variable with itself is a no-op.
DIM p = 1
swap p, p
PRINT "p="; p

SUB three(x, y, z)
  x = 1
  y = 2
  z = x + y
END SUB

DIM q = 0
three q, q, q
PRINT "q="; q

' Mixed: one aliased pair plus an independent variable.
DIM m = 0, n = 0
two m, n
PRINT "m="; m; " n="; n
