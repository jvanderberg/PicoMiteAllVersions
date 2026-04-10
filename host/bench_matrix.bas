DIM N% = 40
DIM a!(N%, N%), b!(N%, N%), c!(N%, N%)
DIM i%, j%, k%, t!
FOR i% = 0 TO N%
  FOR j% = 0 TO N%
    a!(i%, j%) = i% + j%
    b!(i%, j%) = i% - j%
  NEXT j%
NEXT i%
t! = TIMER
FOR i% = 0 TO N%
  FOR j% = 0 TO N%
    c!(i%, j%) = 0
    FOR k% = 0 TO N%
      c!(i%, j%) = c!(i%, j%) + a!(i%, k%) * b!(k%, j%)
    NEXT k%
  NEXT j%
NEXT i%
t! = TIMER - t!
PRINT "c(0,0)="; c!(0, 0)
PRINT "c(20,20)="; c!(20, 20)
PRINT "Time: "; t!; " sec"
