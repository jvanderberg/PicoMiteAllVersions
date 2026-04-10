OPTION EXPLICIT

DIM INTEGER x%(1), y%(1), r%(1), w%(1), c%(1), f%(1)
DIM a!(1)

x%(0) = 40 : y%(0) = 20 : r%(0) = 5
x%(1) = 60 : y%(1) = 20 : r%(1) = 5
w%(0) = 0  : w%(1) = 0
a!(0) = 1.0 : a!(1) = 1.0
c%(0) = RGB(WHITE) : c%(1) = RGB(WHITE)
f%(0) = RGB(BLUE)  : f%(1) = RGB(GREEN)

CLS RGB(BLACK)

' Scalar mode keeps the requested radius.
CIRCLE 20, 20, 5, 0, 1.0, , RGB(RED)

' Vector mode currently draws with radius-1.
CIRCLE x%(), y%(), r%(), w%(), a!(), c%(), f%()

PRINT "done"
END
