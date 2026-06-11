'PicoVaders — TILEMAP GAME engine variant
'----------------------------------------
'
' Martin Herhaus 2022, V 0.9.8
' Concept based on the Game Space Invaders
'  (c) 1978 by Tomohiro Nishikado of Taito.
'
' Rewritten against the proposed TILEMAP GAME engine API
' (docs/tilemap-game-engine.md).  DOES NOT RUN until that engine lands;
' this file is a design-validation artifact: every engine feature it
' uses must exist, and everything it can't express is an API gap.
'
' Differences from Picovaders_fastgfx.bas:
'  - 4 rows of 11 aliens instead of 5, so the whole cast fits the
'    64-sprite budget: 44 aliens + player + bullet + UFO + 10 bombs
'    + 1 explosion slot = 58.
'  - Aliens march by TILEMAP SPRITE MOVE (position-driven, one alien
'    per frame as in the original); bullet/bombs/UFO fly by engine
'    velocity in px/sec.
'  - All hit detection is engine pair events / tile events.  Bunkers
'    are map tiles with two damage states, chipped via TILEMAP SET.
'  - Explosion freeze-frames use TILEMAP UPDATE 0.
'
'------------------ Engine constants ------------------
Const M_NONE=0, M_DETECT=1, M_BOUNCE=2, M_DESTROY=3, M_STOP=4
Const G_ALIEN=1, G_PLAYER=2, G_BULLET=4, G_BOMB=8, G_UFO=16
Const A_SOLID=1, A_BUNKER=2
'Sprite slots
Const S_PLY=45, S_BLT=46, S_UFO=47, S_BOMB0=48, S_XPL=58
'Atlas tile indices
' 1/2 alien1 A/B   3/4 alien2 A/B   5/6 alien3 A/B
' 7 player   8/9 player explosion   10 alien explosion
' 11 ufo     12 ufo explosion       13 bullet   14 bomb
' 15 bunker full   16 bunker damaged
Const T_PLY=7, T_XPL=10, T_UFO=11, T_UFOX=12, T_BLT=13, T_BOMB=14

'------------------ Vars and Arrays ------------------
Dim alien$(3,2)            '3 alien types x 2 animation frames
Dim alive%(44)             'formation liveness (engine destroys; we book-keep)
Dim atyp%(44)              'alien type 1..3
Dim ply$(3)
Dim bombAlive%(10)
Dim Noise%(200)
Dim snd%(4) = (100,90,85,80,70)

Udir=0:UA%=0:USCR=50:Myst%=0
UfoSndMin%=800:UfoSndMax%=1100:UfoSnd%=800:Ustp%=100
Score%=0:HighScore%=0
adir=1:anr%=44:mvsnd=0:frame%=1
BA=0
plx=153:pvx=0
'Paddle feel: keystroke adds velocity (px/s), per-frame decay, clamp.
PAD_ACCEL=60: PAD_DECAY=0.85: PAD_MAX=180
weiss=RGB(white):Rot=RGB(RED):gelb=RGB(yellow):grn=RGB(green)

'------------------ UDG -> tile atlas ------------------
'Read the classic DATA bitmaps (kept verbatim from the original),
'paint them once as a 16-tile strip, save as BMP, flash-load as the
'tileset.  Tiles are 16x8.
Restore sr1
For al=1 To 3:For f=1 To 2:bitm$="":For n=1 To 16
 Read a:bitm$=bitm$+Chr$(a):Next n
 alien$(al,f)=bitm$:Next f
Next al
For f=1 To 3
  bitm$=""
  For n=1 To 16:Read a:bitm$=bitm$+Chr$(a):Next n:ply$(f)=bitm$
Next f
Restore xpld
xpl$="":For n=1 To 16:Read a:xpl$=xpl$+Chr$(a):Next n
Restore ufo
uf1$="":For n=1 To 16:Read a:uf1$=uf1$+Chr$(a):Next n
uf2$="":For n=1 To 16:Read a:uf2$=uf2$+Chr$(a):Next n
For f=1 To 200:noise%(f)=Int(Rnd*1000):Next f

build_atlas

'------------------ Tilemap world ------------------
'One tilemap doubles as sprite atlas and structure map: 16x8 tiles,
'20x30 cells = 320x240 world.  Bunker cells carry SOLID+BUNKER attrs.
TILEMAP CREATE mapdata, 1, 1, 16, 8, 16, 20, 30
TILEMAP ATTR attrdata, 1, 16

