// SPDX-License-Identifier: GPL-2.0+
/*
 * Qualcomm QUPv3 GENI SPI master — U-Boot port.
 *
 * Polled FIFO mode only. No DMA, no GSI, no interrupts, no slave mode.
 * Sized for the Citadel (Titan M) SPI use case: short transfers (≤2 KB),
 * 1.2 MHz clock, CS held across multiple back-to-back transfers via the
 * SPI uclass SPI_XFER_BEGIN/END flags.
 *
 * Based on Linux drivers/spi/spi-geni-qcom.c and the existing U-Boot
 * geni_i2c / serial_msm_geni drivers for QUPv3 register conventions.
 *
 * Copyright (c) 2026 Petr Hodina <go2null@protonmail.com>
 */

#include <log.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <dm/read.h>
#include <asm/io.h>
#include <clk.h>
#include <spi.h>
#include <time.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <soc/qcom/geni-se.h>
#include <soc/qcom/qup-fw-load.h>

/* SPI-specific GENI registers. */
#define SE_SPI_CPHA			0x224
#define  CPHA				BIT(0)
#define SE_SPI_LOOPBACK			0x22c
#define  LOOPBACK_ENABLE		0x1
#define SE_SPI_CPOL			0x230
#define  CPOL				BIT(2)
#define SE_SPI_DEMUX_OUTPUT_INV		0x24c
#define SE_SPI_DEMUX_SEL		0x250
#define SE_SPI_TRANS_CFG		0x25c
#define  CS_TOGGLE			BIT(1)
#define SE_SPI_WORD_LEN			0x268
#define  WORD_LEN_MSK			GENMASK(9, 0)
#define  MIN_WORD_LEN			4
#define SE_SPI_TX_TRANS_LEN		0x26c
#define SE_SPI_RX_TRANS_LEN		0x270
#define  TRANS_LEN_MSK			GENMASK(23, 0)

/* M_CMD opcodes for SPI. */
#define SPI_TX_ONLY			1
#define SPI_RX_ONLY			2
#define SPI_TX_RX			7
#define SPI_CS_ASSERT			8
#define SPI_CS_DEASSERT			9

/* M_CMD params. */
#define FRAGMENTATION			BIT(2)

#define PACKING_BYTES_PW		4

#define SPI_TIMEOUT_MS			100
#define SPI_DEFAULT_BPW			8
#define SPI_DEFAULT_SCLK_HZ		100000000

struct geni_spi_priv {
	fdt_addr_t wrapper;
	phys_addr_t base;
	struct clk core;
	struct clk se;
	u32 tx_fifo_depth;
	u32 fifo_width_bits;
	u32 tx_wm;
	unsigned long cur_speed_hz;
	unsigned long sclk_hz;
	u8 cur_mode;
	unsigned int cur_bpw;
	unsigned int oversampling;
	bool cs_asserted;
};

static u32 geni_spi_get_tx_fifo_depth(struct geni_spi_priv *priv)
{
	u32 hw_version = readl(priv->wrapper + QUP_HW_VER_REG);
	u32 hw_major = GENI_SE_VERSION_MAJOR(hw_version);
	u32 hw_minor = GENI_SE_VERSION_MINOR(hw_version);
	u32 mask, val;

	if ((hw_major == 3 && hw_minor >= 10) || hw_major > 3)
		mask = TX_FIFO_DEPTH_MSK_256_BYTES;
	else
		mask = TX_FIFO_DEPTH_MSK;

	val = readl(priv->base + SE_HW_PARAM_0);
	return (val & mask) >> TX_FIFO_DEPTH_SHFT;
}

static u32 geni_spi_get_fifo_width(struct geni_spi_priv *priv)
{
	u32 val = readl(priv->base + SE_HW_PARAM_0);

	return (val & TX_FIFO_WIDTH_MSK) >> TX_FIFO_WIDTH_SHFT;
}

/* Bit positions inside a 16-bit cfg slot — per downstream qcom-geni-se.c
 * (~/upstream/proton_kernel_redbull/drivers/soc/qcom/qcom-geni-se.c). */
#define PACKING_START_SHIFT	5
#define PACKING_DIR_SHIFT	4
#define PACKING_LEN_SHIFT	1
#define PACKING_STOP_BIT	BIT(0)
#define NUM_PACKING_VECTORS	4

