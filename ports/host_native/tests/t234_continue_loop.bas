' CONTINUE FOR and CONTINUE DO skip to the next iteration.
DIM INTEGER i = 0
FOR i = 1 TO 5
  IF i = 3 THEN CONTINUE FOR
  PRINT "f"; i
NEXT i
i = 0
DO
  i = i + 1
  IF i = 2 THEN CONTINUE DO
  PRINT "d"; i
LOOP UNTIL i >= 4
PRINT "done"
