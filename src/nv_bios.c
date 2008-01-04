/*
 * Copyright 2005-2006 Erik Waling
 * Copyright 2006 Stephane Marchesin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "nv_include.h"
#include "nvreg.h"
#include <byteswap.h>

/* FIXME: put these somewhere */
#define CRTC_INDEX_COLOR (VGA_IOBASE_COLOR + VGA_CRTC_INDEX_OFFSET)
#define NV_VGA_CRTCX_OWNER_HEADA 0x0
#define NV_VGA_CRTCX_OWNER_HEADB 0x3
#define NV_PBUS_PCI_NV_19 0x0000184C
#define NV_PBUS_PCI_NV_20 0x00001850
#define NV_PBUS_PCI_NV_20_ROM_SHADOW_DISABLED 0x00000000
#define NV_PBUS_PCI_NV_20_ROM_SHADOW_ENABLED 0x00000001
#define NV_PEXTDEV_BOOT_0 0x00101000
/* undef, as we want the +0x00100000 version */
#undef NV_PFB_CFG0
#define NV_PFB_CFG0 0x00100200
#define NV_PFB_REFCTRL 0x00100210
#define NV_PFB_REFCTRL_VALID_1 0x80000000
#define NV_PRAMIN_ROM_OFFSET 0x00700000

#define DEBUGLEVEL 6

/* TODO: 
 *       * PLL algorithms.
 */

static int crtchead = 0;

/* this will need remembering across a suspend */
static uint32_t saved_nv_pfb_cfg0;

typedef struct {
	Bool execute;
	Bool repeat;
} init_exec_t;

static uint16_t le16_to_cpu(const uint16_t x)
{
#if X_BYTE_ORDER == X_BIG_ENDIAN
	return bswap_16(x);
#else
	return x;
#endif
}

static uint32_t le32_to_cpu(const uint32_t x)
{
#if X_BYTE_ORDER == X_BIG_ENDIAN
	return bswap_32(x);
#else
	return x;
#endif
}

static Bool nv_cksum(const uint8_t *data, unsigned int length)
{
	/* there's a few checksums in the BIOS, so here's a generic checking function */
	int i;
	uint8_t sum = 0;

	for (i = 0; i < length; i++)
		sum += data[i];

	if (sum)
		return TRUE;

	return FALSE;
}

static int NVValidVBIOS(ScrnInfoPtr pScrn, const uint8_t *data)
{
	/* check for BIOS signature */
	if (!(data[0] == 0x55 && data[1] == 0xAA)) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "... BIOS signature not found\n");
		return 0;
	}

	if (nv_cksum(data, data[2] * 512)) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "... BIOS checksum invalid\n");
		/* probably ought to set a do_not_execute flag for table parsing here,
		 * assuming most BIOSen are valid */
		return 1;
	} else
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "... appears to be valid\n");

	return 2;
}

static void NVShadowVBIOS_PROM(ScrnInfoPtr pScrn, uint8_t *data)
{
	NVPtr pNv = NVPTR(pScrn);
	int i;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Attempting to locate BIOS image in PROM\n");

	/* enable ROM access */
	nvWriteMC(pNv, NV_PBUS_PCI_NV_20, NV_PBUS_PCI_NV_20_ROM_SHADOW_DISABLED);
	for (i = 0; i < NV_PROM_SIZE; i++) {
		/* according to nvclock, we need that to work around a 6600GT/6800LE bug */
		data[i] = pNv->PROM[i];
		data[i] = pNv->PROM[i];
		data[i] = pNv->PROM[i];
		data[i] = pNv->PROM[i];
		data[i] = pNv->PROM[i];
	}
	/* disable ROM access */
	nvWriteMC(pNv, NV_PBUS_PCI_NV_20, NV_PBUS_PCI_NV_20_ROM_SHADOW_ENABLED);
}

static void NVShadowVBIOS_PRAMIN(ScrnInfoPtr pScrn, uint32_t *data)
{
	NVPtr pNv = NVPTR(pScrn);
	const uint32_t *pramin = (uint32_t *)&pNv->REGS[NV_PRAMIN_ROM_OFFSET/4];
	uint32_t old_bar0_pramin = 0;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Attempting to locate BIOS image in PRAMIN\n");

	if (pNv->Architecture >= NV_ARCH_50) {
		uint32_t vbios_vram;

		vbios_vram = (pNv->REGS[0x619f04/4] & ~0xff) << 8;
		if (!vbios_vram) {
			vbios_vram = pNv->REGS[0x1700/4] << 16;
			vbios_vram += 0xf0000;
		}

		old_bar0_pramin = pNv->REGS[0x1700/4];
		pNv->REGS[0x1700/4] = vbios_vram >> 16;
	}

	memcpy(data, pramin, NV_PROM_SIZE);

	if (pNv->Architecture >= NV_ARCH_50) {
		pNv->REGS[0x1700/4] = old_bar0_pramin;
	}
}

static Bool NVShadowVBIOS(ScrnInfoPtr pScrn, uint8_t *data)
{
	NVShadowVBIOS_PROM(pScrn, data);
	if (NVValidVBIOS(pScrn, data) == 2)
		return TRUE;

	NVShadowVBIOS_PRAMIN(pScrn, (uint32_t *)data);
	if (NVValidVBIOS(pScrn, data))
		return TRUE;

	return FALSE;
}

typedef struct {
	char* name;
	uint8_t id;
	int length;
	int length_offset;
	int length_multiplier;
	Bool (*handler)(ScrnInfoPtr pScrn, bios_t *, uint16_t, init_exec_t *);
} init_tbl_entry_t;

typedef struct {
	uint8_t id[2];
	uint16_t length;
	uint16_t offset;
} bit_entry_t;

static void parse_init_table(ScrnInfoPtr pScrn, bios_t *bios, unsigned int offset, init_exec_t *iexec);

#define MACRO_INDEX_SIZE        2
#define MACRO_SIZE              8
#define CONDITION_SIZE          12
#define IO_FLAG_CONDITION_SIZE  9 

void still_alive()
{
	sync();
//	usleep(200000);
}

static int nv_valid_reg(uint32_t reg)
{
	#define WITHIN(x,y,z) ((x>=y)&&(x<y+z))
	if (WITHIN(reg,NV_PRAMIN_OFFSET,NV_PRAMIN_SIZE))
		return 1;
	if (WITHIN(reg,NV_PCRTC0_OFFSET,NV_PCRTC0_SIZE))
		return 1;
	if (WITHIN(reg,NV_PRAMDAC0_OFFSET,NV_PRAMDAC0_SIZE))
		return 1;
	if (WITHIN(reg,NV_PFB_OFFSET,NV_PFB_SIZE))
		return 1;
	if (WITHIN(reg,NV_PFIFO_OFFSET,NV_PFIFO_SIZE))
		return 1;
	if (WITHIN(reg,NV_PGRAPH_OFFSET,NV_PGRAPH_SIZE))
		return 1;
	if (WITHIN(reg,NV_PEXTDEV_OFFSET,NV_PEXTDEV_SIZE))
		return 1;
	if (WITHIN(reg,NV_PTIMER_OFFSET,NV_PTIMER_SIZE))
		return 1;
	if (WITHIN(reg,NV_PVIDEO_OFFSET,NV_PVIDEO_SIZE))
		return 1;
	if (WITHIN(reg,NV_PMC_OFFSET,NV_PMC_SIZE))
		return 1;
	if (WITHIN(reg,NV_FIFO_OFFSET,NV_FIFO_SIZE))
		return 1;
	if (WITHIN(reg,NV_PCIO0_OFFSET,NV_PCIO0_SIZE))
		return 1;
	if (WITHIN(reg,NV_PDIO0_OFFSET,NV_PDIO0_SIZE))
		return 1;
	if (WITHIN(reg,NV_PVIO_OFFSET,NV_PVIO_SIZE))
		return 1;
	if (WITHIN(reg,NV_PROM_OFFSET,NV_PROM_SIZE))
		return 1;
	if (WITHIN(reg,NV_PRAMIN_ROM_OFFSET,NV_PROM_SIZE))
		return 1;
	/* A new PBUS? */
	if (WITHIN(reg,0x88000,0x1000))
		return 1;
	#undef WITHIN
	return 0;
}

static uint32_t nv32_rd(ScrnInfoPtr pScrn, uint32_t reg)
{
	NVPtr pNv = NVPTR(pScrn);
	uint32_t data;

	if (!nv_valid_reg(reg)) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "========= unknown reg 0x%08X ==========\n", reg);
		return 0;
	}
	data = pNv->REGS[reg/4];
	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "	Read:  Reg: 0x%08X, Data: 0x%08X\n", reg, data);

	return data;
}

static int nv32_wr(ScrnInfoPtr pScrn, uint32_t reg, uint32_t data)
{
	NVPtr pNv = NVPTR(pScrn);
	int specialcase = 0;
	uint8_t saved1 = 0, saved2 = 0;
	volatile uint8_t *crtcptr = crtchead ? pNv->PCIO1 : pNv->PCIO0;

	if (DEBUGLEVEL >= 8)
		nv32_rd(pScrn, reg);
	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "	Write: Reg: 0x%08X, Data: 0x%08X\n", reg, data);
	if (!nv_valid_reg(reg)) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "========= unknown reg 0x%08X ==========\n", reg);
		return 0;
	}

	if (pNv->VBIOS.execute) {
		still_alive();

		if ((reg & 0xffc) == 0x3c0) {
			specialcase = 1;
			saved1 = VGA_RD08(crtcptr, VGA_MISC_OUT_R);
			saved2 = VGA_RD08(crtcptr, VGA_ENABLE);
		}
		if ((reg & 0xffc) == 0x3cc) {
			specialcase = 2;
			saved1 = VGA_RD08(crtcptr, VGA_GRAPH_INDEX);
			VGA_WR08(crtcptr, VGA_GRAPH_INDEX, 0x06);
			saved2 = VGA_RD08(crtcptr, VGA_GRAPH_DATA);
		}

		pNv->REGS[reg/4] = data;

		if (specialcase == 1) {
			VGA_WR08(crtcptr, VGA_ENABLE, saved2);
			VGA_WR08(crtcptr, VGA_MISC_OUT_W, saved1);
		}
		if (specialcase == 2) {
			VGA_WR08(crtcptr, VGA_GRAPH_INDEX, 0x06);
			VGA_WR08(crtcptr, VGA_GRAPH_DATA, saved2);
			VGA_WR08(crtcptr, VGA_GRAPH_INDEX, saved1);
		}
	}

	return 1;
}

static uint8_t nv_idx_port_rd(ScrnInfoPtr pScrn, uint16_t port, uint8_t index)
{
	NVPtr pNv = NVPTR(pScrn);
	volatile uint8_t *ptr = crtchead ? pNv->PCIO1 : pNv->PCIO0;
	uint8_t data;

	VGA_WR08(ptr, port, index);
	data = VGA_RD08(ptr, port + 1);

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "	Indexed read:  Port: 0x%04X, Index: 0x%02X, Head: 0x%02X, Data: 0x%02X\n",
			   port, index, crtchead, data);

	return data;
}

static void nv_idx_port_wr(ScrnInfoPtr pScrn, uint16_t port, uint8_t index, uint8_t data)
{
	NVPtr pNv = NVPTR(pScrn);
	volatile uint8_t *ptr;

	/* The current head is maintained in a file scope variable crtchead.
	 * We trap changes to CRTCX_OWNER and update the head variable
	 * and hence the register set written.
	 * As CRTCX_OWNER only exists on CRTC0, we update crtchead to head0
	 * in advance of the write, and to head1 after the write
	 */
	if (port == CRTC_INDEX_COLOR && index == NV_VGA_CRTCX_OWNER && data != NV_VGA_CRTCX_OWNER_HEADB)
		crtchead = 0;
	ptr = crtchead ? pNv->PCIO1 : pNv->PCIO0;

	if (DEBUGLEVEL >= 8)
		nv_idx_port_rd(pScrn, port, index);
	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "	Indexed write: Port: 0x%04X, Index: 0x%02X, Head: 0x%02X, Data: 0x%02X\n",
			   port, index, crtchead, data);

	if (pNv->VBIOS.execute) {
		still_alive();
		VGA_WR08(ptr, port, index);
		VGA_WR08(ptr, port + 1, data);
	}

	if (port == CRTC_INDEX_COLOR && index == NV_VGA_CRTCX_OWNER && data == NV_VGA_CRTCX_OWNER_HEADB)
		crtchead = 1;
}

#define ACCESS_UNLOCK 0
#define ACCESS_LOCK 1
static void crtc_access(ScrnInfoPtr pScrn, Bool lock)
{
	int savedhead = crtchead;
	uint8_t cr11;

	nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, NV_VGA_CRTCX_OWNER, NV_VGA_CRTCX_OWNER_HEADA);
	nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, NV_VGA_CRTCX_LOCK, lock ? 0x99 : 0x57);
	cr11 = nv_idx_port_rd(pScrn, CRTC_INDEX_COLOR, NV_VGA_CRTCX_VSYNCE);
	nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, NV_VGA_CRTCX_VSYNCE, lock ? cr11 | 0x80 : cr11 & ~0x80);

	nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, NV_VGA_CRTCX_OWNER, NV_VGA_CRTCX_OWNER_HEADB);
	nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, NV_VGA_CRTCX_LOCK, lock ? 0x99 : 0x57);
	cr11 = nv_idx_port_rd(pScrn, CRTC_INDEX_COLOR, NV_VGA_CRTCX_VSYNCE);
	nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, NV_VGA_CRTCX_VSYNCE, lock ? cr11 | 0x80 : cr11 & ~0x80);

	crtchead = savedhead;
}

static Bool io_flag_condition(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, uint8_t cond)
{
	/* The IO flag condition entry has 2 bytes for the CRTC port; 1 byte
	 * for the CRTC index; 1 byte for the mask to apply to the value
	 * retrieved from the CRTC; 1 byte for the shift right to apply to the
	 * masked CRTC value; 2 bytes for the offset to the flag array, to
	 * which the shifted value is added; 1 byte for the mask applied to the
	 * value read from the flag array; and 1 byte for the value to compare
	 * against the masked byte from the flag table.
	 */

	uint16_t condptr = bios->io_flag_condition_tbl_ptr + cond * IO_FLAG_CONDITION_SIZE;
	uint16_t crtcport = le16_to_cpu(*((uint16_t *)(&bios->data[condptr])));
	uint8_t crtcindex = bios->data[condptr + 2];
	uint8_t mask = bios->data[condptr + 3];
	uint8_t shift = bios->data[condptr + 4];
	uint16_t flagarray = le16_to_cpu(*((uint16_t *)(&bios->data[condptr + 5])));
	uint8_t flagarraymask = bios->data[condptr + 7];
	uint8_t cmpval = bios->data[condptr + 8];
	uint8_t data;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Port: 0x%04X, Index: 0x%02X, Mask: 0x%02X, Shift: 0x%02X, FlagArray: 0x%04X, FAMask: 0x%02X, Cmpval: 0x%02X\n",
			   offset, crtcport, crtcindex, mask, shift, flagarray, flagarraymask, cmpval);

	data = nv_idx_port_rd(pScrn, crtcport, crtcindex);

	data = bios->data[flagarray + ((data & mask) >> shift)];
	data &= flagarraymask;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Checking if 0x%02X equals 0x%02X\n",
			   offset, data, cmpval);

	if (data == cmpval)
		return TRUE;

	return FALSE;
}