static void geni_spi_config_packing(struct geni_spi_priv *priv,
				    unsigned int bpw, unsigned int pack_words,
				    bool msb_to_lsb)
{
	/*
	 * The cfg layout I'd been using was guessed at and very wrong.
	 * The downstream algorithm, distilled, fills four 16-bit slots
	 * — each describing one byte of the FIFO word and where on the
	 * wire it lives:
	 *
	 *   bit  0      STOP   (1 on the last slot used)
	 *   bits 1..3   LEN-1  (length of this slice, minus 1, in bits)
	 *   bit  4      DIR    (1 = msb-to-lsb within the slice)
	 *   bits 5..N   START  (start bit index inside the FIFO word)
	 *
	 * For our usual case (bpw=8, pack_words=4, msb_first) that
	 * yields start indices 7, 15, 23, 31 — i.e. one byte per slot,
	 * each transmitted MSB-first, four bytes packed LSB-byte-first
	 * into one 32-bit FIFO word.
	 */
	u32 cfg[NUM_PACKING_VECTORS] = {0};
	int idx_start = msb_to_lsb ? (int)bpw - 1 : 0;
	int idx = idx_start;
	int idx_delta = msb_to_lsb ? -8 : 8;
	int ceil_bpw = (bpw + 7) & ~7;
	int iter = (ceil_bpw * (int)pack_words) / 8;
	int temp_bpw = bpw;
	int i;
	u32 cfg0, cfg1;

	if (iter <= 0 || iter > NUM_PACKING_VECTORS)
		return;

	for (i = 0; i < iter; i++) {
		int len = (temp_bpw < 8 ? temp_bpw : 8) - 1;

		cfg[i] = (u32)idx << PACKING_START_SHIFT;
		cfg[i] |= (u32)(msb_to_lsb ? 1 : 0) << PACKING_DIR_SHIFT;
		cfg[i] |= (u32)len << PACKING_LEN_SHIFT;

		if (temp_bpw <= 8) {
			idx = ((i + 1) * 8) + idx_start;
			temp_bpw = bpw;
		} else {
			idx = idx + idx_delta;
			temp_bpw = temp_bpw - 8;
		}
	}
	cfg[iter - 1] |= PACKING_STOP_BIT;

	cfg0 = cfg[0] | (cfg[1] << 16);
	cfg1 = cfg[2] | (cfg[3] << 16);

	writel(cfg0, priv->base + SE_GENI_TX_PACKING_CFG0);
	writel(cfg1, priv->base + SE_GENI_TX_PACKING_CFG1);
	writel(cfg0, priv->base + SE_GENI_RX_PACKING_CFG0);
	writel(cfg1, priv->base + SE_GENI_RX_PACKING_CFG1);
	writel(PACKING_BYTES_PW, priv->base + SE_GENI_BYTE_GRAN);
}

static int geni_spi_set_clock(struct geni_spi_priv *priv, unsigned long hz)
{
	u32 div;

	if (hz == priv->cur_speed_hz)
		return 0;

	if (!hz)
		return -EINVAL;

	div = DIV_ROUND_UP(priv->sclk_hz, priv->oversampling * hz);
	if (!div || div > (CLK_DIV_MSK >> CLK_DIV_SHFT))
		return -EINVAL;

	writel(0, priv->base + SE_GENI_CLK_SEL);
	writel((div << CLK_DIV_SHFT) | SER_CLK_EN,
	       priv->base + GENI_SER_M_CLK_CFG);

	priv->cur_speed_hz = hz;
	return 0;
}

static void geni_spi_apply_mode(struct geni_spi_priv *priv, uint mode)
{
	u32 loopback = (mode & SPI_LOOP) ? LOOPBACK_ENABLE : 0;
	u32 cpol = (mode & SPI_CPOL) ? CPOL : 0;
	u32 cpha = (mode & SPI_CPHA) ? CPHA : 0;
	u32 trans_cfg;

	writel(loopback, priv->base + SE_SPI_LOOPBACK);
	writel(cpol, priv->base + SE_SPI_CPOL);
	writel(cpha, priv->base + SE_SPI_CPHA);
	writel(0, priv->base + SE_SPI_DEMUX_OUTPUT_INV);
	writel(0, priv->base + SE_SPI_DEMUX_SEL);

	/* We drive CS via explicit M_CMD opcodes, not toggle-on-transfer. */
	trans_cfg = readl(priv->base + SE_SPI_TRANS_CFG);
	trans_cfg &= ~CS_TOGGLE;
	writel(trans_cfg, priv->base + SE_SPI_TRANS_CFG);

	priv->cur_mode = mode;
}

