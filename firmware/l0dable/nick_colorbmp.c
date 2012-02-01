/* Written by  the_nihilant
 at Chaos Communication Camp 2011, heavily patched for the 28C3
 CCCamp2011: DECT 3950
 28C3: DECT 3141
 thenihilant@googlemail

 USAGE:
 *  choose a standard 24- or 32- bits per pixel BMP picture of size 98x(70*x)
 *  select it
 *  the image will be shown statically, if its size is exactly 98x70
 *  frames will be shown with configurable delay in case the height is
 an exact multiple of 70 (like an animation).

 -- NOTICE!! -- This thing will only work on colour displays!

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

#define BMPHEADERSIZE 54
#define CONFFILE "BMPNICK.CFG"

/* a array  s start */
#define GETINT(a,s) ((((uint32_t)((a)[(s)+3]))<<24)|(((uint32_t)((a)[(s)+2]))<<16)|(((uint32_t)((a)[(s)+1]))<<8)|((a)[s]))
#define GETSHORT(a,s) ((((uint16_t)((a)[(s)+1]))<<8)|((a)[s]))
#define FAIL(s) {lcdPrintln(s);return EPICFAIL;}

void ram_real(void);
void ram(void) {
	ram_real();
}

uint8_t line[98 * 4];
uint8_t nframes;
char filename[24] = { '\0' };
int animPause = 100;

int parseBitmap(FIL*inf, int*bpp) {
	uint8_t header[54]; /* BMP header size (hopefully) */
	FRESULT res;
	int i, w, h;
	UINT readbytes;

	for (i = 0; i < 54; i++)
		header[i] = 0; /* useless, but paranoia ftw */

	res = f_read(inf, (char*) header, BMPHEADERSIZE, &readbytes);
	if ((res) || (readbytes != 54))
		return EPICFAIL;

	if ((header[0] != 0x42) || (header[1] != 0x4D))
		FAIL("BM");
	w = GETINT(header,0x12);
	h = GETINT(header,0x16);

	if (w != 98)
		FAIL("ERR:width!=98");

	nframes = h / 70;
	if ((h % 70) != 0)
		FAIL("ERR:height%70!=0");

	if (GETSHORT(header,0x1A) != 1)
		FAIL("> 1 plane!?");
	if (GETINT(header,0x1E) != 0)
		FAIL("compression!?");
	if (GETINT(header,0x2E) != 0)
		FAIL("palette!?");
	*bpp = GETSHORT(header,0x1C);
	if ((*bpp != 24) && (*bpp != 32))
		FAIL("BPP!=24 or 32");

	return YAY;
}

int bmpreadline(FIL *inf, int size) {
	int readsize;
	FRESULT res;
	UINT readreally;

	size = size / 8; //bits to BYTES!

	readsize = 98 * size + (size == 3 ? 2 : 0);
	res = f_read(inf, (char*) line, readsize, &readreally);
	if (readreally != readsize)
		FAIL("BMP data IOerr");
	return YAY;
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

int getInputWaitDebounce() {
	delayms(100);
	return getInputWait();

}

void lcdpushpixel(uint32_t r, uint32_t g, uint32_t b) {
	int color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3);
	lcdWrite(TYPE_DATA, color >> 8);
	lcdWrite(TYPE_DATA, color & 0xFF);
}

void pushcolorline(int size) {
	int i;
	for (i = 98; i > 0; i--) {
		if (size == 24) {
			lcdpushpixel(line[i * 3 - 1], line[i * 3 - 2], line[i * 3 - 3]);
		} else {
			lcdpushpixel(line[i * 4 - 1], line[i * 4 - 2], line[i * 4 - 3]);
		}
	}
}

void lcd_start_frame() {
	lcd_select();
	lcdWrite(TYPE_CMD, 0x2C);
	lcd_deselect();
}

int displayframe(FIL*file, int size) {
	int i;
	for (i = 0; i < 70; i++) {
		if (bmpreadline(file, size) == EPICFAIL)
			return EPICFAIL;
		lcd_select();
		pushcolorline(size);
		lcd_deselect();
	}
	return YAY;
}

