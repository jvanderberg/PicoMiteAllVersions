DIM flags%(8192)
DIM i%, j%, count%, pass%, t!
t! = TIMER
FOR pass% = 1 TO 10
  FOR i% = 0 TO 8191
    flags%(i%) = 1
  NEXT i%
  count% = 0
  FOR i% = 2 TO 8191
    IF flags%(i%) THEN
      count% = count% + 1
      FOR j% = i% + i% TO 8191 STEP i%
        flags%(j%) = 0
      NEXT j%
    ENDIF
  NEXT i%
NEXT pass%
t! = TIMER - t!
PRINT "Primes: "; count%
PRINT "Passes: 10"
PRINT "Time: "; t!; " sec"
