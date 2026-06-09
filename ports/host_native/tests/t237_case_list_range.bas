' SELECT CASE with comma-separated value lists and TO ranges.
DIM INTEGER n
FOR n = 1 TO 8
  SELECT CASE n
    CASE 1, 3, 5
      PRINT n; " odd-list"
    CASE 2, 4 TO 6
      PRINT n; " two-or-4to6"
    CASE ELSE
      PRINT n; " other"
  END SELECT
NEXT n
