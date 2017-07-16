/*
             LUFA Library
     Copyright (C) Dean Camera, 2017.

  dean [at] fourwalledcubicle [dot] com
           www.lufa-lib.org
*/

/*
  Copyright 2017  Dean Camera (dean [at] fourwalledcubicle [dot] com)

  Permission to use, copy, modify, distribute, and sell this
  software and its documentation for any purpose is hereby granted
  without fee, provided that the above copyright notice appear in
  all copies and that both that the copyright notice and this
  permission notice and warranty disclaimer appear in supporting
  documentation, and that the name of the author not be used in
  advertising or publicity pertaining to distribution of the
  software without specific, written prior permission.

  The author disclaims all warranties with regard to this
  software, including all implied warranties of merchantability
  and fitness.  In no event shall the author be liable for any
  special, indirect or consequential damages or any damages
  whatsoever resulting from loss of use, data or profits, whether
  in an action of contract, negligence or other tortious action,
  arising out of or in connection with the use or performance of
  this software.
*/

/** \file
 *
 *  Main source file for the USBtoSerial project. This file contains the main tasks of
 *  the project and is responsible for the initial application hardware configuration.
 */

#include "USBtoSerial.h"


/** enforce flow control */
//#define ENFORCE_FLOW

#ifndef LEDMASK_RX
/** LED mask for receving indicator */
#define LEDMASK_RX		 LEDS_NO_LEDS
#endif // ! LEDMASK_RX

#ifndef LEDMASK_TX
/** LED mask for transmitting indicator */
#define LEDMASK_TX		 LEDS_NO_LEDS
#endif // ! LEDMASK_TX

#ifndef LEDMASK_DSR
/** LED mask for flow input indicator */
#define LEDMASK_DSR		 LEDS_NO_LEDS
#endif // ! LEDMASK_DSR

#ifndef LEDMASK_DTR
/** LED mask for flow output indicator */
#define LEDMASK_DTR		 LEDS_NO_LEDS
#endif // ! LEDMASK_DTR


/* PortD */
#define FLOW_IN (1 << 7)
#define FLOW_OUT (1 << 6)

#define CONTROL_LINE_OUT CDC_CONTROL_LINE_OUT_DTR
//#define CONTROL_LINE_OUT CDC_CONTROL_LINE_OUT_RTS

#define CONTROL_LINE_IN CDC_CONTROL_LINE_IN_DSR

/** Circular buffer to hold data from the host before it is sent to the device via the serial port. */
static RingBuffer_t USBtoUSART_Buffer;

/** Underlying data buffer for \ref USBtoUSART_Buffer, where the stored bytes are located. */
static uint8_t      USBtoUSART_Buffer_Data[128];

/** Circular buffer to hold data from the serial port before it is sent to the host. */
static RingBuffer_t USARTtoUSB_Buffer;

/** Underlying data buffer for \ref USARTtoUSB_Buffer, where the stored bytes are located. */
static uint8_t      USARTtoUSB_Buffer_Data[128];

/** LUFA CDC Class driver interface configuration and state information. This structure is
 *  passed to all CDC Class driver functions, so that multiple instances of the same class
 *  within a device can be differentiated from one another.
 */
USB_ClassInfo_CDC_Device_t VirtualSerial_CDC_Interface =
	{
		.Config =
			{
				.ControlInterfaceNumber         = INTERFACE_ID_CDC_CCI,
				.DataINEndpoint                 =
					{
						.Address                = CDC_TX_EPADDR,
						.Size                   = CDC_TXRX_EPSIZE,
						.Banks                  = 1,
					},
				.DataOUTEndpoint                =
					{
						.Address                = CDC_RX_EPADDR,
						.Size                   = CDC_TXRX_EPSIZE,
						.Banks                  = 1,
					},
				.NotificationEndpoint           =
					{
						.Address                = CDC_NOTIFICATION_EPADDR,
						.Size                   = CDC_NOTIFICATION_EPSIZE,
						.Banks                  = 1,
					},
			},
	};


