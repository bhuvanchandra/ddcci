/*
    ddc/ci interface program
    Copyright(c) 2004 Oleg I. Vdovikin (oleg@cs.msu.su)

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* See USB Monitor Control Class 1.0 specification for control codes
   available at http://www.usb.org/developers/devclass_docs/usbmon10.pdf,
   ACCESS.bus (tm) Specifications Version 3.0 available at
   http://www.semtech.com/pdf/abusv30.pdf
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>

#include "i2c-dev.h"

/* ddc/ci defines */
#define DDCCI_COMMAND_READ	0x01	/* read ctrl value */
#define DDCCI_REPLY_READ	0x02	/* read ctrl value reply */
#define DDCCI_COMMAND_WRITE	0x03	/* write ctrl value */

#define DDCCI_COMMAND_SAVE	0x0c	/* save current settings */

#define DDCCI_REPLY_CAPS	0xe3	/* get monitor caps reply */
#define DDCCI_COMMAND_CAPS	0xf3	/* get monitor caps */
#define DDCCI_COMMAND_PRESENCE	0xf7	/* ACCESS.bus presence check */

/* control numbers */
#define DDCCI_CTRL_BRIGHTNESS	0x10

/* samsung specific, magictune starts with writing 1 to this register */
#define DDCCI_CTRL		0xf5
#define DDCCI_CTRL_ENABLE	0x0001
#define DDCCI_CTRL_DISABLE	0x0000

/* ddc/ci iface tunables */
#define DEFAULT_DDCCI_ADDR	0x37	/* samsung ddc/ci logic sits at 0x37 */
#define MAX_BYTES		127	/* max message length */
#define DELAY   		30000	/* uS to wait after write */
#define RETRYS			3	/* number of retry */

/* magic numbers */
#define MAGIC_1	0x51	/* first byte to send, host address */
#define MAGIC_2	0x80	/* second byte to send, ored with length */
#define MAGIC_XOR 0x50	/* initial xor for received frame */


/* verbosity level (0 - normal, 1 - encoded data, 2 - ddc/ci frames) */
static int verbosity = 0;

/* debugging */
void dumphex(FILE *f, unsigned char *buf, unsigned char len)
{
	int i, j;
	
	for (j = 0; j < len; j +=16) {
		if (len > 16) {
			fprintf(f, "%04x: ", j);
		}
		
		for (i = 0; i < 16; i++) {
			if (i + j < len) fprintf(f, "%02x ", buf[i + j]);
			else fprintf(f, "   ");
		}

		fprintf(f, "| ");

		for (i = 0; i < 16; i++) {
			if (i + j < len) fprintf(f, "%c", 
				buf[i + j] >= ' ' && buf[i + j] < 127 ? buf[i + j] : '.');
			else fprintf(f, " ");
		}
		
		fprintf(f, "\n");
	}
}


/* write len bytes (stored in buf) to i2c address addr */
/* return 0 on success, -1 on failure */
int i2c_write(int fd, unsigned int addr, unsigned char *buf, unsigned char len)
{
	int i;
	struct i2c_rdwr_ioctl_data msg_rdwr;
	struct i2c_msg             i2cmsg;

	/* done, prepare message */	
	msg_rdwr.msgs = &i2cmsg;
	msg_rdwr.nmsgs = 1;

	i2cmsg.addr  = addr;
	i2cmsg.flags = 0;
	i2cmsg.len   = len;
	i2cmsg.buf   = buf;

	if ((i = ioctl(fd, I2C_RDWR, &msg_rdwr)) < 0 )
	{
	    perror("ioctl()");
	    fprintf(stderr,"ioctl returned %d\n",i);
	    return -1;
	}

	return i;

}

/* read at most len bytes from i2c address addr, to buf */
/* return -1 on failure */
int i2c_read(int fd, unsigned int addr, unsigned char *buf, unsigned char len)
{
	struct i2c_rdwr_ioctl_data msg_rdwr;
	struct i2c_msg             i2cmsg;
	int i;

	msg_rdwr.msgs = &i2cmsg;
	msg_rdwr.nmsgs = 1;

	i2cmsg.addr  = addr;
	i2cmsg.flags = I2C_M_RD;
	i2cmsg.len   = len;
	i2cmsg.buf   = buf;

	if ((i = ioctl(fd, I2C_RDWR, &msg_rdwr)) < 0)
	{
	    perror("ioctl()");
	    fprintf(stderr,"ioctl returned %d\n",i);
	    return -1;
	}

	return i;
}

