// SPDX-License-Identifier: GPL-2.0
/*
 * hci_stp.c - MediaTek MT6630 CONSYS Bluetooth HCI-over-STP driver
 *
 * Gemini PDA (MT6797X, "MT6630 CONSYS" combo) Bluetooth bring-up on
 * Linux 6.6 (2026-09-10). Port of the vendor 3.18 driver:
 *   gemini-linux-kernel-3.18/drivers/misc/mediatek/connectivity/
 *   drv_bt/{linux/hci_stp.c,include/*.h}  (CONFIG_MTK_COMBO_BT_HCI
 *   "MTK BT driver for BlueZ", the halium defconfig path for this
 *   device - aeon6797_6m_n_halium_defconfig enables CONFIG_BT=y +
 *   CONFIG_MTK_COMBO_BT_HCI=y).
 *
 * Architecture (see docs/wifi-consys.md, the delta mtk_wcn module):
 *   The BT radio lives in the same CONSYS MCU that runs Wi-Fi. The host
 *   speaks to the MCU over BTIF (WMT/STP framing). The WMT core module
 *   (mtk_wcn) owns chip power-on and the STP transport; this driver is
 *   the BT HCI client of that transport:
 *
 *     - TX: H4-framed HCI packets -> mtk_wcn_stp_send_data(...,BT_TASK_INDX)
 *     - RX: STP BT channel frames -> mtk_wcn_stp_register_if_rx() (in
 *       "bluez mode" the stp core delivers BT rx straight to this
 *       callback), parsed by an H4 state machine -> hci_recv_frame().
 *     - BT power: mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT) issues the WMT
 *       OPCODE_FUNC_CTRL to the MCU; the chip's BT side then runs and
 *       answers HCI. The vendor init script (bt_init_script_6630, from
 *       mediatek/external/bluetooth/driver/combo/radiomod.c) configures
 *       the radio via vendor HCI commands; the BD address is read from
 *       the chip eFUSE (bt_get_bd_addr) unless overridden by the
 *       bd_addr module parameter.
 *
 * 6.6 port deltas vs the 3.18 original:
 *   - no kernel file IO (get_fs/set_fs are gone): the /data/BT.cfg and
 *     /data/bluetooth/BT.cfg store is dropped; defaults come from the
 *     init-table command bytes + the cfg below, and the BD address is
 *     always (re)queried from the chip eFUSE on each open unless the
 *     bd_addr parameter is given.
 *   - no hdev->dev_type: 6.6.0 needed HCI_PRIMARY (HCI_BREDR was gone);
 *     6.6.y stable removed AMP support and the field itself.
 *   - driver cleans up like the vendor on close (unregister stp rx +
 *     bluez mode off + WMT BT func off).
 *
 * Init-table events are matched on the first 7 bytes (HCI cmd-complete
 * header) exactly like the vendor hci_stp_dev_init_rx_cb; sizes must
 * match evt_sz.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/sched.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "hci_stp.h"
#include "stp_exp.h"
#include "wmt_exp.h"

#define PFX "[HCI-STP] "
#define VERSION "3.0 (6.6 gemini port)"

static int g_dbg_level = 2;	/* 0 err .. 4 loud */
module_param(g_dbg_level, int, 0644);
MODULE_PARM_DESC(g_dbg_level, "log level (0=err,1=warn,2=info,3=dbg,4=loud)");

#define BT_LOUD(fmt, ...)	do { if (g_dbg_level >= 4) pr_debug(PFX "%s: " fmt, __func__, ##__VA_ARGS__); } while (0)
#define BT_DBG(fmt, ...)	do { if (g_dbg_level >= 3) pr_debug(PFX "%s: " fmt, __func__, ##__VA_ARGS__); } while (0)
#define BT_INFO(fmt, ...)	do { if (g_dbg_level >= 2) pr_info(PFX "%s: " fmt, __func__, ##__VA_ARGS__); } while (0)
#define BT_WARN(fmt, ...)	do { if (g_dbg_level >= 1) pr_warn(PFX "%s: " fmt, __func__, ##__VA_ARGS__); } while (0)
#define BT_ERR(fmt, ...)	pr_err(PFX "%s: " fmt, __func__, ##__VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Vendor HCI init script for MT6630 (subset actually sent; see the    */
/* init_table below). All byte strings verbatim from the 3.18 drv_bt.  */
/* ------------------------------------------------------------------ */