'------------------------------------------------------
coldstart:
plx=153:pvx=0
Y_Pos=48
Intro
'Logical 320x240: 1:1 on pico LCDs, banded on the PicoCalc, 2x on HDMI.
TILEMAP GAME 60, 320, 240
Level%=1
Score%=0
plyers%=3
start:
tic%=1
drops%=0
bmax%=2+Int(Level%/2):If bmax%>10 Then bmax%=10
reset_bunkers
Setup_aliens
make_player
Levelup%=0:GameOver%=0
nxtPlyr:
plHit%=0
clear_shots
'
'------------------ Game Loop ------------------
'
Do
  TILEMAP UPDATE
  '---- engine events: every collision in the game arrives here ----
  For i=1 To TILEMAP(EVENTS)
    Select Case TILEMAP(EVENT i, T)
      Case 1 'sprite pair
        pair_event TILEMAP(EVENT i, A), TILEMAP(EVENT i, B)
      Case 2 'bunker tile touched by bullet or bomb
        chip_bunker TILEMAP(EVENT i, A), TILEMAP(EVENT i, C), TILEMAP(EVENT i, R), TILEMAP(EVENT i, I)
    End Select
  Next i
  '---- BASIC game logic ----
  march_one
  Move_Player
  bounds_check
  If Not (tic% Mod 16) Then drop_bomb
  ufo_logic
  Inc tic%
  If aldeath() Then Levelup%=1:Exit
  If plHit% Then Exit
  If A_Ground% Then Expl_Player:GameOver%=1:Exit
  '---- HUD: engine clears each frame, so redraw every frame ----
  hud
Loop
If plHit% Then
  Expl_Player
  Inc plyers%,-1
  If plyers%=0 Then GameOver%=1:GoTo GmOv
  hold 120
  GoTo nxtPlyr
EndIf
If Levelup%=1 Then
  Inc Level%:hold 120
  If Level%<6 Then Inc Y_Pos,8
  GoTo start
EndIf
GmOv:
If GameOver%=1 Then
  'Banner runs outside the engine: CLOSE presents the final frame and
  'returns the display to direct drawing.
  TILEMAP CLOSE
  Print @(128,100);"PLAYER<1>"
  For f=1 To 10
    Print @(128,116);"GAME OVER"
    If iPause(600) Then Exit For
    Print @(128,116);"         "
    If iPause(600) Then Exit For
  Next f
  GameOver%=0:Level%=1:GoTo coldstart
EndIf
End

'------------------ Event handling ------------------
'
Sub pair_event a%, b%
  'Normalize: lo% gets the smaller slot so each pair has one shape.
  Local lo%, hi%
  lo%=a%:hi%=b%:If lo%>hi% Then lo%=b%:hi%=a%
  If hi%=S_BLT Then
    'bullet vs alien (engine already destroyed both: M_DESTROY)
    If lo%>=1 And lo%<=44 Then
      alive%(lo%)=0
      BA=0
      Inc Score%,40-(10*atyp%(lo%))
      alien_xpl lo%
    EndIf
  ElseIf lo%=S_BLT Then
    Select Case hi%
      Case S_UFO 'bullet vs UFO
        BA=0:UA%=0
        ufo_explode
      Case S_BOMB0 To S_BOMB0+9 'bullet vs bomb: both gone (M_DESTROY)
        BA=0
        bombAlive%(hi%-S_BOMB0+1)=0
    End Select
  ElseIf lo%=S_PLY Then
    If hi%>=S_BOMB0 And hi%<=S_BOMB0+9 Then 'bomb vs player
      bombAlive%(hi%-S_BOMB0+1)=0
      plHit%=1
    EndIf
  EndIf
End Sub
'
Sub chip_bunker who%, c%, r%, t%
  'Two damage states: full(15) -> damaged(16) -> gone(0).
  If t%=15 Then
    TILEMAP SET 1, c%, r%, 16
  Else
    TILEMAP SET 1, c%, r%, 0
  EndIf
  'The shot is already engine-destroyed (SOLID ... M_DESTROY);
  'just settle the books.
  If who%=S_BLT Then BA=0
  If who%>=S_BOMB0 And who%<=S_BOMB0+9 Then bombAlive%(who%-S_BOMB0+1)=0
End Sub

