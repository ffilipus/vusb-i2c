/* Name: powerSwitch.c
 * Project: PowerSwitch based on AVR USB driver
 * Author: Christian Starkjohann
 * Creation Date: 2005-01-16
 * Tabsize: 4
 * Copyright: (c) 2005 by OBJECTIVE DEVELOPMENT Software GmbH
 * License: GNU GPL v2 (see License.txt) or proprietary (CommercialLicense.txt)
 * This Revision: $Id$
 */

/*
General Description:
This program controls the PowerSwitch USB device from the command line.
It must be linked with libusb, a library for accessing the USB bus from
Linux, FreeBSD, Mac OS X and other Unix operating systems. Libusb can be
obtained from http://libusb.sourceforge.net/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <usb.h>    /* this is libusb, see http://libusb.sourceforge.net/ */


#define BIT_SDA 6
#define BIT_SCL 7


#define USBDEV_SHARED_VENDOR    0x16C0  /* VOTI */
#define USBDEV_SHARED_PRODUCT   0x05DC  /* Obdev's free shared PID */
/* Use obdev's generic shared VID/PID pair and follow the rules outlined
 * in firmware/usbdrv/USBID-License.txt.
 */

#define PSCMD_ECHO  0
#define PSCMD_GET   1
#define PSCMD_ON    2
#define PSCMD_OFF   3
/* These are the vendor specific SETUP commands implemented by our USB device */


usb_dev_handle      *handle = NULL;

static int  usbGetStringAscii(usb_dev_handle *dev, int index, int langid, char *buf, int buflen)
{
char    buffer[256];
int     rval, i;

    if((rval = usb_control_msg(dev, USB_ENDPOINT_IN, USB_REQ_GET_DESCRIPTOR, (USB_DT_STRING << 8) + index, langid, buffer, sizeof(buffer), 1000)) < 0)
        return rval;
    if(buffer[1] != USB_DT_STRING)
        return 0;
    if((unsigned char)buffer[0] < rval)
        rval = (unsigned char)buffer[0];
    rval /= 2;
    /* lossy conversion to ISO Latin1 */
    for(i=1;i<rval;i++){
        if(i > buflen)  /* destination buffer overflow */
            break;
        buf[i-1] = buffer[2 * i];
        if(buffer[2 * i + 1] != 0)  /* outside of ISO Latin1 range */
            buf[i-1] = '?';
    }
    buf[i-1] = 0;
    return i-1;
}


/* PowerSwitch uses the free shared default VID/PID. If you want to see an
 * example device lookup where an individually reserved PID is used, see our
 * RemoteSensor reference implementation.
 */

#define USB_ERROR_NOTFOUND  1
#define USB_ERROR_ACCESS    2
#define USB_ERROR_IO        3

static int usbOpenDevice(usb_dev_handle **device, int vendor, char *vendorName, int product, char *productName)
{
struct usb_bus      *bus;
struct usb_device   *dev;
usb_dev_handle      *handle = NULL;
int                 errorCode = USB_ERROR_NOTFOUND;
static int          didUsbInit = 0;

    if(!didUsbInit){
        didUsbInit = 1;
        usb_init();
    }
    usb_find_busses();
    usb_find_devices();
    for(bus=usb_get_busses(); bus; bus=bus->next){
        for(dev=bus->devices; dev; dev=dev->next){
            if(dev->descriptor.idVendor == vendor && dev->descriptor.idProduct == product){
                char    string[256];
                int     len;
                handle = usb_open(dev); /* we need to open the device in order to query strings */
                if(!handle){
                    errorCode = USB_ERROR_ACCESS;
                    fprintf(stderr, "Warning: cannot open USB device: %s\n", usb_strerror());
                    continue;
                }
                if(vendorName == NULL && productName == NULL){  /* name does not matter */
                    break;
                }
                /* now check whether the names match: */
                len = usbGetStringAscii(handle, dev->descriptor.iManufacturer, 0x0409, string, sizeof(string));
                if(len < 0){
                    errorCode = USB_ERROR_IO;
                    fprintf(stderr, "Warning: cannot query manufacturer for device: %s\n", usb_strerror());
                }else{
                    errorCode = USB_ERROR_NOTFOUND;
                    /* fprintf(stderr, "seen device from vendor ->%s<-\n", string); */
                    if(strcmp(string, vendorName) == 0){
                        len = usbGetStringAscii(handle, dev->descriptor.iProduct, 0x0409, string, sizeof(string));
                        if(len < 0){
                            errorCode = USB_ERROR_IO;
                            fprintf(stderr, "Warning: cannot query product for device: %s\n", usb_strerror());
                        }else{
                            errorCode = USB_ERROR_NOTFOUND;
                            /* fprintf(stderr, "seen product ->%s<-\n", string); */
                            if(strcmp(string, productName) == 0)
                                break;
                        }
                    }
                }
                usb_close(handle);
                handle = NULL;
            }
        }
        if(handle)
            break;
    }
    if(handle != NULL){
        errorCode = 0;
        *device = handle;
    }
    return errorCode;
}


