#ifndef ENVYAS
static uint32_t
NVC0FP_Source[] = {
	0x00021462,
	0x00000000,
	0x00000000,
	0x00000000,
	0x00000000,
	0x80000000,
	0x0000000a,
	0x00000000,
	0x00000000,
	0x00000000,
	0x00000000,
	0x00000000,
	0x00000000,
	0x00000000,
	0x00000000,
	0x00000000,
	0x00000000,
	0x00000000,
	0x0000000f,
	0x00000000,
#include "exascnvc0.fpc"
};
#else

interp pass f32 $r0 a[0x7c] 0x0 0x0
rcp f32 $r0 $r0
interp mul f32 $r1 a[0x84] $r0 0x0
interp mul f32 $r0 a[0x80] $r0 0x0
tex t lauto live dfp $r0:$r1:$r2:$r3 t2d $t0 $s0 $r0:$r1 ()
exit
#endif