/** Main program entry point. This routine contains the overall program flow, including initial
 *  setup of all components and the main program loop.
 */
int main(void)
{
	SetupHardware();

	RingBuffer_InitBuffer(&USBtoUSART_Buffer, USBtoUSART_Buffer_Data, sizeof(USBtoUSART_Buffer_Data));
	RingBuffer_InitBuffer(&USARTtoUSB_Buffer, USARTtoUSB_Buffer_Data, sizeof(USARTtoUSB_Buffer_Data));

	LEDs_SetAllLEDs(LEDMASK_USB_NOTREADY);
	GlobalInterruptEnable();

	uint16_t   oldDeviceToHost=VirtualSerial_CDC_Interface.State.ControlLineStates.DeviceToHost;

	for (;;)
	{

		CDC_Device_USBTask(&VirtualSerial_CDC_Interface);
		USB_USBTask();

		if (USB_DeviceState != DEVICE_STATE_Configured)
			continue;

		/* Only try to read in bytes from the CDC interface if the transmit buffer is not full */
		if (!(RingBuffer_IsFull(&USBtoUSART_Buffer)))
		{
			int16_t ReceivedByte = CDC_Device_ReceiveByte(&VirtualSerial_CDC_Interface);

			/* Store received byte into the USART transmit buffer */
			if (!(ReceivedByte < 0))
			  RingBuffer_Insert(&USBtoUSART_Buffer, ReceivedByte);
		}

		uint16_t BufferCount = RingBuffer_GetCount(&USARTtoUSB_Buffer);
		if (BufferCount)
		{
			Endpoint_SelectEndpoint(VirtualSerial_CDC_Interface.Config.DataINEndpoint.Address);

			/* Check if a packet is already enqueued to the host - if so, we shouldn't try to send more data
			 * until it completes as there is a chance nothing is listening and a lengthy timeout could occur */
			if (Endpoint_IsINReady())
			{
				/* Never send more than one bank size less one byte to the host at a time, so that we don't block
				 * while a Zero Length Packet (ZLP) to terminate the transfer is sent if the host isn't listening */
				uint8_t BytesToSend = MIN(BufferCount, (CDC_TXRX_EPSIZE - 1));

				/* Read bytes from the USART receive buffer into the USB IN endpoint */
				while (BytesToSend--)
				{
					/* Try to send the next byte of data to the host, abort if there is an error without dequeuing */
					if (CDC_Device_SendByte(&VirtualSerial_CDC_Interface,
											RingBuffer_Peek(&USARTtoUSB_Buffer)) != ENDPOINT_READYWAIT_NoError)
					{
						break;
					}

					/* Dequeue the already sent byte from the buffer now we have confirmed that no transmission error occurred */
					RingBuffer_Remove(&USARTtoUSB_Buffer);
				}
			}
		}

		if( ! RingBuffer_IsEmpty(&USARTtoUSB_Buffer) )
			LEDs_TurnOnLEDs(LEDMASK_RX);
		else
			LEDs_TurnOffLEDs(LEDMASK_RX);

		if( ! RingBuffer_IsEmpty(&USBtoUSART_Buffer) )
			LEDs_TurnOnLEDs(LEDMASK_TX);
		else
			LEDs_TurnOffLEDs(LEDMASK_TX);

#ifdef ENFORCE_FLOW
		/* host not keeping up? */
		if( RingBuffer_GetFreeCount(&USARTtoUSB_Buffer) < 16 && (PORTD & FLOW_OUT) == 0 )
		{
			PORTD |= FLOW_OUT;
			LEDs_TurnOffLEDs(LEDMASK_DTR);
		}

		if( RingBuffer_GetCount(&USARTtoUSB_Buffer) < 16 && (PORTD & FLOW_OUT) != 0 && (VirtualSerial_CDC_Interface.State.ControlLineStates.HostToDevice & CONTROL_LINE_OUT) != 0 )
		{
			PORTD &= ~FLOW_OUT;
			LEDs_TurnOnLEDs(LEDMASK_DTR);
		}

		/* only send when DSR is ready */
		if( (VirtualSerial_CDC_Interface.State.ControlLineStates.DeviceToHost & CONTROL_LINE_IN) != 0 )
#endif /* ENFORCE_FLOW */
		/* Load the next byte from the USART transmit buffer into the USART if transmit buffer space is available */
		if (Serial_IsSendReady() && !(RingBuffer_IsEmpty(&USBtoUSART_Buffer)))
			Serial_SendByte(RingBuffer_Remove(&USBtoUSART_Buffer));

		/* Inform the host if any control line states changed */
		if( VirtualSerial_CDC_Interface.State.ControlLineStates.DeviceToHost != oldDeviceToHost )
		{
			CDC_Device_SendControlLineStateChange(&VirtualSerial_CDC_Interface);
			oldDeviceToHost = VirtualSerial_CDC_Interface.State.ControlLineStates.DeviceToHost;
		}

	}
}

