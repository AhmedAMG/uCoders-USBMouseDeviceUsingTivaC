/*
 * usb_dev_mouse.c
 *
 *  Created on: May 6, 2020
 *      Author: DELL
 */

//*****************************************************************************
//
// External declarations for the interrupt handlers used by the application.
//
//*****************************************************************************


#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_gpio.h"
#include "inc/hw_sysctl.h"
#include "driverlib/debug.h"
#include "driverlib/fpu.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/systick.h"
#include "driverlib/uart.h"
#include "usblib/usblib.h"
#include "usblib/usbhid.h"
#include "usblib/device/usbdevice.h"
#include "usblib/device/usbdhid.h"
#include "usblib/device/usbdhidmouse.h"
#include "drivers/buttons.h"
#include "utils/uartstdio.h"
#include "utils/ustdlib.h"
#include "usb_mouse_structs.h"



//*****************************************************************************
//
// The system tick timer period.
//
//*****************************************************************************
#define SYSTICKS_PER_SECOND     100

//*****************************************************************************
//
// This global indicates whether or not we are connected to a USB host.
//
//*****************************************************************************
volatile bool g_bConnected = false;

//*****************************************************************************
//
// This global indicates whether or not the USB bus is currently in the suspend
// state.
//
//*****************************************************************************
volatile bool g_bSuspended = false;

//*****************************************************************************
//
// Global system tick counter holds elapsed time since the application started
// expressed in 100ths of a second.
//
//*****************************************************************************
volatile uint32_t g_ui32SysTickCount;

//*****************************************************************************
//
// The number of system ticks to wait for each USB packet to be sent before
// we assume the host has disconnected.  The value 50 equates to half a second.
//
//*****************************************************************************
#define MAX_SEND_DELAY          50

//*****************************************************************************
//
// This global is set to true if the host sends a request to set or clear
// any keyboard LED.
//
//*****************************************************************************
//volatile bool g_bDisplayUpdateRequired;

//*****************************************************************************
//
// This enumeration holds the various states that the mouse can be in during
// normal operation.
//
//*****************************************************************************
volatile enum
{
    //
    // Unconfigured.
    //
    STATE_UNCONFIGURED,

    //
    // No keys to send and not waiting on data.
    //
    STATE_IDLE,

    //
    // Waiting on data to be sent out.
    //
    STATE_SENDING
}
g_eMouseState = STATE_UNCONFIGURED;

//*****************************************************************************
//
// The error routine that is called if the driver library encounters an error.
//
//*****************************************************************************
#ifdef DEBUG
void
__error__(char *pcFilename, uint32_t ui32Line)
{
}
#endif

//*****************************************************************************
//
// Handles asynchronous events from the HID Mouse driver.
//
// \param pvCBData is the event callback pointer provided during
// USBDHIDMouseInit().  This is a pointer to our mouse device structure
// (&g_sMouseDevice).
// \param ui32Event identifies the event we are being called back for.
// \param ui32MsgData is an event-specific value.
// \param pvMsgData is an event-specific pointer.
//
// This function is called by the HID mouse driver to inform the application
// of particular asynchronous events related to operation of the mouse HID
// device.
//
// \return Returns 0 in all cases.
//
//*****************************************************************************
uint32_t
MouseHandler(void *pvCBData, uint32_t ui32Event, uint32_t ui32MsgData,
                void *pvMsgData)
{
    switch (ui32Event)
    {
        //
        // The host has connected to us and configured the device.
        //
        case USB_EVENT_CONNECTED:
        {
            g_bConnected = true;
            g_bSuspended = false;
            break;
        }

        //
        // The host has disconnected from us.
        //
        case USB_EVENT_DISCONNECTED:
        {
            g_bConnected = false;
            break;
        }

        //
        // We receive this event every time the host acknowledges transmission
        // of a report.  It is used here purely as a way of determining whether
        // the host is still talking to us or not.
        //
        case USB_EVENT_TX_COMPLETE:
        {
            //
            // Enter the idle state since we finished sending something.
            //
            g_eMouseState = STATE_IDLE;
            break;
        }

        //
        // This event indicates that the host has suspended the USB bus.
        //
        case USB_EVENT_SUSPEND:
        {
            g_bSuspended = true;
            break;
        }

        //
        // This event signals that the host has resumed signaling on the bus.
        //
        case USB_EVENT_RESUME:
        {
            g_bSuspended = false;
            break;
        }

        //
        // We ignore all other events.
        //
        default:
        {
            break;
        }
    }

    return(0);
}

