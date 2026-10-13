// SPDX-License-Identifier: GPL-2.0
/*
 * oled_temp - show the LM75 temperature on a 128x32 SSD1306 OLED
 *
 * Glue between two independent drivers, entirely in user space:
 *   read : /sys/class/hwmon/hwmonN/temp1_input   (LM75 hwmon driver)
 *   write: /dev/oled, 512 bytes per frame        (OLED misc driver)
 *
 * Usage:
 *   oled_temp                 # hwmon name "lm75", output /dev/oled
 *   oled_temp -n my_lm75      # if your driver registers another hwmon name
 *   oled_temp -d /dev/xxx     # another OLED node
 *   oled_temp -f FILE         # read millidegrees from FILE instead (testing)
 *   oled_temp -a              # no OLED needed: draw the frame as ASCII art
 *   oled_temp -1              # one frame, then exit
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W	128
#define H	32
#define FB_SIZE	(W * H / 8)	/* 512 bytes */

/*
 * SSD1306 memory layout: 4 "pages" of 8 rows. Each byte is one COLUMN of
 * 8 pixels in a page, bit 0 at the top:
 *   fb[page * 128 + x], bit (y % 8)
 */
static unsigned char fb[FB_SIZE];

static void set_pixel(int x, int y)
{
	if (x < 0 || x >= W || y < 0 || y >= H)
		return;
	fb[(y / 8) * W + x] |= 1u << (y % 8);
}

/*
 * 5x7 font, also stored column by column with bit 0 at the top - the same
 * orientation as SSD1306, so a glyph column maps straight onto a pixel column.
 */
struct glyph {
	char ch;
	unsigned char col[5];
};

static const struct glyph font[] = {
	{ '0', { 0x3E, 0x51, 0x49, 0x45, 0x3E } },
	{ '1', { 0x00, 0x42, 0x7F, 0x40, 0x00 } },
	{ '2', { 0x42, 0x61, 0x51, 0x49, 0x46 } },
	{ '3', { 0x21, 0x41, 0x45, 0x4B, 0x31 } },
	{ '4', { 0x18, 0x14, 0x12, 0x7F, 0x10 } },
	{ '5', { 0x27, 0x45, 0x45, 0x45, 0x39 } },
	{ '6', { 0x3C, 0x4A, 0x49, 0x49, 0x30 } },
	{ '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } },
	{ '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } },
	{ '9', { 0x06, 0x49, 0x49, 0x29, 0x1E } },
	{ '.', { 0x00, 0x60, 0x60, 0x00, 0x00 } },
	{ '-', { 0x08, 0x08, 0x08, 0x08, 0x08 } },
	{ ':', { 0x00, 0x36, 0x36, 0x00, 0x00 } },
	{ '*', { 0x00, 0x06, 0x09, 0x09, 0x06 } },	/* degree sign */
	{ 'C', { 0x3E, 0x41, 0x41, 0x41, 0x22 } },
	{ 'E', { 0x7F, 0x49, 0x49, 0x49, 0x41 } },
	{ 'L', { 0x7F, 0x40, 0x40, 0x40, 0x40 } },
	{ 'M', { 0x7F, 0x02, 0x0C, 0x02, 0x7F } },
	{ 'P', { 0x7F, 0x09, 0x09, 0x09, 0x06 } },
	{ 'R', { 0x7F, 0x09, 0x19, 0x29, 0x46 } },
	{ 'T', { 0x01, 0x01, 0x7F, 0x01, 0x01 } },
	{ ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
};

static const unsigned char *find_glyph(char c)
{
	for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); i++)
		if (font[i].ch == c)
			return font[i].col;
	return font[sizeof(font) / sizeof(font[0]) - 1].col;	/* blank */
}

/* Draw text with its top-left corner at (x, y), each font pixel = s x s */
static int draw_text(int x, int y, const char *s, int scale)
{
	for (; *s; s++) {
		const unsigned char *g = find_glyph(*s);

		for (int cx = 0; cx < 5; cx++)
			for (int cy = 0; cy < 8; cy++)
				if (g[cx] & (1u << cy))
					for (int dx = 0; dx < scale; dx++)
						for (int dy = 0; dy < scale; dy++)
							set_pixel(x + cx * scale + dx,
								  y + cy * scale + dy);
		x += 6 * scale;			/* 5 columns + 1 gap */
	}
	return x;
}

static int text_width(const char *s, int scale)
{
	return (int)strlen(s) * 6 * scale - scale;
}

/* Find /sys/class/hwmon/hwmonN whose "name" file matches */
static int find_hwmon(const char *name, char *path, size_t len)
{
	DIR *d = opendir("/sys/class/hwmon");
	struct dirent *e;
	int found = 0;

	if (!d)
		return 0;
	while (!found && (e = readdir(d))) {
		char p[512], buf[64];
		FILE *f;

		if (strncmp(e->d_name, "hwmon", 5))
			continue;
		snprintf(p, sizeof(p), "/sys/class/hwmon/%s/name", e->d_name);
		f = fopen(p, "r");
		if (!f)
			continue;
		if (fgets(buf, sizeof(buf), f)) {
			buf[strcspn(buf, "\n")] = '\0';
			if (!strcmp(buf, name)) {
				snprintf(path, len,
					 "/sys/class/hwmon/%s/temp1_input",
					 e->d_name);
				found = 1;
			}
		}
		fclose(f);
	}
	closedir(d);
	return found;
}

/* hwmon reports millidegrees Celsius; re-read the file every time */
static int read_temp(const char *path, long *mdeg)
{
	FILE *f = fopen(path, "r");
	int ok;

	if (!f)
		return 0;
	ok = fscanf(f, "%ld", mdeg) == 1;
	fclose(f);
	return ok;
}

static void render(int have_temp, long mdeg)
{
	char big[32];
	int w;

	memset(fb, 0, sizeof(fb));
	draw_text(0, 0, "TEMP", 1);			/* page 0: small label */

	if (have_temp) {
		long tenths = (mdeg >= 0 ? mdeg + 50 : mdeg - 50) / 100;
		long whole = tenths / 10, frac = labs(tenths % 10);

		snprintf(big, sizeof(big), "%s%ld.%ld*C",
			 (tenths < 0 && whole == 0) ? "-" : "", whole, frac);
	} else {
		snprintf(big, sizeof(big), "ERR");
	}

	/* pages 1..3: 3x scaled (24 px tall), right-aligned */
	w = text_width(big, 3);
	draw_text(W - w > 0 ? W - w : 0, 8, big, 3);
}

static void print_ascii(void)
{
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++)
			putchar(fb[(y / 8) * W + x] & (1u << (y % 8)) ? '#' : '.');
		putchar('\n');
	}
	putchar('\n');
}

