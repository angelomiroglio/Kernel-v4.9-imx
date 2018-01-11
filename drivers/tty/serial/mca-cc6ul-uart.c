/* mca-cc6ul-uart.c - UART driver for MCA devices.
 * Based on sc16is7xx.c, by Jon Ringle <jringle@gridpoint.com>
 *
 * Copyright (C) 2017  Digi International Inc
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library.
 */

#include <linux/device.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/serial_core.h>
#include <linux/tty_flip.h>
#include <linux/mfd/mca-common/registers.h>
#include <linux/mfd/mca-common/core.h>
#include <linux/mfd/mca-cc6ul/core.h>
#include <linux/mfd/mca-cc6ul/registers.h>
#include <linux/delay.h>

#define MCA_UART_DEV_NAME		"ttyMCA"
#define MCA_UART_DEFAULT_BRATE		9600
#define MCA_UART_DEFAULT_BAUD_REG	MCA_REG_UART_BAUD_9600
#define MCA_UART_MIN_BAUD		1200
#define MCA_UART_MAX_BAUD		230400
#define MCA_UART_RX_FIFO_SIZE		128
#define MCA_UART_TX_FIFO_SIZE		128
#define MCA_UART_CLK			24000000
#define MCA_UART_MIN_FW_VERSION		MCA_MAKE_FW_VER(1, 1)

#define MCA_UART_HAS_RTS		BIT(0)
#define MCA_UART_HAS_CTS		BIT(1)

#define to_mca_uart(p, e)		(container_of((p), struct mca_uart, e))

enum {
	WORK_STOP_RX = BIT(0),
	WORK_STOP_TX = BIT(1),
	WORK_SET_RTS = BIT(2),
	WORK_CLEAR_RTS = BIT(3),
};

struct mca_uart {
	struct mca_cc6ul *mca;
	struct device *dev;
	struct uart_driver uart;
	struct uart_port port;
	struct mutex mutex;
	unsigned int pending_work;
	struct work_struct tx_work;
	struct work_struct delayed_work;
	unsigned int has_rtscts;
	int rts_pin;
	int cts_pin;
	bool enable_power_on;
};

static void mca_uart_stop_tx(struct uart_port *port)
{
	struct mca_uart *mca_uart = dev_get_drvdata(port->dev);

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	/* Work is queued since we can't access I2C regmap in atomic context */
	mca_uart->pending_work |= WORK_STOP_TX;
	if (!work_pending(&mca_uart->delayed_work))
		schedule_work(&mca_uart->delayed_work);
}

static void mca_uart_stop_rx(struct uart_port *port)
{
	struct mca_uart *mca_uart = dev_get_drvdata(port->dev);

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	/* Work is queued since we can't access I2C regmap in atomic context */
	mca_uart->pending_work |= WORK_STOP_RX;
	if (!work_pending(&mca_uart->delayed_work))
		schedule_work(&mca_uart->delayed_work);
}

static void mca_uart_start_tx(struct uart_port *port)
{
	struct mca_uart *mca_uart = to_mca_uart(port, port);

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	if (!work_pending(&mca_uart->tx_work))
		schedule_work(&mca_uart->tx_work);
}

static unsigned int mca_uart_tx_empty(struct uart_port *port)
{
	struct mca_uart *mca_uart = to_mca_uart(port, port);
	struct regmap *regmap = mca_uart->mca->regmap;
	unsigned int txlvl;
	int ret;

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	ret = regmap_read(regmap, MCA_REG_UART_TXLVL, &txlvl);
	if (ret) {
		dev_err(mca_uart->dev, "Failed to read MCA_REG_UART_TXLVL\n");
		/* This is the behavior if not implemented */
		return TIOCSER_TEMT;
	}

	return (txlvl == MCA_UART_TX_FIFO_SIZE) ? TIOCSER_TEMT : 0;
}

static unsigned int mca_uart_get_mctrl(struct uart_port *port)
{
	struct mca_uart *mca_uart = to_mca_uart(port, port);

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);
	/*
	 * DCD and DSR are not wired and CTS/RTS is handled automatically
	 * so just indicate DSR and CAR asserted. Also regmap cannot be called
	 * from atomic context, so reading the status of the lines here is not
	 * possible.
	 */
	return TIOCM_DSR | TIOCM_CAR;
}

