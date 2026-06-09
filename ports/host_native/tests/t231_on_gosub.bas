' ON expr GOSUB dispatch to one of several subroutine labels.
DIM INTEGER n
FOR n = 1 TO 3
  ON n GOSUB s1, s2, s3
NEXT n
PRINT "done"
END
s1:
  PRINT "one"
  RETURN
s2:
  PRINT "two"
  RETURN
s3:
  PRINT "three"
  RETURN
