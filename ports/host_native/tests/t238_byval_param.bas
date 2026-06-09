' BYVAL parameter prefix: the callee gets a private copy, so changes it makes
' do not propagate back to the caller's variable.
DIM INTEGER x = 5
modify x
PRINT x

DIM s$ = "keep"
tweak s$
PRINT s$

SUB modify(BYVAL a AS INTEGER)
  a = 42
  PRINT a
END SUB

SUB tweak(BYVAL t AS STRING)
  t = "changed"
  PRINT t
END SUB
