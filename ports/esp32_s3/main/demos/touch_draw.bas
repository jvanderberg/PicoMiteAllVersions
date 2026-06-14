' touch_draw.bas - simple touch drawing pad
DIM COL%(8)
COL%(1)=RGB(BLACK)
COL%(2)=RGB(RED)
COL%(3)=RGB(YELLOW)
COL%(4)=RGB(GREEN)
COL%(5)=RGB(CYAN)
COL%(6)=RGB(BLUE)
COL%(7)=RGB(MAGENTA)
COL%(8)=RGB(WHITE)
W%=MM.HRES
H%=MM.VRES
BAR%=32
CLX%=W%-58
SW%=(CLX%-2)\8
PEN%=COL%(1)
SEL%=1
LX%=-1
LY%=-1
UIHELD%=0
CLS RGB(WHITE)
GOSUB DRAWUI
DO
  K$=INKEY$
  IF K$<>"" THEN END
  IF TOUCH(DOWN) THEN
    X%=TOUCH(X)
    Y%=TOUCH(Y)
    IF X%>=0 AND Y%>=0 THEN
      IF Y%<BAR% THEN
        IF UIHELD%=0 THEN
          IF X%>=CLX% THEN
            GOSUB CLEARPAD
          ELSE
            FOR I%=1 TO 8
              IF X%>=(I%-1)*SW% AND X%<I%*SW% THEN PEN%=COL%(I%):SEL%=I%:GOSUB DRAWUI
            NEXT I%
          ENDIF
        ENDIF
        UIHELD%=1
        LX%=-1
        LY%=-1
      ELSE
        UIHELD%=0
        IF LX%<0 THEN
          PIXEL X%,Y%,PEN%
        ELSE
          LINE LX%,LY%,X%,Y%,2,PEN%
        ENDIF
        LX%=X%
        LY%=Y%
      ENDIF
    ENDIF
  ELSE
    UIHELD%=0
    LX%=-1
    LY%=-1
  ENDIF
  PAUSE 8
LOOP
DRAWUI:
BOX 0,0,W%,BAR%,0,,RGB(200,200,200)
FOR I%=1 TO 8
  X0%=(I%-1)*SW%
  BOX X0%+2,3,SW%-4,BAR%-7,1,RGB(BLACK),COL%(I%)
  IF I%=SEL% THEN BOX X0%+1,2,SW%-2,BAR%-5,2,RGB(WHITE)
NEXT I%
BOX CLX%,2,W%-CLX%-2,BAR%-5,1,RGB(WHITE),RGB(RED)
TEXT CLX%+7,9,"CLEAR","LT",1,1,RGB(WHITE),RGB(RED)
RETURN
CLEARPAD:
BOX 0,BAR%,W%,H%-BAR%,0,,RGB(WHITE)
GOSUB DRAWUI
RETURN
