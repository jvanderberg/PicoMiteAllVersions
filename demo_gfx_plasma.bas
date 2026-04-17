' demo_gfx_plasma.bas -- classic sine-wave plasma effect.
'
' Heavy trig per pixel — use FRUN for real-time animation. Under plain
' RUN (interpreter) each frame takes ~1 s; FRUN drops that to ~30 ms.
' If it feels too fast, launch the sim with --slowdown 2 or so.
' Press any key to exit.

Option Explicit
Const BLK% = 4             ' plot every Nth pixel (bigger = coarser + faster)
Const DT! = 0.25           ' phase advance per frame (bigger = more motion)
Dim W% = MM.HRES, H% = MM.VRES
Dim T! = 0
Dim X%, Y%, R%, G%, B%, C%
Dim V!, CX%, CY%
CX% = W% \ 2 : CY% = H% \ 2

FRAMEBUFFER CREATE
FRAMEBUFFER WRITE F

Do
    For Y% = 0 To H% - 1 Step BLK%
        For X% = 0 To W% - 1 Step BLK%
            V! = Sin((X% - CX%) * 0.06 + T!)
            V! = V! + Sin((Y% - CY%) * 0.05 + T! * 1.3)
            V! = V! + Sin(Sqr((X% - CX%) * (X% - CX%) + (Y% - CY%) * (Y% - CY%)) * 0.08 + T! * 0.7)
            R% = Int((Sin(V! * 2) + 1) * 127)
            G% = Int((Sin(V! * 2 + 2.0944) + 1) * 127)
            B% = Int((Sin(V! * 2 + 4.1888) + 1) * 127)
            C% = RGB(R%, G%, B%)
            BOX X%, Y%, BLK%, BLK%, 0, C%, C%
        Next X%
    Next Y%
    FRAMEBUFFER COPY F, N
    T! = T! + DT!
Loop Until Inkey$ <> ""

FRAMEBUFFER WRITE N
FRAMEBUFFER CLOSE F
