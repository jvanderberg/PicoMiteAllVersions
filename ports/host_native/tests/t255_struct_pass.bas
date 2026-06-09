' Documented structure passing (MMBasic Structures Manual): structures and
' structure-array elements pass by reference; whole structure arrays pass by
' reference; functions take and return structures. The VM must match the
' interpreter on all of these.
OPTION EXPLICIT
TYPE Point
  x AS INTEGER
  y AS INTEGER
END TYPE

SUB DoublePoint(pt AS Point)
  pt.x = pt.x * 2
  pt.y = pt.y * 2
END SUB

FUNCTION Dist(pt AS Point) AS FLOAT
  Dist = SQR(pt.x * pt.x + pt.y * pt.y)
END FUNCTION

SUB SumX(pts() AS Point, count%, total%)
  LOCAL i%
  total% = 0
  FOR i% = 0 TO count% - 1
    total% = total% + pts(i%).x
  NEXT i%
END SUB

' whole struct by reference
DIM p AS Point
p.x = 10 : p.y = 20
DoublePoint p
PRINT "p="; p.x; p.y

' struct array element (a whole struct) by reference
DIM pts(4) AS Point
pts(2).x = 3 : pts(2).y = 4
DoublePoint pts(2)
PRINT "pts2="; pts(2).x; pts(2).y
PRINT "dist="; Dist(pts(2))

' whole struct array by reference + a by-ref scalar out-param
DIM i%, t%
FOR i% = 0 TO 4 : pts(i%).x = i% : NEXT i%
SumX pts(), 5, t%
PRINT "sumx="; t%