/** Configures the board hardware and chip peripherals for the demo's functionality. */
void SetupHardware(void)
{
#if (ARCH == ARCH_AVR8)
	/* Disable watchdog if enabled by bootloader/fuses */
	MCUSR &= ~(1 << WDRF);
	wdt_disable();

	/* Disable clock division */
	clock_prescale_set(clock_div_1);
#endif

	/* DSR high */
	PORTD |= FLOW_OUT;
	/* DSR as output */
	DDRD |= FLOW_OUT;
	/* DTR as input, not really needed as this is the default */
	DDRD &= ~FLOW_IN;
	/* DTR enable pull-up */
	PORTD |= FLOW_IN;

	/* Trigger INT7 on logical change */
	EICRB |= (1 << ISC70);

	/* Hardware Initialization */
	LEDs_Init();
	USB_Init();

}


/** Manage DSR flow control input
 */
void HandleDSR(USB_ClassInfo_CDC_Device_t* const CDCInterfaceInfo)
{
	if( (PIND & FLOW_IN) != 0 )
	{
		CDCInterfaceInfo->State.ControlLineStates.DeviceToHost &= ~CONTROL_LINE_IN;
		LEDs_TurnOffLEDs(LEDMASK_DSR);
	}
	else
	{
		CDCInterfaceInfo->State.ControlLineStates.DeviceToHost |= CONTROL_LINE_IN;
		LEDs_TurnOnLEDs(LEDMASK_DSR);
	}

}

/** Manage DTR flow control output
 */
void HandleDTR(USB_ClassInfo_CDC_Device_t* const CDCInterfaceInfo)
{
	if( (CDCInterfaceInfo->State.ControlLineStates.HostToDevice & CONTROL_LINE_OUT) != 0 )
	{
		PORTD &= ~FLOW_OUT;
		LEDs_TurnOnLEDs(LEDMASK_DTR);
	}
	else
	{
		PORTD |= FLOW_OUT;
		LEDs_TurnOffLEDs(LEDMASK_DTR);
	}
}

/** ISR to manage the DSR flow control input
 */
ISR(INT7_vect)
{
	HandleDSR(&VirtualSerial_CDC_Interface);
}

/** Event handler for a control line state change on a CDC interface.
 *
 *  \param[in] CDCInterfaceInfo  Pointer to the CDC class interface configuration structure being referenced
 */
void EVENT_CDC_Device_ControLineStateChanged(USB_ClassInfo_CDC_Device_t* const CDCInterfaceInfo)
{
	HandleDTR(CDCInterfaceInfo);
}



/** Event handler for the library USB Connection event. */
void EVENT_USB_Device_Connect(void)
{
	LEDs_SetAllLEDs(LEDMASK_USB_ENUMERATING);
}

/** Event handler for the library USB Disconnection event. */
void EVENT_USB_Device_Disconnect(void)
{
	LEDs_SetAllLEDs(LEDMASK_USB_NOTREADY);
}

