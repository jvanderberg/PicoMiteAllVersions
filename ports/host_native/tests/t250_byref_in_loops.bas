' A reference parameter read inside a DO/LOOP (which the VM may convert to a
' fast register loop) must still resolve to the caller's storage, not the
' callee's empty slot. Isolates a fast-loop / by-reference interaction.
OPTION EXPLICIT

DIM a%(9), i%
FOR i% = 0 TO 9 : a%(i%) = i% * 10 : NEXT i%

' Linear search via FOR.
FUNCTION lin%(n%, x%)
  LOCAL k%
  lin% = -1
  FOR k% = 0 TO n% - 1
    IF a%(k%) = x% THEN lin% = k%
  NEXT k%
END FUNCTION
PRINT "lin="; lin%(10, a%(9))

' Linear search via DO/LOOP.
FUNCTION dw%(n%, x%)
  LOCAL k%
  dw% = -1
  k% = 0
  DO WHILE k% < n%
    IF a%(k%) = x% THEN dw% = k%
    k% = k% + 1
  LOOP
END FUNCTION
PRINT "dw="; dw%(10, a%(9))

' Accumulate a by-ref scalar inside a DO/LOOP.
SUB addloop(acc, BYVAL count)
  LOCAL j%
  j% = 0
  DO WHILE j% < count
    acc = acc + 1
    j% = j% + 1
  LOOP
END SUB
DIM total = 0
addloop total, 5
PRINT "total="; total