static void mca_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct mca_uart *mca_uart = to_mca_uart(port, port);

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);
	/*
	 * Regmap cannot be called from atomic context, so this would require a
	 * work queue to set/clear RTS. However, that line is handled
	 * automatically by the hardware when using flow control, and the
	 * get_mctrl for reading CTS and RTS cannot be implemented for the same
	 * reason. If RTS/CTS are used for something different that hardware
	 * flow control, perhaps they should be declared as GPIOs.
	 */
}

static void mca_uart_break_ctl(struct uart_port *port, int break_state)
{
	struct mca_uart *mca_uart = to_mca_uart(port, port);

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);
	dev_warn(mca_uart->dev, "BREAK condition not supported\n");
}

static void mca_uart_set_termios(struct uart_port *port,
				  struct ktermios *termios,
				  struct ktermios *old)
{
	struct mca_uart *mca_uart = to_mca_uart(port, port);
	struct regmap *regmap = mca_uart->mca->regmap;
	unsigned int cfg1 = 0;
	unsigned int baudrate;
	unsigned int baud_reg_val;
	int ret;

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	/* Mask unsupported termios capabilities */
	if (!mca_uart->has_rtscts)
		termios->c_cflag &= ~CRTSCTS;

	termios->c_iflag &= ~(IXON | IXOFF | IXANY | CMSPAR | CSIZE);

	/* Only 8-bit size supported */
	termios->c_cflag |= CS8;

	if (termios->c_cflag & CSTOPB)
		cfg1 |= MCA_REG_UART_CFG1_TWO_STOPBITS;
	if (termios->c_cflag & PARENB)
		cfg1 |= MCA_REG_UART_CFG1_PARITY_EN;
	if (termios->c_cflag & PARODD)
		cfg1 |= MCA_REG_UART_CFG1_PARITY_ODD;
	if (termios->c_cflag & CRTSCTS) {
		if (mca_uart->has_rtscts & MCA_UART_HAS_CTS) {
			cfg1 |= MCA_REG_UART_CFG1_CTS_EN;
			port->status |= UPSTAT_AUTOCTS;
		}
		if (mca_uart->has_rtscts & MCA_UART_HAS_RTS) {
			cfg1 |= MCA_REG_UART_CFG1_RTS_EN;
			port->status |= UPSTAT_AUTORTS;
		}
	} else {
		port->status &= ~(UPSTAT_AUTOCTS | UPSTAT_AUTORTS);
	}

	ret = regmap_write(regmap, MCA_REG_UART_CFG1, cfg1);
	if (ret) {
		dev_err(mca_uart->dev, "Failed to write MCA_REG_UART_CFG1\n");
		return;
	}

	baudrate = uart_get_baud_rate(port, termios, old, MCA_UART_MIN_BAUD,
				      MCA_UART_MAX_BAUD);
	uart_update_timeout(port, termios->c_cflag, baudrate);

	switch (baudrate) {
	case 1200:
		baud_reg_val = MCA_REG_UART_BAUD_1200;
		break;
	case 2400:
		baud_reg_val = MCA_REG_UART_BAUD_2400;
		break;
	case 4800:
		baud_reg_val = MCA_REG_UART_BAUD_4800;
		break;
	case 9600:
		baud_reg_val = MCA_REG_UART_BAUD_9600;
		break;
	case 19200:
		baud_reg_val = MCA_REG_UART_BAUD_19200;
		break;
	case 38400:
		baud_reg_val = MCA_REG_UART_BAUD_38400;
		break;
	case 57600:
		baud_reg_val = MCA_REG_UART_BAUD_57600;
		break;
	case 115200:
		baud_reg_val = MCA_REG_UART_BAUD_115200;
		break;
	case 230400:
		baud_reg_val = MCA_REG_UART_BAUD_230400;
		break;
	default:
		dev_warn(mca_uart->dev,
			 "Baud rate %d not supported, using default %d\n",
			 baudrate, MCA_UART_DEFAULT_BRATE);
		baud_reg_val = MCA_UART_DEFAULT_BAUD_REG;
		break;
	}

	ret = regmap_write(regmap, MCA_REG_UART_BAUD, baud_reg_val);
	if (ret) {
		dev_err(mca_uart->dev, "Failed to write MCA_REG_UART_BAUD\n");
		return;
	}
}