static unsigned char bt_get_bd_addr[4] =
	{0x01, 0x09, 0x10, 0x00};
static unsigned char bt_get_bd_addr_evt[13] =
	{0x04, 0x0E, 0x0A, 0x01, 0x09, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static unsigned char bt_set_bd_addr[10] =
	{0x01, 0x1A, 0xFC, 0x06, 0x01, 0x20, 0x66, 0x46, 0x00, 0x00};
static unsigned char bt_set_bd_addr_evt[7] =
	{0x04, 0x0E, 0x04, 0x01, 0x1A, 0xFC, 0x00};
static unsigned char bt_set_radio[10] =
	{0x01, 0x79, 0xFC, 0x06, 0x06, 0x80, 0x00, 0x06, 0x03, 0x06};
static unsigned char bt_set_radio_evt[7] =
	{0x04, 0x0E, 0x04, 0x01, 0x79, 0xFC, 0x00};
static unsigned char bt_set_tx_pwr_offset[7] =
	{0x01, 0x93, 0xFC, 0x03, 0xFF, 0xFF, 0xFF};
static unsigned char bt_set_tx_pwr_offset_evt[7] =
	{0x04, 0x0E, 0x04, 0x01, 0x93, 0xFC, 0x00};
static unsigned char bt_set_sleep[11] =
	{0x01, 0x7A, 0xFC, 0x07, 0x03, 0x40, 0x1F, 0x40, 0x1F, 0x00, 0x04};
static unsigned char bt_set_sleep_evt[7] =
	{0x04, 0x0E, 0x04, 0x01, 0x7A, 0xFC, 0x00};
static unsigned char bt_reset[4] =
	{0x01, 0x03, 0x0C, 0x00};
static unsigned char bt_reset_evt[7] =
	{0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00};

/* The 6630 init script actually sent per open (vendor trimmed table,
 * referencing mediatek/external/bluetooth/driver/combo/radiomod.c
 * bt_init_script_6630[]). Index 0 (get_bd_addr) is skipped when the
 * bd_addr module parameter is set.
 */
static struct hci_stp_init_cmd init_table[] = {
	hci_stp_init_entry(bt_get_bd_addr),
	hci_stp_init_entry(bt_set_bd_addr),
	hci_stp_init_entry(bt_set_radio),
	hci_stp_init_entry(bt_set_tx_pwr_offset),
	hci_stp_init_entry(bt_set_sleep),
	hci_stp_init_entry(bt_reset),
};

/* Static per-device state (single combo chip). */
static struct hci_stp *g_hu;
static struct task_struct *hci_stp_tx_thrd;
static spinlock_t hci_stp_txqlock;
static wait_queue_head_t hci_stp_tx_thrd_wq;

static int hci_stp_dev_init(struct hci_stp *phu);

/* BD address override, e.g. bd_addr=00:09:34:5a:af:c2. Empty: query
 * the chip eFUSE on open (vendor bt_get_bd_addr path).
 */
static char bd_addr_param[18] = "";
module_param_string(bd_addr, bd_addr_param, sizeof(bd_addr_param), 0444);
MODULE_PARM_DESC(bd_addr, "BD address override (xx:xx:xx:xx:xx:xx); empty = read chip eFUSE");

/* Run the vendor radio-config init script at open. Bring-up default 0:
 * the glass session (2026-09-10) shows the chip answers BT HCI only
 * ~one command-window late (slow BT-core wake after func-on), so a
 * serial 6-cmd script stalls hci0 power-on badly. The script only
 * sets radio/sleep/BD-address hints (chip defaults + the autogen
 * address cover those); re-enable when the reply latency is tamed.
 */
static int init_script = 0;
module_param(init_script, int, 0644);
MODULE_PARM_DESC(init_script, "run the vendor radio-config init script at open (1=yes, bring-up default 0)");

static int parse_bd_addr(const char *s, unsigned char addr[6])
{
	unsigned int b[6];
	int i;

	if (strlen(s) != 17)
		return -EINVAL;
	if (sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x",
		   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		return -EINVAL;
	for (i = 0; i < 6; i++)
		addr[i] = b[i];
	return 0;
}

/* ------------------------------------------------------------------ */
/* TX path: a single kthread drains the txq into the STP BT channel.   */
/* ------------------------------------------------------------------ */

static void hci_stp_tx_kick(void)
{
	smp_wmb();
	wake_up_interruptible(&hci_stp_tx_thrd_wq);
}

static int hci_stp_tx_thrd_func(void *pdata)
{
	struct hci_stp *hu = pdata;

	for (;;) {
		struct sk_buff *skb;

		wait_event_interruptible(hci_stp_tx_thrd_wq,
			!skb_queue_empty(&hu->txq) || kthread_should_stop());
		if (kthread_should_stop())
			break;

		while ((skb = skb_dequeue(&hu->txq))) {
			int len;

			BT_INFO("TX %d bytes: %*ph\n", skb->len,
				min_t(int, skb->len, 12), skb->data);
			len = mtk_wcn_stp_send_data(skb->data, skb->len,
						    BT_TASK_INDX);
			BT_INFO("stp_send_data -> %d\n", len);
			if (unlikely(len != skb->len)) {
				BT_ERR("stp_send_data fail (%d != %d), requeue\n",
				       len, skb->len);
				spin_lock_bh(&hci_stp_txqlock);
				skb_queue_head(&hu->txq, skb);
				spin_unlock_bh(&hci_stp_txqlock);
				break;
			}
			kfree_skb(skb);
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* RX path. Two modes:                                                 */
/*  - during the open-time init sequence: hci_stp_dev_init_rx_cb does  */
/*    an exact match of each expected cmd-complete event;              */
/*  - afterwards: stp_rx_event_cb_directly runs the H4 state machine   */
/*    and delivers HCI frames to the core (hci_recv_frame).            */
/* The stp core calls the registered if_rx with one BT-channel frame   */
/* at a time (bluez mode).                                             */
/* ------------------------------------------------------------------ */

static void hci_stp_dev_init_rx_cb(unsigned char *data, int count)
{
	struct hci_stp *hu = g_hu;
	unsigned int idx;

	if (!hu)
		return;

	idx = hu->init_cmd_idx;
	if (unlikely(idx >= ARRAY_SIZE(init_table)))
		return;

	if (unlikely(count != init_table[idx].evt_sz)) {
		hu->init_evt_rx_flag = -1;
	} else if (unlikely(memcmp(data, init_table[idx].hci_evt, 7))) {
		hu->init_evt_rx_flag = -2;
	} else {
		hu->init_evt_rx_flag = 1;
		if (idx == 0) {
			/* store the returned eFUSE BD address */
			memcpy(&bt_get_bd_addr_evt[7], &data[7], 6);
		}
	}

	if (unlikely(hu->init_evt_rx_flag != 1))
		BT_WARN("EVT(%u) len(%d) flag(%d) - expected %d bytes, rx: %*ph\n",
			idx, count, hu->init_evt_rx_flag,
			init_table[idx].evt_sz, count, data);

	spin_lock(&hu->init_lock);
	if (likely(hu->p_init_evt_wq))
		wake_up(hu->p_init_evt_wq);
	spin_unlock(&hu->init_lock);
}

static void hci_stp_dev_init_work(struct work_struct *work)
{
	struct hci_stp *phu = container_of(work, struct hci_stp, init_work);
	/* the chip eFUSE unprogrammed default (vendor sDefaultCfg.addr):
	 * if the fetch returns exactly this, auto-generate a random
	 * locally-administered address (no factory BT address fused).
	 */
	static const unsigned char def_addr[6] =
		{0x00, 0x00, 0x46, 0x66, 0x20, 0x01};
	struct btradio_conf_data cfg = {
		{0x00, 0x00, 0x46, 0x66, 0x20, 0x01},
		{0x60, 0x00},
		{0x23, 0x10, 0x00, 0x00},
		{0x06, 0x80, 0x00, 0x06, 0x03, 0x06},
		{0x03, 0x40, 0x1F, 0x40, 0x1F, 0x00, 0x04},
		{0x80, 0x00},
		{0xFF, 0xFF, 0xFF}};
	bool use_efuse = false;
	unsigned int idx;
	long ret, to;
	int i;
	
	phu->init_ok_cmds = 0;

	if (bd_addr_param[0]) {
		if (parse_bd_addr(bd_addr_param, cfg.addr) == 0) {
			BT_INFO("using module-param BD address "
				"%02x:%02x:%02x:%02x:%02x:%02x\n",
				cfg.addr[0], cfg.addr[1], cfg.addr[2],
				cfg.addr[3], cfg.addr[4], cfg.addr[5]);
		} else {
			BT_WARN("bad bd_addr param '%s', falling back to eFUSE\n",
				bd_addr_param);
			use_efuse = true;
		}
	} else {
		/* no override: retrieve the factory BD address from the
		 * chip eFUSE via the vendor read command (index 0)
		 */
		use_efuse = true;
	}

	/* Patch the radio-config HCI commands from cfg (defaults match
	 * the hardcoded table bytes; kept for a later config surface).
	 * set_bd_addr payload = the 6 address bytes LSB-first (display
	 * order reversed - the chip's eFUSE reply layout). Patched again
	 * below when the eFUSE fetch (idx 0) returns a real address.
	 */
	for (i = 0; i < 6; i++)
		bt_set_bd_addr[4 + i] = cfg.addr[5 - i];
	memcpy(&bt_set_radio[4], cfg.radio, 6);
	memcpy(&bt_set_tx_pwr_offset[4], cfg.tx_pwr_offset, 3);
	memcpy(&bt_set_sleep[4], cfg.sleep, 7);

	idx = use_efuse ? 0 : 1;

	for (; idx < ARRAY_SIZE(init_table); ++idx) {
		phu->init_cmd_idx = idx;
		phu->init_evt_rx_flag = 0;
		to = (init_table[idx].hci_cmd == bt_reset) ?
			BT_CMD_DELAY_MS_RESET : BT_CMD_DELAY_MS_COMM;
		to = msecs_to_jiffies(to * BT_CMD_DELAY_SAFE_GUARD);

		BT_DBG("CMD(%u) %s timeout %ld jiffies\n",
		       idx, init_table[idx].str, to);
		smp_wmb();

		mtk_wcn_stp_send_data(init_table[idx].hci_cmd,
				      init_table[idx].cmd_sz, BT_TASK_INDX);
		ret = wait_event_timeout(*phu->p_init_evt_wq,
					 phu->init_evt_rx_flag != 0, to);

		if (phu->init_evt_rx_flag != 1) {
			BT_WARN("init CMD(%u) %s no event (flag %d) after %ld ms - "
				"continuing (the eFUSE BD read is optional; "
				"an auto-generated address is used when it fails)\n",
			       idx, init_table[idx].str,
			       phu->init_evt_rx_flag,
			       ret ? jiffies_to_msecs(ret) : -1);
			continue;
		}
		phu->init_ok_cmds++;

		if (idx == 0) {
			/* eFUSE BD address reply (6 bytes at evt[7..12],
			 * LSB-first). Decode to display order.
			 */
			for (i = 0; i < 6; i++)
				cfg.addr[i] = bt_get_bd_addr_evt[12 - i];

			if (!memcmp(cfg.addr, def_addr, 6)) {
				get_random_bytes(cfg.addr, 6);
				cfg.addr[0] = (cfg.addr[0] & 0xFE) | 0x02;
				BT_WARN("eFUSE BD address unprogrammed "
					"(default); random locally-administered "
					"address used\n");
			}
			BT_INFO("BD address "
				"%02x:%02x:%02x:%02x:%02x:%02x\n",
				cfg.addr[0], cfg.addr[1], cfg.addr[2],
				cfg.addr[3], cfg.addr[4], cfg.addr[5]);

			/* re-patch set_bd_addr (sent as cmd 1) with the
			 * fetched address
			 */
			for (i = 0; i < 6; i++)
				bt_set_bd_addr[4 + i] = cfg.addr[5 - i];
		}
	}

	BT_INFO("init script done: %d/%zu commands answered\n",
		phu->init_ok_cmds, ARRAY_SIZE(init_table));

	if (phu->p_init_comp)
		complete(phu->p_init_comp);
}

/* Send the init script and wait for completion. Returns 0 on success
 * (all commands in the table got their matching event).
 */
static int hci_stp_dev_init(struct hci_stp *phu)
{
	DECLARE_COMPLETION_ONSTACK(comp);
	DECLARE_WAIT_QUEUE_HEAD_ONSTACK(evt_wq);
	int ret;

	if (!init_script) {
		BT_INFO("init script skipped (init_script=0) - controller "
			"defaults + autogen BD address\n");
		return 0;
	}
	spin_lock(&phu->init_lock);
	phu->p_init_comp = &comp;
	phu->p_init_evt_wq = &evt_wq;
	spin_unlock(&phu->init_lock);

	/* direct rx delivery for the init sequence (bluez mode) */
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);
	mtk_wcn_stp_register_if_rx(hci_stp_dev_init_rx_cb);
	mtk_wcn_stp_set_bluez(1);

	schedule_work(&phu->init_work);
	wait_for_completion(&comp);

	spin_lock(&phu->init_lock);
	phu->p_init_comp = NULL;
	phu->p_init_evt_wq = NULL;
	spin_unlock(&phu->init_lock);

	ret = phu->init_evt_rx_flag;
	/* succeed when the controller answered anything; a deaf controller
	 * shows up as init_ok_cmds == 0.
	 */
	if (phu->init_ok_cmds > 0 || ret == 1)
		return 0;
	return ret + 256;	/* non-zero error */
}

/* STP TX event (the BT tx queue drained) -> re-kick the tx thread */
static void stp_tx_event_cb(void)
{
	hci_stp_tx_kick();
}

/* H4 receive state machine over the STP BT channel. Persistent state
 * (hu->rx_*): the STP core may deliver one HCI frame in several
 * chunks.
 */
static void stp_rx_event_cb_directly(unsigned char *data, int count)
{
	struct hci_stp *hu = g_hu;
	struct hci_dev *hdev;
	struct hci_event_hdr *eh;
	struct hci_acl_hdr *ah;
	struct hci_sco_hdr *sh;
	struct sk_buff *rx_skb;
	unsigned int rx_count;
	int rx_state;
	int type = 0;
	int len, dlen, room;
	int loop_guard = 0;
	const unsigned char *ptr = data;

	if (!hu || !data || count <= 0)
		return;
	hdev = hu->hdev;
	BT_INFO("RX cb %d bytes: %*ph\n", count,
		min_t(int, count, 12), data);

	/* per-call copies of the persistent parser state */
	rx_skb = hu->rx_skb;
	rx_count = hu->rx_count;
	rx_state = hu->rx_state;

	while (count > 0) {
		if (++loop_guard > 5000) {
			BT_WARN("abnormal rx loop, count=%d\n", count);
			break;
		}

		if (rx_count) {
			len = min_t(unsigned int, rx_count, count);
			memcpy(skb_put(rx_skb, len), ptr, len);
			rx_count -= len;
			count -= len;
			ptr += len;

			if (rx_count)
				continue;

			switch (rx_state) {
			case H4_W4_DATA:
				hci_recv_frame(hdev, rx_skb);
				rx_skb = NULL;
				rx_state = H4_W4_PACKET_TYPE;
				continue;
			case H4_W4_EVENT_HDR:
				eh = hci_event_hdr(rx_skb);
				room = skb_tailroom(rx_skb);
				if (!eh->plen) {
					hci_recv_frame(hdev, rx_skb);
					rx_skb = NULL;
					rx_state = H4_W4_PACKET_TYPE;
				} else if (eh->plen > room) {
					BT_ERR("event too large plen(%d) room(%d)\n",
					       eh->plen, room);
					kfree_skb(rx_skb);
					rx_skb = NULL;
					rx_state = H4_W4_PACKET_TYPE;
				} else {
					rx_state = H4_W4_DATA;
					rx_count = eh->plen;
				}
				continue;
			case H4_W4_ACL_HDR:
				ah = hci_acl_hdr(rx_skb);
				dlen = __le16_to_cpu(ah->dlen);
				room = skb_tailroom(rx_skb);
				if (!dlen) {
					hci_recv_frame(hdev, rx_skb);
					rx_skb = NULL;
					rx_state = H4_W4_PACKET_TYPE;
				} else if (dlen > room) {
					BT_ERR("ACL too large dlen(%d) room(%d)\n",
					       dlen, room);
					kfree_skb(rx_skb);
					rx_skb = NULL;
					rx_state = H4_W4_PACKET_TYPE;
				} else {
					rx_state = H4_W4_DATA;
					rx_count = dlen;
				}
				continue;
			case H4_W4_SCO_HDR:
				sh = hci_sco_hdr(rx_skb);
				room = skb_tailroom(rx_skb);
				if (!sh->dlen) {
					hci_recv_frame(hdev, rx_skb);
					rx_skb = NULL;
					rx_state = H4_W4_PACKET_TYPE;
				} else if (sh->dlen > room) {
					BT_ERR("SCO too large dlen(%d) room(%d)\n",
					       sh->dlen, room);
					kfree_skb(rx_skb);
					rx_skb = NULL;
					rx_state = H4_W4_PACKET_TYPE;
				} else {
					rx_state = H4_W4_DATA;
					rx_count = sh->dlen;
				}
				continue;
			}
		}

		/* H4_W4_PACKET_TYPE */
		switch (*ptr) {
		case HCI_EVENT_PKT:
			rx_state = H4_W4_EVENT_HDR;
			rx_count = HCI_EVENT_HDR_SIZE;
			type = HCI_EVENT_PKT;
			break;
		case HCI_ACLDATA_PKT:
			rx_state = H4_W4_ACL_HDR;
			rx_count = HCI_ACL_HDR_SIZE;
			type = HCI_ACLDATA_PKT;
			break;
		case HCI_SCODATA_PKT:
			rx_state = H4_W4_SCO_HDR;
			rx_count = HCI_SCO_HDR_SIZE;
			type = HCI_SCODATA_PKT;
			break;
		default:
			BT_ERR("unknown HCI packet type 0x%02x\n", *ptr);
			ptr++;
			count--;
			continue;
		}
		ptr++;
		count--;

		rx_skb = bt_skb_alloc(HCI_MAX_FRAME_SIZE, GFP_ATOMIC);
		if (!rx_skb) {
			BT_ERR("bt_skb_alloc(%d) failed\n", HCI_MAX_FRAME_SIZE);
			rx_state = H4_W4_PACKET_TYPE;
			rx_count = 0;
			break;
		}
		bt_cb(rx_skb)->pkt_type = type;
	}

	/* persist state across STP channel indications */
	hu->rx_skb = rx_skb;
	hu->rx_count = rx_count;
	hu->rx_state = rx_state;
}

/* ------------------------------------------------------------------ */
/* HCI device callbacks                                                */
/* ------------------------------------------------------------------ */

static int hci_stp_open(struct hci_dev *hdev)
{
	struct hci_stp *hu = hci_get_drvdata(hdev);
	int num_tries = 0;
	int ret;

	if (!hu)
		return -ENODEV;

	BT_INFO("opening %s\n", hdev->name);

	spin_lock_bh(&hci_stp_txqlock);
	skb_queue_purge(&hu->txq);
	spin_unlock_bh(&hci_stp_txqlock);

	/* reset the H4 parser state (a previous open may have left a
	 * partial frame behind)
	 */
	if (hu->rx_skb) {
		kfree_skb(hu->rx_skb);
		hu->rx_skb = NULL;
	}
	hu->rx_count = 0;
	hu->rx_state = H4_W4_PACKET_TYPE;

	/* Turn BT on at the CONSYS MCU (WMT OPCODE_FUNC_CTRL). The WMT
	 * core + STP transport must already be up (mtk_wcn module does
	 * this at boot); retry up to ~60 s like the vendor driver.
	 */
	while (mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE) {
		BT_WARN("WMT turn-on BT failed (retry %d)\n", num_tries);
		if (++num_tries > 300)
			return -ENODEV;
		msleep_interruptible(200);
	}
	BT_INFO("WMT BT function on OK\n");

	if (mtk_wcn_stp_is_ready() != MTK_WCN_BOOL_TRUE) {
		BT_ERR("STP not ready - cannot init BT\n");
		goto err_bt_off;
	}

	ret = hci_stp_dev_init(hu);
	if (ret) {
		BT_WARN("hci_stp_dev_init failed (%d)\n", ret);
		goto err_bt_off;
	}
	BT_INFO("hci_stp_dev_init ok\n");

	set_bit(HCI_RUNNING, &hdev->flags);

	/* hand the BT channel to the H4 parser + BlueZ */
	mtk_wcn_stp_register_if_rx(stp_rx_event_cb_directly);
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);
	mtk_wcn_stp_register_tx_event_cb(BT_TASK_INDX, stp_tx_event_cb);
	mtk_wcn_stp_set_bluez(1);

	return 0;

err_bt_off:
	mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT);
	return -ENODEV;
}

static int hci_stp_flush(struct hci_dev *hdev)
{
	struct hci_stp *hu = hci_get_drvdata(hdev);

	if (!hu)
		return -EFAULT;
	spin_lock_bh(&hci_stp_txqlock);
	skb_queue_purge(&hu->txq);
	spin_unlock_bh(&hci_stp_txqlock);
	return 0;
}

static int hci_stp_close(struct hci_dev *hdev)
{
	struct hci_stp *hu = hci_get_drvdata(hdev);

	if (!test_and_clear_bit(HCI_RUNNING, &hdev->flags))
		return 0;

	BT_INFO("closing %s\n", hdev->name);

	hci_stp_flush(hdev);

	mtk_wcn_stp_register_if_rx(NULL);
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);
	mtk_wcn_stp_register_tx_event_cb(BT_TASK_INDX, NULL);
	mtk_wcn_stp_set_bluez(0);

	if (mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE)
		BT_WARN("WMT turn-off BT failed\n");
	else
		BT_INFO("WMT BT function off\n");

	return 0;
}

/* Send frames from the HCI layer: prepend the H4 packet type and queue
 * them for the tx thread.
 */
static int hci_stp_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct hci_stp *hu = hci_get_drvdata(hdev);

	if (!hu)
		return -ENODEV;
	if (!test_bit(HCI_RUNNING, &hdev->flags)) {
		BT_ERR("not running\n");
		return -EBUSY;
	}

	memcpy(skb_push(skb, 1), &bt_cb(skb)->pkt_type, 1);

	spin_lock_bh(&hci_stp_txqlock);
	skb_queue_tail(&hu->txq, skb);
	spin_unlock_bh(&hci_stp_txqlock);

	hci_stp_tx_kick();
	return 0;
}