'------------------ Aliens ------------------
'
Sub Setup_aliens
  Local num%, r, n, at, x, y
  A_Ground%=0
  num%=1
  For r=1 To 4
    at=3
    If r=1 Then at=1
    If r=2 Or r=3 Then at=2
    For n=1 To 11
      x=50+n*16: y=Y_Pos+r*16
      TILEMAP SPRITE CREATE num%, 1, (at-1)*2+1, x, y
      TILEMAP SPRITE GROUP num%, G_ALIEN, 0
      TILEMAP SPRITE COLL num%, M_DESTROY
      TILEMAP SPRITE HITBOX num%, 1, 0, 14, 8
      alive%(num%)=1: atyp%(num%)=at
      Inc num%
    Next n
  Next r
  adir=1: anr%=44: frame%=1: turn%=0
End Sub
'
Sub march_one
  'Classic accelerating march: one alien steps per frame, scanned
  'bottom-right to top-left; survivors march faster as ranks thin.
  'Sprites are position-driven: MOVE preserves contact history.
  Local x, y
  Do
    If alive%(anr%) Then
      x=TILEMAP(SPRITE X anr%): y=TILEMAP(SPRITE Y anr%)
      Inc x, adir
      TILEMAP SPRITE MOVE anr%, x, y
      If x>=254 Or x<=51 Then turn%=1
      Inc anr%,-1
      If anr%<1 Then sweep_done
      Exit Sub
    EndIf
    Inc anr%,-1
    If anr%<1 Then sweep_done
  Loop
End Sub
'
Sub sweep_done
  'All survivors moved one step: maybe turn and drop, flip animation
  'frame, advance the four-note march beat.
  Local f, x, y
  anr%=44
  If turn%=1 Then
    adir=-adir: turn%=0
    Inc drops%
    For f=44 To 1 Step -1
      If alive%(f) Then
        x=TILEMAP(SPRITE X f): y=TILEMAP(SPRITE Y f)+8
        TILEMAP SPRITE MOVE f, x, y
        If y>=194 Then A_Ground%=1
      EndIf
    Next f
  EndIf
  Inc mvsnd: mvsnd=mvsnd And 3
  If UA%=0 Then Play TONE snd%(mvsnd+1),snd%(mvsnd+1),80
  Inc frame%: If frame%=3 Then frame%=1
  For f=1 To 44
    If alive%(f) Then TILEMAP SPRITE SET f, (atyp%(f)-1)*2+frame%
  Next f
End Sub
'
Function aldeath()
  Local f, n%
  n%=0
  For f=1 To 44: Inc n%,alive%(f): Next f
  aldeath = (n%=0)
End Function

'------------------ Player / bullet ------------------
'
Sub make_player
  TILEMAP SPRITE CREATE S_PLY, 1, T_PLY, plx, 214
  TILEMAP SPRITE GROUP S_PLY, G_PLAYER, 0
  TILEMAP SPRITE COLL S_PLY, M_DETECT
End Sub
'
Sub Move_Player
  Local fire%, k$
  Do
    k$=Inkey$
    If k$="" Then Exit Do
    If Asc(k$)=27 Then TILEMAP CLOSE: End
    If Asc(k$)=130 Then
      pvx = pvx - PAD_ACCEL
    ElseIf Asc(k$)=131 Then
      pvx = pvx + PAD_ACCEL
    ElseIf k$=" " Or Asc(k$)=13 Or Asc(k$)=10 Then
      fire%=1
    EndIf
  Loop
  If pvx >  PAD_MAX Then pvx =  PAD_MAX
  If pvx < -PAD_MAX Then pvx = -PAD_MAX
  pvx = pvx * PAD_DECAY
  If Abs(pvx) < 12 Then pvx = 0
  'BASIC clamp at the playfield borders (tighter than the world edge)
  plx = TILEMAP(SPRITE X S_PLY)
  If plx < 66  Then TILEMAP SPRITE MOVE S_PLY, 66, 214 : pvx=0 : plx=66
  If plx > 238 Then TILEMAP SPRITE MOVE S_PLY, 238, 214: pvx=0 : plx=238
  TILEMAP SPRITE VEL S_PLY, pvx, 0
  If fire% And BA=0 Then
    If Not UA% Then Inc Myst%,Int(Rnd*3)
    BA=1
    TILEMAP SPRITE CREATE S_BLT, 1, T_BLT, plx, 204
    TILEMAP SPRITE GROUP S_BLT, G_BULLET, G_ALIEN+G_UFO+G_BOMB
    TILEMAP SPRITE COLL S_BLT, M_DESTROY
    TILEMAP SPRITE SOLID S_BLT, A_SOLID, M_DESTROY
    TILEMAP SPRITE EVENTTILES S_BLT, A_BUNKER
    TILEMAP SPRITE HITBOX S_BLT, 7, 1, 2, 6
    TILEMAP SPRITE VEL S_BLT, 0, -120
    For f=1000 To 1 Step -50:Play tone 1000+f,1000+f,5:Next f
  EndIf