static int mca_uart_startup(struct uart_port *port)
{
	struct mca_uart *mca_uart = dev_get_drvdata(port->dev);
	struct regmap *regmap = mca_uart->mca->regmap;
	int ret;
	unsigned int cfg_mask;
	unsigned int ier_mask;

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	/* Reset RX and TX FIFOs and enable TX and RX */
	cfg_mask = MCA_REG_UART_CFG0_CTX | MCA_REG_UART_CFG0_CRX |
		   MCA_REG_UART_CFG0_TXEN | MCA_REG_UART_CFG0_RXEN;

	ret = regmap_update_bits(regmap, MCA_REG_UART_CFG0, cfg_mask, cfg_mask);
	if (ret) {
		dev_err(mca_uart->dev, "Failed to read MCA_REG_UART_CFG0\n");
		return ret;
	}

	ier_mask = MCA_REG_UART_IER_THR | MCA_REG_UART_IER_RHR |
		   MCA_REG_UART_IER_RLSE;
	ret = regmap_update_bits(regmap, MCA_REG_UART_IER, ier_mask, ier_mask);
	if (ret)
		dev_err(mca_uart->dev, "Failed to read MCA_REG_UART_IER\n");

	return ret;
}

static void mca_uart_shutdown(struct uart_port *port)
{
	struct mca_uart *mca_uart = dev_get_drvdata(port->dev);
	struct regmap *regmap = mca_uart->mca->regmap;
	int ret;
	unsigned int cfg_mask;

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	/* Reset RX and TX FIFOs and disable TX and RX */
	cfg_mask = MCA_REG_UART_CFG0_CTX | MCA_REG_UART_CFG0_CRX |
		   MCA_REG_UART_CFG0_TXEN | MCA_REG_UART_CFG0_RXEN;

	ret = regmap_update_bits(regmap, MCA_REG_UART_CFG0, cfg_mask,
				 MCA_REG_UART_CFG0_CTX | MCA_REG_UART_CFG0_CRX);
	if (ret)
		dev_err(mca_uart->dev, "Failed to read MCA_REG_UART_CFG0\n");

	/* Disable all IRQs */
	ret = regmap_write(regmap, MCA_REG_UART_IER, 0);
	if (ret)
		dev_err(mca_uart->dev, "Failed to write MCA_REG_UART_IER\n");
	cancel_work_sync(&mca_uart->tx_work);
	cancel_work_sync(&mca_uart->delayed_work);
}

static const char *mca_uart_type(struct uart_port *port)
{
	struct mca_uart *mca_uart = dev_get_drvdata(port->dev);

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	return "MCA UART";
}

static int mca_uart_request_port(struct uart_port *port)
{
	struct mca_uart *mca_uart = dev_get_drvdata(port->dev);

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	/* Do nothing */
	return 0;
}

static void mca_uart_config_port(struct uart_port *port, int flags)
{
	struct mca_uart *mca_uart = dev_get_drvdata(port->dev);

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_LPUART;
}

static int mca_uart_verify_port(struct uart_port *port,
				 struct serial_struct *s)
{
	struct mca_uart *mca_uart = dev_get_drvdata(port->dev);

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);
	if ((s->type != PORT_UNKNOWN) && (s->type != PORT_LPUART))
		return -EINVAL;
	if (s->irq != port->irq)
		return -EINVAL;

	return 0;
}

static void mca_uart_release_port(struct uart_port *port)
{
	struct mca_uart *mca_uart = dev_get_drvdata(port->dev);

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);
	/* Do nothing */
}

static void mca_uart_throttle(struct uart_port *port)
{
	struct mca_uart *mca_uart = dev_get_drvdata(port->dev);
	struct regmap *regmap = mca_uart->mca->regmap;
	int ret;

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);
	mutex_lock(&mca_uart->mutex);
	ret = regmap_update_bits(regmap, MCA_REG_UART_CFG1,
				 MCA_REG_UART_CFG1_THROTTLE,
				 MCA_REG_UART_CFG1_THROTTLE);
	if (ret)
		dev_err(mca_uart->dev, "Failed to write MCA_REG_UART_CFG1\n");
	mutex_unlock(&mca_uart->mutex);
}