/** Event handler for the library USB Configuration Changed event. */
void EVENT_USB_Device_ConfigurationChanged(void)
{
	bool ConfigSuccess = true;

	ConfigSuccess &= CDC_Device_ConfigureEndpoints(&VirtualSerial_CDC_Interface);

	if (ConfigSuccess)
	{
		LEDs_SetAllLEDs(LEDMASK_USB_READY);
		HandleDSR(&VirtualSerial_CDC_Interface);
		EIMSK |=  ( 1 << INT7);
	}
	else
	{
		LEDs_SetAllLEDs(LEDMASK_USB_ERROR);
		EIMSK &= ~( 1 << INT7);
		HandleDSR(&VirtualSerial_CDC_Interface);
	}
}

/** Event handler for the library USB Control Request reception event. */
void EVENT_USB_Device_ControlRequest(void)
{
	CDC_Device_ProcessControlRequest(&VirtualSerial_CDC_Interface);
}

/** ISR to manage the reception of data from the serial port, placing received bytes into a circular buffer
 *  for later transmission to the host.
 */
ISR(USART1_RX_vect, ISR_BLOCK)
{
	uint8_t ReceivedByte = UDR1;

	if ((USB_DeviceState == DEVICE_STATE_Configured) && !(RingBuffer_IsFull(&USARTtoUSB_Buffer)))
		RingBuffer_Insert(&USARTtoUSB_Buffer, ReceivedByte);
}

/** Event handler for the CDC Class driver Line Encoding Changed event.
 *
 *  \param[in] CDCInterfaceInfo  Pointer to the CDC class interface configuration structure being referenced
 */
void EVENT_CDC_Device_LineEncodingChanged(USB_ClassInfo_CDC_Device_t* const CDCInterfaceInfo)
{
	uint8_t ConfigMask = 0;

	switch (CDCInterfaceInfo->State.LineEncoding.ParityType)
	{
		case CDC_PARITY_Odd:
			ConfigMask = ((1 << UPM11) | (1 << UPM10));
			break;
		case CDC_PARITY_Even:
			ConfigMask = (1 << UPM11);
			break;
	}

	if (CDCInterfaceInfo->State.LineEncoding.CharFormat == CDC_LINEENCODING_TwoStopBits)
		ConfigMask |= (1 << USBS1);

	switch (CDCInterfaceInfo->State.LineEncoding.DataBits)
	{
		case 6:
			ConfigMask |= (1 << UCSZ10);
			break;
		case 7:
			ConfigMask |= (1 << UCSZ11);
			break;
		case 8:
			ConfigMask |= ((1 << UCSZ11) | (1 << UCSZ10));
			break;
	}


	/* Keep the DTR line held high (not ready) while the USART is reconfigured */
	PORTD |= FLOW_OUT;
	/* Keep the TX line held high (idle) while the USART is reconfigured */
	PORTD |= (1 << 3);

	/* Must turn off USART before reconfiguring it, otherwise incorrect operation may occur */
	UCSR1B = 0;
	UCSR1A = 0;
	UCSR1C = 0;

	/* Set the new baud rate before configuring the USART */
	UBRR1  = SERIAL_2X_UBBRVAL(CDCInterfaceInfo->State.LineEncoding.BaudRateBPS);

	/* Reconfigure the USART in double speed mode for a wider baud rate range at the expense of accuracy */
	UCSR1C = ConfigMask;
	UCSR1A = (1 << U2X1);
	UCSR1B = ((1 << RXCIE1) | (1 << TXEN1) | (1 << RXEN1));

	/* Clear ringbuffers of old data */
	RingBuffer_InitBuffer(&USBtoUSART_Buffer, USBtoUSART_Buffer_Data, sizeof(USBtoUSART_Buffer_Data));
	RingBuffer_InitBuffer(&USARTtoUSB_Buffer, USARTtoUSB_Buffer_Data, sizeof(USARTtoUSB_Buffer_Data));

	/* Release the TX line after the USART has been reconfigured */
	PORTD &= ~(1 << 3);
	/* Reset the DTR line after the USART has been reconfigured */
	HandleDTR(CDCInterfaceInfo);
}

