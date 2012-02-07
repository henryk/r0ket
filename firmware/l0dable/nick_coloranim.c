/* Written by Henryk Plötz <henryk@ploetzli.ch>,
 * based on code by the_nihilant <thenihilant@googlemail>
 * USAGE:
 *  + Needs PCA (preprocessed color animation) files, see separate python tool to generate them
 *
 * -- NOTICE!! -- This thing will only work on colour displays!
 */

#include <sysinit.h>

#include "basic/basic.h"
#include "lcd/lcd.h"
#include "lcd/print.h"
#include "filesystem/ff.h"

#include "usetable.h"

#define YAY 1
#define EPICFAIL 0

#define TYPE_CMD    0
#define TYPE_DATA   1

#define WIDTH 98
#define HEIGHT 70

#define CONFFILE "COLORANI.CFG"

/* a array  s start */
#define GETINT(a,s) ((((uint32_t)((a)[(s)+3]))<<24)|(((uint32_t)((a)[(s)+2]))<<16)|(((uint32_t)((a)[(s)+1]))<<8)|((a)[s]))
#define GETSHORT(a,s) ((((uint16_t)((a)[(s)+1]))<<8)|((a)[s]))
#define FAIL(s) {lcdClear();lcdPrintln(s);lcdRefresh();delayms(1000);return EPICFAIL;}

void ram_real(void);
void ram(void) {
	ram_real();
}

uint8_t instruction_buffer[127*2];
char filename[24] = { '\0' };

/*
 * File format:
 * 4 bytes magic: "CANI"
 * 1 byte version, 2
 * 1 byte width
 * 1 byte height
 * sequence of directives:
 *  1 byte pixel/pause switch:
 *      0xxx xxxx - xxx xxxx * 2 bytes of pixel instructions follow
 *  	1xxx xxxx - pause for xxx xxxx (binary) * 10 ms,
 *  	            if xxx xxxx == 000 000, pause forever
 *  pixel instructions:
 *      0xxx xxxx - xxx xxxx 16-bit words of RGB565 data follow, send to screen literally
 *      			  (note: 0000 0000 is valid padding)
 *      10xx xxxx - repeat following 16-bit (RGB 565) xx xxxx times.
 *      1100 0000 - next are 4 bytes of x,y coordinates, change drawing area
 *                  	1 byte xs
 *                  	1 byte xe
 *                  	1 byte ys
 *                  	1 byte ye
 */

struct pca_header {
	uint8_t magic[4];
	unsigned int version:8;
	unsigned int width:8;
	unsigned int height:8;
} __attribute__((packed));

int parseBitmap(FIL*inf) {
	struct pca_header header;
	FRESULT res;
	int w, h;
	UINT readbytes;

	res = f_read(inf, (char*) &header, sizeof(header), &readbytes);
	if ((res) || (readbytes != sizeof(header))) {
		FAIL("file reading");
	}

	if(header.magic[0] != 'C' || header.magic[1] != 'A' || header.magic[2] != 'N' || header.magic[3] != 'I' )
		FAIL("unsupported format");

	if(header.version != 2) {
		FAIL("unsupported version");
	}
	w = header.width;
	h = header.height;

	if (w != WIDTH)
		FAIL("ERR:width!=98");

	if (h != HEIGHT)
		FAIL("ERR:height!=70");

	return YAY;
}

static void lcdWrite(uint8_t cd, uint8_t data) {
	uint16_t frame = 0x0;

	frame = cd << 8;
	frame |= data;

	while ((SSP_SSP0SR & (SSP_SSP0SR_TNF_NOTFULL | SSP_SSP0SR_BSY_BUSY))
			!= SSP_SSP0SR_TNF_NOTFULL)
		;
	SSP_SSP0DR = frame;
	while ((SSP_SSP0SR & (SSP_SSP0SR_BSY_BUSY | SSP_SSP0SR_RNE_NOTEMPTY))
			!= SSP_SSP0SR_RNE_NOTEMPTY)
		;
	/* clear the FIFO */
	frame = SSP_SSP0DR;
}

