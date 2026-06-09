' EXPECT_BOTH_ERROR:
' MMBasic does not allow passing an individual structure member to a SUB or
' FUNCTION — you pass the whole structure (by reference) or copy member data out
' with STRUCT EXTRACT. Both engines must reject `addone p.x`.
OPTION EXPLICIT
TYPE Point
  x AS INTEGER
  y AS INTEGER
END TYPE
SUB addone(n%)
  n% = n% + 1
END SUB
DIM p AS Point
p.x = 5
addone p.x
PRINT p.x