/* write len bytes (stored in buf) to ddc/ci at address addr */
/* return 0 on success, -1 on failure */
int ddcci_write(int fd, unsigned int addr, unsigned char *buf, unsigned char len)
{
	int i = 0;
	unsigned char _buf[MAX_BYTES + 3];
	unsigned xor = ((unsigned char)addr << 1);	/* initial xor value */

	if (verbosity > 1) {
		fprintf(stderr, "Send: ");
		dumphex(stderr, buf, len);
	}

	/* put first magic */
	xor ^= (_buf[i++] = MAGIC_1);
	
	/* second magic includes message size */
	xor ^= (_buf[i++] = MAGIC_2 | len);
	
	while (len--) /* bytes to send */
		xor ^= (_buf[i++] = *buf++);
		
	/* finally put checksum */
	_buf[i++] = xor;

	return i2c_write(fd, addr, _buf, i);
}

/* read ddc/ci formatted frame from ddc/ci at address addr, to buf */
int ddcci_read(int fd, unsigned int addr, unsigned char *buf, unsigned char len)
{
	unsigned char _buf[MAX_BYTES];
	unsigned char xor = MAGIC_XOR;
	int i, _len;

	if (i2c_read(fd, addr, _buf, len + 3) <= 0 ||
	    _buf[0] == 0x51 || _buf[0] == 0xff) // busy ???
	{
		return -1;
	}
	
	/* validate answer */
	if (_buf[0] != addr * 2) {
		dumphex(stderr, _buf, sizeof(_buf));
		fprintf(stderr, "Invalid response, first byte is 0x%02x, should be 0x%02x\n",
			_buf[0], addr * 2);
		return -1;
	}

	if ((_buf[1] & MAGIC_2) == 0) {
		fprintf(stderr, "Invalid response, magic is 0x%02x\n", _buf[1]);
		return -1;
	}

	_len = _buf[1] & ~MAGIC_2;
	if (_len > len || _len > sizeof(_buf)) {
		fprintf(stderr, "Invalid response, length is %d, should be %d at most\n",
			_len, len);
		return -1;
	}

	/* get the xor value */
	for (i = 0; i < _len + 3; i++) {
		xor ^= _buf[i];
	}
	
	if (xor != 0) {
		fprintf(stderr, "Invalid response, corrupted data - xor is 0x%02x, length 0x%02x\n", xor, _len);
		for (i = 0; i < _len + 3; i++) {
			fprintf(stderr, "0x%02x ", _buf[i]);
		}
		fprintf(stderr, "\n");
		
		return -1;
	}

	/* copy payload data */
	memcpy(buf, _buf + 2, _len);
	
	if (verbosity > 1) {
		fprintf(stderr, "Recv: ");
		dumphex(stderr, buf, _len);
	}
	
	return _len;
}

/* write value to register ctrl of ddc/ci at address addr */
int ddcci_writectrl(int fd, unsigned int addr, unsigned char ctrl, unsigned short value)
{
	unsigned char buf[4];

	buf[0] = DDCCI_COMMAND_WRITE;
	buf[1] = ctrl;
	buf[2] = (value >> 8);
	buf[3] = (value & 255);

	return ddcci_write(fd, addr, buf, sizeof(buf));
}

/* read register ctrl raw data of ddc/ci at address addr */
int ddcci_readctrl(int fd, unsigned int addr, 
	unsigned char ctrl, unsigned char *buf, unsigned char len)
{
	unsigned char _buf[2];

	_buf[0] = DDCCI_COMMAND_READ;
	_buf[1] = ctrl;

	if (ddcci_write(fd, addr, _buf, sizeof(_buf)) < 0)
	{
		return -1;
	}

    	usleep(DELAY);
    	
	return ddcci_read(fd, addr, buf, len);
}

/* read capabilities raw data of ddc/ci at address addr starting at offset to buf */
int ddcci_caps(int fd, unsigned int addr, 
	unsigned int offset, unsigned char *buf, unsigned char len)
{
	unsigned char _buf[3];

	_buf[0] = DDCCI_COMMAND_CAPS;
	_buf[1] = offset >> 8;
	_buf[2] = offset & 255;

	if (ddcci_write(fd, addr, _buf, sizeof(_buf)) < 0) 
	{
		return -1;
	}
	
	usleep(DELAY);
	
	return ddcci_read(fd, addr, buf, len);
}