static void lcd_window(int xs, int xe, int ys, int ye)
{
	lcdWrite(TYPE_CMD, 0x2A),
	lcdWrite(TYPE_DATA, xs);
	lcdWrite(TYPE_DATA, xe);
	lcdWrite(TYPE_CMD, 0x2B);
	lcdWrite(TYPE_DATA, ys);
	lcdWrite(TYPE_DATA, ye);
	lcdWrite(TYPE_CMD, 0x2C);
}

static void lcd_select() {
#if CFG_USBMSC_SHOULDNT_HAPPEN
	if(usbMSCenabled) {
		intstatus=USB_DEVINTEN;
		USB_DEVINTEN=0;
	};
#endif
	/* the LCD requires 9-Bit frames */
	uint32_t configReg = (SSP_SSP0CR0_DSS_9BIT // Data size = 9-bit
	| SSP_SSP0CR0_FRF_SPI // Frame format = SPI
	| SSP_SSP0CR0_SCR_8); // Serial clock rate = 8
	SSP_SSP0CR0 = configReg;
	gpioSetValue(RB_LCD_CS, 0);
}

static void lcd_deselect() {
	gpioSetValue(RB_LCD_CS, 1);
	/* reset the bus to 8-Bit frames that everyone else uses */
	uint32_t configReg = (SSP_SSP0CR0_DSS_8BIT // Data size = 8-bit
	| SSP_SSP0CR0_FRF_SPI // Frame format = SPI
	| SSP_SSP0CR0_SCR_8); // Serial clock rate = 8
	SSP_SSP0CR0 = configReg;
#if CFG_USBMSC_SHOULDNT_HAPPEN
	if(usbMSCenabled) {
		USB_DEVINTEN=intstatus;
	};
#endif
}

static void lcd_setup(void)
{
	/* Defensively set up the LCD for the whole screen, in case no
	 * command for that is in the file
	 */
	lcd_select();
	lcd_window(0, WIDTH-1, 0, HEIGHT-1);
	lcd_deselect();
}


int getInputWaitDebounce() {
	delayms(100);
	return getInputWait();

}

void lcd_write_data(const uint8_t *buffer, int length)
{
	for(int i=0; i<length; i++) {
		lcdWrite(TYPE_DATA, buffer[i]);
	}
}

void handle_instructions(const uint8_t *buffer, int buffer_length)
{
	int pos = 0;
	while(pos < buffer_length) {
		if( (buffer[pos] & 0x80) == 0x00 ) {
			/* literal pixel data */
			int count = buffer[pos] & 0x7F; // FIXME BOUNDARY CHECK
			pos++;

			lcd_write_data(buffer + pos, count*2);
			pos += count*2;

		} else if( (buffer[pos] & 0xC0) == 0x80 ) {
			/* repeating data (pseudo-RLE) */
			int count = buffer[pos] & 0x3F;
			pos++;

			for(int i=0; i<count; i++) {
				lcd_write_data(buffer + pos, 2);
			}
			pos+=2;

		} else if( (buffer[pos] == 0xC0) ) {
			/* change window */ /* FIXME check boundary*/
			pos++;
			int xs = buffer[pos++];
			int xe = buffer[pos++];
			int ys = buffer[pos++];
			int ye = buffer[pos++];

			lcd_window(xs, xe, ys, ye);
		} // FIXME else-Exception
	}
}

int displayfile(FIL*file, int *key) {

	while(true) {
		uint8_t pixelpause_switch;
		unsigned int r;
		int res = f_read(file, (char*) &pixelpause_switch, sizeof(pixelpause_switch), &r);
		if(res == 0 && r == 0) {
			/* End of file */
			return YAY;
		}

		if(res || r != sizeof(pixelpause_switch)) {
			FAIL("PPSWITCH");
			return EPICFAIL;
		}

		if( (pixelpause_switch & 0x80) == 0x00) {
			/* read (pixelpause_switch & 0x7F) * 2 bytes of pixel instructions */
			int instruction_length = (pixelpause_switch & 0x7F) * 2;

			res = f_read(file, (char*) instruction_buffer, instruction_length, &r);
			if(res || r != instruction_length) {
				FAIL("IBUF");
				return EPICFAIL;
			}

			lcd_select();
			handle_instructions(instruction_buffer, instruction_length);
			lcd_deselect();

		} else {
			/* do a pause */
			int timeout = (pixelpause_switch & 0x7F) * 10;

			if(timeout == 0) {
				/* wait forever */
				*key = getInputWaitDebounce();
			} else {
				/* wait some time */
				*key = getInputWaitTimeout(timeout);
			}
		}

		if(*key != BTN_NONE) {
			return YAY;
		}

	}

	return YAY;
}