static void mca_uart_unthrottle(struct uart_port *port)
{
	struct mca_uart *mca_uart = dev_get_drvdata(port->dev);
	struct regmap *regmap = mca_uart->mca->regmap;
	int ret;

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);
	mutex_lock(&mca_uart->mutex);
	ret = regmap_update_bits(regmap, MCA_REG_UART_CFG1,
				 MCA_REG_UART_CFG1_THROTTLE, 0);
	if (ret)
		dev_err(mca_uart->dev, "Failed to write MCA_REG_UART_CFG1\n");
	mutex_unlock(&mca_uart->mutex);
}

static const struct uart_ops mca_uart_ops = {
	.tx_empty	= mca_uart_tx_empty,
	.set_mctrl	= mca_uart_set_mctrl,
	.get_mctrl	= mca_uart_get_mctrl,
	.stop_tx	= mca_uart_stop_tx,
	.start_tx	= mca_uart_start_tx,
	.stop_rx	= mca_uart_stop_rx,
	.break_ctl	= mca_uart_break_ctl,
	.startup	= mca_uart_startup,
	.shutdown	= mca_uart_shutdown,
	.set_termios	= mca_uart_set_termios,
	.type		= mca_uart_type,
	.request_port	= mca_uart_request_port,
	.release_port	= mca_uart_release_port,
	.config_port	= mca_uart_config_port,
	.verify_port	= mca_uart_verify_port,
	.throttle	= mca_uart_throttle,
	.unthrottle	= mca_uart_unthrottle,
	.pm		= NULL,
};

static void mca_uart_handle_tx(struct uart_port *port)
{
	struct mca_uart *mca_uart = dev_get_drvdata(port->dev);
	struct regmap *regmap = mca_uart->mca->regmap;
	struct circ_buf *xmit = &port->state->xmit;
	unsigned int to_send;
	uint8_t tx_buf[MCA_UART_TX_FIFO_SIZE];

	/*
	 * There is a corner case in which the job is scheduled after the port
	 * has been shut down and port->state->port.tty is NULL. If not checked,
	 * uart_tx_stopped() would crash.
	 */
	if (!port->state->port.tty || uart_circ_empty(xmit) ||
	    uart_tx_stopped(port))
		return;

	/* Get length of data pending in circular buffer */
	to_send = uart_circ_chars_pending(xmit);
	if (likely(to_send)) {
		unsigned int txlen;
		unsigned int i;
		int ret;

		/* Limit to size of TX FIFO */
		ret = regmap_read(regmap, MCA_REG_UART_TXLVL, &txlen);
		if (ret) {
			dev_err(mca_uart->dev,
				"Failed to read MCA_REG_UART_TXLVL\n");
			txlen = 0;
		}

		if (unlikely(!txlen)) {
			dev_dbg(mca_uart->dev, "TX FIFO is full\n");
			if (!work_pending(&mca_uart->tx_work))
				schedule_work(&mca_uart->tx_work);
			return;
		}

		if (unlikely(txlen > sizeof(tx_buf))) {
			dev_err(mca_uart->dev,
				"Invalid MCA_REG_UART_TXLVL value %d\n", txlen);
			if (!work_pending(&mca_uart->tx_work))
				schedule_work(&mca_uart->tx_work);
			return;
		}

		if (to_send > txlen)
			to_send = txlen;

		port->icount.tx += to_send;
		/* Convert to linear buffer */
		for (i = 0; i < to_send; ++i) {
			tx_buf[i] = xmit->buf[xmit->tail];
			xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		}

		ret = regmap_bulk_write(regmap, MCA_REG_UART_THR, tx_buf,
					to_send);
		if (ret)
			dev_err(mca_uart->dev,
				"Failed to write MCA_REG_UART_THR\n");
	}

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);
}

