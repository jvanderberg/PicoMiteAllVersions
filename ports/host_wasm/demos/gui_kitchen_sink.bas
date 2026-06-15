' GUI kitchen-sink touch demo (640x480 / MODE 1).
' Tap controls with the mouse. Press any key to exit.
'   - Pen|Idle switch + Snap checkbox + pen-width spinbox control the
'     drawing pad on the right; drag in it to paint.
'   - Clear wipes the pad, Msg opens a message box.
'   - Pulse radio animates the gauge + bar gauge.
' GUI controls are enabled by default in the browser build.

MODE 1

' --- modern palette -----------------------------------------------------
DIM INTEGER C_BG   = RGB(22,24,30)
DIM INTEGER C_PANEL= RGB(40,44,54)
DIM INTEGER C_FG   = RGB(222,226,232)
DIM INTEGER C_MUTE = RGB(120,128,140)
DIM INTEGER C_BLUE = RGB(56,132,255)
DIM INTEGER C_TEAL = RGB(64,196,210)
DIM INTEGER C_GREEN= RGB(60,200,130)
DIM INTEGER C_AMBER= RGB(240,196,84)
DIM INTEGER C_RED  = RGB(232,96,104)

GUI DELETE ALL
CLS C_BG
COLOUR C_FG, C_BG

GUI FRAME #1, "GUI Kitchen Sink", 4, 4, 632, 472, C_MUTE
GUI CAPTION #2, "Controls", 18, 30
GUI BUTTON #3, "Clear", 470, 18, 150, 38, RGB(30,30,30), C_AMBER
GUI BUTTON #17, "Msg", 470, 64, 150, 38, C_FG, C_BLUE

GUI SWITCH #4, "Pen|Idle", 18, 62, 180, 40, C_FG, C_TEAL
GUI CHECKBOX #5, "Snap", 224, 70, 26, C_AMBER
GUI RADIO #6, "Pulse", 340, 82, 13, C_TEAL

GUI NUMBERBOX #8, 18, 124, 120, 36, C_FG, C_PANEL
GUI SPINBOX #9, 152, 124, 140, 36, C_FG, C_PANEL, 1, 1, 8
GUI TEXTBOX #10, 306, 124, 160, 36, C_FG, C_PANEL
GUI LED #7, "Touch", 600, 142, 14, C_GREEN

GUI FORMATBOX #11, "2hHh(:)5m9m", 18, 178, 150, 36, C_FG, C_PANEL
GUI DISPLAYBOX #12, 182, 178, 284, 36, C_TEAL, C_PANEL

GUI GAUGE #13, 110, 350, 86, C_FG, C_BG, 0, 100, 0, "V", C_GREEN, 25, C_AMBER, 50, C_TEAL, 75, C_RED
GUI BARGAUGE #14, 18, 452, 220, 16, C_MUTE, C_PANEL, 0, 100, C_GREEN, 25, C_AMBER, 50, C_TEAL, 75, C_RED

GUI CAPTION #16, "Touch canvas - drag to draw", 470, 238
GUI AREA #15, 300, 250, 320, 210

CTRLVAL(4) = 1
CTRLVAL(6) = 1
CTRLVAL(8) = 42
CTRLVAL(9) = 3
CTRLVAL(10) = "hello"
CTRLVAL(11) = "09:30"
CTRLVAL(12) = "Ready"
CTRLVAL(13) = 35
CTRLVAL(14) = 65
GUI REDRAW ALL
GOSUB ClearPad

LastRef = -1
LastDown = -1
LastX = -1
LastY = -1
OldS$ = ""
V = 35
NextAnim = TIMER
NextUi = TIMER

DO
  K$ = INKEY$
  IF K$ <> "" THEN EXIT

  Ref = TOUCH(REF)
  X = TOUCH(X)
  Y = TOUCH(Y)

  IF Ref = 3 AND LastRef <> 3 THEN GOSUB ClearPad
  IF Ref = 17 AND LastRef <> 17 THEN
    M = MSGBOX("Kitchen sink~touch dialog", "OK", "Cancel")
    GUI REDRAW ALL
    GOSUB ClearPad
    CTRLVAL(12) = "MSGBOX=" + STR$(M)
    OldS$ = CTRLVAL(12)
  ENDIF
  LastRef = Ref

  Down = 0
  IF X >= 302 AND X <= 618 AND Y >= 252 AND Y <= 458 THEN Down = 1
  IF Down <> LastDown THEN
    CTRLVAL(7) = Down
    LastDown = Down
  ENDIF

  IF Down = 1 AND CTRLVAL(4) = 1 THEN
    DX = X
    DY = Y
    IF CTRLVAL(5) = 1 THEN
      DX = INT(X / 4) * 4
      DY = INT(Y / 4) * 4
    ENDIF
    IF LastX >= 0 THEN
      LINE LastX, LastY, DX, DY, CTRLVAL(9), C_TEAL
    ELSE
      PIXEL DX, DY, C_AMBER
    ENDIF
    LastX = DX
    LastY = DY
  ELSE
    LastX = -1
    LastY = -1
  ENDIF

  IF TIMER - NextAnim >= 750 THEN
    NextAnim = TIMER
    IF CTRLVAL(6) = 1 THEN
      V = V + 5
      IF V > 100 THEN V = 0
      CTRLVAL(13) = V
      CTRLVAL(14) = 100 - V
    ENDIF
  ENDIF

  IF TIMER - NextUi >= 120 THEN
    NextUi = TIMER
    S$ = "R" + STR$(Ref) + " X" + STR$(X) + " Y" + STR$(Y)
    IF S$ <> OldS$ THEN
      CTRLVAL(12) = S$
      OldS$ = S$
    ENDIF
  ENDIF

  PAUSE 1
LOOP

GUI DELETE ALL
CLS
PRINT "GUI_KITCHEN_SINK_DONE"
END

ClearPad:
BOX 300, 250, 320, 210, 1, C_MUTE, C_BG
LINE 300, 250, 620, 460, 1, RGB(40,52,70)
LINE 300, 460, 620, 250, 1, RGB(40,52,70)
LastX = -1
LastY = -1
RETURN
