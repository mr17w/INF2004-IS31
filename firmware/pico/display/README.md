# Display Module

Owner: Lay How

ST7789 display driver / display-task code.

Rule: only the Display Task should directly control the LCD/SPI interface.
Other modules should request display updates through the application's
inter-task communication mechanism.