/* save current settings */
int ddcci_command(int fd, unsigned int addr, unsigned char cmd)
{
	unsigned char _buf[1];

	_buf[0] = cmd;

	return ddcci_write(fd, addr, _buf, sizeof(_buf));
}

/* testing stuff */

/* get ctrlname based on id */
char *ctrlname(unsigned char ctrl)
{
	switch (ctrl) {
	case 0x00: return "Degauss";	/* ACCESS.bus */
	case 0x01: return "Degauss";	/* USB */
	case 0x02: return "Secondary Degauss";	/* ACCESS.bus */
	case 0x04: return "Reset Factory Defaults";
	case 0x05: return "SAM: Reset Brightness and Contrast";	/* ??? */
	case 0x06: return "Reset Factory Geometry";
	case 0x08: return "Reset Factory Default Color";	/* ACCESS.bus */
	case 0x0a: return "Reset Factory Default Position";	/* ACCESS.bus */
	case 0x0c: return "Reset Factory Default Size";		/* ACCESS.bus */
	case 0x0e: return "SAM: Image Lock Coarse";	/* ??? */
	case 0x10: return "Brightness";
	case 0x12: return "Contrast";
	case 0x14: return "Select Color Preset";	/* ACCESS.bus */
	case 0x16: return "Red Video Gain";
	case 0x18: return "Green Video Gain";
	case 0x1a: return "Blue Video Gain";
	case 0x1c: return "Focus";	/* ACCESS.bus */
	case 0x1e: return "SAM: Auto Size Center";	/* ??? */
	case 0x20: return "Horizontal Position";
	case 0x22: return "Horizontal Size";
	case 0x24: return "Horizontal Pincushion";
	case 0x26: return "Horizontal Pincushion Balance";
	case 0x28: return "Horizontal Misconvergence";
	case 0x2a: return "Horizontal Linearity";
	case 0x2c: return "Horizontal Linearity Balance";
	case 0x30: return "Vertical Position";
	case 0x32: return "Vertical Size";
	case 0x34: return "Vertical Pincushion";
	case 0x36: return "Vertical Pincushion Balance";
	case 0x38: return "Vertical Misconvergence";
	case 0x3a: return "Vertical Linearity";
	case 0x3c: return "Vertical Linearity Balance";
	case 0x3e: return "SAM: Image Lock Fine";	/* ??? */
	case 0x40: return "Parallelogram Distortion";
	case 0x42: return "Trapezoidal Distortion";
	case 0x44: return "Tilt (Rotation)";
	case 0x46: return "Top Corner Distortion Control";
	case 0x48: return "Top Corner Distortion Balance";
	case 0x4a: return "Bottom Corner Distortion Control";
	case 0x4c: return "Bottom Corner Distortion Balance";
	case 0x50: return "Hue";	/* ACCESS.bus */
	case 0x52: return "Saturation";	/* ACCESS.bus */
	case 0x54: return "Color Curve Adjust";	/* ACCESS.bus */
	case 0x56: return "Horizontal Moire";
	case 0x58: return "Vertical Moire";
	case 0x5a: return "Auto Size Center Enable/Disable";	/* ACCESS.bus */
	case 0x5c: return "Landing Adjust";	/* ACCESS.bus */
	case 0x5e: return "Input Level Select";	/* ACCESS.bus */
	case 0x60: return "Input Source Select";
	case 0x62: return "Audio Speaker Volume Adjust";	/* ACCESS.bus */
	case 0x64: return "Audio Microphone Volume Adjust";	/* ACCESS.bus */
	case 0x66: return "On Screen Display Enable/Disable";	/* ACCESS.bus */
	case 0x68: return "Language Select";	/* ACCESS.bus */
	case 0x6c: return "Red Video Black Level";
	case 0x6e: return "Green Video Black Level";
	case 0x70: return "Blue Video Black Level";
	case 0xa2: return "Auto Size Center";	/* USB */
	case 0xa4: return "Polarity Horizontal Synchronization";	/* USB */
	case 0xa6: return "Polarity Vertical Synchronization";	/* USB */
	case 0xa8: return "Synchronization Type";	/* USB */
	case 0xaa: return "Screen Orientation";	/* USB */
	case 0xac: return "Horizontal Frequency";	/* USB */
	case 0xae: return "Vertical Frequency";	/* USB */
	case 0xb0: return "Settings";
/*	case 0xb6: return "b6 r/o";
	case 0xc6: return "c6 r/o";
	case 0xc8: return "c8 r/o";
	case 0xc9: return "c9 r/o";*/
	case 0xca: return "On Screen Display";	/* USB */
	case 0xcc: return "SAM: On Screen Display Language";	/* ??? */
	case 0xd4: return "Stereo Mode";	/* USB */
	case 0xd6: return "SAM: DPMS control (1 - on/4 - stby)";
	case 0xdc: return "SAM: MagicBright (1 - text/2 - internet/3 - entertain/4 - custom)";
	case 0xdf: return "VCP Version";	/* ??? */
	case 0xe0: return "SAM: Color preset (0 - normal/1 - warm/2 - cool)";
	case 0xe1: return "SAM: Power control (0 - off/1 - on)";
/*	case 0xe2: return "e2 r/w";*/
	case 0xed: return "SAM: Red Video Black Level";
	case 0xee: return "SAM: Green Video Black Level";
	case 0xef: return "SAM: Blue Video Black Level";
	case 0xf5: return "SAM: VCP Enable";
	}
	
	return "???";
}

