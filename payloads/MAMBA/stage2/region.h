#ifndef __REGION_H__
#define __REGION_H__

#include <lv1/inttypes.h>

extern u8 dvd_video_region;
extern u8 bd_video_region;

enum 
{
	DVD_VIDEO_REGION_1 = 1,
	DVD_VIDEO_REGION_2 = 2,
	DVD_VIDEO_REGION_3 = 4,
	DVD_VIDEO_REGION_4 = 8,
	DVD_VIDEO_REGION_5 = 16,
	DVD_VIDEO_REGION_6 = 32,
	DVD_VIDEO_REGION_MAX = DVD_VIDEO_REGION_6
};

enum
{
	BD_VIDEO_REGION_A = 1,
	BD_VIDEO_REGION_B = 2,
	BD_VIDEO_REGION_C = 4,
	BD_VIDEO_REGION_MAX = BD_VIDEO_REGION_C
};

void region_patches(void);

#ifdef PS3M_API

void unhook_all_region(void);

#endif

#endif