uint32_t getMNP_single(ScrnInfoPtr pScrn, uint32_t clk, int *bestNM, int *bestlog2P)
{
	/* Find M, N and P for a single stage PLL
	 *
	 * Note that some bioses (NV3x) have lookup tables of precomputed MNP
	 * values, but we're too lazy to use those atm
	 *
	 * "clk" parameter in kHz
	 * returns calculated clock
	 */

	bios_t *bios = &NVPTR(pScrn)->VBIOS;
	int maxM = 0, M, N;
	int maxlog2P, log2P, P;
	int crystal = 0;
	uint32_t minvco = bios->fminvco;
	uint32_t maxvco = bios->fmaxvco;
	int clkP;
	int calcclk, delta;
	unsigned int bestdelta = UINT_MAX;
	uint32_t bestclk = 0;

	unsigned int crystal_strap_mask = 1 << 6;
	/* open coded pNv->twoHeads test */
	if (bios->chip_version > 0x10 && bios->chip_version != 0x15 &&
	    bios->chip_version != 0x1a && bios->chip_version != 0x20)
		crystal_strap_mask |= 1 << 22;
	switch (nv32_rd(pScrn, NV_PEXTDEV_BOOT_0) & crystal_strap_mask) {
	case 0:
		maxM = 13;
		crystal = 13500;
		break;
	case (1 << 6):
		maxM = 14;
		crystal = 14318;
		break;
	case (1 << 22):
	case (1 << 22 | 1 << 6):
		maxM = 14;
		crystal = 27000;
		break;
	}

	/* this division verified for nv20, nv28 (Haiku), nv34 -- nv17 is guessed */
	/* possibly correlated with introduction of 27MHz crystal */
	if (bios->chip_version <= 0x16 || bios->chip_version == 0x20) {
		if (clk > 250000)
			maxM = 6;
		if (clk > 340000)
			maxM = 2;
		maxlog2P = 4;
	} else {
		if (clk > 150000)
			maxM = 6;
		if (clk > 200000)
			maxM = 4;
		if (clk > 340000)
			maxM = 2;
		maxlog2P = 5;
	}

	if ((clk << maxlog2P) < minvco) {
		minvco = clk << maxlog2P;
		maxvco = minvco * 2;
	}
	if (clk + clk/200 > maxvco)	/* +0.5% */
		maxvco = clk + clk/200;

	/* NV34 goes maxlog2P->0, NV20 goes 0->maxlog2P */
	for (log2P = 0; log2P <= maxlog2P; log2P++) {
		P = 1 << log2P;
		clkP = clk * P;
		if (clkP < minvco)
			continue;
		if (clkP > maxvco)
			return bestclk;

		/* nv_hw.c in nv driver uses 7 and 8 for minM */
		for (M = 1; M <= maxM; M++) {
			/* add crystal/2 to round better */
			N = (clkP * M + crystal/2) / crystal;
			if (N > 256)	/* we lost */
				goto nextP;

			/* more rounding additions */
			calcclk = ((N * crystal + P/2) / P + M/2) / M;
			delta = abs(calcclk - clk);
			/* we do an exhaustive search rather than terminating
			 * on an optimality condition...
			 */
			if (delta < bestdelta) {
				bestdelta = delta;
				bestclk = calcclk;
				*bestNM = N << 8 | M;
				*bestlog2P = log2P;
				if (delta == 0)	/* except this one */
					return bestclk;
			}
		}
nextP:
		continue;
	}

	return bestclk;
}

uint32_t getMNP_double(ScrnInfoPtr pScrn, struct pll_lims *pll_lim, uint32_t clk, int *bestNM1, int *bestNM2, int *bestlog2P)
{
	/* Find M, N and P for a two stage PLL
	 *
	 * Note that some bioses (NV30+) have lookup tables of precomputed MNP
	 * values, but we're too lazy to use those atm
	 *
	 * "clk" parameter in kHz
	 * returns calculated clock
	 */

	int crystal = 0;
	uint32_t minvco1 = pll_lim->vco1.minfreq, maxvco1 = pll_lim->vco1.maxfreq;
	uint32_t minvco2 = pll_lim->vco2.minfreq, maxvco2 = pll_lim->vco2.maxfreq, vco2;
	int maxM1 = 13, M1, N1;
	int maxM2 = 4, M2, N2;
	uint32_t minU1 = pll_lim->vco1.min_inputfreq, minU2 = pll_lim->vco2.min_inputfreq;
	int log2P;
	int clkP;
	int calcclk1, calcclk2, calcclkout, delta;
	unsigned int bestdelta = UINT_MAX;
	uint32_t bestclk = 0;

	/* some defaults */
	*bestNM1 = 0xff << 8 | 13;
	*bestNM2 = 0xff << 8 | 5;
	*bestlog2P = 6;

	switch (nv32_rd(pScrn, NV_PEXTDEV_BOOT_0) & (1 << 22 | 1 << 6)) {
	case 0:
		crystal = 13500;
		break;
	case (1 << 6):
		crystal = 14318;
		break;
	case (1 << 22):
	case (1 << 22 | 1 << 6):
		crystal = 27000;
		break;
	}

	if (maxvco2 < clk + clk/200)	/* +0.5% */
		maxvco2 = clk + clk/200;
	vco2 = (maxvco2 - maxvco2/200) / 2;

	for (log2P = 0; log2P < 6 && clk <= (vco2 >> log2P); log2P++) /* log2P is maximum of 6 */
		;
	clkP = clk << log2P;

	for (M1 = 1; M1 <= maxM1; M1++) {
		if (crystal/M1 < minU1)
			return bestclk;

		for (N1 = 1; N1 <= 0xff; N1++) {
			calcclk1 = crystal * N1 / M1;
			if (calcclk1 < minvco1)
				continue;
			if (calcclk1 > maxvco1)
				break;

			for (M2 = 1; M2 <= maxM2; M2++) {
				if (calcclk1/M2 < minU2)
					break;

				/* add calcclk1/2 to round better */
				N2 = (clkP * M2 + calcclk1/2) / calcclk1;
				/* this N2 > maxM2 test is a bit weird, but it's correct for nv31 */
				if (N2 < 4 || N2 > 0x46 || N2 > maxM2)
					continue;
				if (N2/M2 < 4 || N2/M2 > 10)
					continue;

				calcclk2 = calcclk1 * N2 / M2;
				if (calcclk2 < minvco2 || calcclk2 > maxvco2)
					continue;

				calcclkout = calcclk2 >> log2P;
				delta = abs(calcclkout - clk);
				/* we do an exhaustive search rather than terminating
				 * on an optimality condition...
				 */
				if (delta < bestdelta) {
					bestdelta = delta;
					bestclk = calcclkout;
					*bestNM1 = N1 << 8 | M1;
					*bestNM2 = N2 << 8 | M2;
					*bestlog2P = log2P;
					if (delta == 0)	/* except this one */
						return bestclk;
				}
			}
		}
	}

	return bestclk;
}

static void setPLL_single(ScrnInfoPtr pScrn, uint32_t reg, int NM, int log2P)
{
	uint32_t pll;

	pll = nv32_rd(pScrn, reg);
	if (pll == (log2P << 16 | NM))
		return;	/* already set */

#if 0
	//this stuff is present on my nv34 and something similar on the nv31
	//it is not on nv20, and I don't know how useful or necessary it is

	uint32_t saved_1584, shift_1584;
	Bool frob1584 = FALSE;
	switch (reg) {
	case 0x680500:
		shift_1584 = 0;
		frob1584 = TRUE;
		break;
	case 0x680504:
		shift_1584 = 4;
		frob1584 = TRUE;
		break;
	case 0x680508:
		shift_1584 = 8;
		frob1584 = TRUE;
		break;
	case 0x680520:
		shift_1584 = 12;
		frob1584 = TRUE;
		break;
	}

	if (frob1584) {
		saved_1584 = nv32_rd(pScrn, 0x00001584);
		nv32_wr(pScrn, 0x00001584, (saved_1584 & ~(0xf << shift_1584)) | 1 << shift_1584);
	}
#endif

	/* write NM first */
	pll = (pll & 0xffff0000) | NM;
	nv32_wr(pScrn, reg, pll);

	/* wait a bit */
	usleep(64000);
	nv32_rd(pScrn, reg);

	/* then write P as well */
	nv32_wr(pScrn, reg, (pll & 0xfff8ffff) | log2P << 16);

#if 0
	if (frob1584)
		nv32_wr(pScrn, 0x00001584, saved_1584);
#endif
}

static void setPLL_double(ScrnInfoPtr pScrn, uint32_t reg1, int NM1, int NM2, int log2P)
{
	uint32_t reg2, pll1, pll2;

	reg2 = reg1 + 0x70;
	if (reg2 == 0x680590)
		reg2 = NV_RAMDAC_VPLL2_B;

	pll1 = nv32_rd(pScrn, reg1);
	pll2 = nv32_rd(pScrn, reg2);
	if (pll1 == (log2P << 16 | NM1) && pll2 == (1 << 31 | NM2))
		return;	/* already set */

#if 0
	//this stuff is present on my nv31
	//I don't know how useful or necessary it is

	uint32_t saved_1584, shift_1584;
	Bool frob1584 = FALSE;
	switch (reg1) {
	case 0x680500:
		shift_1584 = 0;
		frob1584 = TRUE;
		break;
	case 0x680504:
		shift_1584 = 4;
		frob1584 = TRUE;
		break;
	}

	if (frob1584) {
		saved_1584 = nv32_rd(pScrn, 0x00001584);
		nv32_wr(pScrn, 0x00001584, (saved_1584 & ~(0xf << shift_1584)) | 1 << shift_1584);
	}
#endif

	nv32_wr(pScrn, reg2, (pll2 & 0x7fff0000) | NM2);
	nv32_wr(pScrn, reg1, (pll1 & 0xfff80000) | log2P << 16 | NM1);

#if 0
	if (frob1584)
		nv32_wr(pScrn, 0x00001584, saved_1584);
#endif
}

Bool get_pll_limits(ScrnInfoPtr pScrn, enum pll_types plltype, struct pll_lims *pll_lim);

static void setPLL(ScrnInfoPtr pScrn, bios_t *bios, uint32_t reg, uint32_t clk)
{
	/* clk in kHz */
	int NM1, NM2, log2P;

	// FIXME: both getMNP versions will need some alterations for nv40 type stuff
	if (bios->chip_version >= 0x40 || bios->chip_version == 0x31 || bios->chip_version == 0x36) {
		struct pll_lims pll_lim;
		// for NV40, pll_type will need setting
		get_pll_limits(pScrn, 0, &pll_lim);
		getMNP_double(pScrn, &pll_lim, clk, &NM1, &NM2, &log2P);
		setPLL_double(pScrn, reg, NM1, NM2, log2P);
	} else {
		getMNP_single(pScrn, clk, &NM1, &log2P);
		setPLL_single(pScrn, reg, NM1, log2P);
	}
}

static Bool init_prog(ScrnInfoPtr pScrn, bios_t *bios, CARD16 offset, init_exec_t *iexec)
{
	/* INIT_PROG   opcode: 0x31
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): reg
	 * offset + 5  (32 bit): and mask
	 * offset + 9  (8  bit): shift right
	 * offset + 10 (8  bit): number of configurations
	 * offset + 11 (32 bit): register
	 * offset + 15 (32 bit): configuration 1
	 * ...
	 * 
	 * Starting at offset + 15 there are "number of configurations"
	 * 32 bit values. To find out which configuration value to use
	 * read "CRTC reg" on the CRTC controller with index "CRTC index"
	 * and bitwise AND this value with "and mask" and then bit shift the
	 * result "shift right" bits to the right.
	 * Assign "register" with appropriate configuration value.
	 */

	CARD32 reg = *((CARD32 *) (&bios->data[offset + 1]));
	CARD32 and = *((CARD32 *) (&bios->data[offset + 5]));
	CARD8 shiftr = *((CARD8 *) (&bios->data[offset + 9]));
	CARD8 nr = *((CARD8 *) (&bios->data[offset + 10]));
	CARD32 reg2 = *((CARD32 *) (&bios->data[offset + 11]));
	CARD8 configuration;
	CARD32 configval, tmp;

	if (iexec->execute) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: REG: 0x%04X\n", offset, 
				reg);

		tmp = nv32_rd(pScrn, reg);
		configuration = (tmp & and) >> shiftr;

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: CONFIGURATION TO USE: 0x%02X\n", 
				offset, configuration);

		if (configuration <= nr) {

			configval = 
				*((CARD32 *) (&bios->data[offset + 15 + configuration * 4]));

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: REG: 0x%08X, VALUE: 0x%08X\n", offset, 
					reg2, configval);
			
			tmp = nv32_rd(pScrn, reg2);
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: CURRENT VALUE IS: 0x%08X\n",
				offset, tmp);
			nv32_wr(pScrn, reg2, configval);
		}
	}
	return TRUE;
}

static Bool init_io_restrict_prog(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_IO_RESTRICT_PROG   opcode: 0x32 ('2')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): CRTC port
	 * offset + 3  (8  bit): CRTC index
	 * offset + 4  (8  bit): mask
	 * offset + 5  (8  bit): shift
	 * offset + 6  (8  bit): count
	 * offset + 7  (32 bit): register
	 * offset + 11 (32 bit): configuration 1
	 * ...
	 *
	 * Starting at offset + 11 there are "count" 32 bit values.
	 * To find out which value to use read index "CRTC index" on "CRTC port",
	 * AND this value with "mask" and then bit shift right "shift" bits.
	 * Read the appropriate value using this index and write to "register"
	 */

	uint16_t crtcport = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 1])));
	uint8_t crtcindex = bios->data[offset + 3];
	uint8_t mask = bios->data[offset + 4];
	uint8_t shift = bios->data[offset + 5];
	uint8_t count = bios->data[offset + 6];
	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 7])));
	uint8_t config;
	uint32_t configval;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Port: 0x%04X, Index: 0x%02X, Mask: 0x%02X, Shift: 0x%02X, Count: 0x%02X, Reg: 0x%08X\n",
			   offset, crtcport, crtcindex, mask, shift, count, reg);

	config = (nv_idx_port_rd(pScrn, crtcport, crtcindex) & mask) >> shift;
	if (config > count) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Config 0x%02X exceeds maximal bound 0x%02X\n",
			   offset, config, count);
		return FALSE;
	}

	configval = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 11 + config * 4])));

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Writing config %02X\n", offset, config);

	nv32_wr(pScrn, reg, configval);

	return TRUE;
}

static Bool init_repeat(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_REPEAT   opcode: 0x33 ('3')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): count
	 *
	 * Execute script following this opcode up to INIT_REPEAT_END
	 * "count" times
	 */

	uint8_t count = bios->data[offset + 1];
	uint8_t i;

	/* no iexec->execute check by design */

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "0x%04X: REPEATING FOLLOWING SEGMENT %d TIMES\n",
		   offset, count);

	iexec->repeat = TRUE;

	/* count - 1, as the script block will execute once when we leave this
	 * opcode -- this is compatible with bios behaviour as:
	 * a) the block is always executed at least once, even if count == 0
	 * b) the bios interpreter skips to the op following INIT_END_REPEAT,
	 * while we don't
	 */
	for (i = 0; i < count - 1; i++)
		parse_init_table(pScrn, bios, offset + 2, iexec);

	iexec->repeat = FALSE;

	return TRUE;
}

static Bool init_io_restrict_pll(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_IO_RESTRICT_PLL   opcode: 0x34 ('4')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): CRTC port
	 * offset + 3  (8  bit): CRTC index
	 * offset + 4  (8  bit): mask
	 * offset + 5  (8  bit): shift
	 * offset + 6  (8  bit): IO flag condition index
	 * offset + 7  (8  bit): count
	 * offset + 8  (32 bit): register
	 * offset + 12 (16 bit): frequency 1
	 * ...
	 *
	 * Starting at offset + 12 there are "count" 16 bit frequencies (10kHz).
	 * Set PLL register "register" to coefficients for frequency n,
	 * selected by reading index "CRTC index" of "CRTC port" ANDed with
	 * "mask" and shifted right by "shift". If "IO flag condition index" > 0,
	 * and condition met, double frequency before setting it.
	 */

	uint16_t crtcport = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 1])));
	uint8_t crtcindex = bios->data[offset + 3];
	uint8_t mask = bios->data[offset + 4];
	uint8_t shift = bios->data[offset + 5];
	int8_t io_flag_condition_idx = bios->data[offset + 6];
	uint8_t count = bios->data[offset + 7];
	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 8])));
	uint8_t config;
	uint16_t freq;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Port: 0x%04X, Index: 0x%02X, Mask: 0x%02X, Shift: 0x%02X, IO Flag Condition: 0x%02X, Count: 0x%02X, Reg: 0x%08X\n",
			   offset, crtcport, crtcindex, mask, shift, io_flag_condition_idx, count, reg);

	config = (nv_idx_port_rd(pScrn, crtcport, crtcindex) & mask) >> shift;
	if (config > count) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Config 0x%02X exceeds maximal bound 0x%02X\n",
			   offset, config, count);
		return FALSE;
	}

	freq = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 12 + config * 2])));

	if (io_flag_condition_idx > 0) {
		if (io_flag_condition(pScrn, bios, offset, io_flag_condition_idx)) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: CONDITION FULFILLED - FREQ DOUBLED\n", offset);
			freq *= 2;
		} else
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: CONDITION IS NOT FULFILLED. FREQ UNCHANGED\n", offset);
	}

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Reg: 0x%08X, Config: 0x%02X, Freq: %d0kHz\n",
			   offset, reg, config, freq);

	setPLL(pScrn, bios, reg, freq * 10);

	return TRUE;
}

static Bool init_end_repeat(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_END_REPEAT   opcode: 0x36 ('6')
	 *
	 * offset      (8 bit): opcode
	 *
	 * Marks the end of the block for INIT_REPEAT to repeat
	 */

	/* no iexec->execute check by design */

	/* iexec->repeat flag necessary to go past INIT_END_REPEAT opcode when
	 * we're not in repeat mode
	 */
	if (iexec->repeat)
		return FALSE;

	return TRUE;
}