static void mca_uart_handle_rx(struct mca_uart *mca_uart, bool has_errors)
{
	struct uart_port *port = &mca_uart->port;
	struct regmap *regmap = mca_uart->mca->regmap;
	unsigned int flag = TTY_NORMAL;
	unsigned int lsr;
	unsigned int rxlen;
	unsigned int i;
	int ret;
	uint8_t rx_buf[MCA_UART_RX_FIFO_SIZE];
	uint8_t error_buf[MCA_UART_RX_FIFO_SIZE];

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	ret = regmap_read(regmap, MCA_REG_UART_RXLVL, &rxlen);
	if (ret) {
		dev_err(mca_uart->dev, "Failed to read MCA_REG_UART_RXLVL\n");
		return;
	}

	if (unlikely(!rxlen))
		return;

	if (unlikely(has_errors)) {
		ret = regmap_read(regmap, MCA_REG_UART_LSR, &lsr);
		if (ret) {
			dev_err(mca_uart->dev,
				"Failed to read MCA_REG_UART_LSR\n");
			return;
		}
		if (likely(lsr)) {
			ret = regmap_bulk_read(regmap, MCA_REG_UART_RX_ERRORS,
					       error_buf, rxlen);
			if (ret) {
				dev_err(mca_uart->dev,
					"Failed to read MCA_REG_UART_RX_ERRORS\n");
				return;
			}
		} else {
			/* No errors */
			has_errors = false;
		}
	}

	ret = regmap_bulk_read(regmap, MCA_REG_UART_RHR, rx_buf, rxlen);
	if (ret) {
		dev_warn(mca_uart->dev,
			"Failed to read MCA_REG_UART_RHR %d, retrying\n", ret);
		ret = regmap_bulk_read(regmap, MCA_REG_UART_RHR, rx_buf, rxlen);
		if (ret) {
			dev_err(mca_uart->dev,
				"Failed to read MCA_REG_UART_RHR %d\n", ret);
			goto exit;
		}
	}

	port->icount.rx += rxlen;
	for (i = 0; i < rxlen; i++) {
		uint8_t const ch = rx_buf[i];

		if (uart_handle_sysrq_char(port, ch))
			continue;

		if (unlikely(has_errors)) {
			switch (error_buf[i]) {
			case MCA_REG_UART_LSR_FRAMING_ERROR:
				flag = TTY_FRAME;
				port->icount.frame++;
				break;
			case MCA_REG_UART_LSR_PARITY_ERROR:
				flag = TTY_PARITY;
				port->icount.parity++;
				break;
			case MCA_REG_UART_LSR_FIFO_OR_ERROR:
				/* MCA didn't read its UART fast enough */
				flag = TTY_OVERRUN;
				port->icount.overrun++;
				break;
			case MCA_REG_UART_LSR_BREAK:
			case MCA_REG_UART_LSR_HW_OR_ERROR:
				flag = TTY_BREAK;
				port->icount.brk++;
				break;
			case MCA_REG_UART_LSR_NO_ERROR:
			default:
				flag = TTY_NORMAL;
				break;
			}
		}
		ret = tty_insert_flip_char(&port->state->port, ch, flag);
		if (!ret) {
			dev_err(mca_uart->dev,
				"tty_insert_flip_char failed for %x\n", ch);
			port->icount.overrun++;
			break;
		}
	}
exit:
	tty_flip_buffer_push(&port->state->port);
}

static irqreturn_t mca_uart_irq_handler(int irq, void *private)
{
	struct mca_uart *mca_uart = private;
	struct regmap *regmap = mca_uart->mca->regmap;
	unsigned int iir;
	int ret;

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);
	mutex_lock(&mca_uart->mutex);

	ret = regmap_read(regmap, MCA_REG_UART_IIR, &iir);
	if (ret) {
		dev_err(mca_uart->dev, "Failed to read MCA_REG_UART_IIR\n");
		return IRQ_HANDLED;
	}

	if (iir & MCA_REG_UART_IIR_RHR) {
		bool has_errors = iir & MCA_REG_UART_IIR_RLSE;

		mca_uart_handle_rx(mca_uart, has_errors);
	}

	if (iir & MCA_REG_UART_IIR_THR) {
		if (!work_pending(&mca_uart->tx_work))
			schedule_work(&mca_uart->tx_work);
	}

	mutex_unlock(&mca_uart->mutex);
	return IRQ_HANDLED;
}