int main(int argc, char *argv[])
{
	const char *name = "lm75", *dev = "/dev/oled";
	char temp_path[512];
	int ascii = 0, once = 0, fd = -1, opt;

	temp_path[0] = '\0';
	while ((opt = getopt(argc, argv, "n:d:f:a1")) != -1) {
		switch (opt) {
		case 'n': name = optarg; break;
		case 'f': snprintf(temp_path, sizeof(temp_path), "%s", optarg); break;
		case 'd': dev = optarg; break;
		case 'a': ascii = 1; break;
		case '1': once = 1; break;
		default:
			fprintf(stderr, "usage: %s [-n hwmon_name | -f temp_file] [-d dev] [-a] [-1]\n",
				argv[0]);
			return 1;
		}
	}

	if (!temp_path[0] && !find_hwmon(name, temp_path, sizeof(temp_path))) {
		fprintf(stderr, "no hwmon device named \"%s\" "
			"(check: cat /sys/class/hwmon/*/name)\n", name);
		return 1;
	}
	fprintf(stderr, "reading %s\n", temp_path);

	if (!ascii) {
		fd = open(dev, O_WRONLY);
		if (fd < 0) {
			perror(dev);
			return 1;
		}
	}

	for (;;) {
		long mdeg = 0;
		int ok = read_temp(temp_path, &mdeg);

		render(ok, mdeg);
		if (ascii) {
			print_ascii();
		} else if (pwrite(fd, fb, FB_SIZE, 0) != FB_SIZE) {
			perror("write");	/* keep going: the OLED may come back */
		}
		if (once)
			break;
		sleep(1);
	}

	if (fd >= 0)
		close(fd);
	return 0;
}