static Bool init_copy(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_COPY   opcode: 0x37 ('7')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): register
	 * offset + 5  (8  bit): shift
	 * offset + 6  (8  bit): srcmask
	 * offset + 7  (16 bit): CRTC port
	 * offset + 9  (8 bit): CRTC index
	 * offset + 10  (8 bit): mask
	 *
	 * Read index "CRTC index" on "CRTC port", AND with "mask", OR with
	 * (REGVAL("register") >> "shift" & "srcmask") and write-back to CRTC port
	 */

	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint8_t shift = bios->data[offset + 5];
	uint8_t srcmask = bios->data[offset + 6];
	uint16_t crtcport = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 7])));
	uint8_t crtcindex = bios->data[offset + 9];
	uint8_t mask = bios->data[offset + 10];
	uint32_t data;
	uint8_t crtcdata;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Reg: 0x%08X, Shift: 0x%02X, SrcMask: 0x%02X, Port: 0x%04X, Index: 0x%02X, Mask: 0x%02X\n",
			   offset, reg, shift, srcmask, crtcport, crtcindex, mask);

	data = nv32_rd(pScrn, reg);

	if (shift < 0x80)
		data >>= shift;
	else
		data <<= (0x100 - shift);

	data &= srcmask;

	crtcdata = (nv_idx_port_rd(pScrn, crtcport, crtcindex) & mask) | (uint8_t)data;
	nv_idx_port_wr(pScrn, crtcport, crtcindex, crtcdata);

	return TRUE;
}

static Bool init_not(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_NOT   opcode: 0x38 ('8')
	 *
	 * offset      (8  bit): opcode
	 *
	 * Invert the current execute / no-execute condition (i.e. "else")
	 */
	if (iexec->execute)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: ------ SKIPPING FOLLOWING COMMANDS  ------\n", offset);
	else
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: ------ EXECUTING FOLLOWING COMMANDS ------\n", offset);

	iexec->execute = !iexec->execute;
	return TRUE;
}

static Bool init_io_flag_condition(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_IO_FLAG_CONDITION   opcode: 0x39 ('9')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): condition number
	 *
	 * Check condition "condition number" in the IO flag condition table.
	 * If condition not met skip subsequent opcodes until condition
	 * is inverted (INIT_NOT), or we hit INIT_RESUME
	 */

	uint8_t cond = bios->data[offset + 1];

	if (!iexec->execute)
		return TRUE;

	if (io_flag_condition(pScrn, bios, offset, cond))
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: CONDITION FULFILLED - CONTINUING TO EXECUTE\n", offset);
	else {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: CONDITION IS NOT FULFILLED\n", offset);
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: ------ SKIPPING FOLLOWING COMMANDS  ------\n", offset);
		iexec->execute = FALSE;
	}

	return TRUE;
}

Bool init_idx_addr_latched(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_INDEX_ADDRESS_LATCHED   opcode: 0x49 ('I')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): control register
	 * offset + 5  (32 bit): data register
	 * offset + 9  (32 bit): mask
	 * offset + 13 (32 bit): data
	 * offset + 17 (8  bit): count
	 * offset + 18 (8  bit): address 1
	 * offset + 19 (8  bit): data 1
	 * ...
	 *
	 * For each of "count" address and data pairs, write "data n" to "data register",
	 * read the current value of "control register", and write it back once ANDed
	 * with "mask", ORed with "data", and ORed with "address n"
	 */

	uint32_t controlreg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint32_t datareg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 5])));
	uint32_t mask = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 9])));
	uint32_t data = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 13])));
	uint8_t count = bios->data[offset + 17];
	uint32_t value;
	int i;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: ControlReg: 0x%08X, DataReg: 0x%08X, Mask: 0x%08X, Data: 0x%08X, Count: 0x%02X\n",
			   offset, controlreg, datareg, mask, data, count);

	for (i = 0; i < count; i++) {
		uint8_t instaddress = bios->data[offset + 18 + i * 2];
		uint8_t instdata = bios->data[offset + 19 + i * 2];

		if (DEBUGLEVEL >= 6)
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: Address: 0x%02X, Data: 0x%02X\n", offset, instaddress, instdata);

		nv32_wr(pScrn, datareg, instdata);
		value = (nv32_rd(pScrn, controlreg) & mask) | data | instaddress;
		nv32_wr(pScrn, controlreg, value);
	}

	return TRUE;
}

static Bool init_io_restrict_pll2(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_IO_RESTRICT_PLL2   opcode: 0x4A ('J')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): CRTC port
	 * offset + 3  (8  bit): CRTC index
	 * offset + 4  (8  bit): mask
	 * offset + 5  (8  bit): shift
	 * offset + 6  (8  bit): count
	 * offset + 7  (32 bit): register
	 * offset + 11 (32 bit): frequency 1
	 * ...
	 *
	 * Starting at offset + 11 there are "count" 32 bit frequencies (kHz).
	 * Set PLL register "register" to coefficients for frequency n,
	 * selected by reading index "CRTC index" of "CRTC port" ANDed with
	 * "mask" and shifted right by "shift".
	 */

	uint16_t crtcport = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 1])));
	uint8_t crtcindex = bios->data[offset + 3];
	uint8_t mask = bios->data[offset + 4];
	uint8_t shift = bios->data[offset + 5];
	uint8_t count = bios->data[offset + 6];
	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 7])));
	uint8_t config;
	uint32_t freq;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Port: 0x%04X, Index: 0x%02X, Mask: 0x%02X, Shift: 0x%02X, Count: 0x%02X, Reg: 0x%08X\n",
			   offset, crtcport, crtcindex, mask, shift, count, reg);

	if (!reg)
		return TRUE;

	config = (nv_idx_port_rd(pScrn, crtcport, crtcindex) & mask) >> shift;
	if (config > count) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Config 0x%02X exceeds maximal bound 0x%02X\n",
			   offset, config, count);
		return FALSE;
	}

	freq = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 11 + config * 4])));

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Reg: 0x%08X, Config: 0x%02X, Freq: %dkHz\n",
			   offset, reg, config, freq);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x%04X: [ NOT YET IMPLEMENTED ]\n", offset);

	return TRUE;
}

static Bool init_pll2(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_PLL2   opcode: 0x4B ('K')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): register
	 * offset + 5  (32 bit): freq
	 *
	 * Set PLL register "register" to coefficients for frequency "freq"
	 */

	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint32_t freq = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 5])));

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Reg: 0x%04X, Freq: %dkHz\n",
			   offset, reg, freq);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x%04X: [ NOT YET IMPLEMENTED ]\n", offset);

	return TRUE;
}

static uint32_t get_tmds_index_reg(ScrnInfoPtr pScrn, uint8_t mlv)
{
	/* For mlv < 0x80, it is an index into a table of TMDS base addresses
	 * For mlv == 0x80 use the "or" value of the dcb_entry indexed by CR58 for CR57 = 0
	 * to index a table of offsets to the basic 0x6808b0 address
	 * For mlv == 0x81 use the "or" value of the dcb_entry indexed by CR58 for CR57 = 0
	 * to index a table of offsets to the basic 0x6808b0 address, and then flip the offset by 8
	 */

	NVPtr pNv = NVPTR(pScrn);
	int pramdac_offset[13] = {0, 0, 0x8, 0, 0x2000, 0, 0, 0, 0x2008, 0, 0, 0, 0x2000};
	uint32_t pramdac_table[4] = {0x6808b0, 0x6808b8, 0x6828b0, 0x6828b8};

	if (mlv >= 0x80) {
		/* here we assume that the DCB table has already been parsed */
		uint8_t dcb_entry;
		int dacoffset;
		/* This register needs to be written to set index for reading CR58 */
		nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, 0x57, 0);
		dcb_entry = nv_idx_port_rd(pScrn, CRTC_INDEX_COLOR, 0x58);
		if (dcb_entry > pNv->dcb_table.entries) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "CR58 doesn't have a valid DCB entry currently (%02X)\n", dcb_entry);
			return FALSE;
		}
		dacoffset = pramdac_offset[pNv->dcb_table.entry[dcb_entry].or];
		if (mlv == 0x81)
			dacoffset ^= 8;
		return (0x6808b0 + dacoffset);
	} else {
		if (mlv > (sizeof(pramdac_table) / sizeof(uint32_t))) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "Magic Lookup Value too big (%02X)\n", mlv);
			return FALSE;
		}
		return pramdac_table[mlv];
	}
}

static Bool init_tmds(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_TMDS   opcode: 0x4F ('O')	(non-canon name)
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): magic lookup value
	 * offset + 2  (8 bit): TMDS address
	 * offset + 3  (8 bit): mask
	 * offset + 4  (8 bit): data
	 *
	 * Read the data reg for TMDS address "TMDS address", AND it with mask
	 * and OR it with data, then write it back
	 * "magic lookup value" determines which TMDS base address register is used --
	 * see get_tmds_index_reg()
	 */

	uint8_t mlv = bios->data[offset + 1];
	uint32_t tmdsaddr = bios->data[offset + 2];
	uint8_t mask = bios->data[offset + 3];
	uint8_t data = bios->data[offset + 4];
	uint32_t reg, value;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: MagicLookupValue: 0x%02X, TMDSAddr: 0x%02X, Mask: 0x%02X, Data: 0x%02X\n",
			   offset, mlv, tmdsaddr, mask, data);

	reg = get_tmds_index_reg(pScrn, mlv);

	nv32_wr(pScrn, reg, tmdsaddr | 0x10000);
	value = (nv32_rd(pScrn, reg + 4) & mask) | data;
	nv32_wr(pScrn, reg + 4, value);
	nv32_wr(pScrn, reg, tmdsaddr);

	return TRUE;
}

Bool init_zm_tmds_group(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_ZM_TMDS_GROUP   opcode: 0x50 ('P')	(non-canon name)
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): magic lookup value
	 * offset + 2  (8 bit): count
	 * offset + 3  (8 bit): addr 1
	 * offset + 4  (8 bit): data 1
	 * ...
	 *
	 * For each of "count" TMDS address and data pairs write "data n" to "addr n"
	 * "magic lookup value" determines which TMDS base address register is used --
	 * see get_tmds_index_reg()
	 */

	uint8_t mlv = bios->data[offset + 1];
	uint8_t count = bios->data[offset + 2];
	uint32_t reg;
	int i;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: MagicLookupValue: 0x%02X, Count: 0x%02X\n",
			   offset, mlv, count);

	reg = get_tmds_index_reg(pScrn, mlv);

	for (i = 0; i < count; i++) {
		uint8_t tmdsaddr = bios->data[offset + 3 + i * 2];
		uint8_t tmdsdata = bios->data[offset + 4 + i * 2];

		nv32_wr(pScrn, reg + 4, tmdsdata);
		nv32_wr(pScrn, reg, tmdsaddr);
	}

	return TRUE;
}

Bool init_cr_idx_adr_latch(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_CR_INDEX_ADDRESS_LATCHED   opcode: 0x51 ('Q')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): CRTC index1
	 * offset + 2  (8 bit): CRTC index2
	 * offset + 3  (8 bit): baseaddr
	 * offset + 4  (8 bit): count
	 * offset + 5  (8 bit): data 1
	 * ...
	 *
	 * For each of "count" address and data pairs, write "baseaddr + n" to
	 * "CRTC index1" and "data n" to "CRTC index2"
	 * Once complete, restore initial value read from "CRTC index1"
	 */
	uint8_t crtcindex1 = bios->data[offset + 1];
	uint8_t crtcindex2 = bios->data[offset + 2];
	uint8_t baseaddr = bios->data[offset + 3];
	uint8_t count = bios->data[offset + 4];
	uint8_t oldaddr, data;
	int i;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Index1: 0x%02X, Index2: 0x%02X, BaseAddr: 0x%02X, Count: 0x%02X\n",
			   offset, crtcindex1, crtcindex2, baseaddr, count);

	oldaddr = nv_idx_port_rd(pScrn, CRTC_INDEX_COLOR, crtcindex1);

	for (i = 0; i < count; i++) {
		nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, crtcindex1, baseaddr + i);

		data = bios->data[offset + 5 + i];
		nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, crtcindex2, data);
	}

	nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, crtcindex1, oldaddr);

	return TRUE;
}

Bool init_cr(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_CR   opcode: 0x52 ('R')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (8  bit): CRTC index
	 * offset + 2  (8  bit): mask
	 * offset + 3  (8  bit): data
	 *
	 * Assign the value of at "CRTC index" ANDed with mask and ORed with data
	 * back to "CRTC index"
	 */

	uint8_t crtcindex = bios->data[offset + 1];
	uint8_t mask = bios->data[offset + 2];
	uint8_t data = bios->data[offset + 3];
	uint8_t value;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Index: 0x%02X, Mask: 0x%02X, Data: 0x%02X\n",
			   offset, crtcindex, mask, data);

	value = (nv_idx_port_rd(pScrn, CRTC_INDEX_COLOR, crtcindex) & mask) | data;
	nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, crtcindex, value);

	return TRUE;
}

static Bool init_zm_cr(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_ZM_CR   opcode: 0x53 ('S')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): CRTC index
	 * offset + 2  (8 bit): value
	 *
	 * Assign "value" to CRTC register with index "CRTC index".
	 */

	uint8_t crtcindex = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint8_t data = bios->data[offset + 2];

	if (!iexec->execute)
		return TRUE;

	nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, crtcindex, data);

	return TRUE;
}

static Bool init_zm_cr_group(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_ZM_CR_GROUP   opcode: 0x54 ('T')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): count
	 * offset + 2  (8 bit): CRTC index 1
	 * offset + 3  (8 bit): value 1
	 * ...
	 *
	 * For "count", assign "value n" to CRTC register with index "CRTC index n".
	 */
    
	uint8_t count = bios->data[offset + 1];
	int i;

	if (!iexec->execute)
		return TRUE;

	for (i = 0; i < count; i++)
		init_zm_cr(pScrn, bios, offset + 2 + 2 * i - 1, iexec);

	return TRUE;
}

static Bool init_condition_time(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_CONDITION_TIME   opcode: 0x56 ('V')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): condition number
	 *
	 * Check condition "condition number" in the condition table.
	 * The condition table entry has 4 bytes for the address of the
	 * register to check, 4 bytes for a mask and 4 for a test value.
	 * If condition not met sleep for 2ms
	 */

	// this opcode makes no sense. it seems to do some competely useless things
	uint8_t cond = bios->data[offset + 1];
//	uint16_t b = bios->data[offset + 2];	// this needs printing
	uint16_t condptr = bios->condition_tbl_ptr + cond * CONDITION_SIZE;
	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[condptr])));
	uint32_t mask = le32_to_cpu(*((uint32_t *)(&bios->data[condptr + 4])));
	uint32_t cmpval = le32_to_cpu(*((uint32_t *)(&bios->data[condptr + 8])));
	uint32_t data;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Cond: 0x%02X, Reg: 0x%08X, Mask: 0x%08X, Cmpval: 0x%08X\n",
			   offset, cond, reg, mask, cmpval);

//	b *= 50;
	reg &= 0xfffffffc;	// FIXME: this not in init_condition() - should it be?

	data = nv32_rd(pScrn, reg) & mask;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Checking if 0x%08X equals 0x%08X\n",
			   offset, data, cmpval);

	if (data != cmpval) {
		if (DEBUGLEVEL >= 6)
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: Condition not met, sleeping for 2ms\n", offset);
//		reg--;
		usleep(2000);
	} else
		if (DEBUGLEVEL >= 6)
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: Condition met, continuing\n", offset);

	return TRUE;
}

static Bool init_zm_reg_sequence(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_ZM_REG_SEQUENCE   opcode: 0x58 ('X')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): base register
	 * offset + 5  (8  bit): count
	 * offset + 6  (32 bit): value 1
	 * ...
	 *
	 * Starting at offset + 6 there are "count" 32 bit values.
	 * For "count" iterations set "base register" + 4 * current_iteration
	 * to "value current_iteration"
	 */

	uint32_t basereg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint32_t count = bios->data[offset + 5];
	int i;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: BaseReg: 0x%08X, Count: 0x%02X\n",
			   offset, basereg, count);

	for (i = 0; i < count; i++) {
		uint32_t reg = basereg + i * 4;
		uint32_t data = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 6 + i * 4])));

		nv32_wr(pScrn, reg, data);
	}

	return TRUE;
}

static Bool init_indirect_reg(ScrnInfoPtr pScrn, bios_t *bios, CARD16 offset, init_exec_t *iexec)
{
	/* INIT_INDIRECT_REG opcode: 0x5A
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): register
	 * offset + 5  (16 bit): adress offset (in bios)
	 *
	 * Lookup value at offset data in the bios and write it to reg
	 */
	CARD32 reg = *((CARD32 *) (&bios->data[offset + 1]));
	CARD16 data = le16_to_cpu(*((CARD16 *) (&bios->data[offset + 5])));
	CARD32 data2 = bios->data[data];

	if (iexec->execute) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
				"0x%04X: REG: 0x%04X, DATA AT: 0x%04X, VALUE IS: 0x%08X\n", 
				offset, reg, data, data2);

		if (DEBUGLEVEL >= 6) {
			CARD32 tmpval;
			tmpval = nv32_rd(pScrn, reg);
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: CURRENT VALUE IS: 0x%08X\n", offset, tmpval);
		}

		nv32_wr(pScrn, reg, data2);
	}
	return TRUE;
}

