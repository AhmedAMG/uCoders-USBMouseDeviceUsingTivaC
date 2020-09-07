/*
 * usb_mouse_structs.h
 *
 *  Created on: May 6, 2020
 *      Author: DELL
 */

#ifndef USB_MOUSE_STRUCTS_H_
#define USB_MOUSE_STRUCTS_H_

extern uint32_t MouseHandler(void *pvCBData,
                                     uint32_t ui32Event,
                                     uint32_t ui32MsgData,
                                     void *pvMsgData);

extern tUSBDHIDMouseDevice g_sMouseDevice;

#endif /* USB_MOUSE_STRUCTS_H_ */
