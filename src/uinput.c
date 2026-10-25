#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>

#define XK_LATIN1
#define XK_MISCELLANY
#include "keysymdef.h"
#include "uinput.h"
#ifndef NC4
#include <im.h>
#endif

#ifndef NC4
static int fd = 0;

// x11vnc/src/uinput.c
// TODO: proper error handling/cleanup
int initialize_uinput(void)
{
	int i;
	char *s;
	struct uinput_user_dev udev;

	fd = open("/dev/uinput", O_WRONLY | O_NDELAY);

	if (fd < 0) {
		printf("Failed to open uinput dev\n");
		return -1;
	}

	memset(&udev, 0, sizeof(udev));

	strncpy(udev.name, "vramvnc injector", UINPUT_MAX_NAME_SIZE);

	udev.id.bustype = BUS_USB;
	udev.id.version = 4;

	ioctl(fd, UI_SET_EVBIT, EV_REL);
	ioctl(fd, UI_SET_RELBIT, REL_X);
	ioctl(fd, UI_SET_RELBIT, REL_Y);
	ioctl(fd, UI_SET_RELBIT, REL_WHEEL);

	ioctl(fd, UI_SET_EVBIT, EV_KEY);

	ioctl(fd, UI_SET_EVBIT, EV_SYN);

	for (i=0; i < 256; i++) {
		ioctl(fd, UI_SET_KEYBIT, i);
	}

	ioctl(fd, UI_SET_KEYBIT, BTN_MOUSE);
	ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
	ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
	ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
	ioctl(fd, UI_SET_KEYBIT, BTN_FORWARD);
	ioctl(fd, UI_SET_KEYBIT, BTN_BACK);
	ioctl(fd, UI_SET_KEYBIT, KEY_PREVIOUS);

	ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);

	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	ioctl(fd, UI_SET_ABSBIT, ABS_X);
	ioctl(fd, UI_SET_ABSBIT, ABS_Y);
	udev.absmin[ABS_X] = 0;
	udev.absmax[ABS_X] = 1920; // XXX: hardcoded
	udev.absfuzz[ABS_X] = 0;
	udev.absflat[ABS_X] = 0;
	udev.absmin[ABS_Y] = 0;
	udev.absmax[ABS_Y] = 1080; // XXX: hardcoded
	udev.absfuzz[ABS_Y] = 0;
	udev.absflat[ABS_Y] = 0;

	write(fd, &udev, sizeof(udev));

	if (ioctl(fd, UI_DEV_CREATE) != 0) {
		printf("UI_DEV_CREATE ioctl failed\n");
		return -1;
	}

	if (IM_RequestToCheckInput() < 0) {
		printf("IM_RequestToCheckInput() failed\n");
		return -1;
	}

	return 0;
}

// x11vnc/src/uinput.c
void shutdown_uinput(void) {
	if (fd >= 0) {
		ioctl(fd, UI_DEV_DESTROY);
		close(fd);
		fd = -1;
	}
}

// x11vnc/src/uinput.c
void ptr_abs(int x, int y, int p) {
	struct input_event ev;
	struct timeval tval;
	int x0, y0;

	memset(&ev, 0, sizeof(ev));

	gettimeofday(&tval, NULL);
	ev.time.tv_sec = tval.tv_sec;
	ev.time.tv_usec = tval.tv_usec;
	ev.type = EV_ABS;
	ev.code = ABS_Y;
	ev.value = y;
	write(fd, &ev, sizeof(ev));

	ev.type = EV_ABS;
	ev.code = ABS_X;
	ev.value = x;
	write(fd, &ev, sizeof(ev));

	if (p >= 0) {
		ev.type = EV_ABS;
		ev.code = ABS_PRESSURE;
		ev.value = p;
		write(fd, &ev, sizeof(ev));

		if (p & 0x08 || p & 0x10) {
			// Scroll wheel
			ev.type = EV_REL;
			ev.code = REL_WHEEL;
			ev.value = p & 0x08 ? 1 : -1;
			write(fd, &ev, sizeof(ev));
		} else {
			// Mouse button
			ev.type = EV_KEY;
			ev.code = BTN_LEFT;
			ev.value = p & 0x01;
			write(fd, &ev, sizeof(ev));

			ev.type = EV_KEY;
			ev.code = KEY_PREVIOUS;
			ev.value = p & 0x04;
			write(fd, &ev, sizeof(ev));
		}
	}

	ev.type = EV_SYN;
	ev.code = SYN_REPORT;
	ev.value = 0;
	write(fd, &ev, sizeof(ev));
}