static Bool init_sub_direct(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_SUB_DIRECT   opcode: 0x5B ('[')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): subroutine offset (in bios)
	 *
	 * Calls a subroutine that will execute commands until INIT_DONE
	 * is found. 
	 */

	uint16_t sub_offset = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 1])));

	if (!iexec->execute)
		return TRUE;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: EXECUTING SUB-ROUTINE AT 0x%04X\n",
			offset, sub_offset);

	parse_init_table(pScrn, bios, sub_offset, iexec);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: END OF SUB-ROUTINE AT 0x%04X\n",
			offset, sub_offset);

	return TRUE;
}

static Bool init_copy_nv_reg(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_COPY_NV_REG   opcode: 0x5F ('_')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): src reg
	 * offset + 5  (8  bit): shift
	 * offset + 6  (32 bit): src mask
	 * offset + 10 (32 bit): xor
	 * offset + 14 (32 bit): dst reg
	 * offset + 18 (32 bit): dst mask
	 *
	 * Shift REGVAL("src reg") right by (signed) "shift", AND result with
	 * "src mask", then XOR with "xor". Write this OR'd with
	 * (REGVAL("dst reg") AND'd with "dst mask") to "dst reg"
	 */

	uint32_t srcreg = *((uint32_t *)(&bios->data[offset + 1]));
	uint8_t shift = bios->data[offset + 5];
	uint32_t srcmask = *((uint32_t *)(&bios->data[offset + 6]));
	uint32_t xor = *((uint32_t *)(&bios->data[offset + 10]));
	uint32_t dstreg = *((uint32_t *)(&bios->data[offset + 14]));
	uint32_t dstmask = *((uint32_t *)(&bios->data[offset + 18]));
	uint32_t srcvalue, dstvalue;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: SrcReg: 0x%08X, Shift: 0x%02X, SrcMask: 0x%08X, Xor: 0x%08X, DstReg: 0x%08X, DstMask: 0x%08X\n",
			   offset, srcreg, shift, srcmask, xor, dstreg, dstmask);

	srcvalue = nv32_rd(pScrn, srcreg);

	if (shift < 0x80)
		srcvalue >>= shift;
	else
		srcvalue <<= (0x100 - shift);

	srcvalue = (srcvalue & srcmask) ^ xor;

	dstvalue = nv32_rd(pScrn, dstreg) & dstmask;

	nv32_wr(pScrn, dstreg, dstvalue | srcvalue);

	return TRUE;
}

static Bool init_zm_index_io(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_ZM_INDEX_IO   opcode: 0x62 ('b')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): CRTC port
	 * offset + 3  (8  bit): CRTC index
	 * offset + 4  (8  bit): data
	 *
	 * Write "data" to index "CRTC index" of "CRTC port"
	 */
	uint16_t crtcport = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 1])));
	uint8_t crtcindex = bios->data[offset + 3];
	uint8_t data = bios->data[offset + 4];

	if (!iexec->execute)
		return TRUE;

	nv_idx_port_wr(pScrn, crtcport, crtcindex, data);

	return TRUE;
}

static Bool init_compute_mem(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_COMPUTE_MEM   opcode: 0x63 ('c')
	 *
	 * offset      (8 bit): opcode
	 *
	 * This opcode is meant to set NV_PFB_CFG0 (0x100200) appropriately so
	 * that the hardware can correctly calculate how much VRAM it has
	 * (and subsequently report that value in 0x10020C)
	 *
	 * The implementation of this opcode in general consists of two parts:
	 * 1) determination of the memory bus width
	 * 2) determination of how many of the card's RAM pads have ICs attached
	 *
	 * 1) is done by a cunning combination of writes to offsets 0x1c and
	 * 0x3c in the framebuffer, and seeing whether the written values are
	 * read back correctly. This then affects bits 4-7 of NV_PFB_CFG0
	 *
	 * 2) is done by a cunning combination of writes to an offset slightly
	 * less than the maximum memory reported by 0x10020C, then seeing if
	 * the test pattern can be read back. This then affects bits 12-15 of
	 * NV_PFB_CFG0
	 *
	 * In this context a "cunning combination" may include multiple reads
	 * and writes to varying locations, often alternating the test pattern
	 * and 0, doubtless to make sure buffers are filled, residual charges
	 * on tracks are removed etc.
	 *
	 * Unfortunately, the "cunning combination"s mentioned above, and the
	 * changes to the bits in NV_PFB_CFG0 differ with nearly every bios
	 * trace I have.
	 *
	 * Therefore, we cheat and assume the value of NV_PFB_CFG0 with which
	 * we started was correct, and use that instead
	 */

	/* no iexec->execute check by design */

	/* on every card I've seen, this step gets done for us earlier in the init scripts
	uint8_t crdata = nv_idx_port_rd(pScrn, VGA_SEQ_INDEX, 0x01);
	nv_idx_port_wr(pScrn, VGA_SEQ_INDEX, 0x01, crdata | 0x20);
	*/

	/* this also has probably been done in the scripts, but an mmio trace of
	 * s3 resume shows nvidia doing it anyway (unlike the VGA_SEQ_INDEX write)
	 */
	nv32_wr(pScrn, NV_PFB_REFCTRL, NV_PFB_REFCTRL_VALID_1);

	/* write back the saved configuration value */
	nv32_wr(pScrn, NV_PFB_CFG0, saved_nv_pfb_cfg0);

	return TRUE;
}

static Bool init_reset(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_RESET   opcode: 0x65 ('e')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): register
	 * offset + 5  (32 bit): value1
	 * offset + 9  (32 bit): value2
	 *
	 * Assign "value1" to "register", then assign "value2" to "register"
	 */

	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint32_t value1 = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 5])));
	uint32_t value2 = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 9])));
	uint32_t pci_nv_19, pci_nv_20;

	/* no iexec->execute check by design */

	pci_nv_19 = nv32_rd(pScrn, NV_PBUS_PCI_NV_19);
	nv32_wr(pScrn, NV_PBUS_PCI_NV_19, 0);
	nv32_wr(pScrn, reg, value1);

	usleep(10);

	nv32_wr(pScrn, reg, value2);
	nv32_wr(pScrn, NV_PBUS_PCI_NV_19, pci_nv_19);

	pci_nv_20 = nv32_rd(pScrn, NV_PBUS_PCI_NV_20);
	pci_nv_20 &= ~NV_PBUS_PCI_NV_20_ROM_SHADOW_ENABLED;	/* 0xfffffffe */
	nv32_wr(pScrn, NV_PBUS_PCI_NV_20, pci_nv_20);

	return TRUE;
}

static Bool init_index_io8(ScrnInfoPtr pScrn, bios_t *bios, CARD16 offset, init_exec_t *iexec)
{
	/* INIT_INDEX_IO8   opcode: 0x69
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): CRTC reg
	 * offset + 3  (8  bit): and mask
	 * offset + 4  (8  bit): or with
	 * 
	 * 
	 */

	NVPtr pNv = NVPTR(pScrn);
	volatile CARD8 *ptr = crtchead ? pNv->PCIO1 : pNv->PCIO0;
	CARD16 reg = le16_to_cpu(*((CARD16 *)(&bios->data[offset + 1])));
	CARD8 and  = *((CARD8 *)(&bios->data[offset + 3]));
	CARD8 or = *((CARD8 *)(&bios->data[offset + 4]));
	CARD8 data;

	if (iexec->execute) {
		data = (VGA_RD08(ptr, reg) & and) | or;

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
				"0x%04X: CRTC REG: 0x%04X, VALUE: 0x%02X\n", 
				offset, reg, data);
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: CURRENT VALUE IS: 0x%02X\n", offset, 
				VGA_RD08(ptr, reg));

#ifdef PERFORM_WRITE
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "init_index_io8 crtcreg 0x%X value 0x%X\n",reg,data);
		still_alive();
		VGA_WR08(ptr, reg, data);
#endif
	}
	return TRUE;
}

static Bool init_sub(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_SUB   opcode: 0x6B ('k')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): script number
	 *
	 * Execute script number "script number", as a subroutine
	 */

	uint8_t sub = bios->data[offset + 1];

	if (!iexec->execute)
		return TRUE;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "0x%04X: EXECUTING SUB-SCRIPT %d\n", offset, sub);

	parse_init_table(pScrn, bios,
			 le16_to_cpu(*((uint16_t *)(&bios->data[bios->init_script_tbls_ptr + sub * 2]))),
			 iexec);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "0x%04X: END OF SUB-SCRIPT %d\n", offset, sub);

	return TRUE;
}

static Bool init_ram_condition(ScrnInfoPtr pScrn, bios_t *bios, CARD16 offset, init_exec_t *iexec)
{
	/* INIT_RAM_CONDITION   opcode: 0x6D
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (8  bit): and mask
	 * offset + 2  (8  bit): cmpval
	 *
	 * Test if (NV_PFB_BOOT & and mask) matches cmpval
	 */
	NVPtr pNv = NVPTR(pScrn);
	CARD8 and = *((CARD8 *) (&bios->data[offset + 1]));
	CARD8 cmpval = *((CARD8 *) (&bios->data[offset + 2]));
	CARD32 data;

	if (iexec->execute) {
		data=(pNv->PFB[NV_PFB_BOOT/4])&and;

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
				"0x%04X: CHECKING IF REGVAL: 0x%08X equals COND: 0x%08X\n",
				offset, data, cmpval);

		if (data == cmpval) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
					"0x%04X: CONDITION FULFILLED - CONTINUING TO EXECUTE\n",
					offset);
		} else {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: CONDITION IS NOT FULFILLED\n", offset);
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
					"0x%04X: ------ SKIPPING FOLLOWING COMMANDS  ------\n", offset);
			iexec->execute = FALSE;     
		}
	}
	return TRUE;
}

static Bool init_nv_reg(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_NV_REG   opcode: 0x6E ('n')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): register
	 * offset + 5  (32 bit): mask
	 * offset + 9  (32 bit): data
	 *
	 * Assign ((REGVAL("register") & "mask") | "data") to "register"
	 */

	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint32_t mask = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 5])));
	uint32_t data = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 9])));

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Reg: 0x%08X, Mask: 0x%08X, Data: 0x%08X\n",
			   offset, reg, mask, data);

	nv32_wr(pScrn, reg, (nv32_rd(pScrn, reg) & mask) | data);

	return TRUE;
}

static Bool init_macro(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_MACRO   opcode: 0x6F ('o')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): macro number
	 *
	 * Look up macro index "macro number" in the macro index table.
	 * The macro index table entry has 1 byte for the index in the macro table,
	 * and 1 byte for the number of times to repeat the macro.
	 * The macro table entry has 4 bytes for the register address and
	 * 4 bytes for the value to write to that register
	 */

	uint8_t macro_index_tbl_idx = bios->data[offset + 1];
	uint16_t tmp = bios->macro_index_tbl_ptr + (macro_index_tbl_idx * MACRO_INDEX_SIZE);
	uint8_t macro_tbl_idx = bios->data[tmp];
	uint8_t count = bios->data[tmp + 1];
	uint32_t reg, data;
	int i;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Macro: 0x%02X, MacroTableIndex: 0x%02X, Count: 0x%02X\n",
			   offset, macro_index_tbl_idx, macro_tbl_idx, count);

	for (i = 0; i < count; i++) {
		uint16_t macroentryptr = bios->macro_tbl_ptr + (macro_tbl_idx + i) * MACRO_SIZE;

		reg = le32_to_cpu(*((uint32_t *)(&bios->data[macroentryptr])));
		data = le32_to_cpu(*((uint32_t *)(&bios->data[macroentryptr + 4])));

		nv32_wr(pScrn, reg, data);
	}

	return TRUE;
}

static Bool init_done(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_DONE   opcode: 0x71 ('q')
	 *
	 * offset      (8  bit): opcode
	 *
	 * End the current script
	 */

	/* mild retval abuse to stop parsing this table */
	return FALSE;
}

static Bool init_resume(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_RESUME   opcode: 0x72 ('r')
	 *
	 * offset      (8  bit): opcode
	 *
	 * End the current execute / no-execute condition
	 */

	if (iexec->execute)
		return TRUE;

	iexec->execute = TRUE;;
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "0x%04X: ---- EXECUTING FOLLOWING COMMANDS ----\n", offset);

	return TRUE;
}

static Bool init_ram_condition2(ScrnInfoPtr pScrn, bios_t *bios, CARD16 offset, init_exec_t *iexec)
{
	/* INIT_RAM_CONDITION2   opcode: 0x73
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (8  bit): and mask
	 * offset + 2  (8  bit): cmpval
	 *
	 * Test if (NV_EXTDEV_BOOT & and mask) matches cmpval
	 */
	NVPtr pNv = NVPTR(pScrn);
	CARD32 and = *((CARD32 *) (&bios->data[offset + 1]));
	CARD32 cmpval = *((CARD32 *) (&bios->data[offset + 5]));
	CARD32 data;

	if (iexec->execute) {
		data=(nvReadEXTDEV(pNv, NV_PEXTDEV_BOOT))&and;
		
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
				"0x%04X: CHECKING IF REGVAL: 0x%08X equals COND: 0x%08X\n",
				offset, data, cmpval);

		if (data == cmpval) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
					"0x%04X: CONDITION FULFILLED - CONTINUING TO EXECUTE\n",
					offset);
		} else {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: CONDITION IS NOT FULFILLED\n", offset);
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
					"0x%04X: ------ SKIPPING FOLLOWING COMMANDS  ------\n", offset);
			iexec->execute = FALSE;     
		}
	}
	return TRUE;
}

static Bool init_time(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_TIME   opcode: 0x74 ('t')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): time
	 *
	 * Sleep for "time" microseconds.
	 */

	uint16_t time = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 1])));

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Sleeping for 0x%04X microseconds\n", offset, time);

	usleep(time);

	return TRUE;
}

static Bool init_condition(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_CONDITION   opcode: 0x75 ('u')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): condition number
	 *
	 * Check condition "condition number" in the condition table.
	 * The condition table entry has 4 bytes for the address of the
	 * register to check, 4 bytes for a mask and 4 for a test value.
	 * If condition not met skip subsequent opcodes until condition
	 * is inverted (INIT_NOT), or we hit INIT_RESUME
	 */

	uint8_t cond = bios->data[offset + 1];
	uint16_t condptr = bios->condition_tbl_ptr + cond * CONDITION_SIZE;
	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[condptr])));
	uint32_t mask = le32_to_cpu(*((uint32_t *)(&bios->data[condptr + 4])));
	uint32_t cmpval = le32_to_cpu(*((uint32_t *)(&bios->data[condptr + 8])));
	uint32_t data;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Cond: 0x%02X, Reg: 0x%08X, Mask: 0x%08X, Cmpval: 0x%08X\n",
			   offset, cond, reg, mask, cmpval);

	data = nv32_rd(pScrn, reg) & mask;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Checking if 0x%08X equals 0x%08X\n",
			   offset, data, cmpval);

	if (data == cmpval) {
		if (DEBUGLEVEL >= 6)
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: CONDITION FULFILLED - CONTINUING TO EXECUTE\n", offset);
	} else {
		if (DEBUGLEVEL >= 6)
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: CONDITION IS NOT FULFILLED\n", offset);
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: ------ SKIPPING FOLLOWING COMMANDS  ------\n", offset);
		iexec->execute = FALSE;
	}

	return TRUE;
}

static Bool init_index_io(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_INDEX_IO   opcode: 0x78 ('x')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): CRTC port
	 * offset + 3  (8  bit): CRTC index
	 * offset + 4  (8  bit): mask
	 * offset + 5  (8  bit): data
	 *
	 * Read value at index "CRTC index" on "CRTC port", AND with "mask", OR with "data", write-back
	 */

	uint16_t crtcport = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 1])));
	uint8_t crtcindex = bios->data[offset + 3];
	uint8_t mask = bios->data[offset + 4];
	uint8_t data = bios->data[offset + 5];
	uint8_t value;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Port: 0x%04X, Index: 0x%02X, Mask: 0x%02X, Data: 0x%02X\n",
			   offset, crtcport, crtcindex, mask, data);

	value = (nv_idx_port_rd(pScrn, crtcport, crtcindex) & mask) | data;
	nv_idx_port_wr(pScrn, crtcport, crtcindex, value);

	return TRUE;
}

static Bool init_pll(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_PLL   opcode: 0x79 ('y')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): register
	 * offset + 5  (16 bit): freq
	 *
	 * Set PLL register "register" to coefficients for frequency (10kHz) "freq"
	 */

	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint16_t freq = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 5])));

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Reg: 0x%08X, Freq: %d0kHz\n",
			   offset, reg, freq);

	setPLL(pScrn, bios, reg, freq * 10);

	return TRUE;
}

static Bool init_zm_reg(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_ZM_REG   opcode: 0x7A ('z')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): register
	 * offset + 5  (32 bit): value
	 *
	 * Assign "value" to "register"
	 */

	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint32_t value = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 5])));

	if (!iexec->execute)
		return TRUE;

	nv32_wr(pScrn, reg, value);

	return TRUE;
}

/* hack to avoid moving the itbl_entry array before this function */
int init_ram_restrict_zm_reg_group_blocklen = 0;