static void mca_uart_delayed_work_proc(struct work_struct *ws)
{
	struct mca_uart *mca_uart = to_mca_uart(ws, delayed_work);
	struct regmap *regmap = mca_uart->mca->regmap;
	int ret;
	unsigned int ier_mask = 0;
	unsigned int cfg0_mask = 0;

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	mutex_lock(&mca_uart->mutex);

	if (!mca_uart->pending_work)
		return;
	if (mca_uart->pending_work & WORK_STOP_RX) {
		ier_mask |= MCA_REG_UART_IER_RHR;
		cfg0_mask |= MCA_REG_UART_CFG0_CRX;
	}
	if (mca_uart->pending_work & WORK_STOP_TX) {
		ier_mask |= MCA_REG_UART_IER_THR;
		cfg0_mask |= MCA_REG_UART_CFG0_CTX;
	}

	ret = regmap_update_bits(regmap, MCA_REG_UART_IER, ier_mask, 0);
	if (ret)
		dev_err(mca_uart->dev, "Failed to write MCA_REG_UART_IER\n");

	ret = regmap_update_bits(regmap, MCA_REG_UART_CFG0, cfg0_mask,
				 MCA_REG_UART_CFG0_CRX | MCA_REG_UART_CFG0_CRX);
	if (ret)
		dev_err(mca_uart->dev, "Failed to write MCA_REG_UART_CFG0\n");

	if ((mca_uart->pending_work & WORK_SET_RTS) ||
	    (mca_uart->pending_work & WORK_CLEAR_RTS)) {
		uint8_t msr_mask = mca_uart->pending_work & WORK_SET_RTS ?
						MCA_REG_UART_MSR_RTS : 0;
		ret = regmap_update_bits(regmap, MCA_REG_UART_MSR,
					 MCA_REG_UART_MSR_RTS, msr_mask);
		if (ret)
			dev_err(mca_uart->dev,
				"Failed to write MCA_REG_UART_MSR\n");
	}

	mca_uart->pending_work = 0;
	mutex_unlock(&mca_uart->mutex);
}

static void mca_uart_tx_work_proc(struct work_struct *ws)
{
	struct mca_uart *mca_uart = to_mca_uart(ws, tx_work);

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	mutex_lock(&mca_uart->mutex);
	mca_uart_handle_tx(&mca_uart->port);
	mutex_unlock(&mca_uart->mutex);
}

static ssize_t power_on_rx_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct tty_port *port = dev_get_drvdata(dev);
	struct uart_state *state = container_of(port, struct uart_state, port);
	struct uart_port *uart_port = state->uart_port;
	struct mca_uart *mca_uart = container_of(uart_port, struct mca_uart,
						 port);

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	return sprintf(buf, "%s\n", mca_uart->enable_power_on ?
							"enabled" : "disabled");
}

static ssize_t power_on_rx_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct tty_port *port = dev_get_drvdata(dev);
	struct uart_state *state = container_of(port, struct uart_state, port);
	struct uart_port *uart_port = state->uart_port;
	struct mca_uart *mca_uart = container_of(uart_port, struct mca_uart,
						 port);
	struct regmap *regmap = mca_uart->mca->regmap;
	int ret;

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	if (!strncmp(buf, "enabled", sizeof("enabled") - 1))
		mca_uart->enable_power_on = true;
	else if (!strncmp(buf, "disabled", sizeof("disabled") - 1))
		mca_uart->enable_power_on = false;
	else
		return -EINVAL;

	ret = regmap_update_bits(regmap, MCA_REG_UART_CFG0,
				 MCA_REG_UART_CFG0_PWR_ON,
				 mca_uart->enable_power_on ?
						MCA_REG_UART_CFG0_PWR_ON : 0);
	if (ret < 0)
		dev_err(mca_uart->dev, "Failed to write MCA_REG_UART_CFG0\n");
	return count;
}
static DEVICE_ATTR(power_on_rx, 0600, power_on_rx_show, power_on_rx_store);

static struct attribute *uart_sysfs_entries[] = {
	&dev_attr_power_on_rx.attr,
	NULL,
};

static struct attribute_group uart_port_extra_attr = {
	.name	= "power_extra_opts",
	.attrs	= uart_sysfs_entries,
};

