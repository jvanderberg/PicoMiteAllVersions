' Array elements passed to scalar reference parameters — global 1D/2D, local
' arrays, and a whole array passed by reference (the pre-existing alias path).
OPTION EXPLICIT

SUB addone(n)
  n = n + 1
END SUB

' Global 1D element.
DIM a(3), i
FOR i = 0 TO 3 : a(i) = i * 10 : NEXT i
addone a(2)
PRINT "a2="; a(2)          ' 21

' Global 2D element.
DIM m(2, 2), r, c
FOR r = 0 TO 2 : FOR c = 0 TO 2 : m(r, c) = r * 10 + c : NEXT c : NEXT r
addone m(1, 1)
PRINT "m11="; m(1, 1)      ' 12

' Element index computed from a variable.
DIM idx = 3
addone a(idx)
PRINT "a3="; a(3)          ' 31

' Local array element.
SUB localarr
  LOCAL b(3), j
  FOR j = 0 TO 3 : b(j) = j : NEXT j
  addone b(1)
  PRINT "b1="; b(1)        ' 2
END SUB
localarr

' Whole array still passes by reference (alias path).
SUB fill(z())
  z(0) = 7
END SUB
fill a()
PRINT "a0="; a(0)          ' 7
