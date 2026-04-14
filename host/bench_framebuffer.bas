OPTION EXPLICIT

CONST W% = MM.HRES
CONST H% = MM.VRES
CONST HUDH% = 18
CONST FRAMES% = 240

CONST COL_BG% = RGB(BLACK)
CONST COL_FG% = RGB(WHITE)
CONST COL_ACCENT% = RGB(CYAN)
CONST COL_ALT% = RGB(YELLOW)

DIM INTEGER i%, x%, y%, oldx%, oldy%, dx%, dy%, t0%
DIM INTEGER r%, c%, bx%, by%

FRAMEBUFFER CREATE
FRAMEBUFFER LAYER RGB(BLACK)

FRAMEBUFFER WRITE F
CLS COL_BG%
FOR r% = 0 TO 4
  FOR c% = 0 TO 7
    bx% = 8 + c% * 38
    by% = 34 + r% * 16
    BOX bx%, by%, 30, 10, 1, COL_ACCENT%, COL_ALT%
  NEXT
NEXT

FRAMEBUFFER WRITE L
CLS RGB(BLACK)
FRAMEBUFFER MERGE RGB(BLACK), R, 0

x% = 20
y% = HUDH% + 16
oldx% = x%
oldy% = y%
dx% = 3
dy% = 2
t0% = TIMER

FOR i% = 1 TO FRAMES%
  FRAMEBUFFER WRITE F
  BOX oldx%-3, oldy%-3, 26, 26, 0, , COL_BG%
  CIRCLE x% + 8, y% + 8, 7, 0, 1.0, , COL_ALT%
  LINE x%, y%, x% + 18, y% + 18, 1, COL_FG%
  BOX x% + 2, y% + 2, 12, 12, 1, COL_ACCENT%

  FRAMEBUFFER WRITE L
  BOX 0, 0, W%, HUDH%, 0, , RGB(BLACK)
  TEXT 4, 3, "frame " + STR$(i%), "LT", , , COL_FG%, RGB(BLACK)
  TEXT W%-4, 3, "fb bench", "RT", , , COL_ACCENT%, RGB(BLACK)

  FRAMEBUFFER SYNC

  oldx% = x%
  oldy% = y%
  x% = x% + dx%
  y% = y% + dy%
  IF x% < 4 OR x% > W% - 24 THEN dx% = -dx% : x% = x% + dx%
  IF y% < HUDH% + 4 OR y% > H% - 24 THEN dy% = -dy% : y% = y% + dy%
NEXT

FRAMEBUFFER MERGE RGB(BLACK), A
FRAMEBUFFER CLOSE

PRINT "elapsed_ms="; TIMER - t0%
