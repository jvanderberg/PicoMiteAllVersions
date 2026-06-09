' RUN_ARGS: --interp
' t223 — SETPIN special-function config reachability on the host sim.
'
' SETPIN <pin>,I2C0SDA / I2C0SCL / UART0TX / SPI0RX must reach the shared
' config layer (shared/peripheral/i2c_config.c / uart_config.c /
' spi_config.c) through vm_sys_pin_sim.c. The host pin table
' (host_runtime.c PinDef) carries no I2C/UART/SPI capability bits, so the
' locked-in behavior is the shared layer's capability guard: each role
' fails with "Invalid configuration". An unknown mode fails earlier, in
' the command parser, with "Unsupported SETPIN mode" — the two distinct
' errors prove the special-function modes are parsed and dispatched to
' the shared config layer rather than rejected as unknown keywords.
' (The "Already Set to pin" guard and the SETPIN OFF reset sit behind the
' capability guard, so they are exercised on targets whose pin tables
' grant the capabilities, not here.)

SUB expect_err(msg$, what$)
  IF INSTR(MM.ERRMSG$, msg$) = 0 THEN
    ERROR what$ + ": expected [" + msg$ + "] got [" + MM.ERRMSG$ + "]"
  ENDIF
  PRINT what$ + ": ok"
  ON ERROR CLEAR
END SUB

' --- I2C roles hit the shared i2c_config capability guard ---------------
ON ERROR SKIP 1
SETPIN 1, I2C0SDA
expect_err "Invalid configuration", "I2C0SDA"

ON ERROR SKIP 1
SETPIN 2, I2C0SCL
expect_err "Invalid configuration", "I2C0SCL"

ON ERROR SKIP 1
SETPIN 3, I2C1SDA
expect_err "Invalid configuration", "I2C1SDA"

' --- UART / SPI roles hit the same shared mechanism ---------------------
ON ERROR SKIP 1
SETPIN 4, UART0TX
expect_err "Invalid configuration", "UART0TX"

ON ERROR SKIP 1
SETPIN 5, SPI0RX
expect_err "Invalid configuration", "SPI0RX"

' --- unknown mode is rejected by the parser, not the config layer -------
ON ERROR SKIP 1
SETPIN 1, NOTAMODE
expect_err "Unsupported SETPIN mode", "unknown mode"

' --- a failed special-function SETPIN leaves the pin usable -------------
SETPIN 1, DIN
PRINT "pin 1 reads "; PIN(1)
SETPIN 1, OFF
PRINT "t223 ok"
