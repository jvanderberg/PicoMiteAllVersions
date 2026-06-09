' Default parameter passing is BY REFERENCE: a SUB/FUNCTION that modifies a
' scalar variable argument changes the caller's variable. BYVAL opts out.
OPTION EXPLICIT

SUB swap(a, b)
  LOCAL t = a
  a = b
  b = t
END SUB

SUB addone(n)
  n = n + 1
END SUB

SUB addoneval(BYVAL n)
  n = n + 1
END SUB

FUNCTION bump(x)
  x = x + 10
  bump = x * 2
END FUNCTION

DIM p = 3, q = 7
swap p, q
PRINT p; q              ' 7 3

DIM c = 5
addone c
PRINT c                 ' 6

DIM v = 5
addoneval v
PRINT v                 ' 5  (BYVAL — unchanged)

DIM w = 4
DIM r = bump(w)
PRINT w; r              ' 14 28

' String by reference.
SUB tackon(s$)
  s$ = s$ + "!"
END SUB
DIM g$ = "hi"
tackon g$
PRINT g$                ' hi!
