/*
    ChibiOS - Copyright (C) 2006..2016 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
 */

#include <ch.h>
#include <hal.h>
#include <chprintf.h>
#include <ch_test.h>

#include "usbconf.h"
#include "cmd.h"

static THD_WORKING_AREA(wablink, 128);
static THD_FUNCTION(blink, arg) {

    (void) arg;
    chRegSetThreadName(__FUNCTION__);

    while (true) {
        palSetLine(LINE_LED2_GREEN);
        chThdSleepMilliseconds(500);
        palClearLine(LINE_LED2_GREEN);
        chThdSleepMilliseconds(500);
    }
}

int main(void)
{
    halInit();
    chSysInit();

    /*
     * ARD_D13 is programmed as output (board LED).
     */
    palClearLine(LINE_ARD_D13);
    palSetLineMode(LINE_ARD_D13, PAL_MODE_OUTPUT_PUSHPULL);

    /*
     * Activates the serial driver 1 using the driver default configuration.
     */
    sdStart(&SD1, NULL);

    /*
     * Creates the example thread.
     */
    chThdCreateStatic(wablink, sizeof(wablink), NORMALPRIO, blink, NULL);

    /* Starts the virtual serial port. */
    usb_start();
    shell_start();

    /*
     * Normal main() thread activity, in this demo it does nothing except
     * sleeping in a loop and check the button state.
     */
    while (true) {
        chThdSleepMilliseconds(500);
    }
}