static unsigned char get_status()
{
	unsigned char buffer[8];
	int nBytes = usb_control_msg(handle, USB_TYPE_VENDOR | USB_RECIP_DEVICE |
			USB_ENDPOINT_IN, PSCMD_GET, 0, 0, (char *)buffer, sizeof(buffer), 5000);

	if (nBytes < 2) {
		fprintf(stderr, "ERR: read status: got %d bytes, expected 2\n", nBytes);
		exit(1);
	}
	return buffer[0];
}

static void verify_low(unsigned char bit, char *bitname)
{
	int i;
	for (i = 0; i < 10; i++) {
		if (~get_status() & (1 << bit))
			return;
		usleep(10);
	}
	fprintf(stderr, "%s stuck high for >1ms\n", bitname);
}

static void verify_high(unsigned char bit, char *bitname)
{
	int i;
	for (i = 0; i < 100; i++) {
		if (get_status() & (1 << bit))
			return;
		usleep(10);
	}
	fprintf(stderr, "%s stuck low for >1ms\n", bitname);
}

static void verify_sda_low()
{
	verify_low(BIT_SDA, "SDA");
}

static void verify_sda_high()
{
	verify_high(BIT_SDA, "SDA");
}

static void verify_scl_low()
{
	verify_low(BIT_SCL, "SCL");
}

static void verify_scl_high()
{
	verify_high(BIT_SCL, "SCL");
}



/*
 * for the record: Yes, this is ugly.
 *
 * Firmware: DDRB  = ~b  (with hardware pull-ups and PORTB = 0)
 * So to pull an output high (1), we turn it on, which sets it as input
 * -> pull-up works. for low (0): turn it off -> output -> pulled low
 */

#define SDA_1 usb_control_msg(handle, USB_TYPE_VENDOR \
		| USB_RECIP_DEVICE | USB_ENDPOINT_IN, \
		PSCMD_ON, 0, BIT_SDA, (char *)buffer, sizeof(buffer), \
		5000);

#define SDA_0 usb_control_msg(handle, USB_TYPE_VENDOR \
		| USB_RECIP_DEVICE | USB_ENDPOINT_IN, \
		PSCMD_OFF, 0, BIT_SDA, (char *)buffer, sizeof(buffer), \
		5000);

#define SCL_1 usb_control_msg(handle, USB_TYPE_VENDOR \
		| USB_RECIP_DEVICE | USB_ENDPOINT_IN, \
		PSCMD_ON, 0, BIT_SCL, (char *)buffer, sizeof(buffer), \
		5000);

#define SCL_0 usb_control_msg(handle, USB_TYPE_VENDOR \
		| USB_RECIP_DEVICE | USB_ENDPOINT_IN, \
		PSCMD_OFF, 0, BIT_SCL, (char *)buffer, sizeof(buffer), \
		5000);

int main(int argc, char **argv)
{
unsigned char       buffer[8];

    usb_init();
    if(usbOpenDevice(&handle, USBDEV_SHARED_VENDOR, "www.obdev.at", USBDEV_SHARED_PRODUCT, "PowerSwitch") != 0){
        fprintf(stderr, "Could not find USB device \"PowerSwitch\" with vid=0x%x pid=0x%x\n", USBDEV_SHARED_VENDOR, USBDEV_SHARED_PRODUCT);
        exit(1);
    }
/* We have searched all devices on all busses for our USB device above. Now
 * try to open it and perform the vendor specific control operations for the
 * function requested by the user.
 */
    
	char line[8];
	signed char i;
	short int number;

	SDA_1;
	SCL_1;
	usleep(1000);
	SDA_0;
	usleep(1000);
	SCL_0;
	verify_sda_low();
	verify_scl_low();
	puts("ready");

	while (fgets(line, 8, stdin) != NULL) {
		if (sscanf(line, "%hi\n", &number) == 1) {
			if ((number >= 0) && (number <= 256)) {
				for (i = 7; i >= -1; i--) {
					if ((i < 0) || (number == 256) || (number & (1 << i))) {
						puts("sda ↑");
						SDA_1;
						//verify_sda_high();
					} else {
						puts("sda ↓");
						SDA_0;
						//verify_sda_low();
					}
					usleep(10);
					puts("scl ↑");
					SCL_1;
					usleep(10);
					//verify_scl_high();
					if (i < 0) {
						if (get_status() & (1 << BIT_SDA))
							puts("> NAK");
						else
							puts("> ACK");
					}
					else if (number == 256) {
						if (get_status() & (1 << BIT_SDA))
							puts("1");
						else
							puts("0");
					}
					puts("scl ↓");
					SCL_0;
					usleep(10);
					//verify_scl_low();
				}
			}
		}
		if (strcmp(line, "push\n") == 0) {
			SCL_1;
			usleep(30000);
			SDA_1;
			usleep(100000);
			SDA_0;
			usleep(100000);
			SCL_0;
		}
	}

	SCL_1;
	usleep(10);
	verify_scl_high();
	SDA_1;
	usleep(10);
	verify_sda_high();

    usb_close(handle);
    return 0;
}