static Bool init_ram_restrict_zm_reg_group(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_RAM_RESTRICT_ZM_REG_GROUP   opcode: 0x8F ('')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): reg
	 * offset + 5  (8  bit): regincrement
	 * offset + 6  (8  bit): count
	 * offset + 7  (32 bit): value 1,1
	 * ...
	 *
	 * Use the RAMCFG strap of PEXTDEV_BOOT as an index into the table at
	 * ram_restrict_table_ptr. The value read from here is 'n', and
	 * "value 1,n" gets written to "reg". This repeats "count" times and on
	 * each iteration 'm', "reg" increases by "regincrement" and
	 * "value m,n" is used. The extent of n is limited by a number read
	 * from the 'M' BIT table, herein called "blocklen"
	 */

	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint8_t regincrement = bios->data[offset + 5];
	uint8_t count = bios->data[offset + 6];
	uint32_t strap_ramcfg, data;
	uint16_t blocklen;
	uint8_t index;
	int i;

	/* previously set by 'M' BIT table */
	blocklen = init_ram_restrict_zm_reg_group_blocklen;

	if (!iexec->execute)
		return TRUE;

	if (!blocklen) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Zero block length - has the M table been parsed?\n", offset);
		return FALSE;
	}

	strap_ramcfg = (nv32_rd(pScrn, NV_PEXTDEV_BOOT_0) >> 2) & 0xf;
	index = bios->data[bios->ram_restrict_tbl_ptr + strap_ramcfg];

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Reg: 0x%08X, RegIncrement: 0x%02X, Count: 0x%02X, StrapRamCfg: 0x%02X, Index: 0x%02X\n",
			   offset, reg, regincrement, count, strap_ramcfg, index);

	for (i = 0; i < count; i++) {
		data = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 7 + index * 4 + blocklen * i])));

		nv32_wr(pScrn, reg, data);

		reg += regincrement;
	}

	return TRUE;
}

static Bool init_copy_zm_reg(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_COPY_ZM_REG   opcode: 0x90 ('')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): src reg
	 * offset + 5  (32 bit): dst reg
	 *
	 * Put contents of "src reg" into "dst reg"
	 */

	uint32_t srcreg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint32_t dstreg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 5])));

	if (!iexec->execute)
		return TRUE;

	nv32_wr(pScrn, dstreg, nv32_rd(pScrn, srcreg));

	return TRUE;
}

static Bool init_zm_reg_group_addr_latched(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_ZM_REG_GROUP_ADDRESS_LATCHED   opcode: 0x91 ('')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): src reg
	 * offset + 5  (8  bit): count
	 * offset + 6  (32 bit): data 1
	 * ...
	 *
	 * For each of "count" values write "data n" to "src reg"
	 */

	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint8_t count = bios->data[offset + 5];
	int i;

	if (!iexec->execute)
		return TRUE;

	for (i = 0; i < count; i++) {
		uint32_t data = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 6 + 4 * i])));
		nv32_wr(pScrn, reg, data);
	}

	return TRUE;
}

static Bool init_reserved(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_RESERVED   opcode: 0x92 ('')
	 *
	 * offset      (8 bit): opcode
	 *
	 * Seemingly does nothing
	 */

	return TRUE;
}

static init_tbl_entry_t itbl_entry[] = {
	/* command name                       , id  , length  , offset  , mult    , command handler                 */
//	{ "INIT_PROG"                         , 0x31, 15      , 10      , 4       , init_prog                       },
	{ "INIT_IO_RESTRICT_PROG"             , 0x32, 11      , 6       , 4       , init_io_restrict_prog           },
	{ "INIT_REPEAT"                       , 0x33, 2       , 0       , 0       , init_repeat                     },
	{ "INIT_IO_RESTRICT_PLL"              , 0x34, 12      , 7       , 2       , init_io_restrict_pll            },
	{ "INIT_END_REPEAT"                   , 0x36, 1       , 0       , 0       , init_end_repeat                 },
	{ "INIT_COPY"                         , 0x37, 11      , 0       , 0       , init_copy                       },
	{ "INIT_NOT"                          , 0x38, 1       , 0       , 0       , init_not                        },
	{ "INIT_IO_FLAG_CONDITION"            , 0x39, 2       , 0       , 0       , init_io_flag_condition          },
	{ "INIT_INDEX_ADDRESS_LATCHED"        , 0x49, 18      , 17      , 2       , init_idx_addr_latched           },
	{ "INIT_IO_RESTRICT_PLL2"             , 0x4A, 11      , 6       , 4       , init_io_restrict_pll2           },
	{ "INIT_PLL2"                         , 0x4B, 9       , 0       , 0       , init_pll2                       },
/*	{ "INIT_I2C_BYTE"                     , 0x4C, x       , x       , x       , init_i2c_byte                   }, */
/*	{ "INIT_ZM_I2C_BYTE"                  , 0x4D, x       , x       , x       , init_zm_i2c_byte                }, */
/*	{ "INIT_ZM_I2C"                       , 0x4E, x       , x       , x       , init_zm_i2c                     }, */
	{ "INIT_TMDS"                         , 0x4F, 5       , 0       , 0       , init_tmds                       },
	{ "INIT_ZM_TMDS_GROUP"                , 0x50, 3       , 2       , 2       , init_zm_tmds_group              },
	{ "INIT_CR_INDEX_ADDRESS_LATCHED"     , 0x51, 5       , 4       , 1       , init_cr_idx_adr_latch           },
	{ "INIT_CR"                           , 0x52, 4       , 0       , 0       , init_cr                         },
	{ "INIT_ZM_CR"                        , 0x53, 3       , 0       , 0       , init_zm_cr                      },
	{ "INIT_ZM_CR_GROUP"                  , 0x54, 2       , 1       , 2       , init_zm_cr_group                },
	{ "INIT_CONDITION_TIME"               , 0x56, 3       , 0       , 0       , init_condition_time             },
	{ "INIT_ZM_REG_SEQUENCE"              , 0x58, 6       , 5       , 4       , init_zm_reg_sequence            },
//	{ "INIT_INDIRECT_REG"                 , 0x5A, 7       , 0       , 0       , init_indirect_reg               },
	{ "INIT_SUB_DIRECT"                   , 0x5B, 3       , 0       , 0       , init_sub_direct                 },
	{ "INIT_COPY_NV_REG"                  , 0x5F, 22      , 0       , 0       , init_copy_nv_reg                },
	{ "INIT_ZM_INDEX_IO"                  , 0x62, 5       , 0       , 0       , init_zm_index_io                },
	{ "INIT_COMPUTE_MEM"                  , 0x63, 1       , 0       , 0       , init_compute_mem                },
	{ "INIT_RESET"                        , 0x65, 13      , 0       , 0       , init_reset                      },
/*	{ "INIT_NEXT"                         , 0x66, x       , x       , x       , init_next                       }, */	
/*	{ "INIT_NEXT"                         , 0x67, x       , x       , x       , init_next                       }, */	
/*	{ "INIT_NEXT"                         , 0x68, x       , x       , x       , init_next                       }, */	
//	{ "INIT_INDEX_IO8"                    , 0x69, 5       , 0       , 0       , init_index_io8                  },
	{ "INIT_SUB"                          , 0x6B, 2       , 0       , 0       , init_sub                        },
//	{ "INIT_RAM_CONDITION"                , 0x6D, 3       , 0       , 0       , init_ram_condition              },
	{ "INIT_NV_REG"                       , 0x6E, 13      , 0       , 0       , init_nv_reg                     },
	{ "INIT_MACRO"                        , 0x6F, 2       , 0       , 0       , init_macro                      },
	{ "INIT_DONE"                         , 0x71, 1       , 0       , 0       , init_done                       },
	{ "INIT_RESUME"                       , 0x72, 1       , 0       , 0       , init_resume                     },
//	{ "INIT_RAM_CONDITION2"               , 0x73, 9       , 0       , 0       , init_ram_condition2             },
	{ "INIT_TIME"                         , 0x74, 3       , 0       , 0       , init_time                       },
	{ "INIT_CONDITION"                    , 0x75, 2       , 0       , 0       , init_condition                  },
/*	{ "INIT_IO_CONDITION"                 , 0x76, x       , x       , x       , init_io_condition               }, */
	{ "INIT_INDEX_IO"                     , 0x78, 6       , 0       , 0       , init_index_io                   },
	{ "INIT_PLL"                          , 0x79, 7       , 0       , 0       , init_pll                        },
	{ "INIT_ZM_REG"                       , 0x7A, 9       , 0       , 0       , init_zm_reg                     },
	/* INIT_RAM_RESTRICT_ZM_REG_GROUP's mult is loaded by M table in BIT */
	{ "INIT_RAM_RESTRICT_ZM_REG_GROUP"    , 0x8F, 7       , 6       , 0       , init_ram_restrict_zm_reg_group  },
	{ "INIT_COPY_ZM_REG"                  , 0x90, 9       , 0       , 0       , init_copy_zm_reg                },
	{ "INIT_ZM_REG_GROUP_ADDRESS_LATCHED" , 0x91, 6       , 5       , 4       , init_zm_reg_group_addr_latched  },
	{ "INIT_RESERVED"                     , 0x92, 1       , 0       , 0       , init_reserved                   },
	{ 0                                   , 0   , 0       , 0       , 0       , 0                               }
};

static unsigned int get_init_table_entry_length(bios_t *bios, unsigned int offset, int i)
{
	/* Calculates the length of a given init table entry. */
	return itbl_entry[i].length + bios->data[offset + itbl_entry[i].length_offset]*itbl_entry[i].length_multiplier;
}

static void parse_init_table(ScrnInfoPtr pScrn, bios_t *bios, unsigned int offset, init_exec_t *iexec)
{
	/* Parses all commands in a init table. */

	/* We start out executing all commands found in the
	 * init table. Some op codes may change the status
	 * of this variable to SKIP, which will cause
	 * the following op codes to perform no operation until
	 * the value is changed back to EXECUTE.
	 */
	unsigned char id;
	int i;

	int count=0;
	/* Loop until INIT_DONE causes us to break out of the loop
	 * (or until offset > bios length just in case... )
	 * (and no more than 10000 iterations just in case... ) */
	while ((offset < bios->length) && (count++ < 10000)) {
		id = bios->data[offset];

		/* Find matching id in itbl_entry */
		for (i = 0; itbl_entry[i].name && (itbl_entry[i].id != id); i++)
			;

		if (itbl_entry[i].name) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x%04X: [ (0x%02X) - %s ]\n",
				   offset, itbl_entry[i].id, itbl_entry[i].name);

			/* execute eventual command handler */
			if (itbl_entry[i].handler)
				if (!(*itbl_entry[i].handler)(pScrn, bios, offset, iexec))
					break;
		} else {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: Init table command not found: 0x%02X\n", offset, id);
			break;
		}

		/* Add the offset of the current command including all data
		 * of that command. The offset will then be pointing on the
		 * next op code.
		 */
		offset += get_init_table_entry_length(bios, offset, i);
	}
}

static void parse_init_tables(ScrnInfoPtr pScrn, bios_t *bios)
{
	/* Loops and calls parse_init_table() for each present table. */

	int i = 0;
	uint16_t table;
	init_exec_t iexec = {TRUE, FALSE};

	while ((table = le16_to_cpu(*((uint16_t *)(&bios->data[bios->init_script_tbls_ptr + i]))))) {

		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x%04X: Parsing init table %d\n",
			table, i / 2);

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: ------ EXECUTING FOLLOWING COMMANDS ------\n", table);
		still_alive();
		parse_init_table(pScrn, bios, table, &iexec);
		i += 2;
	}
}

void link_head_and_output(ScrnInfoPtr pScrn, int head, int dcb_entry, Bool overrideval)
{
	/* The BIOS scripts don't do this for us, sadly
	 * Luckily we do know the values ;-)
	 *
	 * head < 0 indicates we wish to force a setting with the overrideval
	 * (for VT restore etc.)
	 */

	NVPtr pNv = NVPTR(pScrn);
	int preferred_output = (ffs(pNv->dcb_table.entry[dcb_entry].or) & OUTPUT_1) >> 1;
	uint8_t tmds04 = 0x80;
	uint32_t tmds_ctrl, tmds_ctrl2;

	/* Bit 3 crosswires output and bus. */
	if (head >= 0 && head != preferred_output)
		tmds04 = 0x88;
	if (head < 0 && overrideval)
		tmds04 = 0x88;

	if (pNv->dcb_table.entry[dcb_entry].type == OUTPUT_LVDS)
		tmds04 |= 0x01;

	tmds_ctrl = NV_PRAMDAC0_OFFSET + (preferred_output ? NV_PRAMDAC0_SIZE : 0) + NV_RAMDAC_FP_TMDS_CONTROL;
	tmds_ctrl2 = NV_PRAMDAC0_OFFSET + (preferred_output ? NV_PRAMDAC0_SIZE : 0) + NV_RAMDAC_FP_TMDS_CONTROL_2;

	Bool oldexecute = pNv->VBIOS.execute;
	pNv->VBIOS.execute = TRUE;
	nv32_wr(pScrn, tmds_ctrl + 4, tmds04);
	nv32_wr(pScrn, tmds_ctrl, 0x04);
	if (pNv->dcb_table.entry[dcb_entry].type == OUTPUT_LVDS && pNv->VBIOS.fp.dual_link)
		nv32_wr(pScrn, tmds_ctrl2 + 4, tmds04 ^ 0x08);
	else {
		/* I have encountered no dvi (dual-link or not) that sets to anything else. */
		/* Does this change beyond the 165 MHz boundary? */
		nv32_wr(pScrn, tmds_ctrl2 + 4, 0x0);
	}
	nv32_wr(pScrn, tmds_ctrl2, 0x04);
	pNv->VBIOS.execute = oldexecute;
}

static void call_lvds_manufacturer_script(ScrnInfoPtr pScrn, int head, int dcb_entry, enum LVDS_script script)
{
	NVPtr pNv = NVPTR(pScrn);
	bios_t *bios = &pNv->VBIOS;
	init_exec_t iexec = {TRUE, FALSE};

	uint8_t sub = bios->data[bios->fp.xlated_entry + script];
	uint16_t scriptofs = le16_to_cpu(*((CARD16 *)(&bios->data[bios->init_script_tbls_ptr + sub * 2])));
	Bool power_off_for_reset, reset_after_pclk_change;
	uint16_t off_on_delay;

	if (!bios->fp.xlated_entry || !sub || !scriptofs)
		return;

	if (script == LVDS_INIT && bios->data[scriptofs] != 'q') {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "LVDS init script not stubbed\n");
		return;
	}

	power_off_for_reset = bios->data[bios->fp.xlated_entry] & 1;
	reset_after_pclk_change = bios->data[bios->fp.xlated_entry] & 2;
	off_on_delay = le16_to_cpu(*(uint16_t *)&bios->data[bios->fp.xlated_entry + 7]);

	if (script == LVDS_PANEL_ON && reset_after_pclk_change)
		call_lvds_manufacturer_script(pScrn, head, dcb_entry, LVDS_RESET);
	if (script == LVDS_RESET && power_off_for_reset)
		call_lvds_manufacturer_script(pScrn, head, dcb_entry, LVDS_PANEL_OFF);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Calling LVDS script %d:\n", script);
	pNv->VBIOS.execute = TRUE;
	nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, NV_VGA_CRTCX_OWNER,
		   head ? NV_VGA_CRTCX_OWNER_HEADB : NV_VGA_CRTCX_OWNER_HEADA);
	parse_init_table(pScrn, bios, scriptofs, &iexec);
	pNv->VBIOS.execute = FALSE;

	if (script == LVDS_PANEL_OFF)
		usleep(off_on_delay * 1000);
	if (script == LVDS_RESET)
		link_head_and_output(pScrn, head, dcb_entry, FALSE);
}

static uint16_t clkcmptable(bios_t *bios, uint16_t clktable, uint16_t pxclk)
{
	int compare_record_len, i = 0;
	uint16_t compareclk, scriptptr = 0;

	if (bios->major_version < 5) /* pre BIT */
		compare_record_len = 3;
	else
		compare_record_len = 4;

	do {
		compareclk = le16_to_cpu(*((uint16_t *)&bios->data[clktable + compare_record_len * i]));
		if (pxclk >= compareclk) {
			if (bios->major_version < 5) {
				uint8_t tmdssub = bios->data[clktable + 2 + compare_record_len * i];
				scriptptr = le16_to_cpu(*((uint16_t *)(&bios->data[bios->init_script_tbls_ptr + tmdssub * 2])));
			} else
				scriptptr = le16_to_cpu(*((uint16_t *)&bios->data[clktable + 2 + compare_record_len * i]));
			break;
		}
		i++;
	} while (compareclk);

	return scriptptr;
}