static int mca_uart_probe(struct platform_device *pdev)
{
	struct mca_cc6ul *mca = dev_get_drvdata(pdev->dev.parent);
	struct regmap *regmap = mca->regmap;
	struct mca_uart *mca_uart;
	struct device_node *np;
	int ret;

	dev_dbg(&pdev->dev, "<%s>\n", __func__);

	if (IS_ERR(mca))
		return PTR_ERR(mca);

	/* Find entry in device-tree */
	if (!mca->dev->of_node)
		return -ENODEV;

	/* Check if node does not exist or if it is disabled */
	np = of_find_compatible_node(mca->dev->of_node, NULL,
					"digi,mca-cc6ul-uart");
	if (!np || !of_device_is_available(np))
		return -ENODEV;

	if (mca->fw_version < MCA_UART_MIN_FW_VERSION) {
		dev_err(&pdev->dev,
			"UART is not supported in MCA firmware v%d.%02d.\n",
			MCA_FW_VER_MAJOR(mca->fw_version),
			MCA_FW_VER_MINOR(mca->fw_version));
		return -ENODEV;
	}

	mca_uart = devm_kzalloc(&pdev->dev, sizeof(*mca_uart), GFP_KERNEL);
	if (!mca_uart)
		return -ENOMEM;

	mca_uart->mca = mca;
	mca_uart->dev = &pdev->dev;
	platform_set_drvdata(pdev, mca_uart);

	mca_uart->enable_power_on = false;
	mca_uart->has_rtscts = 0;

	ret = of_property_read_u32(np, "rts-pin", &mca_uart->rts_pin);
	if (ret) {
		dev_dbg(&pdev->dev, "No RTS pin provided\n");
	} else {
		const int gpio_base = mca_uart->mca->gpio_base;

		ret = devm_gpio_request(&pdev->dev,
					gpio_base + mca_uart->rts_pin,
					"MCA UART RTS");
		if (ret) {
			dev_err(&pdev->dev,
				"Failed to allocate RTS pin\n");
		} else {
			ret = regmap_write(regmap, MCA_REG_UART_RTSPIN,
						mca_uart->rts_pin);
			if (ret)
				dev_err(mca_uart->dev,
					"Failed to write MCA_REG_UART_RTSPIN\n");
			else
				mca_uart->has_rtscts |= MCA_UART_HAS_RTS;
		}
	}

	ret = of_property_read_u32(np, "cts-pin", &mca_uart->cts_pin);
	if (ret) {
		dev_dbg(&pdev->dev, "No CTS pin provided\n");
	} else {
		const int gpio_base = mca_uart->mca->gpio_base;

		ret = devm_gpio_request(&pdev->dev,
					gpio_base + mca_uart->cts_pin,
					"MCA UART CTS");
		if (ret) {
			dev_err(&pdev->dev,
				"Failed to allocate CTS pin\n");
		} else {
			ret = regmap_write(regmap, MCA_REG_UART_CTSPIN,
					mca_uart->cts_pin);
			if (ret)
				dev_err(mca_uart->dev,
					"Failed to write MCA_REG_UART_CTSPIN\n");
			else
				mca_uart->has_rtscts |= MCA_UART_HAS_CTS;
		}
	}

	/* Register UART driver */
	mca_uart->uart.owner = THIS_MODULE;
	mca_uart->uart.dev_name = MCA_UART_DEV_NAME;
	mca_uart->uart.nr = 1;
	ret = uart_register_driver(&mca_uart->uart);
	if (ret) {
		dev_err(&pdev->dev, "Registering UART driver failed\n");
		goto error;
	}

	mutex_init(&mca_uart->mutex);

	/* Initialize port data */
	mca_uart->port.line = 0;
	mca_uart->port.dev = &pdev->dev;
	mca_uart->port.irq = platform_get_irq_byname(pdev,
						     MCA_CC6UL_IRQ_UART_NAME);
	mca_uart->port.type = PORT_LPUART;
	mca_uart->port.fifosize = max(MCA_UART_TX_FIFO_SIZE,
				      MCA_UART_RX_FIFO_SIZE);
	mca_uart->port.flags = UPF_FIXED_TYPE | UPF_LOW_LATENCY;
	mca_uart->port.iotype = UPIO_PORT;
	mca_uart->port.uartclk = MCA_UART_CLK;
	mca_uart->port.rs485_config = NULL;
	mca_uart->port.ops = &mca_uart_ops;
	mca_uart->port.attr_group = &uart_port_extra_attr;

	/* Initialize queue for start TX */
	INIT_WORK(&mca_uart->tx_work, mca_uart_tx_work_proc);
	INIT_WORK(&mca_uart->delayed_work, mca_uart_delayed_work_proc);

	/* Register port */
	ret = uart_add_one_port(&mca_uart->uart, &mca_uart->port);
	if (ret) {
		dev_err(mca_uart->dev, "Failed adding a port (%d)\n", ret);
		goto error;
	}

	/* Setup interrupt */
	ret = devm_request_threaded_irq(&pdev->dev,
					mca_uart->port.irq,
					NULL, mca_uart_irq_handler,
					IRQF_ONESHOT,
					MCA_CC6UL_IRQ_UART_NAME,
					mca_uart);
	if (ret) {
		dev_err(mca_uart->dev, "Failed to register IRQ\n");
		goto error;
	}

	ret = regmap_write(regmap, MCA_REG_UART_CFG0, MCA_REG_UART_CFG0_ENABLE);
	if (ret) {
		dev_err(mca_uart->dev, "Failed to write MCA_REG_UART_CFG0\n");
		goto error;
	}

	dev_info(mca_uart->dev, "Registered successfully\n");
	return 0;
error:
	mutex_destroy(&mca_uart->mutex);
	uart_remove_one_port(&mca_uart->uart, &mca_uart->port);
	uart_unregister_driver(&mca_uart->uart);
	devm_kfree(&pdev->dev, mca_uart);

	return ret;
}