int ddcci_dumpctrl(int fd, unsigned int addr, 
	unsigned char ctrl, int force)
{
	unsigned char buf[8];

	int len = ddcci_readctrl(fd, addr, ctrl, buf, sizeof(buf));
	
	if (len == sizeof(buf) && buf[0] == DDCCI_REPLY_READ &&
		buf[2] == ctrl && (force || !buf[1])) /* buf[1] is validity (0 - valid, 1 - invalid) */
	{	
		int current = buf[6] * 256 + buf[7];
		int maximum = buf[4] * 256 + buf[5];
		
		fprintf(stdout, "Control 0x%02x: %c/%d/%d\t[%s]\n", ctrl, 
			buf[1] ? '-' : '+',  current, maximum, ctrlname(ctrl));
		if (verbosity) {
			fprintf(stderr, "Raw: ");
			dumphex(stderr, buf, sizeof(buf));
		}
	}
	
	return len;
}

void usage(char *name)
{
	fprintf(stderr,"%s [-a adr] [-e] [-d] [-c] [-f] [-v] [-s] [-S] [-r ctrl] [-w value] dev\n", name);
	fprintf(stderr,"\tdev: device, e.g. /dev/i2c-0\n");
	fprintf(stderr,"\tadr: base address of ddc/ci, eg 0x37 (def)\n");
	fprintf(stderr,"\t-e : query EDID at 0x50\n");
	fprintf(stderr,"\t-c : query capability\n");
	fprintf(stderr,"\t-d : query ctrls 0 - 255\n");
	fprintf(stderr,"\t-r : query ctrl\n");
	fprintf(stderr,"\t-w : value to write to ctrl\n");
	fprintf(stderr,"\t-f : force (avoid validity checks)\n");
	fprintf(stderr,"\t-s : save settings\n");
	fprintf(stderr,"\t-v : verbosity (specify more to increase)\n");
	fprintf(stderr,"\t-S : send Samsung DDC/CI enable\n");
}