// x11vnc/src/uinput.c
void uinput_key_command(int down, int keysym) {
	struct input_event ev;
	struct timeval tval;
	int scancode;

	scancode = lookup_code(keysym);

	if (scancode < 0) {
		return;
	}

	memset(&ev, 0, sizeof(ev));
	gettimeofday(&tval, NULL);
	ev.time.tv_sec = tval.tv_sec;
	ev.time.tv_usec = tval.tv_usec;
	ev.type = EV_KEY;
	ev.code = (unsigned char) scancode;
	ev.value = down;

	write(fd, &ev, sizeof(ev));

	ev.type = EV_SYN;
	ev.code = SYN_REPORT;
	ev.value = 0;
	write(fd, &ev, sizeof(ev));
}

#else /* NC4 */

/*
 * procmgr reads the input devices itself: a device with EV_REL is a mouse
 * (whose ABS_X/ABS_Y set the cursor position in 1280x720 coordinates), one
 * with only EV_KEY a keyboard. It only scans /dev/input when told that a new
 * device was attached, with key code 1266 on one of the devices it reads, e.g.
 * its own "LGE KEY RETURNPATH".
 */
#define NC4_KEY_NEW_DEVICE 1266
#define NC4_SCREEN_W 1280
#define NC4_SCREEN_H 720

static int kbd_fd = -1;
static int mouse_fd = -1;

static void emit(int fd, int type, int code, int value) {
	struct input_event ev;

	memset(&ev, 0, sizeof(ev));
	gettimeofday(&ev.time, NULL);
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		perror("uinput write");
}

static int create_device(const char* name, int mouse) {
	struct uinput_user_dev udev;
	int fd, i;

	fd = open("/dev/uinput", O_WRONLY | O_NDELAY);
	if (fd < 0) {
		perror("/dev/uinput");
		return -1;
	}

	memset(&udev, 0, sizeof(udev));
	strncpy(udev.name, name, UINPUT_MAX_NAME_SIZE - 1);
	udev.id.bustype = BUS_USB;
	udev.id.version = 1;

	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	if (mouse) {
		ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
		ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
		ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
		ioctl(fd, UI_SET_EVBIT, EV_REL);
		ioctl(fd, UI_SET_RELBIT, REL_X);
		ioctl(fd, UI_SET_RELBIT, REL_Y);
		ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
		ioctl(fd, UI_SET_EVBIT, EV_ABS);
		ioctl(fd, UI_SET_ABSBIT, ABS_X);
		ioctl(fd, UI_SET_ABSBIT, ABS_Y);
		udev.absmax[ABS_X] = NC4_SCREEN_W - 1;
		udev.absmax[ABS_Y] = NC4_SCREEN_H - 1;
	} else {
		for (i = 1; i < 256; i++)
			ioctl(fd, UI_SET_KEYBIT, i);
	}

	if (write(fd, &udev, sizeof(udev)) != sizeof(udev) || ioctl(fd, UI_DEV_CREATE) != 0) {
		perror("UI_DEV_CREATE");
		close(fd);
		return -1;
	}
	return fd;
}