//***************************************************************************
//
// Wait for a period of time for the state to become idle.
//
// \param ui32TimeoutTick is the number of system ticks to wait before
// declaring a timeout and returning \b false.
//
// This function polls the current mouse state for ui32TimeoutTicks system
// ticks waiting for it to become idle.  If the state becomes idle, the
// function returns true.  If it ui32TimeoutTicks occur prior to the state
// becoming idle, false is returned to indicate a timeout.
//
// \return Returns \b true on success or \b false on timeout.
//
//***************************************************************************
bool
WaitForSendIdle(uint_fast32_t ui32TimeoutTicks)
{
    uint32_t ui32Start;
    uint32_t ui32Now;
    uint32_t ui32Elapsed;

    ui32Start = g_ui32SysTickCount;
    ui32Elapsed = 0;

    while(ui32Elapsed < ui32TimeoutTicks)
    {
        //
        // Is the mouse is idle, return immediately.
        //
        if(g_eMouseState == STATE_IDLE)
        {
            return(true);
        }

        //
        // Determine how much time has elapsed since we started waiting.  This
        // should be safe across a wrap of g_ui32SysTickCount.
        //
        ui32Now = g_ui32SysTickCount;
        ui32Elapsed = ((ui32Start < ui32Now) ? (ui32Now - ui32Start) :
                     (((uint32_t)0xFFFFFFFF - ui32Start) + ui32Now + 1));
    }

    //
    // If we get here, we timed out so return a bad return code to let the
    // caller know.
    //
    return(false);
}


//*****************************************************************************
//
// Configure the UART and its pins.  This must be called before UARTprintf().
//
//*****************************************************************************
void
ConfigureUART(void)
{
    //
    // Enable the GPIO Peripheral used by the UART.
    //
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);

    //
    // Enable UART0.
    //
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);

    //
    // Configure GPIO Pins for UART mode.
    //
    MAP_GPIOPinConfigure(GPIO_PA0_U0RX);
    MAP_GPIOPinConfigure(GPIO_PA1_U0TX);
    MAP_GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    //
    // Use the internal 16MHz oscillator as the UART clock source.
    //
    UARTClockSourceSet(UART0_BASE, UART_CLOCK_PIOSC);

    //
    // Initialize the UART for console I/O.
    //
    UARTStdioConfig(0, 115200, 16000000);
}


//*****************************************************************************
//
// This is the interrupt handler for the SysTick interrupt.  It is used to
// update our local tick count which, in turn, is used to check for transmit
// timeouts.
//
//*****************************************************************************
void
SysTickIntHandler(void)
{
    g_ui32SysTickCount++;
}


//*****************************************************************************
//
// Conduct a click and send data via the USB HID keyboard interface.
//
//*****************************************************************************


void Move(uint8_t button){
    if(button == RIGHT_BUTTON){
        //
        // Send the mouse click message.
        //
        g_eMouseState = STATE_SENDING;
        if(USBDHIDMouseStateChange(&g_sMouseDevice, 10, 0, 0) != MOUSE_SUCCESS)
        {
            UARTprintf("1\n\r");
            return;
        }
    }else if(button == LEFT_BUTTON){
        //
        // Send the mouse click message.
        //
        g_eMouseState = STATE_SENDING;
        if(USBDHIDMouseStateChange(&g_sMouseDevice, -10, 0, 0) != MOUSE_SUCCESS)
        {
            UARTprintf("2\n\r");
            return;
        }
    }else{
        return;
    }


    //
    // Wait until the mouse click message has been sent.
    //
    if(!WaitForSendIdle(MAX_SEND_DELAY))
    {
        UARTprintf("Done\n\r");
        g_bConnected = 0;
        return;
    }


}