int main(int argc, char **argv)
{
	int i, retry;

	/* filedescriptor and name of device */
	int fd; 
	char *fn;
	unsigned int addr = DEFAULT_DDCCI_ADDR;

	/* what to do */
	int dump = 0;
	int ctrl = -1;
	int value = -1;
	int caps = 0;
	int edid = 0;
	int save = 0;
	int force = 0;
	int sam = 0;
	
	fprintf(stdout, "ddcci-tool version 0.03\n");
    
	while ((i=getopt(argc,argv,"a:hdr:w:cesfvS")) >= 0)
	{
		switch(i) {
		case 'h':
			usage(argv[0]);
			exit(1);
			break;
		case 'a':
	    		if ((addr = strtol(optarg, NULL, 0)) < 0 || (addr > 127)){
				fprintf(stderr,"'%s' does not seem to be a valid i2c address\n", optarg);
				exit(1);
	    		}
			break;
		case 'r':
	    		if ((ctrl = strtol(optarg, NULL, 0)) < 0 || (ctrl > 255)){
				fprintf(stderr,"'%s' does not seem to be a valid register name\n", optarg);
				exit(1);
	    		}
			break;
		case 'w':
			if ((value = strtol(optarg, NULL, 0)) < 0 || (value > 65535)){
				fprintf(stderr,"'%s' does not seem to be a valid value.\n", optarg);
				exit(1);
	    		}
	    		break;
		case 'c':
	    		caps++;
			break;
		case 'd':
			dump++;
			break;
		case 'e':
			edid = 0x50;
			break;
		case 's':
			save++;
			break;
		case 'f':
			force++;
			break;
		case 'v':
			verbosity++;
			break;
		case 'S':
			sam++;
			break;
		}
    	}

	if (optind == argc) 
	{
		usage(argv[0]);
		exit(1);
	}
	
	fn = argv[optind];
	
	if ((fd = open(fn, O_RDWR)) < 0)
	{
		perror(fn);
		fprintf(stderr, "Be sure you've modprobed i2c-dev and correct i2c device.\n");
		exit(1);
    	}

	if (edid)
    	{
    		unsigned char buf[128];
		buf[0] = 0;	/* eeprom offset */
		
		fprintf(stdout, "\nReading EDID : 0x%02x@%s\n", edid, fn);
    		if (i2c_write(fd, edid, buf, 1) > 0 &&
		    i2c_read(fd, edid, buf, sizeof(buf)) > 0) 
		{
    			if (verbosity) {
				dumphex(stdout, buf, sizeof(buf));
			}
			
    			printf("\tPlug and Play ID: %c%c%c%02X%02X\n", 
				((buf[8] >> 2) & 31) + 'A' - 1, 
				((buf[8] & 3) << 3) + (buf[9] >> 5) + 'A' - 1, 
				(buf[9] & 31) + 'A' - 1, 
				buf[11], buf[10]);
    			printf("\tInput type: %s\n", (buf[20] & 0x80) ? "Digital" : "Analog");
    		} else {
			fprintf(stderr, "Reading EDID 0x%02x@%s failed.\n", edid, fn);
    		}
    	}

	fprintf(stdout, "\nUsing ddc/ci : 0x%02x@%s\n", addr, fn);
	
	if ((sam ? ddcci_writectrl(fd, addr, DDCCI_CTRL, DDCCI_CTRL_ENABLE) :
  	           ddcci_command(fd, addr, DDCCI_COMMAND_PRESENCE)) < 0)
	{
		fprintf(stderr, "\nDDC/CI at 0x%02x is unusable.\n", addr);
	} else {
		/* enable/presence delay */
		usleep(DELAY);
		
		if (caps) {
			unsigned char buf[35];	/* 19 bytes chunk */
			int len, offset = 0;

			fprintf(stdout, "\nCapabilities:\n");
			
 			do {
 				for (retry = RETRYS; retry &&
					((len = ddcci_caps(fd, addr, offset, buf, sizeof(buf))) < 0); retry--) usleep(DELAY);
 				
 				if (len < 3 || buf[0] != DDCCI_REPLY_CAPS || 
					(buf[1] * 256 + buf[2]) != offset) 
				{
					fprintf(stderr, "Invalid sequence in caps.\n");
 					break;
 				}

				for (i = 3; i < len; i++) {
					fprintf(stdout, i > 2 && buf[i] >= 0x20 && buf[i] < 127 ? "%c" : "0x%02x ", buf[i]);
				}

				offset += len - 3;
			} while (len > 3);
			
			fprintf(stdout, "\n");
		}
		if (ctrl >= 0) {
			if (value >= 0) {
				fprintf(stdout, "\nWriting 0x%02x(%s), 0x%02x(%d)\n",
					ctrl, ctrlname(ctrl), value, value);
				ddcci_writectrl(fd, addr, ctrl, value);
				usleep(DELAY);
			} else {
				fprintf(stdout, "\nReading 0x%02x(%s)\n",
					ctrl, ctrlname(ctrl));
			}
			
			for (retry = RETRYS; retry &&
				(ddcci_dumpctrl(fd, addr, ctrl, 1) < 0); retry--) usleep(DELAY);
		}
		if (dump) {
			fprintf(stdout, "\nControls (valid/current/max):\n");
			
			for (i = 0; i < 256; i++) {
				for (retry = RETRYS; retry &&
					(ddcci_dumpctrl(fd, addr, i, force) < 0); retry--) usleep(DELAY);
			}
		}
		if (save) {
			fprintf(stdout, "\nSaving settings...\n");
			ddcci_command(fd, addr, DDCCI_COMMAND_SAVE);
		}
		
		usleep(DELAY);
		sam && ddcci_writectrl(fd, addr, DDCCI_CTRL, DDCCI_CTRL_DISABLE);
	}

	close(fd);

	exit(0);
}