static void geni_spi_apply_word_len(struct geni_spi_priv *priv,
				    unsigned int bpw)
{
	u32 word_len = (bpw - MIN_WORD_LEN) & WORD_LEN_MSK;

	geni_spi_config_packing(priv, bpw, priv->fifo_width_bits / bpw, true);
	writel(word_len, priv->base + SE_SPI_WORD_LEN);
	priv->cur_bpw = bpw;
}

/*
 * Real error bits — does NOT include M_IO_DATA_ASSERT/DEASSERT (bits 22..23)
 * which fire on every normal CS transition, nor M_RX_IRQ (bit 7) which is a
 * harmless status. Matches the error mask the Linux GENI SPI ISR warns on.
 */
#define M_SPI_ERR_MASK	(M_CMD_OVERRUN_EN | M_ILLEGAL_CMD_EN | \
			 M_CMD_FAILURE_EN | \
			 M_RX_FIFO_RD_ERR_EN | M_RX_FIFO_WR_ERR_EN | \
			 M_TX_FIFO_RD_ERR_EN | M_TX_FIFO_WR_ERR_EN)

static int geni_spi_wait_cmd_done(struct geni_spi_priv *priv)
{
	ulong start = get_timer(0);
	u32 status;

	while (get_timer(start) < SPI_TIMEOUT_MS) {
		status = readl(priv->base + SE_GENI_M_IRQ_STATUS);
		if (status & M_SPI_ERR_MASK) {
			writel(status, priv->base + SE_GENI_M_IRQ_CLEAR);
			return -EIO;
		}
		if (status & M_CMD_DONE_EN) {
			writel(status, priv->base + SE_GENI_M_IRQ_CLEAR);
			return 0;
		}
	}
	writel(M_CMD_DONE_EN, priv->base + SE_GENI_M_IRQ_CLEAR);
	return -ETIMEDOUT;
}

static int geni_spi_set_cs(struct geni_spi_priv *priv, bool assert)
{
	u32 m_cmd;
	int ret;

	if (assert == priv->cs_asserted)
		return 0;

	m_cmd = (assert ? SPI_CS_ASSERT : SPI_CS_DEASSERT) << M_OPCODE_SHFT;
	writel(0xFFFFFFFF, priv->base + SE_GENI_M_IRQ_CLEAR);
	writel(m_cmd, priv->base + SE_GENI_M_CMD0);

	ret = geni_spi_wait_cmd_done(priv);
	if (ret)
		return ret;

	priv->cs_asserted = assert;
	return 0;
}

static int geni_spi_fifo_tx(struct geni_spi_priv *priv, const void *txbuf,
			    unsigned int len)
{
	const u8 *p = txbuf;
	ulong start = get_timer(0);
	unsigned int sent = 0;

	while (sent < len && get_timer(start) < SPI_TIMEOUT_MS) {
		u32 status = readl(priv->base + SE_GENI_TX_FIFO_STATUS);
		u32 used = status & TX_FIFO_WC;
		u32 free_words = priv->tx_fifo_depth - used;

		while (free_words > 0 && sent < len) {
			u32 word = 0;
			unsigned int n = min((unsigned int)4, len - sent);

			for (unsigned int i = 0; i < n; i++)
				word |= (u32)p[sent + i] << (i * 8);
			writel(word, priv->base + SE_GENI_TX_FIFOn);
			sent += n;
			free_words--;
		}
	}
	return sent == len ? 0 : -ETIMEDOUT;
}

static int geni_spi_fifo_rx(struct geni_spi_priv *priv, void *rxbuf,
			    unsigned int len)
{
	u8 *p = rxbuf;
	ulong start = get_timer(0);
	unsigned int got = 0;

	while (got < len && get_timer(start) < SPI_TIMEOUT_MS) {
		u32 status = readl(priv->base + SE_GENI_RX_FIFO_STATUS);
		u32 avail = status & RX_FIFO_WC_MSK;

		while (avail > 0 && got < len) {
			u32 word = readl(priv->base + SE_GENI_RX_FIFOn);
			unsigned int n = min((unsigned int)4, len - got);

			for (unsigned int i = 0; i < n; i++)
				p[got + i] = (word >> (i * 8)) & 0xff;
			got += n;
			avail--;
		}
	}
	return got == len ? 0 : -ETIMEDOUT;
}

