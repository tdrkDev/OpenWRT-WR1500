// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek SPIC SPI flash controller driver
 *
 * The SPIC (called SHEIPA in the vendor SDK) is the NOR flash controller of
 * the Realtek RTL8197F router SoCs. It is derived from the DesignWare SSI and
 * adds an "auto mode" that maps the flash into memory. This driver only uses
 * the "user mode": command, address and data are pushed into a byte wide FIFO
 * and the controller runs the whole transaction on its own while chip select
 * is asserted. As a transaction cannot be longer than the FIFO, data is split
 * into chunks and the command is repeated for each of them.
 */

#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>

#define SPIC_CTRLR0			0x000
#define   SPIC_CTRLR0_CH		GENMASK(19, 16)
#define   SPIC_CTRLR0_TMOD		GENMASK(9, 8)
#define   SPIC_CTRLR0_TMOD_TX		0
#define   SPIC_CTRLR0_TMOD_RX		3
#define SPIC_CTRLR1			0x004
#define SPIC_SSIENR			0x008
#define SPIC_SER			0x010
#define SPIC_BAUDR			0x014
#define SPIC_SR				0x028
#define   SPIC_SR_TXE			BIT(5)
#define   SPIC_SR_BUSY			BIT(0)
#define SPIC_DR				0x060
#define SPIC_ADDR_LENGTH		0x118
#define SPIC_AUTO_LENGTH		0x11c
#define   SPIC_AUTO_LENGTH_DUMMY	GENMASK(15, 0)

#define SPIC_FIFO_SIZE			64
/* The vendor driver never fills more than half of the FIFO for writes */
#define SPIC_MAX_TX_DATA		(SPIC_FIFO_SIZE / 2)
/* Without an address, the address length tells how many data bytes follow */
#define SPIC_MAX_TX_NOADDR_DATA		3
/* Address length of the vendor driver when no address is sent */
#define SPIC_DEF_ADDR_LENGTH		3

#define SPIC_TIMEOUT_US			(100 * USEC_PER_MSEC)

struct realtek_spic {
	struct device *dev;
	void __iomem *base;
};

static int realtek_spic_wait_idle(struct realtek_spic *spic)
{
	u32 sr;
	int ret;

	ret = readl_poll_timeout(spic->base + SPIC_SR, sr,
				 (sr & SPIC_SR_TXE) || !(sr & SPIC_SR_BUSY),
				 0, SPIC_TIMEOUT_US);
	if (ret)
		return ret;

	return (sr & SPIC_SR_TXE) ? -EIO : 0;
}

static void realtek_spic_push(struct realtek_spic *spic, const u8 *buf,
			      unsigned int len)
{
	u32 val;

	for (; len >= 4; len -= 4, buf += 4) {
		memcpy(&val, buf, 4);
		writel(val, spic->base + SPIC_DR);
	}

	for (; len; len--, buf++)
		writeb(*buf, spic->base + SPIC_DR);
}

static void realtek_spic_pull(struct realtek_spic *spic, u8 *buf,
			      unsigned int len)
{
	u32 val;

	for (; len >= 4; len -= 4, buf += 4) {
		val = readl(spic->base + SPIC_DR);
		memcpy(buf, &val, 4);
	}

	/* As the vendor driver, fetch the remaining bytes as a whole word */
	if (len) {
		val = readl(spic->base + SPIC_DR);
		memcpy(buf, &val, len);
	}
}

static bool realtek_spic_supports_op(struct spi_mem *mem,
				     const struct spi_mem_op *op)
{
	if (!spi_mem_default_supports_op(mem, op))
		return false;

	/* Single I/O only, dummy cycles are not supported yet */
	if (op->cmd.nbytes != 1 || op->cmd.buswidth != 1)
		return false;
	if (op->addr.nbytes && (op->addr.buswidth != 1 || op->addr.nbytes > 4))
		return false;
	if (op->dummy.nbytes)
		return false;
	if (op->data.nbytes && op->data.buswidth != 1)
		return false;

	if (op->data.dir == SPI_MEM_DATA_OUT && !op->addr.nbytes &&
	    op->data.nbytes > SPIC_MAX_TX_NOADDR_DATA)
		return false;

	return true;
}

static int realtek_spic_adjust_op_size(struct spi_mem *mem,
				       struct spi_mem_op *op)
{
	if (op->data.dir == SPI_MEM_DATA_IN)
		op->data.nbytes = min_t(unsigned int, op->data.nbytes, SPIC_FIFO_SIZE);
	else if (op->data.dir == SPI_MEM_DATA_OUT)
		op->data.nbytes = min_t(unsigned int, op->data.nbytes, SPIC_MAX_TX_DATA);