/* ------------------------------------------------------------------ */
/* Module init/exit                                                    */
/* ------------------------------------------------------------------ */

static int __init hci_stp_init(void)
{
	struct hci_stp *hu;
	struct hci_dev *hdev;

	hu = kzalloc(sizeof(*hu), GFP_KERNEL);
	if (!hu)
		return -ENOMEM;
	skb_queue_head_init(&hu->txq);
	spin_lock_init(&hu->init_lock);
	hu->rx_state = H4_W4_PACKET_TYPE;

	hdev = hci_alloc_dev();
	if (!hdev) {
		kfree(hu);
		return -ENOMEM;
	}
	hu->hdev = hdev;
	hdev->bus = HCI_UART;
	/* hdev->dev_type = HCI_PRIMARY removed: 6.6.y stable dropped AMP
	 * support (and the dev_type field); every hci_dev is primary. */
	hci_set_drvdata(hdev, hu);

	hdev->open = hci_stp_open;
	hdev->close = hci_stp_close;
	hdev->flush = hci_stp_flush;
	hdev->send = hci_stp_send_frame;

	/* MT6630 CONSYS (Gemini PDA bring-up): the controller answers the
	 * LE read commands but refuses the LE write commands (LE Set Event
	 * Mask first among them) with status 0x20 during the open-time
	 * init, which would abort every hci0 open. Best-effort the LE init
	 * stage so BR/EDR comes up; LE handled separately (see the quirk
	 * comment in include/net/bluetooth/hci.h, 2026-09-11). */
	set_bit(HCI_QUIRK_EXT_INIT_BEST_EFFORT, &hdev->quirks);

	INIT_WORK(&hu->init_work, hci_stp_dev_init_work);

	/* tx kthread must exist before hci_register_dev can be opened */
	spin_lock_init(&hci_stp_txqlock);
	init_waitqueue_head(&hci_stp_tx_thrd_wq);
	hci_stp_tx_thrd = kthread_create(hci_stp_tx_thrd_func, hu, "hci_stpd");
	if (IS_ERR(hci_stp_tx_thrd)) {
		BT_ERR("kthread_create failed\n");
		hci_free_dev(hdev);
		kfree(hu);
		return PTR_ERR(hci_stp_tx_thrd);
	}
	wake_up_process(hci_stp_tx_thrd);

	g_hu = hu;

	if (hci_register_dev(hdev) < 0) {
		BT_ERR("hci_register_dev failed\n");
		kthread_stop(hci_stp_tx_thrd);
		hci_free_dev(hdev);
		kfree(hu);
		g_hu = NULL;
		return -ENODEV;
	}

	BT_INFO("HCI STP driver ver %s registered (%s)\n", VERSION, hdev->name);
	return 0;
}

static void __exit hci_stp_exit(void)
{
	struct hci_stp *hu = g_hu;

	if (!hu)
		return;
	hci_unregister_dev(hu->hdev);
	if (hci_stp_tx_thrd)
		kthread_stop(hci_stp_tx_thrd);
	skb_queue_purge(&hu->txq);
	hci_free_dev(hu->hdev);
	kfree(hu);
	g_hu = NULL;
}

module_init(hci_stp_init);
module_exit(hci_stp_exit);

MODULE_AUTHOR("MediaTek Inc. (ported to 6.6 for the Gemini PDA bring-up)");
MODULE_DESCRIPTION("MediaTek MT6630 CONSYS Bluetooth HCI driver (hci_stp over WMT/STP)");
MODULE_LICENSE("GPL v2");
