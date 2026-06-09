' SELECT CASE with relational CASE IS comparisons.
DIM INTEGER n
FOR n = 1 TO 6
  SELECT CASE n
    CASE IS < 3
      PRINT n; " low"
    CASE IS > 4
      PRINT n; " high"
    CASE ELSE
      PRINT n; " mid"
  END SELECT
NEXT n
