' NEXT listing multiple loop variables to close nested loops at once.
DIM INTEGER i, j
FOR i = 1 TO 2
  FOR j = 1 TO 2
    PRINT i; j
  NEXT j, i
PRINT "done"