End Sub
'
Sub bounds_check
  'Ceiling kills the bullet; the floor kills bombs.
  Local f
  If BA Then
    If TILEMAP(SPRITE Y S_BLT) <= 32 Then
      TILEMAP SPRITE DESTROY S_BLT
      BA=0
    EndIf
  EndIf
  For f=1 To 10
    If bombAlive%(f) Then
      If TILEMAP(SPRITE Y S_BOMB0+f-1) > 224 Then
        TILEMAP SPRITE DESTROY S_BOMB0+f-1
        bombAlive%(f)=0
      EndIf
    EndIf
  Next f
End Sub
'
Sub clear_shots
  Local f
  If BA Then TILEMAP SPRITE DESTROY S_BLT: BA=0
  For f=1 To 10
    If bombAlive%(f) Then TILEMAP SPRITE DESTROY S_BOMB0+f-1: bombAlive%(f)=0
  Next f
End Sub

'------------------ Bombs ------------------
'
Sub drop_bomb
  Local aln%, bn%, n%, ax
  n%=0
  For bn%=1 To 10: Inc n%,bombAlive%(bn%): Next bn%
  If n%>=bmax% Then Exit Sub
  'Scan from the bottom-right survivor for a dropper near the player.
  aln%=45
tna:
  Inc aln%,-1
  If aln%=0 Then Exit Sub
  If Not alive%(aln%) Then GoTo tna
  ax=TILEMAP(SPRITE X aln%)
  If ax<plx-8 Or ax>plx+8 And Int(Rnd*25)>1 Then GoTo tna
  'No live alien below me (next row down = slot+11)
  If aln%<=33 Then If alive%(aln%+11) Then GoTo tna
  For bn%=1 To 10
    If Not bombAlive%(bn%) Then
      TILEMAP SPRITE CREATE S_BOMB0+bn%-1, 1, T_BOMB, ax, TILEMAP(SPRITE Y aln%)+6
      TILEMAP SPRITE GROUP S_BOMB0+bn%-1, G_BOMB, G_PLAYER+G_BULLET
      TILEMAP SPRITE COLL S_BOMB0+bn%-1, M_DESTROY
      TILEMAP SPRITE SOLID S_BOMB0+bn%-1, A_SOLID, M_DESTROY
      TILEMAP SPRITE EVENTTILES S_BOMB0+bn%-1, A_BUNKER
      TILEMAP SPRITE HITBOX S_BOMB0+bn%-1, 7, 0, 3, 8
      TILEMAP SPRITE VEL S_BOMB0+bn%-1, 0, 60
      bombAlive%(bn%)=1
      Exit Sub
    EndIf
  Next bn%
End Sub

'------------------ UFO ------------------
'
Sub ufo_logic
  Local f
  If UA%=0 And Myst%>20 Then
    UA%=1: Myst%=0
    USCR=50
    f=Int(Rnd*10): If f>6 Then USCR=100:If f=9 Then USCR=150
    If Int(Rnd*2)=1 Then
      TILEMAP SPRITE CREATE S_UFO, 1, T_UFO, 254, 32
      TILEMAP SPRITE VEL S_UFO, -30, 0
    Else
      TILEMAP SPRITE CREATE S_UFO, 1, T_UFO, 51, 32
      TILEMAP SPRITE VEL S_UFO, 30, 0
    EndIf
    TILEMAP SPRITE GROUP S_UFO, G_UFO, 0
    TILEMAP SPRITE COLL S_UFO, M_DESTROY
  EndIf
  If UA% Then
    Play tone UfoSnd%,UfoSnd%,150
    Inc UfoSnd%,Ustp%
    If UfoSnd%<=UfoSndMin% Or UfoSnd%>UfoSndMax% Then Ustp%=-Ustp%
    If TILEMAP(SPRITE X S_UFO)<50 Or TILEMAP(SPRITE X S_UFO)>254 Then
      TILEMAP SPRITE DESTROY S_UFO
      UA%=0
    EndIf
  EndIf
  'Score flash after a UFO kill
  If uscrShow% Then
    Text uscrX%, 32, Str$(USCR)
    Inc uscrShow%,-1
    If uscrShow%=0 Then Inc Score%,USCR
  EndIf