static int geni_spi_xfer(struct udevice *dev, unsigned int bitlen,
			 const void *dout, void *din, unsigned long flags)
{
	struct udevice *bus = dev_get_parent(dev);
	struct geni_spi_priv *priv = dev_get_priv(bus);
	struct dm_spi_slave_plat *slv = dev_get_parent_plat(dev);
	unsigned int len_bytes = bitlen / 8;
	u32 m_cmd = 0;
	int ret = 0;

	if (bitlen % 8) {
		dev_err(bus, "geni-spi: only byte-aligned transfers\n");
		return -EINVAL;
	}

	if (slv->max_hz && slv->max_hz != priv->cur_speed_hz) {
		ret = geni_spi_set_clock(priv, slv->max_hz);
		if (ret)
			return ret;
	}

	if (flags & SPI_XFER_BEGIN) {
		ret = geni_spi_set_cs(priv, true);
		if (ret)
			return ret;
	}

	if (!len_bytes)
		goto out_cs;

	writel(0xFFFFFFFF, priv->base + SE_GENI_M_IRQ_CLEAR);

	/*
	 * Opcode selection per downstream Qualcomm spi-geni-qcom.c:
	 *   SPI_FULL_DUPLEX = 3 (= SPI_TX_ONLY | SPI_RX_ONLY)
	 *   SPI_TX_ONLY     = 1
	 *   SPI_RX_ONLY     = 2
	 *
	 * Confirmed against ~/upstream/proton_kernel_redbull's
	 * drivers/spi/spi-geni-qcom.c (line 74: #define SPI_FULL_DUPLEX 3),
	 * which is what runs against the same GENI hardware on bramble.
	 */
	if (dout && din)
		m_cmd = SPI_TX_ONLY | SPI_RX_ONLY;	/* = SPI_FULL_DUPLEX */
	else if (dout)
		m_cmd = SPI_TX_ONLY;
	else if (din)
		m_cmd = SPI_RX_ONLY;

	if (dout)
		writel(len_bytes, priv->base + SE_SPI_TX_TRANS_LEN);
	if (din)
		writel(len_bytes, priv->base + SE_SPI_RX_TRANS_LEN);

	/*
	 * Pre-fill the TX FIFO BEFORE arming M_CMD0 and kicking the
	 * watermark. The downstream flow relies on the WM IRQ firing
	 * and the ISR filling the FIFO; we're polled, so we have to
	 * fill in advance — otherwise the controller arms with an
	 * empty FIFO and starves (possibly raising M_TX_FIFO_RD_ERR).
	 */
	if (dout) {
		ret = geni_spi_fifo_tx(priv, dout, len_bytes);
		if (ret)
			goto out_cs;
	}

	m_cmd = (m_cmd << M_OPCODE_SHFT) | FRAGMENTATION;
	writel(m_cmd, priv->base + SE_GENI_M_CMD0);

	if (dout) {
		/*
		 * Kick the watermark — downstream always writes tx_wm here,
		 * regardless of whether more data is coming. The act of
		 * writing TX_WATERMARK_REG is what tells the controller to
		 * start consuming what's already in the FIFO.
		 */
		writel(priv->tx_wm,
		       priv->base + SE_GENI_TX_WATERMARK_REG);
	}

	if (din) {
		ret = geni_spi_fifo_rx(priv, din, len_bytes);
		if (ret)
			goto out_cs;
	}

	ret = geni_spi_wait_cmd_done(priv);

out_cs:
	if (flags & SPI_XFER_END) {
		int cs_ret = geni_spi_set_cs(priv, false);

		if (!ret)
			ret = cs_ret;
	}
	return ret;
}

static int geni_spi_set_speed(struct udevice *bus, uint hz)
{
	struct geni_spi_priv *priv = dev_get_priv(bus);

	return geni_spi_set_clock(priv, hz);
}

static int geni_spi_set_mode(struct udevice *bus, uint mode)
{
	struct geni_spi_priv *priv = dev_get_priv(bus);

	if (mode == priv->cur_mode)
		return 0;
	geni_spi_apply_mode(priv, mode);
	geni_spi_apply_word_len(priv, SPI_DEFAULT_BPW);
	return 0;
}

static int geni_spi_claim_bus(struct udevice *dev)
{
	struct udevice *bus = dev_get_parent(dev);
	struct geni_spi_priv *priv = dev_get_priv(bus);

	if (!priv->cur_bpw)
		geni_spi_apply_word_len(priv, SPI_DEFAULT_BPW);
	return 0;
}

static int geni_spi_release_bus(struct udevice *dev)
{
	struct udevice *bus = dev_get_parent(dev);
	struct geni_spi_priv *priv = dev_get_priv(bus);

	return geni_spi_set_cs(priv, false);
}

