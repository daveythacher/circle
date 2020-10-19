//
// usbmouse.cpp
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2014-2018  R. Stange <rsta2@o2online.de>
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <circle/usb/usbmouse.h>
#include <circle/usb/usbhid.h>
#include <circle/logger.h>
#include <assert.h>

// HID Report Items from HID 1.11 Section 6.2.2
#define HID_USAGE_PAGE      0x04
#define HID_USAGE           0x08
#define HID_COLLECTION      0xA0
#define HID_END_COLLECTION  0xC0
#define HID_REPORT_COUNT    0x94
#define HID_REPORT_SIZE     0x74
#define HID_INPUT           0x80
#define HID_REPORT_ID       0x84

// HID Report Usage Pages from HID Usage Tables 1.12 Section 3, Table 1
#define HID_USAGE_PAGE_BUTTONS         0x09

// HID Report Usages from HID Usage Tables 1.12 Section 4, Table 6
#define HID_USAGE_MOUSE     0x02
#define HID_USAGE_X         0x30
#define HID_USAGE_Y         0x31
#define HID_USAGE_WHEEL     0x38

static const char FromUSBMouse[] = "umouse";

CUSBMouseDevice::CUSBMouseDevice (CUSBFunction *pFunction)
:	CUSBHIDDevice (pFunction),
	m_pMouseDevice (0),
	m_pHIDReportDescriptor (0)
{
}

CUSBMouseDevice::~CUSBMouseDevice (void)
{
	delete m_pMouseDevice;
	m_pMouseDevice = 0;

	delete [] m_pHIDReportDescriptor;
	m_pHIDReportDescriptor = 0;
}

boolean CUSBMouseDevice::Configure (void)
{
	TUSBHIDDescriptor *pHIDDesc = (TUSBHIDDescriptor *) GetDescriptor (DESCRIPTOR_HID);
	if (   pHIDDesc == 0
	    || pHIDDesc->wReportDescriptorLength == 0)
	{
		ConfigurationError (FromUSBMouse);

		return FALSE;
	}

	m_usReportDescriptorLength = pHIDDesc->wReportDescriptorLength;
	m_pHIDReportDescriptor = new u8[m_usReportDescriptorLength];
	assert (m_pHIDReportDescriptor != 0);

	if (   GetHost ()->GetDescriptor (GetEndpoint0 (),
					  pHIDDesc->bReportDescriptorType, DESCRIPTOR_INDEX_DEFAULT,
					  m_pHIDReportDescriptor, m_usReportDescriptorLength,
					  REQUEST_IN | REQUEST_TO_INTERFACE, GetInterfaceNumber ())
	    != m_usReportDescriptorLength)
	{
		CLogger::Get ()->Write (FromUSBMouse, LogError, "Cannot get HID report descriptor");

		return FALSE;
	}

	DecodeReport ();

	// ignoring unsupported HID interface
	if (m_MouseReport.nItems == 0)
	{
		return FALSE;
	}

	if (!CUSBHIDDevice::Configure (m_MouseReport.size))
	{
		CLogger::Get ()->Write (FromUSBMouse, LogError, "Cannot configure HID device");

		return FALSE;
	}

	m_pMouseDevice = new CMouseDevice;
	assert (m_pMouseDevice != 0);

	return StartRequest ();
}

void CUSBMouseDevice::ReportHandler (const u8 *pReport, unsigned nReportSize)
{
	if (   pReport != 0
	    && nReportSize == m_MouseReport.size)
	{
		if (m_pMouseDevice != 0)
		{
			u32 ucHIDButtons = 0;
			s32 xMove = 0;
			s32 yMove = 0;
			s32 wheelMove = 0;
			for (u32 index = 0; index < m_MouseReport.nItems; index++)
			{
				TMouseReportItem *item = &m_MouseReport.items[index];
				switch (item->type)
				{
				case MouseItemButtons:
					ucHIDButtons = ExtractUnsigned(pReport, item->offset, item->count);
					break;
				case MouseItemXAxis:
					xMove = ExtractSigned(pReport, item->offset, item->count);
					break;
				case MouseItemYAxis:
					yMove = ExtractSigned(pReport, item->offset, item->count);
					break;
				case MouseItemWheel:
					wheelMove = ExtractSigned(pReport, item->offset, item->count);
					break;
				}
			}

			u32 nButtons = 0;
			if (ucHIDButtons & USBHID_BUTTON1)
			{
				nButtons |= MOUSE_BUTTON_LEFT;
			}
			if (ucHIDButtons & USBHID_BUTTON2)
			{
				nButtons |= MOUSE_BUTTON_RIGHT;
			}
			if (ucHIDButtons & USBHID_BUTTON3)
			{
				nButtons |= MOUSE_BUTTON_MIDDLE;
			}

			m_pMouseDevice->ReportHandler (nButtons, xMove, yMove, wheelMove);
		}
	}
}