static void rundigitaloutscript(ScrnInfoPtr pScrn, uint16_t scriptptr, int head, int dcb_entry)
{
	bios_t *bios = &NVPTR(pScrn)->VBIOS;
	init_exec_t iexec = {TRUE, FALSE};

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x%04X: Parsing digital output script table\n", scriptptr);
	bios->execute = TRUE;
	nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, NV_VGA_CRTCX_OWNER,
			head ? NV_VGA_CRTCX_OWNER_HEADB : NV_VGA_CRTCX_OWNER_HEADA);
	nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, 0x57, 0);
	nv_idx_port_wr(pScrn, CRTC_INDEX_COLOR, 0x58, dcb_entry);
	parse_init_table(pScrn, bios, scriptptr, &iexec);
	bios->execute = FALSE;

	link_head_and_output(pScrn, head, dcb_entry, FALSE);
}

static void run_lvds_table(ScrnInfoPtr pScrn, int head, int dcb_entry, enum LVDS_script script, uint16_t pxclk)
{
	/* The BIT LVDS table's header has the information to setup the
	 * necessary registers. Following the standard 4 byte header are:
	 * A bitmask byte and a dual-link transition pxclk valur for use in
	 * selecting the init script when not using straps; 4 script pointers
	 * for panel power, selected by output and on/off; and 8 table pointers
	 * for panel init, the needed one determined by output, and bits in the
	 * conf byte. These tables are similar to the TMDS tables, consisting
	 * of a list of pxclks and script pointers.
	 */

	NVPtr pNv = NVPTR(pScrn);
	bios_t *bios = &pNv->VBIOS;
	unsigned int fpstrapping, outputset = (pNv->dcb_table.entry[dcb_entry].or == 4) ? 1 : 0;
	uint16_t scriptptr = 0, clktable;
	uint8_t clktableptr = 0;

	fpstrapping = (nv32_rd(pScrn, NV_PEXTDEV_BOOT_0) >> 16) & 0xf;

	/* no sign of the "do reset on panel on" and "do panel off on reset" bits
	 * but it seems that doing so is necessary anyway */
	if (script == LVDS_PANEL_ON)
		run_lvds_table(pScrn, head, dcb_entry, LVDS_RESET, pxclk);
	if (script == LVDS_RESET)
		run_lvds_table(pScrn, head, dcb_entry, LVDS_PANEL_OFF, pxclk);

	/* for now we assume version 3.0 table - g80 support will need some changes */

	switch (script) {
	case LVDS_INIT:
		return;
	case LVDS_BACKLIGHT_ON:	// check applicability of the script for this
	case LVDS_PANEL_ON:
		scriptptr = le16_to_cpu(*(uint16_t *)&bios->data[bios->fp.lvdsmanufacturerpointer + 7 + outputset * 2]);
		break;
	case LVDS_BACKLIGHT_OFF:	// check applicability of the script for this
	case LVDS_PANEL_OFF:
		scriptptr = le16_to_cpu(*(uint16_t *)&bios->data[bios->fp.lvdsmanufacturerpointer + 11 + outputset * 2]);
		break;
	case LVDS_RESET:
		if (pNv->dcb_table.entry[dcb_entry].lvdsconf.use_straps_for_mode ||
			(fpstrapping != 0x0f && bios->data[bios->fp.xlated_entry + 1] != 0x0f)) {
			if (bios->fp.dual_link)
				clktableptr += 2;
			if (bios->fp.BITbit1)
				clktableptr++;
		} else {
			uint8_t fallback = bios->data[bios->fp.lvdsmanufacturerpointer + 4];
			int fallbackcmpval = (pNv->dcb_table.entry[dcb_entry].or == 4) ? 4 : 1;
			uint8_t dltransitionclk = le16_to_cpu(*(uint16_t *)&bios->data[bios->fp.lvdsmanufacturerpointer + 5]);
			if (pxclk > dltransitionclk) {	// dual-link
				clktableptr += 2;
				fallbackcmpval *= 2;
			}
			if (fallbackcmpval & fallback)
				clktableptr++;
		}

		/* adding outputset * 8 may not be correct */
		clktable = le16_to_cpu(*(uint16_t *)&bios->data[bios->fp.lvdsmanufacturerpointer + 15 + clktableptr * 2 + outputset * 8]);
		if (!clktable) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Pixel clock comparison table not found\n");
			return;
		}
		scriptptr = clkcmptable(bios, clktable, pxclk);
	}

	if (!scriptptr) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "LVDS output init script not found\n");
		return;
	}
	rundigitaloutscript(pScrn, scriptptr, head, dcb_entry);
}

void call_lvds_script(ScrnInfoPtr pScrn, int head, int dcb_entry, enum LVDS_script script, uint16_t pxclk)
{
	/* LVDS operations are multiplexed in an effort to present a single API
	 * which works with two vastly differing underlying structures.
	 * This acts as the demux
	 */

	bios_t *bios = &NVPTR(pScrn)->VBIOS;
	uint8_t lvds_ver = bios->data[bios->fp.lvdsmanufacturerpointer];

	if (!lvds_ver)
		return;

	if (lvds_ver < 0x30)
		call_lvds_manufacturer_script(pScrn, head, dcb_entry, script);
	else
		run_lvds_table(pScrn, head, dcb_entry, script, pxclk);
}

struct fppointers {
	uint16_t fptablepointer;
	uint16_t fpxlatetableptr;
	uint16_t fpxlatemanufacturertableptr;
	int xlatwidth;
};

static void parse_fp_mode_table(ScrnInfoPtr pScrn, bios_t *bios, struct fppointers *fpp)
{
	unsigned int fpstrapping;
	uint8_t *fptable;
	uint8_t fptable_ver, headerlen = 0, recordlen, fpentries = 0xf, fpindex;
	int ofs;
	DisplayModePtr mode;

	fpstrapping = (nv32_rd(pScrn, NV_PEXTDEV_BOOT_0) >> 16) & 0xf;

	if (fpp->fptablepointer == 0x0 || fpp->fpxlatetableptr == 0x0) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Pointers to flat panel table invalid\n");
		return;
	}

	fptable = &bios->data[fpp->fptablepointer];

	fptable_ver = fptable[0];

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Found flat panel mode table revision %d.%d\n",
		   fptable_ver >> 4, fptable_ver & 0xf);

	switch (fptable_ver) {
	/* BMP version 0x5.0x11 BIOSen have version 1 like tables, but no version field,
	 * and miss one of the spread spectrum/PWM bytes.
	 * This could affect early GF2Go parts (not seen any appropriate ROMs though).
	 * Here we assume that a version of 0x05 matches this case (combining with a
	 * BMP version check would be better), as the common case for the panel type
	 * field is 0x0005, and that is in fact what we are reading the first byte of. */
	case 0x05:	/* some NV10, 11, 15, 16 */
		recordlen = 42;
		ofs = 6;
		break;
	case 0x10:	/* some NV15/16, and NV11+ */
		recordlen = 44;
		ofs = 7;
		break;
	case 0x20:	/* NV40+ */
		headerlen = fptable[1];
		recordlen = fptable[2];
		fpentries = fptable[3];
		ofs = 0;
		break;
	default:
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "FP Table revision not currently supported\n");
		return;
	}

	fpindex = bios->data[fpp->fpxlatetableptr + fpstrapping * fpp->xlatwidth];
	if (fpindex > fpentries) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Bad flat panel table index\n");
		return;
	}

	if (!(mode = xcalloc(1, sizeof(DisplayModeRec))))
		return;

	int modeofs = headerlen + recordlen * fpindex + ofs;
	mode->Clock = le16_to_cpu(*(uint16_t *)&fptable[modeofs]) * 10;
	mode->HDisplay = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 2]);
	mode->HSyncStart = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 10] + 1);
	mode->HSyncEnd = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 12] + 1);
	mode->HTotal = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 14] + 1);
	mode->VDisplay = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 16]);
	mode->VSyncStart = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 24] + 1);
	mode->VSyncEnd = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 26] + 1);
	mode->VTotal = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 28] + 1);
	mode->Flags |= (fptable[modeofs + 30] & 0x10) ? V_PHSYNC : V_NHSYNC;
	mode->Flags |= (fptable[modeofs + 30] & 0x1) ? V_PVSYNC : V_NVSYNC;

	/* for version 1.0:
	 * bytes 1-2 are "panel type", including bits on whether Colour/mono, single/dual link, and type (TFT etc.)
	 * bytes 3-6 are bits per colour in RGBX
	 * 11-12 is HDispEnd
	 * 13-14 is HValid Start
	 * 15-16 is HValid End
	 * bytes 38-39 relate to spread spectrum settings
	 * bytes 40-43 are something to do with PWM */

	mode->prev = mode->next = NULL;
	mode->status = MODE_OK;
	mode->type = M_T_DRIVER | M_T_PREFERRED;
	xf86SetModeDefaultName(mode);

//	if (XF86_CRTC_CONFIG_PTR(pScrn)->debug_modes) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Found flat panel mode in BIOS tables:\n");
		xf86PrintModeline(pScrn->scrnIndex, mode);
//	}

	bios->fp.native_mode = mode;
}

static void parse_lvds_manufacturer_table_init(ScrnInfoPtr pScrn, bios_t *bios, struct fppointers *fpp)
{
	/* The LVDS table changed considerably with BIT bioses. Previously
	 * there was a header of version and record length, followed by several
	 * records, indexed by a seperate xlat table, indexed in turn by the fp
	 * strap in EXTDEV_BOOT. Each record had a config byte, followed by 6
	 * script numbers for use by INIT_SUB which controlled panel init and
	 * power, and finally a dword of ms to sleep between power off and on
	 * operations.
	 *
	 * The BIT LVDS table has the typical BIT table header: version byte,
	 * header length byte, record length byte, and a byte for the maximum
	 * number of records that can be held in the table.
	 *
	 * The table following the header serves as an integrated config and
	 * xlat table: the records in the table are indexed by the FP strap
	 * nibble in EXTDEV_BOOT, and each record has two bytes - the first as
	 * a config byte, the second for indexing the fp mode table pointed to
	 * by the BIT 'D' table
	 */

	unsigned int fpstrapping, lvdsmanufacturerindex = 0;
	uint8_t lvds_ver, headerlen, recordlen;

	fpstrapping = (nv32_rd(pScrn, NV_PEXTDEV_BOOT_0) >> 16) & 0xf;

	if (bios->fp.lvdsmanufacturerpointer == 0x0) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Pointer to LVDS manufacturer table invalid\n");
		return;
	}

	lvds_ver = bios->data[bios->fp.lvdsmanufacturerpointer];

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Found LVDS manufacturer table revision %d.%d\n",
		   lvds_ver >> 4, lvds_ver & 0xf);

	switch (lvds_ver) {
	case 0x0a:	/* pre NV40 */
		lvdsmanufacturerindex = bios->data[fpp->fpxlatemanufacturertableptr + fpstrapping];

		headerlen = 2;
		recordlen = bios->data[bios->fp.lvdsmanufacturerpointer + 1];

		break;
	case 0x30:	/* NV4x */
		lvdsmanufacturerindex = fpstrapping;
		headerlen = bios->data[bios->fp.lvdsmanufacturerpointer + 1];
		if (headerlen < 0x1f) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "LVDS table header not understood\n");
			return;
		}
		recordlen = bios->data[bios->fp.lvdsmanufacturerpointer + 2];
		break;
	case 0x40:	/* It changed again with gf8 :o( */
	default:
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "LVDS table revision not currently supported\n");
		return;
	}

	uint16_t lvdsofs = bios->fp.xlated_entry = bios->fp.lvdsmanufacturerpointer + headerlen + recordlen * lvdsmanufacturerindex;
	switch (lvds_ver) {
	case 0x0a:
		bios->fp.dual_link = bios->data[lvdsofs] & 4;
		bios->fp.if_is_18bit = !(bios->data[lvdsofs] & 16);
		break;
	case 0x30:
		bios->fp.dual_link = bios->data[lvdsofs] & 1;
		bios->fp.BITbit1 = bios->data[lvdsofs] & 2;
		fpp->fpxlatetableptr = bios->fp.lvdsmanufacturerpointer + headerlen + 1;
		fpp->xlatwidth = recordlen;
		break;
	}
}

void run_tmds_table(ScrnInfoPtr pScrn, int dcb_entry, int head, uint16_t pxclk)
{
	/* the dcb_entry parameter is the index of the appropriate DCB entry
	 * the pxclk parameter is in 10s of kHz (eg. 108Mhz is 10800, or 0x2a30)
	 *
	 * This runs the TMDS regs setting code found on BIT bios cards
	 *
	 * For ffs(or) == 1 use the first table, for ffs(or) == 2 and
	 * ffs(or) == 3, use the second.
	 */

	NVPtr pNv = NVPTR(pScrn);
	bios_t *bios = &pNv->VBIOS;
	uint16_t clktable = 0, scriptptr;

	if (pNv->dcb_table.entry[dcb_entry].location) /* off chip */
		return;

	switch (ffs(pNv->dcb_table.entry[dcb_entry].or)) {
	case 1:
		clktable = bios->tmds.output0_script_ptr;
		break;
	case 2:
	case 3:
		clktable = bios->tmds.output1_script_ptr;
		break;
	}

	if (!clktable) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Pixel clock comparison table not found\n");
		return;
	}

	scriptptr = clkcmptable(bios, clktable, pxclk);

	if (!scriptptr) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "TMDS output init script not found\n");
		return;
	}

	rundigitaloutscript(pScrn, scriptptr, head, dcb_entry);
}

static void parse_bios_version(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset)
{
	/* offset + 0  (8 bits): Micro version
	 * offset + 1  (8 bits): Minor version
	 * offset + 2  (8 bits): Chip version
	 * offset + 3  (8 bits): Major version
	 */

	bios->major_version = bios->data[offset + 3];
	bios->chip_version = bios->data[offset + 2];
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Bios version %02x.%02x.%02x.%02x\n",
		   bios->data[offset + 3], bios->data[offset + 2],
		   bios->data[offset + 1], bios->data[offset]);
}

Bool get_pll_limits(ScrnInfoPtr pScrn, enum pll_types plltype, struct pll_lims *pll_lim)
{
	/* PLL limits table
	 *
	 * Version 0x10: NV31
	 * One byte header (version), one record of 24 bytes
	 * Version 0x11: NV36 - Not implemented
	 * Seems to have same record style as 0x10, but 3 records rather than 1
	 * Version 0x20: Found on Geforce 6 cards
	 * Trivial 4 byte BIT header. 31 (0x1f) byte record length
	 * Version 0x21: Found on Geforce 7, 8 and some Geforce 6 cards
	 * 5 byte header, fifth byte of unknown purpose. 35 (0x23) byte record length
	 */

	bios_t *bios = &NVPTR(pScrn)->VBIOS;
	uint8_t pll_lim_ver, headerlen, recordlen, entries;
	int pllindex = 0, i;

	if (!bios->pll_limit_tbl_ptr) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Pointer to PLL limits table invalid\n");
		return FALSE;
	}

	pll_lim_ver = bios->data[bios->pll_limit_tbl_ptr + 0];

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Found BIT PLL limits table version 0x%X\n", pll_lim_ver);

	switch (pll_lim_ver) {
	case 0x10:
		headerlen = 1;
		recordlen = 0x18;
		entries = 1;
		pllindex = 0;
		break;
	case 0x20:
	case 0x21:
		headerlen = bios->data[bios->pll_limit_tbl_ptr + 1];
		recordlen = bios->data[bios->pll_limit_tbl_ptr + 2];
		entries = bios->data[bios->pll_limit_tbl_ptr + 3];
		break;
	default:
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "PLL limits table revision not currently supported\n");
		return FALSE;
	}

	/* initialize all members to zero */
	memset (pll_lim, 0, sizeof(struct pll_lims));

	if (pll_lim_ver == 0x10) {
		uint16_t plloffs = bios->pll_limit_tbl_ptr + headerlen + recordlen * pllindex;

		pll_lim->vco1.minfreq = le32_to_cpu(*((uint32_t *)(&bios->data[plloffs])));
		pll_lim->vco1.maxfreq = le32_to_cpu(*((uint32_t *)(&bios->data[plloffs + 4])));
		pll_lim->vco2.minfreq = le32_to_cpu(*((uint32_t *)(&bios->data[plloffs + 8])));
		pll_lim->vco2.maxfreq = le32_to_cpu(*((uint32_t *)(&bios->data[plloffs + 12])));
		pll_lim->vco1.min_inputfreq = le32_to_cpu(*((uint32_t *)(&bios->data[plloffs + 16])));
		pll_lim->vco2.min_inputfreq = le32_to_cpu(*((uint32_t *)(&bios->data[plloffs + 20])));
	} else {	/* ver 0x20, 0x21 */
		uint16_t plloffs = bios->pll_limit_tbl_ptr + headerlen;

		for (i = 0; i < entries; i++) {
			uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[plloffs + recordlen * i])));

			if (plltype == VPLL1 && (reg == 0x680508 || reg == 0x4010)) {
				pllindex = i;
				break;
			}
			if (plltype == VPLL2 && (reg == 0x680520 || reg == 0x4018)) {
				pllindex = i;
				break;
			}
			if (reg == 0x0000) /* generic pll settings */
				pllindex = i;
		}

		plloffs += recordlen * pllindex;

		if (DEBUGLEVEL >= 6)
			xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Loading PLL limits for reg 0x%08x\n",
				   le32_to_cpu(*((uint32_t *)(&bios->data[plloffs]))));

		/* What output frequencies can each VCO generate? */
		pll_lim->vco1.minfreq = le16_to_cpu(*((uint16_t *)(&bios->data[plloffs + 4])));
		pll_lim->vco1.maxfreq = le16_to_cpu(*((uint16_t *)(&bios->data[plloffs + 6])));
		pll_lim->vco2.minfreq = le16_to_cpu(*((uint16_t *)(&bios->data[plloffs + 8])));
		pll_lim->vco2.maxfreq = le16_to_cpu(*((uint16_t *)(&bios->data[plloffs + 10])));

		/* What input frequencies do they accept (past the m-divider)? */
		pll_lim->vco1.min_inputfreq = le16_to_cpu(*((uint16_t *)(&bios->data[plloffs + 12])));
		pll_lim->vco1.max_inputfreq = le16_to_cpu(*((uint16_t *)(&bios->data[plloffs + 14])));
		pll_lim->vco2.min_inputfreq = le16_to_cpu(*((uint16_t *)(&bios->data[plloffs + 16])));
		pll_lim->vco2.max_inputfreq = le16_to_cpu(*((uint16_t *)(&bios->data[plloffs + 18])));

		/* What values are accepted as multiplier and divider? */
		pll_lim->vco1.min_n = bios->data[plloffs + 20];
		pll_lim->vco1.max_n = bios->data[plloffs + 21];
		pll_lim->vco1.min_m = bios->data[plloffs + 22];
		pll_lim->vco1.max_m = bios->data[plloffs + 23];
		pll_lim->vco2.min_n = bios->data[plloffs + 24];
		pll_lim->vco2.max_n = bios->data[plloffs + 25];
		pll_lim->vco2.min_m = bios->data[plloffs + 26];
		pll_lim->vco2.max_m = bios->data[plloffs + 27];

		pll_lim->unk1c = bios->data[plloffs + 28];
		pll_lim->unk1d = bios->data[plloffs + 29];
		pll_lim->unk1e = bios->data[plloffs + 30];
	}