int main(void){
    uint_fast32_t ui32LastTickCount;
    bool bLastSuspend;

    //
    // Enable lazy stacking for interrupt handlers.  This allows floating-point
    // instructions to be used within interrupt handlers, but at the expense of
    // extra stack usage.
    //
    MAP_FPULazyStackingEnable();

    //
    // Set the clocking to run from the PLL at 50MHz.
    //
    MAP_SysCtlClockSet(SYSCTL_SYSDIV_4 | SYSCTL_USE_PLL | SYSCTL_OSC_MAIN |
                       SYSCTL_XTAL_16MHZ);

    //
    // Initialize the UART and display initial message.
    //
    ConfigureUART();
    UARTprintf("usb-dev-mouse my example\n\r");

    //
    // Configure the required pins for USB operation.
    //
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOD);
    MAP_GPIOPinTypeUSBAnalog(GPIO_PORTD_BASE, GPIO_PIN_4 | GPIO_PIN_5);

    //
    // Erratum workaround for silicon revision A1.  VBUS must have pull-down.
    //
    if(CLASS_IS_TM4C123 && REVISION_IS_A1)
    {
        HWREG(GPIO_PORTB_BASE + GPIO_O_PDR) |= GPIO_PIN_1;
    }

    //
    // Enable the GPIO that is used for the on-board LED.
    //
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
    MAP_GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_2);
    MAP_GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_2, 0);

    //
    // Initialize the buttons driver.
    //
    ButtonsInit();

    //
    // Not configured initially.
    //
    g_bConnected = false;
    g_bSuspended = false;
    bLastSuspend = false;

    //
    // Initialize the USB stack for device mode.  We do not operate in USB
    // device mode with active monitoring of VBUS and therefore, we will
    // specify eUSBModeForceDevice as the operating mode instead of
    // eUSBModeDevice.  To use eUSBModeDevice, the EK-TM4C123GXL LaunchPad
    // must have the R28 and R29 populated with zero ohm resistors.
    //
    USBStackModeSet(0, eUSBModeForceDevice, 0);

    //
    // Pass our device information to the USB HID device class driver,
    // initialize the USB controller, and connect the device to the bus.
    //
    USBDHIDMouseInit(0, &g_sMouseDevice);

    //
    // Set the system tick to fire 100 times per second.
    //
    MAP_SysTickPeriodSet(MAP_SysCtlClockGet() / SYSTICKS_PER_SECOND);
    MAP_SysTickIntEnable();
    MAP_SysTickEnable();

    //
    // The main loop starts here.  We begin by waiting for a host connection
    // then drop into the main mouse handling section.  If the host
    // disconnects, we return to the top and wait for a new connection.
    //
    while(1)
    {
        uint8_t ui8Buttons;
        uint8_t ui8ButtonsChanged;

        UARTprintf("Waiting for host...\n\r");

        //
        // Wait here until USB device is connected to a host.
        //
        while(!g_bConnected)
        {
        }

        UARTprintf("Host connected.\n\r");
        UARTprintf("Now press any button.\n\r");

        //
        // Enter the idle state.
        //
        g_eMouseState = STATE_IDLE;

        //
        // Assume that the bus is not currently suspended if we have just been
        // configured.
        //
        bLastSuspend = false;




        while(g_bConnected)
        {
            //
            // Remember the current time.
            //
            ui32LastTickCount = g_ui32SysTickCount;

            //
            // Has the suspend state changed since last time we checked?
            //
            if(bLastSuspend != g_bSuspended)
            {
                //
                // Yes, update the state on the terminal.
                //
                bLastSuspend = g_bSuspended;
                if(bLastSuspend)
                {
                    UARTprintf("Bus suspended ...\n\r");
                }
                else
                {
                    UARTprintf("Host connected ...\n\r");
                }
            }

            //
            // See if the button was just pressed.
            //
            ui8Buttons = ButtonsPoll(&ui8ButtonsChanged, 0);
            if(BUTTON_PRESSED(LEFT_BUTTON, ui8Buttons,
                              ui8ButtonsChanged))
            {
                //
                // If the bus is suspended then resume it.  Otherwise, type
                // out an instructional message.
                //
                if(g_bSuspended)
                {
                    USBDHIDMouseRemoteWakeupRequest(
                                                   (void *)&g_sMouseDevice);
                }
                else
                {
                    Move(LEFT_BUTTON);
                }
            }
            else if(BUTTON_PRESSED(RIGHT_BUTTON, ui8Buttons,
                                   ui8ButtonsChanged))
            {
                //
                // If the bus is suspended then resume it.  Otherwise, type
                // out an instructional message.
                //
                if(g_bSuspended)
                {
                    USBDHIDMouseRemoteWakeupRequest(
                                                   (void *)&g_sMouseDevice);
                }
                else
                {
                    Move(RIGHT_BUTTON);
                }
            }

            //
            // Wait for at least 1 system tick to have gone by before we poll
            // the buttons again.
            //
            while(g_ui32SysTickCount == ui32LastTickCount)
            {
            }
        }
    }















    return 0;
}



