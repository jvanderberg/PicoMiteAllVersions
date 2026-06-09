' STATIC local variable retains its value across subroutine calls.
counter
counter
counter

SUB counter
  STATIC INTEGER n = 0
  n = n + 1
  PRINT n
END SUB