End Sub
'
Sub ufo_explode
  Local n
  uscrX%=TILEMAP(SPRITE X S_UFO)
  TILEMAP SPRITE CREATE S_XPL, 1, T_UFOX, uscrX%, 32
  For n=1 To 12
    TILEMAP UPDATE 0
    Play tone 900+15*n,900+15*n,30
    hud
  Next n
  TILEMAP SPRITE DESTROY S_XPL
  uscrShow%=60 'show the score text for a second; then bank it
End Sub

'------------------ Explosions (world-freeze) ------------------
'
Sub alien_xpl who%
  'Freeze the world, show the burst where the alien died, play noise.
  'NOTE: reads the position of a sprite the engine just destroyed —
  'the engine must keep SPRITE X/Y readable (last position) after
  'deactivation, or pair events need x/y fields.  API finding.
  Local n, x, y
  x=TILEMAP(SPRITE X who%): y=TILEMAP(SPRITE Y who%)
  TILEMAP SPRITE CREATE S_XPL, 1, T_XPL, x, y
  For n=1 To 5
    TILEMAP UPDATE 0
    Play tone noise%(n*15),noise%(n*15),16
    hud
  Next n
  Play tone 0,0,1
  TILEMAP SPRITE DESTROY S_XPL
End Sub
'
Sub Expl_Player
  Local f, n
  For f=1 To 3
    TILEMAP SPRITE SET S_PLY, 8
    For n=1 To 6: TILEMAP UPDATE 0: Play tone noise%(n*16),noise%(n*16),16: hud: Next n
    TILEMAP SPRITE SET S_PLY, 9
    For n=1 To 6: TILEMAP UPDATE 0: Play tone noise%(100+n*16),noise%(100+n*16),16: hud: Next n
  Next f
  Play tone 0,0,1
  TILEMAP SPRITE SET S_PLY, T_PLY
  hold 30
End Sub
'
Sub hold frames%
  'Present unchanged world for N frames (engine-paced wait).
  Local n
  For n=1 To frames%: TILEMAP UPDATE 0: hud: Next n
End Sub

'------------------ HUD ------------------
'
Sub hud
  Box 50,229,220,1,,,grn
  Print @(58,0);"SCORE <1>  HI-SCORE"
  Print @(66,16);:Pscore Score%
  If Score%>HighScore% Then HighScore%=Score%
  Print @(154,16);:PScore HighScore%
  Print @(46,230);Level%;
  Box 72,232,40,8,1,0,0
  If plyers%>1 Then GUI BITMAP 72,232,ply$(1),16,8,1,grn,0
  If plyers%=3 Then GUI BITMAP 88,232,ply$(1),16,8,1,grn,0
End Sub
'
Sub PScore wert
  If wert<1000 Then Print "0";
  If wert<100 Then Print "0";
  If wert<10 Then Print "0";
  Print Str$(wert);
End Sub

'------------------ Bunkers ------------------
'
Sub reset_bunkers
  'Map carries the bunkers: 4 bunkers x 2 cols x 2 rows of full tiles.
  Local b, c, r
  For r=23 To 24
    For c=0 To 19
      TILEMAP SET 1, c, r, 0
    Next c
  Next r
  For b=0 To 3
    For c=0 To 1
      For r=23 To 24
        TILEMAP SET 1, 5+b*3+c, r, 15
      Next r
    Next c
  Next b
End Sub