#if 1 /* for easy debugging */
	ErrorF("pll.vco1.minfreq: %d\n", pll_lim->vco1.minfreq);
	ErrorF("pll.vco1.maxfreq: %d\n", pll_lim->vco1.maxfreq);
	ErrorF("pll.vco2.minfreq: %d\n", pll_lim->vco2.minfreq);
	ErrorF("pll.vco2.maxfreq: %d\n", pll_lim->vco2.maxfreq);

	ErrorF("pll.vco1.min_inputfreq: %d\n", pll_lim->vco1.min_inputfreq);
	ErrorF("pll.vco1.max_inputfreq: %d\n", pll_lim->vco1.max_inputfreq);
	ErrorF("pll.vco2.min_inputfreq: %d\n", pll_lim->vco2.min_inputfreq);
	ErrorF("pll.vco2.max_inputfreq: %d\n", pll_lim->vco2.max_inputfreq);

	ErrorF("pll.vco1.min_n: %d\n", pll_lim->vco1.min_n);
	ErrorF("pll.vco1.max_n: %d\n", pll_lim->vco1.max_n);
	ErrorF("pll.vco1.min_m: %d\n", pll_lim->vco1.min_m);
	ErrorF("pll.vco1.max_m: %d\n", pll_lim->vco1.max_m);
	ErrorF("pll.vco2.min_n: %d\n", pll_lim->vco2.min_n);
	ErrorF("pll.vco2.max_n: %d\n", pll_lim->vco2.max_n);
	ErrorF("pll.vco2.min_m: %d\n", pll_lim->vco2.min_m);
	ErrorF("pll.vco2.max_m: %d\n", pll_lim->vco2.max_m);

	ErrorF("pll.unk1c: %d\n", pll_lim->unk1c);
	ErrorF("pll.unk1d: %d\n", pll_lim->unk1d);
	ErrorF("pll.unk1e: %d\n", pll_lim->unk1e);
#endif

	return TRUE;
}

static int parse_bit_B_tbl_entry(ScrnInfoPtr pScrn, bios_t *bios, bit_entry_t *bitentry)
{
	/* offset + 0  (32 bits): BIOS version dword
	 *
	 * There's a bunch of bits in this table other than the bios version
	 * that we don't use - their use currently unknown
	 */

	if (bitentry->length < 0x4) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Do not understand B table entry\n");
		return 0;
	}

	parse_bios_version(pScrn, bios, bitentry->offset);

	return 1;
}

static int parse_bit_C_tbl_entry(ScrnInfoPtr pScrn, bios_t *bios, bit_entry_t *bitentry)
{
	/* offset + 8  (16 bits): PLL limits table pointer
	 *
	 * There's more in here, but that's unknown.
	 */

	if (bitentry->length < 10) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Do not understand C table entry\n");
		return 0;
	}

	bios->pll_limit_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 8])));

	return 1;
}

static int parse_bit_display_tbl_entry(ScrnInfoPtr pScrn, bios_t *bios, bit_entry_t *bitentry, struct fppointers *fpp)
{
	/* Parses the flat panel table segment that the bit entry points to.
	 * Starting at bitentry->offset:
	 *
	 * offset + 0  (16 bits): FIXME table pointer
	 * offset + 2  (16 bits): mode table pointer
	 */

	if (bitentry->length != 4) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Do not understand BIT display table entry\n");
		return 0;
	}

	fpp->fptablepointer = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 2])));

	return 1;
}

static unsigned int parse_bit_init_tbl_entry(ScrnInfoPtr pScrn, bios_t *bios, bit_entry_t *bitentry)
{
	/* Parses the init table segment that the bit entry points to.
	 * Starting at bitentry->offset: 
	 * 
	 * offset + 0  (16 bits): init script tables pointer
	 * offset + 2  (16 bits): macro index table pointer
	 * offset + 4  (16 bits): macro table pointer
	 * offset + 6  (16 bits): condition table pointer
	 * offset + 8  (16 bits): io condition table pointer
	 * offset + 10 (16 bits): io flag condition table pointer
	 * offset + 12 (16 bits): init function table pointer
	 *
	 * TODO:
	 * * Are 'I' bit entries always of length 0xE?
	 * 
	 */

	if (bitentry->length < 14) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Unable to recognize BIT init table entry\n");
		return 0;
	}

	bios->init_script_tbls_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset])));
	bios->macro_index_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 2])));
	bios->macro_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 4])));
	bios->condition_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 6])));
	bios->io_condition_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 8])));
	bios->io_flag_condition_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 10])));
	bios->init_function_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 12])));

	return 1;
}

static int parse_bit_lvds_tbl_entry(ScrnInfoPtr pScrn, bios_t *bios, bit_entry_t *bitentry, struct fppointers *fpp)
{
	/* Parses the LVDS table segment that the bit entry points to.
	 * Starting at bitentry->offset:
	 *
	 * offset + 0  (16 bits): LVDS strap xlate table pointer
	 */

	if (bitentry->length != 2) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Do not understand BIT LVDS table entry\n");
		return 0;
	}

	/* no idea if it's still called the LVDS manufacturer table, but the concept's close enough */
	bios->fp.lvdsmanufacturerpointer = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset])));

	parse_lvds_manufacturer_table_init(pScrn, bios, fpp);

	return 1;
}

static int parse_bit_M_tbl_entry(ScrnInfoPtr pScrn, bios_t *bios, bit_entry_t *bitentry)
{
	/* offset + 2  (8  bits): number of options in an INIT_RAM_RESTRICT_ZM_REG_GROUP opcode option set
	 * offset + 3  (16 bits): pointer to strap xlate table for RAM restrict option selection
	 *
	 * There's a bunch of bits in this table other than the RAM restrict
	 * stuff that we don't use - their use currently unknown
	 */

	int i;

	/* Older bios versions don't have a sufficiently long table for what we want */
	if (bitentry->length < 0x5)
		return 1;

	/* set up multiplier for INIT_RAM_RESTRICT_ZM_REG_GROUP */
	for (i = 0; itbl_entry[i].name && (itbl_entry[i].id != 0x8f); i++)
		;
	itbl_entry[i].length_multiplier = bios->data[bitentry->offset + 2] * 4;
	init_ram_restrict_zm_reg_group_blocklen = itbl_entry[i].length_multiplier;

	bios->ram_restrict_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 3])));

	return 1;
}

static int parse_bit_tmds_tbl_entry(ScrnInfoPtr pScrn, bios_t *bios, bit_entry_t *bitentry)
{
	/* Parses the pointer to the TMDS table
	 *
	 * Starting at bitentry->offset:
	 *
	 * offset + 0  (16 bits): TMDS table pointer
	 *
	 * The TMDS table is typically found just before the DCB table, with a
	 * characteristic signature of 0x11,0x13 (1.1 being version, 0x13 being
	 * length?)
	 *
	 * At offset +7 is a pointer to a script, which I don't know how to run yet
	 * At offset +9 is a pointer to another script, likewise
	 * Offset +11 has a pointer to a table where the first word is a pxclk
	 * frequency and the second word a pointer to a script, which should be
	 * run if the comparison pxclk frequency is less than the pxclk desired.
	 * This repeats for decreasing comparison frequencies
	 * Offset +13 has a pointer to a similar table
	 * The selection of table (and possibly +7/+9 script) is dictated by
	 * "or" from the DCB.
	 */

	uint16_t tmdstableptr, script1, script2;

	if (bitentry->length != 2) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Do not understand BIT TMDS table entry\n");
		return 0;
	}

	tmdstableptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset])));

	if (tmdstableptr == 0x0) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Pointer to TMDS table invalid\n");
		return 0;
	}

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Found TMDS table revision %d.%d\n",
		   bios->data[tmdstableptr] >> 4, bios->data[tmdstableptr] & 0xf);

	/* These two scripts are odd: they don't seem to get run even when they are not stubbed */
	script1 = le16_to_cpu(*((uint16_t *)&bios->data[tmdstableptr + 7]));
	script2 = le16_to_cpu(*((uint16_t *)&bios->data[tmdstableptr + 9]));
	if (bios->data[script1] != 'q' || bios->data[script2] != 'q')
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "TMDS table script pointers not stubbed\n");

	bios->tmds.output0_script_ptr = le16_to_cpu(*((uint16_t *)&bios->data[tmdstableptr + 11]));
	bios->tmds.output1_script_ptr = le16_to_cpu(*((uint16_t *)&bios->data[tmdstableptr + 13]));

	return 1;
}

static void parse_bit_structure(ScrnInfoPtr pScrn, bios_t *bios, unsigned int offset)
{
	bit_entry_t bitentry;
	char done = 0;
	struct fppointers fpp;
	NVPtr pNv = NVPTR(pScrn);

	memset(&fpp, 0, sizeof(struct fppointers));

	while (!done) {
		bitentry.id[0] = bios->data[offset];
		bitentry.id[1] = bios->data[offset + 1];
		bitentry.length = le16_to_cpu(*((uint16_t *)&bios->data[offset + 2]));
		bitentry.offset = le16_to_cpu(*((uint16_t *)&bios->data[offset + 4]));

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Found BIT command with id 0x%02X (%c)\n",
			   offset, bitentry.id[0], bitentry.id[0]);

		switch (bitentry.id[0]) {
		case 0:
			/* id[0] = 0 and id[1] = 0 ==> end of BIT struture */
			if (bitentry.id[1] == 0)
				done = 1;
			break;
		case 'B':
			parse_bit_B_tbl_entry(pScrn, bios, &bitentry);
			break;
		case 'C':
			parse_bit_C_tbl_entry(pScrn, bios, &bitentry);
			break;
		case 'D':
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: Found flat panel display table entry in BIT structure\n", offset);
			parse_bit_display_tbl_entry(pScrn, bios, &bitentry, &fpp);
			break;
		case 'I':
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: Found init table entry in BIT structure\n", offset);
			parse_bit_init_tbl_entry(pScrn, bios, &bitentry);
			break;
		case 'L':
			parse_bit_lvds_tbl_entry(pScrn, bios, &bitentry, &fpp);
			break;
		case 'M': /* memory? */
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: Found M table entry in BIT structure\n", offset);
			parse_bit_M_tbl_entry(pScrn, bios, &bitentry);
			break;
		case 'T':
			parse_bit_tmds_tbl_entry(pScrn, bios, &bitentry);
			break;

			/* TODO: What kind of information does the other BIT entrys point to?
			 *       'P' entry is probably performance tables, but there are
			 *       quite a few others...
			 */
		}

		offset += sizeof(bit_entry_t);
	}

	/* C and M tables have to be parsed before init can run */
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Parsing previously deferred init table entry\n");
	parse_init_tables(pScrn, bios);

	/* If it's not a laptop, you probably don't care about LVDS */
	/* FIXME: detect mobile BIOS? */
	if (!pNv->Mobile)
		return;

	/* Need D and L tables parsed before doing this */
	parse_fp_mode_table(pScrn, bios, &fpp);
}

static void parse_bmp_structure(ScrnInfoPtr pScrn, bios_t *bios, unsigned int offset)
{
	/* Parse the BMP structure for useful things
	 *
	 * offset +   5: BMP major version
	 * offset +   6: BMP minor version
	 * offset +  10: BCD encoded BIOS version
	 *
	 * offset +  18: init script table pointer (for bios versions < 5.10h)
	 * offset +  20: extra init script table pointer (for bios versions < 5.10h)
	 *
	 * offset +  24: FIXME
	 * offset +  26: FIXME
	 * offset +  28: FIXME
	 *
	 * offset +  54: index of I2C CRTC pair to use for CRT output
	 * offset +  55: index of I2C CRTC pair to use for TV output
	 * offset +  56: index of I2C CRTC pair to use for flat panel output
	 * offset +  58: write CRTC index for I2C pair 0
	 * offset +  59: read CRTC index for I2C pair 0
	 * offset +  60: write CRTC index for I2C pair 1
	 * offset +  61: read CRTC index for I2C pair 1
	 *
	 * offset +  67: maximum internal PLL frequency (single stage PLL)
	 * offset +  71: minimum internal PLL frequency (single stage PLL)
	 *
	 * offset +  75: script table pointers, as for parse_bit_init_tbl_entry
	 *
	 * offset +  89: TMDS single link output A table pointer
	 * offset +  91: TMDS single link output B table pointer
	 * offset + 105: flat panel timings table pointer
	 * offset + 107: flat panel strapping translation table pointer
	 * offset + 117: LVDS manufacturer panel config table pointer
	 * offset + 119: LVDS manufacturer strapping translation table pointer
	 *
	 * offset + 142: PLL limits table pointer
	 */

	NVPtr pNv = NVPTR(pScrn);
	uint16_t bmplength;
	struct fppointers fpp;
	memset(&fpp, 0, sizeof(struct fppointers));

	uint8_t bmp_version_major = bios->data[offset + 5];
	uint8_t bmp_version_minor = bios->data[offset + 6];

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "BMP version %d.%d\n",
		   bmp_version_major, bmp_version_minor);

	/* version 6 could theoretically exist, but I suspect BIT happened instead */
	if (bmp_version_major < 2 || bmp_version_major > 5) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "You have an unsupported BMP version. Please send in your bios\n");
		return;
	}

	if (bmp_version_major == 2)
		bmplength = 48; /* exact for 2.01 - not sure if minor version used in versions < 5 */
	else if (bmp_version_major == 3)
		bmplength = 54; /* guessed - mem init tables added in this version */
	else if (bmp_version_major == 4 || bmp_version_minor < 0x1) /* don't know if 5.0 exists... */
		bmplength = 62; /* guessed - BMP I2C indices added in version 4*/
	else if (bmp_version_minor < 0x6)
		bmplength = 67; /* exact for 5.01 */
	else if (bmp_version_minor < 0x10)
		bmplength = 75; /* exact for 5.06 */
	else if (bmp_version_minor == 0x10)
		bmplength = 89; /* exact for 5.10h */
	else if (bmp_version_minor < 0x14)
		bmplength = 118; /* exact for 5.11h */
	else if (bmp_version_minor < 0x24) /* not sure of version where pll limits came in;
					    * certainly exist by 0x24 though */
		/* length not exact: this is long enough to get lvds members */
		bmplength = 123;
	else
		/* length not exact: this is long enough to get pll limit member */
		bmplength = 144;

	/* checksum */
	if (nv_cksum(bios->data + offset, 8)) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Bad BMP checksum\n");
		return;
	}

	parse_bios_version(pScrn, bios, offset + 10);

	bios->init_script_tbls_ptr = le16_to_cpu(*(uint16_t *)&bios->data[offset + 18]);
	bios->extra_init_script_tbl_ptr = le16_to_cpu(*(uint16_t *)&bios->data[offset + 20]);

#if 0
	// FIXME needed for pre v16? - haiku uses this in its COMPUTE_MEM on early biosen
	if (bmp_version_major > 2) {
		uint16_t meminittbl = le16_to_cpu(*(uint16_t *)&bios->data[offset + 24]);
		uint16_t sdrmemseqtbl = le16_to_cpu(*(uint16_t *)&bios->data[offset + 26]);
		uint16_t ddrmemseqtbl = le16_to_cpu(*(uint16_t *)&bios->data[offset + 28]);
	}