static int geni_spi_probe(struct udevice *dev)
{
	ofnode parent = ofnode_get_parent(dev_ofnode(dev));
	struct geni_spi_priv *priv = dev_get_priv(dev);
	u32 proto, ver, major, minor;
	int ret;

	priv->wrapper = ofnode_get_addr(parent);
	if (priv->wrapper == FDT_ADDR_T_NONE)
		return -EINVAL;

	priv->base = (phys_addr_t)dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	ret = clk_get_by_name(dev, "se", &priv->se);
	if (ret) {
		dev_err(dev, "missing 'se' clock: %d\n", ret);
		return ret;
	}
	ret = clk_enable(&priv->se);
	if (ret)
		return ret;

	/*
	 * Read the protocol the GENI was loaded with. If the FW slot is empty
	 * (GENI_SE_INVALID_PROTO) try to load the SPI firmware via the existing
	 * qup-fw-load helper used by I2C/serial.
	 */
	proto = (readl(priv->base + GENI_FW_REVISION_RO) & FW_REV_PROTOCOL_MSK)
		>> FW_REV_PROTOCOL_SHFT;
	if (proto == GENI_SE_INVALID_PROTO) {
		qcom_geni_load_firmware(priv->base, dev);
		proto = (readl(priv->base + GENI_FW_REVISION_RO)
			 & FW_REV_PROTOCOL_MSK) >> FW_REV_PROTOCOL_SHFT;
	}
	if (proto != GENI_SE_SPI) {
		dev_err(dev, "GENI is not in SPI proto (got %u)\n", proto);
		return -ENXIO;
	}

	priv->tx_fifo_depth = geni_spi_get_tx_fifo_depth(priv);
	priv->fifo_width_bits = geni_spi_get_fifo_width(priv);
	if (!priv->tx_fifo_depth || !priv->fifo_width_bits) {
		dev_err(dev, "invalid FIFO geometry\n");
		return -ENXIO;
	}
	priv->tx_wm = 1;

	ver = readl(priv->wrapper + QUP_HW_VER_REG);
	major = GENI_SE_VERSION_MAJOR(ver);
	minor = GENI_SE_VERSION_MINOR(ver);
	priv->oversampling = (major == 1 && minor == 0) ? 2 : 1;

	/*
	 * Drive the source RCG via the clock framework so the per-SoC GCC
	 * driver (clock-sdm845 / -sdm670 / -sm7250) actually configures the
	 * SE's source clock. clk_set_rate returns the rate the framework
	 * landed on; we cache it for the SE-internal divider math below.
	 *
	 * 19.2 MHz CXO is the natural target for Citadel (1.2 MHz × 16) and
	 * is the rate the per-SoC drivers map to CFG_CLK_SRC_CXO in
	 * ftbl_gcc_qupv3_wrap0_s0_clk_src.
	 */
	ret = clk_set_rate(&priv->se, 19200000);
	priv->sclk_hz = (ret > 0) ? (unsigned long)ret : SPI_DEFAULT_SCLK_HZ;
	if (ret <= 0)
		dev_warn(dev, "clk_set_rate failed (%d); guessing sclk=%lu Hz\n",
			 ret, priv->sclk_hz);

	/*
	 * SE common init — port of geni_se_init() in Linux qcom-geni-se.c.
	 * The crucial bits are GENI_CGC_CTRL (gate internal clocks),
	 * SE_DMA_GENERAL_CFG (clock-gate the DMA shells even though we
	 * don't use DMA), and GENI_OUTPUT_CTRL — without that last write
	 * the SE's IO output drivers are disabled and CS/CLK/MOSI never
	 * leave the controller. The fact that "citadel raw" returned
	 * sixteen bytes of 0x00 was this missing register.
	 */
	{
		u32 v;

		v = readl(priv->base + GENI_CGC_CTRL);
		v |= DEFAULT_CGC_EN;
		writel(v, priv->base + GENI_CGC_CTRL);

		v = readl(priv->base + SE_DMA_GENERAL_CFG);
		v |= AHB_SEC_SLV_CLK_CGC_ON | DMA_AHB_SLV_CFG_ON |
		     DMA_TX_CLK_CGC_ON | DMA_RX_CLK_CGC_ON;
		writel(v, priv->base + SE_DMA_GENERAL_CFG);

		writel(DEFAULT_IO_OUTPUT_CTRL_MSK,
		       priv->base + GENI_OUTPUT_CTRL);
		writel(FORCE_DEFAULT, priv->base + GENI_FORCE_DEFAULT_REG);

		/* IRQ masks — we poll, but the M_CMD_DONE bit lives in
		 * the same status register and we depend on the common
		 * mask being set for status to update.
		 */
		writel(M_COMMON_GENI_M_IRQ_EN | M_CMD_DONE_EN,
		       priv->base + SE_GENI_M_IRQ_EN);
		writel(S_COMMON_GENI_S_IRQ_EN,
		       priv->base + SE_GENI_S_IRQ_EN);

		/* RX watermarks (FIFO depth − 3 / − 2 per the Linux driver). */
		writel(priv->tx_fifo_depth - 3,
		       priv->base + SE_GENI_RX_WATERMARK_REG);
		writel(priv->tx_fifo_depth - 2,
		       priv->base + SE_GENI_RX_RFR_WATERMARK_REG);
	}

	/* FIFO mode + manual CS. */
	writel(0, priv->base + SE_GENI_DMA_MODE_EN);
	writel(0xFFFFFFFF, priv->base + SE_GENI_M_IRQ_CLEAR);
	writel(0xFFFFFFFF, priv->base + SE_GENI_S_IRQ_CLEAR);

	geni_spi_apply_mode(priv, 0);
	geni_spi_apply_word_len(priv, SPI_DEFAULT_BPW);

	return 0;
}