'------------------ Atlas build ------------------
'
Sub build_atlas
  'Paint the 16-tile strip (16x8 each), save, flash-load as tileset 1.
  Local x
  CLS
  GUI BITMAP 0,0,alien$(1,1),16,8,1,weiss,0
  GUI BITMAP 16,0,alien$(1,2),16,8,1,weiss,0
  GUI BITMAP 32,0,alien$(2,1),16,8,1,weiss,0
  GUI BITMAP 48,0,alien$(2,2),16,8,1,weiss,0
  GUI BITMAP 64,0,alien$(3,1),16,8,1,weiss,0
  GUI BITMAP 80,0,alien$(3,2),16,8,1,weiss,0
  GUI BITMAP 96,0,ply$(1),16,8,1,grn,0
  GUI BITMAP 112,0,ply$(2),16,8,1,grn,0
  GUI BITMAP 128,0,ply$(3),16,8,1,grn,0
  GUI BITMAP 144,0,xpl$,16,8,1,gelb,0
  GUI BITMAP 160,0,uf1$,16,8,1,Rot,0
  GUI BITMAP 176,0,uf2$,16,8,1,Rot,0
  Line 199,1,199,6,,weiss: Line 200,1,200,6,,weiss      'tile 13: bullet
  x=215: Line x,0,x+1,2,,gelb: Line x+1,3,x,5,,gelb: Line x,6,x+1,7,,gelb 'tile 14: bomb
  Box 224,0,16,8,,grn,grn                               'tile 15: bunker full
  Box 240,0,16,8,,grn                                   'tile 16: damaged
  Box 242,2,4,2,,grn,grn: Box 250,4,4,2,,grn,grn
  Save Image "vaders_tiles.bmp",0,0,256,8
  FLASH LOAD IMAGE 1, "vaders_tiles.bmp"
  CLS
End Sub

'------------------ Intro ------------------
'
Sub Intro
  CLS
  Box 50,229,220,1,,,grn
  Print @(122,50) "PICOVADERS"
  Print @(74,70) "*SCORE ADVANCE TABLE*"
  GUI BITMAP 104,88,uf1$,16,8,1,Rot,0
  Print @(130,88) "= ? MYSTERY"
  GUI BITMAP 104,108,alien$(1,1),16,8,1,weiss,0
  Print @(130,108) "=30 POINTS"
  GUI BITMAP 104,128,alien$(2,1),16,8,1,weiss,0
  Print @(130,128) "=20 POINTS"
  GUI BITMAP 104,148,alien$(3,1),16,8,1,weiss,0
  Print @(130,148) "=10 POINTS"
  Print @(90,170) "(C) 1978 BY TAITO"
  Print @(74,190) "2022 BY MARTIN HERHAUS"
  Print @(58,210) "TILEMAP ENGINE VARIANT"
  Do
    Print @(74,225) "  PRESS ANY KEY TO START "
    If iPause(600) Then Exit Sub
    Print @(82,225) "                         "
    If iPause(600) Then Exit Sub
  Loop
End Sub
'
Function iPause(ms)
  Local k$, t
  t = Timer + ms
  iPause = 0
  Do
    k$ = Inkey$
    If k$ <> "" Then
      If Asc(k$) = 27 Then End
      iPause = 1
      Exit Function
    EndIf
    If Timer >= t Then Exit Function
    Pause 5
  Loop
End Function

'------------------ Map + attribute DATA ------------------
'20 cols x 30 rows, all empty: bunkers are placed by reset_bunkers
'each level so damage heals between rounds.
mapdata:
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
'
'Tile attributes: only the bunker tiles are SOLID+BUNKER (=3).
attrdata:
Data 0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3
'
'------------------ UDG DATA (verbatim from the original) ------------------
sr1:
Data  1, 128, 3, 192, 7, 224, 13, 176, 15, 240, 5, 160, 8, 16, 4, 32
Data  1, 128, 3, 192, 7, 224, 13, 176, 15, 240, 2, 64, 5, 160, 10, 80

sr2:
Data  8, 32, 4, 64, 15, 224, 27, 176, 63, 248, 47, 232, 40, 40, 6, 192
Data  8, 32, 36, 72, 47, 232, 59, 184, 63, 248, 31, 240, 8, 32, 16, 16

sr3:
Data  7, 224, 31, 248, 63, 252, 57, 156, 63, 252, 14, 112, 25, 152, 48, 12
Data  7, 224, 31, 248, 63, 252, 57, 156, 63, 252, 14, 112, 25, 152, 12, 48

plyr:
Data  1, 0, 3, 128, 3, 128, 63, 248, 127, 252, 127, 252, 127, 252, 127, 252
Data  2, 0, 0, 16, 2, 160, 18, 0, 1, 176, 69, 168, 31, 228, 63, 245
Data  16, 4, 130, 25, 16, 192, 2, 2, 75, 97, 33, 196, 31, 224, 55, 228

xpld:
Data  4, 64, 34, 136, 16, 16, 8, 32, 96, 12, 8, 32, 18, 144, 36, 72

ufo:
Data  0, 0, 7, 224, 31, 248, 63, 252, 109, 182, 255, 255, 57, 156, 16, 8
ufo_xpl:
Data  148, 10, 64, 48, 143, 24, 31, 206, 58, 167, 143, 140, 5, 24, 136, 136
