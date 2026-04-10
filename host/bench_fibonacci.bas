FUNCTION Fib%(n%)
  IF n% <= 1 THEN
    Fib% = n%
  ELSE
    Fib% = Fib%(n% - 1) + Fib%(n% - 2)
  ENDIF
END FUNCTION
DIM t!
t! = TIMER
PRINT Fib%(30)
t! = TIMER - t!
PRINT "Time: "; t!; " sec"
