' Pass-by-reference smoke: a callee that mutates a scalar parameter must change
' the caller's storage, whatever the call site's scope. Covers globals,
' caller-locals, nested-sub locals, FOR-loop variables, array elements,
' function-locals, the explicit BYREF keyword, and chained references.
OPTION EXPLICIT

SUB inc(n)
  n = n + 1
END SUB

SUB incby(n, BYVAL d)
  n = n + d
END SUB

SUB incref(BYREF n)
  n = n + 100
END SUB

' 1. Global scalar.
DIM g = 10
inc g
PRINT "g="; g

' 2. Caller-local scalar.
SUB use_caller_local
  LOCAL x = 100
  inc x
  PRINT "x="; x
END SUB
use_caller_local

' 3. Nested-sub local (two call levels deep).
SUB times2(n)
  n = n * 2
END SUB
SUB nested
  LOCAL y = 5
  times2 y
  PRINT "y="; y
END SUB
nested

' 4. FOR-loop variable: bump the counter mid-loop through a by-ref call.
DIM i, seq$ = ""
FOR i = 1 TO 6
  seq$ = seq$ + STR$(i)
  IF i = 2 THEN inc i
NEXT i
PRINT "seq="; seq$

' 5. Array element to a scalar parameter.
DIM a(4), k
FOR k = 0 TO 4 : a(k) = k : NEXT k
incby a(2), 40
PRINT "a2="; a(2)

' 6. Function-local scalar.
FUNCTION compute()
  LOCAL z = 7
  inc z
  compute = z
END FUNCTION
PRINT "fn="; compute()

' 7. Explicit BYREF keyword.
DIM b = 1
incref b
PRINT "b="; b

' 8. Chained reference: a ref parameter passed onward by reference.
SUB relay(n)
  inc n
END SUB
DIM c = 50
relay c
PRINT "c="; c

' 9. String by reference.
SUB tackon(s$)
  s$ = s$ + "!"
END SUB
DIM t$ = "hi"
tackon t$
PRINT "t="; t$