static int mca_uart_remove(struct platform_device *pdev)
{
	struct mca_uart *mca_uart = dev_get_drvdata(pdev->dev.parent);

	dev_dbg(mca_uart->dev, "<%s>\n", __func__);

	cancel_work_sync(&mca_uart->tx_work);
	cancel_work_sync(&mca_uart->delayed_work);
	mutex_destroy(&mca_uart->mutex);
	uart_remove_one_port(&mca_uart->uart, &mca_uart->port);
	uart_unregister_driver(&mca_uart->uart);
	devm_kfree(&pdev->dev, mca_uart);

	return 0;
}

#ifdef CONFIG_PM

/*
 * The code snippet below was grabbed from drivers/tty/serial/serial_core.c
 * It is used for retrieving the TTY layer struct device. This struct is used to
 * check the value of /sys/class/tty/ttyMCAx/power/wakeup which is more standard
 * than the one at /sys/bus/i2c/devices/0-007e/mca-cc6ul-uart/power/wakeup.
 */
struct uart_match {
	struct uart_port *port;
	struct uart_driver *driver;
};

static int serial_match_port(struct device *dev, void *data)
{
	struct uart_match *match = data;
	struct tty_driver *tty_drv = match->driver->tty_driver;
	dev_t devt = MKDEV(tty_drv->major, tty_drv->minor_start) +
							      match->port->line;

	return dev->devt == devt; /* Actually, only one tty per port */
}

static int mca_cc6ul_uart_suspend(struct device *d)
{
	int ret;
	struct mca_uart *mca_uart = platform_get_drvdata(to_platform_device(d));
	struct regmap *regmap = mca_uart->mca->regmap;
	struct uart_match match = {&mca_uart->port, &mca_uart->uart};
	struct device *tty_dev = device_find_child(mca_uart->port.dev, &match,
						   serial_match_port);
	int mask = MCA_REG_UART_CFG0_WAKEUP;
	unsigned int new_value = 0;

	if (tty_dev && device_may_wakeup(tty_dev))
		new_value |= MCA_REG_UART_CFG0_WAKEUP;

	ret = regmap_update_bits(regmap, MCA_REG_UART_CFG0, mask, new_value);
	if (ret < 0)
		dev_err(mca_uart->dev, "Failed to write MCA_REG_UART_CFG0\n");

	return 0;
}

static const struct dev_pm_ops mca_cc6ul_uart_pm_ops = {
	.suspend	= mca_cc6ul_uart_suspend,
	.resume		= NULL,
	.poweroff	= NULL,
};
#endif

#ifdef CONFIG_OF
static const struct of_device_id mca_uart_ids[] = {
	{ .compatible = "digi,mca-cc6ul-uart", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mca_uart_ids);
#endif

static struct platform_driver mca_uart_driver = {
	.probe	= mca_uart_probe,
	.remove	= mca_uart_remove,
	.driver	= {
		.name	= MCA_CC6UL_DRVNAME_UART,
		.of_match_table = of_match_ptr(mca_uart_ids),
#ifdef CONFIG_PM
		.pm	= &mca_cc6ul_uart_pm_ops,
#endif
	},
};

static int __init mca_uart_init(void)
{
	return platform_driver_register(&mca_uart_driver);
}
module_init(mca_uart_init);

static void __exit mca_uart_exit(void)
{
	platform_driver_unregister(&mca_uart_driver);
}
module_exit(mca_uart_exit);

MODULE_AUTHOR("Digi International Inc");
MODULE_DESCRIPTION("UART for MCA of ConnectCore 6UL");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" MCA_CC6UL_DRVNAME_UART);