/* tell procmgr to scan /dev/input again */
static int request_input_scan(void) {
	char path[64], name[64];
	int i, fd;

	for (i = 0; i < 32; i++) {
		FILE* f;

		snprintf(path, sizeof(path), "/sys/class/input/event%d/device/name", i);
		if (!(f = fopen(path, "r")))
			continue;
		name[0] = 0;
		fgets(name, sizeof(name), f);
		fclose(f);
		if (strncmp(name, "LGE KEY RETURNPATH", 18) != 0)
			continue;

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		if ((fd = open(path, O_WRONLY)) < 0) {
			perror(path);
			return -1;
		}
		emit(fd, EV_KEY, NC4_KEY_NEW_DEVICE, 1);
		emit(fd, EV_KEY, NC4_KEY_NEW_DEVICE, 0);
		emit(fd, EV_SYN, SYN_REPORT, 0);
		close(fd);
		return 0;
	}
	printf("LGE KEY RETURNPATH input device not found\n");
	return -1;
}

int initialize_uinput(void)
{
	if ((kbd_fd = create_device("vncserver keyboard", 0)) < 0)
		return -1;
	if ((mouse_fd = create_device("vncserver mouse", 1)) < 0) {
		shutdown_uinput();
		return -1;
	}
	if (request_input_scan() != 0) {
		shutdown_uinput();
		return -1;
	}
	return 0;
}

void shutdown_uinput(void) {
	/* procmgr notices the removal by itself */
	if (kbd_fd >= 0) {
		ioctl(kbd_fd, UI_DEV_DESTROY);
		close(kbd_fd);
		kbd_fd = -1;
	}
	if (mouse_fd >= 0) {
		ioctl(mouse_fd, UI_DEV_DESTROY);
		close(mouse_fd);
		mouse_fd = -1;
	}
}

void ptr_abs(int x, int y, int p) {
	static int buttons = 0;

	if (mouse_fd < 0)
		return;

	emit(mouse_fd, EV_ABS, ABS_X, x);
	emit(mouse_fd, EV_ABS, ABS_Y, y);
	emit(mouse_fd, EV_SYN, SYN_REPORT, 0);

	if (p & 0x08 || p & 0x10) {
		emit(mouse_fd, EV_REL, REL_WHEEL, p & 0x08 ? 1 : -1);
		emit(mouse_fd, EV_SYN, SYN_REPORT, 0);
		return;
	}
	/* button changes only, they are separate events for procmgr */
	if ((p ^ buttons) & 0x01)
		emit(mouse_fd, EV_KEY, BTN_LEFT, p & 0x01);
	if ((p ^ buttons) & 0x02)
		emit(mouse_fd, EV_KEY, BTN_MIDDLE, (p & 0x02) != 0);
	if ((p ^ buttons) & 0x04)
		emit(mouse_fd, EV_KEY, BTN_RIGHT, (p & 0x04) != 0);
	if ((p ^ buttons) & 0x07)
		emit(mouse_fd, EV_SYN, SYN_REPORT, 0);
	buttons = p & 0x07;
}

void uinput_key_command(int down, int keysym) {
	int scancode = lookup_code(keysym);

	if (scancode < 0 || kbd_fd < 0)
		return;

	emit(kbd_fd, EV_KEY, scancode, down);
	emit(kbd_fd, EV_SYN, SYN_REPORT, 0);
}

#endif /* NC4 */