	return 0;
}

static int realtek_spic_exec_op(struct spi_mem *mem,
				const struct spi_mem_op *op)
{
	struct realtek_spic *spic = spi_controller_get_devdata(mem->spi->controller);
	bool rx = op->data.dir == SPI_MEM_DATA_IN;
	u8 addr[4];
	u32 addr_len, ctrlr0;
	unsigned int i;
	int ret;

	if (op->addr.nbytes)
		addr_len = op->addr.nbytes & 0x3; /* 0 means 4 bytes */
	else if (op->data.dir == SPI_MEM_DATA_OUT && op->data.nbytes)
		addr_len = op->data.nbytes;
	else
		addr_len = SPIC_DEF_ADDR_LENGTH;

	writel(0, spic->base + SPIC_SSIENR);

	ctrlr0 = readl(spic->base + SPIC_CTRLR0);
	ctrlr0 &= ~(SPIC_CTRLR0_TMOD | SPIC_CTRLR0_CH);
	ctrlr0 |= FIELD_PREP(SPIC_CTRLR0_TMOD,
			     rx ? SPIC_CTRLR0_TMOD_RX : SPIC_CTRLR0_TMOD_TX);
	writel(ctrlr0, spic->base + SPIC_CTRLR0);

	writel(addr_len, spic->base + SPIC_ADDR_LENGTH);
	writel(readl(spic->base + SPIC_AUTO_LENGTH) & ~SPIC_AUTO_LENGTH_DUMMY,
	       spic->base + SPIC_AUTO_LENGTH);
	if (rx)
		writel(op->data.nbytes, spic->base + SPIC_CTRLR1);

	writeb(op->cmd.opcode, spic->base + SPIC_DR);

	for (i = 0; i < op->addr.nbytes; i++)
		addr[i] = op->addr.val >> (8 * (op->addr.nbytes - i - 1));
	realtek_spic_push(spic, addr, op->addr.nbytes);

	if (op->data.dir == SPI_MEM_DATA_OUT)
		realtek_spic_push(spic, op->data.buf.out, op->data.nbytes);

	writel(1, spic->base + SPIC_SSIENR);

	ret = realtek_spic_wait_idle(spic);
	if (ret)
		dev_err(spic->dev, "command 0x%02x failed: %d\n",
			op->cmd.opcode, ret);
	else if (rx)
		realtek_spic_pull(spic, op->data.buf.in, op->data.nbytes);

	writel(0, spic->base + SPIC_SSIENR);

	return ret;
}

static const struct spi_controller_mem_ops realtek_spic_mem_ops = {
	.supports_op = realtek_spic_supports_op,
	.adjust_op_size = realtek_spic_adjust_op_size,
	.exec_op = realtek_spic_exec_op,
};

static int realtek_spic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *ctlr;
	struct realtek_spic *spic;

	ctlr = devm_spi_alloc_host(dev, sizeof(*spic));
	if (!ctlr)
		return -ENOMEM;

	spic = spi_controller_get_devdata(ctlr);
	spic->dev = dev;

	spic->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spic->base))
		return PTR_ERR(spic->base);

	/* Keep the clock divider set up by the bootloader */
	dev_dbg(dev, "clock divider %u\n", readl(spic->base + SPIC_BAUDR));

	writel(0, spic->base + SPIC_SSIENR);
	writel(BIT(0), spic->base + SPIC_SER);

	ctlr->dev.of_node = dev->of_node;
	ctlr->bus_num = -1;
	ctlr->num_chipselect = 1;
	ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
	ctlr->mem_ops = &realtek_spic_mem_ops;

	return devm_spi_register_controller(dev, ctlr);
}

static const struct of_device_id realtek_spic_of_match[] = {
	{ .compatible = "realtek,rtl8197f-spic" },
	{}
};
MODULE_DEVICE_TABLE(of, realtek_spic_of_match);

static struct platform_driver realtek_spic_driver = {
	.driver = {
		.name = "realtek-spic",
		.of_match_table = realtek_spic_of_match,
	},
	.probe = realtek_spic_probe,
};
module_platform_driver(realtek_spic_driver);

MODULE_DESCRIPTION("Realtek SPIC SPI flash controller driver");
MODULE_LICENSE("GPL");