static const struct dm_spi_ops geni_spi_ops = {
	.claim_bus	= geni_spi_claim_bus,
	.release_bus	= geni_spi_release_bus,
	.xfer		= geni_spi_xfer,
	.set_speed	= geni_spi_set_speed,
	.set_mode	= geni_spi_set_mode,
};

void geni_spi_dump_state(struct udevice *bus, const char *tag)
{
	struct geni_spi_priv *priv = dev_get_priv(bus);

	printf("== geni-spi %s @ %lx ==\n", tag, (unsigned long)priv->base);
	printf("  GENI_FW_REVISION_RO  = %08x\n", readl(priv->base + GENI_FW_REVISION_RO));
	printf("  GENI_OUTPUT_CTRL     = %08x  (need bit0..bit6 set)\n",
	       readl(priv->base + GENI_OUTPUT_CTRL));
	printf("  GENI_CGC_CTRL        = %08x\n", readl(priv->base + GENI_CGC_CTRL));
	printf("  SE_GENI_DMA_MODE_EN  = %08x  (FIFO=0)\n",
	       readl(priv->base + SE_GENI_DMA_MODE_EN));
	printf("  GENI_SER_M_CLK_CFG   = %08x  (bit0 SER_CLK_EN, bits4..15 div)\n",
	       readl(priv->base + GENI_SER_M_CLK_CFG));
	printf("  SE_GENI_M_CMD0       = %08x\n", readl(priv->base + SE_GENI_M_CMD0));
	printf("  SE_GENI_M_IRQ_STATUS = %08x\n",
	       readl(priv->base + SE_GENI_M_IRQ_STATUS));
	printf("  SE_GENI_M_IRQ_EN     = %08x\n",
	       readl(priv->base + SE_GENI_M_IRQ_EN));
	printf("  SE_GENI_S_IRQ_STATUS = %08x\n",
	       readl(priv->base + SE_GENI_S_IRQ_STATUS));
	printf("  SE_GENI_TX_FIFO_STATUS = %08x\n",
	       readl(priv->base + SE_GENI_TX_FIFO_STATUS));
	printf("  SE_GENI_RX_FIFO_STATUS = %08x\n",
	       readl(priv->base + SE_GENI_RX_FIFO_STATUS));
	printf("  SE_GENI_STATUS       = %08x  (bit0 M_ACTIVE)\n",
	       readl(priv->base + SE_GENI_STATUS));
	printf("  SE_GENI_IOS          = %08x\n", readl(priv->base + SE_GENI_IOS));
	printf("  SE_HW_PARAM_0        = %08x\n", readl(priv->base + SE_HW_PARAM_0));
}

static const struct udevice_id geni_spi_ids[] = {
	{ .compatible = "qcom,geni-spi" },
	{ .compatible = "qcom,spi-geni" },
	{}
};

U_BOOT_DRIVER(spi_geni_qcom) = {
	.name		= "spi-geni-qcom",
	.id		= UCLASS_SPI,
	.of_match	= geni_spi_ids,
	.ops		= &geni_spi_ops,
	.priv_auto	= sizeof(struct geni_spi_priv),
	.probe		= geni_spi_probe,
};
