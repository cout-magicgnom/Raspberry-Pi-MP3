#include <stdio.h>
#include <pigpio.h>

#define BTN_PLAY 27
#define BTN_NEXT 80
#define BTN_PREV 72

gpioSetMode(BTN_PLAY, PI_INPUT);
gpioSetMode(BTN_NEXT, PI_INPUT);
gpioSetMode(BTN_PREV, PI_INPUT);