u32 CUSBMouseDevice::ExtractUnsigned(const void *buffer, u32 offset, u32 length)
{
	assert(buffer != 0);
	assert(length <= 32);

	u8 *bits = (u8 *)buffer;
	unsigned shift = offset % 8;
	offset = offset / 8;
	bits = bits + offset;
	unsigned number = *(unsigned *)bits;
	offset = shift;

	unsigned result = 0;
	if (length > 24) {
		result = (((1 << 24) - 1) & (number >> offset));
		bits = bits + 3;
		number = *(unsigned *)bits;
		length = length - 24;
		unsigned result2 = (((1 << length) - 1) & (number >> offset));
		result = (result2 << 24) | result;
	} else {
		result = (((1 << length) - 1) & (number >> offset));
	}

	return result;
}

s32 CUSBMouseDevice::ExtractSigned(const void *buffer, u32 offset, u32 length)
{
	assert(buffer != 0);
	assert(length <= 32);

	unsigned result = ExtractUnsigned(buffer, offset, length);
	if (length == 32)
	{
		return result;
	}

	if (result & (1 << (length - 1)))
	{
		result |= 0xffffffff - ((1 << length) - 1);
	}

	return result;
}

void CUSBMouseDevice::DecodeReport ()
{
	s32 item, arg;
	u32 offset = 0, size = 0, count = 0;
	u32 id = 0;
	u32 nCollections = 0;
	u32 itemIndex = 0;
	u32 reportIndex = 0;
	boolean parse = FALSE;

	assert (m_pHIDReportDescriptor != 0);
	s8 *pHIDReportDescriptor = (s8 *) m_pHIDReportDescriptor;

	for (u16 usReportDescriptorLength = m_usReportDescriptorLength; usReportDescriptorLength > 0; )
	{
		item = *pHIDReportDescriptor++;
		usReportDescriptorLength--;

		switch(item & 0x03)
		{
		case 0:
			arg = 0;
			break;
		case 1:
			arg = *pHIDReportDescriptor++;
			usReportDescriptorLength--;
			break;
		case 2:
			arg = *pHIDReportDescriptor++ & 0xFF;
			arg = arg | (*pHIDReportDescriptor++ << 8);
			usReportDescriptorLength -= 2;
			break;
		default:
			arg = *pHIDReportDescriptor++;
			arg = arg | (*pHIDReportDescriptor++ << 8);
			arg = arg | (*pHIDReportDescriptor++ << 16);
			arg = arg | (*pHIDReportDescriptor++ << 24);
			usReportDescriptorLength -= 4;
			break;
		}

		switch(item & 0xFC)
		{
		case HID_COLLECTION:
			nCollections++;
			break;
		case HID_END_COLLECTION:
			nCollections--;
			if (nCollections == 0)
				parse = FALSE;
			break;
		case HID_USAGE:
			if (arg == HID_USAGE_MOUSE)
				parse = TRUE;
			break;
		}

		if (! parse)
			continue;

		if ((item & 0xFC) == HID_REPORT_ID)
		{
			assert(id == 0);
			id = arg;
			offset = 8;
		}

		switch(item & 0xFC)
		{
		case HID_USAGE_PAGE:
			switch(arg)
			{
			case HID_USAGE_PAGE_BUTTONS:
				m_MouseReport.items[itemIndex].type = MouseItemButtons;
				itemIndex++;
				break;
			}
			break;
		case HID_USAGE:
			switch(arg)
			{
			case HID_USAGE_X:
				m_MouseReport.items[itemIndex].type = MouseItemXAxis;
				itemIndex++;
				break;
			case HID_USAGE_Y:
				m_MouseReport.items[itemIndex].type = MouseItemYAxis;
				itemIndex++;
				break;
			case HID_USAGE_WHEEL:
				m_MouseReport.items[itemIndex].type = MouseItemWheel;
				itemIndex++;
				break;
			}
			break;
		case HID_REPORT_SIZE:
			size = arg;
			break;
		case HID_REPORT_COUNT:
			count = arg;
			break;
		case HID_INPUT:
			if ((arg & 0x03) == 0x02)
			{
				u32 tmp = offset;
				while (reportIndex < itemIndex)
				{
					TMouseReportItem *item = &m_MouseReport.items[reportIndex];
					switch (item->type)
					{
					case MouseItemButtons:
						item->count = count * size;
						item->offset = tmp;
						break;
					case MouseItemXAxis:
					case MouseItemYAxis:
					case MouseItemWheel:
						item->count = size;
						item->offset = tmp;
						break;
					}
					tmp += item->count;
					reportIndex++;
				}
			}

			offset += count * size;
			break;
		}
	}

	m_MouseReport.id = id;
	m_MouseReport.size = (offset + 7) / 8;
	m_MouseReport.nItems = itemIndex;
}
