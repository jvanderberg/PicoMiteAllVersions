PRINT "Sieve of Eratosthenes"
PRINT "====================="
DIM flags%(8192)
DIM i%, j%, count%, t!
t! = TIMER
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
t! = TIMER - t!
PRINT "Primes found: "; count%
PRINT "Time: "; t!; " sec"
PRINT
PRINT "First 20 primes:"
DIM n% = 0
FOR i% = 2 TO 8191
  IF flags%(i%) THEN
    PRINT i%;
    n% = n% + 1
    IF n% >= 20 THEN EXIT FOR
  ENDIF
NEXT i%
PRINT