#endif

	uint16_t legacy_i2c_offset = 0x48;	/* BMP version 2 & 3 */
	if (bmplength > 61)
		legacy_i2c_offset = offset + 54;
	bios->legacy_i2c_indices.crt = bios->data[legacy_i2c_offset];
	bios->legacy_i2c_indices.tv = bios->data[legacy_i2c_offset + 1];
	bios->legacy_i2c_indices.panel = bios->data[legacy_i2c_offset + 2];
	pNv->dcb_table.i2c_write[0] = bios->data[legacy_i2c_offset + 4];
	pNv->dcb_table.i2c_read[0] = bios->data[legacy_i2c_offset + 5];
	pNv->dcb_table.i2c_write[1] = bios->data[legacy_i2c_offset + 6];
	pNv->dcb_table.i2c_read[1] = bios->data[legacy_i2c_offset + 7];

	if (bmplength > 74) {
		bios->fmaxvco = le32_to_cpu(*((uint32_t *)&bios->data[offset + 67]));
		bios->fminvco = le32_to_cpu(*((uint32_t *)&bios->data[offset + 71]));
	}
	if (bmplength > 88) {
		bit_entry_t initbitentry;
		initbitentry.length = bmplength - 75;
		initbitentry.offset = offset + 75;
		parse_bit_init_tbl_entry(pScrn, bios, &initbitentry);
	}
	if (bmplength > 92) {
		bios->tmds.output0_script_ptr = le16_to_cpu(*((uint16_t *)&bios->data[offset + 89]));
		bios->tmds.output1_script_ptr = le16_to_cpu(*((uint16_t *)&bios->data[offset + 91]));
	}
	if (bmplength > 108) {
		fpp.fptablepointer = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 105])));
		fpp.fpxlatetableptr = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 107])));
		fpp.xlatwidth = 1;
	}
	if (bmplength > 120) {
		bios->fp.lvdsmanufacturerpointer = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 117])));
		fpp.fpxlatemanufacturertableptr = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 119])));
	}
	if (bmplength > 143)
		bios->pll_limit_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 142])));

	/* want pll_limit_tbl_ptr set (if available) before init is run */
	if (bmp_version_major < 5 || bmp_version_minor < 0x10) {
		init_exec_t iexec = {TRUE, FALSE};
		parse_init_table(pScrn, bios, bios->init_script_tbls_ptr, &iexec);
		parse_init_table(pScrn, bios, bios->extra_init_script_tbl_ptr, &iexec);
	} else
		parse_init_tables(pScrn, bios);

	/* If it's not a laptop, you probably don't care about fptables */
	/* FIXME: detect mobile BIOS? */
	if (!pNv->Mobile)
		return;

	parse_fp_mode_table(pScrn, bios, &fpp);
	parse_lvds_manufacturer_table_init(pScrn, bios, &fpp);
	/* I've never seen a valid LVDS_INIT script, so we'll do a test for it here */
	call_lvds_script(pScrn, 0, 0, LVDS_INIT, 0);
}

static unsigned int findstr(bios_t *bios, const unsigned char *str, int len)
{
	int i;

	for (i = 2; i <= (bios->length - len); i++)
		if (strncmp((char *)&bios->data[i], (char *)str, len) == 0)
			return i;

	return 0;
}

static Bool parse_dcb_entry(ScrnInfoPtr pScrn, uint8_t dcb_version, uint32_t conn, uint32_t conf, struct dcb_entry *entry)
{
	NVPtr pNv = NVPTR(pScrn);

	memset(entry, 0, sizeof (struct dcb_entry));

	/* safe defaults for a crt */
	entry->type = 0;
	entry->i2c_index = 0;
	entry->heads = 1;
	entry->bus = 0;
	entry->location = 0;
	entry->or = 1;
	entry->duallink_possible = FALSE;

	if (dcb_version >= 0x20) {
		entry->type = conn & 0xf;
		entry->i2c_index = (conn >> 4) & 0xf;
		entry->heads = (conn >> 8) & 0xf;
		entry->bus = (conn >> 16) & 0xf;
		entry->location = (conn >> 20) & 0xf;
		entry->or = (conn >> 24) & 0xf;
		/* Normal entries consist of a single bit. */
		if ((1 << (ffs(entry->or) - 1)) != entry->or)
			entry->duallink_possible = TRUE;

		switch (entry->type) {
		case OUTPUT_LVDS:
			if (conf & 0xfffffffa)
				ErrorF("Unknown LVDS configuration bits, please report\n");
			if (conf & 0x1)
				entry->lvdsconf.use_straps_for_mode = TRUE;
			if (conf & 0x4)
				entry->lvdsconf.use_power_scripts = TRUE;
			break;
		}
	} else if (dcb_version >= 0x14 ) {
		if (conn != 0xf0003f00 && conn != 0xf2045f14 && conn != 0xf4204011) {
			ErrorF("Unknown DCB 1.4 / 1.5 entry, please report\n");
			/* cause output setting to fail, so message is seen */
			pNv->dcb_table.entries = 0;
			return FALSE;
		}
		/* most of the below is a "best guess" atm */
		entry->type = conn & 0xf;
		if (entry->type == 4)
			entry->type = OUTPUT_LVDS;
		entry->i2c_index = (conn >> 14) & 0xf;
		/* raw heads field is in range 0-1, so move to 1-2 */
		entry->heads = ((conn >> 18) & 0x7) << 1;
		entry->location = (conn >> 21) & 0xf;
		entry->bus = (conn >> 25) & 0x7;
		/* set or to be same as heads -- hopefully safe enough */
		entry->or = entry->heads;
	} else if (dcb_version >= 0x12) {
		/* use the defaults for a crt
		 * v1.2 tables often have other entries though - need a trace
		 */
		entry->type = conn & 0xf;	// this is valid, but will probably confuse the randr stuff
		entry->type = 0;
	} else { /* pre DCB / v1.1 - use the safe defaults for a crt */
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "No information in BIOS output table; assuming a CRT output exists\n");
		entry->i2c_index = pNv->VBIOS.legacy_i2c_indices.crt;
	}

	pNv->dcb_table.entries++;

	return TRUE;
}

static void
read_dcb_i2c_table(ScrnInfoPtr pScrn, bios_t *bios, uint8_t dcb_version, uint16_t i2ctabptr)
{
	NVPtr pNv = NVPTR(pScrn);
	uint8_t *i2ctable;
	uint8_t headerlen = 0;
	int i2c_entries;
	int recordoffset = 0, rdofs = 1, wrofs = 0;
	int i;

	i2c_entries = MAX_NUM_DCB_ENTRIES;
	memset(pNv->dcb_table.i2c_read, 0, sizeof(pNv->dcb_table.i2c_read));
	memset(pNv->dcb_table.i2c_write, 0, sizeof(pNv->dcb_table.i2c_write));

	i2ctable = &bios->data[i2ctabptr];

	if (dcb_version >= 0x30) {
		if (i2ctable[0] != dcb_version) { /* necessary? */
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "DCB I2C table version mismatch (%02X vs %02X)\n",
				   i2ctable[0], dcb_version);
			return;
		}
		headerlen = i2ctable[1];
		i2c_entries = i2ctable[2];
		if (i2ctable[0] >= 0x40) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "G80 DCB I2C table detected, arrgh\n"); /* they're plain weird */
			return;
		}
	}
	/* it's your own fault if you call this function on a DCB 1.1 BIOS --
	 * the below assumes DCB 1.2
	 */
	if (dcb_version < 0x14) {
		recordoffset = 2;
		rdofs = 0;
		wrofs = 1;
	}

	for (i = 0; i < i2c_entries; i++)
		if (i2ctable[headerlen + 4 * i + 3] != 0xff) {
			pNv->dcb_table.i2c_read[i] = i2ctable[headerlen + recordoffset + rdofs + 4 * i];
			pNv->dcb_table.i2c_write[i] = i2ctable[headerlen + recordoffset + wrofs + 4 * i];
		}
}

static unsigned int parse_dcb_table(ScrnInfoPtr pScrn, bios_t *bios)
{
	NVPtr pNv = NVPTR(pScrn);
	uint16_t dcbptr, i2ctabptr = 0;
	uint8_t *dcbtable;
	uint8_t dcb_version, headerlen = 0x4, entries = MAX_NUM_DCB_ENTRIES;
	Bool configblock = TRUE;
	int recordlength = 8, confofs = 4;
	int i;

	pNv->dcb_table.entries = 0;

	/* get the offset from 0x36 */
	dcbptr = le16_to_cpu(*(uint16_t *)&bios->data[0x36]);

	if (dcbptr == 0x0) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "No Display Configuration Block pointer found\n");
		/* this situation likely means a really old card, pre DCB, so we'll add the safe CRT entry */
		parse_dcb_entry(pScrn, 0, 0, 0, &pNv->dcb_table.entry[0]);
		return 1;
	}

	dcbtable = &bios->data[dcbptr];

	/* get DCB version */
	dcb_version = dcbtable[0];
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Display Configuration Block version %d.%d found\n",
		   dcb_version >> 4, dcb_version & 0xf);

	if (dcb_version >= 0x20) { /* NV17+ */
		uint32_t sig;

		if (dcb_version >= 0x30) { /* NV40+ */
			headerlen = dcbtable[1];
			entries = dcbtable[2];
			i2ctabptr = le16_to_cpu(*(uint16_t *)&dcbtable[4]);
			sig = le32_to_cpu(*(uint32_t *)&dcbtable[6]);

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "DCB header length %02X, with %02X possible entries\n",
				   headerlen, entries);
		} else {
			/* dcb_block_count = *(dcbtable[1]); */
			i2ctabptr = le16_to_cpu(*(uint16_t *)&dcbtable[2]);
			sig = le32_to_cpu(*(uint32_t *)&dcbtable[4]);
			headerlen = 8;
		}

		if (sig != 0x4edcbdcb) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "Bad Display Configuration Block signature (%08X)\n", sig);
			return 0;
		}
	} else if (dcb_version >= 0x14) { /* some NV15/16, and NV11+ */
		char sig[8];

		memset(sig, 0, 8);
		strncpy(sig, (char *)&dcbtable[-7], 7);
		/* dcb_block_count = *(dcbtable[1]); */
		i2ctabptr = le16_to_cpu(*(uint16_t *)&dcbtable[2]);
		recordlength = 10;
		confofs = 6;

		if (strcmp(sig, "DEV_REC")) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "Bad Display Configuration Block signature (%s)\n", sig);
			return 0;
		}
	} else if (dcb_version >= 0x12) { /* some NV6/10, and NV15+ */
		/* dcb_block_count = *(dcbtable[1]); */
		i2ctabptr = le16_to_cpu(*(uint16_t *)&dcbtable[2]);
		configblock = FALSE;
	} else {	/* NV5+, maybe NV4 */
		/* DCB 1.1 seems to be quite unhelpful - we'll just add the safe CRT entry */
		parse_dcb_entry(pScrn, dcb_version, 0, 0, &pNv->dcb_table.entry[0]);
		return 1;
	}

	if (entries >= MAX_NUM_DCB_ENTRIES)
		entries = MAX_NUM_DCB_ENTRIES;

	for (i = 0; i < entries; i++) {
		uint32_t connection, config = 0;

		connection = le32_to_cpu(*(uint32_t *)&dcbtable[headerlen + recordlength * i]);
		if (configblock)
			config = le32_to_cpu(*(uint32_t *)&dcbtable[headerlen + confofs + recordlength * i]);

		/* Should we allow discontinuous DCBs? Certainly DCB I2C tables
		 * can be discontinuous */
		if ((connection & 0x0000000f) == 0x0000000f) /* end of records */
			break;

		ErrorF("Raw DCB entry %d: %08x %08x\n", i, connection, config);
		if (!parse_dcb_entry(pScrn, dcb_version, connection, config, &pNv->dcb_table.entry[i]))
			break;
	}

	read_dcb_i2c_table(pScrn, bios, dcb_version, i2ctabptr);

	/* This is needed for DCB version 2.0 */
	/* Otherwise you end up with multiple outputs that are trying to be activated */
	for ( i = 0; i < pNv->dcb_table.entries; i ++) {
		int j;
		int cur_i2c = pNv->dcb_table.entry[i].i2c_index;
		int cur_type = pNv->dcb_table.entry[i].type;
		for ( j = 0; j < pNv->dcb_table.entries; j ++ ) {
			if ( i == j ) continue;
			if ( pNv->dcb_table.entry[j].type == 100) continue; /* merged entry */
			if (( pNv->dcb_table.entry[j].i2c_index == cur_i2c )  && ( pNv->dcb_table.entry[j].type == cur_type ))  {
				/* We can only merge entries with the same allowed crtc's. */
				/* This has not occured so far and needs some logic (to merge dual link properly). */ 
				/* So this remains TODO for the moment. */

				/* We also merge entries with the same allowed output routes */
				if (pNv->dcb_table.entry[i].or == pNv->dcb_table.entry[j].or) {
					xf86DrvMsg(0, X_INFO, "Merging DCB entries %d and %d!\n", i, j);
					pNv->dcb_table.entry[i].heads |= pNv->dcb_table.entry[j].heads;

					pNv->dcb_table.entry[j].type = 100; /* dummy value */
				}
			}
		}
	}

	/* Remove "disabled" entries (merged) */
	int valid_entries[pNv->dcb_table.entries];
	int cent = 0;
	for ( i = 0; i < pNv->dcb_table.entries; i ++) valid_entries[i] = -1;
	for ( i = 0; i < pNv->dcb_table.entries; i ++)
		if ( pNv->dcb_table.entry[i].type != 100 ) {
			valid_entries[cent] = i;
			cent++;
		}
	for ( i = 0; i < cent; i++) {
		memmove(&pNv->dcb_table.entry[i], &pNv->dcb_table.entry[valid_entries[i]], sizeof(pNv->dcb_table.entry[i]));
		memmove(&pNv->dcb_table.i2c_read[i], &pNv->dcb_table.i2c_read[valid_entries[i]], sizeof(pNv->dcb_table.i2c_read[i]));
		memmove(&pNv->dcb_table.i2c_write[i], &pNv->dcb_table.i2c_write[valid_entries[i]], sizeof(pNv->dcb_table.i2c_write[i]));
	}

	pNv->dcb_table.entries = cent;

	return pNv->dcb_table.entries;
}

Bool NVInitVBIOS(ScrnInfoPtr pScrn)
{
	NVPtr pNv = NVPTR(pScrn);

	memset(&pNv->VBIOS, 0, sizeof(bios_t));
	pNv->VBIOS.data = xalloc(64 * 1024);

	if (!NVShadowVBIOS(pScrn, pNv->VBIOS.data)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "No valid BIOS image found\n");
		xfree(pNv->VBIOS.data);
		return FALSE;
	}

	pNv->VBIOS.length = pNv->VBIOS.data[2] * 512;
	if (pNv->VBIOS.length > NV_PROM_SIZE)
		pNv->VBIOS.length = NV_PROM_SIZE;

	return TRUE;
}

Bool NVRunVBIOSInit(ScrnInfoPtr pScrn)
{
	NVPtr pNv = NVPTR(pScrn);
	const uint8_t bmp_signature[] = { 0xff, 0x7f, 'N', 'V', 0x0 };
	const uint8_t bit_signature[] = { 'B', 'I', 'T' };
	int offset, ret = 0;

	crtc_access(pScrn, ACCESS_UNLOCK);

	if ((offset = findstr(&pNv->VBIOS, bit_signature, sizeof(bit_signature)))) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "BIT BIOS found\n");
		parse_bit_structure(pScrn, &pNv->VBIOS, offset + 4);
	} else if ((offset = findstr(&pNv->VBIOS, bmp_signature, sizeof(bmp_signature)))) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "BMP BIOS found\n");
		parse_bmp_structure(pScrn, &pNv->VBIOS, offset);
	} else {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "No known BIOS signature found\n");
		ret = 1;
	}

	crtc_access(pScrn, ACCESS_LOCK);

	if (ret)
		return FALSE;

	return TRUE;
}

unsigned int NVParseBios(ScrnInfoPtr pScrn)
{
	NVPtr pNv = NVPTR(pScrn);
	uint32_t saved_nv_pextdev_boot_0;

	if (!NVInitVBIOS(pScrn))
		return 0;

	/* these will need remembering across a suspend */
	saved_nv_pextdev_boot_0 = nv32_rd(pScrn, NV_PEXTDEV_BOOT_0);
	saved_nv_pfb_cfg0 = nv32_rd(pScrn, NV_PFB_CFG0);

	pNv->VBIOS.execute = FALSE;

	if (!NVRunVBIOSInit(pScrn))
		return 0;

	nv32_wr(pScrn, NV_PEXTDEV_BOOT_0, saved_nv_pextdev_boot_0);

	if (parse_dcb_table(pScrn, &pNv->VBIOS))
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Found %d entries in DCB\n", pNv->dcb_table.entries);

	return 1;
}