// x11vnc/src/uinput.c
int lookup_code(int keysym) {

	if (keysym == 0) {
		return -1;
	}

	switch(keysym) {
	case XK_Escape:	return KEY_ESC;
	case XK_1:		return KEY_1;
	case XK_2:		return KEY_2;
	case XK_3:		return KEY_3;
	case XK_4:		return KEY_4;
	case XK_5:		return KEY_5;
	case XK_6:		return KEY_6;
	case XK_7:		return KEY_7;
	case XK_8:		return KEY_8;
	case XK_9:		return KEY_9;
	case XK_0:		return KEY_0;
	case XK_exclam:	return KEY_1;
	case XK_at:		return KEY_2;
	case XK_numbersign:	return KEY_3;
	case XK_dollar:	return KEY_4;
	case XK_percent:	return KEY_5;
	case XK_asciicircum:	return KEY_6;
	case XK_ampersand:	return KEY_7;
	case XK_asterisk:	return KEY_8;
	case XK_parenleft:	return KEY_9;
	case XK_parenright:	return KEY_0;
	case XK_minus:	return KEY_MINUS;
	case XK_underscore:	return KEY_MINUS;
	case XK_equal:	return KEY_EQUAL;
	case XK_plus:	return KEY_EQUAL;
	case XK_BackSpace:	return KEY_BACKSPACE;
	case XK_Tab:		return KEY_TAB;
	case XK_q:		return KEY_Q;
	case XK_Q:		return KEY_Q;
	case XK_w:		return KEY_W;
	case XK_W:		return KEY_W;
	case XK_e:		return KEY_E;
	case XK_E:		return KEY_E;
	case XK_r:		return KEY_R;
	case XK_R:		return KEY_R;
	case XK_t:		return KEY_T;
	case XK_T:		return KEY_T;
	case XK_y:		return KEY_Y;
	case XK_Y:		return KEY_Y;
	case XK_u:		return KEY_U;
	case XK_U:		return KEY_U;
	case XK_i:		return KEY_I;
	case XK_I:		return KEY_I;
	case XK_o:		return KEY_O;
	case XK_O:		return KEY_O;
	case XK_p:		return KEY_P;
	case XK_P:		return KEY_P;
	case XK_braceleft:	return KEY_LEFTBRACE;
	case XK_braceright:	return KEY_RIGHTBRACE;
	case XK_bracketleft:	return KEY_LEFTBRACE;
	case XK_bracketright:	return KEY_RIGHTBRACE;
	case XK_Return:	return KEY_ENTER;
	case XK_Control_L:	return KEY_LEFTCTRL;
	case XK_a:		return KEY_A;
	case XK_A:		return KEY_A;
	case XK_s:		return KEY_S;
	case XK_S:		return KEY_S;
	case XK_d:		return KEY_D;
	case XK_D:		return KEY_D;
	case XK_f:		return KEY_F;
	case XK_F:		return KEY_F;
	case XK_g:		return KEY_G;
	case XK_G:		return KEY_G;
	case XK_h:		return KEY_H;
	case XK_H:		return KEY_H;
	case XK_j:		return KEY_J;
	case XK_J:		return KEY_J;
	case XK_k:		return KEY_K;
	case XK_K:		return KEY_K;
	case XK_l:		return KEY_L;
	case XK_L:		return KEY_L;
	case XK_semicolon:	return KEY_SEMICOLON;
	case XK_colon:	return KEY_SEMICOLON;
	case XK_apostrophe:	return KEY_APOSTROPHE;
	case XK_quotedbl:	return KEY_APOSTROPHE;
	case XK_grave:	return KEY_GRAVE;
	case XK_asciitilde:	return KEY_GRAVE;
	case XK_Shift_L:	return KEY_LEFTSHIFT;
	case XK_backslash:	return KEY_BACKSLASH;
	case XK_bar:		return KEY_BACKSLASH;
	case XK_z:		return KEY_Z;
	case XK_Z:		return KEY_Z;
	case XK_x:		return KEY_X;
	case XK_X:		return KEY_X;
	case XK_c:		return KEY_C;
	case XK_C:		return KEY_C;
	case XK_v:		return KEY_V;
	case XK_V:		return KEY_V;
	case XK_b:		return KEY_B;
	case XK_B:		return KEY_B;
	case XK_n:		return KEY_N;
	case XK_N:		return KEY_N;
	case XK_m:		return KEY_M;
	case XK_M:		return KEY_M;
	case XK_comma:	return KEY_COMMA;
	case XK_less:	return KEY_COMMA;
	case XK_period:	return KEY_DOT;
	case XK_greater:	return KEY_DOT;
	case XK_slash:	return KEY_SLASH;
	case XK_question:	return KEY_SLASH;
	case XK_Shift_R:	return KEY_RIGHTSHIFT;
	case XK_KP_Multiply:	return KEY_KPASTERISK;
	case XK_Alt_L:	return KEY_LEFTALT;
	case XK_space:	return KEY_SPACE;
	case XK_Caps_Lock:	return KEY_CAPSLOCK;
	case XK_F1:		return KEY_F1;
	case XK_F2:		return KEY_F2;
	case XK_F3:		return KEY_F3;
	case XK_F4:		return KEY_F4;
	case XK_F5:		return KEY_F5;
	case XK_F6:		return KEY_F6;
	case XK_F7:		return KEY_F7;
	case XK_F8:		return KEY_F8;
	case XK_F9:		return KEY_F9;
	case XK_F10:		return KEY_F10;
	case XK_Num_Lock:	return KEY_NUMLOCK;
	case XK_Scroll_Lock:	return KEY_SCROLLLOCK;
	case XK_KP_7:		return KEY_KP7;
	case XK_KP_8:		return KEY_KP8;
	case XK_KP_9:		return KEY_KP9;
	case XK_KP_Subtract:	return KEY_KPMINUS;
	case XK_KP_4:		return KEY_KP4;
	case XK_KP_5:		return KEY_KP5;
	case XK_KP_6:		return KEY_KP6;
	case XK_KP_Add:	return KEY_KPPLUS;
	case XK_KP_1:		return KEY_KP1;
	case XK_KP_2:		return KEY_KP2;
	case XK_KP_3:		return KEY_KP3;
	case XK_KP_0:		return KEY_KP0;
	case XK_KP_Decimal:	return KEY_KPDOT;
	case XK_F13:		return KEY_F13;
	case XK_F11:		return KEY_F11;
	case XK_F12:		return KEY_F12;
	case XK_F14:		return KEY_F14;
	case XK_F15:		return KEY_F15;
	case XK_F16:		return KEY_F16;
	case XK_F17:		return KEY_F17;
	case XK_F18:		return KEY_F18;
	case XK_F19:		return KEY_F19;
	case XK_F20:		return KEY_F20;
	case XK_KP_Enter:	return KEY_KPENTER;
	case XK_Control_R:	return KEY_RIGHTCTRL;
	case XK_KP_Divide:	return KEY_KPSLASH;
	case XK_Sys_Req:	return KEY_SYSRQ;
	case XK_Alt_R:	return KEY_RIGHTALT;
	case XK_Linefeed:	return KEY_LINEFEED;
	case XK_Home:		return KEY_HOME;
	case XK_Up:		return KEY_UP;
	case XK_Page_Up:	return KEY_PAGEUP;
	case XK_Left:		return KEY_LEFT;
	case XK_Right:	return KEY_RIGHT;
	case XK_End:		return KEY_END;
	case XK_Down:		return KEY_DOWN;
	case XK_Page_Down:	return KEY_PAGEDOWN;
	case XK_Insert:	return KEY_INSERT;
	case XK_Delete:	return KEY_DELETE;
	case XK_KP_Equal:	return KEY_KPEQUAL;
	case XK_Pause:	return KEY_PAUSE;
	case XK_F21:		return KEY_F21;
	case XK_F22:		return KEY_F22;
	case XK_F23:		return KEY_F23;
	case XK_F24:		return KEY_F24;
	case XK_KP_Separator:	return KEY_KPCOMMA;
	case XK_Meta_L:	return KEY_LEFTMETA;
	case XK_Meta_R:	return KEY_RIGHTMETA;
	case XK_Multi_key:	return KEY_COMPOSE;
	case XK_Super_L:	return KEY_LEFTMETA; // win key
	default:		return -1;
	}
}
