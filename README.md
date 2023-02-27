# Picoprobe
Picoprobe allows a Pico / RP2040 to be used as USB -> SWD and UART bridge. This means it can be used as a debugger and serial console for another Pico.

# Picoprobe for Seeed Xiao
The xiao branch of this fork ports the Picoprobe firmware to Seeed Xiao board.
Differences:  
because the xiao doesn't break out GP4 and GP5, the UART settings are moved to GP0 and GP1:
```c++
#define PICOPROBE_UART_TX 0
#define PICOPROBE_UART_RX 1
#define PICOPROBE_UART_INTERFACE uart0  
```

the board is set to XIAO
```cmake
set(PICO_BOARD seeed_xiao_rp2040)
```

# Documentation
Picoprobe documentation can be found in the [Pico Getting Started Guide](https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf). See "Appendix A: Using Picoprobe".