FRESULT readconfig() {
	FIL cfile;
	FRESULT fr;
	UINT readbytes = 0;

	fr = f_open(&cfile, CONFFILE, FA_OPEN_EXISTING | FA_READ);
	if (fr != FR_OK) {
		filename[0] = 0;
		return fr;
	}

	fr = f_read(&cfile, filename, sizeof(filename) - 1, &readbytes);
	if (fr != FR_OK) {
		filename[0] = 0;
		return fr;
	}

	filename[readbytes] = 0;
	f_close(&cfile);

	return FR_OK;
}

void displayScreen(char*things[]) {
	int cx;
	lcdClear();
	for (cx = 0; (cx < 8) && (things[cx] != NULL); cx++)
		lcdPrintln(things[cx]);
	lcdRefresh();
}

static char *info[] = {
		"COLOR BITMAP",
		"NICK ANIMATION",
		"by",
		" the_nihilant",
		"28C3 DECT 3141", NULL };

static char *menu[] = {
		" Info",
		" Choose file",
		" Main menu",
		NULL };
#define MENUITEMS 3

int menuhandling(void) {
	int cx, pos = 0;
	do {
		lcdClear();
		lcdPrintln("Color anim");
		for (cx = 0; menu[cx] != NULL; cx++) {
			lcdPrint((cx == pos) ? "*" : " ");
			lcdPrintln(menu[cx]);
		}
		lcdRefresh();
		cx = getInputWaitDebounce();
		switch (cx) {
		case BTN_LEFT:
			return -1;
		case BTN_UP:
			if (pos > 0)
				pos--;
			else
				pos = MENUITEMS - 1;
			break;
		case BTN_DOWN:
			if (pos < (MENUITEMS - 1))
				pos++;
			else
				pos = 0;
			break;
		case BTN_RIGHT:
		case BTN_ENTER:
			return pos;
		}
	} while (true);
	return -1;
}

void writeconfig() {
	FIL cf;
	FRESULT fr;
	unsigned int written;

	fr = f_open(&cf, CONFFILE, FA_WRITE | FA_OPEN_ALWAYS);
	if (fr)
		return;

	f_write(&cf, filename, strlen(filename), &written);
	f_write(&cf, "\n", 1, &written);
	f_close(&cf);
}

//shows *.bmp
void ram_real(void) {
	FIL file;
	FRESULT res;
	int key = BTN_NONE, menuselection;

	res = readconfig();

	do {
		if(res == FR_OK) {
			res = f_open(&file, (const char*) filename, FA_OPEN_EXISTING | FA_READ);
		}

		if ((res != FR_OK) || (parseBitmap(&file) == EPICFAIL)) {
			lcdClear();
			lcdPrintln("Error while");
			lcdPrintln("loading:");
			lcdPrintln(filename);
			lcdPrintln("");
			lcdPrintln("Press any");
			lcdPrintln("        key...");
			lcdRefresh();
			getInputWaitDebounce();

			displayScreen(info);
			key = getInputWaitDebounce();
		} else {
			while (true) {
				lcd_setup();

				if (displayfile(&file, &key) == EPICFAIL) {
					break;
				}

				if (key != BTN_NONE) {
					break;
				}

				f_lseek(&file, sizeof(struct pca_header));
			}
			f_close(&file);
		}

		if (key == BTN_ENTER) {
			do {
				menuselection = menuhandling();

				switch (menuselection) {
				case 0:
					displayScreen(info);
					getInputWaitDebounce();
					break;
				case 1:
					lcdClear();
					selectFile(filename, "PCA");
					writeconfig();
					break;
				case 2: /* Fall-through */
				default:
					key = BTN_RIGHT;
					menuselection = -1;
					break;
				}
			} while (menuselection >= 0);
		}

	} while (key == BTN_ENTER);

	/* Debounce, but only if not (actually) pressing right or enter (== enter main menu) */
	if(key == BTN_RIGHT) {
		key = getInputRaw();
		if(key != BTN_RIGHT && key != BTN_ENTER)
			while(getInputRaw() == key);
	}

	return;
}