FRESULT openfilebyconfig() {
	FIL cfile;
	FRESULT fr;
	UINT readbytes = 24;
	char*t;

	fr = f_open(&cfile, CONFFILE, FA_OPEN_EXISTING | FA_READ);
	if (fr)
		return fr;

	fr = f_read(&cfile, filename, readbytes - 1, &readbytes);
	if (fr)
		return fr;

	filename[readbytes] = '\0';
	t = filename;
	animPause = 0;
	while ((*t != '\n') && (*t != '\r') && (*t != '\0'))
		t++;
	while ((*t == '\n') || (*t == '\r'))
		*t++ = '\0';
	while ((*t >= '0') && (*t <= '9'))
		animPause = 10 * animPause + ((*t++) - '0');
	if (animPause == 0)
		animPause = 100;
	f_close(&cfile);
	return 0;
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
		" Choose BMP",
		" Anim delay",
		" Main menu",
		NULL };
#define MENUITEMS 4

int menuhandling() {
	int cx, pos = 0;
	do {
		lcdClear();
		lcdPrintln("BMP NICK");
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
}

void writeconfig() {
	FIL cf;
	FRESULT fr;
	unsigned int written, dec;
	int cx;
	char tmp[9];

	fr = f_open(&cf, "BMPNICK.CFG", FA_WRITE | FA_OPEN_ALWAYS);
	if (fr)
		return;

	f_write(&cf, filename, strlen(filename), &written);
	f_write(&cf, "\n", 1, &written);
	if (animPause <= 0)
		animPause = 100;

	dec = animPause;
	for (cx = 8; (cx >= 0) && (dec != 0); cx--) {
		tmp[cx] = '0' + (dec % 10);
		dec /= 10;
	}
	cx++;

	f_write(&cf, &tmp[cx], 9 - cx, &written);
	f_write(&cf, "\n", 1, &written);
	f_close(&cf);
}

void choosedelay() {
	int res, tmp = animPause;
	do {
		lcdClear();
		lcdPrintln("Frame delay");
		lcdPrintln(" (in ms)");
		lcdPrintln("");
		lcdPrintInt(tmp);

		lcdRefresh();
		res = getInputWaitDebounce();
		if (res == BTN_UP)
			tmp += 100;
		else if (res == BTN_DOWN)
			tmp -= 100;
	} while ((res != BTN_LEFT) && (res != BTN_ENTER));

	if (res == BTN_ENTER) {
		animPause = tmp;
		writeconfig();
	}
}

//shows *.bmp
void ram_real(void) {
	FIL file;
	FRESULT res;
	int size, curframe, key, menuselection;

	nframes = 0;

	res = openfilebyconfig();
	if (res)
		goto display;

	do {
		res = f_open(&file, (const char*) filename, FA_OPEN_EXISTING | FA_READ);
		if ((res) || (parseBitmap(&file, &size) == EPICFAIL)) {
			errorhandling:
			lcdClear();
			lcdPrintln("Error while");
			lcdPrintln("loading:");
			lcdPrintln(filename);
			lcdPrintln("");
			lcdPrintln("Press any");
			lcdPrintln("        key...");
			lcdRefresh();
			nframes = 0;
			getInputWaitDebounce();
		}

		display: switch (nframes) {
		case 0:
			displayScreen(info);
			key = getInputWaitDebounce();
			break;
		case 1:
			if (displayframe(&file, size) == EPICFAIL)
				goto errorhandling;
			key = getInputWaitDebounce();
			break;
		default:
			while (true) {
				for (curframe = 0; curframe < nframes; curframe++) {
					if (displayframe(&file, size) == EPICFAIL)
						goto errorhandling;
					if ((key = getInputWaitTimeout(animPause)) != BTN_NONE)
						goto exitloop;
				}
				f_lseek(&file, BMPHEADERSIZE);
			}
		}
		exitloop:
		f_close(&file);

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
					selectFile(filename, "BMP");
					writeconfig();
					break;
				case 2:
					choosedelay();
					break;
				case 3:
					key = BTN_RIGHT;
				default:
					goto exitmenu;
				}
			} while (menuselection >= 0);
		} /* if */
		exitmenu: menuselection = -1;
	} while (key == BTN_ENTER);

	return;
}

