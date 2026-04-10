DIM x!, y!, zr!, zi!, tr!, ti!, iter%
DIM cx%, cy%
DIM maxiter% = 200
DIM t!
t! = TIMER
FOR cy% = -80 TO 80
  FOR cx% = -120 TO 40
    x! = cx% / 40.0
    y! = cy% / 40.0
    zr! = 0
    zi! = 0
    iter% = 0
    DO WHILE iter% < maxiter%
      tr! = zr! * zr! - zi! * zi! + x!
      ti! = 2 * zr! * zi! + y!
      zr! = tr!
      zi! = ti!
      IF zr! * zr! + zi! * zi! > 4 THEN EXIT DO
      iter% = iter% + 1
    LOOP
  NEXT cx%
NEXT cy%
t! = TIMER - t!
PRINT "Mandelbrot 161x161 done"
PRINT "Time: "; t!; " sec"
