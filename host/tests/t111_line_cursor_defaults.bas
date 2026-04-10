OPTION EXPLICIT

CLS RGB(BLACK)

' Move the line cursor without drawing.
PIXEL 12, 12, -1

' Omitted end point should draw from the cursor to the new point.
LINE 20, 12

' Width zero should leave the framebuffer unchanged.
LINE 30, 12, 40, 12, 0, RGB(RED)

PRINT "done"
END
